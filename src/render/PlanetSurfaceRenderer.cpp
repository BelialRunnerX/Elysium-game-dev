#include "render/PlanetSurfaceRenderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace elysium {

PlanetSurfaceRenderer::PlanetSurfaceRenderer(JobSystem& jobs, IGraphicsBackend& graphics)
    : jobs_(jobs), graphics_(graphics), chunkCache_(jobs,10,16U*1024U*1024U,8), chunks_(PlanetSurface::ChunkCount), pending_(PlanetSurface::ChunkCount) {
    targetRevisions_.fill(0);
    targetDetails_.fill(SurfaceRenderDetail::Full);
    editInfluenceCounts_.fill(0);
}

PlanetSurfaceRenderer::~PlanetSurfaceRenderer() {
    invalidate();
}

int PlanetSurfaceRenderer::slotIndex(int face, int cu, int cv, int cr) {
    return cu + PlanetSurface::ChunksPerFaceAxis *
           (cv + PlanetSurface::ChunksPerFaceAxis *
           (cr + PlanetSurface::RadialChunks * face));
}

PlanetChunkAddress PlanetSurfaceRenderer::slotAddress(int slot) {
    const int chunksPerRadial = PlanetSurface::ChunksPerFaceAxis * PlanetSurface::ChunksPerFaceAxis;
    const int chunksPerFace = chunksPerRadial * PlanetSurface::RadialChunks;
    const int face = slot / chunksPerFace;
    int rem = slot - face * chunksPerFace;
    const int cr = rem / chunksPerRadial;
    rem -= cr * chunksPerRadial;
    const int cv = rem / PlanetSurface::ChunksPerFaceAxis;
    const int cu = rem - cv * PlanetSurface::ChunksPerFaceAxis;
    return {static_cast<CubeFace>(face),cu,cv,cr};
}

Vec3 PlanetSurfaceRenderer::chunkCenterDirection(const PlanetChunkAddress& address) {
    const int u = address.u * PlanetSurface::ChunkSize + PlanetSurface::ChunkSize/2;
    const int v = address.v * PlanetSurface::ChunkSize + PlanetSurface::ChunkSize/2;
    return faceGridCellDirection(address.face,
                                 std::clamp(u,0,PlanetSurface::FaceResolution-1),
                                 std::clamp(v,0,PlanetSurface::FaceResolution-1),
                                 PlanetSurface::FaceResolution);
}

void PlanetSurfaceRenderer::invalidate() {
    if (orbitalClimateHandle_) graphics_.destroyMesh(orbitalClimateHandle_);
    if (orbitalCloudHandle_) graphics_.destroyMesh(orbitalCloudHandle_);
    orbitalClimateHandle_={};
    orbitalCloudHandle_={};
    orbitalSeed_=0;
    orbitalRadius_=0.0f;
    orbitalClimateQuads_=orbitalCloudQuads_=0;
    chunkCache_.clear();
    for (auto& c : chunks_) {
        if (c.handle) graphics_.destroyMesh(c.handle);
        c = {};
    }
    // Futures cannot be force-cancelled; dropping them is safe because workers
    // own immutable snapshots and publication is guarded by target revision +
    // detail. This mirrors stale generation-job rejection elsewhere.
    for (auto& p : pending_) p.reset();
    latestSnapshot_.reset();
    targetRevisions_.fill(0);
    targetDetails_.fill(streaming_ ? SurfaceRenderDetail::FieldFar : SurfaceRenderDetail::Full);
    editInfluenceCounts_.fill(0);
    snapshotRevision_ = 0;
    detailTargetsDirty_ = true;
    rebuiltChunksLastSync_ = 0;
    quads_ = triangles_ = 0;
}

void PlanetSurfaceRenderer::setStreamingFocus(Vec3 planetLocalPosition, int highDetailBudget, int nearFieldBudget) {
    orbitalShellOnly_=false;
    const Vec3 nextDir = lengthSq(planetLocalPosition) > 0.001f ? normalize(planetLocalPosition) : Vec3{0,0,1};
    highDetailBudget = std::clamp(highDetailBudget,0,PlanetSurface::ChunkCount);
    nearFieldBudget = std::clamp(nearFieldBudget,0,PlanetSurface::ChunkCount-highDetailBudget);
    const bool changed = !streaming_ || highDetailBudget_ != highDetailBudget || nearFieldBudget_ != nearFieldBudget || dot(nextDir,focusDirection_) < 0.9995f;
    streaming_ = true;
    highDetailBudget_ = highDetailBudget;
    nearFieldBudget_ = nearFieldBudget;
    focusDirection_ = nextDir;
    if (changed) detailTargetsDirty_ = true;
}

