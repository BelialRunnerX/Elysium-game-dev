#pragma once

#include "world/Block.hpp"
#include "world/CubeSphere.hpp"
#include "world/PlanetTypes.hpp"

#include <cstdint>

namespace elysium {

// Shared deterministic macro height (one metre shells). Same contract as the
// PlanetSurface column cache; regenerable from seed alone.
int proceduralSurfaceRadial(std::uint64_t seed, PlanetClass planetClass,
                            CubeFace face, int u, int v);

// Shared deterministic macro material for a radial shell in a column.
BlockType proceduralMacroBlock(std::uint64_t seed, PlanetClass planetClass,
                               CubeFace face, int u, int v, int radial, int surface);

// Max |Δsurface| vs four face-local neighbors (wraps across cube edges). O(1).
int proceduralColumnSlope(std::uint64_t seed, PlanetClass planetClass,
                          CubeFace face, int u, int v);

// True when this macro cell should be queried/meshed at 16³ resolution via
// regenerable subvoxel shaping (no journal storage required).
bool proceduralMicroActive(PlanetClass planetClass, int radial, int surface, int slope);

// Occupancy for one microcell. When the parent cell is inactive, returns
// macroBaseline unchanged. O(1) — never scans the brick.
BlockType proceduralMicroBlock(std::uint64_t seed, PlanetClass planetClass,
                               CubeFace face, int u, int v, int radial,
                               int surface, int slope,
                               int mu, int mr, int mv,
                               BlockType macroBaseline);

} // namespace elysium
