#pragma once

#include <cstdint>

namespace elysium {

// P0-21: solidity / refined occupancy packed into machine words so neighbor
// solid-solid culls are shifts + bitwise AND-NOT instead of per-cell probes.
//
// Layout convention for a core+halo column: bit (local + halo) corresponds to
// local coordinate `local`. Halo defaults to 1, matching SurfaceChunkData and
// planar chunk-edge / world-bound padding. Example for ChunkSize=32:
//   bit 0 → local -1 (near halo / OOB / mantle)
//   bits 1..32 → core 0..31
//   bit 33 → local +32 (far halo / OOB)
//
// Visible mesh topology is unchanged vs the prior scalar solid-solid test:
// a cell emits a greedy macro face iff it is solid and not refined, and the
// neighbor along that axis is neither solid nor refined (planar also requires
// the neighbor to be exterior air or out of bounds).

using FaceCullWord = std::uint64_t;

struct FaceCullColumn {
    FaceCullWord solid{};
    FaceCullWord refined{};
};

inline constexpr int kFaceCullWordBits = 64;
inline constexpr int kFaceCullHalo = 1;

inline constexpr FaceCullWord faceCullBit(int i) {
    if (i < 0 || i >= kFaceCullWordBits) return FaceCullWord{0};
    return FaceCullWord{1} << i;
}

inline constexpr bool faceCullTest(FaceCullWord word, int i) {
    return (word & faceCullBit(i)) != 0;
}

inline constexpr int faceCullBitIndex(int localCoord, int halo = kFaceCullHalo) {
    return localCoord + halo;
}

// Neighbor +1 is the next higher bit → shift toward LSB.
inline constexpr FaceCullWord cullSolidSolidPos(FaceCullWord solid) {
    return solid & ~(solid >> 1);
}

// Neighbor -1 is the next lower bit → shift toward MSB.
inline constexpr FaceCullWord cullSolidSolidNeg(FaceCullWord solid) {
    return solid & ~(solid << 1);
}

// Macro greedy candidate along +axis: self solid & ~refined; neighbor not
// (solid | refined). Equivalent to the prior snapshot/cached scalar test.
inline constexpr FaceCullWord cullMacroPos(FaceCullWord solid, FaceCullWord refined) {
    const FaceCullWord self = solid & ~refined;
    const FaceCullWord blocked = (solid | refined) >> 1;
    return self & ~blocked;
}

inline constexpr FaceCullWord cullMacroNeg(FaceCullWord solid, FaceCullWord refined) {
    const FaceCullWord self = solid & ~refined;
    const FaceCullWord blocked = (solid | refined) << 1;
    return self & ~blocked;
}

// Planar VoxelMesher extra: neighbor must be exterior air or out of world
// bounds (emitNeighbor bit set). Interior cavities stay hidden.
inline constexpr FaceCullWord cullPlanarPos(FaceCullWord solid, FaceCullWord refined,
                                            FaceCullWord emitNeighbor) {
    return cullMacroPos(solid, refined) & (emitNeighbor >> 1);
}

inline constexpr FaceCullWord cullPlanarNeg(FaceCullWord solid, FaceCullWord refined,
                                            FaceCullWord emitNeighbor) {
    return cullMacroNeg(solid, refined) & (emitNeighbor << 1);
}

// Scalar oracles used by headless agreement tests (must stay identical to the
// bitwise forms above).
inline constexpr bool scalarSolidSolidExposed(bool selfSolid, bool neighborSolid) {
    return selfSolid && !neighborSolid;
}

inline constexpr bool scalarMacroExposed(bool selfSolid, bool selfRefined,
                                         bool neighborSolid, bool neighborRefined) {
    return selfSolid && !selfRefined && !neighborSolid && !neighborRefined;
}

inline constexpr bool scalarPlanarExposed(bool selfSolid, bool selfRefined,
                                          bool neighborSolid, bool neighborRefined,
                                          bool neighborEmit) {
    return scalarMacroExposed(selfSolid, selfRefined, neighborSolid, neighborRefined)
        && neighborEmit;
}

template <class Pred>
FaceCullWord packPredBits(int count, Pred&& pred) {
    FaceCullWord word = 0;
    const int n = count < kFaceCullWordBits ? count : kFaceCullWordBits;
    for (int i = 0; i < n; ++i) {
        if (pred(i)) word |= (FaceCullWord{1} << i);
    }
    return word;
}

// Pack one micro-brick axis including a one-cell halo on each end. axis: 0=U, 1=V, 2=R.
// solidAt may be queried at local coordinates in [-1, n] (neighbor brick / macro).
template <class SolidAt>
FaceCullWord packMicroAxisBits(int n, int originU, int originR, int originV, int axis, SolidAt&& solidAt) {
    const int count = n + kFaceCullHalo * 2;
    return packPredBits(count, [&](int i) {
        int mu = originU, mr = originR, mv = originV;
        const int p = i - kFaceCullHalo;
        if (axis == 0) mu = p;
        else if (axis == 1) mv = p;
        else mr = p;
        return solidAt(mu, mr, mv);
    });
}

} // namespace elysium
