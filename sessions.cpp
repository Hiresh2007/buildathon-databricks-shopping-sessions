#include "input.hpp"
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

struct Components {
    Names index;
    std::vector<int> parent, sizes;
    std::vector<Text> canonical;
    int id(Text name) {
        auto [it, fresh] = index.try_emplace(name, static_cast<int>(parent.size()));
        if (fresh) {
            parent.push_back(it->second);
            sizes.push_back(1);
            canonical.push_back(name);
        }
        return it->second;
    }
    int root(int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    }
    void join(Text x, Text y) {
        auto a = root(id(x)), b = root(id(y));
        if (a == b) return;
        if (sizes[a] < sizes[b]) std::swap(a, b);
        parent[b] = a;
        sizes[a] += sizes[b];
        canonical[a] = std::min(canonical[a], canonical[b]);
    }
};

struct People {
    struct Entry { Text name, canonical; };
    std::vector<Entry> lookup;
    explicit People(const Files& links) {
        Components components;
        std::size_t bytes = 0;
        for (auto chunk : links.chunks) bytes += chunk.size();
        components.index.reserve(bytes / 20);
        for (auto chunk : links.chunks) each_line(chunk, [&](Text line) {
            std::array<Text, 2> f;
            if (fields(line, f) && !f[0].empty() && !f[1].empty()) components.join(f[0], f[1]);
        });
        std::size_t capacity = 1;
        while (capacity < components.index.size() * 2) capacity *= 2;
        lookup.resize(capacity);
        for (const auto& [name, i] : components.index) {
            auto slot = std::hash<Text>{}(name) & (capacity - 1);
            while (!lookup[slot].name.empty()) slot = (slot + 1) & (capacity - 1);
            lookup[slot] = {name, components.canonical[components.root(i)]};
        }
    }
    Text find(Text name) const {
        auto mask = lookup.size() - 1;
        auto slot = std::hash<Text>{}(name) & mask;
        while (!lookup[slot].name.empty()) {
            if (lookup[slot].name == name) return lookup[slot].canonical;
            slot = (slot + 1) & mask;
        }
        return name;
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

struct Event { Text person; Millis ts; int price, category; };
struct Click { Text person, id; Millis ts; int campaign; };
struct Version { Text id; std::size_t hash; Millis ingest; Event event; };

Event validate_event(const std::array<Text, 7>& f, Millis ingest, Millis start, Millis end,
                     const People& people, const Catalog& catalog) {
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
    return {people.find(f[1]), ts, price, category};
}

void load_events(const Files& files, const People& people, const Catalog& catalog,
                 Millis start, Millis end, Partition<Event>& out) {
    Partition<Version> versions;
    parallel(files.chunks.size(), [&](auto t, auto j) {
        each_line(files.chunks[j], [&](Text line) {
            std::array<Text, 7> f;
            if (!fields(line, f) || f[0].empty()) return;
            auto hash = std::hash<Text>{}(f[0]);
            auto ingest = ingest_time(f[3]);
            if (ingest == invalid_time) return;
            // Retain the earliest version even when its payload fails validation.
            versions.add(t, hash, {f[0], hash, ingest, validate_event(f, ingest, start, end, people, catalog)});
        });
    });
    parallel(shards, [&](auto t, auto shard) {
        std::size_t count = 0, capacity = 1;
        for (const auto& worker : versions.local) count += worker[shard].size();
        while (capacity < count + count / 2) capacity *= 2;
        std::vector<const Version*> slots(capacity);
        for (const auto& worker : versions.local) {
            for (const auto& row : worker[shard]) {
                // Low hash bits already selected the shard. Probe with the remaining bits.
                auto slot = (row.hash / shards) & (capacity - 1);
                while (slots[slot] && (slots[slot]->hash != row.hash || slots[slot]->id != row.id))
                    slot = (slot + 1) & (capacity - 1);
                if (!slots[slot] || row.ingest < slots[slot]->ingest) slots[slot] = &row;
            }
        }
        for (auto row : slots) {
            if (row && !row->event.person.empty())
                out.add(t, std::hash<Text>{}(row->event.person), row->event);
        }
        for (auto& worker : versions.local) std::vector<Version>().swap(worker[shard]);
    });
}

void load_clicks(const Files& files, const People& people, const Catalog& catalog,
                 Millis start, Millis end, Partition<Click>& out) {
    parallel(files.chunks.size(), [&](auto t, auto j) {
        each_line(files.chunks[j], [&](Text line) {
            std::array<Text, 4> f;
            if (!fields(line, f) || f[0].empty() || f[1].empty()) return;
            auto ts = timestamp(f[3]);
            if (ts < start - week_ms || ts >= end) return;
            auto campaign = catalog.campaigns.find(f[2]);
            if (campaign == catalog.campaigns.end()) return;
            auto person = people.find(f[1]);
            out.add(t, std::hash<Text>{}(person), {person, f[0], ts, campaign->second});
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
using CampaignTotals = std::unordered_map<CampaignDay, Totals, CampaignDayHash>;

// Source strings cannot contain double quotes. Quoting preserves empty strings, spaces and CR.
void quoted(std::ostream& stream, Text text) { stream << '"' << text << '"'; }

void aggregate(std::vector<Event>& events, std::vector<Click>& clicks, const Catalog& catalog,
               CampaignTotals& totals, std::ostream& users) {
    struct TimedEvent { Millis ts; int price, category; };
    struct TimedClick { Millis ts; Text id; int campaign; };
    struct Activity {
        Text person;
        std::vector<TimedEvent> events;
        std::vector<TimedClick> clicks;
    };
    std::vector<Activity> people;
    std::unordered_map<Text, std::size_t> positions;
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
    for (auto& activity : people) {
        auto person = activity.person;
        auto& events = activity.events;
        auto& clicks = activity.clicks;
        std::sort(events.begin(), events.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.ts < b.ts; });
        std::sort(clicks.begin(), clicks.end(), [](const TimedClick& a, const TimedClick& b) {
            return a.ts == b.ts ? a.id > b.id : a.ts < b.ts;
        });
        std::size_t ei = 0, left = 0, right = 0;
        std::unordered_map<int, int> active;
        int user_day = 0, sessions = 0;
        std::int64_t daily_events = 0, daily_revenue = 0;
        std::unordered_map<int, std::int64_t> category_revenue;
        auto write_day = [&] {
            quoted(users, person);
            users << ',' << user_day << ',' << sessions << ',' << daily_events << ',' << daily_revenue << ',';
            int best = -1;
            std::int64_t best_revenue = -1;
            for (auto [id, revenue] : category_revenue)
                if (revenue > best_revenue || (revenue == best_revenue && catalog.categories[id] < catalog.categories[best])) {
                    best = id;
                    best_revenue = revenue;
                }
            if (best != -1) quoted(users, catalog.categories[best]);
            users << '\n';
        };
        while (ei < events.size()) {
            Millis start = events[ei].ts;
            int day = static_cast<int>(start / day_ms - (start % day_ms < 0));
            if (sessions && day != user_day) {
                write_day();
                sessions = 0;
                daily_events = daily_revenue = 0;
                category_revenue.clear();
            }
            user_day = day;
            ++sessions;
            auto first = ei;
            std::int64_t revenue = 0;
            do {
                const auto& event = events[ei++];
                if (event.price) {
                    revenue += event.price;
                    category_revenue[event.category] += event.price;
                }
            } while (ei < events.size() && events[ei].ts - events[ei - 1].ts <= session_gap);
            daily_events += ei - first;
            daily_revenue += revenue;
            // Identical duplicate clicks enter and leave together. Only distinct campaigns
            // and the identity of the final click affect either output.
            while (right < clicks.size() && clicks[right].ts <= start)
                ++active[clicks[right++].campaign];
            while (left < right && clicks[left].ts < start - week_ms) {
                int campaign = clicks[left++].campaign;
                if (!--active[campaign]) active.erase(campaign);
            }
            int credited = left == right ? 0 : clicks[right - 1].campaign;
            auto& metrics = totals[{credited, day}];
            ++metrics.sessions;
            metrics.revenue += revenue;
            metrics.purchasing += revenue > 0;
            for (auto [campaign, count] : active) ++totals[{campaign, day}].assists;
        }
        write_day();
    }
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: sessions DATA SCRATCH WINDOW_START_MS WINDOW_END_MS THREADS\n";
        return 2;
    }
    try {
        fs::path data = argv[1], scratch = argv[2];
        Millis start = std::stoll(argv[3]), end = std::stoll(argv[4]);
        threads = std::clamp(std::stoi(argv[5]), 1, 8);
        Files links(data / "links"), products(data / "products.csv"), campaigns(data / "campaigns.csv");
        Files event_files(data / "events"), click_files(data / "clicks");
        People people(links);
        Catalog catalog(products, campaigns);
        Partition<Event> events;
        Partition<Click> clicks;
        load_events(event_files, people, catalog, start, end, events);
        load_clicks(click_files, people, catalog, start, end, clicks);
        std::vector<CampaignTotals> totals(threads);
        std::vector<std::ofstream> users;
        for (std::size_t t = 0; t < threads; ++t) {
            users.emplace_back(scratch / ("user-" + std::to_string(t) + ".csv"));
            users.back().exceptions(std::ios::failbit | std::ios::badbit);
            users.back() << "user_id,day,sessions,events,revenue_cents,top_category\n";
        }
        parallel(shards, [&](auto t, auto shard) {
            auto e = events.take(shard);
            auto c = clicks.take(shard);
            aggregate(e, c, catalog, totals[t], users[t]);
        });
        for (auto& stream : users) stream.close();
        CampaignTotals combined;
        for (const auto& local : totals) for (const auto& [key, metrics] : local) combined[key].add(metrics);
        std::ofstream output(scratch / "campaign.csv");
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << "campaign_id,channel,day,sessions,purchasing_sessions,revenue_cents,assisted_sessions\n";
        for (auto& [key, metrics] : combined) {
            auto [id, day] = key;
            quoted(output, catalog.campaign_ids[id]);
            output << ',';
            quoted(output, catalog.channels[id]);
            output << ',' << day << ',' << metrics.sessions << ',' << metrics.purchasing << ','
                   << metrics.revenue << ',' << metrics.assists << '\n';
        }
        output.close();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
