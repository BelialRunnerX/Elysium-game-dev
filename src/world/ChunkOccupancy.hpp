#pragma once

#include "world/Block.hpp"

#include <algorithm>
#include <cstdint>

namespace elysium {

// Adaptive chunk representation kind (catalog P0-7 / P0-10 foundations).
// Empty/Homogeneous/Full are cheap logical states; Mixed requires dense scans
// but still carries extremity bounds (catalog P0-6) to restrict work.
enum class ChunkRepKind : std::uint8_t {
    Empty = 0,        // no solid occupancy in the analyzed volume
    Homogeneous = 1,  // every cell identical non-air BlockType
    Full = 2,         // every cell solid (materials may differ)
    Mixed = 3         // partial solids and/or multiple materials
};

// Axis-aligned occupied solid extents in chunk-local coordinates.
struct ChunkOccupancyExtents {
    bool any{};
    int minX{};
    int minY{};
    int minZ{};
    int maxX{};
    int maxY{};
    int maxZ{};

    void reset() { *this = ChunkOccupancyExtents{}; }

    void include(int x, int y, int z) {
        if (!any) {
            any = true;
            minX = maxX = x;
            minY = maxY = y;
            minZ = maxZ = z;
            return;
        }
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        minZ = std::min(minZ, z);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        maxZ = std::max(maxZ, z);
    }

    bool contains(int x, int y, int z) const {
        return any && x >= minX && x <= maxX && y >= minY && y <= maxY && z >= minZ && z <= maxZ;
    }

    bool onBoundary(int x, int y, int z) const {
        return any && (x == minX || x == maxX || y == minY || y == maxY || z == minZ || z == maxZ);
    }

    int spanX() const { return any ? (maxX - minX + 1) : 0; }
    int spanY() const { return any ? (maxY - minY + 1) : 0; }
    int spanZ() const { return any ? (maxZ - minZ + 1) : 0; }
};

struct ChunkAdaptiveSummary {
    ChunkRepKind kind{ChunkRepKind::Empty};
    BlockType fill{BlockType::Air}; // meaningful when kind == Homogeneous
    ChunkOccupancyExtents extents{};
    int solidCount{};
    int cellCount{};

    bool isEmpty() const { return kind == ChunkRepKind::Empty; }
    bool isUniform() const { return kind == ChunkRepKind::Empty || kind == ChunkRepKind::Homogeneous; }
    bool isCompletelySolid() const {
        return kind == ChunkRepKind::Homogeneous || kind == ChunkRepKind::Full;
    }
};

// Finalize classification from scan counters.
ChunkAdaptiveSummary makeChunkAdaptiveSummary(int cellCount,
                                              int solidCount,
                                              int distinctSolidMaterials,
                                              BlockType firstSolid,
                                              bool allCellsIdentical,
                                              BlockType identicalType,
                                              const ChunkOccupancyExtents& extents);

// Analyze a dense XYZ block volume. get(x,y,z) returns BlockType; solid uses
// blockProperties().solid. Coordinates are chunk-local in [0,size).
template <typename Getter>
ChunkAdaptiveSummary analyzeChunkOccupancy(int sizeX, int sizeY, int sizeZ, Getter&& get) {
    ChunkOccupancyExtents extents;
    int solidCount = 0;
    int distinctSolid = 0;
    BlockType firstSolid = BlockType::Air;
    bool seenSolid[256]{};
    bool allIdentical = true;
    BlockType firstCell = BlockType::Air;
    bool haveFirst = false;
    const int cellCount = sizeX * sizeY * sizeZ;

    for (int z = 0; z < sizeZ; ++z) {
        for (int y = 0; y < sizeY; ++y) {
            for (int x = 0; x < sizeX; ++x) {
                const BlockType type = get(x, y, z);
                if (!haveFirst) {
                    firstCell = type;
                    haveFirst = true;
                } else if (type != firstCell) {
                    allIdentical = false;
                }
                if (!blockProperties(type).solid) continue;
                ++solidCount;
                extents.include(x, y, z);
                const auto raw = static_cast<std::uint8_t>(type);
                if (!seenSolid[raw]) {
                    seenSolid[raw] = true;
                    if (distinctSolid == 0) firstSolid = type;
                    ++distinctSolid;
                }
            }
        }
    }

    return makeChunkAdaptiveSummary(cellCount, solidCount, distinctSolid, firstSolid,
                                    allIdentical && haveFirst, firstCell, extents);
}

// Incremental occupancy update after a macro solidity change inside one chunk.
// Expanding solids is cheap; boundary removals may set *needsRescan.
void applyChunkSolidEdit(ChunkAdaptiveSummary& summary,
                         int localX, int localY, int localZ,
                         bool wasSolid, bool nowSolid,
                         BlockType nowType,
                         int sizeX, int sizeY, int sizeZ,
                         bool& needsRescan);

} // namespace elysium
