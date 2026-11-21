#include <memory>
#pragma once
#include "identity.hpp"
#include <fstream>
#include <limits>
#include <type_traits>

using Bytes = std::vector<std::uint8_t>;
inline void varint(Bytes &dst, std::uint64_t n) {
    while (n >= 128) {
        dst.push_back(std::uint8_t(n) | 128);
        n >>= 7;
    }
    dst.push_back(std::uint8_t(n));
}
inline void raw32(Bytes &dst, std::uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        dst.push_back(std::uint8_t(n >> (8 * i)));
}
inline void byte_string(Bytes &dst, Text text) {
    if (text.size() > INT32_MAX)
        throw std::length_error("Parquet byte array exceeds 2 GiB");
    raw32(dst, std::uint32_t(text.size()));
    dst.insert(dst.end(), text.begin(), text.end());
}
// Compact Thrift encodes consecutive field IDs in the high nibble of each header.
struct Compact {
    Bytes bytes;
    int previous = 0;
    void field(unsigned id, unsigned type) {
        auto delta = id - previous;
        if (!delta || delta > 15)
            throw std::logic_error("invalid Parquet metadata field order");
        bytes.push_back((delta << 4) | type);
        previous = id;
    }
    void integer(std::int64_t v) {
        varint(bytes, (std::uint64_t(v) << 1) ^ std::uint64_t(-(v < 0)));
    }
    void i32(unsigned id, std::int32_t n) {
        field(id, 5);
        integer(n);
    }
    void i64(unsigned id, std::int64_t n) {
        field(id, 6);
        integer(n);
    }
    void string(Text s) {
        varint(bytes, s.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
    void binary(unsigned id, Text s) {
        field(id, 8);
        string(s);
    }
    void list(unsigned id, std::size_t size, unsigned type) {
        field(id, 9);
        bytes.push_back((std::min<std::size_t>(15, size) << 4) | type);
        if (size >= 15)
            varint(bytes, size);
    }
    template <class F> void record(F fill) {
        auto prior = previous;
        previous = 0;
        fill();
        bytes.push_back(0);
        previous = prior;
    }
};
struct FieldSpec {
    Text name;
    int type, converted = -1;
    bool optional = false;
};
struct BufferedFile {
    static constexpr std::size_t capacity = 1u << 20;
    int descriptor = -1;
    std::unique_ptr<char[]> bytes{new char[capacity]};
    std::size_t used = 0;
    explicit BufferedFile(const fs::path &path)
        : descriptor(::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644)) {
        if (descriptor < 0)
            throw std::runtime_error("cannot open Parquet output");
    }
    BufferedFile(const BufferedFile &) = delete;
    BufferedFile(BufferedFile &&other) noexcept
        : descriptor(other.descriptor), bytes(std::move(other.bytes)), used(other.used) {
        other.descriptor = -1;
    }
    ~BufferedFile() {
        if (descriptor >= 0)
            ::close(descriptor);
    }
    void direct(const char *p, std::size_t n) {
        while (n) {
            auto result = ::write(descriptor, p, n);
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                throw std::runtime_error("cannot write Parquet output");
            p += result;
            n -= result;
        }
    }
    void flush() {
        if (used) {
            direct(bytes.get(), used);
            used = 0;
        }
    }
    void write(const char *p, std::size_t n) {
        if (n > capacity - used)
            flush();
        if (n >= capacity) {
            direct(p, n);
            return;
        }
        std::memcpy(bytes.get() + used, p, n);
        used += n;
    }
    void close() {
        flush();
        auto result = ::close(descriptor);
        descriptor = -1;
        if (result < 0)
            throw std::runtime_error("cannot close Parquet output");
    }
};
struct FlatParquet {
    struct Chunk {
        std::int64_t offset, bytes;
    };
    struct Group {
        std::int64_t rows;
        std::vector<Chunk> chunks;
    };
    BufferedFile file;
    std::vector<FieldSpec> schema;
    std::vector<Group> groups;
    std::int64_t at = 0;
    FlatParquet(const fs::path &path, std::vector<FieldSpec> fields) : file(path), schema(std::move(fields)) {
        write("PAR1", 4);
    }
    void write(const void *data, std::size_t size) {
        if (size)
            file.write(static_cast<const char *>(data), size);
        at += size;
    }
    void begin(std::size_t rows) {
        groups.push_back({std::int64_t(rows), {}});
    }
    void page(const void *data, std::size_t size, const Bytes *levels = nullptr) {
        auto &g = groups.back();
        Bytes prefix;
        if (levels) {
            Bytes header;
            varint(header, (levels->size() << 1) | 1);
            raw32(prefix, std::uint32_t(header.size() + levels->size()));
            prefix.insert(prefix.end(), header.begin(), header.end());
        }
        auto bytes = size + prefix.size() + (levels ? levels->size() : 0);
        if (bytes > INT32_MAX || g.rows > INT32_MAX)
            throw std::length_error("Parquet page exceeds 2 GiB");
        Compact h;
        h.record([&] {
            h.i32(1, 0);
            h.i32(2, bytes);
            h.i32(3, bytes);
            h.field(5, 12);
            h.record([&] {
                h.i32(1, g.rows);
                h.i32(2, 0);
                h.i32(3, 3);
                h.i32(4, 3);
            });
        });
        auto offset = at;
        write(h.bytes.data(), h.bytes.size());
        write(prefix.data(), prefix.size());
        if (levels)
            write(levels->data(), levels->size());
        write(data, size);
        g.chunks.push_back({offset, at - offset});
    }
    template <class I> void ints(const std::vector<I> &values) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        page(values.data(), values.size() * sizeof(I));
#else
        Bytes encoded;
        for (auto v : values)
            for (unsigned k = 0; k < sizeof(I); k++)
                encoded.push_back(std::uint8_t(std::make_unsigned_t<I>(v) >> (8 * k)));
        page(encoded.data(), encoded.size());
#endif
    }
    void close() {
        Compact m;
        std::int64_t count = 0;
        for (auto &g : groups)
            count += g.rows;
        m.record([&] {
            m.i32(1, 1);
            m.list(2, schema.size() + 1, 12);
            m.record([&] {
                m.binary(4, "schema");
                m.i32(5, schema.size());
            });
            for (const auto &f : schema)
                m.record([&] {
                    m.i32(1, f.type);
                    m.i32(3, f.optional ? 1 : 0);
                    m.binary(4, f.name);
                    if (f.converted >= 0)
                        m.i32(6, f.converted);
                });
            m.i64(3, count);
            m.list(4, groups.size(), 12);
            for (const auto &g : groups)
                m.record([&] {
                    if (g.chunks.size() != schema.size())
                        throw std::logic_error("incomplete Parquet row group");
                    m.list(1, g.chunks.size(), 12);
                    std::int64_t total = 0;
                    for (std::size_t i = 0; i < g.chunks.size(); i++) {
                        const auto &c = g.chunks[i];
                        const auto &f = schema[i];
                        total += c.bytes;
                        m.record([&] {
                            m.i64(2, 0);
                            m.field(3, 12);
                            m.record([&] {
                                m.i32(1, f.type);
                                m.list(2, f.optional ? 2 : 1, 5);
                                m.integer(0);
                                if (f.optional)
                                    m.integer(3);
                                m.list(3, 1, 8);
                                m.string(f.name);
                                m.i32(4, 0);
                                m.i64(5, g.rows);
                                m.i64(6, c.bytes);
                                m.i64(7, c.bytes);
                                m.i64(9, c.offset);
                            });
                        });
                    }
                    m.i64(2, total);
                    m.i64(3, g.rows);
                });
        });
        if (m.bytes.size() > UINT32_MAX)
            throw std::length_error("oversize Parquet footer");
        write(m.bytes.data(), m.bytes.size());
        Bytes trailer;
        raw32(trailer, std::uint32_t(m.bytes.size()));
        write(trailer.data(), 4);
        write("PAR1", 4);
        file.close();
    }
};
struct BinaryColumn {
    std::unique_ptr<std::uint8_t[]> storage;
    std::size_t size = 0, capacity = 0;
    std::uint8_t *reserve_one(std::size_t n) {
        if (n > INT32_MAX)
            throw std::length_error("Parquet byte array exceeds 2 GiB");
        auto end = size + n + 4;
        if (end > capacity) {
            auto next = std::max(end, std::max<std::size_t>(524288, capacity * 2));
            auto block = std::unique_ptr<std::uint8_t[]>(new std::uint8_t[next]);
            if (size)
                std::memcpy(block.get(), storage.get(), size);
            storage = std::move(block);
            capacity = next;
        }
        auto ptr = storage.get() + size;
        size = end;
        std::uint32_t len = n;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        len = __builtin_bswap32(len);
#endif
        std::memcpy(ptr, &len, 4);
        return ptr + 4;
    }
    void append(Text s) {
        auto dest = reserve_one(s.size());
        std::memcpy(dest, s.data(), s.size());
    }
    void append(Name name) {
        if (name.value & Name::external) {
            append(name.reference());
            return;
        }
        unsigned first = Name::leading(name.value);
        auto body = name.value & Name::ascii;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        body = __builtin_bswap64(body);
#endif
        auto p = reserve_one(8 + bool(first));
        if (first)
            *p++ = first;
        std::memcpy(p, &body, 8);
    }
};
struct alignas(64) UserRows {
    static constexpr unsigned capacity = 32768;
    struct Columns {
        std::int32_t days[capacity], sessions[capacity];
        std::int64_t events[capacity], revenue[capacity];
    };
    FlatParquet file;
    BinaryColumn names, categories;
    std::unique_ptr<Columns> data{new Columns};
    Bytes present = Bytes(capacity / 8);
    unsigned count = 0;
    explicit UserRows(const fs::path &path)
        : file(path, {{"user_id", 6, 0},
                      {"day", 1, 6},
                      {"sessions", 1},
                      {"events", 2},
                      {"revenue_cents", 2},
                      {"top_category", 6, 0, true}}) {}
    void append(Name name, int day, int n, std::int64_t events, std::int64_t cents, const Text *category) {
        unsigned i = count++;
        names.append(name);
        data->days[i] = day;
        data->sessions[i] = n;
        data->events[i] = events;
        data->revenue[i] = cents;
        if (!(i & 7))
            present[i >> 3] = 0;
        if (category) {
            present[i >> 3] |= std::uint8_t(1u << (i & 7));
            categories.append(*category);
        }
        if (count == capacity || names.size >= (1u << 26) || categories.size >= (1u << 26))
            flush();
    }
    template <class I> void numbers(I *values) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        file.page(values, count * sizeof(I));
#else
        file.ints(std::vector<I>(values, values + count));
#endif
    }
    void flush() {
        if (!count)
            return;
        file.begin(count);
        file.page(names.storage.get(), names.size);
        numbers(data->days);
        numbers(data->sessions);
        numbers(data->events);
        numbers(data->revenue);
        present.resize((count + 7) / 8);
        file.page(categories.storage.get(), categories.size, &present);
        present.resize(capacity / 8);
        names.size = categories.size = count = 0;
    }
    void close() {
        flush();
        file.close();
    }
};
