#include "world/SurfaceChunkCache.hpp"

#include "world/ProceduralMicro.hpp"

#include "world/Block.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace elysium {
namespace {

SurfaceCellAddress normalizeCell(SurfaceCellAddress a) {
    if (a.u >= 0 && a.u < PlanetSurface::FaceResolution &&
        a.v >= 0 && a.v < PlanetSurface::FaceResolution) return a;
    const auto wrapped=wrapFaceCell(a.face,a.u,a.v,PlanetSurface::FaceResolution);
    a.face=wrapped.face;
    a.u=wrapped.u;
    a.v=wrapped.v;
    return a;
}

} // namespace

int SurfaceChunkData::haloIndex(int u, int v, int radial) {
    const int hu=u+Halo, hv=v+Halo, hr=radial+Halo;
    if (hu<0 || hu>=HaloSize || hv<0 || hv>=HaloSize || hr<0 || hr>=HaloSize) return -1;
    return hr + HaloSize * (hu + HaloSize * hv);
}

BlockType SurfaceChunkData::getLocal(int u, int v, int radial) const {
    if (u < 0 || u >= CoreSize || v < 0 || v >= CoreSize || radial < 0 || radial >= CoreSize)
        return BlockType::Air;
    return getWithHalo(u,v,radial);
}

BlockType SurfaceChunkData::getWithHalo(int u, int v, int radial) const {
    const int hu=u+Halo, hv=v+Halo, hr=radial+Halo;
    if (hu<0 || hu>=HaloSize || hv<0 || hv>=HaloSize || hr<0 || hr>=HaloSize) return BlockType::Air;
    if (voxels.extent() != HaloSize) return BlockType::Air;
    return voxels.get(hu, hv, hr);
}

SurfaceCellAddress SurfaceChunkData::worldAddress(int localU, int localV, int localRadial) const {
    SurfaceCellAddress a{address.face,
        address.u*CoreSize + localU,
        address.v*CoreSize + localV,
        address.radial*CoreSize + localRadial};
    return normalizeCell(a);
}

int SurfaceChunkData::flatCellIndex(SurfaceCellAddress a) {
    a=normalizeCell(a);
    return a.radial + PlanetSurface::RadialLayers * (a.u + PlanetSurface::FaceResolution *
        (a.v + PlanetSurface::FaceResolution * static_cast<int>(a.face)));
}

bool SurfaceChunkData::isRefined(SurfaceCellAddress a) const {
    if (a.radial<0 || a.radial>=PlanetSurface::RadialLayers) return false;
    return microBricks.contains(flatCellIndex(a));
}

const MicroBrick* SurfaceChunkData::microBrick(SurfaceCellAddress a) const {
    if (a.radial<0 || a.radial>=PlanetSurface::RadialLayers) return nullptr;
    const auto it=microBricks.find(flatCellIndex(a));
    return it==microBricks.end()?nullptr:&it->second;
}

bool SurfaceChunkData::localCoordsFor(SurfaceCellAddress a, int& lu, int& lv, int& lr) const {
    a = normalizeCell(a);
    const int u0 = address.u * CoreSize;
    const int v0 = address.v * CoreSize;
    const int r0 = address.radial * CoreSize;
    lr = a.radial - r0;
    if (lr < -Halo || lr > CoreSize) return false;

    if (a.face == address.face) {
        lu = a.u - u0;
        lv = a.v - v0;
        return lu >= -Halo && lu <= CoreSize && lv >= -Halo && lv <= CoreSize;
    }

    // Cross-face halo samples only appear on the U/V ring (not the core interior).
    for (int candV = -Halo; candV <= CoreSize; ++candV) {
        for (int candU = -Halo; candU <= CoreSize; ++candU) {
            if (candU >= 0 && candU < CoreSize && candV >= 0 && candV < CoreSize) continue;
            const auto w = worldAddress(candU, candV, lr);
            if (w.face == a.face && w.u == a.u && w.v == a.v && w.radial == a.radial) {
                lu = candU;
                lv = candV;
                return true;
            }
        }
    }
    return false;
}

