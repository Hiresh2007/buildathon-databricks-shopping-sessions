#pragma once

#include "input.hpp"
#include <cstring>
#include <deque>
#include <functional>

// Eight ASCII bytes occupy the low seven bits of each byte in a word. For
// nine-byte names, the leading character occupies the seven spare high bits.
// A zero prefix denotes an eight-byte name; nine-byte names beginning with NUL
// and all other unrepresentable names retain a view of their original bytes.
struct Name {
    static constexpr std::uint64_t external = UINT64_C(1) << 63;
    static constexpr std::uint64_t cached_flag = 1;
    static constexpr std::size_t cached_length = 33;
    struct CachedName { const char* data; std::size_t hash; };
    static_assert(alignof(CachedName) >= 2 && alignof(Text) >= 2);
    std::uint64_t value = external;

    bool empty() const { return value == external; }
    const CachedName& cached_name() const {
        return *reinterpret_cast<const CachedName*>(value & ~(external | cached_flag));
    }
    Text reference() const {
        if (value & cached_flag) return {cached_name().data, cached_length};
        return *reinterpret_cast<const Text*>(value & ~external);
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
            return value & cached_flag ? cached_name().hash : std::hash<Text>{}(reference());
        const auto mixed = value * UINT64_C(0x9e3779b97f4a7c15);
        return mixed ^ (mixed >> 32);
    }
};

struct NameHash { std::size_t operator()(Name name) const { return name.hash(); } };

struct NamePool {
    std::deque<Text> external_names;
    std::deque<Name::CachedName> cached_names;
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
        // At this frequent width the length fits in the representation tag. Keep
        // the full hash in the space normally used by the string view's length.
        if (text.size() == Name::cached_length) {
            cached_names.push_back({text.data(), std::hash<Text>{}(text)});
            return {Name::external | Name::cached_flag | reinterpret_cast<std::uintptr_t>(&cached_names.back())};
        }
        external_names.push_back(text);
        return {Name::external | reinterpret_cast<std::uintptr_t>(&external_names.back())};
    }
};
