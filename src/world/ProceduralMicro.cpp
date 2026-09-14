#include "world/ProceduralMicro.hpp"

#include "core/Determinism.hpp"
#include "world/MicroBrick.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace elysium {
namespace {

int faceIndex(CubeFace face) {
    return static_cast<int>(face);
}

float microNoise(std::uint64_t seed, CubeFace face, int u, int v, int mu, int mv, std::uint64_t tag) {
    const int gu = u * MicroBrick::Resolution + mu;
    const int gv = v * MicroBrick::Resolution + mv;
    return hash01(seed, gu, faceIndex(face), gv, tag);
}

int clampMicro(int x) {
    return std::clamp(x, 0, MicroBrick::Resolution - 1);
}

void downhillFromNeighbors(int surface, int sUPos, int sUNeg, int sVPos, int sVNeg,
                           int& du, int& dv) {
    du = 0;
    dv = 0;
    int best = 0;
    const std::array<std::tuple<int, int, int>, 4> dirs{{
        {1, 0, surface - sUPos},
        {-1, 0, surface - sUNeg},
        {0, 1, surface - sVPos},
        {0, -1, surface - sVNeg}
    }};
    for (const auto& [x, y, drop] : dirs) {
        if (drop > best) {
            best = drop;
            du = x;
            dv = y;
        }
    }
}

BlockType rockBandType(PlanetClass planetClass) {
    switch (planetClass) {
        case PlanetClass::Temperate: return BlockType::Stone;
        case PlanetClass::Barren: return BlockType::Regolith;
        case PlanetClass::Scorched: return BlockType::Basalt;
        case PlanetClass::Frozen: return BlockType::Stone;
        case PlanetClass::Toxic: return BlockType::Dirt;
        case PlanetClass::Irradiated: return BlockType::Regolith;
        case PlanetClass::Oceanic: return BlockType::Stone;
        case PlanetClass::Anomalous: return BlockType::Stone;
        case PlanetClass::Count: break;
    }
    return BlockType::Stone;
}

} // namespace

int proceduralSlopeFromNeighbors(int surface, int sUPos, int sUNeg, int sVPos, int sVNeg) {
    return std::max({std::abs(surface - sUPos), std::abs(surface - sUNeg),
                     std::abs(surface - sVPos), std::abs(surface - sVNeg)});
}

bool proceduralMicroActive(PlanetClass planetClass, int radial, int surface, int slope) {
    const int depth = surface - radial;
    switch (planetClass) {
        case PlanetClass::Temperate:
            // Sparse: steep relief only — keeps most cells on the 1 m macro path.
            if (depth == 0 && slope >= 2) return true;
            if (depth >= 1 && depth <= 2 && slope >= 3) return true;
            if (depth == -1 && slope >= 3) return true;
            return false;
        case PlanetClass::Barren:
        case PlanetClass::Scorched:
        case PlanetClass::Frozen:
        case PlanetClass::Toxic:
        case PlanetClass::Irradiated:
        case PlanetClass::Oceanic:
        case PlanetClass::Anomalous:
            return depth == 0 && slope >= 3;
        case PlanetClass::Count:
            break;
    }
    return false;
}

BlockType proceduralMicroBlock(std::uint64_t seed, PlanetClass planetClass,
                               CubeFace face, int u, int v, int radial,
                               int surface, int slope,
                               int sUPos, int sUNeg, int sVPos, int sVNeg,
                               int mu, int mr, int mv,
                               BlockType macroBaseline) {
    if (!MicroBrick::inBounds(mu, mr, mv)) return BlockType::Air;
    if (!proceduralMicroActive(planetClass, radial, surface, slope)) return macroBaseline;

    const int depth = surface - radial;
    const int N = MicroBrick::Resolution;

    int ddu = 0, ddv = 0;
    downhillFromNeighbors(surface, sUPos, sUNeg, sVPos, sVNeg, ddu, ddv);

    int edge = N / 2;
    if (ddu > 0) edge = mu;
    else if (ddu < 0) edge = (N - 1) - mu;
    else if (ddv > 0) edge = mv;
    else if (ddv < 0) edge = (N - 1) - mv;

    const float nCrust = microNoise(seed, face, u, v, mu, mv, 0x4D494352ULL); // MICR
    const float nRubble = microNoise(seed, face, u, v, mu, mv, 0x5255424CULL); // RUBL
    const float nCrack = microNoise(seed, face, u, v, mu, mv, 0x4352414BULL); // CRAK
    const BlockType rock = rockBandType(planetClass);

    // Air shell above steep columns: rubble skirts + sparse overhang chips.
    if (depth == -1) {
        if (planetClass != PlanetClass::Temperate) return macroBaseline;
        if (mr == 0 && edge >= N - 4 && nRubble > 0.55f - 0.04f * static_cast<float>(slope)) {
            return rock;
        }
        if (mr <= 2 && edge >= N - 2 && nCrust > 0.82f) return rock;
        return BlockType::Air;
    }

    if (depth < 0) return macroBaseline;

    // Rocky step heightfield inside the surface metre.
    const float relief = std::clamp(0.18f * static_cast<float>(slope) + 0.35f * nCrust, 0.0f, 0.92f);
    const int stepTop = clampMicro(static_cast<int>(std::floor(relief * static_cast<float>(N))));
    const int lipBias = (slope >= 2) ? std::max(0, edge - (N / 2)) / 2 : 0;
    const int localTop = clampMicro(stepTop + lipBias);

    if (depth == 0) {
        if (mr > localTop) return BlockType::Air;
        // Eroded cliff lip: rock near the downhill edge.
        if (planetClass == PlanetClass::Temperate && slope >= 2 && edge >= N - 3 && mr >= localTop - 1) {
            return rock;
        }
        // Cracked crust seams on grassy tops.
        if (planetClass == PlanetClass::Temperate && mr == localTop && nCrack > 0.88f) {
            return rock;
        }
        if (planetClass != PlanetClass::Temperate && mr == localTop && nCrust > 0.70f) {
            return rock;
        }
        return macroBaseline;
    }

    // Near-surface rock band on steep Temperate: cavities + fracture chips.
    if (planetClass == PlanetClass::Temperate && depth >= 1 && depth <= 2 && slope >= 2) {
        if (edge >= N - 2 && mr >= N - 3 && nRubble > 0.72f) return BlockType::Air;
        if (edge >= N - 3 && nCrack > 0.90f && (mu + mv + mr) % 5 == 0) return rock;
    }

    return macroBaseline;
}

} // namespace elysium
