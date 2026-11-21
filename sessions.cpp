#include "parquet_output.hpp"
#include <memory>
#include <map>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
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
template <typename T, std::size_t bins = shards> struct Partition {
    std::vector<std::array<std::vector<T>, bins>> local{threads};
    void reserve_rows(std::size_t rows) {
        const auto per_bucket = std::min<std::size_t>(131072, rows / (local.size() * bins));
        for (auto& worker : local) for (auto& bucket : worker) bucket.reserve(per_bucket);
    }
    void add(std::size_t thread, std::size_t hash, T row) {
        local[thread][hash % bins].push_back(row);
    }
};

// Publish graph nodes through their keys. Parent links always point to a strictly
// smaller name, so concurrent unions cannot form a cycle. A successful union
// records the child once; these are precisely the aliases that need a lookup.
struct ConnectedAliases {
    struct Node {
        std::atomic<std::uint64_t> key{Name::external};
        std::atomic<int> parent{-1};
    };
    struct alignas(64) Reader {
        NamePool names;
        std::vector<int> children;
    };

    std::size_t mask;
    std::unique_ptr<Node[]> nodes;
    std::vector<Reader> readers{threads};

    explicit ConnectedAliases(const Files& links) {
        std::size_t rows = 0;
        for (Text chunk : links.chunks)
            rows += std::count(chunk.begin(), chunk.end(), '\n') + (!chunk.empty() && chunk.back() != '\n');

        // At most two distinct IDs can be introduced by a row. One empty slot
        // is sufficient to guarantee that every unsuccessful probe terminates.
        std::size_t capacity = 16;
        while (capacity * 19 / 20 < 2 * rows) {
            if (capacity > static_cast<std::size_t>(INT32_MAX) / 2)
                throw std::length_error("too many linked identities");
            capacity *= 2;
        }
        mask = capacity - 1;
        nodes = std::make_unique<Node[]>(capacity);
    }

    int intern(Name name) {
        auto i = name.hash() & mask;
        for (;;) {
            auto key = nodes[i].key.load(std::memory_order_acquire);
            if (key == Name::external && nodes[i].key.compare_exchange_strong(
                    key, name.value, std::memory_order_acq_rel, std::memory_order_acquire))
                return static_cast<int>(i);
            if (Name{key} == name) return static_cast<int>(i);
            i = (i + 1) & mask;
        }
    }

    int root(int i) const {
        for (;;) {
            auto parent = nodes[i].parent.load(std::memory_order_acquire);
            if (parent < 0) return i;
            auto next = nodes[parent].parent.load(std::memory_order_acquire);
            if (next < 0) return parent;
            nodes[i].parent.compare_exchange_weak(parent, next, std::memory_order_release,
                                                  std::memory_order_relaxed);
            i = next;
        }
    }

    Name name(int i) const { return {nodes[i].key.load(std::memory_order_acquire)}; }

    void read(Text chunk, std::size_t worker) {
        auto& reader = readers[worker];
        records<2>(chunk, [&](const auto& row) {
            if (row[0].empty() || row[1].empty()) return;
            int a = intern(reader.names.get(row[0]));
            int b = intern(reader.names.get(row[1]));
            for (;;) {
                a = root(a);
                b = root(b);
                if (a == b) return;
                if (name(a) > name(b)) std::swap(a, b);
                int expected = -1;
                if (nodes[b].parent.compare_exchange_weak(expected, a, std::memory_order_release,
                                                          std::memory_order_relaxed)) {
                    reader.children.push_back(b);
                    return;
                }
            }
        });
    }
};

struct People {
    struct Entry { Name name, owner; };
    std::vector<Entry> entries;
    std::vector<std::uint64_t> membership;
    std::vector<NamePool> storage;

    std::size_t slot(Name name) const {
        const auto mask = entries.size() - 1;
        auto i = name.hash() & mask;
        while (!entries[i].name.empty() && entries[i].name != name) i = (i + 1) & mask;
        return i;
    }

