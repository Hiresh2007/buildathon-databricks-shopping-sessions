#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
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
__attribute__((target("avx512f,avx512bw"))) inline Separators scan64_avx512bw(const char* bytes) {
    const auto input = _mm512_loadu_si512(reinterpret_cast<const void*>(bytes));
    const std::uint64_t commas = _mm512_cmpeq_epi8_mask(input, _mm512_set1_epi8(','));
    const std::uint64_t newlines = _mm512_cmpeq_epi8_mask(input, _mm512_set1_epi8('\n'));
    const std::uint64_t quotes = _mm512_cmpeq_epi8_mask(input, _mm512_set1_epi8('"'));
    return {commas | newlines, quotes, newlines};
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
// Verify the CSV structure for a short run of complete lines before decoding
// it. Each structural index stores a field end and whether its quotes must be
// stripped. Newlines must occur at every Nth field, and every separator must
// fall outside a quoted field. Unusual or malformed runs use the row scanner.
template <std::size_t N, auto scan_block, typename F>
__attribute__((always_inline)) inline void batched_records(Text bytes, F& consume) {
    constexpr std::size_t max_bytes = 1024;
    constexpr unsigned quoted = 0x8000;
    std::array<std::uint16_t, max_bytes> field_ends;
    std::array<Text, N> fields;
    auto is_separator = [](char c) { return c == ',' || c == '\n'; };
    while (!bytes.empty()) {
        auto end = bytes.rfind('\n', std::min(bytes.size(), max_bytes) - 1);
        if (end == Text::npos) {
            end = bytes.find('\n');
            const auto length = end == Text::npos ? bytes.size() : end + 1;
            vector_records<N, scan_block>(bytes.substr(0, length), consume);
            bytes.remove_prefix(length);
            continue;
        }
        const auto batch = bytes.substr(0, end + 1);
        bytes.remove_prefix(batch.size());
        unsigned field_count = 0, row_columns = 0;
        bool malformed = false, inside_quotes = false;
        for (std::size_t offset = 0; offset < batch.size(); offset += 64) {
            const auto length = std::min<std::size_t>(64, batch.size() - offset);
            const char* source = batch.data() + offset;
            char tail[64]{};
            const char* scan = source;
            if (length < 64) {
                std::memcpy(tail, source, length);
                scan = tail;
            }
            auto marks = scan_block(scan);
            const bool left = !offset || is_separator(source[-1]);
            const bool right = offset + length == batch.size() || is_separator(source[length]);
            const auto edges = (marks.endings << 1) | left | (marks.endings >> 1)
                             | (std::uint64_t(right) << (length - 1));
            auto parity = marks.quotes;
            parity ^= parity << 1;
            parity ^= parity << 2;
            parity ^= parity << 4;
            parity ^= parity << 8;
            parity ^= parity << 16;
            parity ^= parity << 32;
            if (inside_quotes) parity = ~parity;
            inside_quotes = (parity >> (length - 1)) & 1;
            if ((marks.quotes & ~edges) || (parity & marks.endings)) {
                malformed = true;
                break;
            }
            auto remaining = marks.endings;
            for (auto lines = marks.newlines; lines; lines &= lines - 1) {
                const auto boundary = lines & -lines;
                const auto through = boundary | (boundary - 1);
                row_columns += __builtin_popcountll(remaining & through);
                if (row_columns != N) {
                    malformed = true;
                    break;
                }
                remaining &= ~through;
                row_columns = 0;
            }
            if (malformed) break;
            row_columns += __builtin_popcountll(remaining);
            const auto closing = (marks.quotes << 1) | std::uint64_t(offset && source[-1] == '"');
            for (auto separators = marks.endings; separators; separators &= separators - 1) {
                const unsigned bit = __builtin_ctzll(separators);
                field_ends[field_count++] = std::uint16_t((offset + bit) | (((closing >> bit) & 1) * quoted));
            }
        }
        if (malformed) {
            vector_records<N, scan_block>(batch, consume);
            continue;
        }
        unsigned begin = 0;
        for (unsigned i = 0; i < field_count; i += N) {
#if defined(__clang__)
#pragma clang loop unroll(full)
#elif defined(__GNUC__)
#pragma GCC unroll 8
#endif
            for (unsigned j = 0; j < N; ++j) {
                const unsigned encoded = field_ends[i + j];
                const unsigned end = encoded & (quoted - 1), trim = encoded >> 15;
                fields[j] = Text(batch.data() + begin + trim, end - begin - 2 * trim);
                begin = end + 1;
            }
            consume(fields);
        }
    }
}
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
template <std::size_t N, typename F>
__attribute__((target("avx2"))) void records_avx2(Text bytes, F& consume) {
    batched_records<N, scan64_avx2>(bytes, consume);
}
template <std::size_t N, typename F>
__attribute__((target("avx512f,avx512bw,popcnt"))) void records_avx512bw(Text bytes, F& consume) {
    batched_records<N, scan64_avx512bw>(bytes, consume);
}
#endif
#endif

template <std::size_t N, typename F> void records(Text bytes, F consume) {
#if defined(__SSE2__) || defined(__ARM_NEON)
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("popcnt")) {
        records_avx512bw<N>(bytes, consume);
        return;
    }
    if (__builtin_cpu_supports("avx2")) {
        records_avx2<N>(bytes, consume);
        return;
    }
#endif
    batched_records<N, scan64>(bytes, consume);
#else
    each_line(bytes, [&](Text line) {
        std::array<Text, N> row;
        if (fields(line, row)) consume(row);
    });
#endif
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
        digits &= UINT64_C(0x0f0f0f0f0f0f0f0f);
        digits = (digits * 10 + (digits >> 8)) & UINT64_C(0x00ff00ff00ff00ff);
        digits = (digits * 100 + (digits >> 16)) & UINT64_C(0x0000ffff0000ffff);
        n = n * 100000000 + (digits & 65535) * 10000 + (digits >> 32);
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

inline Millis timestamp(Text value) {
    if (value.size() < 5 || value[4] != '-') {
        auto n = number(value, INT64_MAX);
        return n < 0 ? invalid_time : n;
    }
    if (value.size() != 20 && value.size() != 25) return invalid_time;
    const auto* p = value.data();
    if (p[10] != 'T') return invalid_time;
    const int seconds = daytime<true>(p + 11);
    if (seconds < 0) return invalid_time;
    int offset = 0;
    if (value.size() == 20) {
        if (p[19] != 'Z') return invalid_time;
    } else {
        if ((p[19] != '+' && p[19] != '-') || p[22] != ':') return invalid_time;
        const auto h = digit_pair(p + 20), m = digit_pair(p + 23);
        if (h > 23 || m > 59) return invalid_time;
        offset = static_cast<int>(h * 60 + m) * (p[19] == '+' ? 1 : -1);
    }
    const auto day = midnight(p);
    if (day == invalid_time) return day;
    return day + (seconds - offset * 60) * 1000LL;
}
