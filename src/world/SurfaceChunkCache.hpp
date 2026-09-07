#pragma once

#include "core/JobSystem.hpp"
#include "world/ChunkOccupancy.hpp"
#include "world/ChunkVoxelSpans.hpp"
#include "world/PlanetSurface.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace elysium {

enum class SurfaceChunkPriority : std::uint8_t {
    EditRemesh = 0,
    Visible = 1,
    Prefetch = 2
};

// Reconstructed disposable LOD0 chunk packet. v0.11 retains a one-cell halo
// in all three axes so the mesher can decide boundary visibility without
// reaching back into a whole-planet snapshot. U/V halo sampling wraps across
// cube-sphere faces deterministically.
struct SurfaceChunkData {
    static constexpr int Halo = 1;
    static constexpr int CoreSize = PlanetSurface::ChunkSize;
    static constexpr int HaloSize = CoreSize + Halo * 2;

    PlanetChunkAddress address{};
    std::uint64_t revision{};
    float referenceRadius{48.0f};
    // Adaptive Homogeneous / RLE-column / Dense payload (P0-7/9/10). Logical
    // voxels match the prior dense HaloSize^3 layout: indexed [u+1,v+1,r+1].
    ChunkVoxelSpans voxels;
    // Refined cells from the core plus one-cell halo, keyed by stable whole-
    // planet flat cell index. This allows macro/refined boundary decisions at
    // chunk edges and cube-face seams without a snapshot dependency.
    std::unordered_map<int, MicroBrick> microBricks;
    // Catalog P0-6/P0-7/P0-10: occupied core extents + adaptive rep kind.
    // Computed when the packet is built; consumers may skip Empty cores and
    // restrict scans to extents without a second full pass.
    ChunkAdaptiveSummary occupancy{};
    // Per-column (localU, localV) radial solid min/max in core; 0xFF = empty.
    static constexpr std::uint8_t kColumnEmpty = 0xFF;
    std::array<std::uint8_t, CoreSize * CoreSize> columnMinR{};
    std::array<std::uint8_t, CoreSize * CoreSize> columnMaxR{};

    BlockType getLocal(int u, int v, int radial) const;
    BlockType getWithHalo(int u, int v, int radial) const;
    SurfaceCellAddress worldAddress(int localU, int localV, int localRadial) const;
    bool isRefined(SurfaceCellAddress address) const;
    const MicroBrick* microBrick(SurfaceCellAddress address) const;
    BlockType microGet(SurfaceCellAddress address, int mu, int mr, int mv) const;
    std::size_t estimatedBytes() const;
    void recomputeOccupancy();

    static int flatCellIndex(SurfaceCellAddress address);

private:
    static int haloIndex(int u, int v, int radial);
};

struct SurfaceChunkCacheStats {
    int resident{};
    int pending{};
    int wanted{};
    int retainedWhileRebuilding{};
    std::size_t residentBytes{};
    std::uint64_t generationRequests{};
    std::uint64_t published{};
    std::uint64_t evictions{};
    std::uint64_t cancellations{};
    std::uint64_t staleResults{};
    std::uint64_t droppedPrefetch{};
    std::uint64_t droppedVisible{};
    std::uint64_t boundedQueueRejects{};
    std::uint64_t priorityPreemptions{};
};

// Owner-thread cache for regenerated LOD0 voxel chunks. It intentionally stores
// reconstructed baseline data only; authoritative player history remains in the
// PlanetSurface per-chunk journals. Worker jobs own immutable snapshots and may
// be discarded by request token/revision without mutating the world.
class SurfaceChunkCache {
public:
    SurfaceChunkCache(JobSystem& jobs,
                      std::size_t maxResidentChunks = 12,
                      std::size_t maxResidentBytes = 8U * 1024U * 1024U,
                      std::size_t maxPending = 8);

    void beginFrame();
    bool request(const PlanetSurface& planet, const PlanetChunkAddress& address,
                 SurfaceChunkPriority priority = SurfaceChunkPriority::Visible);
    void cancelUnwanted();
    void sync(const PlanetSurface& planet);
    void clear();

    std::shared_ptr<const SurfaceChunkData> find(const PlanetChunkAddress& address) const;
    SurfaceChunkCacheStats stats() const;

    std::size_t maxResidentChunks() const { return maxResidentChunks_; }
    std::size_t maxResidentBytes() const { return maxResidentBytes_; }

private:
    struct BuildResult {
        PlanetChunkAddress address{};
        std::uint64_t revision{};
        std::uint64_t token{};
        std::shared_ptr<SurfaceChunkData> data;
    };

    struct Entry {
        PlanetChunkAddress address{};
        std::uint64_t revision{};
        std::uint64_t token{};
        std::uint64_t lastUse{};
        bool wanted{};
        SurfaceChunkPriority priority{SurfaceChunkPriority::Prefetch};
        std::shared_ptr<SurfaceChunkData> data;
        std::optional<std::future<BuildResult>> pending;
    };

    JobSystem& jobs_;
    std::size_t maxResidentChunks_{};
    std::size_t maxResidentBytes_{};
    std::size_t maxPending_{};
    std::uint64_t clock_{1};
    std::unordered_map<std::uint64_t, Entry> entries_;
    SurfaceChunkCacheStats cumulative_{};

    static std::shared_ptr<SurfaceChunkData> buildChunk(const PlanetSurfaceSnapshot& snapshot,
                                                        const PlanetChunkAddress& address,
                                                        std::uint64_t revision);
    void processCompleted(const PlanetSurface& planet);
    void evictToBudget();
    std::size_t pendingCount() const;
    // Cancel one strictly lower-priority in-flight job to free a pending slot.
    // Tie-break: lowest urgency first, then stable chunk key for determinism.
    bool tryCancelLowerPriorityPending(SurfaceChunkPriority needed);
};

} // namespace elysium
