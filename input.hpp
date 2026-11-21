#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using Text = std::string_view;
using Millis = std::int64_t;
namespace fs = std::filesystem;
constexpr Millis day_ms = 86'400'000, week_ms = 7 * day_ms;

// Every string_view into a mapped file remains valid through aggregation.
class MappedFile {
    const char* address_ = nullptr;
    std::size_t size_ = 0;
public:
    explicit MappedFile(const fs::path& path) {
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd == -1) throw std::runtime_error("cannot open " + path.string());
        struct stat s{};
        if (fstat(fd, &s) == -1) {
            close(fd);
            throw std::runtime_error("cannot stat " + path.string());
        }
        size_ = s.st_size;
        if (size_) {
            auto* mapped = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapped == MAP_FAILED) {
                close(fd);
                throw std::runtime_error("cannot map " + path.string());
            }
            address_ = static_cast<const char*>(mapped);
        }
        close(fd);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept : address_(other.address_), size_(other.size_) {
        other.address_ = nullptr;
    }
    ~MappedFile() { if (address_) munmap(const_cast<char*>(address_), size_); }
    Text rows() const {
        if (!size_) return {};
        Text content(address_, size_);
        auto end = content.find('\n');
        return end == Text::npos ? Text{} : content.substr(end + 1);
    }
};

struct Files {
    std::vector<MappedFile> mapped;
    std::vector<Text> chunks;
    // Size reservations only; an atypical sample cannot affect row selection.
    std::size_t estimated_rows() const {
        std::size_t rows = 0;
        for (auto chunk : chunks) {
            auto sample = chunk.substr(0, 2048);
            if (!sample.empty()) rows += chunk.size() * std::count(sample.begin(), sample.end(), '\n') / sample.size();
        }
        return rows;
    }
    explicit Files(const fs::path& path) {
        if (fs::is_directory(path)) {
            for (const auto& entry : fs::directory_iterator(path))
                if (entry.path().extension() == ".csv") mapped.emplace_back(entry.path());
        } else {
            mapped.emplace_back(path);
        }
        for (const auto& file : mapped) {
            auto data = file.rows();
            while (!data.empty()) {
                constexpr std::size_t block_size = 1U << 21;
                const auto pos = data.size() > block_size ? data.find('\n', block_size) : Text::npos;
                const auto length = pos == Text::npos ? data.size() : pos + 1;
                chunks.push_back(data.substr(0, length));
                data.remove_prefix(length);
            }
        }
    }
};

template <typename F> void each_line(Text rows, F action) {
    while (!rows.empty()) {
        const auto end = rows.find('\n');
        action(rows.substr(0, end));
        if (end == Text::npos) break;
        rows.remove_prefix(end + 1);
    }
}

inline bool unquote(Text& field) {
    if (!field.empty() && field.front() == '"') {
        if (field.size() < 2 || field.back() != '"') return false;
        field = field.substr(1, field.size() - 2);
    }
    return field.find('"') == Text::npos;
}

template <std::size_t N> bool fields(Text line, std::array<Text, N>& out) {
    for (std::size_t i = 0; i < N; ++i) {
        auto comma = line.find(',');
        if ((comma == Text::npos) != (i + 1 == N)) return false;
        out[i] = line.substr(0, comma);
        if (!unquote(out[i])) return false;
        if (comma != Text::npos) line.remove_prefix(comma + 1);
    }
    return true;
}

