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
struct Separators { std::uint64_t endings, quotes; };
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
        int quotes = _mm_movemask_epi8(_mm_cmpeq_epi8(x, _mm_set1_epi8('"')));
#else
        auto x = vld1q_u8(reinterpret_cast<const std::uint8_t*>(bytes + offset));
        int endings = bitmask(vorrq_u8(vceqq_u8(x, vdupq_n_u8(',')), vceqq_u8(x, vdupq_n_u8('\n'))));
        int quotes = bitmask(vceqq_u8(x, vdupq_n_u8('"')));
#endif
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
        result.endings |= std::uint64_t(ends) << offset;
        result.quotes |= std::uint64_t(quote_bits) << offset;
    }
    return result;
}
#endif

inline unsigned quote_count(std::uint64_t bits) {
#if defined(__x86_64__) && !defined(__POPCNT__)
    // Generic x86 can otherwise enter a software population-count routine once
    // per field. The CSV grammar distinguishes zero, one, two, and three or more.
    if (!bits) return 0;
    const auto rest = bits & (bits - 1);
    return 1 + (rest != 0) + ((rest & (rest - 1)) != 0);
#else
    return __builtin_popcountll(bits);
#endif
}

#endif

// Fields can be quoted, but the input contract forbids embedded delimiters.
// One vector pass locates field boundaries and validates quote placement.
template <std::size_t N, typename F> void records(Text bytes, F consume) {
#if defined(__SSE2__) || defined(__ARM_NEON)
    std::array<Text, N> row;
    auto first = bytes.data();
    std::size_t column = 0, quotes = 0;
    bool valid = true;
    auto field = [&](const char* last, bool newline) {
        if (column < N) {
            Text value(first, last - first);
            if (quotes) {
                if (quotes == 2 && value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                else valid = false;
            }
            row[column] = value;
        }
        ++column;
        if (newline) {
            if (valid && column == N) consume(row);
            valid = true;
            column = 0;
        }
        quotes = 0;
    };
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    const auto scan_block = __builtin_cpu_supports("avx2") ? &scan64_avx2 : &scan64;
#else
    constexpr auto scan_block = &scan64;
#endif
    const char* block = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining) {
        std::size_t n = std::min<std::size_t>(64, remaining);
        char tail[64] = {};
        const char* scan = block;
        if (n < 64) { std::memcpy(tail, block, n); scan = tail; }
        auto marks = scan_block(scan);
        while (marks.endings) {
            const unsigned bit = __builtin_ctzll(marks.endings);
            const auto before = (marks.endings & -marks.endings) - 1;
            quotes += quote_count(marks.quotes & before);
            marks.quotes &= ~before;
            const auto* end = block + bit;
            field(end, *end == '\n');
            first = end + 1;
            marks.endings &= marks.endings - 1;
        }
        quotes += quote_count(marks.quotes);
        block += n;
        remaining -= n;
    }
    if (first != block || column) field(block, true);
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

inline Millis ingest_time(Text value) {
    if (value.size() != 20) return invalid_time;
    const auto* p = value.data();
    // ingest_ts is guaranteed to be a valid F1 timestamp.
    const auto seconds = (digit_pair(p + 11) * 60 + digit_pair(p + 14)) * 60 + digit_pair(p + 17);
    return midnight(p) + seconds * 1000;
}

inline Millis timestamp(Text value) {
    if (value.size() < 5 || value[4] != '-') {
        auto n = number(value, INT64_MAX);
        return n < 0 ? invalid_time : n;
    }
    if (value.size() != 20 && value.size() != 25) return invalid_time;
    const auto* p = value.data();
    if (p[10] != 'T' || p[13] != ':' || p[16] != ':') return invalid_time;
    const auto hour = digit_pair(p + 11), minute = digit_pair(p + 14), sec = digit_pair(p + 17);
    if (hour > 23 || minute > 59 || sec > 59) return invalid_time;
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
    return day + ((static_cast<int>(hour * 60 + minute) - offset) * 60 + static_cast<int>(sec)) * 1000LL;
}