    explicit People(ConnectedAliases& links) {
        std::size_t aliases = 0, capacity = 16;
        for (const auto& reader : links.readers) aliases += reader.children.size();
        while (capacity * 6 / 10 < aliases) capacity *= 2;
        entries.resize(capacity);
        membership.resize(std::max<std::size_t>(1, capacity / 8));
        storage.reserve(links.readers.size());
        for (auto& reader : links.readers) {
            for (int child : reader.children) {
                auto name = links.name(child);
                entries[slot(name)] = {name, links.name(links.root(child))};
                auto h = name.hash();
                membership[(h >> 6) & (membership.size() - 1)] |= UINT64_C(1) << (h & 63);
            }
            storage.push_back(std::move(reader.names));
        }
    }

    Name find(Name name) const {
        auto h = name.hash();
        if (!(membership[(h >> 6) & (membership.size() - 1)] & (UINT64_C(1) << (h & 63)))) return name;
        const auto& e = entries[slot(name)];
        return e.name.empty() ? name : e.owner;
    }
};

struct CatalogIndex {
    struct Entry { Text key; int value = -1; };
    Names fallback;
    Text prefix;
    std::vector<Entry> dense;

    void compact() {
        if (fallback.empty()) return;
        auto sample = fallback.begin()->first;
        auto cut = sample.size();
        while (cut && sample[cut-1] >= '0' && sample[cut-1] <= '9') --cut;
        if (cut == sample.size()) return;
        prefix = sample.substr(0, cut);
        std::size_t limit = std::min<std::size_t>(fallback.size()*4, 4000000), largest = 0;
        for (const auto& [key, value] : fallback) {
            if (key.size() <= cut || key.substr(0, cut) != prefix) return;
            auto i = number(key.substr(cut), limit);
            if (i < 0) return;
            largest = std::max(largest, std::size_t(i));
        }
        dense.resize(largest + 1);
        for (const auto& [key, value] : fallback) {
            auto& entry = dense[number(key.substr(cut), limit)];
            if (entry.value != -1) { dense.clear(); return; }
            entry = {key, value};
        }
        Names().swap(fallback);
    }
    int find(Text key) const {
        if (!dense.empty()) {
            if (key.size() <= prefix.size()) return -1;
            const auto i = number(key.substr(prefix.size()), dense.size()-1);
            return i < 0 || dense[i].key != key ? -1 : dense[i].value;
        }
        auto it = fallback.find(key);
        return it == fallback.end() ? -1 : it->second;
    }
};

struct Catalog {
    CatalogIndex products, campaigns;
    std::vector<Text> categories{"unknown"}, campaign_ids{"organic"}, channels{"organic"};
    Catalog(const Files& product_files, const Files& campaign_files) {
        Names category_index{{"unknown", 0}};
        for (auto chunk : product_files.chunks) each_line(chunk, [&](Text line) {
            std::array<Text, 2> f;
            if (!fields(line, f)) return;
            auto [category, fresh] = category_index.try_emplace(f[1], static_cast<int>(categories.size()));
            if (fresh) categories.push_back(f[1]);
            products.fallback[f[0]] = category->second;
        });
        for (auto chunk : campaign_files.chunks) each_line(chunk, [&](Text line) {
            std::array<Text, 2> f;
            if (!fields(line, f)) return;
            campaigns.fallback.emplace(f[0], static_cast<int>(campaign_ids.size()));
            campaign_ids.push_back(f[0]);
            channels.push_back(f[1]);
        });
        products.compact();
        campaigns.compact();
    }
};

struct Sale { int price, category; };

// Use exact relative milliseconds and the published price bound whenever the
// manifest span and catalog fit. Wider workloads use per-reader side tables.
struct PackedValues {
    Millis origin;
    bool narrow_time, narrow_sales;
    std::vector<std::vector<Millis>> full_times;
    std::vector<std::vector<Sale>> full_sales;
    PackedValues(Millis start, Millis end, std::size_t categories)
        : origin(start - week_ms), narrow_time(end - origin <= UINT32_MAX),
          narrow_sales(categories <= 256), full_times(threads), full_sales(threads) {}

