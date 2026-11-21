#include "parquet_output.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_map>

constexpr std::size_t shards = 64;
constexpr Millis session_gap = 30 * 60 * 1000;
using Names = std::unordered_map<Text, int>;
std::size_t threads;

template <typename F> void parallel(std::size_t jobs, F work) {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (std::size_t t = 0; t < std::min(jobs, threads); ++t) {
        pool.emplace_back([&, t] {
            for (;;) {
                auto j = next.fetch_add(1);
                if (j >= jobs) break;
                work(t, j);
            }
        });
    }
    for (auto& t : pool) t.join();
}

// Writers own separate partitions. Readers can merge and release each shard independently.
template <typename T> struct Partition {
    std::vector<std::array<std::vector<T>, shards>> local{threads};
    void reserve_rows(std::size_t rows) {
        const auto per_bucket = std::min<std::size_t>(131072, rows / (local.size() * shards));
        for (auto& worker : local) for (auto& bucket : worker) bucket.reserve(per_bucket);
    }
    void add(std::size_t thread, std::size_t hash, T row) {
        local[thread][hash % shards].push_back(row);
    }
    std::vector<T> take(std::size_t shard) {
        std::size_t count = 0;
        for (const auto& w : local) count += w[shard].size();
        std::vector<T> result;
        result.reserve(count);
        for (auto& w : local) {
            result.insert(result.end(), w[shard].begin(), w[shard].end());
            std::vector<T>().swap(w[shard]);
        }
        return result;
    }
};

struct People {
    struct Entry { Name name, owner; };
    std::vector<Entry> entries;
    NamePool storage;

    std::size_t slot(Name name) const {
        const auto mask = entries.size() - 1;
        auto i = name.hash() & mask;
        while (!entries[i].name.empty() && entries[i].name != name) i = (i + 1) & mask;
        return i;
    }
    explicit People(const Files& links) {
        std::size_t bytes = 0, capacity = 16;
        for (auto chunk : links.chunks) bytes += chunk.size();
        while (capacity < bytes / 10) capacity *= 2;
        entries.resize(capacity);
        std::vector<int> parent, sizes;
        std::vector<Name> canonical;
        std::vector<Text> labels;
        auto id = [&](Text text) {
            Name name = storage.get(text);
            auto i = slot(name);
            if (entries[i].name.empty()) {
                if (parent.size() * 10 >= entries.size() * 7) {
                    auto previous = std::move(entries);
                    entries.resize(previous.size() * 2);
                    for (auto e : previous) if (!e.name.empty()) entries[slot(e.name)] = e;
                    i = slot(name);
                }
                entries[i] = {name, {parent.size()}};
                parent.push_back(static_cast<int>(parent.size()));
                sizes.push_back(1);
                canonical.push_back(name);
                labels.push_back(text);
            }
            return static_cast<int>(entries[i].owner.value);
        };
        auto root = [&](int i) {
            while (parent[i] != i) {parent[i] = parent[parent[i]]; i = parent[i];}
            return i;
        };
        for (auto chunk : links.chunks) each_line(chunk, [&](Text line) {
            std::array<Text, 2> f;
            if (!fields(line, f) || f[0].empty() || f[1].empty()) return;
            auto a = root(id(f[0])), b = root(id(f[1]));
            if (a == b) return;
            if (sizes[a] < sizes[b]) std::swap(a, b);
            parent[b] = a;
            sizes[a] += sizes[b];
            if (labels[b] < labels[a]) { labels[a] = labels[b]; canonical[a] = canonical[b]; }
        });
        for (auto& e : entries) if (!e.name.empty()) e.owner = canonical[root(e.owner.value)];
    }
    Name find(Name name) const {
        const auto& e = entries[slot(name)];
        return e.name.empty() ? name : e.owner;
    }
};

struct Catalog {
    Names products, campaigns;
    std::vector<Text> categories{"unknown"}, campaign_ids{"organic"}, channels{"organic"};
    Catalog(const Files& product_files, const Files& campaign_files) {
        Names category_index{{"unknown", 0}};
        for (auto chunk : product_files.chunks) each_line(chunk, [&](Text line) {
            std::array<Text, 2> f;
            if (!fields(line, f)) return;
            auto [category, fresh] = category_index.try_emplace(f[1], static_cast<int>(categories.size()));
            if (fresh) categories.push_back(f[1]);
            products[f[0]] = category->second;
        });
        for (auto chunk : campaign_files.chunks) each_line(chunk, [&](Text line) {
            std::array<Text, 2> f;
            if (!fields(line, f)) return;
            campaigns.emplace(f[0], static_cast<int>(campaign_ids.size()));
            campaign_ids.push_back(f[0]);
            channels.push_back(f[1]);
        });
    }
};

