#include "world/SurfaceNavigation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>

namespace elysium {
namespace {

constexpr int kColumnsPerFace=PlanetSurface::FaceResolution*PlanetSurface::FaceResolution;
constexpr int kTotalColumns=PlanetSurface::FaceCount*kColumnsPerFace;

int quantize100(float v) {
    return static_cast<int>(std::lround(v*100.0f));
}

float angularDistance(Vec3 a,Vec3 b,float radius) {
    a=normalize(a); b=normalize(b);
    const float c=std::clamp(dot(a,b),-1.0f,1.0f);
    return std::acos(c)*radius;
}

} // namespace

SurfaceNavigationService::SurfaceNavigationService(const PlanetSurface& planet,
                                                   std::size_t maxCachedRoutes,
                                                   int maxExpandedNodes)
    : planet_(planet),
      maxCachedRoutes_(std::max<std::size_t>(1,maxCachedRoutes)),
      maxExpandedNodes_(std::max(8,maxExpandedNodes)),
      cacheRevision_(planet.revision()) {}

std::size_t SurfaceNavigationService::RouteKeyHash::operator()(const RouteKey& k) const noexcept {
    std::uint64_t h=1469598103934665603ULL;
    auto add=[&](std::uint64_t v){h^=v;h*=1099511628211ULL;};
    add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.start)));
    add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.goal)));
    add(k.revision);
    add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.hoverQ)));
    add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.radiusQ)));
    add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.stepQ)));
    return static_cast<std::size_t>(h);
}

int SurfaceNavigationService::columnIndex(CubeFace face,int u,int v) const {
    return static_cast<int>(face)*kColumnsPerFace + v*PlanetSurface::FaceResolution + u;
}

SurfaceCellAddress SurfaceNavigationService::columnFromIndex(int index) const {
    index=std::clamp(index,0,kTotalColumns-1);
    const int face=index/kColumnsPerFace;
    const int rem=index-face*kColumnsPerFace;
    const int v=rem/PlanetSurface::FaceResolution;
    const int u=rem-v*PlanetSurface::FaceResolution;
    return {static_cast<CubeFace>(face),u,v,PlanetSurface::ReferenceRadial};
}

int SurfaceNavigationService::locateColumn(Vec3 position) const {
    const auto a=planet_.locate(position);
    return columnIndex(a.face,a.u,a.v);
}

Vec3 SurfaceNavigationService::columnDirection(int index) const {
    const auto a=columnFromIndex(index);
    return faceGridCellDirection(a.face,a.u,a.v,PlanetSurface::FaceResolution);
}

Vec3 SurfaceNavigationService::hoverPosition(const SurfaceWorldReadService& read,int index,float hoverOffset) const {
    const Vec3 dir=columnDirection(index);
    return dir*(read.surfaceBoundaryRadius(dir)+hoverOffset);
}

bool SurfaceNavigationService::traversable(const SurfaceWorldReadService& read,int from,int to,
                                           float hoverOffset,float agentRadius,float maxStepHeight) const {
    const Vec3 a=hoverPosition(read,from,hoverOffset);
    const Vec3 b=hoverPosition(read,to,hoverOffset);
    const float ra=length(a), rb=length(b);
    if(std::abs(rb-ra)>std::max(0.1f,maxStepHeight)) return false;
    if(read.sphereCollides(b,agentRadius)) return false;

    // Midpoint probe catches thin one-column obstacles that can otherwise be
    // skipped by center-only sampling across a curved edge.
    const Vec3 midDir=normalize(normalize(a)+normalize(b));
    if(lengthSq(midDir)>0.5f) {
        const float midRadius=(ra+rb)*0.5f;
        if(read.sphereCollides(midDir*midRadius,agentRadius*0.9f)) return false;
    }
    return true;
}