void PlanetSurfaceRenderer::setFullDetail() {
    orbitalShellOnly_=false;
    if (!streaming_ && !detailTargetsDirty_) return;
    streaming_ = false;
    detailTargetsDirty_ = true;
}


void PlanetSurfaceRenderer::setOrbitalShellOnly() {
    orbitalShellOnly_=true;
}

void PlanetSurfaceRenderer::updateDetailTargets() {
    if (!detailTargetsDirty_) return;
    detailTargetsDirty_ = false;

    if (!streaming_) {
        targetDetails_.fill(SurfaceRenderDetail::Full);
        return;
    }

    targetDetails_.fill(SurfaceRenderDetail::FieldFar);
    std::vector<std::pair<float,int>> ranked;
    ranked.reserve(PlanetSurface::ChunkCount);
    for (int slot=0;slot<PlanetSurface::ChunkCount;++slot) {
        const Vec3 d = chunkCenterDirection(slotAddress(slot));
        ranked.emplace_back(dot(d,focusDirection_),slot);
    }
    std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    const int fullCount = std::min(highDetailBudget_,static_cast<int>(ranked.size()));
    for (int i=0;i<fullCount;++i)
        targetDetails_[static_cast<std::size_t>(ranked[static_cast<std::size_t>(i)].second)] = SurfaceRenderDetail::Full;
    const int nearEnd = std::min(fullCount+nearFieldBudget_,static_cast<int>(ranked.size()));
    for (int i=fullCount;i<nearEnd;++i)
        targetDetails_[static_cast<std::size_t>(ranked[static_cast<std::size_t>(i)].second)] = SurfaceRenderDetail::FieldNear;

    // Player-authored distant meaning is now preserved inside the field mesher
    // with localized adaptive tiles. Whole chunks no longer need promotion just
    // because one column contains an edit.
}

void PlanetSurfaceRenderer::updateTargets(const PlanetSurface& planet) {
    if (snapshotRevision_ != planet.revision()) {
        snapshotRevision_ = planet.revision();
        latestSnapshot_ = std::make_shared<PlanetSurfaceSnapshot>(planet.snapshot());
        for (int slot=0; slot<PlanetSurface::ChunkCount; ++slot) {
            const auto address = slotAddress(slot);
            targetRevisions_[static_cast<std::size_t>(slot)] = planet.chunkRevision(address);
            editInfluenceCounts_[static_cast<std::size_t>(slot)] = planet.chunkEditCount(address);
        }
        detailTargetsDirty_ = true;
    }
    updateDetailTargets();
}

void PlanetSurfaceRenderer::sync(const PlanetSurface& planet) {
    rebuiltChunksLastSync_ = 0;
    updateTargets(planet);
    updateCpuResidency(planet);
    if (orbitalShellOnly_) {
        ensureOrbitalShell(planet);
        return;
    }
    processCompleted();
    scheduleMissing();
    processCompleted();
    chunkCache_.sync(planet);
}

void PlanetSurfaceRenderer::draw() const {
    for (const auto& c : chunks_) if (c.handle) graphics_.drawMesh(c.handle);
}

void PlanetSurfaceRenderer::drawOrbitalShell() const {
    if (orbitalClimateHandle_) graphics_.drawMesh(orbitalClimateHandle_);
    if (orbitalCloudHandle_) graphics_.drawMesh(orbitalCloudHandle_);
}

bool PlanetSurfaceRenderer::ready() const {
    if (orbitalShellOnly_) return static_cast<bool>(orbitalClimateHandle_);
    if (chunks_.empty()) return false;
    for (int slot=0; slot<PlanetSurface::ChunkCount; ++slot) {
        const auto& c=chunks_[static_cast<std::size_t>(slot)];
        if (!c.initialized || c.builtRevision != targetRevisions_[static_cast<std::size_t>(slot)] ||
            c.detail != targetDetails_[static_cast<std::size_t>(slot)]) return false;
    }
    return true;
}

int PlanetSurfaceRenderer::pendingJobs() const {
    int count=0;
    for (const auto& p : pending_) if (p) ++count;
    count += chunkCache_.stats().pending;
    return count;
}

int PlanetSurfaceRenderer::dirtyChunks() const {
    if (orbitalShellOnly_) return 0;
    int count=0;
    for (int slot=0; slot<PlanetSurface::ChunkCount; ++slot) {
        const auto& c=chunks_[static_cast<std::size_t>(slot)];
        if (!c.initialized || c.builtRevision != targetRevisions_[static_cast<std::size_t>(slot)] ||
            c.detail != targetDetails_[static_cast<std::size_t>(slot)]) ++count;
    }
    return count;
}

int PlanetSurfaceRenderer::fullDetailChunks() const {
    int n=0;
    for (const auto& c:chunks_) if (c.initialized && c.detail==SurfaceRenderDetail::Full) ++n;
    return n;
}