struct Event { Name person; Millis ts; int price, category; };
struct Click { Name person; Text id; Millis ts; int campaign; };
struct Version { Name id; Millis ingest; Event event; };

Event validate_event(const std::array<Text, 7>& f, Millis ingest, Millis start, Millis end,
                     const People& people, const Catalog& catalog, NamePool& names) {
    if (f[1].empty() || f[5].empty()) return {};
    const auto ts = timestamp(f[2]);
    if (ts < start || ts >= end || ingest - ts > day_ms) return {};
    int price = 0, category = 0;
    if (f[4] == "purchase") {
        price = static_cast<int>(number(f[6], 10'000'000));
        if (price < 1) return {};
        auto product = catalog.products.find(f[5]);
        if (product != catalog.products.end()) category = product->second;
    } else if (f[4] != "view" && f[4] != "cart") return {};
    return {people.find(names.get(f[1])), ts, price, category};
}

void load_events(const Files& files, const People& people, const Catalog& catalog,
                 Millis start, Millis end, Partition<Event>& out, std::vector<NamePool>& names) {
    Partition<Version> versions;
    versions.reserve_rows(files.estimated_rows());
    std::vector<NamePool> ids(threads);
    parallel(files.chunks.size(), [&](auto t, auto j) {
        records<7>(files.chunks[j], [&](const auto& f) {
            if (f[0].empty()) return;
            auto id = ids[t].get(f[0]);
            auto hash = id.hash();
            auto ingest = ingest_time(f[3]);
            if (ingest == invalid_time) return;
            // Retain the earliest version even when its payload fails validation.
            versions.add(t, hash, {id, ingest, validate_event(f, ingest, start, end, people, catalog, names[t])});
        });
    });
    out.reserve_rows(files.estimated_rows());
    parallel(shards, [&](auto t, auto shard) {
        std::size_t count = 0, capacity = 1;
        for (const auto& worker : versions.local) count += worker[shard].size();
        while (capacity < count + count / 2) capacity *= 2;
        std::vector<const Version*> slots(capacity);
        for (const auto& worker : versions.local) {
            for (const auto& row : worker[shard]) {
                // Low hash bits already selected the shard. Probe with the remaining bits.
                auto slot = (row.id.hash() / shards) & (capacity - 1);
                while (slots[slot] && slots[slot]->id != row.id)
                    slot = (slot + 1) & (capacity - 1);
                if (!slots[slot] || row.ingest < slots[slot]->ingest) slots[slot] = &row;
            }
        }
        for (auto row : slots) {
            if (row && !row->event.person.empty())
                out.add(t, row->event.person.hash(), row->event);
        }
        for (auto& worker : versions.local) std::vector<Version>().swap(worker[shard]);
    });
}

void load_clicks(const Files& files, const People& people, const Catalog& catalog,
                 Millis start, Millis end, Partition<Click>& out, std::vector<NamePool>& names) {
    out.reserve_rows(files.estimated_rows());
    parallel(files.chunks.size(), [&](auto t, auto j) {
        records<4>(files.chunks[j], [&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            auto ts = timestamp(f[3]);
            if (ts < start - week_ms || ts >= end) return;
            auto campaign = catalog.campaigns.find(f[2]);
            if (campaign == catalog.campaigns.end()) return;
            auto person = people.find(names[t].get(f[1]));
            out.add(t, person.hash(), {person, f[0], ts, campaign->second});
        });
    });
}

struct Totals {
    std::int64_t sessions = 0, purchasing = 0, revenue = 0, assists = 0;
    void add(const Totals& b) {
        sessions += b.sessions;
        purchasing += b.purchasing;
        revenue += b.revenue;
        assists += b.assists;
    }
};
struct CampaignDay {
    int id, day;
    bool operator==(const CampaignDay& other) const { return id == other.id && day == other.day; }
};
struct CampaignDayHash {
    std::size_t operator()(const CampaignDay& key) const {
        return static_cast<std::size_t>(static_cast<unsigned>(key.day)) * 0x9e3779b1U + static_cast<unsigned>(key.id);
    }
};
using SparseTotals = std::unordered_map<CampaignDay, Totals, CampaignDayHash>;

