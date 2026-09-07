#pragma once

#include <cstdint>

namespace elysium {

enum class PlanetClass : std::uint8_t {
    Temperate = 0,
    Barren = 1,
    Scorched = 2
};

} // namespace elysium
