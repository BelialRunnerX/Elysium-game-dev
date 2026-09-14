#pragma once

#include <cstdint>

namespace elysium {

// Stable Part 24 vertical-slice indices: 0=Temperate, 1=Barren, 2=Scorched.
// Catalog classes append after Scorched; do not renumber the first three.
enum class PlanetClass : std::uint8_t {
    Temperate = 0,
    Barren = 1,
    Scorched = 2,
    Frozen = 3,
    Toxic = 4,
    Irradiated = 5,
    Oceanic = 6,
    Anomalous = 7,
    Count
};

constexpr int kPlanetClassCount = static_cast<int>(PlanetClass::Count);

} // namespace elysium
