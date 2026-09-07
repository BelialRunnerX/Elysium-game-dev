#include "world/ChunkOccupancy.hpp"

namespace elysium {

ChunkAdaptiveSummary makeChunkAdaptiveSummary(int cellCount,
                                              int solidCount,
                                              int distinctSolidMaterials,
                                              BlockType firstSolid,
                                              bool allCellsIdentical,
                                              BlockType identicalType,
                                              const ChunkOccupancyExtents& extents) {
    ChunkAdaptiveSummary out{};
    out.cellCount = cellCount;
    out.solidCount = solidCount;
    out.extents = extents;

    if (solidCount <= 0 || cellCount <= 0) {
        out.kind = ChunkRepKind::Empty;
        out.fill = BlockType::Air;
        out.extents.reset();
        return out;
    }

    if (allCellsIdentical) {
        // Uniform fill across the entire volume.
        if (!blockProperties(identicalType).solid) {
            out.kind = ChunkRepKind::Empty;
            out.fill = BlockType::Air;
            out.extents.reset();
            out.solidCount = 0;
            return out;
        }
        out.kind = ChunkRepKind::Homogeneous;
        out.fill = identicalType;
        return out;
    }

    if (solidCount == cellCount) {
        // Every cell solid; materials may differ.
        out.kind = ChunkRepKind::Full;
        out.fill = (distinctSolidMaterials == 1) ? firstSolid : BlockType::Air;
        return out;
    }

    out.kind = ChunkRepKind::Mixed;
    out.fill = (distinctSolidMaterials == 1) ? firstSolid : BlockType::Air;
    return out;
}

void applyChunkSolidEdit(ChunkAdaptiveSummary& summary,
                         int localX, int localY, int localZ,
                         bool wasSolid, bool nowSolid,
                         BlockType nowType,
                         int sizeX, int sizeY, int sizeZ,
                         bool& needsRescan) {
    needsRescan = false;
    if (wasSolid == nowSolid) {
        // Material-only change inside an already-solid cell: homogeneous may break.
        if (wasSolid && summary.kind == ChunkRepKind::Homogeneous && nowType != summary.fill) {
            summary.kind = (summary.solidCount == summary.cellCount) ? ChunkRepKind::Full
                                                                    : ChunkRepKind::Mixed;
            summary.fill = BlockType::Air;
        }
        return;
    }

    if (nowSolid) {
        ++summary.solidCount;
        summary.extents.include(localX, localY, localZ);
        if (summary.kind == ChunkRepKind::Empty) {
            // First solid in an empty chunk — treat as mixed until a full rescan
            // can prove homogeneity (single-cell edits almost never fill a chunk).
            summary.kind = ChunkRepKind::Mixed;
            summary.fill = nowType;
        } else if (summary.kind == ChunkRepKind::Homogeneous) {
            if (nowType != summary.fill) {
                summary.kind = (summary.solidCount == summary.cellCount) ? ChunkRepKind::Full
                                                                        : ChunkRepKind::Mixed;
                summary.fill = BlockType::Air;
            } else if (summary.solidCount == summary.cellCount) {
                // Still uniform solid after filling remaining air? Unreachable for
                // Homogeneous which already required full uniformity; keep kind.
            }
        } else if (summary.solidCount == summary.cellCount) {
            summary.kind = ChunkRepKind::Full;
        } else {
            summary.kind = ChunkRepKind::Mixed;
        }
        return;
    }

    // Solid removed.
    --summary.solidCount;
    if (summary.solidCount <= 0) {
        summary = ChunkAdaptiveSummary{};
        summary.cellCount = sizeX * sizeY * sizeZ;
        summary.kind = ChunkRepKind::Empty;
        return;
    }
    if (!summary.extents.onBoundary(localX, localY, localZ)) {
        // Interior removal: AABB unchanged; representation is at best Mixed/Full→Mixed.
        if (summary.kind == ChunkRepKind::Homogeneous || summary.kind == ChunkRepKind::Full)
            summary.kind = ChunkRepKind::Mixed;
        summary.fill = BlockType::Air;
        return;
    }
    // Boundary removal may shrink extents — rescan the chunk volume.
    needsRescan = true;
}

} // namespace elysium
