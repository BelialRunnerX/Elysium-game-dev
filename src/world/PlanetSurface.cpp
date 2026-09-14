#include "world/PlanetSurface.hpp"

#include "world/ProceduralMicro.hpp"
#include "core/Determinism.hpp"
#include "world/ChunkVoxelSpans.hpp"
#include "world/BiomeCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_set>

namespace elysrum {
namespace {

int faceIndex(CubeFace face) {
    return static_cast<int>(face);
}

int surfaceColumnIndex(CubeFace face, int u, int v) {
    return u + PlanetSurface::FaceResolution * (v + PlanetSurface::FaceResolution * faceIndex(face));
}

// Smooth deterministic field evaluated in shared 3D planetary space. No face
// index participates, so cube ownership changes cannot create a height seam.
// P0-12: memoize SphericalNoiseBasis by (seed, field label) so neighboring
// columns / biomes / materials reuse axes+phases without recomputing hashes.
float sphericalField(std::uint64_t seed, Vec3 d, std::uint64_t label) {
    struct Entry {
        std::uint64_t seed{};
        std::uint64_t label{};
        SphericalNoiseBasis basis{};
    };
    thread_local std::vector<Entry> cache;
    for (const auto& e : cache) {
        if (e.seed == seed && e.label == label) return e.basis.evaluate(d);
    }
    Entry entry{seed, label, SphericalNoiseBasis::make(seed, label)};
    const float value = entry.basis.evaluate(d);
    // Bound the memo: generator uses a small fixed label set.
    if (cache.size() < 24) cache.push_back(std::move(entry));
    else cache[label % cache.size()] = std::move(entry);
    return value;
}


// Multi-band height delta in shared 3D direction space (no face index).
float temperateHeightDelta(std::uint64_t seed, Vec3 d) {
    // Continental / soft ocean mask — bias keeps most of the small prototype
    // shell as land while still carving coasts and inland basins.
    const float continents = sphericalField(seed, d, 0x4C41594FULL); // LAYO
    const float hills = sphericalField(seed, d, 0x48494C4CULL);      // HILL
    const float ridgeSigned = sphericalField(seed, d, 0x52494447ULL); // RIDG
    const float ridges = std::abs(ridgeSigned);
    const float rolling = sphericalField(seed, d, 0x524F4C4CULL);    // ROLL
    const float erode = std::max(0.0f, sphericalField(seed, d, 0x45524F44ULL)); // EROD

    const float landBias = continents * 2.75f + 0.75f;
    const float basin = std::min(0.0f, continents + 0.22f) * 4.6f;
    return landBias
         + hills * 2.65f
         + ridges * 3.75f
         + rolling * 1.45f
         - erode * ridges * 0.85f
         + basin;
}

float barrenHeightDelta(std::uint64_t seed, Vec3 d) {
    const float continents = sphericalField(seed, d, 0x4C41594FULL);
    const float hills = sphericalField(seed, d, 0x48494C4CULL);
    const float ridges = std::abs(sphericalField(seed, d, 0x52494447ULL));
    const float rolling = sphericalField(seed, d, 0x524F4C4CULL);
    return continents * 2.85f + hills * 1.15f + ridges * 2.45f + rolling * 0.55f - 0.65f;
}

float scorchedHeightDelta(std::uint64_t seed, Vec3 d) {
    const float continents = sphericalField(seed, d, 0x4C41594FULL);
    const float ridges = std::abs(sphericalField(seed, d, 0x52494447ULL));
    const float rolling = sphericalField(seed, d, 0x524F4C4CULL);
    const float vents = std::abs(sphericalField(seed, d, 0x56454E54ULL)); // VENT
    return continents * 3.05f + ridges * 3.05f + rolling * 0.85f + vents * 0.7f - 0.25f;
}

int generatedSurfaceRadialFor(std::uint64_t seed, PlanetClass planetClass, CubeFace face, int u, int v) {
    const Vec3 d = faceGridCellDirection(face, u, v, PlanetSurface::FaceResolution);
    float height = 0.0f;
    switch (planetClass) {
        case PlanetClass::Temperate: height = temperateHeightDelta(seed, d); break;
        case PlanetClass::Barren: height = barrenHeightDelta(seed, d); break;
        case PlanetClass::Scorched: height = scorchedHeightDelta(seed, d); break;
        // Catalog classes: distinct profiles; T/B/S formulas unchanged (GeneratorVersion stays).
        case PlanetClass::Frozen: height = barrenHeightDelta(seed, d) * 0.85f - 0.2f; break;
        case PlanetClass::Toxic: height = temperateHeightDelta(seed, d) * 0.75f; break;
        case PlanetClass::Irradiated: height = barrenHeightDelta(seed, d) * 1.05f; break;
        case PlanetClass::Oceanic: height = temperateHeightDelta(seed, d) * 0.55f - 1.4f; break;
        case PlanetClass::Anomalous: height = scorchedHeightDelta(seed, d) * 0.9f + 0.4f; break;
        case PlanetClass::Count: height = temperateHeightDelta(seed, d); break;
    }
    return std::clamp(PlanetSurface::ReferenceRadial + static_cast<int>(std::lround(height)),
                      6, PlanetSurface::RadialLayers - 4);
}

BlockType temperateSurfaceBlock(std::uint64_t seed, Vec3 d, int surface, int depth) {
    const float moisture = sphericalField(seed, d, 0x4D4F4953ULL) * 0.5f + 0.5f; // MOIS
    const float temperature = sphericalField(seed, d, 0x54454D50ULL) * 0.5f + 0.5f; // TEMP
    const float ridges = std::abs(sphericalField(seed, d, 0x52494447ULL));
    const float elev = static_cast<float>(surface - PlanetSurface::ReferenceRadial);
    const bool outcrop = elev >= 4.0f || ridges > 0.58f;

    if (depth == 0) {
        if (outcrop) return BlockType::Stone;
        // Deeper dirt exposure in carved valleys / basins.
        if (elev <= -2.0f) return BlockType::Dirt;
        // Dry scrub / dirt shoulders.
        if (moisture < 0.34f || (temperature > 0.68f && moisture < 0.52f)) return BlockType::Dirt;
        return BlockType::Grass;
    }
    // Rocky columns stay stone; soil mantle elsewhere (deeper in valleys).
    if (outcrop) return BlockType::Stone;
    if (depth <= 2) return BlockType::Dirt;
    if (depth <= 4 && elev <= -1.0f) return BlockType::Dirt;
    return BlockType::Stone;
}

BlockType generatedBlockFor(std::uint64_t seed, PlanetClass planetClass, CubeFace face,
                            int u, int v, int radial, int surface) {
    if (radial > surface) return BlockType::Air;
    if (radial <= 1 && planetClass == PlanetClass::Scorched) return BlockType::Magma;

    const int depth = surface - radial;
    const Vec3 d = faceGridCellDirection(face, u, v, PlanetSurface::FaceResolution);

    if (planetClass == PlanetClass::Temperate) {
        if (depth <= 4) return temperateSurfaceBlock(seed, d, surface, depth);
    } else if (planetClass == PlanetClass::Barren) {
        if (depth == 0) {
            const float ridges = std::abs(sphericalField(seed, d, 0x52494447ULL));
            const float elev = static_cast<float>(surface - PlanetSurface::ReferenceRadial);
            if (elev >= 3.0f || ridges > 0.62f) return BlockType::Stone;
            return BlockType::Regolith;
        }
        if (depth <= 2) return BlockType::Regolith;
    } else if (planetClass == PlanetClass::Scorched) {
        if (depth == 0) {
            // Scorched crust: basalt with occasional stone high points.
            const float elev = static_cast<float>(surface - PlanetSurface::ReferenceRadial);
            if (elev >= 4.0f) return BlockType::Stone;
            return BlockType::Basalt;
        }
        if (depth <= 3) return BlockType::Basalt;
    } else {
        // Catalog-only classes — coarse class defaults until per-biome sampling drives gen.
        if (depth == 0) {
            switch (planetClass) {
                case PlanetClass::Frozen: return BlockType::Stone;
                case PlanetClass::Toxic: return BlockType::Dirt;
                case PlanetClass::Irradiated: return BlockType::Regolith;
                case PlanetClass::Oceanic: return BlockType::Grass;
                case PlanetClass::Anomalous: return BlockType::Stone;
                default: return BlockType::Stone;
            }
        }
        if (depth <= 2) {
            switch (planetClass) {
                case PlanetClass::Toxic: return BlockType::Dirt;
                case PlanetClass::Irradiated: return BlockType::Regolith;
                case PlanetClass::Oceanic: return BlockType::Dirt;
                case PlanetClass::Anomalous: return BlockType::Dirt;
                default: break;
            }
        }
    }

    const std::uint64_t label = 0x4F524553ULL ^ static_cast<std::uint64_t>(faceIndex(face));
    const float ore = hash01(seed, u, radial, v, label);
    if (radial < PlanetSurface::ReferenceRadial - 5 && ore > 0.986f) return BlockType::IronOre;
    if (radial < PlanetSurface::ReferenceRadial - 2 && ore > 0.974f) return BlockType::CopperOre;
    if (radial < PlanetSurface::ReferenceRadial && ore > 0.966f) return BlockType::CoalOre;
    return planetClass == PlanetClass::Scorched ? BlockType::Basalt : BlockType::Stone;
}

BiomeId sampleBiomeImpl(std::uint64_t seed, PlanetClass planetClass, Vec3 direction) {
    const Vec3 d = lengthSq(direction) > 1e-8f ? normalize(direction) : Vec3{0.0f, 1.0f, 0.0f};
    if (planetClass != PlanetClass::Temperate) {
        // Non-temperate classes: default catalog biome (light local variation later).
        return defaultBiomeFor(planetClass);
    }

    const float moisture = sphericalField(seed, d, 0x4D4F4953ULL) * 0.5f + 0.5f;
    const float temperature = sphericalField(seed, d, 0x54454D50ULL) * 0.5f + 0.5f;
    const float continents = sphericalField(seed, d, 0x4C41594FULL);
    const float hills = sphericalField(seed, d, 0x48494C4CULL);
    const float ridges = std::abs(sphericalField(seed, d, 0x52494447ULL));
    const float elev = temperateHeightDelta(seed, d);

    if (elev >= 5.0f || ridges > 0.72f) return BiomeId::TemperateMountain;
    if (elev >= 3.5f || ridges > 0.55f) return BiomeId::TemperateHighland;
    if (elev <= -2.2f || continents < -0.40f) return BiomeId::TemperateRiverValley;
    if (continents < -0.12f && elev < 1.0f) return BiomeId::TemperateCoastalPlains;
    if (moisture > 0.72f && elev < 1.5f) return BiomeId::TemperateWetlandMarsh;
    if (moisture < 0.30f || (temperature > 0.70f && moisture < 0.48f)) return BiomeId::TemperateScrubland;
    if (moisture > 0.62f && temperature < 0.45f) return BiomeId::TemperateBorealFringe;
    if (moisture > 0.58f) return BiomeId::TemperateForest;
    if (std::abs(hills) > 0.45f && elev > 0.8f) return BiomeId::TemperateRollingHills;
    if (ridges > 0.35f && moisture < 0.40f) return BiomeId::TemperateBadlands;
    return BiomeId::TemperateFrontierPlains;
}

int clampCell(float uv, int resolution) {
    const float scaled = (uv + 1.0f) * 0.5f * static_cast<float>(resolution);
    return std::clamp(static_cast<int>(std::floor(scaled)), 0, resolution-1);
}

float uvCellCoord(float uv, int resolution) {
    return (uv + 1.0f) * 0.5f * static_cast<float>(resolution);
}

SurfaceCellAddress normalizeAddress(SurfaceCellAddress address) {
    if (address.u >= 0 && address.u < PlanetSurface::FaceResolution &&
        address.v >= 0 && address.v < PlanetSurface::FaceResolution) return address;

    const float u = ((static_cast<float>(address.u) + 0.5f) /
                     static_cast<float>(PlanetSurface::FaceResolution)) * 2.0f - 1.0f;
    const float v = ((static_cast<float>(address.v) + 0.5f) /
                     static_cast<float>(PlanetSurface::FaceResolution)) * 2.0f - 1.0f;
    const Vec3 direction = faceUvToDirectionUnclamped(address.face,u,v);
    const FaceUv mapped = directionToFaceUv(direction);
    address.face = mapped.face;
    address.u = clampCell(mapped.u, PlanetSurface::FaceResolution);
    address.v = clampCell(mapped.v, PlanetSurface::FaceResolution);
    return address;
}

template <typename RadialArray>
int cachedColumnRadial(const RadialArray& radials, CubeFace face, int u, int v) {
    const auto a = normalizeAddress({face, u, v, 0});
    return static_cast<int>(radials[static_cast<std::size_t>(surfaceColumnIndex(a.face, a.u, a.v))]);
}

template <typename RadialArray>
void neighborColumnRadials(const RadialArray& radials, CubeFace face, int u, int v,
                           int& sUPos, int& sUNeg, int& sVPos, int& sVNeg) {
    sUPos = cachedColumnRadial(radials, face, u + 1, v);
    sUNeg = cachedColumnRadial(radials, face, u - 1, v);
    sVPos = cachedColumnRadial(radials, face, u, v + 1);
    sVNeg = cachedColumnRadial(radials, face, u, v - 1);
}



Vec3 tangentReference(Vec3 up) {
    const Vec3 north{0,1,0};
    Vec3 forward = north - up * dot(north,up);
    if (lengthSq(forward) < 1e-5f) {
        const Vec3 east{0,0,1};
        forward = east - up * dot(east,up);
    }
    return normalize(forward);
}

Vec3 microBoundary(float referenceRadius, SurfaceCellAddress address,
                   int uEdge, int radialEdge, int vEdge) {
    address = normalizeAddress(address);
    const float fu = static_cast<float>(address.u) +
                     static_cast<float>(std::clamp(uEdge,0,MicroBrick::Resolution)) /
                     static_cast<float>(MicroBrick::Resolution);
    const float fv = static_cast<float>(address.v) +
                     static_cast<float>(std::clamp(vEdge,0,MicroBrick::Resolution)) /
                     static_cast<float>(MicroBrick::Resolution);
    const float u = fu / static_cast<float>(PlanetSurface::FaceResolution) * 2.0f - 1.0f;
    const float v = fv / static_cast<float>(PlanetSurface::FaceResolution) * 2.0f - 1.0f;
    const Vec3 d = faceUvToDirection(address.face,u,v);
    const float rr = static_cast<float>(address.radial) +
                     static_cast<float>(std::clamp(radialEdge,0,MicroBrick::Resolution)) /
                     static_cast<float>(MicroBrick::Resolution);
    const float radius = referenceRadius + rr - static_cast<float>(PlanetSurface::ReferenceRadial);
    return d * radius;
}

SurfaceMicroAddress locateMicroCommon(float referenceRadius, Vec3 p) {
    const float radius = length(p);
    const FaceUv uv = directionToFaceUv(p);
    const float uc = uvCellCoord(uv.u,PlanetSurface::FaceResolution);
    const float vc = uvCellCoord(uv.v,PlanetSurface::FaceResolution);
    const int u = std::clamp(static_cast<int>(std::floor(uc)),0,PlanetSurface::FaceResolution-1);
    const int v = std::clamp(static_cast<int>(std::floor(vc)),0,PlanetSurface::FaceResolution-1);
    const float radialCoord = radius - referenceRadius + static_cast<float>(PlanetSurface::ReferenceRadial);
    const int radial = static_cast<int>(std::floor(radialCoord));
    const float uf = std::clamp(uc-static_cast<float>(u),0.0f,0.999999f);
    const float vf = std::clamp(vc-static_cast<float>(v),0.0f,0.999999f);
    const float rf = std::clamp(radialCoord-static_cast<float>(radial),0.0f,0.999999f);
    return {{uv.face,u,v,radial},
            std::clamp(static_cast<int>(std::floor(uf*MicroBrick::Resolution)),0,MicroBrick::Resolution-1),
            std::clamp(static_cast<int>(std::floor(rf*MicroBrick::Resolution)),0,MicroBrick::Resolution-1),
            std::clamp(static_cast<int>(std::floor(vf*MicroBrick::Resolution)),0,MicroBrick::Resolution-1)};
}

} // namespace

BiomeId sampleBiome(std::uint64_t seed, PlanetClass planetClass, Vec3 direction) {
    return sampleBiomeImpl(seed, planetClass, direction);
}

std::size_t SurfaceChunkJournal::microOverrideCount() const {
    std::size_t total=0;
    for (const auto& [_,brick] : microBricks) total += brick.overrideCount();
    return total;
}

int PlanetSurfaceSnapshot::flatIndex(CubeFace face, int u, int v, int radial) const {
    return radial + RadialLayers * (u + FaceResolution * (v + FaceResolution * faceIndex(face)));
}

bool PlanetSurfaceSnapshot::radialInBounds(int radial) const {
    return radial >= 0 && radial < RadialLayers;
}

SurfaceCellAddress PlanetSurfaceSnapshot::normalize(SurfaceCellAddress address) const {
    return normalizeAddress(address);
}

BlockType PlanetSurfaceSnapshot::get(CubeFace face, int u, int v, int radial) const {
    if (radial < 0) return BlockType::Stone; // mantle-side continuation is solid
    if (radial >= RadialLayers) return BlockType::Air;
    const auto a = normalize({face,u,v,radial});
    const int idx=flatIndex(a.face,a.u,a.v,a.radial);
    if (const auto it=edits.find(idx); it!=edits.end()) return it->second;
    return baseline(a);
}

BlockType PlanetSurfaceSnapshot::get(SurfaceCellAddress address) const {
    return get(address.face,address.u,address.v,address.radial);
}

BlockType PlanetSurfaceSnapshot::baseline(SurfaceCellAddress address) const {
    if (address.radial < 0) return BlockType::Stone;
    if (address.radial >= RadialLayers) return BlockType::Air;
    address=normalize(address);
    const int surface=static_cast<int>(generatedSurfaceRadials[static_cast<std::size_t>(surfaceColumnIndex(address.face,address.u,address.v))]);
    return generatedBlockFor(seed,planetClass,address.face,address.u,address.v,address.radial,surface);
}

bool PlanetSurfaceSnapshot::isRefined(SurfaceCellAddress address) const {
    if (!radialInBounds(address.radial)) return false;
    address=normalize(address);
    return microBricks.contains(flatIndex(address.face,address.u,address.v,address.radial));
}

bool PlanetSurfaceSnapshot::hasMicroDetail(SurfaceCellAddress address) const {
    if (!radialInBounds(address.radial)) return false;
    address = normalize(address);
    if (microBricks.contains(flatIndex(address.face, address.u, address.v, address.radial))) return true;
    const int surface = cachedColumnRadial(generatedSurfaceRadials, address.face, address.u, address.v);
    int sUPos=0,sUNeg=0,sVPos=0,sVNeg=0;
    neighborColumnRadials(generatedSurfaceRadials, address.face, address.u, address.v, sUPos, sUNeg, sVPos, sVNeg);
    const int slope = proceduralSlopeFromNeighbors(surface, sUPos, sUNeg, sVPos, sVNeg);
    return proceduralMicroActive(planetClass, address.radial, surface, slope);
}

const MicroBrick* PlanetSurfaceSnapshot::microBrick(SurfaceCellAddress address) const {
    if (!radialInBounds(address.radial)) return nullptr;
    address=normalize(address);
    const auto it=microBricks.find(flatIndex(address.face,address.u,address.v,address.radial));
    return it==microBricks.end()?nullptr:&it->second;
}

BlockType PlanetSurfaceSnapshot::microGet(SurfaceCellAddress address, int mu, int mr, int mv) const {
    if (!radialInBounds(address.radial)) return address.radial<0?BlockType::Stone:BlockType::Air;
    address=normalize(address);
    const BlockType macro = get(address);
    const auto* brick=microBrick(address);
    if (brick) {
        const int idx = MicroBrick::index(mu, mr, mv);
        if (brick->hasOverride(idx)) return brick->getIndex(idx);
        // Empty brick = player tombstone suppressing procedural (pure macro).
        if (brick->overrideCount() == 0) return macro;
        // Player-touched cell: non-overridden micros keep regenerable underlay.
    }
    const int surface = cachedColumnRadial(generatedSurfaceRadials, address.face, address.u, address.v);
    int sUPos=0,sUNeg=0,sVPos=0,sVNeg=0;
    neighborColumnRadials(generatedSurfaceRadials, address.face, address.u, address.v, sUPos, sUNeg, sVPos, sVNeg);
    const int slope = proceduralSlopeFromNeighbors(surface, sUPos, sUNeg, sVPos, sVNeg);
    return proceduralMicroBlock(seed, planetClass, address.face, address.u, address.v, address.radial,
                                surface, slope, sUPos, sUNeg, sVPos, sVNeg, mu, mr, mv, macro);
}

BlockType PlanetSurfaceSnapshot::microGet(const SurfaceMicroAddress& address) const {
    return microGet(address.cell,address.u,address.radial,address.v);
}

void PlanetSurfaceSnapshot::sampleMicroBrick(SurfaceCellAddress address,
                                             std::array<BlockType, MicroBrick::CellCount>& out) const {
    if (!radialInBounds(address.radial)) {
        out.fill(address.radial < 0 ? BlockType::Stone : BlockType::Air);
        return;
    }
    address = normalize(address);
    const BlockType macro = get(address);
    const auto* brick = microBrick(address);
    const bool tombstone = brick && brick->overrideCount() == 0;
    const int surface = cachedColumnRadial(generatedSurfaceRadials, address.face, address.u, address.v);
    int sUPos = 0, sUNeg = 0, sVPos = 0, sVNeg = 0;
    neighborColumnRadials(generatedSurfaceRadials, address.face, address.u, address.v,
                          sUPos, sUNeg, sVPos, sVNeg);
    const int slope = proceduralSlopeFromNeighbors(surface, sUPos, sUNeg, sVPos, sVNeg);
    const bool procedural = !tombstone &&
        proceduralMicroActive(planetClass, address.radial, surface, slope);

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
            seed, planetClass, address.face, address.u, address.v, address.radial,
            surface, slope, sUPos, sUNeg, sVPos, sVNeg, mu, mr, mv, macro);
    }
}