BlockType SurfaceChunkData::microGetLocal(int lu, int lv, int lr, int mu, int mr, int mv) const {
    // Hot path: AO / boundary probes hammer the same macro cell. Hoist column
    // heights + slope (+ brick lookup) across consecutive queries on one cell.
    struct CellProbeCache {
        const SurfaceChunkData* self{};
        int lu{0}, lv{0}, lr{0};
        bool ready{};
        BlockType macro{BlockType::Air};
        SurfaceCellAddress addr{};
        const MicroBrick* brick{};
        bool tombstone{};
        bool procedural{};
        int surface{};
        int slope{};
        int sUPos{};
        int sUNeg{};
        int sVPos{};
        int sVNeg{};
    };
    thread_local CellProbeCache cache;

    if (!cache.ready || cache.self != this || cache.lu != lu || cache.lv != lv || cache.lr != lr) {
        cache.self = this;
        cache.lu = lu;
        cache.lv = lv;
        cache.lr = lr;
        cache.macro = getWithHalo(lu, lv, lr);
        cache.addr = worldAddress(lu, lv, lr);
        cache.brick = nullptr;
        cache.tombstone = false;
        cache.procedural = false;
        cache.ready = true;
        if (cache.addr.radial < 0 || cache.addr.radial >= PlanetSurface::RadialLayers) {
            // Keep ready so radial OOB repeats are cheap; answered below.
        } else {
            cache.brick = microBrick(cache.addr);
            cache.tombstone = cache.brick && cache.brick->overrideCount() == 0;
            auto columnRadial = [&](CubeFace face, int u, int v) {
                const auto n = normalizeCell({face, u, v, 0});
                const int idx = n.u + PlanetSurface::FaceResolution *
                    (n.v + PlanetSurface::FaceResolution * static_cast<int>(n.face));
                return static_cast<int>(generatedSurfaceRadials[static_cast<std::size_t>(idx)]);
            };
            cache.surface = columnRadial(cache.addr.face, cache.addr.u, cache.addr.v);
            cache.sUPos = columnRadial(cache.addr.face, cache.addr.u + 1, cache.addr.v);
            cache.sUNeg = columnRadial(cache.addr.face, cache.addr.u - 1, cache.addr.v);
            cache.sVPos = columnRadial(cache.addr.face, cache.addr.u, cache.addr.v + 1);
            cache.sVNeg = columnRadial(cache.addr.face, cache.addr.u, cache.addr.v - 1);
            cache.slope = proceduralSlopeFromNeighbors(
                cache.surface, cache.sUPos, cache.sUNeg, cache.sVPos, cache.sVNeg);
            cache.procedural = !cache.tombstone &&
                proceduralMicroActive(planetClass, cache.addr.radial, cache.surface, cache.slope);
        }
    }

    if (cache.addr.radial < 0) return BlockType::Stone;
    if (cache.addr.radial >= PlanetSurface::RadialLayers) return BlockType::Air;

    if (cache.brick) {
        const int idx = MicroBrick::index(mu, mr, mv);
        if (cache.brick->hasOverride(idx)) return cache.brick->getIndex(idx);
        if (cache.tombstone) return cache.macro;
    }
    if (!cache.procedural) return cache.macro;
    return proceduralMicroBlock(seed, planetClass, cache.addr.face, cache.addr.u, cache.addr.v,
                                cache.addr.radial, cache.surface, cache.slope,
                                cache.sUPos, cache.sUNeg, cache.sVPos, cache.sVNeg,
                                mu, mr, mv, cache.macro);
}

void SurfaceChunkData::sampleMicroBrickLocal(int lu, int lv, int lr,
                                             std::array<BlockType, MicroBrick::CellCount>& out) const {
    const BlockType macro = getWithHalo(lu, lv, lr);
    const SurfaceCellAddress a = worldAddress(lu, lv, lr);
    if (a.radial < 0) {
        out.fill(BlockType::Stone);
        return;
    }
    if (a.radial >= PlanetSurface::RadialLayers) {
        out.fill(BlockType::Air);
        return;
    }

    const MicroBrick* brick = microBrick(a);
    const bool tombstone = brick && brick->overrideCount() == 0;

    auto columnRadial = [&](CubeFace face, int u, int v) {
        const auto n = normalizeCell({face, u, v, 0});
        const int idx = n.u + PlanetSurface::FaceResolution *
            (n.v + PlanetSurface::FaceResolution * static_cast<int>(n.face));
        return static_cast<int>(generatedSurfaceRadials[static_cast<std::size_t>(idx)]);
    };
    const int surface = columnRadial(a.face, a.u, a.v);
    const int sUPos = columnRadial(a.face, a.u + 1, a.v);
    const int sUNeg = columnRadial(a.face, a.u - 1, a.v);
    const int sVPos = columnRadial(a.face, a.u, a.v + 1);
    const int sVNeg = columnRadial(a.face, a.u, a.v - 1);
    const int slope = proceduralSlopeFromNeighbors(surface, sUPos, sUNeg, sVPos, sVNeg);
    const bool procedural = !tombstone &&
        proceduralMicroActive(planetClass, a.radial, surface, slope);

    for (int i = 0; i < MicroBrick::CellCount; ++i) {
        if (brick && brick->hasOverride(i)) {
            out[static_cast<std::size_t>(i)] = brick->getIndex(i);
            continue;
        }
        if (!procedural) {
            out[static_cast<std::size_t>(i)] = macro;
            continue;
        }
        const int mu = i % MicroBrick::Resolution;
        const int t = i / MicroBrick::Resolution;
        const int mv = t % MicroBrick::Resolution;
        const int mr = t / MicroBrick::Resolution;
        out[static_cast<std::size_t>(i)] = proceduralMicroBlock(
            seed, planetClass, a.face, a.u, a.v, a.radial,
            surface, slope, sUPos, sUNeg, sVPos, sVNeg,
            mu, mr, mv, macro);
    }
}

