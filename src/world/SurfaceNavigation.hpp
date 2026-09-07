#pragma once

#include "core/Math.hpp"
#include "world/PlanetSurface.hpp"
#include "world/SurfaceWorldRead.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace elysium {

struct SurfaceNavigationStats {
    std::uint64_t requests{};
    std::uint64_t cacheHits{};
    std::uint64_t cacheMisses{};
    std::uint64_t expandedNodes{};
    std::uint64_t failedRoutes{};
    std::uint64_t cacheEvictions{};
    std::uint64_t revisionInvalidations{};
};

struct SurfaceNavigationResult {
    bool found{};
    bool fromCache{};
    int expanded{};
    std::vector<Vec3> waypoints;
};

// Bounded deterministic path planner over cube-sphere surface columns. It is
// intentionally a coarse navigation layer rather than a navmesh: each node is
// one surface column, neighbor ownership wraps through CubeSphere, and the
// final movement is projected back onto the authoritative curved world.
//
// The cache is keyed by start/goal + PlanetSurface revision, so any world edit
// invalidates old obstacle assumptions. This is conservative but correct; a
// later hierarchical graph can invalidate only routes crossing dirty chunks.
// findPath/advance are safe to call from read-only worker phases.
class SurfaceNavigationService {
public:
    explicit SurfaceNavigationService(const PlanetSurface& planet,
                                      std::size_t maxCachedRoutes = 64,
                                      int maxExpandedNodes = 512);

    SurfaceNavigationResult findPath(const SurfaceWorldReadService& read,
                                     Vec3 start,
                                     Vec3 goal,
                                     float hoverOffset = 1.8f,
                                     float agentRadius = 0.38f,
                                     float maxStepHeight = 1.6f) const;

    // Returns one bounded movement step toward goal using a cached/planned
    // route. If no route is found, returns the current position.
    Vec3 advance(const SurfaceWorldReadService& read,
                 Vec3 start,
                 Vec3 goal,
                 float speed,
                 float dt,
                 float hoverOffset = 1.8f,
                 float agentRadius = 0.38f,
                 float maxStepHeight = 1.6f) const;

    void clear() const;
    SurfaceNavigationStats stats() const;
    void resetStats() const;
    std::size_t cachedRouteCount() const;

private:
    struct RouteKey {
        int start{};
        int goal{};
        std::uint64_t revision{};
        int hoverQ{};
        int radiusQ{};
        int stepQ{};
        bool operator==(const RouteKey&) const = default;
    };
    struct RouteKeyHash {
        std::size_t operator()(const RouteKey& k) const noexcept;
    };
    struct CachedRoute {
        std::vector<int> nodes;
        std::uint64_t lastUse{};
    };

    const PlanetSurface& planet_;
    std::size_t maxCachedRoutes_{};
    int maxExpandedNodes_{};

    mutable std::mutex cacheMutex_;
    mutable std::unordered_map<RouteKey,CachedRoute,RouteKeyHash> cache_;
    mutable std::uint64_t cacheClock_{};
    mutable std::uint64_t cacheRevision_{};

    mutable std::atomic<std::uint64_t> requests_{0};
    mutable std::atomic<std::uint64_t> cacheHits_{0};
    mutable std::atomic<std::uint64_t> cacheMisses_{0};
    mutable std::atomic<std::uint64_t> expandedNodes_{0};
    mutable std::atomic<std::uint64_t> failedRoutes_{0};
    mutable std::atomic<std::uint64_t> cacheEvictions_{0};
    mutable std::atomic<std::uint64_t> revisionInvalidations_{0};

    int columnIndex(CubeFace face,int u,int v) const;
    SurfaceCellAddress columnFromIndex(int index) const;
    int locateColumn(Vec3 position) const;
    Vec3 columnDirection(int index) const;
    Vec3 hoverPosition(const SurfaceWorldReadService& read,int index,float hoverOffset) const;
    bool traversable(const SurfaceWorldReadService& read,int from,int to,float hoverOffset,float agentRadius,float maxStepHeight) const;
    std::vector<int> computeRoute(const SurfaceWorldReadService& read,int start,int goal,float hoverOffset,float agentRadius,float maxStepHeight,int& expanded) const;
    void ensureRevisionLocked(std::uint64_t revision) const;
    void insertCacheLocked(const RouteKey& key,std::vector<int> route) const;
};

} // namespace elysium
