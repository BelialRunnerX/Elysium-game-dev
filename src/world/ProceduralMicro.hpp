#pragma once

#include "world/Block.hpp"
#include "world/CubeSphere.hpp"
#include "world/PlanetTypes.hpp"

#include <cstdint>

namespace elysium {

// Regenerable 16³ subvoxel shaping (zero journal storage for virgin terrain).
// Macro column heights/materials remain owned by PlanetSurface; this module only
// carves/adds micro occupancy near interesting surface / steep cells.

// True when this macro cell should be queried/meshed at micro resolution.
bool proceduralMicroActive(PlanetClass planetClass, int radial, int surface, int slope);

// Max |Δsurface| given center + four already-resolved neighbor heights. O(1).
int proceduralSlopeFromNeighbors(int surface, int sUPos, int sUNeg, int sVPos, int sVNeg);

// Occupancy for one microcell. Inactive cells return macroBaseline unchanged.
// Neighbor surfaces select cliff-lip / rubble downhill. O(1) — never scans a brick.
BlockType proceduralMicroBlock(std::uint64_t seed, PlanetClass planetClass,
                               CubeFace face, int u, int v, int radial,
                               int surface, int slope,
                               int sUPos, int sUNeg, int sVPos, int sVNeg,
                               int mu, int mr, int mv,
                               BlockType macroBaseline);

} // namespace elysium