BlockType SurfaceChunkData::microGet(SurfaceCellAddress a, int mu, int mr, int mv) const {
    if (a.radial<0) return BlockType::Stone;
    if (a.radial>=PlanetSurface::RadialLayers) return BlockType::Air;
    a=normalizeCell(a);

    int lu = 0, lv = 0, lr = 0;
    if (localCoordsFor(a, lu, lv, lr))
        return microGetLocal(lu, lv, lr, mu, mr, mv);

    // Outside this packet's halo: fall back to regenerable procedural with an
    // unknown macro baseline (Air). Callers that need seam-correct macros must
    // query through microGetLocal / in-halo addresses.
    BlockType macro = BlockType::Air;
    if (const auto* brick=microBrick(a)) {
        const int idx = MicroBrick::index(mu, mr, mv);
        if (brick->hasOverride(idx)) return brick->getIndex(idx);
        if (brick->overrideCount() == 0) return macro;
    }

    auto columnRadial = [&](CubeFace face, int u, int v) {
        const auto n = normalizeCell({face, u, v, 0});
        const int idx = n.u + PlanetSurface::FaceResolution *
            (n.v + PlanetSurface::FaceResolution * static_cast<int>(n.face));
        return static_cast<int>(generatedSurfaceRadials[static_cast<std::size_t>(idx)]);
    };
    const int surface = columnRadial(a.face, a.u, a.v);
    const int sUPos = columnRadial(a.face, a.u + 1, a.v);
    const int sUNeg = columnRadial(a.face, a.u - 1, a.v);
    const int sVPos = columnRadial(a.face, a.u, a.v + 1);
    const int sVNeg = columnRadial(a.face, a.u, a.v - 1);
    const int slope = proceduralSlopeFromNeighbors(surface, sUPos, sUNeg, sVPos, sVNeg);
    return proceduralMicroBlock(seed, planetClass, a.face, a.u, a.v, a.radial,
                                surface, slope, sUPos, sUNeg, sVPos, sVNeg,
                                mu, mr, mv, macro);
}

bool SurfaceChunkData::hasMicroDetail(SurfaceCellAddress a) const {
    if (a.radial<0 || a.radial>=PlanetSurface::RadialLayers) return false;
    a=normalizeCell(a);
    if (isRefined(a)) return true;
    auto columnRadial = [&](CubeFace face, int u, int v) {
        const auto n = normalizeCell({face, u, v, 0});
        const int idx = n.u + PlanetSurface::FaceResolution *
            (n.v + PlanetSurface::FaceResolution * static_cast<int>(n.face));
        return static_cast<int>(generatedSurfaceRadials[static_cast<std::size_t>(idx)]);
    };
    const int surface = columnRadial(a.face, a.u, a.v);
    const int slope = proceduralSlopeFromNeighbors(
        surface,
        columnRadial(a.face, a.u + 1, a.v),
        columnRadial(a.face, a.u - 1, a.v),
        columnRadial(a.face, a.u, a.v + 1),
        columnRadial(a.face, a.u, a.v - 1));
    return proceduralMicroActive(planetClass, a.radial, surface, slope);
}


