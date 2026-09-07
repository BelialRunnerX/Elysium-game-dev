#pragma once

#include <cstdint>

namespace elysium {

inline std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}

inline std::uint64_t hashCoords(std::uint64_t seed, int x, int y, int z, std::uint64_t label = 0) {
    std::uint64_t h = mix64(seed ^ label);
    h = mix64(h ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)));
    h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 1U));
    h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) << 2U));
    return h;
}

inline float hash01(std::uint64_t seed, int x, int y, int z, std::uint64_t label = 0) {
    const auto h = hashCoords(seed, x, y, z, label);
    return static_cast<float>((h >> 40U) & 0xFFFFFFU) / static_cast<float>(0xFFFFFFU);
}

} // namespace elysium
