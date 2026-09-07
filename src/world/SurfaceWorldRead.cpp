#include "world/SurfaceWorldRead.hpp"

#include "world/Block.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace elysium {

std::shared_ptr<const SurfaceChunkData> SurfaceWorldReadService::residentFor(SurfaceCellAddress a) const {
    if (!planet_.radialInBounds(a.radial)) return {};
    a=planet_.normalize(a);
    const auto chunk=planet_.chunkOf(a);
    auto data=cache_.find(chunk);
    if (!data || data->revision!=planet_.chunkRevision(chunk)) {
        staleOrMissingChunks_.fetch_add(1,std::memory_order_relaxed);
        return {};
    }
    return data;
}

BlockType SurfaceWorldReadService::get(SurfaceCellAddress a) const {
    if (a.radial<0) return BlockType::Stone;
    if (a.radial>=PlanetSurface::RadialLayers) return BlockType::Air;
    a=planet_.normalize(a);
    if (const auto data=residentFor(a)) {
        const int lu=a.u-data->address.u*PlanetSurface::ChunkSize;
        const int lv=a.v-data->address.v*PlanetSurface::ChunkSize;
        const int lr=a.radial-data->address.radial*PlanetSurface::ChunkSize;
        cachedMacroReads_.fetch_add(1,std::memory_order_relaxed);
        return data->getLocal(lu,lv,lr);
    }
    proceduralFallbackReads_.fetch_add(1,std::memory_order_relaxed);
    return planet_.get(a);
}

BlockType SurfaceWorldReadService::microGet(const SurfaceMicroAddress& m) const {
    auto a=m.cell;
    if (a.radial<0) return BlockType::Stone;
    if (a.radial>=PlanetSurface::RadialLayers) return BlockType::Air;
    a=planet_.normalize(a);
    if (const auto data=residentFor(a)) {
        cachedMicroReads_.fetch_add(1,std::memory_order_relaxed);
        return data->microGet(a,m.u,m.radial,m.v);
    }
    proceduralFallbackReads_.fetch_add(1,std::memory_order_relaxed);
    return planet_.microGet(a,m.u,m.radial,m.v);
}

bool SurfaceWorldReadService::solidAt(Vec3 p) const {
    solidQueries_.fetch_add(1,std::memory_order_relaxed);
    const auto m=planet_.locateMicro(p);
    if (!planet_.radialInBounds(m.cell.radial)) return m.cell.radial<0;
    return blockProperties(microGet(m)).solid;
}

bool SurfaceWorldReadService::sphereCollides(Vec3 center,float radius) const {
    if (lengthSq(center)<1.0f) return true;
    radius=std::max(0.01f,radius);
    const auto frame=planet_.surfaceFrame(center);
    const float d=radius*0.70710678f;
    const std::array<Vec3,15> offsets{{
        {0,0,0},
        frame.right*radius,frame.right*(-radius),
        frame.forward*radius,frame.forward*(-radius),
        frame.up*radius,frame.up*(-radius),
        (frame.right+frame.forward)*d,(frame.right-frame.forward)*d,
        (frame.forward-frame.right)*d,(frame.right+frame.forward)*(-d),
        (frame.right+frame.up)*d,(frame.right-frame.up)*d,
        (frame.forward+frame.up)*d,(frame.forward-frame.up)*d
    }};
    for (const auto& off:offsets) if (solidAt(center+off)) return true;
    return false;
}

bool SurfaceWorldReadService::capsuleCollides(Vec3 feet,float radius,float height) const {
    if (lengthSq(feet)<1.0f) return true;
    const auto frame=planet_.surfaceFrame(feet);
    const float low=0.03f;
    const float high=std::max(low,height-0.10f);
    const std::array<float,3> levels{low,(low+high)*0.5f,high};
    const std::array<Vec3,9> offsets{{
        {0,0,0},
        frame.right*radius, frame.right*(-radius),
        frame.forward*radius, frame.forward*(-radius),
        normalize(frame.right+frame.forward)*radius,
        normalize(frame.right-frame.forward)*radius,
        normalize(frame.forward-frame.right)*radius,
        normalize((frame.right+frame.forward)*-1.0f)*radius
    }};
    for (float h:levels) {
        const Vec3 center=feet+frame.up*h;
        for (const auto& off:offsets) if (solidAt(center+off)) return true;
    }
    return false;
}