// Common-width rows use constant-size field readers. The generic callback
// handles every other width, including long records and the scalar scanner.
template <std::size_t Columns> struct FixedWidthRowTag {};
template <std::size_t Columns, class General, class Direct>
struct FixedWidthRows : FixedWidthRowTag<Columns> {
    General &general;
    Direct &common;
    FixedWidthRows(General &general, Direct &common) : general(general), common(common) {}
    void operator()(const std::array<Text, Columns> &row) {
        general(row);
    }
};
#if defined(__SSE2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if defined(__SSE2__) || defined(__ARM_NEON)
struct Separators { std::uint64_t endings, quotes, newlines; };
#if defined(__ARM_NEON)
inline std::uint16_t bitmask(uint8x16_t lanes) {
    static const uint8x16_t weights = {1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128};
    const auto bits = vandq_u8(lanes, weights);
    return vaddv_u8(vget_low_u8(bits)) | (std::uint16_t(vaddv_u8(vget_high_u8(bits))) << 8);
}
#endif
inline Separators scan64(const char* bytes) {
    Separators result{};
    for (int offset = 0; offset < 64; offset += 16) {
#if defined(__SSE2__)
        auto x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + offset));
        int endings = _mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(x, _mm_set1_epi8(',')),
                                                      _mm_cmpeq_epi8(x, _mm_set1_epi8('\n'))));
        int newlines = _mm_movemask_epi8(_mm_cmpeq_epi8(x, _mm_set1_epi8('\n')));
        int quotes = _mm_movemask_epi8(_mm_cmpeq_epi8(x, _mm_set1_epi8('"')));
#else
        auto x = vld1q_u8(reinterpret_cast<const std::uint8_t*>(bytes + offset));
        int endings = bitmask(vorrq_u8(vceqq_u8(x, vdupq_n_u8(',')), vceqq_u8(x, vdupq_n_u8('\n'))));
        int newlines = bitmask(vceqq_u8(x, vdupq_n_u8('\n')));
        int quotes = bitmask(vceqq_u8(x, vdupq_n_u8('"')));
#endif
        result.newlines |= std::uint64_t(newlines) << offset;
        result.endings |= std::uint64_t(endings) << offset;
        result.quotes |= std::uint64_t(quotes) << offset;
    }
    return result;
}
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2"))) inline Separators scan64_avx2(const char* bytes) {
    Separators result{};
    for (int offset = 0; offset < 64; offset += 32) {
        auto x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes + offset));
        auto commas = _mm256_cmpeq_epi8(x, _mm256_set1_epi8(','));
        auto newlines = _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\n'));
        auto quotes = _mm256_cmpeq_epi8(x, _mm256_set1_epi8('"'));
        auto ends = static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_or_si256(commas, newlines)));
        auto quote_bits = static_cast<std::uint32_t>(_mm256_movemask_epi8(quotes));
        result.newlines |= std::uint64_t(static_cast<std::uint32_t>(_mm256_movemask_epi8(newlines))) << offset;
        result.endings |= std::uint64_t(ends) << offset;
        result.quotes |= std::uint64_t(quote_bits) << offset;
    }
    return result;
}
#endif
#endif