int PlanetSurfaceSnapshot::surfaceRadial(CubeFace face, int u, int v) const {
    const auto a = normalize({face,u,v,0});
    for (int r=RadialLayers-1;r>=0;--r) {
        if (blockProperties(get(a.face,a.u,a.v,r)).solid) return r;
    }
    return -1;
}

EditInfluenceSummary PlanetSurfaceSnapshot::editInfluence(const PlanetChunkAddress& address) const {
    const auto it=editInfluenceSummaries.find(stableChunkKey(address));
    return it==editInfluenceSummaries.end()?EditInfluenceSummary{}:it->second;
}

bool PlanetSurfaceSnapshot::hasEditInfluence(CubeFace face, int uBegin, int vBegin, int uEnd, int vEnd) const {
    uBegin=std::clamp(uBegin,0,FaceResolution);
    vBegin=std::clamp(vBegin,0,FaceResolution);
    uEnd=std::clamp(uEnd,0,FaceResolution);
    vEnd=std::clamp(vEnd,0,FaceResolution);
    if(uBegin>=uEnd || vBegin>=vEnd) return false;

    // v0.12: field LOD first consults compact per-chunk summaries rather than
    // scanning every sparse edit for each coarse tile. The exact edit/micro maps
    // remain present for authoritative reconstruction and compatibility.
    const int cu0=uBegin/PlanetSurface::ChunkSize;
    const int cv0=vBegin/PlanetSurface::ChunkSize;
    const int cu1=(uEnd-1)/PlanetSurface::ChunkSize;
    const int cv1=(vEnd-1)/PlanetSurface::ChunkSize;
    for(int cv=cv0;cv<=cv1;++cv) for(int cu=cu0;cu<=cu1;++cu) {
        for(int cr=0;cr<PlanetSurface::RadialChunks;++cr) {
            const PlanetChunkAddress chunk{face,cu,cv,cr};
            const auto summary=editInfluence(chunk);
            if(summary.intersects(uBegin,vBegin,uEnd,vEnd)) return true;
        }
    }

    // Old/hand-constructed snapshots may not carry summaries. Preserve exact
    // behavior as a deterministic fallback.
    if(!editInfluenceSummaries.empty()) return false;
    const int targetFace=static_cast<int>(face);
    constexpr int cellsPerFace=FaceResolution*FaceResolution*RadialLayers;
    auto inside=[&](int flat) {
        if(flat<0 || flat>=TotalCells) return false;
        const int f=flat/cellsPerFace;
        if(f!=targetFace) return false;
        int rem=flat-f*cellsPerFace;
        const int v=rem/(FaceResolution*RadialLayers);
        rem-=v*FaceResolution*RadialLayers;
        const int u=rem/RadialLayers;
        return u>=uBegin && u<uEnd && v>=vBegin && v<vEnd;
    };
    for(const auto& [flat,_]:edits) if(inside(flat)) return true;
    for(const auto& [flat,brick]:microBricks) if(brick.overrideCount()>0 && inside(flat)) return true;
    return false;
}

