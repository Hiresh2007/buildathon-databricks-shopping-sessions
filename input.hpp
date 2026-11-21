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
#include <emmintrin.h>
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
    const char* block = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining) {
        std::size_t n = std::min<std::size_t>(64, remaining);
        char tail[64] = {};
        const char* scan = block;
        if (n < 64) { std::memcpy(tail, block, n); scan = tail; }
        auto marks = scan64(scan);
        while (marks.endings) {
            const unsigned bit = __builtin_ctzll(marks.endings);
            const auto before = (marks.endings & -marks.endings) - 1;
            quotes += __builtin_popcountll(marks.quotes & before);
            marks.quotes &= ~before;
            const auto* end = block + bit;
            field(end, *end == '\n');
            first = end + 1;
            marks.endings &= marks.endings - 1;
        }
        quotes += __builtin_popcountll(marks.quotes);
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

inline int two_digits(Text value, int i) {
    return (value[i] - '0') * 10 + value[i + 1] - '0';
}

// The input contract guarantees a valid, fixed-width UTC timestamp in ingest_ts.
inline Millis ingest_time(Text value) {
    if (value.size() != 20) return invalid_time;
    int year = two_digits(value, 0) * 100 + two_digits(value, 2);
    int day = unix_day(year, two_digits(value, 5), two_digits(value, 8));
    int second = (two_digits(value, 11) * 60 + two_digits(value, 14)) * 60 + two_digits(value, 17);
    return day * day_ms + second * 1000;
}

inline Millis timestamp(Text value) {
    if (value.size() < 5 || value[4] != '-') {
        auto n = number(value, INT64_MAX);
        return n < 0 ? invalid_time : n;
    }
    if (value.size() != 20 && value.size() != 25) return invalid_time;
    if (value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':') return invalid_time;
    for (auto pos : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18})
        if (value[pos] < '0' || value[pos] > '9') return invalid_time;
    auto pair = [&](int i) { return two_digits(value, i); };
    int year = pair(0) * 100 + pair(2), month = pair(5), day = pair(8);
    int hour = pair(11), min = pair(14), sec = pair(17), offset = 0;
    constexpr std::array<int, 12> days = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year == 0 || month < 1 || month > 12) return invalid_time;
    int leap = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day < 1 || day > days[month - 1] + leap || hour > 23 || min > 59 || sec > 59) return invalid_time;
    if (value.size() == 20) {
        if (value[19] != 'Z') return invalid_time;
    } else {
        if ((value[19] != '+' && value[19] != '-') || value[22] != ':') return invalid_time;
        for (auto pos : {20, 21, 23, 24}) if (value[pos] < '0' || value[pos] > '9') return invalid_time;
        if (pair(20) > 23 || pair(23) > 59) return invalid_time;
        offset = (pair(20) * 60 + pair(23)) * (value[19] == '+' ? 1 : -1);
    }
    return unix_day(year, month, day) * day_ms + ((hour * 60 + min - offset) * 60 + sec) * 1000LL;
}