void SurfaceChunkData::recomputeOccupancy() {
    columnMinR.fill(kColumnEmpty);
    columnMaxR.fill(kColumnEmpty);

    // Broad-phase occupancy: solid macros are occupied without scanning 16^3.
    // Only air/non-solid macros with hasMicroDetail (e.g. depth -1 rubble) need
    // a micro probe — and that probe early-outs on the first solid microcell.
    auto coreOccupied = [this](int lu, int lv, int lr) -> BlockType {
        const BlockType macro = getLocal(lu, lv, lr);
        if (blockProperties(macro).solid) return macro;
        const SurfaceCellAddress a = worldAddress(lu, lv, lr);
        if (!hasMicroDetail(a)) return macro;
        // Densify once (hoisted columns) instead of 4096× microGetLocal setup.
        std::array<BlockType, MicroBrick::CellCount> cells{};
        sampleMicroBrickLocal(lu, lv, lr, cells);
        for (const BlockType t : cells)
            if (blockProperties(t).solid) return BlockType::Stone;
        return BlockType::Air;
    };

    // Single pass: adaptive summary + column extremities (avoid double micro scans).
    ChunkOccupancyExtents extents;
    int solidCount = 0;
    int distinctSolid = 0;
    BlockType firstSolid = BlockType::Air;
    bool seenSolid[256]{};
    bool allIdentical = true;
    BlockType firstCell = BlockType::Air;
    bool haveFirst = false;
    const int cellCount = CoreSize * CoreSize * CoreSize;

    for (int lv = 0; lv < CoreSize; ++lv) {
        for (int lu = 0; lu < CoreSize; ++lu) {
            const int col = lu + CoreSize * lv;
            for (int lr = 0; lr < CoreSize; ++lr) {
                const BlockType type = coreOccupied(lu, lv, lr);
                if (!haveFirst) {
                    firstCell = type;
                    haveFirst = true;
                } else if (type != firstCell) {
                    allIdentical = false;
                }
                if (!blockProperties(type).solid) continue;
                ++solidCount;
                extents.include(lu, lv, lr);
                const auto raw = static_cast<std::uint8_t>(type);
                if (!seenSolid[raw]) {
                    seenSolid[raw] = true;
                    if (distinctSolid == 0) firstSolid = type;
                    ++distinctSolid;
                }
                if (columnMinR[static_cast<std::size_t>(col)] == kColumnEmpty) {
                    columnMinR[static_cast<std::size_t>(col)] = static_cast<std::uint8_t>(lr);
                    columnMaxR[static_cast<std::size_t>(col)] = static_cast<std::uint8_t>(lr);
                } else {
                    columnMinR[static_cast<std::size_t>(col)] =
                        static_cast<std::uint8_t>(std::min<int>(columnMinR[static_cast<std::size_t>(col)], lr));
                    columnMaxR[static_cast<std::size_t>(col)] =
                        static_cast<std::uint8_t>(std::max<int>(columnMaxR[static_cast<std::size_t>(col)], lr));
                }
            }
        }
    }

    occupancy = makeChunkAdaptiveSummary(cellCount, solidCount, distinctSolid, firstSolid,
                                         allIdentical && haveFirst, firstCell, extents);
}

std::size_t SurfaceChunkData::estimatedBytes() const {
    std::size_t bytes=voxels.estimatedBytes();
    for (const auto& [_,brick] : microBricks) {
        bytes += sizeof(int) + sizeof(MicroBrick) + brick.overrideCount() * (sizeof(int)+sizeof(BlockType));
    }
    return bytes;
}

SurfaceChunkCache::SurfaceChunkCache(JobSystem& jobs, std::size_t maxResidentChunks,
                                     std::size_t maxResidentBytes, std::size_t maxPending)
    : jobs_(jobs),
      maxResidentChunks_(std::max<std::size_t>(1,maxResidentChunks)),
      maxResidentBytes_(std::max<std::size_t>(SurfaceChunkData::HaloSize*SurfaceChunkData::HaloSize*SurfaceChunkData::HaloSize,maxResidentBytes)),
      maxPending_(std::max<std::size_t>(1,maxPending)) {}

void SurfaceChunkCache::beginFrame() {
    ++clock_;
    for (auto& [_,entry] : entries_) {
        entry.wanted=false;
        entry.priority=SurfaceChunkPriority::Prefetch;
    }
}

std::size_t SurfaceChunkCache::pendingCount() const {
    std::size_t count=0;
    for (const auto& [_,entry] : entries_) if (entry.pending) ++count;
    return count;
}