Vec3 PlanetSurfaceSnapshot::boundaryPosition(CubeFace face, int uEdge, int vEdge, int radialBoundary) const {
    const Vec3 d = faceGridCornerDirection(face,uEdge,vEdge,FaceResolution);
    const float radius = referenceRadius + static_cast<float>(radialBoundary - ReferenceRadial);
    return d * radius;
}

Vec3 PlanetSurfaceSnapshot::cellCenterPosition(SurfaceCellAddress address) const {
    address = normalize(address);
    const Vec3 d = faceGridCellDirection(address.face,address.u,address.v,FaceResolution);
    const float radius = referenceRadius + (static_cast<float>(address.radial) + 0.5f - static_cast<float>(ReferenceRadial));
    return d * radius;
}

Vec3 PlanetSurfaceSnapshot::microBoundaryPosition(SurfaceCellAddress address, int uEdge, int radialEdge, int vEdge) const {
    return microBoundary(referenceRadius,address,uEdge,radialEdge,vEdge);
}

Vec3 PlanetSurfaceSnapshot::microCellCenterPosition(const SurfaceMicroAddress& address) const {
    return microBoundaryPosition(address.cell,address.u,address.radial,address.v) * 0.125f +
           microBoundaryPosition(address.cell,address.u+1,address.radial,address.v) * 0.125f +
           microBoundaryPosition(address.cell,address.u,address.radial+1,address.v) * 0.125f +
           microBoundaryPosition(address.cell,address.u,address.radial,address.v+1) * 0.125f +
           microBoundaryPosition(address.cell,address.u+1,address.radial+1,address.v) * 0.125f +
           microBoundaryPosition(address.cell,address.u+1,address.radial,address.v+1) * 0.125f +
           microBoundaryPosition(address.cell,address.u,address.radial+1,address.v+1) * 0.125f +
           microBoundaryPosition(address.cell,address.u+1,address.radial+1,address.v+1) * 0.125f;
}