std::vector<int> SurfaceNavigationService::computeRoute(const SurfaceWorldReadService& read,int start,int goal,
                                                        float hoverOffset,float agentRadius,float maxStepHeight,
                                                        int& expanded) const {
    expanded=0;
    if(start==goal) return {start};

    constexpr float inf=std::numeric_limits<float>::infinity();
    std::vector<float> g(static_cast<std::size_t>(kTotalColumns),inf);
    std::vector<int> parent(static_cast<std::size_t>(kTotalColumns),-1);
    std::vector<std::uint8_t> closed(static_cast<std::size_t>(kTotalColumns),0);

    struct OpenNode { float f{}; float g{}; int index{}; };
    struct Greater {
        bool operator()(const OpenNode& a,const OpenNode& b) const {
            if(std::abs(a.f-b.f)>1e-6f) return a.f>b.f;
            if(std::abs(a.g-b.g)>1e-6f) return a.g>b.g;
            return a.index>b.index;
        }
    };
    std::priority_queue<OpenNode,std::vector<OpenNode>,Greater> open;
    const Vec3 goalDir=columnDirection(goal);
    g[static_cast<std::size_t>(start)]=0.0f;
    open.push({angularDistance(columnDirection(start),goalDir,planet_.referenceRadius()),0.0f,start});

    constexpr std::array<std::pair<int,int>,4> offsets{{{1,0},{-1,0},{0,1},{0,-1}}};
    bool found=false;
    while(!open.empty() && expanded<maxExpandedNodes_) {
        const auto current=open.top(); open.pop();
        if(closed[static_cast<std::size_t>(current.index)]) continue;
        closed[static_cast<std::size_t>(current.index)]=1;
        ++expanded;
        if(current.index==goal) {found=true;break;}

        const auto c=columnFromIndex(current.index);
        for(const auto& [du,dv]:offsets) {
            const auto n=planet_.normalize({c.face,c.u+du,c.v+dv,PlanetSurface::ReferenceRadial});
            const int ni=columnIndex(n.face,n.u,n.v);
            if(closed[static_cast<std::size_t>(ni)]) continue;
            if(!traversable(read,current.index,ni,hoverOffset,agentRadius,maxStepHeight)) continue;
            const float edge=length(hoverPosition(read,current.index,hoverOffset)-hoverPosition(read,ni,hoverOffset));
            const float candidate=g[static_cast<std::size_t>(current.index)]+edge;
            if(candidate+1e-6f>=g[static_cast<std::size_t>(ni)]) continue;
            g[static_cast<std::size_t>(ni)]=candidate;
            parent[static_cast<std::size_t>(ni)]=current.index;
            const float h=angularDistance(columnDirection(ni),goalDir,planet_.referenceRadius());
            open.push({candidate+h,candidate,ni});
        }
    }
    if(!found) return {};

    std::vector<int> route;
    for(int at=goal;at>=0;at=parent[static_cast<std::size_t>(at)]) {
        route.push_back(at);
        if(at==start) break;
    }
    if(route.empty() || route.back()!=start) return {};
    std::reverse(route.begin(),route.end());
    return route;
}

void SurfaceNavigationService::ensureRevisionLocked(std::uint64_t revision) const {
    if(cacheRevision_==revision) return;
    if(!cache_.empty()) revisionInvalidations_.fetch_add(1,std::memory_order_relaxed);
    cache_.clear();
    cacheRevision_=revision;
}

void SurfaceNavigationService::insertCacheLocked(const RouteKey& key,std::vector<int> route) const {
    if(cache_.size()>=maxCachedRoutes_) {
        auto victim=cache_.end();
        for(auto it=cache_.begin();it!=cache_.end();++it) {
            if(victim==cache_.end() || it->second.lastUse<victim->second.lastUse ||
               (it->second.lastUse==victim->second.lastUse && it->first.start<victim->first.start)) victim=it;
        }
        if(victim!=cache_.end()) {cache_.erase(victim);cacheEvictions_.fetch_add(1,std::memory_order_relaxed);}
    }
    cache_[key]={std::move(route),++cacheClock_};
}