    template<typename T>
    std::uint32_t store(std::vector<std::vector<T>>& all, std::size_t reader, T value) {
        auto& values = all[reader];
        if (values.size() >= (UINT32_MAX >> 3)) throw std::length_error("too many wide values");
        auto key = static_cast<std::uint32_t>((values.size() << 3) | reader);
        values.push_back(value);
        return key;
    }
    std::uint32_t time(Millis value, std::size_t reader) {
        return narrow_time ? static_cast<std::uint32_t>(value - origin) : store(full_times, reader, value);
    }
    Millis millis(std::uint32_t key) const {
        return narrow_time ? origin + key : full_times[key & 7][key >> 3];
    }
    std::uint32_t purchase(int price, int category, std::size_t reader) {
        if (!price) return 0;
        return narrow_sales ? (std::uint32_t(category) << 24) | price : store(full_sales, reader, Sale{price, category}) + 1;
    }
    Sale sale(std::uint32_t key) const {
        if (narrow_sales) return {int(key & 0xffffff), int(key >> 24)};
        if (!key) return {};
        --key;
        return full_sales[key & 7][key >> 3];
    }
};

struct Event { Name person; std::uint32_t stamp, purchase; };
struct Click { Name person, id; std::uint32_t stamp; int campaign; };
// F1 has fixed-width UTC fields. Byte ordering is chronological ordering.
struct Version {
    Name id;
    const char *ingest;
    Event event;
};

Event validate_event(const std::array<Text, 7>& f, Millis ingest, Millis start, Millis end,
                     const Catalog& catalog, NamePool& names, PackedValues& values, std::size_t reader) {
    if (f[1].empty() || f[5].empty()) return {};
    const auto ts = timestamp(f[2]);
    if (ts < start || ts >= end || ingest - ts > day_ms) return {};
    int price = 0, category = 0;
    if (f[4] == "purchase") {
        price = static_cast<int>(number(f[6], 10'000'000));
        if (price < 1) return {};
        auto product = catalog.products.find(f[5]);
        if (product >= 0) category = product;
    } else if (f[4] != "view" && f[4] != "cart") return {};
    return {names.get(f[1]), values.time(ts, reader), values.purchase(price, category, reader)};
}