SurfaceMicroAddress PlanetSurfaceSnapshot::locateMicro(Vec3 p) const {
    return locateMicroCommon(referenceRadius,p);
}

bool PlanetSurfaceSnapshot::solidAt(Vec3 p) const {
    const auto m=locateMicro(p);
    if (!radialInBounds(m.cell.radial)) return m.cell.radial<0;
    return blockProperties(microGet(m)).solid;
}

PlanetSurface::PlanetSurface(std::uint64_t seed, PlanetClass planetClass, float referenceRadius)
    : seed_(seed), class_(planetClass), referenceRadius_(referenceRadius) {
    // v0.10: retain only a one-byte height/field sample per surface column. The
    // 32-layer voxel baseline is never materialized; cells are regenerated from
    // this deterministic field + material rules when queried.
    for (int f=0;f<FaceCount;++f) {
        const auto face=static_cast<CubeFace>(f);
        for (int v=0;v<FaceResolution;++v) for (int u=0;u<FaceResolution;++u) {
            generatedSurfaceRadials_[static_cast<std::size_t>(surfaceColumnIndex(face,u,v))] =
                static_cast<std::uint8_t>(generatedSurfaceRadialFor(seed_,class_,face,u,v));
        }
    }
    chunkRevisions_.fill(revision_);
}