bool SurfaceWorldReadService::groundedAt(Vec3 feet,float probe) const {
    if (lengthSq(feet)<1.0f) return false;
    return capsuleCollides(feet-normalize(feet)*std::max(0.01f,probe));
}

float SurfaceWorldReadService::surfaceBoundaryRadius(Vec3 direction) const {
    const FaceUv uv=directionToFaceUv(direction);
    const float fu=(uv.u+1.0f)*0.5f*static_cast<float>(PlanetSurface::FaceResolution);
    const float fv=(uv.v+1.0f)*0.5f*static_cast<float>(PlanetSurface::FaceResolution);
    const int u=std::clamp(static_cast<int>(std::floor(fu)),0,PlanetSurface::FaceResolution-1);
    const int v=std::clamp(static_cast<int>(std::floor(fv)),0,PlanetSurface::FaceResolution-1);
    int surface=-1;
    for (int r=PlanetSurface::RadialLayers-1;r>=0;--r) {
        if (blockProperties(get({uv.face,u,v,r})).solid) { surface=r; break; }
    }
    if (surface<0) return planet_.referenceRadius()-static_cast<float>(PlanetSurface::ReferenceRadial);
    return planet_.referenceRadius()+static_cast<float>(surface+1-PlanetSurface::ReferenceRadial);
}

SurfaceRayHit SurfaceWorldReadService::raycast(Vec3 origin,Vec3 direction,float maxDistance,float step) const {
    SurfaceRayHit out{};
    direction=normalize(direction);
    if (lengthSq(direction)<0.5f || maxDistance<=0.0f) return out;
    step=std::clamp(step,0.025f,0.5f);
    SurfaceCellAddress previous=planet_.locateUnclamped(origin);
    bool havePrevious=planet_.radialInBounds(previous.radial);
    for(float t=0.0f;t<=maxDistance+step*0.5f;t+=step) {
        raySteps_.fetch_add(1,std::memory_order_relaxed);
        const Vec3 point=origin+direction*t;
        const auto cell=planet_.locateUnclamped(point);
        if (!planet_.radialInBounds(cell.radial)) continue;
        if (solidAt(point)) {
            out.hit=true;
            out.cell=planet_.normalize(cell);
            out.previous=havePrevious?planet_.normalize(previous):out.cell;
            out.point=point;
            out.distance=t;
            return out;
        }
        previous=cell;
        havePrevious=true;
    }
    return out;
}

SurfaceMicroRayHit SurfaceWorldReadService::raycastMicro(Vec3 origin,Vec3 direction,float maxDistance,float step) const {
    SurfaceMicroRayHit out{};
    direction=normalize(direction);
    if (lengthSq(direction)<0.5f || maxDistance<=0.0f) return out;
    step=std::clamp(step,0.008f,0.05f);
    SurfaceMicroAddress last{};
    bool haveLast=false;
    for(float t=0.0f;t<=maxDistance+step*0.5f;t+=step) {
        raySteps_.fetch_add(1,std::memory_order_relaxed);
        const Vec3 point=origin+direction*t;
        const auto m=planet_.locateMicro(point);
        if (!planet_.radialInBounds(m.cell.radial)) continue;
        if (haveLast && m==last) continue;
        last=m; haveLast=true;
        const auto type=microGet(m);
        if (blockProperties(type).solid) {
            out.hit=true; out.micro=m; out.point=point; out.distance=t; out.type=type;
            return out;
        }
    }
    return out;
}

SurfaceWorldReadStats SurfaceWorldReadService::stats() const {
    return {
        cachedMacroReads_.load(std::memory_order_relaxed),
        cachedMicroReads_.load(std::memory_order_relaxed),
        proceduralFallbackReads_.load(std::memory_order_relaxed),
        staleOrMissingChunks_.load(std::memory_order_relaxed),
        solidQueries_.load(std::memory_order_relaxed),
        raySteps_.load(std::memory_order_relaxed)
    };
}

void SurfaceWorldReadService::resetStats() const {
    cachedMacroReads_.store(0,std::memory_order_relaxed);
    cachedMicroReads_.store(0,std::memory_order_relaxed);
    proceduralFallbackReads_.store(0,std::memory_order_relaxed);
    staleOrMissingChunks_.store(0,std::memory_order_relaxed);
    solidQueries_.store(0,std::memory_order_relaxed);
    raySteps_.store(0,std::memory_order_relaxed);
}

} // namespace elysium