void load_events(const Files& files, const Files& links, std::optional<People>& people, const Catalog& catalog,
                 Millis start, Millis end, Partition<Event>& out, std::vector<NamePool>& names, PackedValues& values) {
    constexpr std::size_t id_shards = 256;
    Partition<Version, id_shards> versions;
    versions.reserve_rows(files.estimated_rows());
    std::vector<NamePool> ids(threads);
    const auto lookahead = 2 * threads;
    files.start_prefetch(lookahead);
    auto graph = std::make_unique<ConnectedAliases>(links);
    parallel(links.chunks.size() + files.chunks.size(), [&](auto t, auto j) {
        if (j < links.chunks.size()) {
            graph->read(links.chunks[j], t);
            return;
        }
        files.prefetch(j - links.chunks.size() + lookahead);
        auto general = [&](const auto &f) {
            if (f[0].empty())
                return;
            auto id = ids[t].get(f[0]);
            auto hash = id.hash();
            auto ingest = ingest_time(f[3]);
            if (ingest == invalid_time)
                return;
            // Retain the earliest version even when its payload fails validation.
            versions.add(
                t, hash,
                {id, f[3].data(), validate_event(f, ingest, start, end, catalog, names[t], values, t)});
        };
        // Field widths were verified from this row's delimiter and quote masks.
        auto direct = [&](const char *row, const auto &shape) {
            auto ptr = [&](unsigned i) { return row + shape.begin[i]; };
            auto id = ids[t].get(Text(ptr(0), 9));
            auto hash = id.hash();
            auto ingest = ptr(3);
            auto arrival_day = midnight(ingest);
            if (arrival_day == invalid_time)
                return;
            auto emit = [&](Event event) { versions.add(t, hash, {id, ingest, event}); };
            if (!shape.length[5]) {
                emit({});
                return;
            }
            Millis ts;
            if (shape.length[2] == 20)
                ts = timestamp_at_width<20>(ptr(2), ingest, arrival_day);
            else if (shape.length[2] == 25)
                ts = timestamp_at_width<25>(ptr(2), ingest, arrival_day);
            else
                ts = timestamp_at_width<13>(ptr(2));
            if (ts < start || ts >= end) {
                emit({});
                return;
            }
            // An event at or after ingestion midnight is within the one-day allowance.
            if (ts < arrival_day) {
                auto lag = arrival_day - ts;
                if (lag > day_ms || lag + 1000 * daytime<false>(ingest + 11) > day_ms) {
                    emit({});
                    return;
                }
            }
            int price = 0, category = 0;
            if (shape.length[4] == 8) {
                if (std::memcmp(ptr(4), "purchase", 8)) {
                    emit({});
                    return;
                }
                price = number(Text(ptr(6), shape.length[6]), 10'000'000);
                if (price < 1) {
                    emit({});
                    return;
                }
                category = std::max(0, catalog.products.find(Text(ptr(5), shape.length[5])));
            } else if (std::memcmp(ptr(4), "view", 4) && std::memcmp(ptr(4), "cart", 4)) {
                emit({});
                return;
            }
            emit({names[t].get(Text(ptr(1), 9)), values.time(ts, t), values.purchase(price, category, t)});
        };
        FixedWidthRows<7, decltype(general), decltype(direct)> receiver(general, direct);
        records<7>(files.chunks[j - links.chunks.size()], receiver);
    });
    people.emplace(*graph);
    graph.reset();
    out.reserve_rows(files.estimated_rows());
    parallel(id_shards, [&](auto t, auto shard) {
        std::size_t count = 0;
        for (const auto& worker : versions.local) count += worker[shard].size();
        std::size_t words = 1;
        while (words * 4 < count) words *= 2;
        // A bucket touched just once proves its identifier is unique. Repeated
        // buckets, including accidental hash collisions, still get exact compares.
        std::vector<std::uint64_t> seen(words), repeated(words);
        std::size_t collisions = 0;
        for (const auto& worker : versions.local) for (const auto& row : worker[shard]) {
            const auto hash = row.id.hash() / id_shards;
            auto word = (hash >> 6) & (words - 1);
            auto bit = UINT64_C(1) << (hash & 63);
            auto conflict = seen[word] & bit;
            collisions += conflict != 0;
            repeated[word] |= conflict;
            seen[word] |= bit;
        }
        std::size_t capacity = 1;
        while (capacity < std::min(count, collisions * 2) * 2) capacity *= 2;
        std::vector<Version*> slots(capacity);
        auto emit = [&](Event event) {
            if (!event.person.empty()) {
                event.person = people->find(event.person);
                out.add(t, event.person.hash(), event);
            }
        };
        for (auto& worker : versions.local) for (auto& row : worker[shard]) {
            const auto hash = row.id.hash() / id_shards;
            const auto word = (hash >> 6) & (words - 1);
            if (!(repeated[word] & (UINT64_C(1) << (hash & 63)))) {
                emit(row.event);
                continue;
            }
            auto slot = (hash ^ (hash >> 24)) & (capacity - 1);
            while (slots[slot] && slots[slot]->id != row.id) slot = (slot + 1) & (capacity - 1);
            if (!slots[slot] || std::memcmp(row.ingest, slots[slot]->ingest, 20) < 0)
                slots[slot] = &row;
        }
        for (auto* row : slots) if (row) emit(row->event);
        for (auto& worker : versions.local) std::vector<Version>().swap(worker[shard]);
    });
}

