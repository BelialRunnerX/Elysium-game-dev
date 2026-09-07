#pragma once

#include "world/PlanetSurface.hpp"
#include "world/SurfaceChunkCache.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace elysium {

struct SurfaceWorldReadStats {
    std::uint64_t cachedMacroReads{};
    std::uint64_t cachedMicroReads{};
    std::uint64_t proceduralFallbackReads{};
    std::uint64_t staleOrMissingChunks{};
    std::uint64_t solidQueries{};
    std::uint64_t raySteps{};
};

// Read-only near-player world service. v0.12 makes the bounded reconstructed
// SurfaceChunkCache useful to gameplay as well as rendering: collision,
// raycasts, and AI steering can all consume the same immutable 32^3+halo
// packets. A deterministic PlanetSurface fallback remains mandatory while a
// requested chunk is still generating or has just become stale after an edit.
//
// The service performs no cache publication and no world mutation, so a single
// instance may be shared by read-only worker phases while the owner thread is
// not mutating PlanetSurface/SurfaceChunkCache.
class SurfaceWorldReadService {
public:
    SurfaceWorldReadService(const PlanetSurface& planet, const SurfaceChunkCache& cache)
        : planet_(planet), cache_(cache) {}

    BlockType get(SurfaceCellAddress address) const;
    BlockType microGet(const SurfaceMicroAddress& address) const;
    bool solidAt(Vec3 planetLocalPosition) const;
    bool sphereCollides(Vec3 center, float radius = 0.34f) const;
    bool capsuleCollides(Vec3 feet, float radius = 0.32f, float height = 1.75f) const;
    bool groundedAt(Vec3 feet, float probe = 0.09f) const;
    float surfaceBoundaryRadius(Vec3 direction) const;
    SurfaceRayHit raycast(Vec3 origin, Vec3 direction, float maxDistance, float step = 0.10f) const;
    SurfaceMicroRayHit raycastMicro(Vec3 origin, Vec3 direction, float maxDistance, float step = 0.018f) const;

    SurfaceWorldReadStats stats() const;
    void resetStats() const;

private:
    const PlanetSurface& planet_;
    const SurfaceChunkCache& cache_;

    mutable std::atomic<std::uint64_t> cachedMacroReads_{0};
    mutable std::atomic<std::uint64_t> cachedMicroReads_{0};
    mutable std::atomic<std::uint64_t> proceduralFallbackReads_{0};
    mutable std::atomic<std::uint64_t> staleOrMissingChunks_{0};
    mutable std::atomic<std::uint64_t> solidQueries_{0};
    mutable std::atomic<std::uint64_t> raySteps_{0};

    std::shared_ptr<const SurfaceChunkData> residentFor(SurfaceCellAddress normalized) const;
};

} // namespace elysium