int PlanetSurfaceRenderer::fieldDetailChunks() const {
    int n=0;
    for (const auto& c:chunks_) if (c.initialized && c.detail!=SurfaceRenderDetail::Full) ++n;
    return n;
}

int PlanetSurfaceRenderer::nearFieldChunks() const {
    int n=0;
    for (const auto& c:chunks_) if (c.initialized && c.detail==SurfaceRenderDetail::FieldNear) ++n;
    return n;
}

int PlanetSurfaceRenderer::farFieldChunks() const {
    int n=0;
    for (const auto& c:chunks_) if (c.initialized && c.detail==SurfaceRenderDetail::FieldFar) ++n;
    return n;
}

int PlanetSurfaceRenderer::targetFullDetailChunks() const {
    int n=0;
    for (auto d:targetDetails_) if (d==SurfaceRenderDetail::Full) ++n;
    return n;
}

int PlanetSurfaceRenderer::targetNearFieldChunks() const {
    int n=0;
    for (auto d:targetDetails_) if (d==SurfaceRenderDetail::FieldNear) ++n;
    return n;
}

void PlanetSurfaceRenderer::processCompleted() {
    using namespace std::chrono_literals;
    for (std::size_t i=0; i<pending_.size(); ++i) {
        auto& pending = pending_[i];
        if (!pending) continue;
        if (pending->wait_for(0ms) != std::future_status::ready) continue;
        BuildResult result = pending->get();
        pending.reset();
        if (result.slot < 0 || result.slot >= PlanetSurface::ChunkCount) continue;
        const std::size_t slot=static_cast<std::size_t>(result.slot);
        if (result.revision != targetRevisions_[slot] || result.detail != targetDetails_[slot]) continue;
        publish(result.slot,std::move(result.mesh),result.revision,result.detail);
        ++rebuiltChunksLastSync_;
    }
}

void PlanetSurfaceRenderer::scheduleMissing() {
    if (!latestSnapshot_) return;
    for (int slot=0; slot<PlanetSurface::ChunkCount; ++slot) {
        const std::size_t i=static_cast<std::size_t>(slot);
        const auto targetRevision = targetRevisions_[i];
        const auto targetDetail = targetDetails_[i];
        const auto& current=chunks_[i];
        if (current.initialized && current.builtRevision == targetRevision && current.detail == targetDetail) continue;
        if (pending_[i]) continue;
        const auto address = slotAddress(slot);
        if (targetDetail==SurfaceRenderDetail::Full) {
            // v0.11: LOD0 mesh workers consume the reconstructed bounded cache
            // packet directly. If generation is still in flight, keep the old
            // GPU packet and wait rather than regenerating from a global snapshot.
            const auto chunkData=chunkCache_.find(address);
            if (!chunkData || chunkData->revision!=targetRevision) continue;
            pending_[i] = jobs_.submit([chunkData,slot,targetRevision,targetDetail]() {
                BuildResult result{};
                result.slot=slot;
                result.revision=targetRevision;
                result.detail=targetDetail;
                result.mesh=buildPlanetSurfaceChunkMesh(*chunkData);
                return result;
            });
        } else {
            const auto snapshot=latestSnapshot_;
            pending_[i] = jobs_.submit([snapshot,address,slot,targetRevision,targetDetail]() {
                BuildResult result{};
                result.slot=slot;
                result.revision=targetRevision;
                result.detail=targetDetail;
                const int step = targetDetail==SurfaceRenderDetail::FieldNear ? 4 : 8;
                result.mesh=buildPlanetSurfaceFieldChunkMesh(*snapshot,address,step,true,0.75f,2);
                return result;
            });
        }
    }
}

void PlanetSurfaceRenderer::publish(int slot, CpuMeshData&& mesh, std::uint64_t revision, SurfaceRenderDetail detail) {
    auto& target = chunks_[static_cast<std::size_t>(slot)];
    if (target.handle) graphics_.destroyMesh(target.handle);
    target.handle={};
    target.builtRevision=revision;
    target.detail=detail;
    target.initialized=true;
    target.quads=mesh.quads;
    target.triangles=mesh.triangleCount();
    if (!mesh.empty()) target.handle=graphics_.uploadMesh(mesh);
    recalcStats();
}

void PlanetSurfaceRenderer::recalcStats() {
    quads_=triangles_=0;
    for (const auto& c : chunks_) {
        quads_ += c.quads;
        triangles_ += c.triangles;
    }
}