SurfaceCellAddress PlanetSurface::normalize(SurfaceCellAddress address) const {
    return normalizeAddress(address);
}

bool PlanetSurface::radialInBounds(int radial) const {
    return radial >= 0 && radial < RadialLayers;
}

int PlanetSurface::flatIndex(SurfaceCellAddress address) const {
    address = normalize(address);
    return address.radial + RadialLayers * (address.u + FaceResolution *
           (address.v + FaceResolution * faceIndex(address.face)));
}

SurfaceCellAddress PlanetSurface::cellFromFlatIndex(int index) const {
    const int cellsPerFace = FaceResolution * FaceResolution * RadialLayers;
    const int f = index / cellsPerFace;
    int rem = index - f * cellsPerFace;
    const int v = rem / (FaceResolution * RadialLayers);
    rem -= v * FaceResolution * RadialLayers;
    const int u = rem / RadialLayers;
    const int radial = rem - u * RadialLayers;
    return {static_cast<CubeFace>(std::clamp(f,0,FaceCount-1)),u,v,radial};
}

SurfaceChunkJournal* PlanetSurface::findJournal(const PlanetChunkAddress& address) {
    const auto it=journals_.find(stableChunkKey(address));
    return it==journals_.end()?nullptr:&it->second;
}

const SurfaceChunkJournal* PlanetSurface::findJournal(const PlanetChunkAddress& address) const {
    const auto it=journals_.find(stableChunkKey(address));
    return it==journals_.end()?nullptr:&it->second;
}

SurfaceChunkJournal& PlanetSurface::ensureJournal(const PlanetChunkAddress& address) {
    const auto key=stableChunkKey(address);
    auto [it,inserted]=journals_.try_emplace(key);
    if (inserted) it->second.address=address;
    return it->second;
}

SurfaceChunkJournal& PlanetSurface::ensureJournal(SurfaceCellAddress address) {
    return ensureJournal(chunkOf(normalize(address)));
}

void PlanetSurface::compactJournal(const PlanetChunkAddress& address) {
    const auto key=stableChunkKey(address);
    const auto it=journals_.find(key);
    if (it!=journals_.end() && it->second.empty()) journals_.erase(it);
}

void PlanetSurface::rebuildJournalInfluence(const PlanetChunkAddress& address) {
    auto* j=findJournal(address);
    if(!j) return;
    EditInfluenceSummary summary{};
    auto include=[&](SurfaceCellAddress a) {
        a=normalize(a);
        if(!summary.any) {
            summary.any=true;
            summary.minU=summary.maxU=a.u;
            summary.minV=summary.maxV=a.v;
            summary.minRadial=summary.maxRadial=a.radial;
        } else {
            summary.minU=std::min(summary.minU,a.u); summary.maxU=std::max(summary.maxU,a.u);
            summary.minV=std::min(summary.minV,a.v); summary.maxV=std::max(summary.maxV,a.v);
            summary.minRadial=std::min(summary.minRadial,a.radial); summary.maxRadial=std::max(summary.maxRadial,a.radial);
        }
    };
    auto classify=[&](SurfaceCellAddress a,bool currentAnySolid,bool currentAllSolid) {
        const bool baselineSolid=blockProperties(baseline(a)).solid;
        if(!baselineSolid && currentAnySolid) ++summary.addedSolidCells;
        if(baselineSolid && !currentAllSolid) ++summary.removedSolidCells;
        const int surface=generatedSurfaceRadial(a.face,a.u,a.v);
        if(currentAnySolid && a.radial>surface)
            summary.maxOutwardDelta=std::max(summary.maxOutwardDelta,a.radial-surface);
        if(baselineSolid && !currentAllSolid && a.radial<=surface)
            summary.maxInwardDepth=std::max(summary.maxInwardDepth,surface-a.radial+1);
    };

    for(const auto& [flat,type]:j->macroEdits) {
        // A refined cell may retain a macro override as the MicroBrick baseline.
        // Classify the final refined occupancy exactly once below rather than
        // counting the same authored cell as both a macro and micro edit.
        if(j->microBricks.contains(flat)) continue;
        const auto a=cellFromFlatIndex(flat);
        include(a);
        const bool solid=blockProperties(type).solid;
        classify(a,solid,solid);
    }
    for(const auto& [flat,brick]:j->microBricks) {
        const auto a=cellFromFlatIndex(flat);
        include(a);
        ++summary.refinedCells;
        summary.microOverrides+=static_cast<int>(brick.overrideCount());
        bool anySolid=false,allSolid=true;
        for(int i=0;i<MicroBrick::CellCount;++i) {
            const bool solid=blockProperties(brick.getIndex(i)).solid;
            anySolid=anySolid||solid;
            allSolid=allSolid&&solid;
        }
        classify(a,anySolid,allSolid);
    }
    j->influence=summary;
}

const SurfaceChunkJournal* PlanetSurface::journal(const PlanetChunkAddress& address) const {
    return findJournal(address);
}

BlockType PlanetSurface::get(SurfaceCellAddress address) const {
    if (address.radial < 0) return BlockType::Stone;
    if (address.radial >= RadialLayers) return BlockType::Air;
    address = normalize(address);
    const int idx=flatIndex(address);
    if (const auto* j=findJournal(chunkOf(address))) {
        if (const auto it=j->macroEdits.find(idx); it!=j->macroEdits.end()) return it->second;
    }
    return baseline(address);
}

BlockType PlanetSurface::get(CubeFace face, int u, int v, int radial) const {
    return get({face,u,v,radial});
}

BlockType PlanetSurface::baseline(SurfaceCellAddress address) const {
    if (!radialInBounds(address.radial)) return address.radial < 0 ? BlockType::Stone : BlockType::Air;
    address = normalize(address);
    const int surface=generatedSurfaceRadial(address.face,address.u,address.v);
    return generatedBlock(address.face,address.u,address.v,address.radial,surface);
}

void PlanetSurface::set(SurfaceCellAddress address, BlockType type, bool placedByPlayer) {
    if (!radialInBounds(address.radial)) return;
    address = normalize(address);
    const int idx = flatIndex(address);
    const auto chunk=chunkOf(address);
    const BlockType current=get(address);
    const auto* existing=findJournal(chunk);
    const bool hadRefinement=existing && existing->microBricks.contains(idx);
    if (current == type && !hadRefinement) return;

    auto& j=ensureJournal(chunk);
    j.microBricks.erase(idx); // macro replacement intentionally discards refined state
    if (type == baseline(address)) j.macroEdits.erase(idx);
    else j.macroEdits[idx] = type;
    if (type == BlockType::Air) j.placedMarkers.erase(idx);
    else if (placedByPlayer) j.placedMarkers.insert(idx);
    rebuildJournalInfluence(chunk);
    compactJournal(chunk);
    ++revision_;
    markDirty(address);
}

