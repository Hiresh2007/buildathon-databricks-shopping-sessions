#pragma once

#include "input.hpp"
#include <cstring>
#include <functional>
#include <memory>

// The hash accompanies the original source view for variable-width IDs. Equality always checks the bytes.
struct StoredName {
    Text text;
    std::size_t hash;
};

// Eight ASCII bytes occupy the low seven bits of each byte in a word. For
// nine-byte names, the leading character occupies the seven spare high bits.
// A zero prefix denotes an eight-byte name; nine-byte names beginning with NUL
// and all other unrepresentable names retain a view of their original bytes.

struct Name {
    static constexpr std::uint64_t external = UINT64_C(1) << 63;
    std::uint64_t value = external;

    bool empty() const { return value == external; }
    const StoredName &stored() const {
        return *reinterpret_cast<const StoredName *>(value & ~external);
    }
    Text reference() const {
        return stored().text;
    }
    bool operator==(Name b) const {
        if (value == b.value) return true;
        return (value & b.value & external) && !empty() && !b.empty() && reference() == b.reference();
    }
    bool operator!=(Name b) const { return !(*this == b); }
    static constexpr std::uint64_t ascii = UINT64_C(0x7f7f7f7f7f7f7f7f);
    // Gather the seven high bits into the low end of the result.
    static unsigned leading(std::uint64_t bits) {
        return ((bits & ~ascii) * UINT64_C(0x0002040810204081)) >> 56;
    }
    Text view(std::array<char, 9>& storage) const {
        if (value & external) return reference();
        const auto first = leading(value);
        auto body = value & ascii;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        body = __builtin_bswap64(body);
#endif
        storage[0] = static_cast<char>(first);
        std::memcpy(storage.data() + 1, &body, 8);
        return first ? Text(storage.data(), 9) : Text(storage.data() + 1, 8);
    }
    bool operator>(Name other) const {
        if (value == other.value) return false;
        if (!((value | other.value) & external) && !((value ^ other.value) & ~ascii))
            return __builtin_bswap64(value & ascii) > __builtin_bswap64(other.value & ascii);
        std::array<char, 9> a, b;
        return view(a) > other.view(b);
    }
    std::size_t hash() const {
        if (value & external)
            return stored().hash;
        const auto mixed = value * UINT64_C(0x9e3779b97f4a7c15);
        return mixed ^ (mixed >> 32);
    }
};

struct NameHash { std::size_t operator()(Name name) const { return name.hash(); } };

struct NamePool {
    // A Name can outlive this pool's original owner after the aliases have been joined.
    // Moving a block handle preserves all pointers into the block.
    std::vector<std::unique_ptr<StoredName[]>> blocks;
    StoredName *next = nullptr;
    StoredName *limit = nullptr;
    __attribute__((noinline)) void add_block() {
        constexpr unsigned capacity = 512;
        blocks.push_back(std::make_unique<StoredName[]>(capacity));
        next = blocks.back().get();
        limit = next + capacity;
    }
    Name store_external(Text text) {
        if (next == limit)
            add_block();
        auto *value = next++;
        *value = {text, std::hash<Text>{}(text)};
        return {Name::external | reinterpret_cast<std::uintptr_t>(value)};
    }
    Name get(Text text) {
        if (text.size() == 9 || text.size() == 8) {
            static constexpr auto prefixes = [] {
                std::array<std::uint64_t, 128> table{};
                for (unsigned c = 1; c < table.size(); ++c)
                    for (unsigned bit = 0; bit < 7; ++bit)
                        table[c] |= std::uint64_t((c >> bit) & 1) << (8 * bit + 7);
                return table;
            }();
            const bool has_prefix = text.size() == 9;
            const auto first = has_prefix ? static_cast<unsigned char>(text[0]) : 0;
            std::uint64_t bytes;
            std::memcpy(&bytes, text.data() + has_prefix, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            bytes = __builtin_bswap64(bytes);
#endif
            if (!(bytes & ~Name::ascii) && first < 128 && (first || !has_prefix))
                return {bytes | prefixes[first]};
        }
        return store_external(text);
    }
};