void PlanetSurfaceRenderer::updateCpuResidency(const PlanetSurface& planet) {
    chunkCache_.beginFrame();
    if (!orbitalShellOnly_) {
        std::vector<PlanetChunkAddress> full;
        full.reserve(static_cast<std::size_t>(highDetailBudget_));
        for (int slot=0;slot<PlanetSurface::ChunkCount;++slot) {
            const auto detail=targetDetails_[static_cast<std::size_t>(slot)];
            if (detail!=SurfaceRenderDetail::Full) continue;
            const auto address=slotAddress(slot);
            full.push_back(address);
            const auto& current=chunks_[static_cast<std::size_t>(slot)];
            const auto priority=(current.initialized && current.builtRevision!=targetRevisions_[static_cast<std::size_t>(slot)])
                ? SurfaceChunkPriority::EditRemesh : SurfaceChunkPriority::Visible;
            chunkCache_.request(planet,address,priority);
        }

        if (streaming_) {
        // Seam-aware look-ahead: prefetch tangential neighbors of the current
        // LOD0 set, including neighbors whose ownership wraps onto another cube
        // face. This avoids slot-order bias and makes seam crossing use the same
        // deterministic request path as ordinary intra-face movement.
        std::vector<PlanetChunkAddress> candidates;
        candidates.reserve(full.size()*4U);
        auto addNeighbor=[&](const PlanetChunkAddress& a,int du,int dv) {
            const int u0=a.u*PlanetSurface::ChunkSize;
            const int v0=a.v*PlanetSurface::ChunkSize;
            int sampleU=u0+PlanetSurface::ChunkSize/2;
            int sampleV=v0+PlanetSurface::ChunkSize/2;
            if(du>0) sampleU=u0+PlanetSurface::ChunkSize;
            if(du<0) sampleU=u0-1;
            if(dv>0) sampleV=v0+PlanetSurface::ChunkSize;
            if(dv<0) sampleV=v0-1;
            const auto wrapped=wrapFaceCell(a.face,sampleU,sampleV,PlanetSurface::FaceResolution);
            const auto n=chunkAddress(wrapped.face,wrapped.u,wrapped.v,a.radial*PlanetSurface::ChunkSize,PlanetSurface::ChunkSize);
            if (n.radial<0 || n.radial>=PlanetSurface::RadialChunks) return;
            const auto key=stableChunkKey(n);
            if(std::none_of(candidates.begin(),candidates.end(),[&](const auto& e){return stableChunkKey(e)==key;}))
                candidates.push_back(n);
        };
        for(const auto& a:full) {
            addNeighbor(a,1,0); addNeighbor(a,-1,0); addNeighbor(a,0,1); addNeighbor(a,0,-1);
        }
        std::sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b) {
            const float da=dot(chunkCenterDirection(a),focusDirection_);
            const float db=dot(chunkCenterDirection(b),focusDirection_);
            if(da!=db) return da>db;
            return stableChunkKey(a)<stableChunkKey(b);
        });
        int prefetched=0;
        for(const auto& a:candidates) {
            const int slot=slotIndex(static_cast<int>(a.face),a.u,a.v,a.radial);
            if(slot<0 || slot>=PlanetSurface::ChunkCount) continue;
            if(targetDetails_[static_cast<std::size_t>(slot)]==SurfaceRenderDetail::Full) continue;
            if(chunkCache_.request(planet,a,SurfaceChunkPriority::Prefetch)) {
                if(++prefetched>=4) break;
            }
        }
        }
    }
    chunkCache_.cancelUnwanted();
    chunkCache_.sync(planet);
}

void PlanetSurfaceRenderer::ensureOrbitalShell(const PlanetSurface& planet) {
    if (orbitalClimateHandle_ && orbitalSeed_==planet.seed() && orbitalClass_==planet.planetClass() &&
        std::abs(orbitalRadius_-planet.referenceRadius())<0.0001f) return;
    if (orbitalClimateHandle_) graphics_.destroyMesh(orbitalClimateHandle_);
    if (orbitalCloudHandle_) graphics_.destroyMesh(orbitalCloudHandle_);
    orbitalClimateHandle_={};
    orbitalCloudHandle_={};
    const auto snapshot=planet.snapshot();
    auto climate=buildPlanetOrbitalClimateShellMesh(snapshot,8,0.12f);
    auto clouds=buildPlanetOrbitalCloudShellMesh(snapshot,8,2.0f);
    orbitalClimateQuads_=climate.quads;
    orbitalCloudQuads_=clouds.quads;
    if (!climate.empty()) orbitalClimateHandle_=graphics_.uploadMesh(climate);
    if (!clouds.empty()) orbitalCloudHandle_=graphics_.uploadMesh(clouds);
    orbitalSeed_=planet.seed();
    orbitalClass_=planet.planetClass();
    orbitalRadius_=planet.referenceRadius();
}

} // namespace elysium