void load_clicks(const Files& files, const People& people, const Catalog& catalog,
                 Millis start, Millis end, Partition<Click>& out, std::vector<NamePool>& names, PackedValues& values) {
    out.reserve_rows(files.estimated_rows());
    const auto lookahead = 2 * threads;
    files.start_prefetch(lookahead);
    parallel(files.chunks.size(), [&](auto t, auto j) {
        files.prefetch(j + lookahead);
        auto general = [&](const auto &f) {
            if (f[0].empty() || f[1].empty())
                return;
            auto ts = timestamp(f[3]);
            if (ts < start - week_ms || ts >= end)
                return;
            auto campaign = catalog.campaigns.find(f[2]);
            if (campaign < 0)
                return;
            auto person = people.find(names[t].get(f[1]));
            out.add(t, person.hash(), {person, names[t].get(f[0]), values.time(ts, t), campaign});
        };
        // Field widths were verified from this row's delimiter and quote masks.
        auto direct = [&](const char *row, const auto &shape) {
            auto ptr = [&](unsigned i) { return row + shape.begin[i]; };
            auto tsval = ptr(3);
            auto ts = shape.length[3] == 13   ? timestamp_at_width<13>(tsval)
                      : shape.length[3] == 20 ? timestamp_at_width<20>(tsval)
                                              : timestamp_at_width<25>(tsval);
            if (ts < start - week_ms || ts >= end)
                return;
            auto campaign = catalog.campaigns.find(Text(ptr(2), shape.length[2]));
            if (campaign < 0)
                return;
            auto name = [&](unsigned i) {
                return shape.length[i] == 9 ? names[t].get(Text(ptr(i), 9)) : names[t].get(Text(ptr(i), 8));
            };
            auto person = people.find(name(1));
            out.add(t, person.hash(), {person, name(0), values.time(ts, t), campaign});
        };
        FixedWidthRows<4, decltype(general), decltype(direct)> receiver(general, direct);
        records<4>(files.chunks[j], receiver);
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

// In a narrow window the high 32 bits order events by their exact timestamp.
// The low 32 bits preserve purchase amount and category, including at ties.
struct WideEvent {
    Millis ts;
    int price, category;
};
template <bool Narrow> using EventTimeRow = std::conditional_t<Narrow, std::uint64_t, WideEvent>;
template <bool Narrow> Millis row_time(const EventTimeRow<Narrow>& row, Millis epoch) {
    if constexpr (Narrow)
        return epoch + static_cast<std::uint32_t>(row >> 32);
    else
        return row.ts;
}

// A half-hour bin forms one connected piece of a session. Only its earliest
// and latest event can determine whether it joins the adjacent nonempty bin.
template <bool Narrow> struct SessionOrder {
    using TimedEvent = EventTimeRow<Narrow>;
    struct Bin { std::size_t count = 0, begin, cursor, earliest, latest; };
    Millis origin;
    std::vector<Bin> bins;
    std::vector<TimedEvent> scratch;
    SessionOrder(Millis start, Millis end) : origin(start) {
        const auto n = (end - start) / session_gap + 1;
        if (n > 0 && n <= 8192) bins.resize(n);
    }
    void arrange(std::vector<TimedEvent>& data, std::size_t begin, std::size_t end) {
        auto n = end - begin;
        auto event_time = [&](const TimedEvent& e) { return row_time<Narrow>(e, origin - week_ms); };
        if (bins.empty() || n < 128 || n * 4 < bins.size()) {
            if constexpr (Narrow)
                std::sort(data.begin() + begin, data.begin() + end);
            else
                std::sort(data.begin() + begin, data.begin() + end,
                          [](const auto& a, const auto& b) { return a.ts < b.ts; });
            return;
        }
        auto bucket = [&](Millis ts) { return std::size_t((ts - origin) / session_gap); };
        for (auto i = begin; i < end; ++i) ++bins[bucket(event_time(data[i]))].count;
        std::size_t position = 0;
        for (auto& b : bins) if (b.count) {
            b.earliest = b.latest = b.cursor = b.begin = position;
            position += b.count;
        }
        scratch.resize(n);
        for (auto i = begin; i < end; ++i) {
            const auto& event = data[i];
            auto& b = bins[bucket(event_time(event))];
            auto pos = b.cursor++;
            scratch[pos] = event;
            if (event_time(event) < event_time(scratch[b.earliest])) b.earliest = pos;
            if (event_time(event) > event_time(scratch[b.latest])) b.latest = pos;
        }
        for (auto& b : bins) if (b.count) {
            std::swap(scratch[b.begin], scratch[b.earliest]);
            if (b.latest == b.begin) b.latest = b.earliest;
            std::swap(scratch[b.cursor-1], scratch[b.latest]);
            b.count = 0;
        }
        std::copy(scratch.begin(), scratch.end(), data.begin()+begin);
    }
};

template <bool Narrow>
void aggregate(Partition<Event>& raw_events, Partition<Click>& raw_clicks, std::size_t shard,
               const Catalog& catalog, CampaignTotals& totals, UserRows& users, Millis start, Millis end,
               const PackedValues& values) {
    using TimedEvent = EventTimeRow<Narrow>;
    SessionOrder<Narrow> order(start, end);
    auto event_time = [&](const TimedEvent& e) { return row_time<Narrow>(e, values.origin); };
    using ClickStamp = std::conditional_t<Narrow, std::uint32_t, Millis>;
    struct TimedClick {
        Name id;
        ClickStamp ts;
        int campaign;
    };
    auto click_time = [&](const TimedClick& c) {
        if constexpr (Narrow)
            return values.origin + c.ts;
        else
            return c.ts;
    };
    struct Activity {
        Name person;
        std::size_t event_begin = 0, event_end = 0, click_begin = 0, click_end = 0;
    };
    std::size_t raw_count = 0;
    for (const auto& worker : raw_events.local) raw_count += worker[shard].size();
    std::vector<Activity> people;
    people.reserve(raw_count / 16);
    std::size_t capacity = 16;
    while (capacity < raw_count / 8) capacity *= 2;
    std::vector<int> slots(capacity, -1);
    auto locate = [&](Name person) {
        auto mask = slots.size() - 1;
        // The lower bits already selected this shard.
        auto pos = (person.hash() / shards) & mask;
        while (slots[pos] != -1 && people[slots[pos]].person != person) pos = (pos + 1) & mask;
        return pos;
    };
    // Count once, then scatter into contiguous runs. The temporary group number
    // replaces the identity in these shard-local rows after its final lookup.
    for (auto& worker : raw_events.local) for (auto& e : worker[shard]) {
        auto pos = locate(e.person);
        if (slots[pos] == -1) {
            if (people.size() * 10 >= slots.size() * 7) {
                slots.assign(slots.size() * 2, -1);
                for (std::size_t i = 0; i < people.size(); ++i) slots[locate(people[i].person)] = i;
                pos = locate(e.person);
            }
            slots[pos] = people.size();
            people.push_back({e.person});
        }
        auto i = slots[pos];
        ++people[i].event_end;
        e.person.value = i;
    }
    for (auto& worker : raw_clicks.local) for (auto& c : worker[shard]) {
        auto i = slots[locate(c.person)];
        if (i >= 0) ++people[i].click_end;
        c.person.value = i < 0 ? Name::external : static_cast<std::uint64_t>(i);
    }
    std::size_t ne = 0, nc = 0;
    for (auto& p : people) {
        p.event_begin = ne; ne += p.event_end; p.event_end = p.event_begin;
        p.click_begin = nc; nc += p.click_end; p.click_end = p.click_begin;
    }
    std::vector<TimedEvent> events(ne);
    std::vector<TimedClick> clicks(nc);
    for (auto& worker : raw_events.local) {
        for (const auto& e : worker[shard]) {
            auto& destination = events[people[e.person.value].event_end++];
            if constexpr (Narrow)
                destination = (std::uint64_t(e.stamp) << 32) | e.purchase;
            else {
                const auto sale = values.sale(e.purchase);
                destination = {values.millis(e.stamp), sale.price, sale.category};
            }
        }
        std::vector<Event>().swap(worker[shard]);
    }
    for (auto& worker : raw_clicks.local) {
        for (const auto& c : worker[shard]) {
            if (c.person.empty()) continue;
            auto& item = clicks[people[c.person.value].click_end++];
            if constexpr (Narrow)
                item = {c.id, c.stamp, c.campaign};
            else
                item = {c.id, values.millis(c.stamp), c.campaign};
        }
        std::vector<Click>().swap(worker[shard]);
    }
    std::vector<int> click_counts(catalog.campaign_ids.size()), positions_in_window(click_counts.size());
    std::vector<int> active, purchased;
    std::vector<std::int64_t> category_revenue(catalog.categories.size());
    for (auto& activity : people) {
        order.arrange(events, activity.event_begin, activity.event_end);
        std::sort(clicks.begin() + activity.click_begin, clicks.begin() + activity.click_end, [](const TimedClick& a, const TimedClick& b) {
            return a.ts == b.ts ? a.id > b.id : a.ts < b.ts;
        });
        std::size_t ei = activity.event_begin, left = activity.click_begin, right = left;
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
        while (ei < activity.event_end) {
            Millis start = event_time(events[ei]);
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
                Sale sale;
                if constexpr (Narrow)
                    sale = {int(event & 0xffffff), int(std::uint32_t(event) >> 24)};
                else
                    sale = {event.price, event.category};
                if (sale.price) {
                    revenue += sale.price;
                    if (!category_revenue[sale.category]) purchased.push_back(sale.category);
                    category_revenue[sale.category] += sale.price;
                }
            } while (ei < activity.event_end &&
                     event_time(events[ei]) - event_time(events[ei - 1]) <= session_gap);
            daily_events += ei - first;
            daily_revenue += revenue;
            // Identical duplicate clicks enter and leave together. Only distinct campaigns
            // and the identity of the final click affect either output.
            while (right < activity.click_end && click_time(clicks[right]) <= start) {
                int id = clicks[right++].campaign;
                if (click_counts[id]++ == 0) {
                    positions_in_window[id] = active.size();
                    active.push_back(id);
                }
            }
            while (left < right && click_time(clicks[left]) < start - week_ms) {
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

const char* environment(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value ? value : fallback;
}

// The native path uses the same validated civil-time arithmetic as event rows.
// The compatibility launcher also accepts less common ISO 8601 representations.
std::optional<std::pair<Millis, Millis>> manifest_window(const fs::path& data) {
    std::ifstream input(data / "manifest.json");
    if (!input) throw std::runtime_error("cannot open " + (data / "manifest.json").string());
    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Text text(contents);
    std::optional<Text> start, end;
    auto skip = [&] {
        while (!text.empty() &&
               (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
            text.remove_prefix(1);
    };
    auto take = [&](char c) {
        skip();
        if (text.empty() || text.front() != c)
            return false;
        text.remove_prefix(1);
        return true;
    };
    auto quoted = [&]() -> std::optional<Text> {
        if (!take('"'))
            return {};
        auto k = text.find('"');
        if (k == Text::npos)
            return {};
        auto out = text.substr(0, k);
        if (out.find('\\') != Text::npos)
            return {};
        text.remove_prefix(k + 1);
        return out;
    };
    if (!take('{'))
        return {};
    for (int i = 0; i < 2; i++) {
        if (i && !take(','))
            return {};
        auto key = quoted();
        if (!key || !take(':'))
            return {};
        auto value = quoted();
        if (!value)
            return {};
        if (*key == "window_start" && !start)
            start = value;
        else if (*key == "window_end" && !end)
            end = value;
        else
            return {};
    }
    if (!take('}') || !start || !end)
        return {};
    skip();
    if (!text.empty())
        return {};
    auto parse_iso = [](Text s) {
        return (s.size() == 20 || s.size() == 25) && s[4] == '-' ? timestamp(s) : invalid_time;
    };
    Millis first = parse_iso(*start), last = parse_iso(*end);
    if (first == invalid_time || last == invalid_time) return std::nullopt;
    return std::pair{first, last};
}

int main(int argc, char** argv) {
    if (argc != 1 && argc != 2 && argc != 3 && argc != 6) {
        std::cerr << "usage: sessions [DATA [OUTPUT [WINDOW_START_MS WINDOW_END_MS THREADS]]]\n";
        return 2;
    }
    try {
        fs::path data = argc >= 2 ? argv[1] : environment("DATA_DIR", "/data");
        fs::path output = argc >= 3 ? argv[2] : environment("OUTPUT_DIR", "/output");
        Millis start, end;
        if (argc == 6) {
            start = std::stoll(argv[3]);
            end = std::stoll(argv[4]);
        } else if (auto window = manifest_window(data)) {
            std::tie(start, end) = *window;
        } else {
            auto script = fs::canonical(argv[0]).parent_path() / "run.py";
            execlp("python3", "python3", script.c_str(), data.c_str(), output.c_str(), nullptr);
            throw std::runtime_error("cannot start manifest compatibility launcher");
        }
        threads = std::clamp(std::stoi(argc == 6 ? argv[5] : environment("NCPU", "8")), 1, 8);
        Files links(data / "links"), products(data / "products.csv"), campaigns(data / "campaigns.csv");
        Files event_files(data / "events"), click_files(data / "clicks");
        std::optional<People> people;
        Catalog catalog(products, campaigns);
        PackedValues values(start, end, catalog.categories.size());
        Partition<Event> events;
        Partition<Click> clicks;
        std::vector<NamePool> names(threads);
        load_events(event_files, links, people, catalog, start, end, events, names, values);
        load_clicks(click_files, *people, catalog, start, end, clicks, names, values);
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
            if (values.narrow_time && values.narrow_sales)
                aggregate<true>(events, clicks, shard, catalog, totals[t], users[t], start, end, values);
            else
                aggregate<false>(events, clicks, shard, catalog, totals[t], users[t], start, end, values);
        });
        for (auto& stream : users) stream.close();
        SparseTotals combined;
        for (const auto& local : totals) local.merge_into(combined);
        FlatParquet report(output / "campaign_daily" / "part-0.parquet", {{"campaign_id", 6, 0},
                                                                          {"channel", 6, 0},
                                                                          {"day", 1, 6},
                                                                          {"sessions", 2},
                                                                          {"purchasing_sessions", 2},
                                                                          {"revenue_cents", 2},
                                                                          {"assisted_sessions", 2}});
        Bytes ids, channels;
        std::vector<std::int32_t> days;
        std::vector<std::int64_t> counts, purchasing, revenue, assists;
        auto flush = [&] {
            if (days.empty())
                return;
            report.begin(days.size());
            report.page(ids.data(), ids.size());
            report.page(channels.data(), channels.size());
            report.ints(days);
            report.ints(counts);
            report.ints(purchasing);
            report.ints(revenue);
            report.ints(assists);
            ids.clear();
            channels.clear();
            days.clear();
            counts.clear();
            purchasing.clear();
            revenue.clear();
            assists.clear();
        };
        for (const auto &[key, metrics] : combined) {
            byte_string(ids, catalog.campaign_ids[key.id]);
            byte_string(channels, catalog.channels[key.id]);
            days.push_back(key.day);
            counts.push_back(metrics.sessions);
            purchasing.push_back(metrics.purchasing);
            revenue.push_back(metrics.revenue);
            assists.push_back(metrics.assists);
            if (days.size() >= 32768 || ids.size() >= (1U << 26) || channels.size() >= (1U << 26))
                flush();
        }
        flush();
        report.close();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
