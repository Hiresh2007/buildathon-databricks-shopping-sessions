#pragma once

#include "input.hpp"
#include <cstring>
#include <deque>
#include <functional>

// Nine ASCII characters fit losslessly in 63 bits. Other identifiers keep a view
// of their exact CSV bytes. The choice depends solely on the identifier itself.
struct Name {
    static constexpr std::uint64_t external = UINT64_C(1) << 63;
    std::uint64_t value = external;

    bool empty() const { return value == external; }
    const Text& reference() const { return *reinterpret_cast<const Text*>(value & ~external); }
    bool operator==(Name b) const {
        if (value == b.value) return true;
        return (value & b.value & external) && !empty() && !b.empty() && reference() == b.reference();
    }
    bool operator!=(Name b) const { return !(*this == b); }
    Text view(std::array<char, 9>& storage) const {
        if (value & external) return reference();
        for (int i = 0; i < 9; ++i) storage[i] = (value >> (7 * i)) & 127;
        return Text(storage.data(), storage.size());
    }
    std::size_t hash() const {
        if (value & external) return std::hash<Text>{}(reference());
        const auto mixed = value * UINT64_C(0x9e3779b97f4a7c15);
        return mixed ^ (mixed >> 32);
    }
};

struct NameHash { std::size_t operator()(Name name) const { return name.hash(); } };

struct NamePool {
    std::deque<Text> external_names;
    Name get(Text text) {
        if (text.size() == 9) {
            std::uint64_t x;
            std::memcpy(&x, text.data(), 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            x = __builtin_bswap64(x);
#endif
            auto tail = static_cast<unsigned char>(text[8]);
            if (!(x & UINT64_C(0x8080808080808080)) && tail < 128) {
                x = (x & UINT64_C(0x007f007f007f007f)) | ((x >> 1) & UINT64_C(0x3f803f803f803f80));
                x = (x & UINT64_C(0x00003fff00003fff)) | ((x >> 2) & UINT64_C(0x0fffc0000fffc000));
                x = (x & UINT64_C(0x000000000fffffff)) | ((x >> 4) & UINT64_C(0x00fffffff0000000));
                return {x | (std::uint64_t(tail) << 56)};
            }
        }
        external_names.push_back(text);
        return {Name::external | reinterpret_cast<std::uintptr_t>(&external_names.back())};
    }
};
