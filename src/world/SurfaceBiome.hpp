#pragma once

// Minimal surface biome ids for PlanetSurface sampling.
// When BiomeCatalog.hpp lands, prefer that BiomeId and retire this stub
// rather than duplicating catalogs.

#include "core/Math.hpp"
#include "world/PlanetTypes.hpp"

#include <cstdint>

namespace elysium {

enum class SurfaceBiome : std::uint8_t {
    Plains = 0,
    Scrub,
    Highland,
    Valley,
    BarrenFlat,
    ScorchedCrust,
    Count
};

// Deterministic biome sample from shared 3D direction space (cube-sphere seam-safe).
SurfaceBiome sampleBiome(std::uint64_t seed, PlanetClass planetClass, Vec3 direction);

} // namespace elysium