void PlanetSurface::applySavedEdit(int index, BlockType type) {
    if (index < 0 || index >= TotalCells) return;
    const SurfaceCellAddress address = cellFromFlatIndex(index);
    const auto chunk=chunkOf(address);
    const auto* existing=findJournal(chunk);
    const bool hadRefinement=existing && existing->microBricks.contains(index);
    if (get(address) == type && !hadRefinement) return;
    auto& j=ensureJournal(chunk);
    j.microBricks.erase(index);
    if (type == baseline(address)) j.macroEdits.erase(index);
    else j.macroEdits[index] = type;
    rebuildJournalInfluence(chunk);
    compactJournal(chunk);
    ++revision_;
    markDirty(address);
}

void PlanetSurface::applySavedPlacedMarker(int index) {
    if (index < 0 || index >= TotalCells) return;
    const auto address=cellFromFlatIndex(index);
    if (blockProperties(get(address)).solid) ensureJournal(address).placedMarkers.insert(index);
}

bool PlanetSurface::playerPlaced(SurfaceCellAddress address) const {
    if (!radialInBounds(address.radial)) return false;
    address = normalize(address);
    const auto* j=findJournal(chunkOf(address));
    return j && j->placedMarkers.contains(flatIndex(address));
}

MicroBrick& PlanetSurface::refineCell(SurfaceCellAddress address) {
    address=normalize(address);
    const int idx=flatIndex(address);
    auto& j=ensureJournal(address);
    auto it=j.microBricks.find(idx);
    if (it!=j.microBricks.end()) return it->second;
    // Store only player deltas from macro baseline. Regenerable procedural
    // underlay continues to apply for non-overridden micros via microGet.
    auto [created,_]=j.microBricks.emplace(idx, MicroBrick(get(address)));
    return created->second;
}

bool PlanetSurface::isRefined(SurfaceCellAddress address) const {
    if (!radialInBounds(address.radial)) return false;
    address=normalize(address);
    const auto* j=findJournal(chunkOf(address));
    return j && j->microBricks.contains(flatIndex(address));
}

bool PlanetSurface::hasMicroDetail(SurfaceCellAddress address) const {
    if (!radialInBounds(address.radial)) return false;
    address = normalize(address);
    if (isRefined(address)) return true;
    const int surface = generatedSurfaceRadial(address.face, address.u, address.v);
    const int slope = columnSlope(address.face, address.u, address.v);
    return proceduralMicroActive(class_, address.radial, surface, slope);
}

const MicroBrick* PlanetSurface::microBrick(SurfaceCellAddress address) const {
    if (!radialInBounds(address.radial)) return nullptr;
    address=normalize(address);
    const auto* j=findJournal(chunkOf(address));
    if (!j) return nullptr;
    const auto it=j->microBricks.find(flatIndex(address));
    return it==j->microBricks.end()?nullptr:&it->second;
}

BlockType PlanetSurface::microGet(SurfaceCellAddress address, int mu, int mr, int mv) const {
    if (!radialInBounds(address.radial)) return address.radial<0?BlockType::Stone:BlockType::Air;
    address=normalize(address);
    return resolveMicro(address, mu, mr, mv, microBrick(address));
}

BlockType PlanetSurface::microGet(const SurfaceMicroAddress& address) const {
    return microGet(address.cell,address.u,address.radial,address.v);
}

void PlanetSurface::setMicro(SurfaceCellAddress address, int mu, int mr, int mv, BlockType type) {
    if (!radialInBounds(address.radial) || !MicroBrick::inBounds(mu,mr,mv)) return;
    address=normalize(address);
    const BlockType before=microGet(address,mu,mr,mv);
    // Skip only when a player brick already holds this occupancy. Matching
    // regenerable procedural occupancy still refines so the edit is journaled.
    if (before==type && isRefined(address)) return;
    const int idx=flatIndex(address);
    const auto chunk=chunkOf(address);
    MicroBrick& brick=refineCell(address);
    brick.set(mu,mr,mv,type);
    if (brick.overrideCount()==0) {
        // Keep an empty brick as a tombstone when procedural detail would
        // otherwise resurrect — player cleared the cell back to pure macro.
        const int surface = generatedSurfaceRadial(address.face, address.u, address.v);
        const int slope = columnSlope(address.face, address.u, address.v);
        if (!proceduralMicroActive(class_, address.radial, surface, slope)) {
            if (auto* j=findJournal(chunk)) j->microBricks.erase(idx);
        }
    }
    rebuildJournalInfluence(chunk);
    compactJournal(chunk);
    ++revision_;
    markDirty(address);
}

void PlanetSurface::setMicroIndex(SurfaceCellAddress address, int microIndex, BlockType type) {
    if (microIndex<0 || microIndex>=MicroBrick::CellCount) return;
    const int mu=microIndex%MicroBrick::Resolution;
    const int t=microIndex/MicroBrick::Resolution;
    const int mv=t%MicroBrick::Resolution;
    const int mr=t/MicroBrick::Resolution;
    setMicro(address,mu,mr,mv,type);
}

void PlanetSurface::applySavedMicroEdit(int flatCellIndex, int microIndex, BlockType type) {
    if (flatCellIndex<0 || flatCellIndex>=TotalCells ||
        microIndex<0 || microIndex>=MicroBrick::CellCount) return;
    setMicroIndex(cellFromFlatIndex(flatCellIndex),microIndex,type);
}

std::size_t PlanetSurface::microOverrideCount() const {
    std::size_t count=0;
    for (const auto& [_,journal] : journals_) count += journal.microOverrideCount();
    return count;
}

std::size_t PlanetSurface::macroEditCount() const {
    std::size_t count=0;
    for (const auto& [_,journal] : journals_) count += journal.macroEdits.size();
    return count;
}

std::size_t PlanetSurface::placedMarkerCount() const {
    std::size_t count=0;
    for (const auto& [_,journal] : journals_) count += journal.placedMarkers.size();
    return count;
}

std::size_t PlanetSurface::persistentCellStateCount() const {
    std::size_t count=0;
    for (const auto& [_,journal] : journals_) count += journal.persistentCellStateCount();
    return count;
}

int PlanetSurface::surfaceRadial(CubeFace face, int u, int v) const {
    const auto a = normalize({face,u,v,0});
    for (int r=RadialLayers-1;r>=0;--r) {
        // A refined cell is treated as surface-bearing if any micro material is
        // solid. This keeps radial spawn/ground queries stable after sculpting.
        const SurfaceCellAddress c{a.face,a.u,a.v,r};
        if (const auto* brick=microBrick(c)) {
            bool anySolid=false;
            for (int i=0;i<MicroBrick::CellCount && !anySolid;++i)
                anySolid=blockProperties(brick->getIndex(i)).solid;
            if (anySolid) return r;
        } else if (blockProperties(get(c)).solid) return r;
    }
    return -1;
}

Vec3 PlanetSurface::boundaryPosition(CubeFace face, int uEdge, int vEdge, int radialBoundary) const {
    const Vec3 d = faceGridCornerDirection(face,uEdge,vEdge,FaceResolution);
    const float radius = referenceRadius_ + static_cast<float>(radialBoundary - ReferenceRadial);
    return d * radius;
}

Vec3 PlanetSurface::cellCenterPosition(SurfaceCellAddress address) const {
    address = normalize(address);
    const Vec3 d = faceGridCellDirection(address.face,address.u,address.v,FaceResolution);
    const float radius = referenceRadius_ + (static_cast<float>(address.radial) + 0.5f - static_cast<float>(ReferenceRadial));
    return d * radius;
}