std::shared_ptr<SurfaceChunkData> SurfaceChunkCache::buildChunk(const PlanetSurfaceSnapshot& snapshot,
                                                                const PlanetChunkAddress& address,
                                                                std::uint64_t revision) {
    auto out=std::make_shared<SurfaceChunkData>();
    out->address=address;
    out->revision=revision;
    out->referenceRadius=snapshot.referenceRadius;
    out->seed=snapshot.seed;
    out->planetClass=snapshot.planetClass;
    out->generatedSurfaceRadials=snapshot.generatedSurfaceRadials;
    const int haloCells=SurfaceChunkData::HaloSize*SurfaceChunkData::HaloSize*SurfaceChunkData::HaloSize;
    std::vector<BlockType> dense(static_cast<std::size_t>(haloCells), BlockType::Air);

    const int u0=address.u*PlanetSurface::ChunkSize;
    const int v0=address.v*PlanetSurface::ChunkSize;
    const int r0=address.radial*PlanetSurface::ChunkSize;
    for (int lv=-SurfaceChunkData::Halo;lv<=PlanetSurface::ChunkSize;++lv) {
        for (int lu=-SurfaceChunkData::Halo;lu<=PlanetSurface::ChunkSize;++lu) {
            for (int lr=-SurfaceChunkData::Halo;lr<=PlanetSurface::ChunkSize;++lr) {
                SurfaceCellAddress world{address.face,u0+lu,v0+lv,r0+lr};
                if (world.radial>=0 && world.radial<PlanetSurface::RadialLayers) world=snapshot.normalize(world);
                const int hu=lu+SurfaceChunkData::Halo;
                const int hv=lv+SurfaceChunkData::Halo;
                const int hr=lr+SurfaceChunkData::Halo;
                const int hi=ChunkVoxelSpans::flatIndex(hu,hv,hr,SurfaceChunkData::HaloSize);
                dense[static_cast<std::size_t>(hi)]=snapshot.get(world);
                if (world.radial>=0 && world.radial<PlanetSurface::RadialLayers) {
                    if (const auto* brick=snapshot.microBrick(world))
                        out->microBricks.try_emplace(snapshot.flatIndex(world.face,world.u,world.v,world.radial),*brick);
                }
            }
        }
    }
    out->voxels=ChunkVoxelSpans::encodeAdaptive(dense.data(), SurfaceChunkData::HaloSize);
    // Occupancy/column extremities must be derived from the reconstructed
    // core+halo packet so LOD0 meshing and broad-phase skip Empty correctly
    // without a second planet snapshot walk (halo contract unchanged).
    out->recomputeOccupancy();
    return out;
}

bool SurfaceChunkCache::tryCancelLowerPriorityPending(SurfaceChunkPriority needed) {
    auto victim=entries_.end();
    int worstUrgency=-1;
    std::uint64_t worstKey=0;
    for (auto it=entries_.begin();it!=entries_.end();++it) {
        auto& e=it->second;
        if (!e.pending) continue;
        const int urgency=static_cast<int>(e.priority);
        if (urgency<=static_cast<int>(needed)) continue; // equal/higher must not be preempted
        if (urgency>worstUrgency || (urgency==worstUrgency && it->first>worstKey)) {
            worstUrgency=urgency;
            worstKey=it->first;
            victim=it;
        }
    }
    if (victim==entries_.end()) return false;
    victim->second.pending.reset();
    ++victim->second.token;
    ++cumulative_.cancellations;
    ++cumulative_.priorityPreemptions;
    return true;
}