SurfaceNavigationResult SurfaceNavigationService::findPath(const SurfaceWorldReadService& read,Vec3 start,Vec3 goal,
                                                           float hoverOffset,float agentRadius,float maxStepHeight) const {
    requests_.fetch_add(1,std::memory_order_relaxed);
    SurfaceNavigationResult result{};
    if(lengthSq(start)<1.0f || lengthSq(goal)<1.0f) {failedRoutes_.fetch_add(1,std::memory_order_relaxed);return result;}
    const int startIndex=locateColumn(start);
    const int goalIndex=locateColumn(goal);
    const RouteKey key{startIndex,goalIndex,planet_.revision(),quantize100(hoverOffset),quantize100(agentRadius),quantize100(maxStepHeight)};

    std::vector<int> nodes;
    {
        std::lock_guard lock(cacheMutex_);
        ensureRevisionLocked(key.revision);
        const auto it=cache_.find(key);
        if(it!=cache_.end()) {
            it->second.lastUse=++cacheClock_;
            nodes=it->second.nodes;
            result.fromCache=true;
            cacheHits_.fetch_add(1,std::memory_order_relaxed);
        }
    }

    if(nodes.empty()) {
        cacheMisses_.fetch_add(1,std::memory_order_relaxed);
        nodes=computeRoute(read,startIndex,goalIndex,hoverOffset,agentRadius,maxStepHeight,result.expanded);
        expandedNodes_.fetch_add(static_cast<std::uint64_t>(result.expanded),std::memory_order_relaxed);
        if(nodes.empty()) {failedRoutes_.fetch_add(1,std::memory_order_relaxed);return result;}
        std::lock_guard lock(cacheMutex_);
        ensureRevisionLocked(key.revision);
        insertCacheLocked(key,nodes);
    }

    result.found=true;
    result.waypoints.reserve(nodes.size());
    for(const int node:nodes) result.waypoints.push_back(hoverPosition(read,node,hoverOffset));
    return result;
}

Vec3 SurfaceNavigationService::advance(const SurfaceWorldReadService& read,Vec3 start,Vec3 goal,float speed,float dt,
                                       float hoverOffset,float agentRadius,float maxStepHeight) const {
    if(dt<=0.0f || speed<=0.0f) return start;
    const auto route=findPath(read,start,goal,hoverOffset,agentRadius,maxStepHeight);
    if(!route.found || route.waypoints.empty()) return start;

    Vec3 target=route.waypoints.back();
    for(std::size_t i=1;i<route.waypoints.size();++i) {
        if(length(route.waypoints[i]-start)>0.35f) {target=route.waypoints[i];break;}
    }
    const Vec3 up=normalize(start);
    Vec3 delta=target-start;
    delta-=up*dot(delta,up);
    if(lengthSq(delta)<1e-5f) return start;
    const float distance=std::min(speed*dt,length(delta));
    const Vec3 tangent=normalize(delta);
    const Vec3 candidateDir=normalize(start+tangent*distance);
    Vec3 candidate=candidateDir*(read.surfaceBoundaryRadius(candidateDir)+hoverOffset);
    if(read.sphereCollides(candidate,agentRadius)) return start;
    return candidate;
}

void SurfaceNavigationService::clear() const {
    std::lock_guard lock(cacheMutex_);
    cache_.clear();
    cacheRevision_=planet_.revision();
}

std::size_t SurfaceNavigationService::cachedRouteCount() const {
    std::lock_guard lock(cacheMutex_);
    return cache_.size();
}

SurfaceNavigationStats SurfaceNavigationService::stats() const {
    return {
        requests_.load(std::memory_order_relaxed),
        cacheHits_.load(std::memory_order_relaxed),
        cacheMisses_.load(std::memory_order_relaxed),
        expandedNodes_.load(std::memory_order_relaxed),
        failedRoutes_.load(std::memory_order_relaxed),
        cacheEvictions_.load(std::memory_order_relaxed),
        revisionInvalidations_.load(std::memory_order_relaxed)
    };
}

void SurfaceNavigationService::resetStats() const {
    requests_.store(0,std::memory_order_relaxed);
    cacheHits_.store(0,std::memory_order_relaxed);
    cacheMisses_.store(0,std::memory_order_relaxed);
    expandedNodes_.store(0,std::memory_order_relaxed);
    failedRoutes_.store(0,std::memory_order_relaxed);
    cacheEvictions_.store(0,std::memory_order_relaxed);
    revisionInvalidations_.store(0,std::memory_order_relaxed);
}

} // namespace elysium