Vec3 PlanetSurface::microBoundaryPosition(SurfaceCellAddress address, int uEdge, int radialEdge, int vEdge) const {
    return microBoundary(referenceRadius_,address,uEdge,radialEdge,vEdge);
}

Vec3 PlanetSurface::microCellCenterPosition(const SurfaceMicroAddress& address) const {
    return microBoundaryPosition(address.cell,address.u,address.radial,address.v) * 0.125f +
           microBoundaryPosition(address.cell,address.u+1,address.radial,address.v) * 0.125f +
           microBoundaryPosition(address.cell,address.u,address.radial+1,address.v) * 0.125f +
           microBoundaryPosition(address.cell,address.u,address.radial,address.v+1) * 0.125f +
           microBoundaryPosition(address.cell,address.u+1,address.radial+1,address.v) * 0.125f +
           microBoundaryPosition(address.cell,address.u+1,address.radial,address.v+1) * 0.125f +
           microBoundaryPosition(address.cell,address.u,address.radial+1,address.v+1) * 0.125f +
           microBoundaryPosition(address.cell,address.u+1,address.radial+1,address.v+1) * 0.125f;
}

SurfaceCellAddress PlanetSurface::locateUnclamped(Vec3 p) const {
    const float radius = length(p);
    const FaceUv uv = directionToFaceUv(p);
    SurfaceCellAddress out{};
    out.face = uv.face;
    out.u = clampCell(uv.u,FaceResolution);
    out.v = clampCell(uv.v,FaceResolution);
    out.radial = static_cast<int>(std::floor(radius - referenceRadius_ + static_cast<float>(ReferenceRadial)));
    return out;
}

SurfaceCellAddress PlanetSurface::locate(Vec3 p) const {
    auto out = locateUnclamped(p);
    out.radial = std::clamp(out.radial,0,RadialLayers-1);
    return out;
}

SurfaceMicroAddress PlanetSurface::locateMicro(Vec3 p) const {
    return locateMicroCommon(referenceRadius_,p);
}

SurfaceRayHit PlanetSurface::raycast(Vec3 origin, Vec3 direction, float maxDistance, float step) const {
    SurfaceRayHit out{};
    direction = ::elysium::normalize(direction);
    if (lengthSq(direction) < 0.5f || maxDistance <= 0.0f) return out;
    step = std::clamp(step,0.025f,0.5f);

    // Macro raycast uses macro occupancy. Micro hits use raycastMicro; capsule
    // collision keeps solidAt (micro). This keeps previous as an air placement cell.
    SurfaceCellAddress previous = locateUnclamped(origin);
    bool havePrevious = radialInBounds(previous.radial) &&
                        !blockProperties(get(normalize(previous))).solid;
    for (float t=0.0f; t<=maxDistance + step*0.5f; t+=step) {
        const Vec3 point = origin + direction*t;
        const SurfaceCellAddress cell = locateUnclamped(point);
        if (radialInBounds(cell.radial)) {
            const SurfaceCellAddress ncell = normalize(cell);
            if (blockProperties(get(ncell)).solid) {
                out.hit = true;
                out.cell = ncell;
                if (havePrevious) {
                    out.previous = normalize(previous);
                } else {
                    // Stepped from out-of-bounds / vacuum onto the surface shell.
                    out.previous = ncell;
                    ++out.previous.radial;
                    if (!radialInBounds(out.previous.radial)) out.previous = ncell;
                    else out.previous = normalize(out.previous);
                }
                out.point = point;
                out.distance = t;
                return out;
            }
            previous = cell;
            havePrevious = true;
        }
    }
    return out;
}

SurfaceMicroRayHit PlanetSurface::raycastMicro(Vec3 origin, Vec3 direction, float maxDistance, float step) const {
    SurfaceMicroRayHit out{};
    direction=::elysium::normalize(direction);
    if (lengthSq(direction)<0.5f || maxDistance<=0.0f) return out;
    step=std::clamp(step,0.008f,0.05f);
    SurfaceMicroAddress last{};
    bool haveLast=false;
    for (float t=0.0f;t<=maxDistance+step*0.5f;t+=step) {
        const Vec3 p=origin+direction*t;
        const auto m=locateMicro(p);
        if (!radialInBounds(m.cell.radial)) continue;
        if (haveLast && m==last) continue;
        last=m; haveLast=true;
        const BlockType type=microGet(m);
        if (blockProperties(type).solid) {
            out.hit=true; out.micro=m; out.point=p; out.distance=t; out.type=type;
            return out;
        }
    }
    return out;
}

bool PlanetSurface::solidAt(Vec3 p) const {
    const auto m=locateMicro(p);
    if (!radialInBounds(m.cell.radial)) return m.cell.radial<0;
    return blockProperties(microGet(m)).solid;
}

bool PlanetSurface::capsuleCollides(Vec3 feet, float radius, float height) const {
    if (lengthSq(feet)<1.0f) return true;
    const auto frame=surfaceFrame(feet);
    const float low=0.03f;
    const float high=std::max(low,height-0.10f);
    const std::array<float,3> levels{low,(low+high)*0.5f,high};
    const std::array<Vec3,9> offsets{{
        {0,0,0},
        frame.right*radius, frame.right*(-radius),
        frame.forward*radius, frame.forward*(-radius),
        ::elysium::normalize(frame.right+frame.forward)*radius,
        ::elysium::normalize(frame.right-frame.forward)*radius,
        ::elysium::normalize(frame.forward-frame.right)*radius,
        ::elysium::normalize((frame.right+frame.forward)*-1.0f)*radius
    }};
    for (float h:levels) {
        const Vec3 center=feet+frame.up*h;
        for (const Vec3 off:offsets) if (solidAt(center+off)) return true;
    }
    return false;
}

bool PlanetSurface::groundedAt(Vec3 feet, float probe) const {
    if (lengthSq(feet)<1.0f) return false;
    const Vec3 up=::elysium::normalize(feet);
    return capsuleCollides(feet-up*std::max(0.01f,probe));
}

float PlanetSurface::surfaceBoundaryRadius(Vec3 direction) const {
    const FaceUv uv = directionToFaceUv(direction);
    const int u = clampCell(uv.u,FaceResolution);
    const int v = clampCell(uv.v,FaceResolution);
    const int surface = surfaceRadial(uv.face,u,v);
    if (surface < 0) return referenceRadius_ - static_cast<float>(ReferenceRadial);
    return referenceRadius_ + static_cast<float>(surface + 1 - ReferenceRadial);
}

SurfaceFrame PlanetSurface::surfaceFrame(Vec3 p) const {
    SurfaceFrame frame{};
    frame.position = p;
    frame.up = ::elysium::normalize(p);
    frame.forward = tangentReference(frame.up);
    frame.right = ::elysium::normalize(cross(frame.forward,frame.up));
    frame.forward = ::elysium::normalize(cross(frame.up,frame.right));
    return frame;
}

Vec3 PlanetSurface::gravityDirectionAt(Vec3 p) const {
    const Vec3 up = ::elysium::normalize(p);
    return up * -1.0f;
}

Vec3 PlanetSurface::cameraRelative(Vec3 p, Vec3 camera) const {
    return p - camera;
}