// Scan an entire chunk with one instruction set so the separator finder can
// inline into the loop. Callers dispatch once per chunk.
#if defined(__SSE2__) || defined(__ARM_NEON)
template <std::size_t N, auto scan_block, typename F>
__attribute__((always_inline)) inline void vector_records(Text bytes, F& consume) {
    std::array<Text, N> row;
    auto first = bytes.data();
    std::size_t column = 0;
    bool valid = true;
    auto field = [&](const char* last, bool newline) {
        if (column < N) {
            const auto size = last - first;
            const bool opening = size && *first == '"';
            const bool closing = size && last[-1] == '"';
            if (opening != closing || (opening && size < 2)) valid = false;
            else row[column] = Text(first + opening, size - 2 * opening);
        }
        ++column;
        if (newline) {
            if (valid && column == N) consume(row);
            valid = true;
            column = 0;
        }
    };
    const char* block = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining) {
        std::size_t n = std::min<std::size_t>(64, remaining);
        char tail[64] = {};
        const char* scan = block;
        if (n < 64) { std::memcpy(tail, block, n); scan = tail; }
        auto marks = scan_block(scan);
        auto boundary = [](char c) { return c == ',' || c == '\n'; };
        const bool prior_end = block == bytes.data() || boundary(block[-1]);
        const bool next_end = n == remaining || boundary(block[n]);
        // Quotes can only occur at a field edge. Carry both separators across
        // block boundaries and reject interior quotes in their own row.
        const auto beside = (marks.endings << 1) | std::uint64_t(prior_end)
                          | (marks.endings >> 1) | (std::uint64_t(next_end) << (n - 1));
        auto interior = marks.quotes & ~beside;
        while (marks.endings) {
            const auto bit = __builtin_ctzll(marks.endings);
            const auto* end = block + bit;
            if (interior) {
                const auto before = (marks.endings & -marks.endings) - 1;
                if (interior & before) valid = false;
                interior &= ~before;
            }
            field(end, *end == '\n');
            first = end + 1;
            marks.endings &= marks.endings - 1;
        }
        if (interior) valid = false;
        block += n;
        remaining -= n;
    }
    if (first != block || column) field(block, true);
}
// Validate each row's delimiter and quote masks before reusing its field offsets.
template <std::size_t N, auto scan_block, typename F>
__attribute__((always_inline)) inline void cached_records(Text bytes, F &emit) {
    // Keep complete records together. A row beyond 128 bytes uses the general scanner.
    constexpr unsigned batch_size = 4096, cache_size = N == 7 ? 8192 : N == 4 ? 1024 : 128;
    struct Entry {
        std::uint64_t ends_low, ends_high, quotes_low, quotes_high;
        std::uint8_t begin[N], length[N];
        bool valid_layout, fixed_width;
    };
    // Each scanner instantiation has its own thread-local cache and callback geometry.
    static thread_local std::array<Entry, cache_size> cache{};
    std::array<Text, N> fields;
    while (!bytes.empty()) {
        auto newline_at = bytes.rfind('\n', std::min<std::size_t>(batch_size, bytes.size()) - 1);
        if (newline_at == Text::npos) {
            newline_at = bytes.find('\n');
            auto n = newline_at == Text::npos ? bytes.size() : newline_at + 1;
            vector_records<N, scan_block>(bytes.substr(0, n), emit);
            bytes.remove_prefix(n);
            continue;
        }
        auto part = bytes.substr(0, newline_at + 1);
        bytes.remove_prefix(part.size());
        std::array<Separators, batch_size / 64 + 3> blocks;
        unsigned count = 0;
        for (unsigned i = 0; i < part.size(); i += 64) {
            if (part.size() - i >= 64)
                blocks[count++] = scan_block(part.data() + i);
            else {
                char tail[64]{};
                std::memcpy(tail, part.data() + i, part.size() - i);
                blocks[count++] = scan_block(tail);
            }
        }
        blocks[count] = blocks[count + 1] = blocks[count + 2] = Separators{};
        unsigned begin = 0;
        auto low_bits = [](unsigned n) { return n >= 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1; };
        auto rotate = [](std::uint64_t x, unsigned k) { return (x << k) | (x >> (64 - k)); };
        while (begin < part.size()) {
            const auto *s = blocks.data() + begin / 64;
            auto shift = begin & 63;
            auto join = [&](std::uint64_t l, std::uint64_t hash) {
                return (l >> shift) | (shift ? (hash << (64 - shift)) : 0);
            };
            auto newline = join(s[0].newlines, s[1].newlines);
            unsigned row_size = 0;
            if (newline)
                row_size = 1 + __builtin_ctzll(newline);
            else {
                newline = join(s[1].newlines, s[2].newlines);
                if (newline)
                    row_size = 65 + __builtin_ctzll(newline);
            }
            if (!row_size) {
                auto end = part.find('\n', begin + 128);
                row_size = static_cast<unsigned>(end + 1 - begin);
                vector_records<N, scan_block>(part.substr(begin, row_size), emit);
                begin += row_size;
                continue;
            }
            const auto ends_low = join(s[0].endings, s[1].endings) & low_bits(row_size);
            const auto quotes_low = join(s[0].quotes, s[1].quotes) & low_bits(row_size);
            auto ends_high = UINT64_C(0), quotes_high = UINT64_C(0);
            if (row_size > 64) {
                auto mask = low_bits(row_size - 64);
                ends_high = join(s[1].endings, s[2].endings) & mask;
                quotes_high = join(s[1].quotes, s[2].quotes) & mask;
            }
            auto hash = (ends_low ^ rotate(quotes_low, 17)) * UINT64_C(0x9e3779b97f4a7c15) +
                        (ends_high ^ rotate(quotes_high, 27)) * UINT64_C(0xbf58476d1ce4e5b9);
            auto slot = N == 4 ? (hash >> (64 - __builtin_ctz(cache_size)))
                               : ((hash ^ (hash >> 29)) & (cache_size - 1));
            auto &entry = cache[slot];
            auto row = part.data() + begin;
            begin += row_size;
            // Check all four masks, including the newline. A hash collision only replaces an entry.
            if (entry.ends_low != ends_low || entry.ends_high != ends_high ||
                entry.quotes_low != quotes_low || entry.quotes_high != quotes_high) {
                entry.ends_low = ends_low;
                entry.ends_high = ends_high;
                entry.quotes_low = quotes_low;
                entry.quotes_high = quotes_high;
                entry.valid_layout = false;
                if (__builtin_popcountll(ends_low) + __builtin_popcountll(ends_high) != N)
                    continue;
                // Reconstruct exactly two quotes around each quoted field. Equality with
                // the input masks also rejects stray quotes inside unquoted fields.
                auto a = ends_low, b = ends_high;
                unsigned first = 0;
                std::uint64_t expected_quotes[2]{};
                bool valid = true;
                for (unsigned j = 0; j < N; j++) {
                    unsigned end;
                    if (a) {
                        end = __builtin_ctzll(a);
                        a &= a - 1;
                    } else {
                        end = 64 + __builtin_ctzll(b);
                        b &= b - 1;
                    }
                    bool quote = row[first] == '"';
                    if (quote) {
                        if (end - first < 2) {
                            valid = false;
                            break;
                        }
                        expected_quotes[first >> 6] |= UINT64_C(1) << (first & 63);
                        expected_quotes[(end - 1) >> 6] |= UINT64_C(1) << ((end - 1) & 63);
                    }
                    entry.begin[j] = first + quote;
                    entry.length[j] = end - first - 2 * quote;
                    first = end + 1;
                }
                entry.valid_layout =
                    valid && expected_quotes[0] == quotes_low && expected_quotes[1] == quotes_high;
                if constexpr (N == 4 && std::is_base_of_v<FixedWidthRowTag<4>, F>)
                    entry.fixed_width =
                        entry.valid_layout && (entry.length[0] == 8 || entry.length[0] == 9) &&
                        (entry.length[1] == 8 || entry.length[1] == 9) &&
                        (entry.length[3] == 13 || entry.length[3] == 20 || entry.length[3] == 25);
                if constexpr (N == 7 && std::is_base_of_v<FixedWidthRowTag<7>, F>)
                    entry.fixed_width =
                        entry.valid_layout && entry.length[0] == 9 && entry.length[1] == 9 &&
                        entry.length[3] == 20 &&
                        (entry.length[2] == 20 || entry.length[2] == 25 || entry.length[2] == 13) &&
                        (entry.length[4] == 4 || entry.length[4] == 8);
            }
            if (entry.valid_layout) {
                if constexpr (std::is_base_of_v<FixedWidthRowTag<N>, F>)
                    if (entry.fixed_width) {
                        emit.common(row, entry);
                        continue;
                    }

#if defined(__clang__)
#pragma clang loop unroll(full)
#elif defined(__GNUC__)
#pragma GCC unroll 8
#endif
                for (unsigned j = 0; j < N; j++)
                    fields[j] = {row + entry.begin[j], entry.length[j]};
                emit(fields);
            }
        }
    }
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
template <std::size_t N, typename F>
__attribute__((target("avx2"))) void records_avx2(Text bytes, F& consume) {
    cached_records<N, scan64_avx2>(bytes, consume);
}
#endif
#endif

template <std::size_t N, typename F> void records(Text bytes, F consume) {
#if defined(__SSE2__) || defined(__ARM_NEON)
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (__builtin_cpu_supports("avx2")) {
        records_avx2<N>(bytes, consume);
        return;
    }
#endif
    cached_records<N, scan64>(bytes, consume);
#else
    each_line(bytes, [&](Text line) {
        std::array<Text, N> row;
        if (fields(line, row)) consume(row);
    });
#endif
}

// Combine adjacent digit pairs, then four-digit groups, in one word.
inline std::uint64_t eight_decimal_digits(std::uint64_t bytes) {
    bytes &= UINT64_C(0x0f0f0f0f0f0f0f0f);
    bytes = (bytes * 10 + (bytes >> 8)) & UINT64_C(0x00ff00ff00ff00ff);
    bytes = (bytes * 100 + (bytes >> 16)) & UINT64_C(0x0000ffff0000ffff);
    return (bytes & 65535) * 10000 + (bytes >> 32);
}
inline std::int64_t number(Text value, std::int64_t limit) {
    while (value.size() > 1 && value.front() == '0') value.remove_prefix(1);
    // Nineteen decimal digits always fit in uint64_t; range-check once after parsing.
    if (value.empty() || value.size() > 19) return -1;
    std::uint64_t n = 0;
    while (value.size() >= 8) {
        std::uint64_t digits;
        std::memcpy(&digits, value.data(), 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        digits = __builtin_bswap64(digits);
#endif
        if (((digits - UINT64_C(0x3030303030303030)) | (digits + UINT64_C(0x4646464646464646)))
            & UINT64_C(0x8080808080808080)) return -1;
        n = n * 100000000 + eight_decimal_digits(digits);
        value.remove_prefix(8);
    }
    for (char ch : value) {
        if (ch < '0' || ch > '9') return -1;
        n = n * 10 + (ch - '0');
    }
    return n <= static_cast<std::uint64_t>(limit) ? static_cast<std::int64_t>(n) : -1;
}

// Proleptic Gregorian civil date to Unix days, using March as month zero.
inline int unix_day(int y, int m, int d) {
    y -= m <= 2;
    int era = y / 400, year = y % 400;
    int day = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + year * 365 + year / 4 - year / 100 + day - 719468;
}

constexpr Millis invalid_time = INT64_MIN;

inline unsigned digit_pair(const char* p) {
    static const auto decoded = [] {
        std::array<unsigned char, 65536> table;
        table.fill(255);
        for (unsigned a = 0; a < 10; ++a) for (unsigned b = 0; b < 10; ++b)
            table[('0' + a) | (('0' + b) << 8)] = 10 * a + b;
        return table;
    }();
    return decoded[static_cast<unsigned char>(p[0]) | (static_cast<unsigned char>(p[1]) << 8)];
}

inline Millis midnight(const char* p) {
    struct Date { std::uint64_t prefix = 0; Millis millis = invalid_time; };
    static thread_local std::array<Date, 32> dates;
    const auto d = digit_pair(p + 8);
    if (d == 0 || d > 31) return invalid_time;
    std::uint64_t prefix;
    std::memcpy(&prefix, p, 8);
    auto& date = dates[d];
    if (date.prefix == prefix) return date.millis;
    if (p[4] != '-' || p[7] != '-') return invalid_time;
    const auto century = digit_pair(p), y = digit_pair(p + 2), m = digit_pair(p + 5);
    const auto year = century * 100 + y;
    if (century > 99 || y > 99 || year == 0 || m == 0 || m > 12) return invalid_time;
    constexpr unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const auto leap = m == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (d > days[m - 1] + leap) return invalid_time;
    date = {prefix, unix_day(year, m, d) * day_ms};
    return date.millis;
}

// Hours, minutes and seconds start at byte offsets 0, 3 and 6. Decode all
// three digit pairs in one word. Ingestion times are guaranteed to be F1.
template<bool validate> inline int daytime(const char* p) {
    std::uint64_t bytes;
    std::memcpy(&bytes, p, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    bytes = __builtin_bswap64(bytes);
#endif
    constexpr auto first_digits = UINT64_C(0x000f00000f00000f);
    const auto pairs = (bytes & first_digits) * 10 + ((bytes >> 8) & first_digits);
    const int hour = pairs & 255, minute = (pairs >> 24) & 255, second = pairs >> 48;
    if constexpr (validate) {
        constexpr auto digits = UINT64_C(0xffff00ffff00ffff), zero = UINT64_C(0x3030303030303030);
        const auto numbers = (bytes & digits) | (zero & ~digits);
        const auto nondigits = ((numbers - zero) | (numbers + UINT64_C(0x4646464646464646)))
                             & UINT64_C(0x8080808080808080);
        if (((bytes ^ UINT64_C(0x3a3a3a3a3a3a3a3a)) & ~digits) || nondigits ||
            hour > 23 || minute > 59 || second > 59) return -1;
    }
    return (hour * 60 + minute) * 60 + second;
}

inline Millis ingest_time(Text value) {
    if (value.size() != 20) return invalid_time;
    const auto* p = value.data();
    // ingest_ts is guaranteed to be a valid F1 timestamp.
    const auto seconds = daytime<false>(p + 11);
    return midnight(p) + seconds * 1000;
}

// ISO records have one of two exact widths. The F1 ingestion date may supply a
// midnight already computed for the same ten calendar bytes.
template <unsigned Width>
__attribute__((always_inline)) inline Millis iso_timestamp(const char *p, const char *arrival = nullptr,
                                                           Millis arrival_day = 0) {
    if (p[10] != 'T') return invalid_time;
    const int seconds = daytime<true>(p + 11);
    if (seconds < 0) return invalid_time;
    int offset = 0;
    if constexpr (Width == 20) {
        if (p[19] != 'Z') return invalid_time;
    } else {
        if ((p[19] != '+' && p[19] != '-') || p[22] != ':') return invalid_time;
        const auto h = digit_pair(p + 20), m = digit_pair(p + 23);
        if (h > 23 || m > 59) return invalid_time;
        offset = static_cast<int>(h * 60 + m) * (p[19] == '+' ? 1 : -1);
    }
    const auto day = arrival && std::memcmp(p, arrival, 10) == 0 ? arrival_day : midnight(p);
    if (day == invalid_time) return day;
    return day + (seconds - offset * 60) * 1000LL;
}

inline Millis timestamp(Text value) {
    if (value.size() < 5 || value[4] != '-') {
        auto n = number(value, INT64_MAX);
        return n < 0 ? invalid_time : n;
    }
    if (value.size() == 20)
        return iso_timestamp<20>(value.data());
    if (value.size() == 25)
        return iso_timestamp<25>(value.data());
    return invalid_time;
}

// Two overlapping eight-byte reads cover exactly thirteen decimal places.
inline Millis thirteen_decimal_digits(const char *p) {
    std::uint64_t first, last;
    std::memcpy(&first, p, 8);
    std::memcpy(&last, p + 5, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    first = __builtin_bswap64(first);
    last = __builtin_bswap64(last);
#endif
    constexpr std::uint64_t zeros = UINT64_C(0x3030303030303030), sixes = UINT64_C(0x4646464646464646),
                            high = UINT64_C(0x8080808080808080);
    if ((((first - zeros) | (first + sixes) | (last - zeros) | (last + sixes)) & high) != 0)
        return invalid_time;
    return eight_decimal_digits(first << 24) * UINT64_C(100000000) + eight_decimal_digits(last);
}
template <unsigned Width>
__attribute__((always_inline)) inline Millis timestamp_at_width(const char *p, const char *arrival = nullptr,
                                                                Millis arrival_day = 0) {
    if constexpr (Width == 13)
        return thirteen_decimal_digits(p);
    else {
        if (p[4] == '-')
            return iso_timestamp<Width>(p, arrival, arrival_day);
        // F3 timestamps allow leading zeroes, including at ISO-sized widths.
        auto numeric = number(Text(p, Width), INT64_MAX);
        return numeric < 0 ? invalid_time : numeric;
    }
}