struct CampaignTotals {
    int first_day;
    std::size_t campaigns;
    std::vector<Totals> dense;
    SparseTotals sparse;
    CampaignTotals(int first, int last, std::size_t n) : first_day(first), campaigns(n) {
        auto length = std::int64_t(last) - first + 1;
        if (length > 0 && std::uint64_t(length) <= 500000 / n) dense.resize(n * length);
    }
    Totals& get(int id, int day) {
        if (dense.empty()) return sparse[{id, day}];
        return dense[std::size_t(day - first_day) * campaigns + id];
    }
    void merge_into(SparseTotals& combined) const {
        for (const auto& [key, value] : sparse) combined[key].add(value);
        for (std::size_t i = 0; i < dense.size(); ++i) {
            if (dense[i].sessions || dense[i].assists)
                combined[{int(i % campaigns), first_day + int(i / campaigns)}].add(dense[i]);
        }
    }
};

void aggregate(std::vector<Event>& events, std::vector<Click>& clicks, const Catalog& catalog,
               CampaignTotals& totals, UserRows& users) {
    struct TimedEvent { Millis ts; int price, category; };
    struct TimedClick { Millis ts; Text id; int campaign; };
    struct Activity {
        Name person;
        std::vector<TimedEvent> events;
        std::vector<TimedClick> clicks;
    };
    std::vector<Activity> people;
    std::unordered_map<Name, std::size_t, NameHash> positions;
    positions.reserve(events.size() / 16);
    people.reserve(events.size() / 16);
    for (const auto& e : events) {
        auto [pos, fresh] = positions.try_emplace(e.person, people.size());
        if (fresh) people.push_back({e.person, {}, {}});
        people[pos->second].events.push_back({e.ts, e.price, e.category});
    }
    std::vector<Event>().swap(events);
    for (const auto& c : clicks) {
        auto pos = positions.find(c.person);
        if (pos != positions.end()) people[pos->second].clicks.push_back({c.ts, c.id, c.campaign});
    }
    std::vector<Click>().swap(clicks);
    std::vector<int> click_counts(catalog.campaign_ids.size()), positions_in_window(click_counts.size());
    std::vector<int> active, purchased;
    std::vector<std::int64_t> category_revenue(catalog.categories.size());
    for (auto& activity : people) {
        auto& events = activity.events;
        auto& clicks = activity.clicks;
        std::sort(events.begin(), events.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.ts < b.ts; });
        std::sort(clicks.begin(), clicks.end(), [](const TimedClick& a, const TimedClick& b) {
            return a.ts == b.ts ? a.id > b.id : a.ts < b.ts;
        });
        std::size_t ei = 0, left = 0, right = 0;
        int user_day = 0, sessions = 0;
        std::int64_t daily_events = 0, daily_revenue = 0;
        auto write_day = [&] {
            int best = -1;
            std::int64_t best_revenue = -1;
            for (int id : purchased) {
                auto cents = category_revenue[id];
                if (cents > best_revenue || (cents == best_revenue && catalog.categories[id] < catalog.categories[best])) {
                    best = id;
                    best_revenue = cents;
                }
                category_revenue[id] = 0;
            }
            purchased.clear();
            users.append(activity.person, user_day, sessions, daily_events, daily_revenue,
                         best < 0 ? nullptr : &catalog.categories[best]);
        };
        while (ei < events.size()) {
            Millis start = events[ei].ts;
            int day = static_cast<int>(start / day_ms - (start % day_ms < 0));
            if (sessions && day != user_day) {
                write_day();
                sessions = 0;
                daily_events = daily_revenue = 0;
            }
            user_day = day;
            ++sessions;
            auto first = ei;
            std::int64_t revenue = 0;
            do {
                const auto& event = events[ei++];
                if (event.price) {
                    revenue += event.price;
                    if (!category_revenue[event.category]) purchased.push_back(event.category);
                    category_revenue[event.category] += event.price;
                }
            } while (ei < events.size() && events[ei].ts - events[ei - 1].ts <= session_gap);
            daily_events += ei - first;
            daily_revenue += revenue;
            // Identical duplicate clicks enter and leave together. Only distinct campaigns
            // and the identity of the final click affect either output.
            while (right < clicks.size() && clicks[right].ts <= start) {
                int id = clicks[right++].campaign;
                if (click_counts[id]++ == 0) {
                    positions_in_window[id] = active.size();
                    active.push_back(id);
                }
            }
            while (left < right && clicks[left].ts < start - week_ms) {
                int campaign = clicks[left++].campaign;
                if (!--click_counts[campaign]) {
                    int pos = positions_in_window[campaign];
                    active[pos] = active.back();
                    positions_in_window[active[pos]] = pos;
                    active.pop_back();
                }
            }
            int credited = left == right ? 0 : clicks[right - 1].campaign;
            auto& metrics = totals.get(credited, day);
            ++metrics.sessions;
            metrics.revenue += revenue;
            metrics.purchasing += revenue > 0;
            for (int campaign : active) ++totals.get(campaign, day).assists;
        }
        write_day();
        for (int campaign : active) click_counts[campaign] = 0;
        active.clear();
    }
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: sessions DATA OUTPUT WINDOW_START_MS WINDOW_END_MS THREADS\n";
        return 2;
    }
    try {
        fs::path data = argv[1], output = argv[2];
        Millis start = std::stoll(argv[3]), end = std::stoll(argv[4]);
        threads = std::clamp(std::stoi(argv[5]), 1, 8);
        Files links(data / "links"), products(data / "products.csv"), campaigns(data / "campaigns.csv");
        Files event_files(data / "events"), click_files(data / "clicks");
        People people(links);
        Catalog catalog(products, campaigns);
        Partition<Event> events;
        Partition<Click> clicks;
        std::vector<NamePool> names(threads);
        load_events(event_files, people, catalog, start, end, events, names);
        load_clicks(click_files, people, catalog, start, end, clicks, names);
        auto day_of = [](Millis ts) { return int(ts / day_ms - (ts % day_ms < 0)); };
        std::vector<CampaignTotals> totals;
        for (std::size_t t = 0; t < threads; ++t)
            totals.emplace_back(day_of(start), day_of(end), catalog.campaign_ids.size());
        fs::create_directories(output / "user_daily");
        fs::create_directories(output / "campaign_daily");
        std::vector<UserRows> users;
        for (std::size_t t = 0; t < threads; ++t)
            users.emplace_back(output / "user_daily" / ("part-" + std::to_string(t) + ".parquet"));
        for (std::size_t t = threads; t < 8; ++t)
            fs::remove(output / "user_daily" / ("part-" + std::to_string(t) + ".parquet"));
        std::array<std::pair<std::size_t, std::size_t>, shards> order{};
        for (std::size_t shard = 0; shard < shards; ++shard) {
            order[shard].second = shard;
            for (const auto& worker : events.local) order[shard].first += worker[shard].size();
            for (const auto& worker : clicks.local) order[shard].first += worker[shard].size();
        }
        std::sort(order.rbegin(), order.rend());
        parallel(shards, [&](auto t, auto job) {
            auto shard = order[job].second;
            auto e = events.take(shard);
            auto c = clicks.take(shard);
            aggregate(e, c, catalog, totals[t], users[t]);
        });
        for (auto& stream : users) stream.close();
        SparseTotals combined;
        for (const auto& local : totals) local.merge_into(combined);
        auto campaign_file = parquet_file(output / "campaign_daily" / "part-0.parquet", {
            column("campaign_id", pq::Type::BYTE_ARRAY, pq::ConvertedType::UTF8),
            column("channel", pq::Type::BYTE_ARRAY, pq::ConvertedType::UTF8),
            column("day", pq::Type::INT32, pq::ConvertedType::DATE), column("sessions", pq::Type::INT64),
            column("purchasing_sessions", pq::Type::INT64), column("revenue_cents", pq::Type::INT64),
            column("assisted_sessions", pq::Type::INT64)});
        if (!combined.empty()) {
            auto group = campaign_file->AppendRowGroup();
            std::vector<pq::ByteArray> ids, channels;
            std::vector<std::int32_t> days;
            std::vector<std::int64_t> counts, purchasing, revenue, assists;
            for (const auto& [key, metrics] : combined) {
                ids.push_back(binary(catalog.campaign_ids[key.id]));
                channels.push_back(binary(catalog.channels[key.id]));
                days.push_back(key.day); counts.push_back(metrics.sessions);
                purchasing.push_back(metrics.purchasing); revenue.push_back(metrics.revenue);
                assists.push_back(metrics.assists);
            }
            put_column<pq::ByteArrayWriter>(group, ids); put_column<pq::ByteArrayWriter>(group, channels);
            put_column<pq::Int32Writer>(group, days); put_column<pq::Int64Writer>(group, counts);
            put_column<pq::Int64Writer>(group, purchasing); put_column<pq::Int64Writer>(group, revenue);
            put_column<pq::Int64Writer>(group, assists);
            group->Close();
        }
        campaign_file->Close();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