bool PlanetSurface::gasPassable(SurfaceCellAddress address) const {
    if (address.radial<0) return false;
    if (address.radial>=RadialLayers) return true;
    address=normalize(address);
    if (!blockProperties(get(address)).solid) return true;
    const auto* brick=microBrick(address);
    if (!brick) return false;
    // Conservative v0.6 seal rule: any non-solid microcell makes the macro cell
    // gas-passable. This guarantees chiseled breaches are never falsely sealed;
    // exact micro-pore connectivity can replace it without changing save data.
    for (int i=0;i<MicroBrick::CellCount;++i)
        if (!blockProperties(brick->getIndex(i)).solid) return true;
    return false;
}

SurfaceSealedVolumeQuery PlanetSurface::sealedVolume(SurfaceCellAddress start, int maxCells) const {
    SurfaceSealedVolumeQuery out{};
    maxCells=std::max(1,maxCells);
    if (!radialInBounds(start.radial)) return out;
    start=normalize(start);
    if (!gasPassable(start)) return out;

    std::deque<SurfaceCellAddress> q;
    std::unordered_set<int> visited;
    q.push_back(start);
    visited.insert(flatIndex(start));
    bool leaked=false;

    while(!q.empty()) {
        const auto c=q.front(); q.pop_front();
        out.cells.push_back(flatIndex(c));
        if (static_cast<int>(out.cells.size())>=maxCells) {
            out.truncated=true;
            out.sealed=false;
            return out;
        }
        const std::array<SurfaceCellAddress,6> neighbors{{
            {c.face,c.u+1,c.v,c.radial},{c.face,c.u-1,c.v,c.radial},
            {c.face,c.u,c.v+1,c.radial},{c.face,c.u,c.v-1,c.radial},
            {c.face,c.u,c.v,c.radial+1},{c.face,c.u,c.v,c.radial-1}
        }};
        for (auto n:neighbors) {
            if (n.radial>=RadialLayers) { leaked=true; continue; }
            if (n.radial<0) continue;
            n=normalize(n);
            if (!gasPassable(n)) continue;
            const int idx=flatIndex(n);
            if (visited.insert(idx).second) q.push_back(n);
        }
    }
    out.sealed=!leaked;
    return out;
}

PlanetChunkAddress PlanetSurface::chunkOf(SurfaceCellAddress address) const {
    address = normalize(address);
    return chunkAddress(address.face,address.u,address.v,address.radial,ChunkSize);
}

int PlanetSurface::chunkSlot(const PlanetChunkAddress& address) const {
    const int f = faceIndex(address.face);
    if (address.u < 0 || address.u >= ChunksPerFaceAxis ||
        address.v < 0 || address.v >= ChunksPerFaceAxis ||
        address.radial < 0 || address.radial >= RadialChunks) return -1;
    return address.u + ChunksPerFaceAxis * (address.v + ChunksPerFaceAxis *
           (address.radial + RadialChunks * f));
}

std::uint64_t PlanetSurface::chunkRevision(const PlanetChunkAddress& address) const {
    const int slot = chunkSlot(address);
    return slot >= 0 ? chunkRevisions_[static_cast<std::size_t>(slot)] : 0;
}

void PlanetSurface::markDirty(SurfaceCellAddress address) {
    const auto own = chunkOf(address);
    const int ownSlot = chunkSlot(own);
    if (ownSlot >= 0) chunkRevisions_[static_cast<std::size_t>(ownSlot)] = revision_;

    constexpr std::array<IVec3,4> dirs{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}}};
    for (const auto& d : dirs) {
        const auto n = normalize({address.face,address.u+d.x,address.v+d.y,address.radial});
        const auto nc = chunkOf(n);
        if (!(nc == own)) {
            const int slot = chunkSlot(nc);
            if (slot >= 0) chunkRevisions_[static_cast<std::size_t>(slot)] = revision_;
        }
    }
}

int PlanetSurface::generatedSurfaceRadial(CubeFace face, int u, int v) const {
    const auto a=normalize({face,u,v,0});
    return static_cast<int>(generatedSurfaceRadials_[static_cast<std::size_t>(surfaceColumnIndex(a.face,a.u,a.v))]);
}

int PlanetSurface::columnSlope(CubeFace face, int u, int v) const {
    const auto a = normalize({face, u, v, 0});
    const int surface = generatedSurfaceRadial(a.face, a.u, a.v);
    int sUPos=0,sUNeg=0,sVPos=0,sVNeg=0;
    neighborColumnRadials(generatedSurfaceRadials_, a.face, a.u, a.v, sUPos, sUNeg, sVPos, sVNeg);
    return proceduralSlopeFromNeighbors(surface, sUPos, sUNeg, sVPos, sVNeg);
}

BlockType PlanetSurface::resolveMicro(SurfaceCellAddress address, int mu, int mr, int mv,
                                      const MicroBrick* playerBrick) const {
    const BlockType macro = get(address);
    if (playerBrick) {
        const int idx = MicroBrick::index(mu, mr, mv);
        if (playerBrick->hasOverride(idx)) return playerBrick->getIndex(idx);
        if (playerBrick->overrideCount() == 0) return macro; // tombstone
    }
    const int surface = generatedSurfaceRadial(address.face, address.u, address.v);
    int sUPos=0,sUNeg=0,sVPos=0,sVNeg=0;
    neighborColumnRadials(generatedSurfaceRadials_, address.face, address.u, address.v,
                          sUPos, sUNeg, sVPos, sVNeg);
    const int slope = proceduralSlopeFromNeighbors(surface, sUPos, sUNeg, sVPos, sVNeg);
    return proceduralMicroBlock(seed_, class_, address.face, address.u, address.v, address.radial,
                                surface, slope, sUPos, sUNeg, sVPos, sVNeg, mu, mr, mv, macro);
}

BlockType PlanetSurface::generatedBlock(CubeFace face, int u, int v, int radial, int surface) const {
    return generatedBlockFor(seed_,class_,face,u,v,radial,surface);
}

int PlanetSurface::chunkEditCount(const PlanetChunkAddress& address) const {
    const auto* j=findJournal(address);
    if (!j) return 0;
    int count=static_cast<int>(j->macroEdits.size());
    for (const auto& [idx,_] : j->microBricks) if (!j->macroEdits.contains(idx)) ++count;
    return count;
}

EditInfluenceSummary PlanetSurface::editInfluenceSummary(const PlanetChunkAddress& address) const {
    const auto* j=findJournal(address);
    return j?j->influence:EditInfluenceSummary{};
}

PlanetSurfaceSnapshot PlanetSurface::snapshot() const {
    PlanetSurfaceSnapshot out{};
    out.seed = seed_;
    out.planetClass = class_;
    out.referenceRadius = referenceRadius_;
    out.generatedSurfaceRadials = generatedSurfaceRadials_;
    for (const auto& [key,journal] : journals_) {
        out.edits.insert(journal.macroEdits.begin(),journal.macroEdits.end());
        out.microBricks.insert(journal.microBricks.begin(),journal.microBricks.end());
        if(journal.influence.any) out.editInfluenceSummaries.emplace(key,journal.influence);
    }
    return out;
}

} // namespace elysium