bool SurfaceChunkCache::request(const PlanetSurface& planet, const PlanetChunkAddress& address,
                                SurfaceChunkPriority priority) {
    const auto key=stableChunkKey(address);
    auto [it,inserted]=entries_.try_emplace(key);
    Entry& entry=it->second;
    if (inserted) entry.address=address;
    entry.wanted=true;
    entry.lastUse=clock_;
    if (static_cast<int>(priority)<static_cast<int>(entry.priority) || inserted) entry.priority=priority;

    const auto revision=planet.chunkRevision(address);
    if (entry.data && entry.revision==revision) return true;
    if (entry.pending && entry.revision==revision) return true;

    // Hard-bound the pending queue. Replacing this entry's own in-flight job
    // reuses its slot. Otherwise Prefetch is disposable; Visible/EditRemesh may
    // preempt strictly lower-priority work (edit > visible > prefetch).
    const bool replacingOwnPending=entry.pending.has_value();
    if (!replacingOwnPending && pendingCount()>=maxPending_) {
        if (priority==SurfaceChunkPriority::Prefetch) {
            ++cumulative_.droppedPrefetch;
            return false;
        }
        if (!tryCancelLowerPriorityPending(priority)) {
            ++cumulative_.boundedQueueRejects;
            if (priority==SurfaceChunkPriority::Visible) ++cumulative_.droppedVisible;
            return false;
        }
    }

    if (entry.pending) {
        entry.pending.reset();
        ++entry.token;
        ++cumulative_.cancellations;
    }
    // P0-35/P0-55: retain previous valid reconstructed geometry until the new
    // packet is published (or rejected as stale). Callers that require a matching
    // revision (renderer/world-read) already ignore retained stale packets.
    entry.revision=revision;
    const auto token=++entry.token;
    auto snapshot=std::make_shared<PlanetSurfaceSnapshot>(planet.snapshot());
    ++cumulative_.generationRequests;
    entry.pending=jobs_.submit([snapshot,address,revision,token]() {
        BuildResult result{};
        result.address=address;
        result.revision=revision;
        result.token=token;
        result.data=buildChunk(*snapshot,address,revision);
        return result;
    });
    return true;
}

void SurfaceChunkCache::cancelUnwanted() {
    for (auto it=entries_.begin();it!=entries_.end();) {
        auto& entry=it->second;
        if (!entry.wanted && entry.pending) {
            entry.pending.reset();
            ++entry.token;
            ++cumulative_.cancellations;
        }
        if (!entry.wanted && !entry.pending && !entry.data) it=entries_.erase(it);
        else ++it;
    }
}

void SurfaceChunkCache::processCompleted(const PlanetSurface& planet) {
    using namespace std::chrono_literals;
    for (auto& [_,entry] : entries_) {
        if (!entry.pending || entry.pending->wait_for(0ms)!=std::future_status::ready) continue;
        auto result=entry.pending->get();
        entry.pending.reset();
        if (result.token!=entry.token || result.revision!=planet.chunkRevision(result.address)) {
            ++cumulative_.staleResults;
            continue;
        }
        entry.data=std::move(result.data);
        entry.revision=result.revision;
        entry.lastUse=clock_;
        ++cumulative_.published;
    }
}

void SurfaceChunkCache::evictToBudget() {
    auto totals=[this]() {
        std::pair<std::size_t,std::size_t> t{};
        for (const auto& [_,entry] : entries_) if (entry.data) {
            ++t.first;
            t.second+=entry.data->estimatedBytes();
        }
        return t;
    };

    for (;;) {
        auto [count,bytes]=totals();
        if (count<=maxResidentChunks_ && bytes<=maxResidentBytes_) break;
        auto victim=entries_.end();
        std::uint64_t oldest=std::numeric_limits<std::uint64_t>::max();
        for (auto it=entries_.begin();it!=entries_.end();++it) {
            const auto& e=it->second;
            if (!e.data || e.wanted || e.pending) continue;
            if (e.lastUse<oldest) { oldest=e.lastUse; victim=it; }
        }
        if (victim==entries_.end()) break; // all over-budget data is currently requested
        victim->second.data.reset();
        ++cumulative_.evictions;
        if (!victim->second.pending && !victim->second.wanted) entries_.erase(victim);
    }
}

void SurfaceChunkCache::sync(const PlanetSurface& planet) {
    processCompleted(planet);
    evictToBudget();
}

void SurfaceChunkCache::clear() {
    for (auto& [_,entry] : entries_) if (entry.pending) {
        entry.pending.reset();
        ++cumulative_.cancellations;
    }
    entries_.clear();
}

std::shared_ptr<const SurfaceChunkData> SurfaceChunkCache::find(const PlanetChunkAddress& address) const {
    const auto it=entries_.find(stableChunkKey(address));
    if (it==entries_.end()) return {};
    return it->second.data;
}

SurfaceChunkCacheStats SurfaceChunkCache::stats() const {
    auto s=cumulative_;
    for (const auto& [_,entry] : entries_) {
        if (entry.data) {
            ++s.resident;
            s.residentBytes+=entry.data->estimatedBytes();
            if (entry.pending) ++s.retainedWhileRebuilding;
        }
        if (entry.pending) ++s.pending;
        if (entry.wanted) ++s.wanted;
    }
    return s;
}

} // namespace elysium
