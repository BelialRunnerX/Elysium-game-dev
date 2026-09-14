#include "core/JobSystem.hpp"
#include "core/Math.hpp"
#include "world/BaseInfrastructure.hpp"
#include "world/CubeSphere.hpp"
#include "world/MicroBrick.hpp"
#include "world/ProceduralMicro.hpp"
#include "world/ChunkOccupancy.hpp"
#include "world/PlanetSurface.hpp"
#include "world/BiomeCatalog.hpp"
#include "world/BiomeCatalog.hpp"
#include "world/PlanetEnvironment.hpp"
#include "world/SurvivalPressure.hpp"
#include "world/SurfaceSuspicion.hpp"
#include "world/ShipTransit.hpp"
#include "world/RegistryBeaconClaim.hpp"
#include "world/PlanetSurfaceMesher.hpp"
#include "world/SurfaceInfrastructure.hpp"
#include "world/SurfaceIndustry.hpp"
#include "world/EarlyMetallurgy.hpp"
#include "world/InfrastructureJournal.hpp"
#include "world/SurfaceNavigation.hpp"
#include "world/SurfaceSiege.hpp"
#include "world/ChunkVoxelSpans.hpp"
#include "world/SurfaceChunkCache.hpp"
#include "world/SurfaceChunkPersistence.hpp"
#include "world/SurfaceWorldRead.hpp"
#include "world/VoxelMesher.hpp"
#include "world/FaceCullBitmasks.hpp"
#include "world/World.hpp"
#include "render/GraphicsBackend.hpp"
#include "render/WorldRenderer.hpp"
#include "render/PlanetSurfaceRenderer.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

using namespace elysium;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}


void testPlanetSurfaceTerrainGenV2() {
    require(PlanetSurface::GeneratorVersion == 2, "GeneratorVersion must be 2 after terrain gen bump");
    require(PlanetSurface::GeneratorFingerprint == 0x454C595350483032ULL,
            "GeneratorFingerprint must be ELYSPH02");
    require(kSurfaceGeneratorVersion == PlanetSurface::GeneratorVersion,
            "persistence generator version alias drifted");
    require(kSurfaceGeneratorFingerprint == PlanetSurface::GeneratorFingerprint,
            "persistence generator fingerprint alias drifted");

    constexpr std::uint64_t seed = 0xDEADBEEFULL;
    PlanetSurface a(seed, PlanetClass::Temperate);
    PlanetSurface b(seed, PlanetClass::Temperate);

    // Same seed → identical height fields and surface materials across all faces.
    for (int f = 0; f < PlanetSurface::FaceCount; ++f) {
        const auto face = static_cast<CubeFace>(f);
        for (int v = 0; v < PlanetSurface::FaceResolution; v += 3) {
            for (int u = 0; u < PlanetSurface::FaceResolution; u += 3) {
                require(a.surfaceRadial(face, u, v) == b.surfaceRadial(face, u, v),
                        "temperate height not deterministic for identical seed");
                const int surf = a.surfaceRadial(face, u, v);
                require(a.get(face, u, v, surf) == b.get(face, u, v, surf),
                        "temperate surface block not deterministic for identical seed");
            }
        }
    }

    // Temperate height variance across one face must exceed the pre-v2 floor
    // (v1 PositiveZ @ 0xDEADBEEF: stddev≈1.82, unique≈10).
    std::vector<double> heights;
    heights.reserve(PlanetSurface::FaceResolution * PlanetSurface::FaceResolution);
    std::unordered_map<int, int> uniqueHeights;
    int minH = 999, maxH = -999;
    for (int v = 0; v < PlanetSurface::FaceResolution; ++v) {
        for (int u = 0; u < PlanetSurface::FaceResolution; ++u) {
            const int h = a.surfaceRadial(CubeFace::PositiveZ, u, v);
            heights.push_back(static_cast<double>(h));
            uniqueHeights[h] += 1;
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
    }
    double mean = 0.0;
    for (double h : heights) mean += h;
    mean /= static_cast<double>(heights.size());
    double var = 0.0;
    for (double h : heights) {
        const double d = h - mean;
        var += d * d;
    }
    var /= static_cast<double>(heights.size());
    const double stddev = std::sqrt(var);
    require(stddev >= 2.45, "temperate height stddev did not rise above pre-v2 floor");
    require(static_cast<int>(uniqueHeights.size()) >= 14,
            "temperate unique height count did not rise above pre-v2 floor");
    require(maxH - minH >= 8, "temperate height range too narrow for earth-like bands");

    // Surface materials must actually vary (not all Grass).
    int grass = 0, dirt = 0, stone = 0;
    for (int v = 0; v < PlanetSurface::FaceResolution; v += 2) {
        for (int u = 0; u < PlanetSurface::FaceResolution; u += 2) {
            const int surf = a.surfaceRadial(CubeFace::PositiveZ, u, v);
            const BlockType t = a.get(CubeFace::PositiveZ, u, v, surf);
            if (t == BlockType::Grass) ++grass;
            else if (t == BlockType::Dirt) ++dirt;
            else if (t == BlockType::Stone) ++stone;
        }
    }
    require(grass > 0 && dirt > 0 && stone > 0,
            "temperate surface materials did not vary across grass/dirt/stone");

    // Biome sampling is deterministic and class-aware.
    const Vec3 d0 = faceGridCellDirection(CubeFace::PositiveZ, 10, 10, PlanetSurface::FaceResolution);
    const Vec3 d1 = faceGridCellDirection(CubeFace::PositiveZ, 40, 20, PlanetSurface::FaceResolution);
    require(sampleBiome(seed, PlanetClass::Temperate, d0) == sampleBiome(seed, PlanetClass::Temperate, d0),
            "sampleBiome not deterministic");
    require(sampleBiome(seed, PlanetClass::Barren, d0) == BiomeId::BarrenRegolithPlain,
            "barren default biome mismatch");
    require(sampleBiome(seed, PlanetClass::Scorched, d0) == BiomeId::ScorchedBasaltPlain,
            "scorched default biome mismatch");
    // At least two temperate biome ids should appear across a face sample.
    std::unordered_map<int, int> biomeHits;
    for (int v = 0; v < PlanetSurface::FaceResolution; v += 4) {
        for (int u = 0; u < PlanetSurface::FaceResolution; u += 4) {
            const Vec3 d = faceGridCellDirection(CubeFace::PositiveZ, u, v, PlanetSurface::FaceResolution);
            biomeHits[static_cast<int>(sampleBiome(seed, PlanetClass::Temperate, d))] += 1;
        }
    }
    require(biomeHits.size() >= 2, "temperate biome sample lacked variety");
    (void)d1;

    // Barren / Scorched still generate sane shells.
    PlanetSurface barren(seed, PlanetClass::Barren);
    PlanetSurface scorched(seed, PlanetClass::Scorched);
    int barrenSolid = 0, scorchedSolid = 0;
    for (int v = 0; v < PlanetSurface::FaceResolution; v += 8) {
        for (int u = 0; u < PlanetSurface::FaceResolution; u += 8) {
            const int br = barren.surfaceRadial(CubeFace::PositiveY, u, v);
            const int sr = scorched.surfaceRadial(CubeFace::PositiveY, u, v);
            require(br >= 6 && br < PlanetSurface::RadialLayers - 3, "barren surface radial out of band");
            require(sr >= 6 && sr < PlanetSurface::RadialLayers - 3, "scorched surface radial out of band");
            require(blockProperties(barren.get(CubeFace::PositiveY, u, v, br)).solid, "barren surface not solid");
            require(blockProperties(scorched.get(CubeFace::PositiveY, u, v, sr)).solid, "scorched surface not solid");
            ++barrenSolid;
            ++scorchedSolid;
        }
    }
    require(barrenSolid > 0 && scorchedSolid > 0, "barren/scorched generation produced no samples");
}

void testCubeSphereRoundTrip() {
    const std::vector<CubeFace> faces{
        CubeFace::PositiveX,CubeFace::NegativeX,CubeFace::PositiveY,
        CubeFace::NegativeY,CubeFace::PositiveZ,CubeFace::NegativeZ
    };
    const float values[] {-0.85f,-0.25f,0.0f,0.4f,0.85f};
    for (const CubeFace face : faces) {
        for (const float u : values) for (const float v : values) {
            const Vec3 d = faceUvToDirection(face,u,v);
            const FaceUv mapped = directionToFaceUv(d);
            const Vec3 d2 = faceUvToDirection(mapped.face,mapped.u,mapped.v);
            require(dot(d,d2) > 0.99999f, "cube-sphere round trip drifted");
        }
    }
    const auto a = chunkAddress(CubeFace::PositiveZ, 0, 31, -1, 32);
    const auto b = chunkAddress(CubeFace::PositiveZ, 0, 31, -1, 32);
    require(a == b && stableChunkKey(a) == stableChunkKey(b), "chunk address/key is unstable");
}


void testPlanetSurfaceTopologyAndFrames() {
    constexpr std::uint64_t seed=0x51A7E5EEDULL;
    PlanetSurface a(seed,PlanetClass::Temperate);
    PlanetSurface b(seed,PlanetClass::Temperate);

    // v0.8 storage contract: the deterministic 6x64x64x32 voxel baseline is
    // not materialized. Only a tiny per-column field cache plus sparse player
    // state exists until edits/refinements occur.
    require(a.materializedBaselineCellCount()==0,"spherical baseline voxel volume was materialized");
    require(a.persistentCellStateCount()==0 && a.macroEditCount()==0 && a.microOverrideCount()==0 && a.journalCount()==0,
            "fresh procedural planet unexpectedly owns sparse cell state");
    const auto sparseSnapshot=a.snapshot();
    require(sparseSnapshot.edits.empty() && sparseSnapshot.microBricks.empty(),
            "worker snapshot copied phantom voxel state");

    // Exact geometric seam: +Z right boundary is +X left boundary.
    for(int v=0;v<=PlanetSurface::FaceResolution;v+=8) {
        const Vec3 z=a.boundaryPosition(CubeFace::PositiveZ,PlanetSurface::FaceResolution,v,PlanetSurface::ReferenceRadial+1);
        const Vec3 x=a.boundaryPosition(CubeFace::PositiveX,0,v,PlanetSurface::ReferenceRadial+1);
        require(length(z-x)<0.0001f,"cube-sphere boundary positions cracked across +Z/+X seam");
    }

    // Off-face cells must acquire one deterministic adjacent owner.
    const auto wrapped=a.normalize({CubeFace::PositiveZ,PlanetSurface::FaceResolution,17,PlanetSurface::ReferenceRadial});
    require(wrapped.face==CubeFace::PositiveX,"+Z right-edge ownership did not transfer to +X");
    require(wrapped.u>=0 && wrapped.u<PlanetSurface::FaceResolution && wrapped.v>=0 && wrapped.v<PlanetSurface::FaceResolution,
            "wrapped surface address escaped face bounds");

    // The complete six-face baseline is deterministic.
    for(int f=0;f<PlanetSurface::FaceCount;++f) {
        const auto face=static_cast<CubeFace>(f);
        for(int v : {0,9,31,63}) for(int u : {0,7,32,63}) {
            const int sa=a.surfaceRadial(face,u,v);
            const int sb=b.surfaceRadial(face,u,v);
            require(sa==sb,"same cube-sphere seed generated different surface height");
            require(a.get(face,u,v,sa)==b.get(face,u,v,sb),"same cube-sphere seed generated different surface material");
        }
    }

    // Sparse edits are addressed independently of transient renderer/ECS IDs and
    // compact when restored to the deterministic baseline.
    SurfaceCellAddress edit{CubeFace::NegativeX,12,44,a.surfaceRadial(CubeFace::NegativeX,12,44)};
    const BlockType original=a.get(edit);
    const int savedIndex=a.flatIndex(edit);
    a.set(edit,BlockType::Air);
    require(a.macroEditCount()==1 && a.journalCount()==1 && a.get(edit)==BlockType::Air,"cube-sphere sparse surface edit failed");
    PlanetSurface replay(seed,PlanetClass::Temperate);
    replay.applySavedEdit(savedIndex,BlockType::Air);
    require(replay.get(edit)==BlockType::Air,"cube-sphere saved edit replay failed");
    a.set(edit,original);
    require(a.macroEditCount()==0 && a.journalCount()==0,"cube-sphere edit did not compact after baseline restore");

    // Player placement markers are independent sparse metadata used by mining
    // anti-exploit logic and by the claim structure count.
    const int top=a.surfaceRadial(CubeFace::PositiveZ,20,20);
    SurfaceCellAddress placed{CubeFace::PositiveZ,20,20,top+1};
    require(a.radialInBounds(placed.radial) && a.get(placed)==BlockType::Air,"test placement cell is not empty");
    a.set(placed,BlockType::Planks,true);
    require(a.playerPlaced(placed) && a.placedMarkerCount()==1,"cube-sphere player placement marker was not recorded");
    PlanetSurface placedReplay(seed,PlanetClass::Temperate);
    placedReplay.applySavedEdit(a.flatIndex(placed),BlockType::Planks);
    placedReplay.applySavedPlacedMarker(a.flatIndex(placed));
    require(placedReplay.playerPlaced(placed),"cube-sphere placement marker did not replay");
    a.set(placed,BlockType::Air);
    require(!a.playerPlaced(placed),"removing a spherical placement left a stale marker");

    // Curved raycast enters the shell from atmosphere and returns the previous
    // air cell as the natural placement target.
    const Vec3 rayDir=faceGridCellDirection(CubeFace::PositiveZ,32,32,PlanetSurface::FaceResolution);
    const float shell=a.surfaceBoundaryRadius(rayDir);
    const auto hit=a.raycast(rayDir*(shell+3.0f),rayDir*-1.0f,6.0f,0.05f);
    require(hit.hit,"cube-sphere raycast failed to enter terrain shell");
    require(blockProperties(a.get(hit.cell)).solid,"cube-sphere raycast hit non-solid cell");
    require(a.radialInBounds(hit.previous.radial) && a.get(hit.previous)==BlockType::Air,"cube-sphere raycast lost previous air placement cell");

    const SurfaceCellAddress probe{CubeFace::PositiveY,21,37,a.surfaceRadial(CubeFace::PositiveY,21,37)};
    const Vec3 p=a.cellCenterPosition(probe);
    const auto located=a.locate(p);
    require(located.face==probe.face && located.u==probe.u && located.v==probe.v && located.radial==probe.radial,
            "planet-local position did not round-trip to storage cell");
    const auto frame=a.surfaceFrame(p);
    require(std::abs(length(frame.up)-1.0f)<0.0001f && std::abs(length(frame.right)-1.0f)<0.0001f && std::abs(length(frame.forward)-1.0f)<0.0001f,
            "spherical surface frame axes are not normalized");
    require(std::abs(dot(frame.up,frame.right))<0.0001f && std::abs(dot(frame.up,frame.forward))<0.0001f && std::abs(dot(frame.right,frame.forward))<0.0001f,
            "spherical surface frame is not orthogonal");
    require(dot(a.gravityDirectionAt(p),frame.up)<-0.9999f,"spherical gravity is not radial-inward");

    const Vec3 camera=p+frame.up*100000.0f;
    const Vec3 relative=a.cameraRelative(p,camera);
    require(length(relative)<100001.0f && length(relative)>99999.0f,"camera-relative transform did not subtract floating origin");
}


void testSurfaceChunkJournalSharding() {
    PlanetSurface p(0xC0FFEEULL,PlanetClass::Temperate);
    const int r0=p.surfaceRadial(CubeFace::PositiveZ,8,8);
    const int r1=p.surfaceRadial(CubeFace::PositiveZ,40,8);
    SurfaceCellAddress a{CubeFace::PositiveZ,8,8,r0};
    SurfaceCellAddress b{CubeFace::PositiveZ,40,8,r1};
    const BlockType baseA=p.get(a), baseB=p.get(b);
    p.set(a,BlockType::Air);
    p.set(b,BlockType::Air);
    require(p.journalCount()==2,"edits in separate spherical chunks were not sharded into separate journals");
    require(p.journal(p.chunkOf(a)) && p.journal(p.chunkOf(a))->macroEdits.size()==1,"owner chunk journal missing first macro edit");
    require(p.journal(p.chunkOf(b)) && p.journal(p.chunkOf(b))->macroEdits.size()==1,"owner chunk journal missing second macro edit");

    // Micro state belongs to the same chunk journal and does not allocate a
    // second global persistence structure.
    p.setMicro(b,0,0,0,BlockType::Stone);
    require(p.journalCount()==2 && p.journal(p.chunkOf(b))->microOverrideCount()>=1,
            "spherical MicroBrick state escaped its owner chunk journal");

    // Restoring a touched chunk to baseline compacts the empty journal.
    p.set(a,baseA);
    require(p.journalCount()==1 && p.journal(p.chunkOf(a))==nullptr,"empty touched-chunk journal did not compact");
    p.set(b,baseB); // macro replacement intentionally discards refined state
    require(p.journalCount()==0 && p.persistentCellStateCount()==0,"restoring all touched state left persistent journals behind");
}

void testPlanetSurfaceMicroVolumeAndPersistence() {
    constexpr std::uint64_t seed=0x606D1C20ULL;
    PlanetSurface planet(seed,PlanetClass::Temperate);
    const int r=planet.surfaceRadial(CubeFace::PositiveZ,24,24);
    SurfaceCellAddress cell{CubeFace::PositiveZ,24,24,r};
    const BlockType baseline=planet.get(cell);
    require(blockProperties(baseline).solid,"surface micro test did not choose solid terrain");

    // Prefer a solid microcell: steep columns may procedurally carve the top
    // lip to air, which would no-op setMicro(Air) before refine.
    SurfaceMicroAddress target{cell,8,8,8};
    bool foundSolid=false;
    for (int mr = MicroBrick::Resolution-1; mr >= 0 && !foundSolid; --mr) {
        for (int mv = 0; mv < MicroBrick::Resolution && !foundSolid; ++mv) {
            for (int mu = 0; mu < MicroBrick::Resolution && !foundSolid; ++mu) {
                if (blockProperties(planet.microGet(cell, mu, mr, mv)).solid) {
                    target = {cell, mu, mr, mv};
                    foundSolid = true;
                }
            }
        }
    }
    require(foundSolid, "surface micro test could not find a solid microcell to carve");
    const Vec3 center=planet.microCellCenterPosition(target);
    const auto roundTrip=planet.locateMicro(center);
    require(roundTrip==target,"curved microcell position did not round-trip to its 16^3 address");

    planet.setMicro(cell,target.u,target.radial,target.v,BlockType::Air);
    require(planet.isRefined(cell),"cube-sphere micro edit failed to refine parent macro cell");
    // refineCell materializes regenerable procedural micro into the brick, so
    // override count is often >> 1 after the first player carve on a shaped cell.
    require(planet.microGet(target)==BlockType::Air && planet.microOverrideCount()>=1,
            "cube-sphere micro override was not stored sparsely");

    const auto* brick=planet.microBrick(cell);
    require(brick!=nullptr && !brick->overrides().empty(),"cube-sphere refined brick missing save delta");
    PlanetSurface replay(seed,PlanetClass::Temperate);
    for(const auto& [idx,type]:brick->overrides()) replay.applySavedMicroEdit(planet.flatIndex(cell),idx,type);
    require(replay.microGet(target)==BlockType::Air,"cube-sphere micro save replay failed");

    const auto mesh=buildPlanetSurfaceChunkMesh(planet.snapshot(),planet.chunkOf(cell));
    require(mesh.microQuads>0,"cube-sphere refined cell emitted no micro geometry");
    require(mesh.vertexCount()==mesh.quads*6,"cube-sphere micro mesh violated vertex/quads contract");

    // Collision is volumetric rather than height-field-only: a point in the
    // carved chip is empty while an adjacent untouched subcell remains solid.
    require(!planet.solidAt(center),"micro-carved spherical point still collides as solid");
    SurfaceMicroAddress neighbor = target;
    bool foundNeighbor=false;
    const int nOff[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (const auto& o : nOff) {
        const int mu=target.u+o[0], mr=target.radial+o[1], mv=target.v+o[2];
        if (!MicroBrick::inBounds(mu,mr,mv)) continue;
        if (blockProperties(planet.microGet(cell,mu,mr,mv)).solid) {
            neighbor={cell,mu,mr,mv};
            foundNeighbor=true;
            break;
        }
    }
    require(foundNeighbor && planet.solidAt(planet.microCellCenterPosition(neighbor)),
            "untouched neighboring microcell lost solidity");

    const Vec3 standDir=faceGridCellDirection(CubeFace::PositiveZ,32,32,PlanetSurface::FaceResolution);
    const Vec3 feet=standDir*(planet.surfaceBoundaryRadius(standDir)+0.05f);
    require(!planet.capsuleCollides(feet),"spherical capsule collides while standing above unedited terrain");
    require(planet.groundedAt(feet,0.10f),"spherical volumetric controller failed to detect radial ground");
    require(planet.capsuleCollides(feet-standDir*0.20f),"spherical capsule failed to collide after moving into terrain");
}

void testSurfaceInfrastructureAndAtmosphere() {
    PlanetSurface planet(0x5FACE123ULL,PlanetClass::Barren);
    // Author a 5x5x5 shell in the outer air volume. Interior is 3^3 cells.
    const CubeFace face=CubeFace::PositiveZ;
    const int u0=18,v0=18,r0=24;
    for(int r=r0;r<r0+5;++r) for(int v=v0;v<v0+5;++v) for(int u=u0;u<u0+5;++u) {
        const bool shell=u==u0||u==u0+4||v==v0||v==v0+4||r==r0||r==r0+4;
        planet.set({face,u,v,r},shell?BlockType::SteelPlate:BlockType::Air,true);
    }
    const SurfaceCellAddress inside{face,u0+2,v0+2,r0+2};
    const auto room=planet.sealedVolume(inside,256);
    require(room.sealed && room.cells.size()==27,"cube-sphere sealed-volume query failed on closed room");
    planet.set({face,u0+2,v0+2,r0+4},BlockType::Air,true);
    require(!planet.sealedVolume(inside,4096).sealed,"cube-sphere atmosphere ignored an outward breach");
    planet.set({face,u0+2,v0+2,r0+4},BlockType::SteelPlate,true);

    SurfaceInfrastructure base(planet.seed());
    const auto genId=base.place(MachineType::BurnerGenerator,{face,u0+1,v0+1,r0+1});
    const auto batId=base.place(MachineType::BatteryBank,{face,u0+1,v0+1,r0+2});
    const auto atmId=base.place(MachineType::AtmosphereUnit,inside);
    auto* gen=base.find(genId);
    require(gen!=nullptr,"surface infrastructure lost generator stable object");
    gen->fuelSeconds=10.0f;

    // A closed airlock seals the authored room. Pressure/oxygen then rise over
    // bounded simulation time rather than becoming instantly breathable.
    const SurfaceCellAddress airlockCell{face,u0+2,v0,r0+2};
    planet.set(airlockCell,BlockType::Air,true);
    require(!planet.sealedVolume(inside,4096).sealed,"surface atmosphere fixture did not open to vacuum");
    const auto airlockId=base.placePortal(planet,SurfacePortalType::Airlock,airlockCell,false);
    require(airlockId!=0 && planet.sealedVolume(inside,512).sealed,"closed airlock did not seal atmosphere fixture");

    base.update(planet,3.0f);
    const auto summary=base.summary();
    const auto* atmosphere=base.find(atmId);
    require(summary.networkCount==1 && summary.generation>=19.9f,"surface power network failed to link nearby machines");
    require(atmosphere && atmosphere->powered,"surface critical atmosphere load did not receive power");
    require(base.find(batId)->storedEnergy>0.0f,"surface battery did not charge from surplus generation");
    require(atmosphere->roomSealed && atmosphere->roomPressure>=0.55f && atmosphere->roomOxygen>=0.45f,
            "bounded surface atmosphere did not pressurize/oxygenate sealed room over time");
    require(base.oxygenatedAt(planet,planet.cellCenterPosition(inside),256),"surface atmosphere unit did not make pressurized room breathable");

    // Atmosphere is physical stored room state, not a binary powered flag. An
    // intact room remains breathable during a short power outage even though
    // the Atmosphere Unit itself is no longer powered.
    gen->fuelSeconds=0.0f;
    const float storedPressure=atmosphere->roomPressure;
    base.update(planet,0.25f);
    atmosphere=base.find(atmId);
    require(atmosphere && !atmosphere->powered && std::abs(atmosphere->roomPressure-storedPressure)<1e-5f,
            "sealed room atmosphere vanished or changed during unpowered hold state");
    require(base.oxygenatedAt(planet,planet.cellCenterPosition(inside),256),
            "sealed pressurized room became non-breathable solely because power was lost");
    gen->fuelSeconds=10.0f;
    base.update(planet,0.05f);

    // Opening the airlock creates a real geometry breach. The local atmosphere
    // state vents quickly, then can recover after the portal closes again.
    require(base.setPortalOpen(planet,airlockId,true),"surface airlock failed to open for vent test");
    base.update(planet,1.0f);
    atmosphere=base.find(atmId);
    require(atmosphere && !atmosphere->roomSealed && atmosphere->roomPressure<0.10f && atmosphere->roomOxygen<0.10f,
            "bounded atmosphere did not vent through open airlock");
    require(!base.oxygenatedAt(planet,planet.cellCenterPosition(inside),4096),"vented room remained breathable");

    require(base.setPortalOpen(planet,airlockId,false),"surface airlock failed to close for repressurization test");
    base.update(planet,3.0f);
    atmosphere=base.find(atmId);
    require(atmosphere && atmosphere->roomSealed && atmosphere->roomPressure>=0.55f && atmosphere->roomOxygen>=0.45f,
            "bounded atmosphere did not repressurize after airlock closure");

    SurfaceMachineObject saved=*atmosphere;
    SurfaceInfrastructure restored(planet.seed());
    require(restored.restore(saved) && restored.find(saved.stableId)!=nullptr,
            "surface stable machine object failed save-style restoration");
    require(std::abs(restored.find(saved.stableId)->roomPressure-saved.roomPressure)<1e-5f &&
            std::abs(restored.find(saved.stableId)->roomOxygen-saved.roomOxygen)<1e-5f,
            "surface machine restoration lost persistent atmosphere scalar state");
}


void testSurfaceNavigationAcrossSeamsAndCache() {
    PlanetSurface planet(0xA57A11ULL,PlanetClass::Temperate);
    JobSystem serial(SerialJobs);
    SurfaceChunkCache cache(serial,4,8U*1024U*1024U,4);
    SurfaceWorldReadService read(planet,cache);
    SurfaceNavigationService nav(planet,8,512);

    const Vec3 startDir=faceGridCellDirection(CubeFace::PositiveZ,62,32,PlanetSurface::FaceResolution);
    const Vec3 goalDir=faceGridCellDirection(CubeFace::PositiveX,1,32,PlanetSurface::FaceResolution);
    const Vec3 start=startDir*(read.surfaceBoundaryRadius(startDir)+1.8f);
    const Vec3 goal=goalDir*(read.surfaceBoundaryRadius(goalDir)+1.8f);
    auto first=nav.findPath(read,start,goal);
    require(first.found && first.waypoints.size()>=2,"spherical navigation failed to cross a cube-face seam");
    bool sawX=false;
    for(const auto& p:first.waypoints) if(planet.locate(p).face==CubeFace::PositiveX) sawX=true;
    require(sawX,"spherical navigation route never acquired adjacent cube-face ownership");

    auto second=nav.findPath(read,start,goal);
    require(second.found && second.fromCache,"spherical navigation did not reuse a stable route cache entry");
    const auto cachedStats=nav.stats();
    require(cachedStats.cacheHits>=1 && cachedStats.cacheMisses>=1,"navigation cache telemetry did not record hit/miss");

    // A world edit changes the conservative route revision and must make the
    // next request re-evaluate obstacle assumptions rather than reuse the old path.
    const int er=planet.surfaceRadial(CubeFace::NegativeZ,4,4);
    planet.set({CubeFace::NegativeZ,4,4,er+1},BlockType::SteelPlate,true);
    auto afterEdit=nav.findPath(read,start,goal);
    require(afterEdit.found && !afterEdit.fromCache,"navigation cache reused a route across a changed world revision");
    require(nav.stats().revisionInvalidations>=1,"navigation world-revision invalidation was not observed");

    // Direct terrain step is blocked by a three-metre authored ridge; the A*
    // planner must route around the neighboring column instead of crossing it.
    const CubeFace f=CubeFace::PositiveZ;
    const int v=20;
    for(int u=31;u<=31;++u) {
        const int top=planet.surfaceRadial(f,u,v);
        for(int r=top+1;r<=top+3 && r<PlanetSurface::RadialLayers;++r)
            planet.set({f,u,v,r},BlockType::SteelPlate,true);
    }
    const Vec3 aDir=faceGridCellDirection(f,29,v,PlanetSurface::FaceResolution);
    const Vec3 bDir=faceGridCellDirection(f,33,v,PlanetSurface::FaceResolution);
    const Vec3 a=aDir*(read.surfaceBoundaryRadius(aDir)+1.8f);
    const Vec3 b=bDir*(read.surfaceBoundaryRadius(bDir)+1.8f);
    const auto detour=nav.findPath(read,a,b,1.8f,0.38f,1.6f);
    require(detour.found,"bounded spherical A* could not route around a local authored ridge");
    bool leftRow=false;
    for(const auto& p:detour.waypoints) if(planet.locate(p).v!=v) leftRow=true;
    require(leftRow,"spherical A* crossed a too-tall column instead of detouring");
}

void testSurfaceDoorAirlockSealAndPersistence() {
    PlanetSurface planet(0xD00A10CCULL,PlanetClass::Barren);
    SurfaceInfrastructure infra(planet.seed());
    const CubeFace face=CubeFace::PositiveZ;
    const int u0=18,v0=18,r0=24;
    const SurfaceCellAddress doorCell{face,u0+2,v0,r0+2};
    for(int r=r0;r<r0+5;++r) for(int v=v0;v<v0+5;++v) for(int u=u0;u<u0+5;++u) {
        const bool shell=u==u0||u==u0+4||v==v0||v==v0+4||r==r0||r==r0+4;
        const SurfaceCellAddress cell{face,u,v,r};
        if(cell==doorCell) planet.set(cell,BlockType::Air,true);
        else planet.set(cell,shell?BlockType::SteelPlate:BlockType::Air,true);
    }
    const SurfaceCellAddress inside{face,u0+2,v0+2,r0+2};
    require(!planet.sealedVolume(inside,512).sealed,"test habitat unexpectedly sealed before portal installation");

    const auto doorId=infra.placePortal(planet,SurfacePortalType::Airlock,doorCell,false);
    require(doorId!=0 && planet.get(doorCell)==BlockType::AirlockPanel,"airlock placement did not author its sealed voxel state");
    require(planet.sealedVolume(inside,512).sealed,"closed airlock did not seal the bounded atmosphere volume");
    require(infra.setPortalOpen(planet,doorId,true) && planet.get(doorCell)==BlockType::Air,"opening airlock did not create a gas-passable breach");
    require(!planet.sealedVolume(inside,4096).sealed,"open airlock incorrectly left room sealed");
    require(infra.setPortalOpen(planet,doorId,false) && planet.sealedVolume(inside,512).sealed,"closing airlock did not restore room seal");

    namespace fs=std::filesystem;
    const fs::path root=fs::temp_directory_path()/"elysium_v013_portal_store";
    std::error_code ec; fs::remove_all(root,ec);
    const auto owner=planet.chunkOf(doorCell);
    const auto atmosphereId=infra.place(MachineType::AtmosphereUnit,inside);
    auto* atmosphere=infra.find(atmosphereId);
    require(atmosphere!=nullptr,"portal persistence fixture failed to create atmosphere unit");
    atmosphere->roomPressure=0.73f;
    atmosphere->roomOxygen=0.61f;

    SurfaceChunkStore store(root,0,planet.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
    const auto record=makeSurfaceChunkRecord(0,planet,infra,owner,73);
    require(record.portals.size()==1 && record.portals.front().stableId==doorId,"portal was not grouped into its touched chunk transaction");
    require(record.machines.size()==1 && record.machines.front().stableId==atmosphereId,"atmosphere machine was not grouped into portal chunk transaction");
    std::string error;
    require(store.save(record,&error),"portal sidecar transaction save failed: "+error);
    const auto loaded=store.load(owner,73);
    require(loaded.loaded && loaded.record.portals.size()==1,"portal sidecar transaction round trip lost stable interactive object");
    require(loaded.record.portals.front().type==SurfacePortalType::Airlock && !loaded.record.portals.front().open,
            "airlock type/open state changed during sidecar round trip");
    require(loaded.record.machines.size()==1 &&
            std::abs(loaded.record.machines.front().roomPressure-0.73f)<1e-5f &&
            std::abs(loaded.record.machines.front().roomOxygen-0.61f)<1e-5f,
            "sidecar payload v3 lost bounded atmosphere pressure/oxygen state");
    fs::remove_all(root,ec);
}


void testPoweredAirlockInterlockAndPersistence() {
    PlanetSurface planet(0xA14C0CCULL,PlanetClass::Barren);
    SurfaceInfrastructure infra(planet.seed());
    const CubeFace face=CubeFace::PositiveZ;
    const int u=22,v=22,r=25;
    const SurfaceCellAddress controllerCell{face,u,v,r};
    const SurfaceCellAddress innerCell{face,u,v-1,r};
    const SurfaceCellAddress outerCell{face,u,v+1,r};

    // Keep the three fixture cells in air so portal voxels are the only
    // authoritative blockers introduced by the interlock.
    planet.set(controllerCell,BlockType::Air,true);
    planet.set(innerCell,BlockType::Air,true);
    planet.set(outerCell,BlockType::Air,true);

    const auto generatorId=infra.place(MachineType::BurnerGenerator,{face,u-1,v,r});
    const auto controllerId=infra.place(MachineType::AirlockController,controllerCell);
    auto* generator=infra.find(generatorId);
    require(generator!=nullptr,"airlock fixture lost generator");
    generator->fuelSeconds=30.0f;
    const auto innerId=infra.placePortal(planet,SurfacePortalType::Airlock,innerCell,false);
    const auto outerId=infra.placePortal(planet,SurfacePortalType::Airlock,outerCell,false);
    require(innerId!=0 && outerId!=0,"airlock fixture could not place two portal objects");

    const auto assemblyId=infra.createAirlockAssembly(planet,controllerId,innerId,outerId,controllerCell,1.0f,1.0f);
    require(assemblyId!=0,"powered airlock assembly rejected valid controller/portal tuple");
    require(!infra.findPortal(innerId)->open && !infra.findPortal(outerId)->open,"new airlock assembly did not fail closed");
    require(!infra.setPortalOpen(planet,innerId,true),"interlocked portal accepted direct manual open command");

    infra.update(planet,0.1f);
    require(infra.find(controllerId)->powered,"airlock controller did not receive scalar network power");
    require(infra.requestAirlockCycle(planet,assemblyId,SurfaceAirlockDestination::Exterior),"airlock exterior cycle request failed");
    infra.update(planet,2.0f);
    auto* assembly=infra.findAirlockAssembly(assemblyId);
    require(assembly && assembly->state==SurfaceAirlockState::ExteriorOpen,"airlock did not finish depressurization/exterior-open cycle");
    require(!infra.findPortal(innerId)->open && infra.findPortal(outerId)->open,"airlock exterior cycle violated door interlock");
    require(assembly->chamberPressure<=0.001f && assembly->chamberOxygen<=0.001f,"airlock chamber did not vent before exterior opening");
    auto sample=infra.atmosphereAt(planet,planet.cellCenterPosition(controllerCell),64);
    require(sample.airlockChamber && !sample.breathable(),"vented airlock chamber incorrectly reported breathable atmosphere");

    require(infra.requestAirlockCycle(planet,assemblyId,SurfaceAirlockDestination::Interior),"airlock interior cycle request failed");
    infra.update(planet,2.0f);
    assembly=infra.findAirlockAssembly(assemblyId);
    require(assembly && assembly->state==SurfaceAirlockState::InteriorOpen,"airlock did not finish repressurization/interior-open cycle");
    require(infra.findPortal(innerId)->open && !infra.findPortal(outerId)->open,"airlock interior cycle violated door interlock");
    sample=infra.atmosphereAt(planet,planet.cellCenterPosition(controllerCell),64);
    require(sample.airlockChamber && sample.breathable(),"pressurized airlock chamber did not report breathable local atmosphere");

    // Brownout is fail-closed and pauses the requested transition rather than
    // exposing both sides or inventing pump work without power.
    generator->fuelSeconds=0.0f;
    require(infra.requestAirlockCycle(planet,assemblyId,SurfaceAirlockDestination::Exterior),"brownout transition request was rejected before power update");
    const float pressureBefore=infra.findAirlockAssembly(assemblyId)->chamberPressure;
    infra.update(planet,0.5f);
    assembly=infra.findAirlockAssembly(assemblyId);
    require(!infra.findPortal(innerId)->open && !infra.findPortal(outerId)->open,"airlock brownout did not fail closed");
    require(std::abs(assembly->chamberPressure-pressureBefore)<1e-5f,"unpowered airlock pump changed chamber pressure");

    // Sidecar v4 persists the stable controller/portal links and chamber state.
    generator->fuelSeconds=30.0f;
    infra.update(planet,0.1f);
    const auto owner=planet.chunkOf(controllerCell);
    namespace fs=std::filesystem;
    const fs::path root=fs::temp_directory_path()/"elysium_v014_airlock_store";
    std::error_code ec; fs::remove_all(root,ec);
    SurfaceChunkStore store(root,0,planet.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
    const auto record=makeSurfaceChunkRecord(0,planet,infra,owner,140);
    require(record.airlocks.size()==1 && record.airlocks.front().stableId==assemblyId,"airlock assembly was not grouped into chamber owner sidecar");
    require(record.machines.size()>=1,"airlock controller machine was not present in owner sidecar");
    std::string error;
    require(store.save(record,&error),"airlock v4 sidecar save failed: "+error);
    const auto loaded=store.load(owner,140);
    require(loaded.loaded && loaded.record.airlocks.size()==1,"airlock v4 sidecar round trip lost assembly");
    const auto& saved=loaded.record.airlocks.front();
    require(saved.controllerMachineId==controllerId && saved.innerPortalId==innerId && saved.outerPortalId==outerId,
            "airlock v4 sidecar changed stable object links");
    require(saved.state==assembly->state && std::abs(saved.chamberPressure-assembly->chamberPressure)<1e-5f,
            "airlock v4 sidecar changed cycle/pressure state");
    fs::remove_all(root,ec);
}

void testMicroBrickStorage() {
    MicroBrick brick(BlockType::Stone);
    require(brick.get(4,5,6) == BlockType::Stone, "micro baseline mismatch");
    brick.set(4,5,6,BlockType::Air);
    require(brick.overrideCount() == 1 && brick.get(4,5,6) == BlockType::Air, "sparse micro edit failed");
    brick.set(4,5,6,BlockType::Stone);
    require(brick.overrideCount() == 0, "reverting micro edit did not compact sparse state");

    for (int i = 0; i <= MicroBrick::DensePromotionThreshold; ++i) brick.setIndex(i,BlockType::Air);
    require(brick.storageMode() == MicroBrick::StorageMode::Dense, "micro brick did not promote to dense storage");
    require(brick.overrideCount() == static_cast<std::size_t>(MicroBrick::DensePromotionThreshold + 1), "dense override count mismatch");
}

void testWorldDeterminismAndMicroReplay() {
    constexpr std::uint64_t seed = 0xC001D00D1234ULL;
    World a(seed,PlanetClass::Temperate);
    World b(seed,PlanetClass::Temperate);
    for (int y=0;y<World::Height;++y) for (int z=0;z<World::Depth;++z) for (int x=0;x<World::Width;++x)
        require(a.get(x,y,z) == b.get(x,y,z), "same seed generated different voxels");

    const int x=World::Width/2, z=World::Depth/2, y=a.surfaceY(x,z);
    const BlockType original=a.get(x,y,z);
    a.setMicro(x,y,z,15,15,15,BlockType::Air);
    require(a.isRefined(x,y,z), "world failed to allocate MicroBrick");
    require(a.microGet(x,y,z,15,15,15) == BlockType::Air, "world micro edit not visible");

    const int flat=a.flatIndex(x,y,z);
    const auto* brick=a.microBrick(x,y,z);
    require(brick != nullptr, "missing refined brick");
    const auto deltas=brick->overrides();
    require(deltas.size()==1, "unexpected micro delta count");

    World replay(seed,PlanetClass::Temperate);
    for (const auto& [idx,type] : deltas) replay.applySavedMicroEdit(flat,idx,type);
    require(replay.get(x,y,z)==original, "micro replay changed macro baseline");
    require(replay.microGet(x,y,z,15,15,15)==BlockType::Air, "micro replay failed");
}

WorldSnapshot emptySnapshot() {
    WorldSnapshot s{};
    s.revision=1;
    s.blocks.assign(static_cast<std::size_t>(WorldSnapshot::Width*WorldSnapshot::Height*WorldSnapshot::Depth),BlockType::Air);
    return s;
}

void testGreedyMeshingAndExteriorAir() {
    WorldSnapshot s=emptySnapshot();
    auto put=[&](int x,int y,int z,BlockType t){ s.blocks[static_cast<std::size_t>(s.flatIndex(x,y,z))]=t; };
    for(int y=2;y<4;++y) for(int z=2;z<4;++z) for(int x=2;x<4;++x) put(x,y,z,BlockType::Stone);
    s.computeExteriorAir();
    const CpuMeshData m=buildChunkMesh(s,0,0,0);
    require(m.macroQuads==6, "greedy mesher failed to merge a 2x2x2 cube into six quads");
    require(m.triangleCount()==12, "greedy cube triangle count mismatch");

    WorldSnapshot cavity=emptySnapshot();
    auto put2=[&](int x,int y,int z,BlockType t){ cavity.blocks[static_cast<std::size_t>(cavity.flatIndex(x,y,z))]=t; };
    for(int y=5;y<8;++y) for(int z=5;z<8;++z) for(int x=5;x<8;++x) put2(x,y,z,BlockType::Stone);
    put2(6,6,6,BlockType::Air); // sealed cavity
    cavity.computeExteriorAir();
    const CpuMeshData c=buildChunkMesh(cavity,0,0,0);
    require(c.macroQuads==6, "sealed cavity emitted internal faces");
}

void testMicroMeshing() {
    WorldSnapshot s=emptySnapshot();
    const int x=3,y=3,z=3;
    s.blocks[static_cast<std::size_t>(s.flatIndex(x,y,z))]=BlockType::Stone;
    MicroBrick brick(BlockType::Stone);
    brick.set(15,15,15,BlockType::Air);
    s.microBricks.emplace(s.flatIndex(x,y,z),brick);
    s.computeExteriorAir();
    const CpuMeshData m=buildChunkMesh(s,0,0,0);
    require(m.microQuads>6, "refined cell did not emit micro geometry");
    require(m.macroQuads==0, "refined cell also emitted duplicate macro geometry");
}

void testJobSystem() {
    JobSystem jobs(2);
    std::vector<std::future<std::uint64_t>> futures;
    for (std::uint64_t i=0;i<32;++i) futures.push_back(jobs.submit([i](){ return i*i; }));
    std::uint64_t sum=0;
    for(auto& f:futures) sum+=f.get();
    require(sum==10416, "job system returned wrong deterministic aggregate");

    std::vector<std::uint64_t> parallel(257);
    jobs.parallelFor(parallel.size(),[&](std::size_t i){ parallel[i]=static_cast<std::uint64_t>(i*i); },13);
    std::uint64_t parallelSum=0;
    for(const auto v:parallel) parallelSum+=v;
    std::uint64_t serialSum=0;
    for(std::uint64_t i=0;i<parallel.size();++i) serialSum+=i*i;
    require(parallelSum==serialSum,"parallelFor changed deterministic partition output");

    // Nested parallelFor from a worker must fall back to serial execution
    // rather than deadlocking the bounded worker pool.
    auto nested=jobs.submit([&jobs]() {
        std::uint64_t nestedSum=0;
        jobs.parallelFor(64,[&](std::size_t i){ nestedSum+=static_cast<std::uint64_t>(i); },4);
        return nestedSum;
    });
    require(nested.get()==2016,"nested worker parallelFor did not complete serially/deterministically");

    JobSystem serial(SerialJobs);
    require(serial.serialMode() && serial.workerCount()==0,"explicit zero-worker JobSystem did not enter serial mode");
    auto inlineFuture=serial.submit([](){return std::uint64_t{77};});
    require(inlineFuture.get()==77 && serial.queuedJobs()==0,"serial JobSystem submit did not execute inline");
    std::uint64_t serialParallel=0;
    serial.parallelFor(128,[&](std::size_t i){serialParallel+=static_cast<std::uint64_t>(i);},7);
    require(serialParallel==8128,"serial zero-worker parallelFor changed deterministic output");
}


void settleSurfaceCache(SurfaceChunkCache& cache, const PlanetSurface& planet) {
    for (int i=0;i<1000 && cache.stats().pending>0;++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cache.sync(planet);
    }
    cache.sync(planet);
    require(cache.stats().pending==0,"surface chunk cache jobs failed to settle");
}


std::uint64_t hashMeshPacket(const CpuMeshData& mesh) {
    std::uint64_t h=1469598103934665603ULL;
    auto mix=[&](std::uint64_t v) {
        h^=v;
        h*=1099511628211ULL;
    };
    mix(static_cast<std::uint64_t>(mesh.quads));
    mix(static_cast<std::uint64_t>(mesh.macroQuads));
    mix(static_cast<std::uint64_t>(mesh.microQuads));
    mix(static_cast<std::uint64_t>(mesh.aoDarkenedCorners));
    for(float v:mesh.vertices) mix(std::bit_cast<std::uint32_t>(v));
    for(float v:mesh.normals) mix(std::bit_cast<std::uint32_t>(v));
    for(const auto c:mesh.colors) mix(c);
    for(const auto& r:mesh.materialRanges) {
        mix(static_cast<std::uint64_t>(r.firstVertex));
        mix(static_cast<std::uint64_t>(r.vertexCount));
        mix(static_cast<std::uint64_t>(r.material));
    }
    return h;
}

std::uint64_t captureSurfaceMeshHash(JobSystem& jobs) {
    // Barren keeps procedural micro sparse so worker-count hashing stays fast while
    // still exercising cache build + seam chunks + a player micro edit.
    PlanetSurface planet(0xD37E4D11ULL,PlanetClass::Barren);
    const int u=31,v=15,r=planet.surfaceRadial(CubeFace::PositiveZ,u,v);
    planet.set({CubeFace::PositiveZ,u,v,r+1},BlockType::SteelPlate,true);
    planet.setMicro({CubeFace::PositiveZ,u,v,r},15,15,7,BlockType::Air);

    SurfaceChunkCache cache(jobs,8,16U*1024U*1024U,8);
    const std::array<PlanetChunkAddress,2> addresses{{
        {CubeFace::PositiveZ,0,0,0},
        {CubeFace::PositiveX,0,0,0}
    }};
    cache.beginFrame();
    for(const auto& a:addresses) require(cache.request(planet,a,SurfaceChunkPriority::Visible),"determinism cache request failed");
    settleSurfaceCache(cache,planet);

    std::uint64_t h=1469598103934665603ULL;
    for(const auto& a:addresses) {
        const auto data=cache.find(a);
        require(data!=nullptr,"determinism cache packet missing");
        const auto mesh=buildPlanetSurfaceChunkMesh(*data);
        h^=hashMeshPacket(mesh);
        h*=1099511628211ULL;
    }
    return h;
}

std::uint64_t captureSurfaceMeshHashForWorkers(std::size_t workers) {
    JobSystem jobs(workers);
    return captureSurfaceMeshHash(jobs);
}

std::uint64_t captureSurfaceMeshHashSerial() {
    JobSystem jobs(SerialJobs);
    return captureSurfaceMeshHash(jobs);
}

void testWorkerCountMeshDeterminism() {
    const auto h0=captureSurfaceMeshHashSerial();
    const auto h1=captureSurfaceMeshHashForWorkers(1);
    const auto h2=captureSurfaceMeshHashForWorkers(2);
    const auto h4=captureSurfaceMeshHashForWorkers(4);
    const auto h8=captureSurfaceMeshHashForWorkers(8);
    require(h0==h1 && h1==h2 && h1==h4 && h1==h8,
            "surface reconstruction/meshing changed across 0/1/2/4/8 worker counts");
}

void testEditInfluenceSummary() {
    PlanetSurface planet(0x1F1E11CEULL,PlanetClass::Temperate);
    const CubeFace face=CubeFace::PositiveZ;
    const int u=10,v=12;
    const int surface=planet.surfaceRadial(face,u,v);
    const PlanetChunkAddress chunk=planet.chunkOf({face,u,v,std::max(0,surface)});
    require(!planet.editInfluenceSummary(chunk).any,"clean chunk unexpectedly reported edit influence");

    const SurfaceCellAddress tower{face,u,v,std::min(PlanetSurface::RadialLayers-1,surface+2)};
    require(!blockProperties(planet.baseline(tower)).solid,"edit-influence tower fixture baseline is not air");
    planet.set(tower,BlockType::SteelPlate,true);

    const int pitSurface=planet.surfaceRadial(face,u+2,v+1);
    const SurfaceCellAddress pit{face,u+2,v+1,std::max(0,pitSurface-3)};
    const BlockType pitBaseline=planet.baseline(pit);
    require(blockProperties(pitBaseline).solid,"edit-influence pit fixture baseline is not solid");
    planet.set(pit,BlockType::Air);

    const int refinedSurface=planet.surfaceRadial(face,u+1,v+2);
    const SurfaceCellAddress refined{face,u+1,v+2,refinedSurface};
    const BlockType microBaseline=planet.microGet(refined,5,7,9);
    planet.setMicro(refined,5,7,9,BlockType::Air);

    const auto summary=planet.editInfluenceSummary(chunk);
    require(summary.any && summary.addedSolidCells>=1 && summary.removedSolidCells>=1,
            "edit influence summary did not classify added/removed solid cells");
    require(summary.refinedCells>=1 && summary.microOverrides>=1,
            "edit influence summary omitted refined micro state");
    require(summary.maxOutwardDelta>=1 && summary.maxInwardDepth>=1 && summary.significanceScore()>0,
            "edit influence summary did not preserve vertical/significance information");
    require(summary.intersects(u,v,u+4,v+4) && !summary.intersects(40,40,48,48),
            "edit influence summary bounds/intersection contract failed");

    const auto snap=planet.snapshot();
    const auto snapSummary=snap.editInfluence(chunk);
    require(snapSummary.any && snapSummary.significanceScore()==summary.significanceScore(),
            "worker snapshot did not carry derived edit influence summary");
    require(snap.hasEditInfluence(face,u,v,u+4,v+4),
            "summary-backed field influence query missed edited patch");

    // Restoring deterministic state compacts both the journal and its derived
    // summary; the summary itself is never persistent world truth.
    planet.set(tower,planet.baseline(tower));
    planet.set(pit,pitBaseline);
    planet.setMicro(refined,5,7,9,microBaseline);
    require(!planet.editInfluenceSummary(chunk).any && planet.journal(chunk)==nullptr,
            "restoring baseline left stale edit influence/journal state");
}

void testSurfaceWorldReadUsesCacheAndFallback() {
    PlanetSurface planet(0xCACEB00CULL,PlanetClass::Temperate);
    JobSystem jobs(2);
    SurfaceChunkCache cache(jobs,3,8U*1024U*1024U,3);
    const PlanetChunkAddress a{CubeFace::PositiveZ,0,0,0};
    const int u=8,v=9,r=planet.surfaceRadial(a.face,u,v);
    const SurfaceCellAddress cell{a.face,u,v,r};
    const BlockType original=planet.get(cell);
    planet.setMicro(cell,3,8,4,BlockType::Air);

    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::Visible),"world-read cache request failed");
    settleSurfaceCache(cache,planet);
    SurfaceWorldReadService read(planet,cache);

    require(read.get(cell)==planet.get(cell),"cached gameplay macro read disagreed with PlanetSurface");
    const SurfaceMicroAddress micro{cell,3,8,4};
    require(read.microGet(micro)==planet.microGet(micro),"cached gameplay micro read disagreed with PlanetSurface");
    const auto center=planet.microCellCenterPosition(micro);
    require(read.solidAt(center)==planet.solidAt(center),"cached gameplay solidity disagreed with authoritative query");
    auto stats=read.stats();
    require(stats.cachedMacroReads>0 && stats.cachedMicroReads>0,
            "world-read service did not consume resident reconstructed chunk data");

    // Query an uncached face: deterministic fallback must remain correct while
    // asynchronous streaming catches up.
    const int bu=7,bv=7,br=planet.surfaceRadial(CubeFace::NegativeX,bu,bv);
    const SurfaceCellAddress fallback{CubeFace::NegativeX,bu,bv,br};
    require(read.get(fallback)==planet.get(fallback),"procedural fallback read disagreed with PlanetSurface");
    stats=read.stats();
    require(stats.proceduralFallbackReads>0 && stats.staleOrMissingChunks>0,
            "world-read fallback/miss telemetry was not recorded");

    // Edits invalidate resident revisions immediately. Reads must fall back to
    // fresh authoritative state instead of using the stale packet.
    planet.set(cell,BlockType::Air);
    const auto fallbackBefore=read.stats().proceduralFallbackReads;
    require(read.get(cell)==BlockType::Air,"world-read service returned stale cached voxel after edit");
    require(read.stats().proceduralFallbackReads>fallbackBefore,"stale resident chunk did not trigger deterministic fallback");

    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::EditRemesh),"world-read edited cache rebuild request failed");
    settleSurfaceCache(cache,planet);
    read.resetStats();
    require(read.get(cell)==BlockType::Air && read.stats().cachedMacroReads>0,
            "world-read service did not return to cache after edited chunk reconstruction");

    // Restore for fixture sanity (also exercises refinement discard by macro set).
    planet.set(cell,original);
}

void testSurfaceChunkCacheBudgetCancellationAndRebuild() {
    PlanetSurface planet(0xCA5EULL,PlanetClass::Temperate);
    JobSystem jobs(1);
    SurfaceChunkCache cache(jobs,2,4U*1024U*1024U,1);
    const PlanetChunkAddress a{CubeFace::PositiveZ,0,0,0};
    const PlanetChunkAddress b{CubeFace::PositiveZ,1,0,0};
    const PlanetChunkAddress c{CubeFace::PositiveX,0,0,0};

    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::Visible),"visible chunk cache request was rejected");
    require(!cache.request(planet,b,SurfaceChunkPriority::Prefetch),"bounded cache queue failed to drop excess prefetch");
    settleSurfaceCache(cache,planet);
    require(cache.find(a)!=nullptr && cache.stats().resident==1,"requested spherical chunk did not become resident");
    require(cache.stats().droppedPrefetch>=1,"prefetch back-pressure was not recorded");

    // Reconstructed chunk data must match authoritative deterministic queries.
    const auto data=cache.find(a);
    const int worldU=a.u*PlanetSurface::ChunkSize+7;
    const int worldV=a.v*PlanetSurface::ChunkSize+9;
    const int worldR=12;
    require(data->getLocal(7,9,worldR)==planet.get(a.face,worldU,worldV,worldR),
            "cached reconstructed baseline disagrees with PlanetSurface");

    // Move the wanted set. Unwanted resident data can be evicted once the
    // bounded cache publishes new visible chunks.
    cache.beginFrame();
    require(cache.request(planet,b,SurfaceChunkPriority::Visible),"second visible cache request failed");
    cache.cancelUnwanted();
    settleSurfaceCache(cache,planet);
    cache.beginFrame();
    require(cache.request(planet,b,SurfaceChunkPriority::Visible),"resident cache touch failed");
    require(cache.request(planet,c,SurfaceChunkPriority::Visible),"third visible cache request failed");
    cache.cancelUnwanted();
    settleSurfaceCache(cache,planet);
    require(cache.stats().resident<=2,"surface chunk cache exceeded configured resident chunk budget");
    require(cache.find(b)!=nullptr && cache.find(c)!=nullptr,"LRU cache evicted currently wanted chunks");
    require(cache.stats().evictions>=1,"bounded cache never evicted an unwanted reconstructed chunk");

    // A world edit after generation was requested invalidates the result by
    // chunk revision rather than publishing stale reconstructed data.
    cache.clear();
    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::Visible),"stale-result test request failed");
    SurfaceCellAddress edit{a.face,4,4,planet.surfaceRadial(a.face,4,4)};
    planet.set(edit,BlockType::Air);
    settleSurfaceCache(cache,planet);
    require(cache.find(a)==nullptr && cache.stats().staleResults>=1,"cache published a result generated against a stale chunk revision");

    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::EditRemesh),"post-edit cache rebuild request failed");
    settleSurfaceCache(cache,planet);
    require(cache.find(a)!=nullptr,"edited chunk failed to rebuild into cache after stale rejection");
}

void testSurfaceChunkCacheRetainAndPriorityPreempt() {
    PlanetSurface planet(0x57EED411ULL,PlanetClass::Temperate);
    // Serial jobs finish inline but publication waits for sync — this lets us
    // observe retained geometry and pending-slot preemption deterministically.
    JobSystem jobs(SerialJobs);
    SurfaceChunkCache cache(jobs,4,8U*1024U*1024U,1);
    const PlanetChunkAddress a{CubeFace::PositiveZ,0,0,0};
    const PlanetChunkAddress b{CubeFace::PositiveZ,1,0,0};
    const PlanetChunkAddress c{CubeFace::PositiveX,0,0,0};

    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::Visible),"retain fixture visible request failed");
    settleSurfaceCache(cache,planet);
    const auto before=cache.find(a);
    require(before!=nullptr,"retain fixture chunk never became resident");
    const auto beforeRevision=before->revision;
    const BlockType sample=before->getLocal(5,6,8);

    // Edit bumps revision; remesh must keep the old packet until publish.
    SurfaceCellAddress edit{a.face,5,6,planet.surfaceRadial(a.face,5,6)};
    planet.set(edit,BlockType::Air);
    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::EditRemesh),"edit-remesh retain request failed");
    const auto retained=cache.find(a);
    require(retained!=nullptr && retained.get()==before.get(),
            "edit remesh cleared retained reconstructed geometry before replacement");
    require(retained->revision==beforeRevision && retained->getLocal(5,6,8)==sample,
            "retained geometry changed identity/content before atomic publish");
    require(cache.stats().pending==1 && cache.stats().retainedWhileRebuilding>=1,
            "retained-while-rebuilding telemetry missing while remesh pending");
    settleSurfaceCache(cache,planet);
    const auto after=cache.find(a);
    require(after!=nullptr && after->revision==planet.chunkRevision(a) && after->revision!=beforeRevision,
            "edit remesh did not atomically publish the newer revision");
    require(after.get()!=before.get(),"publish did not replace retained packet pointer");

    // Hard pending bound: Prefetch is disposable under back-pressure.
    cache.clear();
    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::Prefetch),"prefetch seed request failed");
    require(cache.stats().pending==1,"serial prefetch did not occupy the single pending slot");
    require(!cache.request(planet,b,SurfaceChunkPriority::Prefetch),"second prefetch should be dropped at bound");
    require(cache.stats().droppedPrefetch>=1,"prefetch drop counter missing");

    // Visible may preempt Prefetch to claim the bounded slot.
    require(cache.request(planet,b,SurfaceChunkPriority::Visible),"visible failed to preempt prefetch pending slot");
    require(cache.stats().priorityPreemptions>=1,"priority preemption was not recorded");
    require(cache.stats().pending==1,"preemption left more than maxPending in flight");

    // Equal-priority Visible cannot grow the queue; EditRemesh can preempt Visible.
    settleSurfaceCache(cache,planet);
    cache.clear();
    cache.beginFrame();
    require(cache.request(planet,a,SurfaceChunkPriority::Visible),"visible seed for reject test failed");
    require(!cache.request(planet,b,SurfaceChunkPriority::Visible),
            "bounded queue accepted a second equal-priority Visible without a free slot");
    require(cache.stats().droppedVisible>=1 && cache.stats().boundedQueueRejects>=1,
            "visible bounded-queue reject telemetry missing");
    require(cache.request(planet,c,SurfaceChunkPriority::EditRemesh),
            "edit-remesh failed to preempt a visible pending slot");
    require(cache.stats().priorityPreemptions>=2,"edit-remesh preemption of visible was not recorded");
    settleSurfaceCache(cache,planet);
    require(cache.find(c)!=nullptr,"edit-remesh preemption victim never published");
}

class FakeGraphicsBackend final : public IGraphicsBackend {
public:
    GraphicsMeshHandle uploadMesh(const CpuMeshData& data) override {
        const auto id=next_++;
        live_[id]=data.vertexCount();
        ++uploads;
        return {id};
    }
    void destroyMesh(GraphicsMeshHandle handle) override {
        if (!handle) return;
        live_.erase(handle.value);
        ++destroys;
    }
    void drawMesh(GraphicsMeshHandle handle) const override {
        if (handle && live_.contains(handle.value)) ++draws;
    }

    mutable int draws{};
    int uploads{};
    int destroys{};
    std::size_t liveCount() const { return live_.size(); }
private:
    std::uint32_t next_{1};
    std::unordered_map<std::uint32_t,int> live_;
};

void settlePlanetRenderer(PlanetSurfaceRenderer& renderer, const PlanetSurface& planet) {
    renderer.sync(planet);
    for(int i=0;i<1600 && (renderer.pendingJobs()>0 || renderer.dirtyChunks()>0);++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        renderer.sync(planet);
    }
    require(renderer.pendingJobs()==0,"planet renderer jobs failed to settle");
    require(renderer.dirtyChunks()==0,"planet renderer remained dirty after jobs settled");
}

void testPlanetSurfaceMeshingAndRenderer() {
    PlanetSurface planet(0x0B17A1ULL,PlanetClass::Scorched);
    const CpuMeshData mesh=buildPlanetSurfaceMesh(planet.snapshot());
    require(mesh.quads>=PlanetSurface::FaceCount*PlanetSurface::FaceResolution*PlanetSurface::FaceResolution,
            "planet shell mesh is missing top-surface coverage");
    require(mesh.vertexCount()==mesh.quads*6,"planet shell mesh vertex/quads contract mismatch");
    int rangeVertices=0;
    for(const auto& range:mesh.materialRanges) rangeVertices+=range.vertexCount;
    require(rangeVertices==mesh.vertexCount(),"planet shell material ranges do not cover mesh");
    for(float v:mesh.vertices) require(std::isfinite(v),"planet shell emitted non-finite vertex");

    // Each 32x32 face chunk can be built independently while still sampling
    // neighbors from the full immutable snapshot.
    const auto snapshot=planet.snapshot();
    const auto chunkAddress0=PlanetChunkAddress{CubeFace::PositiveZ,0,0,0};
    const auto chunkMesh=buildPlanetSurfaceChunkMesh(snapshot,chunkAddress0);
    require(!chunkMesh.empty(),"planet surface chunk mesher returned empty visible chunk");
    const auto fieldMesh=buildPlanetSurfaceFieldChunkMesh(snapshot,chunkAddress0,4);
    const auto farFieldMesh=buildPlanetSurfaceFieldChunkMesh(snapshot,chunkAddress0,8);
    require(!fieldMesh.empty() && !farFieldMesh.empty(),"planet field-LOD mesher returned empty chunk proxy");
    require(fieldMesh.quads < chunkMesh.quads,"near field LOD did not reduce spherical geometry density");
    require(farFieldMesh.quads < fieldMesh.quads,"far field LOD did not reduce geometry beyond near field");
    for(float v:fieldMesh.vertices) require(std::isfinite(v),"field LOD emitted non-finite vertex");
    const auto climateShell=buildPlanetOrbitalClimateShellMesh(snapshot,8);
    const auto cloudShell=buildPlanetOrbitalCloudShellMesh(snapshot,8);
    require(!climateShell.empty() && climateShell.quads==PlanetSurface::FaceCount*64,
            "orbital climate shell did not produce deterministic coarse face coverage");
    require(!cloudShell.empty(),"orbital cloud shell generated no deterministic cloud patches");
    require(climateShell.quads < fieldMesh.quads*PlanetSurface::ChunkCount,
            "orbital climate shell is not cheaper than chunk field rendering");
    for(float v:climateShell.vertices) require(std::isfinite(v),"orbital climate shell emitted non-finite vertex");

    JobSystem jobs(2);
    FakeGraphicsBackend fake;
    {
        PlanetSurfaceRenderer renderer(jobs,fake);
        settlePlanetRenderer(renderer,planet);
        require(renderer.ready(),"planet surface renderer failed to publish all chunk meshes");
        require(fake.uploads==PlanetSurface::ChunkCount && fake.liveCount()==PlanetSurface::ChunkCount,
                "planet surface renderer did not publish exactly one resource per surface chunk");
        renderer.draw();
        require(fake.draws==PlanetSurface::ChunkCount,"planet surface renderer did not draw every published chunk");

        // Ordinary edit inside a chunk invalidates only the owner.
        SurfaceCellAddress local{CubeFace::PositiveZ,10,10,planet.surfaceRadial(CubeFace::PositiveZ,10,10)};
        const int before=fake.uploads;
        planet.set(local,BlockType::Air);
        settlePlanetRenderer(renderer,planet);
        require(fake.uploads==before+1,"local spherical edit rebuilt more than one surface chunk");

        // Chunk-edge edit invalidates the owner and its seam-dependent neighbor.
        SurfaceCellAddress edge{CubeFace::PositiveZ,31,12,planet.surfaceRadial(CubeFace::PositiveZ,31,12)};
        const int edgeBefore=fake.uploads;
        planet.set(edge,BlockType::Air);
        settlePlanetRenderer(renderer,planet);
        require(fake.uploads==edgeBefore+2,"spherical chunk-edge edit did not rebuild exactly two chunks");

        // A physical cube-face seam is also a dependency seam: the face owner
        // and the adjacent face chunk both need a new packet.
        SurfaceCellAddress faceEdge{CubeFace::PositiveZ,63,18,planet.surfaceRadial(CubeFace::PositiveZ,63,18)};
        const int faceBefore=fake.uploads;
        planet.set(faceEdge,BlockType::Air);
        settlePlanetRenderer(renderer,planet);
        require(fake.uploads==faceBefore+2,"cube-face seam edit did not rebuild owner + adjacent face chunks");

        // v0.7 streaming: only a bounded nearest set remains at full editable
        // detail. Other chunks are replaced by cheap field packets, while the
        // graphics resource count stays bounded and complete for the silhouette.
        const Vec3 focus=planet.cellCenterPosition({CubeFace::PositiveZ,16,16,planet.surfaceRadial(CubeFace::PositiveZ,16,16)});
        renderer.setStreamingFocus(focus,6,9);
        settlePlanetRenderer(renderer,planet);
        require(renderer.fullDetailChunks()==6 && renderer.nearFieldChunks()==9 && renderer.farFieldChunks()==PlanetSurface::ChunkCount-15,
                "surface renderer did not enforce three-tier residency budgets");
        require(fake.liveCount()==PlanetSurface::ChunkCount,"streaming surface renderer lost field proxy coverage");
        const auto cpuResidency=renderer.cpuCacheStats();
        require(cpuResidency.resident>=6 && cpuResidency.resident<=10,
                "runtime spherical renderer did not maintain bounded CPU LOD0/prefetch residency");

        renderer.setStreamingFocus(focus*-1.0f,6,9);
        settlePlanetRenderer(renderer,planet);
        require(renderer.fullDetailChunks()==6 && renderer.targetFullDetailChunks()==6,
                "moving streaming focus changed the requested LOD0 budget");

        // v0.11 preserves player meaning inside far-field packets with localized
        // adaptive tiles instead of promoting an entire edited chunk.
        const PlanetSurface cleanForInfluence(0x0B17A1ULL,PlanetClass::Scorched);
        const auto cleanFar=buildPlanetSurfaceFieldChunkMesh(cleanForInfluence.snapshot(),chunkAddress0,8,true,0.75f,2);
        const auto editedFar=buildPlanetSurfaceFieldChunkMesh(planet.snapshot(),chunkAddress0,8,true,0.75f,2);
        require(editedFar.quads>cleanFar.quads,
                "localized player edit influence did not refine affected far-field tiles");

        renderer.setStreamingFocus(focus*-1.0f,1,0);
        settlePlanetRenderer(renderer,planet);
        require(renderer.targetNearFieldChunks()==0 && renderer.nearFieldChunks()==0,
                "localized edit influence still promoted a whole chunk tier");

        renderer.setStreamingFocus(focus,0,0);
        settlePlanetRenderer(renderer,planet);
        require(renderer.fullDetailChunks()==0 && renderer.fieldDetailChunks()==PlanetSurface::ChunkCount,
                "field-only orbital residency still kept LOD0 chunks");
        require(renderer.nearFieldChunks()==0 && renderer.farFieldChunks()==PlanetSurface::ChunkCount,
                "field-only residency unexpectedly promoted whole edited chunks");

        renderer.setOrbitalShellOnly();
        renderer.sync(planet);
        require(renderer.ready() && renderer.dirtyChunks()==0,
                "orbital shell-only renderer did not become ready without voxel chunk jobs");
        require(renderer.orbitalClimateQuads()>0 && renderer.orbitalCloudQuads()>0,
                "orbital renderer failed to publish climate/cloud shell packets");
        const int orbitDrawsBefore=fake.draws;
        renderer.drawOrbitalShell();
        require(fake.draws>=orbitDrawsBefore+2,"orbital climate/cloud shell did not issue both draws");

        renderer.setFullDetail();
        settlePlanetRenderer(renderer,planet);
        require(renderer.fullDetailChunks()==PlanetSurface::ChunkCount && renderer.fieldDetailChunks()==0,
                "full-detail debug mode did not restore all surface chunks");
    }
    require(fake.liveCount()==0 && fake.destroys==fake.uploads,"planet surface renderer leaked graphics resources");
}

void settleRenderer(WorldRenderer& renderer, const World& world) {
    renderer.sync(world); // establish target revisions and schedule initial work
    for (int i=0;i<500 && (renderer.pendingJobs()>0 || renderer.dirtyChunks()>0);++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        renderer.sync(world);
    }
    require(renderer.pendingJobs()==0, "renderer jobs failed to settle");
    require(renderer.dirtyChunks()==0, "renderer remained dirty after jobs settled");
}

void testChunkDirtyTrackingAndRendererIsolation() {
    World w(0xA11CEULL,PlanetClass::Temperate);
    JobSystem jobs(2);
    FakeGraphicsBackend fake;
    WorldRenderer renderer(jobs,fake);
    settleRenderer(renderer,w);
    require(fake.uploads==World::ChunkCount, "initial renderer did not upload exactly one mesh per chunk");

    const std::uint64_t r00=w.chunkRevision(0,0,0);
    const std::uint64_t r10=w.chunkRevision(1,0,0);
    const std::uint64_t r01=w.chunkRevision(0,0,1);
    const std::uint64_t r11=w.chunkRevision(1,0,1);
    const int x=10,z=10,y=w.surfaceY(x,z);
    w.set(x,y,z,BlockType::Air);
    require(w.chunkRevision(0,0,0)>r00, "local edit did not dirty its owning chunk");
    require(w.chunkRevision(1,0,0)==r10 && w.chunkRevision(0,0,1)==r01 && w.chunkRevision(1,0,1)==r11,
            "ordinary local surface edit dirtied unrelated chunks");
    const int uploadsBefore=fake.uploads;
    settleRenderer(renderer,w);
    require(fake.uploads==uploadsBefore+1, "local edit rebuilt more than one render chunk");

    // A micro edit on x=31 touches both sides of the chunk seam because the
    // neighboring mesh samples the edited cell for face/AO decisions.
    const int sx=31,sz=8,sy=w.surfaceY(sx,sz);
    const auto a0=w.chunkRevision(0,0,0);
    const auto a1=w.chunkRevision(1,0,0);
    w.setMicro(sx,sy,sz,15,15,15,BlockType::Air);
    require(w.chunkRevision(0,0,0)>a0 && w.chunkRevision(1,0,0)>a1,
            "seam micro edit did not dirty both adjacent chunks");
    const int seamBefore=fake.uploads;
    settleRenderer(renderer,w);
    require(fake.uploads==seamBefore+2, "seam micro edit did not rebuild exactly two chunks");
}

void testAoAndMaterialRanges() {
    WorldSnapshot s=emptySnapshot();
    auto put=[&](int x,int y,int z,BlockType t){ s.blocks[static_cast<std::size_t>(s.flatIndex(x,y,z))]=t; };
    put(2,2,2,BlockType::Stone);
    put(4,2,2,BlockType::Dirt);
    // Raised stone near the first cube creates a concave corner AO sample while
    // leaving part of the lower cube's top surface exposed.
    put(3,3,2,BlockType::Stone);
    s.computeExteriorAir();
    const CpuMeshData m=buildChunkMesh(s,0,0,0);
    require(m.aoDarkenedCorners>0, "voxel AO did not darken any concave face corners");
    require(m.materialRanges.size()>=2, "material batching failed to emit multiple ranges");
    int vertices=0,quads=0;
    int expectedFirst=0;
    for(const auto& range:m.materialRanges) {
        require(range.firstVertex==expectedFirst, "material ranges are not contiguous");
        expectedFirst += range.vertexCount;
        vertices += range.vertexCount;
        quads += range.quads;
    }
    require(vertices==m.vertexCount(), "material ranges do not cover all vertices");
    require(quads==m.quads, "material ranges do not cover all quads");
}


void testBasePowerAndSealedAtmosphere() {
    World w(0xB453ULL,PlanetClass::Barren);
    BaseInfrastructure base(w.seed());

    // Construct a small sealed 3x3x3 interior near the top of the terrain by
    // clearing the interior and forcing a stone shell. The query must distinguish
    // it from the open planetary atmosphere.
    const int bx=8,bz=8,by=18;
    for(int y=by;y<by+5;++y) for(int z=bz;z<bz+5;++z) for(int x=bx;x<bx+5;++x) {
        const bool shell=x==bx||x==bx+4||y==by||y==by+4||z==bz||z==bz+4;
        w.set(x,y,z,shell?BlockType::Stone:BlockType::Air);
    }
    const IVec3 inside{bx+2,by+2,bz+2};
    const auto room=w.sealedVolume(inside,256);
    require(room.sealed && room.cells.size()==27, "sealed-volume flood fill failed on closed room");
    w.set(bx+2,by+4,bz+2,BlockType::Air);
    require(!w.sealedVolume(inside,4096).sealed, "sealed-volume flood fill ignored an exterior breach");
    w.set(bx+2,by+4,bz+2,BlockType::Stone);

    const auto genId=base.place(MachineType::BurnerGenerator,{bx+1,by+1,bz+1});
    const auto batId=base.place(MachineType::BatteryBank,{bx+1,by+1,bz+2});
    const auto atmId=base.place(MachineType::AtmosphereUnit,inside);
    auto* gen=base.find(genId);
    require(gen!=nullptr,"missing generator object");
    gen->fuelSeconds=10.0f;
    base.update(1.0f);
    const auto summary=base.summary();
    require(summary.networkCount==1 && summary.generation>=19.9f,"burner generator did not power local base network");
    require(base.find(atmId)->powered,"critical atmosphere load was not powered");
    require(base.find(batId)->storedEnergy>0.0f,"battery did not charge from surplus generation");
    require(base.oxygenatedAt(w,inside,256),"powered atmosphere unit did not oxygenate its sealed room");

    MachineObject saved=*base.find(atmId);
    BaseInfrastructure restored(w.seed());
    require(restored.restore(saved),"stable machine object restore failed");
    require(restored.find(saved.stableId)!=nullptr,"restored machine lost stable ID");
}


void testCachedSphericalMeshingUsesHalo() {
    PlanetSurface planet(0xCACEB00CULL,PlanetClass::Temperate);
    JobSystem jobs(1);
    SurfaceChunkCache cache(jobs,4,8U*1024U*1024U,4);
    const PlanetChunkAddress seam{CubeFace::PositiveZ,1,0,0};

    // Refine an edge cell so the cached packet must carry both the core
    // MicroBrick and the cross-face neighbor state in its one-cell halo.
    const int u=PlanetSurface::FaceResolution-1;
    const int v=9;
    const int r=planet.surfaceRadial(CubeFace::PositiveZ,u,v);
    planet.setMicro({CubeFace::PositiveZ,u,v,r},15,15,8,BlockType::Air);

    // Construct a small concave overhang in the same cached chunk so spherical
    // AO has an unambiguous outside-layer occluder to darken at least one face
    // corner.
    const int aoU=40,aoV=12;
    const int aoBase=std::max({planet.surfaceRadial(CubeFace::PositiveZ,aoU,aoV),
                              planet.surfaceRadial(CubeFace::PositiveZ,aoU+1,aoV),
                              planet.surfaceRadial(CubeFace::PositiveZ,aoU,aoV+1)})+2;
    require(aoBase+1<PlanetSurface::RadialLayers,"AO fixture escaped radial bounds");
    planet.set({CubeFace::PositiveZ,aoU,aoV,aoBase},BlockType::SteelPlate,true);
    planet.set({CubeFace::PositiveZ,aoU+1,aoV,aoBase+1},BlockType::SteelPlate,true);

    cache.beginFrame();
    require(cache.request(planet,seam,SurfaceChunkPriority::Visible),"halo cache request failed");
    settleSurfaceCache(cache,planet);
    const auto data=cache.find(seam);
    require(data!=nullptr,"halo chunk did not become resident");
    require(data->isRefined({CubeFace::PositiveZ,u,v,r}),"cached halo packet lost core MicroBrick");

    // local U=32 is one cell beyond this +Z chunk and must wrap onto +X.
    const int localV=9;
    const int localR=r;
    const auto wrapped=data->worldAddress(PlanetSurface::ChunkSize,localV,localR);
    require(wrapped.face==CubeFace::PositiveX,"chunk halo did not wrap across cube-face seam");
    require(data->getWithHalo(PlanetSurface::ChunkSize,localV,localR)==planet.get(wrapped),
            "cross-face halo sample disagrees with authoritative planet query");

    const auto mesh=buildPlanetSurfaceChunkMesh(*data);
    require(!mesh.empty() && mesh.quads>0,"direct cached LOD0 mesher emitted no geometry");
    require(mesh.microQuads>0,"direct cached LOD0 mesher omitted refined geometry");
    require(mesh.aoDarkenedCorners>0,"spherical cached mesher did not bake any voxel AO corners");
    const auto reference=buildPlanetSurfaceChunkMesh(planet.snapshot(),seam);
    require(mesh.quads==reference.quads && mesh.microQuads==reference.microQuads && mesh.vertexCount()==reference.vertexCount(),
            "cached halo mesher diverged from snapshot reference topology");
}

void testSurfaceChunkTransactionalStore() {
    namespace fs=std::filesystem;
    const fs::path root=fs::temp_directory_path()/"elysium_v10_chunk_store_test";
    std::error_code ec;
    fs::remove_all(root,ec);

    PlanetSurface planet(0x51DECAFEULL,PlanetClass::Barren);
    SurfaceInfrastructure infrastructure(planet.seed());
    const PlanetChunkAddress chunk{CubeFace::PositiveZ,0,0,0};
    const int u=6,v=7,r=planet.surfaceRadial(chunk.face,u,v);
    const SurfaceCellAddress cell{chunk.face,u,v,r};
    planet.set(cell,BlockType::SteelPlate,true);
    planet.setMicro(cell,3,4,5,BlockType::Air);
    infrastructure.place(MachineType::StorageCrate,{chunk.face,u+1,v,r+1});

    SurfaceChunkStore store(root,1,planet.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
    auto record=makeSurfaceChunkRecord(1,planet,infrastructure,chunk);
    std::string error;
    require(store.save(record,&error),"transactional chunk save failed: "+error);
    auto loaded=store.load(chunk);
    require(loaded.loaded && !loaded.usedPreviousGeneration,"transactional chunk load failed: "+loaded.error);
    require(loaded.record.journal.macroEdits.size()==record.journal.macroEdits.size(),"chunk macro transaction round trip mismatch");
    require(loaded.record.journal.placedMarkers.size()==record.journal.placedMarkers.size(),"chunk placement transaction round trip mismatch");
    require(loaded.record.journal.microOverrideCount()==record.journal.microOverrideCount(),"chunk micro transaction round trip mismatch");
    require(loaded.record.machines.size()==1 && loaded.record.machines.front().stableId==record.machines.front().stableId,
            "stable machine object was not grouped into owning chunk transaction");

    // Publish a second generation so the first becomes previous-known-good.
    const SurfaceCellAddress second{chunk.face,u+2,v,r};
    planet.set(second,BlockType::SteelPlate,true);
    auto secondRecord=makeSurfaceChunkRecord(1,planet,infrastructure,chunk);
    error.clear();
    require(store.save(secondRecord,&error),"second transactional generation failed: "+error);
    require(fs::exists(store.previousPath(chunk)),"previous known-good generation was not retained");

    // Corrupt current generation. Loader must reject checksum failure and use
    // the previous file rather than applying partial/torn data.
    {
        std::ofstream out(store.recordPath(chunk),std::ios::binary|std::ios::trunc);
        out<<"ELYSIUM_CHUNK_TXN 1\npayload_bytes 64\nchecksum 1\n\nTORN";
    }
    auto recovered=store.load(chunk);
    require(recovered.loaded && recovered.usedPreviousGeneration,
            "corrupt current chunk did not recover previous known-good generation");
    require(recovered.record.journal.macroEdits.size()==record.journal.macroEdits.size(),
            "fallback generation did not match previous committed chunk state");

    const fs::path manifest=root/"manifest.txt";
    error.clear();
    require(writeAtomicGeneration(manifest,"manifest-A",&error),"atomic manifest generation A failed: "+error);
    require(writeAtomicGeneration(manifest,"manifest-B",&error),"atomic manifest generation B failed: "+error);
    std::ifstream cur(manifest,std::ios::binary),prev(manifest.string()+".prev",std::ios::binary);
    const std::string currentText((std::istreambuf_iterator<char>(cur)),{});
    const std::string previousText((std::istreambuf_iterator<char>(prev)),{});
    require(currentText=="manifest-B" && previousText=="manifest-A","atomic manifest generation rotation failed");

    const fs::path checked=root/"checked_manifest.txt";
    require(writeCheckedAtomicGeneration(checked,"save-A",&error),"checked manifest A failed: "+error);
    require(writeCheckedAtomicGeneration(checked,"save-B",&error),"checked manifest B failed: "+error);
    {
        std::ofstream torn(checked,std::ios::binary|std::ios::trunc);
        torn<<"ELYSIUM_ATOMIC_GENERATION 1\npayload_bytes 99\nchecksum 0\n\nTORN";
    }
    const auto checkedRecovered=readCheckedAtomicGeneration(checked,true);
    require(checkedRecovered.loaded && checkedRecovered.usedPreviousGeneration && checkedRecovered.data=="save-A",
            "checked global generation failed previous-known-good recovery");

    const fs::path legacy=root/"legacy_plain.txt";
    {std::ofstream plain(legacy);plain<<"ELYSIUM_SAVE 6 1\nEND\n";}
    const auto legacyRead=readCheckedAtomicGeneration(legacy,true);
    require(legacyRead.loaded && legacyRead.data.starts_with("ELYSIUM_SAVE 6 1"),"plain legacy manifest compatibility was lost");

    // v0.11 whole-save generation binding: publishing chunk generation N+1
    // before the manifest must not make the still-committed generation N read
    // the newer chunk. The loader selects the .prev copy by generation ID.
    const fs::path boundRoot=root/"bound";
    PlanetSurface boundPlanet(0xB10D5A7EULL,PlanetClass::Temperate);
    SurfaceInfrastructure boundInfra(boundPlanet.seed());
    const PlanetChunkAddress boundChunk{CubeFace::PositiveX,0,1,0};
    const int br=boundPlanet.surfaceRadial(boundChunk.face,7,40);
    const SurfaceCellAddress bc{boundChunk.face,7,40,br};
    boundPlanet.set(bc,BlockType::SteelPlate,true);
    SurfaceChunkStore boundStore(boundRoot,2,boundPlanet.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
    auto gen41=makeSurfaceChunkRecord(2,boundPlanet,boundInfra,boundChunk,41);
    require(boundStore.save(gen41,&error),"generation-bound chunk 41 save failed: "+error);

    boundPlanet.set({boundChunk.face,8,40,br},BlockType::SteelPlate,true);
    auto gen42=makeSurfaceChunkRecord(2,boundPlanet,boundInfra,boundChunk,42);
    require(boundStore.save(gen42,&error),"generation-bound chunk 42 save failed: "+error);
    const auto oldCommit=boundStore.load(boundChunk,std::uint64_t{41});
    require(oldCommit.loaded && oldCommit.usedPreviousGeneration && oldCommit.record.saveGeneration==41,
            "old manifest generation did not select previous compatible sidecar");
    const auto newCommit=boundStore.load(boundChunk,std::uint64_t{42});
    require(newCommit.loaded && !newCommit.usedPreviousGeneration && newCommit.record.saveGeneration==42,
            "new manifest generation did not select current sidecar");

    // Retrying the same uncommitted generation must replace current generation
    // 42 without rotating away the still-needed generation 41 fallback.
    boundPlanet.set({boundChunk.face,9,40,br},BlockType::SteelPlate,true);
    auto retry42=makeSurfaceChunkRecord(2,boundPlanet,boundInfra,boundChunk,42);
    require(boundStore.save(retry42,&error),"same-generation retry failed: "+error);
    const auto oldAfterRetry=boundStore.load(boundChunk,std::uint64_t{41});
    require(oldAfterRetry.loaded && oldAfterRetry.usedPreviousGeneration && oldAfterRetry.record.saveGeneration==41,
            "same-generation retry rotated away previous manifest-compatible chunk");

    std::ostringstream manifest41;
    manifest41<<"ELYSIUM_SAVE 8 1\nsave_generation 41\nsurface_manifest 2 1\nsurface_chunk "
              <<static_cast<int>(boundChunk.face)<<' '<<boundChunk.u<<' '<<boundChunk.v<<' '<<boundChunk.radial<<"\nEND\n";
    std::ostringstream manifest42;
    manifest42<<"ELYSIUM_SAVE 8 1\nsave_generation 42\nsurface_manifest 2 1\nsurface_chunk "
              <<static_cast<int>(boundChunk.face)<<' '<<boundChunk.u<<' '<<boundChunk.v<<' '<<boundChunk.radial<<"\nEND\n";
    const auto refs41=extractSurfaceManifestChunkRefs(manifest41.str());
    const auto refs42=extractSurfaceManifestChunkRefs(manifest42.str());
    require(refs41.size()==1 && refs42.size()==1 && refs42.front().planetSlot==2,
            "surface manifest sidecar reference extraction failed");

    const fs::path orphanDir=boundRoot/"planet_2";
    fs::create_directories(orphanDir);
    const fs::path orphan=orphanDir/"ffffffffffffffff.chunk";
    const fs::path staleTmp=orphanDir/"stale.chunk.tmp";
    {std::ofstream o(orphan);o<<"orphan";}
    {std::ofstream o(staleTmp);o<<"temp";}
    error.clear();
    require(pruneSurfaceChunkStore(boundRoot,refs42,refs41,&error),"surface sidecar prune failed: "+error);
    require(!fs::exists(orphan) && !fs::exists(staleTmp),"orphan/temp surface sidecars were not pruned");
    require(fs::exists(boundStore.recordPath(boundChunk)) && fs::exists(boundStore.previousPath(boundChunk)),
            "sidecar prune removed current/previous manifest recovery data");

    fs::remove_all(root,ec);
}


void testSurfaceDefenseAutomationAndPersistence() {
    PlanetSurface planet(0xD3F315EULL,PlanetClass::Barren);
    SurfaceInfrastructure infra(planet.seed());
    const CubeFace face=CubeFace::PositiveZ;
    const int u=10,v=10;
    int baseRad=0;
    for(int du=0;du<=6;++du) baseRad=std::max(baseRad,planet.surfaceRadial(face,u+du,v));
    baseRad=std::min(PlanetSurface::RadialLayers-2,baseRad+2);

    const auto genId=infra.place(MachineType::BurnerGenerator,{face,u,v,baseRad});
    const auto sensorId=infra.place(MachineType::SensorMast,{face,u+1,v,baseRad});
    const auto turretId=infra.place(MachineType::Turret,{face,u+2,v,baseRad});
    const auto shieldId=infra.place(MachineType::ShieldPylon,{face,u+3,v,baseRad});
    const auto logicId=infra.place(MachineType::LogicController,{face,u+4,v,baseRad});
    require(genId && sensorId && turretId && shieldId && logicId,"defense fixture failed to place stable machines");
    infra.find(genId)->fuelSeconds=120.0f;
    infra.find(turretId)->enabled=false;

    const SurfaceCellAddress doorCell{face,u+5,v,baseRad};
    const auto doorId=infra.placePortal(planet,SurfacePortalType::Door,doorCell,true);
    require(doorId!=0 && infra.findPortal(doorId)->open,"defense automation fixture failed to place open door");

    const auto enableRule=infra.createAutomationRule(logicId,SurfaceAutomationTrigger::HostilesDetected,sensorId,0.0f,
                                                       SurfaceAutomationAction::EnableMachine,turretId);
    const auto closeRule=infra.createAutomationRule(logicId,SurfaceAutomationTrigger::HostilesDetected,sensorId,0.0f,
                                                      SurfaceAutomationAction::ClosePortal,doorId);
    require(enableRule!=0 && closeRule!=0,"logic controller rejected valid finite-vocabulary automation rules");

    infra.update(planet,1.0f);
    require(infra.find(sensorId)->powered && infra.find(logicId)->powered && infra.find(shieldId)->powered,
            "priority-2 defense network failed to receive available generation");
    require(infra.find(shieldId)->shieldCharge>17.5f,"powered shield pylon did not accumulate charge");
    require(infra.find(sensorId)->powerNetworkId==infra.find(logicId)->powerNetworkId,
            "connected spherical defense machines did not receive one stable runtime network identity");

    const Vec3 hostilePos=planet.cellCenterPosition({face,u+3,v+1,baseRad});
    std::vector<SurfaceHostileContact> contacts{{77,hostilePos,30.0f}};
    auto telemetry=infra.updateDefense(planet,0.1f,contacts);
    require(telemetry.poweredSensors==1 && telemetry.hostilesDetected==1,"sensor mast did not publish hostile contact into its power network");
    require(telemetry.automationRulesEvaluated==2 && telemetry.automationActionsApplied==2,
            "logic controller did not deterministically apply matching automation rules");
    require(infra.find(turretId)->enabled,"hostile automation did not enable turret");
    require(!infra.findPortal(doorId)->open && planet.get(doorCell)==BlockType::DoorPanel,
            "hostile automation did not close portal through authoritative voxel state");
    require(infra.consumeTurretFireRequests().empty(),"turret fired in the same tick it was enabled after power allocation");

    infra.update(planet,0.1f);
    telemetry=infra.updateDefense(planet,0.1f,contacts);
    const auto fire=infra.consumeTurretFireRequests();
    require(telemetry.activeTurrets==1 && fire.size()==1 && fire.front().targetStableId==77,
            "powered turret did not emit deterministic ECS-facing fire request from sensor contact");
    require(infra.find(turretId)->ammo==47,"turret fire did not consume persistent ammunition");

    const float shieldBefore=infra.find(shieldId)->shieldCharge;
    const float remaining=infra.absorbShieldDamage(planet,planet.cellCenterPosition(infra.find(shieldId)->anchor),8.0f);
    require(remaining<=0.001f && infra.find(shieldId)->shieldCharge<shieldBefore,
            "shield pylon did not absorb bounded incoming damage from persistent charge");

    const PlanetChunkAddress owner=planet.chunkOf(infra.find(logicId)->anchor);
    const auto record=makeSurfaceChunkRecord(0,planet,infra,owner,55);
    require(record.automationRules.size()==2,"chunk record did not collect logic-controller-owned automation rules");
    const auto turretSaved=std::find_if(record.machines.begin(),record.machines.end(),[&](const auto& m){return m.stableId==turretId;});
    const auto shieldSaved=std::find_if(record.machines.begin(),record.machines.end(),[&](const auto& m){return m.stableId==shieldId;});
    require(turretSaved!=record.machines.end() && turretSaved->ammo==47,"chunk record lost turret ammunition state");
    require(shieldSaved!=record.machines.end() && shieldSaved->shieldCharge>0.0f,"chunk record lost shield charge state");

    const auto root=std::filesystem::temp_directory_path()/"elysium_v015_defense_txn";
    std::error_code ec; std::filesystem::remove_all(root,ec);
    SurfaceChunkStore store(root,0,planet.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
    std::string error;
    require(store.save(record,&error),"v5 defense sidecar save failed: "+error);
    const auto loaded=store.load(owner,55);
    require(loaded.loaded && loaded.record.automationRules.size()==2,"v5 defense/automation sidecar round trip failed");
    const auto loadedTurret=std::find_if(loaded.record.machines.begin(),loaded.record.machines.end(),[&](const auto& m){return m.stableId==turretId;});
    const auto loadedShield=std::find_if(loaded.record.machines.begin(),loaded.record.machines.end(),[&](const auto& m){return m.stableId==shieldId;});
    require(loadedTurret!=loaded.record.machines.end() && loadedTurret->ammo==47,"v5 sidecar lost turret ammunition");
    require(loadedShield!=loaded.record.machines.end() && loadedShield->shieldCharge>0.0f,"v5 sidecar lost shield charge");
    std::filesystem::remove_all(root,ec);
}


void testAddressKeyedInfrastructureJournalRecords() {
    // Merge-regression: infrastructure journal records must accept real
    // large-planet addresses and must never depend on the old 64x64 prototype
    // face dimensions or a dense column index.
    SurfacePortalObject largeDoor{};
    largeDoor.stableId=0xD001ULL;
    largeDoor.type=SurfacePortalType::Door;
    largeDoor.anchor={CubeFace::NegativeX,4096,4173,73};
    largeDoor.open=false;
    auto largeRecord=makeInfrastructureUpsert(largeDoor);
    std::string error;
    require(validateInfrastructureJournalRecord(largeRecord,&error),
            "address-keyed infrastructure record rejected a large-planet address: "+error);
    const std::string encoded=serializeInfrastructureJournalRecord(largeRecord);
    require(!encoded.empty(),"large-address infrastructure record failed to serialize");
    const auto decoded=deserializeInfrastructureJournalRecord(encoded,&error);
    require(decoded.has_value() && decoded->ownerAddress==largeDoor.anchor && decoded->stableId==largeDoor.stableId,
            "large-address infrastructure record failed deterministic round trip: "+error);

    // The actual Part Two prototype remains a useful harness for proving the
    // missing merge behavior: voxel geometry can already survive separately,
    // while the stable object record is what restores interactivity.
    PlanetSurface planet(0x1F2A016ULL,PlanetClass::Barren);
    SurfaceInfrastructure source(planet.seed());
    const CubeFace face=CubeFace::PositiveZ;
    const int u=20,v=20,r=25;
    const auto generator=source.place(MachineType::BurnerGenerator,{face,u,v,r});
    const auto logic=source.place(MachineType::LogicController,{face,u+1,v,r});
    const SurfaceCellAddress doorCell{face,u+2,v,r};
    const auto door=source.placePortal(planet,SurfacePortalType::Door,doorCell,false);
    require(generator && logic && door,"infrastructure journal fixture failed to create stable objects");
    source.find(generator)->fuelSeconds=42.0f;
    const auto rule=source.createAutomationRule(logic,SurfaceAutomationTrigger::HostilesDetected,generator,0.0f,
                                                 SurfaceAutomationAction::ClosePortal,door);
    require(rule!=0,"infrastructure journal fixture failed to create automation rule");

    std::vector<InfrastructureJournalRecord> records;
    require(captureInfrastructureUpserts(source,records,&error),"failed to capture infrastructure records: "+error);
    require(records.size()==4,"infrastructure capture did not produce machine/portal/rule records");
    require(records[0].kind==InfrastructureRecordKind::Machine &&
            records[1].kind==InfrastructureRecordKind::Machine &&
            records[2].kind==InfrastructureRecordKind::Portal &&
            records[3].kind==InfrastructureRecordKind::AutomationRule,
            "infrastructure capture order is not dependency deterministic");

    std::vector<InfrastructureJournalRecord> wire;
    for(const auto& record:records) {
        const auto text=serializeInfrastructureJournalRecord(record);
        require(!text.empty(),"captured infrastructure record failed to serialize");
        const auto parsed=deserializeInfrastructureJournalRecord(text,&error);
        require(parsed.has_value(),"captured infrastructure record failed to parse: "+error);
        wire.push_back(*parsed);
    }

    SurfaceInfrastructure restored(planet.seed());
    require(applyInfrastructureJournal(restored,planet,wire,&error),"failed to restore infrastructure journal: "+error);
    require(restored.find(generator) && std::abs(restored.find(generator)->fuelSeconds-42.0f)<1e-5f,
            "machine persistent state was not reconstructed from edit-journal record");
    require(restored.findPortal(door) && !restored.findPortal(door)->open,
            "closed door stable object was not reconstructed from edit-journal record");
    require(restored.findAutomationRule(rule),"automation rule was not reconstructed after controller restore");

    // The voxel edit and stable object are separate records by design. After
    // restoring the object, it can manipulate the already-persisted door voxel.
    require(planet.get(doorCell)==BlockType::DoorPanel,"door voxel fixture was not closed before interaction test");
    require(restored.setPortalOpen(planet,door,true) && planet.get(doorCell)==BlockType::Air,
            "reconstructed door object could not reopen persisted voxel geometry");
    require(restored.setPortalOpen(planet,door,false) && planet.get(doorCell)==BlockType::DoorPanel,
            "reconstructed door object could not close persisted voxel geometry");

    // Tombstoning the object must not rewrite the voxel journal. This is the
    // critical separation needed by the coworker's edit-journal persistence.
    require(restored.removeAutomationRule(rule),"fixture could not remove dependent automation rule before portal tombstone");
    const auto tombstone=makeInfrastructureTombstone(InfrastructureRecordKind::Portal,door,doorCell);
    require(applyInfrastructureJournal(restored,planet,{tombstone},&error),"portal tombstone failed: "+error);
    require(restored.findPortal(door)==nullptr,"portal tombstone left stable object alive");
    require(planet.get(doorCell)==BlockType::DoorPanel,
            "portal tombstone incorrectly modified authoritative voxel edit state");
}


void testInfrastructureJournalCrossShardClosureAndCompaction() {
    PlanetSurface planet(0x1F2A017ULL,PlanetClass::Barren);
    SurfaceInfrastructure source(planet.seed());
    const CubeFace face=CubeFace::PositiveZ;
    const int r=25;

    const auto controller=source.place(MachineType::AirlockController,{face,10,10,r});
    const auto inner=source.placePortal(planet,SurfacePortalType::Airlock,{face,13,10,r},false);
    const auto outer=source.placePortal(planet,SurfacePortalType::Airlock,{face,17,10,r},false);
    const auto assembly=source.createAirlockAssembly(planet,controller,inner,outer,{face,15,10,r},0.4f,0.2f);
    const auto logic=source.place(MachineType::LogicController,{face,21,10,r});
    const auto sensor=source.place(MachineType::SensorMast,{face,25,10,r});
    const auto rule=source.createAutomationRule(logic,SurfaceAutomationTrigger::HostilesDetected,sensor,0.0f,
                                                SurfaceAutomationAction::ClosePortal,inner);
    require(controller && inner && outer && assembly && logic && sensor && rule,
            "cross-shard infrastructure fixture failed to construct");

    std::string error;
    std::vector<InfrastructureJournalRecord> records;
    require(captureInfrastructureUpserts(source,records,&error),"v2 infrastructure capture failed: "+error);
    require(validateInfrastructureSnapshotClosure(records,&error),"v2 snapshot closure rejected valid cross-shard references: "+error);

    const auto airlockIt=std::find_if(records.begin(),records.end(),[&](const auto& rec){return rec.stableId==assembly;});
    require(airlockIt!=records.end() && airlockIt->schemaVersion==InfrastructureJournalRecord::SchemaVersion,
            "airlock capture did not emit current infrastructure schema");
    require(airlockIt->dependencies.size()==3,"airlock capture did not emit controller + two portal dependencies");
    require(std::any_of(airlockIt->dependencies.begin(),airlockIt->dependencies.end(),[&](const auto& dep){
                return dep.stableId==controller && dep.ownerAddress==source.find(controller)->anchor;
            }),"airlock controller dependency did not carry authoritative owner address");
    require(std::any_of(airlockIt->dependencies.begin(),airlockIt->dependencies.end(),[&](const auto& dep){
                return dep.stableId==inner && dep.ownerAddress==source.findPortal(inner)->anchor;
            }),"airlock inner-portal dependency did not carry authoritative owner address");

    const auto ruleIt=std::find_if(records.begin(),records.end(),[&](const auto& rec){return rec.stableId==rule;});
    require(ruleIt!=records.end() && ruleIt->dependencies.size()==3,
            "automation rule did not persist controller/source/target dependency locators");

    std::vector<InfrastructureJournalShard> shards;
    require(shardInfrastructureRecordsByOwnerAddress(records,shards,&error),"address sharding failed: "+error);
    require(shards.size()>=6,"address-keyed infrastructure sharding collapsed distinct owner addresses");
    for(std::size_t i=1;i<shards.size();++i) {
        const auto& a=shards[i-1].ownerAddress;
        const auto& b=shards[i].ownerAddress;
        require(std::tie(a.face,a.u,a.v,a.radial)<std::tie(b.face,b.u,b.v,b.radial),
                "infrastructure owner shards are not deterministically address ordered");
    }

    // Loading only the airlock-assembly shard must report the exact controller
    // and portal owner-address frontier instead of requiring any dense planet
    // lookup. Loading its dependencies in the same batch closes the frontier.
    std::vector<InfrastructureStableRef> missing;
    require(collectMissingInfrastructureDependencies(SurfaceInfrastructure(planet.seed()),{*airlockIt},missing,&error),
            "dependency-frontier collection failed: "+error);
    require(missing.size()==3,"partial airlock shard did not report exactly three missing stable dependencies");
    require(std::any_of(missing.begin(),missing.end(),[&](const auto& dep){return dep.stableId==controller && dep.ownerAddress==source.find(controller)->anchor;}),
            "dependency frontier lost controller owner address");
    require(std::any_of(missing.begin(),missing.end(),[&](const auto& dep){return dep.stableId==inner && dep.ownerAddress==source.findPortal(inner)->anchor;}),
            "dependency frontier lost inner portal owner address");
    require(std::any_of(missing.begin(),missing.end(),[&](const auto& dep){return dep.stableId==outer && dep.ownerAddress==source.findPortal(outer)->anchor;}),
            "dependency frontier lost outer portal owner address");

    std::vector<InfrastructureJournalRecord> airlockClosure{*airlockIt};
    for(const auto& rec:records) {
        if(rec.stableId==controller || rec.stableId==inner || rec.stableId==outer) airlockClosure.push_back(rec);
    }
    require(collectMissingInfrastructureDependencies(SurfaceInfrastructure(planet.seed()),airlockClosure,missing,&error),
            "closed dependency-frontier collection failed: "+error);
    require(missing.empty(),"same-batch stable dependencies were incorrectly reported missing");

    // A dependency address is a shard locator, not decoration. Corrupting it
    // must be caught before restore even though the stable ID still exists.
    auto bad=records;
    auto badAirlock=std::find_if(bad.begin(),bad.end(),[&](const auto& rec){return rec.stableId==assembly;});
    require(badAirlock!=bad.end(),"fixture lost airlock record");
    badAirlock->dependencies.front().ownerAddress.u+=1;
    require(!validateInfrastructureSnapshotClosure(bad,&error),
            "snapshot closure accepted a dependency that pointed at the wrong owner shard");

    // A surviving dependent cannot refer to an object whose final journal state
    // is tombstoned. This is validated before mutating a live infrastructure set.
    auto dangling=records;
    dangling.push_back(makeInfrastructureTombstone(InfrastructureRecordKind::Machine,sensor,source.find(sensor)->anchor));
    require(!validateInfrastructureSnapshotClosure(dangling,&error),
            "snapshot closure accepted automation pointing at a tombstoned source machine");

    // Latest-write-wins compaction keeps persistent state changes and retains
    // tombstones by default. Stable owner addresses are immutable within a
    // journal stream so sharded history cannot be silently orphaned.
    SurfaceMachineObject machine=*source.find(controller);
    machine.fuelSeconds=3.0f;
    auto oldMachine=makeInfrastructureUpsert(machine);
    machine.fuelSeconds=19.0f;
    auto newMachine=makeInfrastructureUpsert(machine);
    std::vector<InfrastructureJournalRecord> compacted;
    require(compactInfrastructureJournal({oldMachine,newMachine},compacted,InfrastructureCompactionPolicy::KeepTombstones,&error),
            "latest-write compaction failed: "+error);
    require(compacted.size()==1,"compaction did not collapse repeated stable-object upserts");
    const auto* compactedMachine=std::get_if<SurfaceMachineObject>(&*compacted.front().payload);
    require(compactedMachine && std::abs(compactedMachine->fuelSeconds-19.0f)<1e-5f,
            "compaction did not retain latest persistent machine state");

    const auto machineTombstone=makeInfrastructureTombstone(InfrastructureRecordKind::Machine,controller,source.find(controller)->anchor);
    require(compactInfrastructureJournal({oldMachine,machineTombstone},compacted,InfrastructureCompactionPolicy::KeepTombstones,&error),
            "tombstone compaction failed: "+error);
    require(compacted.size()==1 && compacted.front().op==InfrastructureJournalOp::Tombstone,
            "ordinary journal compaction incorrectly discarded final tombstone");
    require(compactInfrastructureJournal({oldMachine,machineTombstone},compacted,
                                         InfrastructureCompactionPolicy::DropTombstonesAgainstEmptyBaseline,&error),
            "known-empty baseline compaction failed: "+error);
    require(compacted.empty(),"known-empty baseline compaction retained unnecessary tombstone");

    auto moved=newMachine;
    moved.ownerAddress.u+=1;
    std::get<SurfaceMachineObject>(*moved.payload).anchor=moved.ownerAddress;
    require(!compactInfrastructureJournal({oldMachine,moved},compacted,InfrastructureCompactionPolicy::KeepTombstones,&error),
            "compaction silently moved one stable ID between owner shards");

    // Tombstones are intentionally idempotent so replaying a committed removal
    // after a crash/retry is not an error. Portal geometry remains separately
    // owned by the voxel edit journal.
    SurfaceInfrastructure empty(planet.seed());
    const auto absent=makeInfrastructureTombstone(InfrastructureRecordKind::Portal,0xABCDEFULL,{face,3000,3100,77});
    require(applyInfrastructureJournal(empty,planet,{absent},&error),
            "idempotent tombstone replay failed on already-absent object: "+error);

    // v1 records remain readable for migration. v1 had no cross-shard dependency
    // locator list; current capture always emits v2.
    auto legacy=oldMachine;
    legacy.schemaVersion=1;
    legacy.dependencies.clear();
    const auto legacyText=serializeInfrastructureJournalRecord(legacy);
    require(!legacyText.empty(),"schema-v1 compatibility record failed to serialize");
    const auto legacyRoundTrip=deserializeInfrastructureJournalRecord(legacyText,&error);
    require(legacyRoundTrip && legacyRoundTrip->schemaVersion==1 && legacyRoundTrip->dependencies.empty(),
            "schema-v1 infrastructure record failed backward-compatible parse: "+error);
}



void testSurfaceIndustryProcessingAndPersistence() {
    require(static_cast<int>(MachineType::LogicController)==8,"legacy machine enum identities changed");
    require(static_cast<int>(MachineType::Furnace)==9 && static_cast<int>(MachineType::CargoLoader)==15,
            "industrial machine IDs were not appended after legacy identities");

    PlanetSurface planet(0x1AD057A1ULL,PlanetClass::Temperate);
    SurfaceInfrastructure infrastructure(planet.seed());
    SurfaceIndustrySystem industry;
    const int r=planet.surfaceRadial(CubeFace::PositiveZ,31,31)+1;
    const auto gen=infrastructure.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,31,31,r});
    const auto storage=infrastructure.place(MachineType::NetworkStorage,{CubeFace::PositiveZ,32,31,r});
    const auto refinery=infrastructure.place(MachineType::Refinery,{CubeFace::PositiveZ,33,31,r});
    const auto furnace=infrastructure.place(MachineType::Furnace,{CubeFace::PositiveZ,34,31,r});
    const auto crucible=infrastructure.place(MachineType::AlloyCrucible,{CubeFace::PositiveZ,35,31,r});
    require(gen && storage && refinery && furnace && crucible,"industrial machine placement failed");
    infrastructure.find(gen)->fuelSeconds=200.0f;
    infrastructure.update(planet,0.0f);
    require(infrastructure.find(storage)->powered && infrastructure.find(refinery)->powered && infrastructure.find(crucible)->powered,
            "powered industry loads were not admitted to scalar base network");
    require(infrastructure.find(storage)->powerNetworkId==infrastructure.find(refinery)->powerNetworkId,
            "nearby industrial machines did not join one stable power network");

    // Refinery compresses already-learned mining work: one raw ore becomes two
    // ingots, and shared Network Storage supplies/receives the material without
    // requiring explicit conveyor routing for ordinary crafting.
    require(industry.insert(infrastructure,storage,static_cast<int>(BlockType::CopperOre),1),"failed to seed network storage with Copper Ore");
    require(industry.selectRecipe(infrastructure,refinery,SurfaceRecipeId::RefineCopper),"failed to select refinery recipe");
    industry.update(infrastructure,0.0f); // reserves input and starts process
    require(infrastructure.find(refinery)->activeRecipeId==static_cast<int>(SurfaceRecipeId::RefineCopper),
            "refinery did not begin selected process");
    require(industry.itemCount(infrastructure,storage,static_cast<int>(BlockType::CopperOre))==0,
            "refinery did not pull raw ore from shared storage");
    industry.update(infrastructure,6.1f);
    require(industry.itemCount(infrastructure,storage,static_cast<int>(IndustryItemId::CopperIngot))==2,
            "refinery did not double ore into shared storage output");
    require(industry.telemetry().completedProcesses==1 && industry.telemetry().networkPushes>=1,
            "refinery completion/network telemetry missing");

    // Furnace is fuel-driven rather than an electric load. One Coal Ore fuels
    // the batch while Iron Ore is consumed as the recipe input.
    require(industry.insert(infrastructure,storage,static_cast<int>(BlockType::CoalOre),1),"failed to seed furnace coal");
    require(industry.insert(infrastructure,storage,static_cast<int>(BlockType::IronOre),1),"failed to seed furnace iron");
    require(industry.selectRecipe(infrastructure,furnace,SurfaceRecipeId::SmeltIron),"failed to select furnace recipe");
    industry.update(infrastructure,0.0f);
    require(infrastructure.find(furnace)->fuelSeconds>0.0f,"furnace did not load deterministic coal fuel");
    industry.update(infrastructure,5.1f);
    require(industry.itemCount(infrastructure,storage,static_cast<int>(IndustryItemId::IronIngot))==1,
            "furnace failed to smelt Iron Ore into Iron Ingot");

    // Bronze follows the specification's 3 copper + 1 tin alloy shape.
    require(industry.insert(infrastructure,storage,static_cast<int>(IndustryItemId::CopperIngot),1),"failed to add third copper ingot");
    require(industry.insert(infrastructure,storage,static_cast<int>(IndustryItemId::TinIngot),1),"failed to seed tin ingot");
    require(industry.selectRecipe(infrastructure,crucible,SurfaceRecipeId::AlloyBronze),"failed to select bronze alloy recipe");
    industry.update(infrastructure,0.0f);
    industry.update(infrastructure,7.1f);
    require(industry.itemCount(infrastructure,storage,static_cast<int>(IndustryItemId::BronzeIngot))==4,
            "3 copper + 1 tin did not produce four Bronze Ingots");

    // Brownout must stop a powered process before material reservation. Power
    // shortage is logistics, not silent deletion of input items.
    SurfaceInfrastructure dark(planet.seed()+1);
    SurfaceIndustrySystem darkIndustry;
    const auto darkRefinery=dark.place(MachineType::Refinery,{CubeFace::PositiveX,10,10,r});
    require(darkIndustry.insert(dark,darkRefinery,static_cast<int>(BlockType::IronOre),1),"failed to seed unpowered refinery");
    require(darkIndustry.selectRecipe(dark,darkRefinery,SurfaceRecipeId::RefineIron),"failed to select unpowered refinery recipe");
    dark.update(planet,0.0f);
    darkIndustry.update(dark,20.0f);
    require(dark.find(darkRefinery)->activeRecipeId==0 &&
            darkIndustry.itemCount(dark,darkRefinery,static_cast<int>(BlockType::IronOre))==1 &&
            darkIndustry.telemetry().stalledForPower==1,
            "brownout consumed input or advanced powered refinery work");

    // Mid-process machine state is part of the stable infrastructure record.
    auto persistent=*infrastructure.find(refinery);
    persistent.selectedRecipeId=static_cast<int>(SurfaceRecipeId::RefineIron);
    persistent.activeRecipeId=static_cast<int>(SurfaceRecipeId::RefineIron);
    persistent.processProgressSeconds=3.25f;
    persistent.inventory={{static_cast<int>(BlockType::IronOre),2},{static_cast<int>(IndustryItemId::IronIngot),3}};
    SurfaceIndustrySystem::normalizeInventory(persistent);
    const auto record=makeInfrastructureUpsert(persistent);
    require(record.schemaVersion==5,"industry machine upsert did not advance infrastructure journal schema to v5");
    std::string error;
    const auto text=serializeInfrastructureJournalRecord(record);
    require(!text.empty(),"industry machine infrastructure record failed serialization");
    const auto decoded=deserializeInfrastructureJournalRecord(text,&error);
    require(decoded.has_value(),"industry machine infrastructure record failed parse: "+error);
    const auto* decodedMachine=std::get_if<SurfaceMachineObject>(&*decoded->payload);
    require(decodedMachine && decodedMachine->inventory==persistent.inventory &&
            decodedMachine->activeRecipeId==persistent.activeRecipeId &&
            std::abs(decodedMachine->processProgressSeconds-persistent.processProgressSeconds)<1e-5f,
            "infrastructure journal lost industrial inventory/process state");

    // The standalone touched-chunk sidecar also carries v0.19 industry state so
    // the prototype does not regress while the coworker translates the sparse
    // journal record into the running build.
    SurfaceInfrastructure persistedInfra(planet.seed());
    require(persistedInfra.restore(persistent),"failed to restore persistent refinery into sidecar fixture");
    const auto chunk=planet.chunkOf(persistent.anchor);
    auto sidecar=makeSurfaceChunkRecord(0,planet,persistedInfra,chunk,77);
    const auto root=std::filesystem::temp_directory_path()/"elysium_v019_industry_sidecar";
    std::error_code ec; std::filesystem::remove_all(root,ec);
    SurfaceChunkStore store(root,0,planet.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
    require(store.save(sidecar,&error),"industry sidecar save failed: "+error);
    const auto loaded=store.load(chunk,77);
    require(loaded.loaded && loaded.record.machines.size()==1,"industry sidecar failed to reload machine");
    require(loaded.record.machines.front().inventory==persistent.inventory &&
            loaded.record.machines.front().activeRecipeId==persistent.activeRecipeId,
            "sidecar v8 lost industrial machine inventory/process state");
    std::filesystem::remove_all(root,ec);
}


void testSurfaceExplicitLogisticsAndPersistence() {
    PlanetSurface planet(0x10A1571C5ULL,PlanetClass::Temperate);
    const int r=planet.surfaceRadial(CubeFace::PositiveZ,21,20)+2;

    // Conveyor: one item per interval, stable source/target IDs, no dense planet
    // index. Adjacent curved positions validate the physical link.
    SurfaceInfrastructure conveyorInfra(planet.seed());
    SurfaceIndustrySystem conveyorIndustry;
    const auto gen=conveyorInfra.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,20,20,r});
    const auto source=conveyorInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,21,20,r});
    const auto conveyor=conveyorInfra.place(MachineType::Conveyor,{CubeFace::PositiveZ,22,20,r});
    const auto target=conveyorInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,23,20,r});
    require(gen && source && conveyor && target,"conveyor fixture placement failed");
    conveyorInfra.find(gen)->fuelSeconds=500.0f;
    conveyorInfra.update(planet,0.0f);
    require(conveyorInfra.find(conveyor)->powered,"conveyor was not powered by local scalar network");
    require(conveyorIndustry.configureLogisticsLink(conveyorInfra,planet,conveyor,source,target),
            "failed to configure adjacent conveyor source/target");
    require(conveyorIndustry.insert(conveyorInfra,source,static_cast<int>(BlockType::CopperOre),3),
            "failed to seed conveyor source");
    conveyorIndustry.update(conveyorInfra,0.49f);
    require(conveyorIndustry.itemCount(conveyorInfra,target,static_cast<int>(BlockType::CopperOre))==0,
            "conveyor transferred before its interval elapsed");
    conveyorIndustry.update(conveyorInfra,0.02f);
    require(conveyorIndustry.itemCount(conveyorInfra,target,static_cast<int>(BlockType::CopperOre))==1 &&
            conveyorIndustry.itemCount(conveyorInfra,source,static_cast<int>(BlockType::CopperOre))==2,
            "conveyor did not conserve/move exactly one item");
    require(conveyorIndustry.telemetry().conveyorTransfers==1 && conveyorIndustry.telemetry().logisticsItemsMoved==1,
            "conveyor transfer telemetry missing");

    // Filling the destination must jam without consuming source inventory.
    require(conveyorIndustry.insert(conveyorInfra,target,static_cast<int>(BlockType::CopperOre),998),
            "failed to fill conveyor target stack");
    conveyorIndustry.update(conveyorInfra,0.50f);
    require(conveyorIndustry.telemetry().logisticsJams==1 && conveyorInfra.find(conveyor)->logisticsBlocked,
            "full target did not produce deterministic conveyor jam");
    require(conveyorIndustry.itemCount(conveyorInfra,source,static_cast<int>(BlockType::CopperOre))==2,
            "jammed conveyor consumed source items");

    // Brownout is a distinct stall state and must preserve source material.
    SurfaceInfrastructure darkLogistics(planet.seed()+99);
    SurfaceIndustrySystem darkLogisticsIndustry;
    const auto dsource=darkLogistics.place(MachineType::StorageCrate,{CubeFace::PositiveZ,25,20,r});
    const auto dconveyor=darkLogistics.place(MachineType::Conveyor,{CubeFace::PositiveZ,26,20,r});
    const auto dtarget=darkLogistics.place(MachineType::StorageCrate,{CubeFace::PositiveZ,27,20,r});
    require(darkLogisticsIndustry.configureLogisticsLink(darkLogistics,planet,dconveyor,dsource,dtarget),
            "failed to configure unpowered conveyor fixture");
    require(darkLogisticsIndustry.insert(darkLogistics,dsource,static_cast<int>(BlockType::IronOre),1),
            "failed to seed unpowered conveyor source");
    darkLogistics.update(planet,0.0f);
    darkLogisticsIndustry.update(darkLogistics,1.0f);
    require(darkLogisticsIndustry.telemetry().logisticsStalledForPower==1 &&
            darkLogisticsIndustry.itemCount(darkLogistics,dsource,static_cast<int>(BlockType::IronOre))==1 &&
            darkLogisticsIndustry.itemCount(darkLogistics,dtarget,static_cast<int>(BlockType::IronOre))==0,
            "browned-out conveyor moved or lost material");

    // Sorter: matching items take the primary output, everything else takes the
    // alternate output. Routing is stable item-ID order.
    SurfaceInfrastructure sorterInfra(planet.seed()+1);
    SurfaceIndustrySystem sorterIndustry;
    const auto sgen=sorterInfra.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,30,30,r});
    const auto ssource=sorterInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,31,30,r});
    const auto sorter=sorterInfra.place(MachineType::Sorter,{CubeFace::PositiveZ,32,30,r});
    const auto primary=sorterInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,33,30,r});
    const auto alternate=sorterInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,32,31,r});
    sorterInfra.find(sgen)->fuelSeconds=500.0f;
    sorterInfra.update(planet,0.0f);
    require(sorterIndustry.configureLogisticsLink(sorterInfra,planet,sorter,ssource,primary,alternate,static_cast<int>(BlockType::CopperOre)),
            "failed to configure sorter routing");
    require(sorterIndustry.insert(sorterInfra,ssource,static_cast<int>(BlockType::CopperOre),1) &&
            sorterIndustry.insert(sorterInfra,ssource,static_cast<int>(BlockType::IronOre),1),
            "failed to seed sorter source");
    sorterIndustry.update(sorterInfra,0.76f);
    require(sorterIndustry.itemCount(sorterInfra,primary,static_cast<int>(BlockType::CopperOre))==1,
            "sorter filter did not route matching item to primary output");
    sorterIndustry.update(sorterInfra,0.76f);
    require(sorterIndustry.itemCount(sorterInfra,alternate,static_cast<int>(BlockType::IronOre))==1,
            "sorter did not route nonmatching item to alternate output");

    // Cargo Loader is the bulk-transfer primitive: eight items per interval.
    SurfaceInfrastructure cargoInfra(planet.seed()+2);
    SurfaceIndustrySystem cargoIndustry;
    const auto cgen=cargoInfra.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,40,41,r});
    const auto csource=cargoInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,40,40,r});
    const auto loader=cargoInfra.place(MachineType::CargoLoader,{CubeFace::PositiveZ,41,40,r});
    const auto ctarget=cargoInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,42,40,r});
    cargoInfra.find(cgen)->fuelSeconds=500.0f;
    cargoInfra.update(planet,0.0f);
    require(cargoIndustry.configureLogisticsLink(cargoInfra,planet,loader,csource,ctarget),
            "failed to configure Cargo Loader");
    require(cargoIndustry.insert(cargoInfra,csource,static_cast<int>(BlockType::SteelPlate),20),
            "failed to seed Cargo Loader source");
    cargoIndustry.update(cargoInfra,2.1f);
    require(cargoIndustry.itemCount(cargoInfra,ctarget,static_cast<int>(BlockType::SteelPlate))==16 &&
            cargoIndustry.itemCount(cargoInfra,csource,static_cast<int>(BlockType::SteelPlate))==4,
            "Cargo Loader did not move two deterministic eight-item batches");
    require(cargoIndustry.telemetry().cargoTransfers==2 && cargoIndustry.telemetry().logisticsItemsMoved==16,
            "Cargo Loader throughput telemetry incorrect");

    // Logistics references are infrastructure dependencies in schema v5 so a
    // sparse loader can request exactly the source/target owner-address shards.
    conveyorInfra.find(conveyor)->logisticsProgressSeconds=0.25f;
    std::vector<InfrastructureJournalRecord> records;
    std::string error;
    require(captureInfrastructureUpserts(conveyorInfra,records,&error),
            "failed to capture logistics infrastructure snapshot: "+error);
    const auto it=std::find_if(records.begin(),records.end(),[&](const auto& rec){return rec.stableId==conveyor;});
    require(it!=records.end() && it->schemaVersion==5 && it->dependencies.size()==2,
            "conveyor record did not persist two machine dependencies in schema v5");
    require(validateInfrastructureSnapshotClosure(records,&error),
            "logistics infrastructure snapshot closure failed: "+error);

    SurfaceInfrastructure replayed(planet.seed());
    require(applyInfrastructureJournal(replayed,planet,records,&error),
            "dependency-ready logistics journal replay failed: "+error);
    const auto* restored=replayed.find(conveyor);
    require(restored && restored->logisticsSourceStableId==source && restored->logisticsTargetStableId==target &&
            std::abs(restored->logisticsProgressSeconds-0.25f)<1e-5f,
            "logistics journal replay lost stable links/progress");

    std::vector<InfrastructureStableRef> missing;
    SurfaceInfrastructure empty(planet.seed()+55);
    require(collectMissingInfrastructureDependencies(empty,{*it},missing,&error),
            "failed to collect logistics dependency frontier: "+error);
    require(missing.size()==2,"partial conveyor load did not expose exactly source/target owner-address dependencies");

    // Standalone sidecar v8 also preserves the routing state for the prototype.
    const auto chunk=planet.chunkOf(conveyorInfra.find(conveyor)->anchor);
    auto sidecar=makeSurfaceChunkRecord(0,planet,conveyorInfra,chunk,88);
    const auto root=std::filesystem::temp_directory_path()/"elysium_v020_logistics_sidecar";
    std::error_code ec; std::filesystem::remove_all(root,ec);
    SurfaceChunkStore store(root,0,planet.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
    require(store.save(sidecar,&error),"logistics sidecar save failed: "+error);
    const auto loaded=store.load(chunk,88);
    require(loaded.loaded,"logistics sidecar did not load: "+loaded.error);
    const auto loadedTransport=std::find_if(loaded.record.machines.begin(),loaded.record.machines.end(),[&](const auto& m){return m.stableId==conveyor;});
    require(loadedTransport!=loaded.record.machines.end() && loadedTransport->logisticsSourceStableId==source &&
            loadedTransport->logisticsTargetStableId==target && std::abs(loadedTransport->logisticsProgressSeconds-0.25f)<1e-5f,
            "sidecar v8 lost Conveyor routing state");
    std::filesystem::remove_all(root,ec);

    // A closed transport loop is a legal logistics graph shape. Stable refs
    // must not turn that into an impossible persistence dependency cycle.
    SurfaceInfrastructure loopInfra(planet.seed()+3);
    SurfaceIndustrySystem loopIndustry;
    const auto lgen=loopInfra.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,10,11,r});
    const auto c1=loopInfra.place(MachineType::Conveyor,{CubeFace::PositiveZ,10,10,r});
    const auto c2=loopInfra.place(MachineType::Conveyor,{CubeFace::PositiveZ,11,10,r});
    const auto sink1=loopInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,10,9,r});
    const auto sink2=loopInfra.place(MachineType::StorageCrate,{CubeFace::PositiveZ,11,9,r});
    loopInfra.find(lgen)->fuelSeconds=500.0f;
    loopInfra.update(planet,0.0f);
    require(loopIndustry.configureLogisticsLink(loopInfra,planet,c1,c2,sink1),"failed to configure first loop edge");
    require(loopIndustry.configureLogisticsLink(loopInfra,planet,c2,c1,sink2),"failed to configure second loop edge");
    std::vector<InfrastructureJournalRecord> loopRecords;
    require(captureInfrastructureUpserts(loopInfra,loopRecords,&error),"failed to capture cyclic logistics graph: "+error);
    require(validateInfrastructureSnapshotClosure(loopRecords,&error),"cyclic logistics snapshot closure failed: "+error);
    SurfaceInfrastructure loopReplay(planet.seed()+3);
    require(applyInfrastructureJournal(loopReplay,planet,loopRecords,&error),"cyclic machine dependency replay failed: "+error);
    require(loopReplay.find(c1) && loopReplay.find(c1)->logisticsSourceStableId==c2 &&
            loopReplay.find(c2) && loopReplay.find(c2)->logisticsSourceStableId==c1,
            "cyclic Conveyor stable references were not restored");

    // Curved-position link validation must also work at cube-face ownership
    // transitions. No twelve-edge rotation table is involved.
    const int edge=PlanetSurface::FaceResolution-1;
    const auto edgeSource=planet.normalize({CubeFace::PositiveZ,edge-1,32,r});
    const auto edgeTransport=planet.normalize({CubeFace::PositiveZ,edge,32,r});
    const auto edgeTarget=planet.normalize({CubeFace::PositiveZ,edge+1,32,r});
    SurfaceInfrastructure seamInfra(planet.seed()+4);
    SurfaceIndustrySystem seamIndustry;
    const auto es=seamInfra.place(MachineType::StorageCrate,edgeSource);
    const auto ecv=seamInfra.place(MachineType::Conveyor,edgeTransport);
    const auto et=seamInfra.place(MachineType::StorageCrate,edgeTarget);
    require(es && ecv && et,"seam logistics fixture placement failed");
    require(seamIndustry.configureLogisticsLink(seamInfra,planet,ecv,es,et),
            "curved logistics link failed across cube-face ownership transition");
}


void testSurfaceAdvancedIndustryExtractorAndDefenseSupply() {
    require(static_cast<int>(MachineType::CargoLoader)==15,"pre-v0.20 machine identity changed");
    require(static_cast<int>(MachineType::Crusher)==16 &&
            static_cast<int>(MachineType::ChemicalVat)==17 &&
            static_cast<int>(MachineType::Fabricator)==18 &&
            static_cast<int>(MachineType::Extractor)==19,
            "v0.20 advanced industry machine IDs were not appended stably");
    PlanetSurface planet(0xFACADE20ULL,PlanetClass::Temperate);
    const int r=planet.surfaceRadial(CubeFace::PositiveZ,25,25)+2;

    // Expanded processors/components remain fully obtainable from the current
    // vertical-slice material spine: no recipe below requires future silver,
    // resin, fiber, etc. merely to make the tests pass.
    SurfaceInfrastructure infra(planet.seed());
    SurfaceIndustrySystem industry;
    const auto g0=infra.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,24,25,r});
    const auto g1=infra.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,24,26,r});
    const auto crusher=infra.place(MachineType::Crusher,{CubeFace::PositiveZ,25,25,r});
    const auto vat=infra.place(MachineType::ChemicalVat,{CubeFace::PositiveZ,26,25,r});
    const auto fabricator=infra.place(MachineType::Fabricator,{CubeFace::PositiveZ,27,25,r});
    const auto refinery=infra.place(MachineType::Refinery,{CubeFace::PositiveZ,25,26,r});
    const auto storage=infra.place(MachineType::NetworkStorage,{CubeFace::PositiveZ,26,26,r});
    const auto turret=infra.place(MachineType::Turret,{CubeFace::PositiveZ,27,26,r});
    require(g0&&g1&&crusher&&vat&&fabricator&&refinery&&storage&&turret,"advanced industry fixture placement failed");
    infra.find(g0)->fuelSeconds=500.0f;
    infra.find(g1)->fuelSeconds=500.0f;
    infra.find(turret)->ammo=0;
    infra.update(planet,0.0f);
    require(infra.find(crusher)->powered && infra.find(vat)->powered && infra.find(fabricator)->powered && infra.find(refinery)->powered,
            "two burner generators did not power advanced industry fixture");

    require(validIndustryItemId(static_cast<int>(IndustryItemId::CompositePanel)) &&
            std::string(industryItemName(static_cast<int>(IndustryItemId::ControlCircuit)))=="Control Circuit",
            "expanded stable industry item registry is incomplete");

    require(industry.insert(infra,crusher,static_cast<int>(BlockType::Stone),1),"failed to seed Crusher");
    require(industry.selectRecipe(infra,crusher,SurfaceRecipeId::CrushStone),"failed to select Crusher recipe");
    industry.update(infra,2.1f);
    require(industry.itemCount(infra,storage,static_cast<int>(IndustryItemId::StoneAggregate))==2,
            "Crusher did not produce two Stone Aggregate into local Network Storage");

    require(industry.insert(infra,vat,static_cast<int>(IndustryItemId::Carbon),1),"failed to seed Chemical Vat carbon");
    require(industry.selectRecipe(infra,vat,SurfaceRecipeId::MixSealant),"failed to select Chemical Vat recipe");
    industry.update(infra,5.1f);
    require(industry.itemCount(infra,storage,static_cast<int>(IndustryItemId::Sealant))==1,
            "Chemical Vat did not consume shared aggregate and produce Sealant");

    require(industry.insert(infra,fabricator,static_cast<int>(IndustryItemId::CopperIngot),1),"failed to seed Fabricator copper");
    require(industry.selectRecipe(infra,fabricator,SurfaceRecipeId::FabricateCopperWire),"failed to select Copper Wire recipe");
    industry.update(infra,3.1f);
    require(industry.itemCount(infra,storage,static_cast<int>(IndustryItemId::CopperWire))==4,
            "Fabricator did not produce Copper Wire");
    // Selected recipes are persistent automation state; disable the Fabricator
    // so it cannot immediately consume the Refinery output we are about to
    // assert in this focused fixture.
    infra.find(fabricator)->enabled=false;

    require(industry.insert(infra,refinery,static_cast<int>(IndustryItemId::CopperConcentrate),1),"failed to seed concentrate Refinery");
    require(industry.selectRecipe(infra,refinery,SurfaceRecipeId::RefineCopperConcentrate),"failed to select concentrate Refinery recipe");
    industry.update(infra,3.6f);
    require(industry.itemCount(infra,storage,static_cast<int>(IndustryItemId::CopperIngot))>=1,
            "Refinery concentrate path did not yield Copper Ingot");

    // Fabricated ammo is consumed by a powered turret on the same local
    // network. One item is a twelve-round pack; no global inventory lookup.
    require(industry.insert(infra,storage,static_cast<int>(IndustryItemId::TurretAmmo),1),"failed to seed turret ammo pack");
    industry.update(infra,0.0f);
    require(infra.find(turret)->ammo==12 && industry.telemetry().turretAmmoLoads==1 && industry.telemetry().turretRoundsLoaded==12,
            "local Network Storage did not resupply turret ammunition");

    // Port policy: a Conveyor sourcing a Crusher must never steal the raw ore
    // that the Crusher still needs as recipe input. It may move only outputs.
    SurfaceInfrastructure chainInfra(planet.seed()+100);
    SurfaceIndustrySystem chainIndustry;
    const auto cg=chainInfra.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,18,20,r});
    const auto cc=chainInfra.place(MachineType::Crusher,{CubeFace::PositiveZ,20,20,r});
    const auto belt=chainInfra.place(MachineType::Conveyor,{CubeFace::PositiveZ,21,20,r});
    const auto cr=chainInfra.place(MachineType::Refinery,{CubeFace::PositiveZ,22,20,r});
    chainInfra.find(cg)->fuelSeconds=500.0f;
    chainInfra.update(planet,0.0f);
    require(chainIndustry.configureLogisticsLink(chainInfra,planet,belt,cc,cr),"failed to configure Crusher->Refinery belt");
    require(chainIndustry.insert(chainInfra,cc,static_cast<int>(BlockType::CopperOre),1),"failed to seed automated chain");
    chainIndustry.update(chainInfra,3.1f);
    require(chainIndustry.itemCount(chainInfra,cr,static_cast<int>(BlockType::CopperOre))==0,
            "Conveyor stole a raw process input from Crusher");
    require(chainIndustry.itemCount(chainInfra,cc,static_cast<int>(IndustryItemId::CopperConcentrate))==2,
            "Crusher did not complete concentrate stage before transport");
    chainIndustry.update(chainInfra,0.51f);
    require(chainIndustry.itemCount(chainInfra,cr,static_cast<int>(IndustryItemId::CopperConcentrate))>=1 ||
            chainInfra.find(cr)->activeRecipeId==static_cast<int>(SurfaceRecipeId::RefineCopperConcentrate),
            "Conveyor did not feed Crusher output into Refinery input port");
    chainIndustry.update(chainInfra,4.1f);
    require(chainIndustry.itemCount(chainInfra,cr,static_cast<int>(IndustryItemId::CopperIngot))>=1,
            "automated Crusher->Conveyor->Refinery chain failed to produce ingot");

    // Find one deterministic naturally-generated ore address and put a powered
    // Extractor above its column. Search is only a test fixture; production
    // Extractor queries remain bounded around its anchor.
    SurfaceCellAddress ore{};
    bool found=false;
    for(int f=0;f<PlanetSurface::FaceCount && !found;++f) {
        const auto face=static_cast<CubeFace>(f);
        for(int v=4;v<PlanetSurface::FaceResolution-4 && !found;++v) for(int u=4;u<PlanetSurface::FaceResolution-4 && !found;++u) {
            const int surface=planet.surfaceRadial(face,u,v);
            for(int radial=surface;radial>=std::max(0,surface-SurfaceIndustrySystem::ExtractorDepth+1);--radial) {
                SurfaceCellAddress a{face,u,v,radial};
                if(blockProperties(planet.get(a)).ore && !planet.playerPlaced(a)) {ore=a;found=true;break;}
            }
        }
    }
    require(found,"could not locate deterministic ore for Extractor test");
    SurfaceInfrastructure extractInfra(planet.seed()+200);
    SurfaceIndustrySystem extractIndustry;
    SurfaceCellAddress exAnchor{ore.face,ore.u,ore.v,std::min(PlanetSurface::RadialLayers-1,planet.surfaceRadial(ore.face,ore.u,ore.v)+2)};
    const auto ex=extractInfra.place(MachineType::Extractor,exAnchor);
    auto genAddress=planet.normalize({ore.face,ore.u+1,ore.v,exAnchor.radial});
    const auto eg=extractInfra.place(MachineType::BurnerGenerator,genAddress);
    auto storeAddress=planet.normalize({ore.face,ore.u-1,ore.v,exAnchor.radial});
    const auto es=extractInfra.place(MachineType::NetworkStorage,storeAddress);
    extractInfra.find(eg)->fuelSeconds=500.0f;
    extractInfra.update(planet,0.0f);
    require(extractInfra.find(ex)->powered,"Extractor was not powered");
    // Determine the lexicographically first natural ore in the exact bounded
    // search region; the production Extractor uses the same stable-address
    // ordering, never a dense planet column index.
    std::vector<SurfaceCellAddress> extractorCandidates;
    for(int dv=-SurfaceIndustrySystem::ExtractorHorizontalRadius;dv<=SurfaceIndustrySystem::ExtractorHorizontalRadius;++dv) {
        for(int du=-SurfaceIndustrySystem::ExtractorHorizontalRadius;du<=SurfaceIndustrySystem::ExtractorHorizontalRadius;++du) {
            if(du*du+dv*dv>SurfaceIndustrySystem::ExtractorHorizontalRadius*SurfaceIndustrySystem::ExtractorHorizontalRadius) continue;
            auto column=planet.normalize({exAnchor.face,exAnchor.u+du,exAnchor.v+dv,exAnchor.radial});
            const int surface=planet.surfaceRadial(column.face,column.u,column.v);
            for(int radial=surface;radial>=std::max(0,surface-SurfaceIndustrySystem::ExtractorDepth+1);--radial) {
                SurfaceCellAddress candidate{column.face,column.u,column.v,radial};
                if(blockProperties(planet.get(candidate)).ore && !planet.playerPlaced(candidate)) extractorCandidates.push_back(candidate);
            }
        }
    }
    auto addrLess=[](const SurfaceCellAddress& a,const SurfaceCellAddress& b){
        if(a.face!=b.face) return static_cast<int>(a.face)<static_cast<int>(b.face);
        if(a.u!=b.u) return a.u<b.u;
        if(a.v!=b.v) return a.v<b.v;
        return a.radial<b.radial;
    };
    std::sort(extractorCandidates.begin(),extractorCandidates.end(),addrLess);
    extractorCandidates.erase(std::unique(extractorCandidates.begin(),extractorCandidates.end()),extractorCandidates.end());
    require(!extractorCandidates.empty(),"Extractor bounded neighborhood unexpectedly contains no natural ore");
    const auto expectedMined=extractorCandidates.front();
    const BlockType minedType=planet.get(expectedMined);
    extractIndustry.update(extractInfra,0.0f);
    extractIndustry.updateExtraction(planet,extractInfra,2.1f);
    require(planet.get(expectedMined)==BlockType::Air,"Extractor did not persist its bounded ore excavation as a voxel edit");
    require(extractIndustry.itemCount(extractInfra,es,static_cast<int>(minedType))==1 ||
            extractIndustry.itemCount(extractInfra,ex,static_cast<int>(minedType))==1,
            "Extractor mined ore without publishing the material output");
    require(extractIndustry.telemetry().extractorOreMined==1 &&
            std::abs(extractIndustry.telemetry().extractorSuspicionGenerated-SurfaceIndustrySystem::ExtractorSuspicionPerOre)<1e-5f,
            "Extractor telemetry did not report mined ore/activity Suspicion");

    // Anti-duplication/output preflight are easiest to prove in a controlled
    // bounded search volume. Clear only the Extractor's local radius/depth,
    // then insert one ore cell with explicit authored-placement identity.
    PlanetSurface controlledPlanet(0xE17AC702ULL,PlanetClass::Temperate);
    const int cu=12,cv=12;
    const int controlledRadial=controlledPlanet.surfaceRadial(CubeFace::PositiveZ,cu,cv)+1;
    auto clearExtractorVolume=[&](BlockType replacement){
        for(int dv=-SurfaceIndustrySystem::ExtractorHorizontalRadius;dv<=SurfaceIndustrySystem::ExtractorHorizontalRadius;++dv) {
            for(int du=-SurfaceIndustrySystem::ExtractorHorizontalRadius;du<=SurfaceIndustrySystem::ExtractorHorizontalRadius;++du) {
                if(du*du+dv*dv>SurfaceIndustrySystem::ExtractorHorizontalRadius*SurfaceIndustrySystem::ExtractorHorizontalRadius) continue;
                const auto column=controlledPlanet.normalize({CubeFace::PositiveZ,cu+du,cv+dv,controlledRadial});
                const int surface=controlledPlanet.surfaceRadial(column.face,column.u,column.v);
                const int bottom=std::max(0,surface-SurfaceIndustrySystem::ExtractorDepth+1);
                for(int radial=surface;radial>=bottom;--radial) {
                    const SurfaceCellAddress a{column.face,column.u,column.v,radial};
                    if(blockProperties(controlledPlanet.get(a)).ore) controlledPlanet.set(a,replacement,false);
                }
            }
        }
    };
    clearExtractorVolume(BlockType::Stone);
    const int controlledSurface=controlledPlanet.surfaceRadial(CubeFace::PositiveZ,cu,cv);
    const SurfaceCellAddress controlledOre{CubeFace::PositiveZ,cu,cv,std::max(0,controlledSurface-2)};
    SurfaceInfrastructure controlledInfra(controlledPlanet.seed());
    SurfaceIndustrySystem controlledIndustry;
    const auto controlledGen=controlledInfra.place(MachineType::BurnerGenerator,{CubeFace::PositiveZ,cu+1,cv,controlledRadial});
    const auto controlledExtractor=controlledInfra.place(MachineType::Extractor,{CubeFace::PositiveZ,cu,cv,controlledRadial});
    controlledInfra.find(controlledGen)->fuelSeconds=500.0f;
    controlledInfra.update(controlledPlanet,0.0f);

    controlledPlanet.set(controlledOre,BlockType::CopperOre,true);
    controlledIndustry.update(controlledInfra,0.0f);
    controlledIndustry.updateExtraction(controlledPlanet,controlledInfra,2.1f);
    require(controlledPlanet.get(controlledOre)==BlockType::CopperOre && controlledPlanet.playerPlaced(controlledOre) &&
            controlledIndustry.itemCount(controlledInfra,controlledExtractor,static_cast<int>(BlockType::CopperOre))==0,
            "Extractor mined player-placed ore or duplicated its material");

    // A full output inventory must stall before committing the voxel edit.
    controlledPlanet.set(controlledOre,BlockType::Air,false); // clears placed marker before non-player fixture restore
    controlledPlanet.set(controlledOre,BlockType::CopperOre,false);
    auto* controlledObject=controlledInfra.find(controlledExtractor);
    controlledObject->inventory={
        {static_cast<int>(BlockType::Dirt),1},{static_cast<int>(BlockType::Stone),1},
        {static_cast<int>(BlockType::CoalOre),1},{static_cast<int>(BlockType::TinOre),1},
        {static_cast<int>(BlockType::IronOre),1},{static_cast<int>(BlockType::Planks),1},
        {static_cast<int>(BlockType::SteelPlate),1},{static_cast<int>(IndustryItemId::Carbon),1}
    };
    SurfaceIndustrySystem::normalizeInventory(*controlledObject);
    controlledObject->extractorProgressSeconds=0.0f;
    controlledIndustry.update(controlledInfra,0.0f);
    controlledIndustry.updateExtraction(controlledPlanet,controlledInfra,2.1f);
    require(controlledPlanet.get(controlledOre)==BlockType::CopperOre && controlledIndustry.telemetry().extractorOutputStalls>=1,
            "full Extractor output deleted ore instead of stalling before edit commit");

    // Extractor progress is explicit persistent state in infrastructure schema
    // v5 / standalone sidecar v8, rather than being inferred from wall clock.
    extractInfra.find(ex)->extractorProgressSeconds=0.75f;
    std::vector<InfrastructureJournalRecord> records;
    std::string error;
    require(captureInfrastructureUpserts(extractInfra,records,&error),"failed to capture Extractor infrastructure: "+error);
    const auto rit=std::find_if(records.begin(),records.end(),[&](const auto& rec){return rec.stableId==ex;});
    require(rit!=records.end() && rit->schemaVersion==5,"Extractor infrastructure record schema is not v5");
    const auto text=serializeInfrastructureJournalRecord(*rit);
    auto parsed=deserializeInfrastructureJournalRecord(text,&error);
    require(parsed && parsed->payload,"Extractor infrastructure record failed codec round trip: "+error);
    const auto restoredExtractor=std::get<SurfaceMachineObject>(*parsed->payload);
    require(std::abs(restoredExtractor.extractorProgressSeconds-0.75f)<1e-5f,
            "Extractor progress was lost by infrastructure journal codec");

    const auto exChunk=planet.chunkOf(extractInfra.find(ex)->anchor);
    auto sidecar=makeSurfaceChunkRecord(0,planet,extractInfra,exChunk,120);
    const auto root=std::filesystem::temp_directory_path()/"elysium_v020_extractor_sidecar";
    std::error_code ec; std::filesystem::remove_all(root,ec);
    SurfaceChunkStore store(root,0,planet.seed(),kSurfaceGeneratorVersion,kSurfaceGeneratorFingerprint);
    require(store.save(sidecar,&error),"Extractor sidecar save failed: "+error);
    const auto loaded=store.load(exChunk,120);
    require(loaded.loaded,"Extractor sidecar failed to load: "+loaded.error);
    const auto loadedExtractor=std::find_if(loaded.record.machines.begin(),loaded.record.machines.end(),[&](const auto& machine){return machine.stableId==ex;});
    require(loadedExtractor!=loaded.record.machines.end() && std::abs(loadedExtractor->extractorProgressSeconds-0.75f)<1e-5f,
            "sidecar v8 lost Extractor progress");
    std::filesystem::remove_all(root,ec);
}

void testSurfaceRegisterActionDirector() {
    SurfaceSiegeTuning tuning{};
    tuning.registerAnnouncementSeconds=0.25f;
    tuning.interWaveSeconds=0.10f;
    SurfaceSiegeDirector director(0x51E63EULL,tuning);

    require(director.bandFor(24.9f)==ImperialAttentionBand::Quiet,"siege attention band activates below Suspicion 25");
    require(director.bandFor(25.0f)==ImperialAttentionBand::Noted,"Suspicion 25 did not enter Noted");
    require(director.bandFor(50.0f)==ImperialAttentionBand::Marked,"Suspicion 50 did not enter Marked");
    require(director.bandFor(75.0f)==ImperialAttentionBand::Hunted,"Suspicion 75 did not enter Hunted");
    require(!director.requestEnforcement(24.0f,true),"quiet system scheduled Imperial enforcement");

    // Unclaimed systems receive a patrol even at high attention. Register
    // Actions are territorial and therefore require a live claim/beacon.
    require(director.requestEnforcement(83.0f,false),"unclaimed Hunted system failed to schedule patrol");
    require(director.state().type==ImperialEnforcementType::Patrol && director.state().totalWaves==1,
            "unclaimed system incorrectly scheduled a Register Action");
    auto patrol=director.update(0.0f,false,false);
    require(patrol.size()==3 && patrol[0].role==ImperialUnitRole::Lictor && patrol[2].role==ImperialUnitRole::Adept,
            "Hunted patrol composition is not deterministic");
    std::vector<std::uint64_t> patrolIds;
    for(const auto& spawn:patrol) patrolIds.push_back(spawn.stableEnemyId);
    require(std::is_sorted(director.state().activeEnemyIds.begin(),director.state().activeEnemyIds.end())==false ||
            director.state().activeEnemyIds.size()==patrol.size(),"patrol stable tracking was not populated");
    director.reconcileLiveEnemies({});
    require(director.state().phase==RegisterActionPhase::Cleared,"patrol did not clear after all tracked enemies vanished");
    director.clearTerminalState();
    require(director.state().phase==RegisterActionPhase::Idle,"terminal patrol did not return to idle");

    // Marked claims produce exactly three escalating waves after an announced
    // countdown. The director owns only stable IDs; combat/ECS lifetime is
    // reconciled externally.
    require(director.requestEnforcement(62.0f,true),"Marked claim failed to schedule Register Action");
    require(director.state().type==ImperialEnforcementType::RegisterAction && director.state().totalWaves==3 &&
            director.state().phase==RegisterActionPhase::Announced,
            "Marked claim did not enter three-wave announced Register Action");
    require(director.update(0.20f,true,true).empty(),"Register Action spawned before announcement expired");
    auto wave0=director.update(0.05f,true,true);
    require(wave0.size()==3 && wave0[0].waveIndex==0,"Marked wave zero composition/count incorrect");
    const auto actionId=director.state().actionId;
    SurfaceSiegeDirector mirror(0x51E63EULL,tuning);
    require(mirror.requestEnforcement(83.0f,false),"mirror serial priming failed");
    mirror.update(0.0f,false,false); mirror.reconcileLiveEnemies({}); mirror.clearTerminalState();
    require(mirror.requestEnforcement(62.0f,true),"mirror Marked action failed to schedule");
    mirror.update(0.25f,true,true);
    const auto mirrorWave0=mirror.state().activeEnemyIds;
    require(wave0.size()==mirrorWave0.size(),"deterministic siege mirror emitted different wave count");
    for(std::size_t i=0;i<wave0.size();++i)
        require(wave0[i].stableEnemyId==mirrorWave0[i],"same seed/action history emitted different enforcement stable IDs");

    director.reconcileLiveEnemies({});
    require(director.state().phase==RegisterActionPhase::InterWave && director.state().waveIndex==1,
            "first Marked wave did not advance to inter-wave state");
    require(director.update(0.05f,true,true).empty(),"next wave ignored inter-wave delay");
    const auto wave1=director.update(0.05f,true,true);
    require(wave1.size()==4 && wave1.back().role==ImperialUnitRole::Adept,
            "second Marked wave lacks support escalation");
    director.reconcileLiveEnemies({});
    const auto wave2=director.update(0.10f,true,true);
    require(wave2.size()==4 &&
            std::any_of(wave2.begin(),wave2.end(),[](const auto& r){return r.role==ImperialUnitRole::Lictor;}) &&
            std::any_of(wave2.begin(),wave2.end(),[](const auto& r){return r.role==ImperialUnitRole::Adept;}),
            "final Marked wave lacks elite/support pressure");
    director.reconcileLiveEnemies({});
    require(director.state().phase==RegisterActionPhase::Cleared,"three-wave Marked action did not clear");
    require(director.telemetry().wavesSpawned>=4 && director.telemetry().actionsCleared>=2,
            "siege telemetry did not count patrol/register waves");

    // Hunted actions are five waves and the final wave must end in a Praetor.
    director.clearTerminalState();
    require(director.requestEnforcement(91.0f,true),"Hunted claim failed to schedule Register Action");
    director.update(0.25f,true,true);
    for(int expectedWave=0;expectedWave<4;++expectedWave) {
        require(director.state().waveIndex==expectedWave,"Hunted wave index drifted before reconciliation");
        director.reconcileLiveEnemies({});
        if(expectedWave<3) director.update(0.10f,true,true);
    }
    // At this point wave 3 has just been reconciled and the director is waiting
    // for wave 4 (the fifth/final wave).
    auto finalWave=director.update(0.10f,true,true);
    require(director.state().waveIndex==4 && finalWave.size()==4,
            "Hunted Register Action did not reach five-wave final composition");
    require(finalWave.front().role==ImperialUnitRole::Praetor,
            "Hunted final Register Action wave does not begin with Praetor");

    // State must survive a save/reload during an active final wave without
    // regenerating different stable enemy IDs.
    const auto stateText=director.serializeState();
    SurfaceSiegeDirector restored(0x51E63EULL,tuning);
    std::string error;
    require(restored.restoreState(stateText,&error),"active Register Action state failed round trip: "+error);
    require(restored.state().actionId==director.state().actionId &&
            restored.state().waveIndex==director.state().waveIndex &&
            restored.state().activeEnemyIds==director.state().activeEnemyIds,
            "restored Register Action lost active stable IDs");

    // Destroying the claim beacon is a failure condition only for a Register
    // Action. The director never edits or deletes constructed world state.
    restored.notifyBeaconDestroyed();
    require(restored.state().phase==RegisterActionPhase::Failed,"Registry Beacon loss did not fail active Register Action");
    require(restored.telemetry().actionsFailed==1,"Register Action failure telemetry not incremented");

    // Malformed active state is rejected loudly.
    const std::string malformed="ELYSIUM_SURFACE_SIEGE 1\n1 5 1 2 2 0 3 0 55 1 0\n";
    require(!restored.restoreState(malformed,&error),"wave-active state with zero tracked enemies was accepted");
    require(actionId!=0,"Register Action allocated zero stable action ID");
}




void testWorldBiomeCatalogAndPlanetClassCoverage() {
    // Every PlanetClass has at least one BiomeCatalog row.
    constexpr PlanetClass kClasses[] = {
        PlanetClass::Temperate, PlanetClass::Barren, PlanetClass::Scorched,
        PlanetClass::Frozen, PlanetClass::Toxic, PlanetClass::Irradiated,
        PlanetClass::Oceanic, PlanetClass::Anomalous
    };
    require(allBiomes().size() == static_cast<std::size_t>(BiomeId::Count),
            "allBiomes size must equal BiomeId::Count");
    require(allBiomes().size() >= 40, "biome catalog unexpectedly small");

    for (PlanetClass pc : kClasses) {
        require(biomeCountFor(pc) > 0, "biome catalog empty for a PlanetClass");
        const auto rows = biomesFor(pc);
        require(rows.size() == biomeCountFor(pc), "biomesFor/biomeCountFor mismatch");
        for (const BiomeDef& def : rows) {
            require(def.planetClass == pc, "biome row planetClass mismatch");
            require(!def.name.empty(), "biome row missing name");
            require(def.tags.size() > 0, "biome row missing tags");
            require(biomeDef(def.id) != nullptr && biomeDef(def.id)->id == def.id,
                    "biomeDef lookup failed");
        }
        // planetEnvironmentFor is defined for every class (name + representative biome).
        const auto& env = planetEnvironmentFor(pc);
        require(!env.name.empty(), "planetEnvironmentFor missing name");
        require(!env.biome.empty(), "planetEnvironmentFor missing representative biome");
        const BiomeDef* def = biomeDef(defaultBiomeFor(pc));
        require(def != nullptr && def->planetClass == pc,
                "defaultBiomeFor must point at a row of the same class");
        require(def->name == env.biome,
                "PlanetEnvironment.biome must match defaultBiomeFor display name");
    }

    // Temperate is earth-like rich; Part24 playables keep known drains.
    require(biomeCountFor(PlanetClass::Temperate) >= 10, "Temperate biome count too low");
    require(planetEnvironmentFor(PlanetClass::Temperate).oxygenDrainPerSecond == 0.0f,
            "Temperate O2 drain drifted");
    require(std::abs(planetEnvironmentFor(PlanetClass::Barren).oxygenDrainPerSecond - 1.8f) < 1e-5f,
            "Barren O2 drain drifted");
    require(planetEnvironmentFor(PlanetClass::Scorched).hazardDamagePerSecond > 0.0f,
            "Scorched hazard missing");

    // Catalog classes have non-zero survival pressure placeholders.
    require(planetEnvironmentFor(PlanetClass::Toxic).oxygenDrainPerSecond > 0.0f, "Toxic O2");
    require(planetEnvironmentFor(PlanetClass::Irradiated).hazardDamagePerSecond > 0.0f, "Irradiated hazard");
    require(planetEnvironmentFor(PlanetClass::Oceanic).oxygenDrainPerSecond > 0.0f, "Oceanic O2");
    require(planetEnvironmentFor(PlanetClass::Frozen).hazardDamagePerSecond > 0.0f, "Frozen hazard");
    require(planetEnvironmentFor(PlanetClass::Anomalous).hazardDamagePerSecond > 0.0f, "Anomalous hazard");

    // Vertical-slice ShipTransit indices unchanged: 0/1/2 = T/B/S.
    require(ShipTransit::classForVerticalSliceIndex(0) == PlanetClass::Temperate, "slice 0 Temperate");
    require(ShipTransit::classForVerticalSliceIndex(1) == PlanetClass::Barren, "slice 1 Barren");
    require(ShipTransit::classForVerticalSliceIndex(2) == PlanetClass::Scorched, "slice 2 Scorched");
    require(static_cast<int>(PlanetClass::Temperate) == 0 &&
            static_cast<int>(PlanetClass::Barren) == 1 &&
            static_cast<int>(PlanetClass::Scorched) == 2,
            "PlanetClass 0/1/2 renumbered — ShipTransit contract broken");
}

void testShipTransitRepairGateAndInterPlanetPhases() {
    // Vertical-slice classes match indices 0/1/2 without needing planar World.
    require(ShipTransit::classForVerticalSliceIndex(0) == PlanetClass::Temperate, "index 0 must be Temperate");
    require(ShipTransit::classForVerticalSliceIndex(1) == PlanetClass::Barren, "index 1 must be Barren");
    require(ShipTransit::classForVerticalSliceIndex(2) == PlanetClass::Scorched, "index 2 must be Scorched");

    ShipTransit ship(0, PlanetClass::Temperate);
    require(ship.phase() == ShipFlightPhase::Docked, "starter must be Docked");
    require(ship.currentPlanetIndex() == 0, "starter dock index");
    require(ship.checklist().ready, "airworthy starter checklist must be ready");

    // Damaged hull fails takeoff closed with a readable reason.
    ship.applyHullDamage(70.0f); // 100 -> 30 < min 40
    require(ship.hullIntegrity() < ShipTransit::kMinHullTakeoff, "damage did not drop hull below takeoff min");
    require(!ship.beginTakeoff(), "damaged hull must fail takeoff");
    require(ship.lastReject() == ShipTravelReject::HullBelowTakeoff, "expected HullBelowTakeoff reject");
    require(ship.phase() == ShipFlightPhase::Docked, "failed takeoff must remain Docked");
    require(!ship.travelTo(1, PlanetClass::Barren), "travelTo must not bypass repair gate");
    require(ship.currentPlanetIndex() == 0, "failed travel must not change planet");

    // Repair kits consume progression spine (Industry RepairKit count is Game-owned;
    // world layer only accepts kit counts).
    require(ship.applyRepairKits(0) == 0, "zero kits must be a no-op");
    const int used = ship.applyRepairKits(1);
    require(used == 1, "one kit should apply");
    require(ship.hullIntegrity() > ShipTransit::kMinHullTakeoff - 1e-4f, "one kit must clear takeoff hull gate");
    require(ship.checklist().hullOk && ship.checklist().ready, "checklist must become ready after repair");

    // Cargo overload is a visible separate gate.
    ship.setCargoMassUnits(ShipTransit::kDefaultMaxCargo + 10.0f);
    require(!ship.beginTakeoff(), "overloaded cargo must fail takeoff");
    require(ship.lastReject() == ShipTravelReject::CargoOverloaded, "expected CargoOverloaded");
    ship.setCargoMassUnits(0.0f);

    // Phase walk: Docked -> TakingOff -> Atmospheric -> Orbital (hold).
    require(ship.beginTakeoff(), "repaired ship must take off");
    require(ship.phase() == ShipFlightPhase::TakingOff, "beginTakeoff enters TakingOff");
    ship.tick(ShipTransit::kTakingOffDuration);
    require(ship.phase() == ShipFlightPhase::Atmospheric, "TakingOff must advance to Atmospheric");
    ship.tick(ShipTransit::kAtmosphericDuration);
    require(ship.phase() == ShipFlightPhase::Orbital, "Atmospheric must advance to Orbital");
    ship.tick(10.0f);
    require(ship.phase() == ShipFlightPhase::Orbital, "Orbital is a hold until transit is chosen");

    require(!ship.beginTransit(0, PlanetClass::Temperate), "same-planet transit rejected");
    require(ship.lastReject() == ShipTravelReject::SamePlanet, "expected SamePlanet");
    require(!ship.beginTransit(99, PlanetClass::Barren), "invalid planet rejected");
    require(ship.lastReject() == ShipTravelReject::InvalidPlanet, "expected InvalidPlanet");

    // Temperate (0) -> Barren (1) without any planar World involvement.
    require(ship.beginTransit(1, PlanetClass::Barren), "Orbital transit Temperate->Barren");
    require(ship.phase() == ShipFlightPhase::InTransit, "beginTransit enters InTransit");
    ship.tick(ShipTransit::kTransitDuration);
    require(ship.phase() == ShipFlightPhase::Descending, "InTransit advances to Descending");
    ship.tick(ShipTransit::kDescendingDuration);
    require(ship.phase() == ShipFlightPhase::Docked, "Descending docks at destination");
    require(ship.currentPlanetIndex() == 1, "docked planet index must be Barren slot");
    require(ship.currentPlanetClass() == PlanetClass::Barren, "docked planet class must be Barren");

    // travelTo convenience walks the full graph atomically for Game/menu callers.
    require(ship.travelTo(0, PlanetClass::Temperate), "return Barren->Temperate via travelTo");
    require(ship.currentPlanetIndex() == 0, "travelTo must reseat on Temperate");
    require(ship.currentPlanetClass() == PlanetClass::Temperate, "travelTo class Temperate");
    require(ship.phase() == ShipFlightPhase::Docked, "travelTo ends Docked");

    // Determinism: identical damage/repair/travel sequences agree.
    ShipTransit a(0, PlanetClass::Temperate);
    ShipTransit b(0, PlanetClass::Temperate);
    a.applyHullDamage(80.0f);
    b.applyHullDamage(80.0f);
    require(a.applyRepairKits(2) == b.applyRepairKits(2), "repair kit consumption must match");
    require(a.travelTo(2, PlanetClass::Scorched) && b.travelTo(2, PlanetClass::Scorched),
            "paired travelTo Scorched must succeed");
    require(a.currentPlanetIndex() == b.currentPlanetIndex() &&
            a.currentPlanetClass() == b.currentPlanetClass() &&
            a.phase() == b.phase() &&
            std::abs(a.hullIntegrity() - b.hullIntegrity()) < 1e-5f,
            "paired ShipTransit sequences diverged");
}

void testPlanetEnvironmentOxygenDrainAndSuspicionSeams() {
    // Authoritative PlanetClass baseline table is shared by planar + cube-sphere.
    const auto& temperate = planetEnvironmentFor(PlanetClass::Temperate);
    const auto& barren = planetEnvironmentFor(PlanetClass::Barren);
    const auto& scorched = planetEnvironmentFor(PlanetClass::Scorched);
    require(temperate.oxygenDrainPerSecond == 0.0f, "Temperate ambient oxygen drain must be near-zero");
    require(barren.oxygenDrainPerSecond > 1.0f, "Barren baseline must drain oxygen");
    require(std::abs(barren.oxygenDrainPerSecond - 1.8f) < 1e-5f, "Barren drain drifted from 1.8/s table");
    require(scorched.oxygenDrainPerSecond > 0.0f, "Scorched baseline must drain oxygen");

    PlanetSurface barrenSphere(0x0B2A11ULL, PlanetClass::Barren);
    World barrenPlanar(0x0B2A11ULL, PlanetClass::Barren);
    PlanetSurface temperateSphere(0x7E111DULL, PlanetClass::Temperate);
    World temperatePlanar(0x7E111DULL, PlanetClass::Temperate);
    require(std::abs(barrenSphere.environment().oxygenDrainPerSecond -
                     barrenPlanar.environment().oxygenDrainPerSecond) < 1e-6f,
            "cube-sphere Barren environment table diverged from planar");
    require(std::abs(temperateSphere.environment().oxygenDrainPerSecond -
                     temperatePlanar.environment().oxygenDrainPerSecond) < 1e-6f,
            "cube-sphere Temperate environment table diverged from planar");
    require(std::abs(barrenSphere.environment().oxygenDrainPerSecond - barren.oxygenDrainPerSecond) < 1e-6f,
            "PlanetSurface::environment did not expose authoritative Barren drain");

    const float baseline = barren.oxygenDrainPerSecond;
    AtmosphereSupportSample openExterior{};
    require(std::abs(effectiveOxygenDrain(baseline, openExterior) - baseline) < 1e-5f,
            "open Barren exterior must apply full baseline drain");

    AtmosphereSupportSample sealedBreathable{1.0f, 1.0f};
    require(effectiveOxygenDrain(baseline, sealedBreathable) < 0.0f,
            "sealed breathable atmosphere must refill suit oxygen");
    require(std::abs(effectiveOxygenDrain(baseline, true) -
                     effectiveOxygenDrain(baseline, sealedBreathable)) < 1e-6f,
            "bool sealedBreathable overload diverged from sample policy");

    AtmosphereSupportSample partial{0.275f, 0.225f}; // half of breathable thresholds
    const float partialDrain = effectiveOxygenDrain(baseline, partial);
    require(partialDrain > 0.0f && partialDrain < baseline,
            "partial sealed atmosphere must reduce but not eliminate Barren drain");
    // half support => 1 - 0.75*0.5 = 0.625 of baseline
    require(std::abs(partialDrain - baseline * 0.625f) < 1e-4f,
            "partial atmosphere blend factor drifted");

    require(std::abs(effectiveOxygenDrain(temperate.oxygenDrainPerSecond, openExterior)) < 1e-6f,
            "Temperate open ambient must remain near-zero after blend");

    // SuspicionState: activity raises; claimFloor clamps decay.
    SuspicionState standing;
    standing.raise(12.0f);
    require(std::abs(standing.value() - 12.0f) < 1e-5f, "activity did not raise Suspicion");
    standing.raise(0.0f);
    require(std::abs(standing.value() - 12.0f) < 1e-5f, "zero raise mutated Suspicion");

    standing.setClaimFloor(8.0f);
    require(std::abs(standing.claimFloor() - 8.0f) < 1e-5f, "claimFloor was not stored");
    // 100s * 0.06 = 6 => 12-6=6, but floor 8 clamps to 8
    standing.decay(100.0f);
    require(std::abs(standing.value() - 8.0f) < 1e-4f, "claimFloor did not clamp Suspicion decay");

    standing.raise(5.0f);
    require(std::abs(standing.value() - 13.0f) < 1e-4f, "raise after floor did not add activity");
    standing.decay(10.0f); // 0.6 drop => 12.4
    require(std::abs(standing.value() - 12.4f) < 1e-3f, "Suspicion decay rate drifted from 0.06/s");

    standing.setClaimFloor(20.0f);
    require(std::abs(standing.value() - 20.0f) < 1e-4f, "raising claimFloor must lift standing to floor");
    standing.reduceTowardFloor(50.0f);
    require(std::abs(standing.value() - 20.0f) < 1e-4f, "reduceTowardFloor must not fall below claimFloor");

    // Siege director remains a pure consumer of the standing scalar.
    SurfaceSiegeDirector director(0x515C10FULL);
    require(director.bandFor(standing.value()) == ImperialAttentionBand::Quiet,
            "SuspicionState value should remain a plain consumer input to siege");
    standing.raise(10.0f); // 30
    require(director.bandFor(standing.value()) == ImperialAttentionBand::Noted,
            "raised SuspicionState did not reach Noted band for siege consumer");
}



void testThreePlanetScenarioOpenExteriorPressure() {
    // Lane H vertical-slice scenario lock: Temperate / Barren / Scorched baselines
    // must differ meaningfully; open exterior Barren drains suit O2 while Temperate
    // does not; Scorched applies hazardous pressure (ambient + local magma).
    // Suit meter math mirrors ecs applyVitals without linking EnTT into headless.
    const auto& temperate = planetEnvironmentFor(PlanetClass::Temperate);
    const auto& barren = planetEnvironmentFor(PlanetClass::Barren);
    const auto& scorched = planetEnvironmentFor(PlanetClass::Scorched);

    require(temperate.name == "Temperate" && barren.name == "Barren" && scorched.name == "Scorched",
            "vertical-slice planet names drifted");
    require(ShipTransit::classForVerticalSliceIndex(0) == PlanetClass::Temperate &&
            ShipTransit::classForVerticalSliceIndex(1) == PlanetClass::Barren &&
            ShipTransit::classForVerticalSliceIndex(2) == PlanetClass::Scorched,
            "ShipTransit vertical-slice index map must stay Temperate/Barren/Scorched");

    // Pairwise distinct survival pressure (O2 drain and/or hazard).
    require(temperate.oxygenDrainPerSecond == 0.0f && temperate.hazardDamagePerSecond == 0.0f,
            "Temperate must be breathable open ambient with no baseline hazard");
    require(barren.oxygenDrainPerSecond > temperate.oxygenDrainPerSecond + 1.0f,
            "Barren must drain O2 meaningfully harder than Temperate");
    require(std::abs(barren.oxygenDrainPerSecond - 1.8f) < 1e-5f, "Barren O2 drain table drifted from 1.8/s");
    require(barren.hazardDamagePerSecond == 0.0f,
            "Barren pressure is vacuum O2 drain, not thermal hazard");
    require(scorched.oxygenDrainPerSecond > 0.0f &&
            scorched.oxygenDrainPerSecond < barren.oxygenDrainPerSecond,
            "Scorched must drain some O2 but remain distinct from Barren vacuum");
    require(std::abs(scorched.oxygenDrainPerSecond - 0.65f) < 1e-5f, "Scorched O2 drain table drifted from 0.65/s");
    require(scorched.hazardDamagePerSecond > 0.0f &&
            scorched.hazardDamagePerSecond > temperate.hazardDamagePerSecond &&
            scorched.hazardDamagePerSecond > barren.hazardDamagePerSecond,
            "Scorched must uniquely carry baseline hazardous pressure");
    require(std::abs(scorched.hazardDamagePerSecond - 0.7f) < 1e-5f, "Scorched hazard table drifted from 0.7/s");

    AtmosphereSupportSample openExterior{}; // pressure=0, oxygen=0 — open exterior
    const float barrenDrain = effectiveOxygenDrain(barren.oxygenDrainPerSecond, openExterior);
    const float temperateDrain = effectiveOxygenDrain(temperate.oxygenDrainPerSecond, openExterior);
    const float scorchedDrain = effectiveOxygenDrain(scorched.oxygenDrainPerSecond, openExterior);
    require(std::abs(barrenDrain - barren.oxygenDrainPerSecond) < 1e-5f,
            "open Barren exterior must apply full baseline O2 drain");
    require(std::abs(temperateDrain) < 1e-6f,
            "open Temperate exterior must not drain O2");
    require(scorchedDrain > 0.0f && scorchedDrain < barrenDrain,
            "open Scorched exterior drain must stay between Temperate and Barren");

    // Integrate suit O2 for 10s open exterior (same scalar path Game feeds updateVitals).
    constexpr float kDt = 0.25f;
    constexpr int kSteps = 40; // 10 seconds
    float suitBarren = 100.0f;
    float suitTemperate = 100.0f;
    float suitScorched = 100.0f;
    float healthScorched = 100.0f;
    for (int i = 0; i < kSteps; ++i) {
        suitBarren = std::clamp(suitBarren - barrenDrain * kDt, 0.0f, 100.0f);
        suitTemperate = std::clamp(suitTemperate - temperateDrain * kDt, 0.0f, 100.0f);
        suitScorched = std::clamp(suitScorched - scorchedDrain * kDt, 0.0f, 100.0f);
        // Hazardous pressure damages health even while O2 remains (Scorched ambient).
        healthScorched = std::max(0.0f, healthScorched - scorched.hazardDamagePerSecond * kDt);
    }
    require(suitBarren < 100.0f - 10.0f, "Barren open exterior did not drain suit O2 over 10s");
    require(std::abs(suitBarren - (100.0f - barrenDrain * 10.0f)) < 1e-3f,
            "Barren open-exterior O2 integration drifted from baseline*dt");
    require(std::abs(suitTemperate - 100.0f) < 1e-5f,
            "Temperate open exterior must leave suit O2 unchanged");
    require(suitScorched < 100.0f && suitScorched > suitBarren,
            "Scorched open exterior must drain some O2 but less than Barren");
    require(healthScorched < 100.0f - 5.0f,
            "Scorched hazardous pressure must damage health over 10s open exterior");
    require(std::abs(healthScorched - (100.0f - scorched.hazardDamagePerSecond * 10.0f)) < 1e-3f,
            "Scorched ambient hazard integration drifted from 0.7/s table");

    // Planar local magma intensifies Scorched pressure; Temperate/Barren stay at baseline.
    World scorchedWorld(0x53434F52ULL, PlanetClass::Scorched);
    World temperateWorld(0x54454D50ULL, PlanetClass::Temperate);
    World barrenWorld(0x42415252ULL, PlanetClass::Barren);
    require(std::abs(temperateWorld.localHazardAt({8.0f, 8.0f, 8.0f})) < 1e-6f,
            "Temperate localHazard must be zero away from magma");
    require(std::abs(barrenWorld.localHazardAt({8.0f, 8.0f, 8.0f})) < 1e-6f,
            "Barren localHazard must be zero (vacuum, not thermal)");
    require(std::abs(scorchedWorld.environment().hazardDamagePerSecond - 0.7f) < 1e-5f,
            "Scorched World::environment hazard must match table");

    bool foundMagmaPressure = false;
    for (int y = 0; y < World::Height && !foundMagmaPressure; ++y) {
        for (int z = 0; z < World::Depth && !foundMagmaPressure; ++z) {
            for (int x = 0; x < World::Width && !foundMagmaPressure; ++x) {
                if (scorchedWorld.get(x, y, z) != BlockType::Magma) continue;
                const float hazard = scorchedWorld.localHazardAt(
                    {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 1.5f, static_cast<float>(z) + 0.5f});
                require(hazard > scorched.hazardDamagePerSecond + 1.0f,
                        "Scorched magma proximity must raise localHazard above ambient table");
                foundMagmaPressure = true;
            }
        }
    }
    require(foundMagmaPressure, "Scorched vertical-slice world must expose Magma for hazardous pressure");

    // Cube-sphere stores share the same authoritative baselines (no Game-only magic).
    PlanetSurface scorchedSphere(0x53434F52ULL, PlanetClass::Scorched);
    require(std::abs(scorchedSphere.environment().hazardDamagePerSecond -
                     scorchedWorld.environment().hazardDamagePerSecond) < 1e-6f,
            "cube-sphere Scorched hazard diverged from planar");
    require(std::abs(scorchedSphere.environment().oxygenDrainPerSecond -
                     scorched.oxygenDrainPerSecond) < 1e-6f,
            "cube-sphere Scorched O2 drain diverged from table");
}


void testCampaignSiegeSaveEmbedRoundTrip() {
    // Append-only ELYSIUM_SAVE 8 siege embed: mid-Announced Register Action on
    // planet 1 survives serialize → restore without bumping global save to v9.
    // Idle planets are omitted (sparse). No Game / raylib / EnTT required.
    SurfaceSiegeTuning tuning{};
    tuning.registerAnnouncementSeconds = 0.5f;
    tuning.interWaveSeconds = 0.1f;

    constexpr int kPlanetCount = 3;
    SurfaceSiegeDirector directors[kPlanetCount]{
        SurfaceSiegeDirector(0xA11E0000ULL, tuning),
        SurfaceSiegeDirector(0xA11E0001ULL, tuning),
        SurfaceSiegeDirector(0xA11E0002ULL, tuning)
    };

    require(directors[1].requestEnforcement(62.0f, /*claimed=*/true),
            "mid-Announced fixture failed to schedule Register Action");
    require(directors[1].state().phase == RegisterActionPhase::Announced &&
            directors[1].state().type == ImperialEnforcementType::RegisterAction &&
            directors[1].state().totalWaves == 3,
            "fixture is not mid-Announced three-wave Register Action");
    require(directors[1].update(0.15f, true, true).empty(),
            "announcement window ended before embed snapshot");
    const auto announced = directors[1].state();
    const auto blobBefore = directors[1].serializeState();

    // Planet 0 stays idle; planet 2 gets a terminal Cleared so sparse count is 2.
    require(directors[2].requestEnforcement(55.0f, true), "planet2 schedule failed");
    directors[2].update(0.5f, true, true); // announce expires → wave 0 spawn
    directors[2].reconcileLiveEnemies({});
    for (int w = 0; w < 2; ++w) {
        directors[2].update(0.1f, true, true);
        directors[2].reconcileLiveEnemies({});
    }
    require(directors[2].state().phase == RegisterActionPhase::Cleared,
            "planet2 Cleared terminal setup failed");

    std::ostringstream campaign;
    campaign << "ELYSIUM_SAVE 8 1\n";
    campaign << "save_generation 7\n";
    // Minimal stub fields so the embed sits where Game would put it — before END.
    campaign << "seed 1\nplanet 1\nstate 62 0 1 0 1 0\n";
    campaign << "player 0 0 0 100 100 100 100\ninventory 0\n";
    const SurfaceSiegeDirector* writePtrs[kPlanetCount] = {
        &directors[0], &directors[1], &directors[2]
    };
    appendCampaignSiegeEmbed(campaign, writePtrs, kPlanetCount);
    campaign << "END\n";

    const std::string text = campaign.str();
    require(text.find("siege_count 2\n") != std::string::npos,
            "embed did not emit sparse siege_count 2");
    require(text.find("siege 1\nELYSIUM_SURFACE_SIEGE 1\n") != std::string::npos,
            "planet 1 Announced blob missing");
    require(text.find("siege 0\n") == std::string::npos,
            "idle planet 0 should be omitted from embed");
    require(text.find("siege 2\n") != std::string::npos,
            "planet 2 Cleared blob missing");

    // Restore into fresh directors (world seeds match so future spawns stay stable).
    SurfaceSiegeDirector restored[kPlanetCount]{
        SurfaceSiegeDirector(0xA11E0000ULL, tuning),
        SurfaceSiegeDirector(0xA11E0001ULL, tuning),
        SurfaceSiegeDirector(0xA11E0002ULL, tuning)
    };
    std::istringstream in(text);
    std::string tag;
    // Skip stub preamble until siege_count (mirrors Game reading after surface blocks).
    while (in >> tag) {
        if (tag == "siege_count") break;
        if (tag == "END")
            throw std::runtime_error("reached END before siege_count");
        // discard rest of line for stub tokens that may be multi-field
        std::string line;
        std::getline(in, line);
    }
    require(tag == "siege_count", "siege_count tag not found in campaign text");
    std::size_t siegeCount{};
    require(static_cast<bool>(in >> siegeCount) && siegeCount == 2,
            "siege_count value incorrect");
    SurfaceSiegeDirector* restorePtrs[kPlanetCount] = {
        &restored[0], &restored[1], &restored[2]
    };
    std::string error;
    require(restoreCampaignSiegeEmbed(in, siegeCount, restorePtrs, kPlanetCount, &error),
            "campaign siege embed restore failed: " + error);
    require(static_cast<bool>(in >> tag) && tag == "END",
            "campaign text did not end with END after siege embed");

    require(restored[0].state().phase == RegisterActionPhase::Idle &&
            restored[0].state().actionId == 0,
            "idle planet 0 was mutated by sparse restore");
    require(restored[1].state().phase == RegisterActionPhase::Announced &&
            restored[1].state().actionId == announced.actionId &&
            restored[1].state().totalWaves == announced.totalWaves &&
            restored[1].state().waveIndex == announced.waveIndex &&
            std::abs(restored[1].state().phaseSecondsRemaining - announced.phaseSecondsRemaining) < 1e-4f,
            "mid-Announced Register Action did not round-trip through campaign embed");
    require(restored[1].serializeState() == blobBefore,
            "restored Announced serializeState diverged from pre-save blob");
    require(restored[2].state().phase == RegisterActionPhase::Cleared,
            "Cleared terminal did not round-trip");

    // WaveActive-without-ECS reconcile hook: restored WaveActive with IDs, empty ECS.
    SurfaceSiegeDirector waveDirector(0x0A7E11ULL, tuning);
    require(waveDirector.requestEnforcement(62.0f, true), "wave fixture schedule failed");
    waveDirector.update(0.5f, true, true);
    require(waveDirector.state().phase == RegisterActionPhase::WaveActive &&
            !waveDirector.state().activeEnemyIds.empty(),
            "wave fixture not WaveActive");
    const auto waveBlob = waveDirector.serializeState();
    SurfaceSiegeDirector waveRestored(0x0A7E11ULL, tuning);
    require(waveRestored.restoreState(waveBlob, &error), "wave blob restore failed: " + error);
    reconcileCampaignSiegeAfterRestore(waveRestored);
    require(waveRestored.state().phase == RegisterActionPhase::InterWave ||
            waveRestored.state().phase == RegisterActionPhase::Cleared,
            "WaveActive-without-ECS reconcile did not advance director");
    require(waveRestored.state().activeEnemyIds.empty(),
            "WaveActive-without-ECS reconcile left stale enemy IDs");

    // siege_count 0 is valid and leaves directors idle.
    std::istringstream zeroIn("0\nEND\n");
    SurfaceSiegeDirector zeroDir(0x0ULL, tuning);
    SurfaceSiegeDirector* zeroPtrs[1] = {&zeroDir};
    // Pretend we already consumed siege_count tag; count is 0.
    require(restoreCampaignSiegeEmbed(zeroIn, 0, zeroPtrs, 1, &error),
            "siege_count 0 restore failed: " + error);
    require(zeroDir.state().phase == RegisterActionPhase::Idle, "siege_count 0 mutated director");
}


void testSurvivalPressureHungerEnergyDecisions() {
    // Part 24 Survival: oxygen/hazard/energy/hunger must produce decisions.
    // Portable world-layer hook (no EnTT). Does not reimplement effectiveOxygenDrain.
    const auto& rates = survivalRateTable();
    require(std::abs(rates.hungerDrainPerSecond - 0.12f) < 1e-6f, "hunger drain table drifted from ECS 0.12/s");
    require(std::abs(rates.energySprintDrainPerSecond - 13.0f) < 1e-6f, "sprint energy drain drifted from ECS 13/s");
    require(std::abs(rates.energyRecoverPerSecond - 8.0f) < 1e-6f, "energy recover drifted from ECS 8/s");

    const auto& barren = planetEnvironmentFor(PlanetClass::Barren);
    const auto& scorched = planetEnvironmentFor(PlanetClass::Scorched);
    const auto& temperate = planetEnvironmentFor(PlanetClass::Temperate);
    AtmosphereSupportSample openExterior{};
    const float barrenDrain = effectiveOxygenDrain(barren.oxygenDrainPerSecond, openExterior);
    require(barrenDrain > 0.0f, "fixture requires positive Barren open-exterior O2 drain");

    SurvivalMeters full{};
    SurvivalContext calm{};
    calm.effectiveOxygenDrainPerSecond = effectiveOxygenDrain(temperate.oxygenDrainPerSecond, openExterior);
    calm.hazardDamagePerSecond = temperate.hazardDamagePerSecond;
    auto calmDecision = evaluateSurvivalPressure(full, calm);
    require(!calmDecision.producesDecision(), "full meters on Temperate open ambient must not force survival decisions");

    // Oxygen decision: Barren vacuum drain advises seek-atmosphere; critical at empty suit.
    SurvivalContext vacuum{};
    vacuum.effectiveOxygenDrainPerSecond = barrenDrain;
    auto o2Advise = evaluateSurvivalPressure(full, vacuum);
    require(o2Advise.seekAtmosphere() && o2Advise.atmosphere == SurvivalUrgency::Advise,
            "Barren open exterior must Advise seekAtmosphere");
    require(!o2Advise.seekFood() && !o2Advise.conserveEnergy(),
            "full hunger/energy must stay quiet under O2-only pressure");

    SurvivalMeters lowO2 = full;
    lowO2.oxygen = 10.0f;
    auto o2Crit = evaluateSurvivalPressure(lowO2, vacuum);
    require(o2Crit.atmosphere == SurvivalUrgency::Critical && o2Crit.lifeThreat(),
            "suit O2 <15 under drain must be Critical lifeThreat");

    // Sealed refill must clear atmosphere decision when O2 healthy (O2 seam intact).
    SurvivalContext sealed{};
    sealed.effectiveOxygenDrainPerSecond = effectiveOxygenDrain(barren.oxygenDrainPerSecond, AtmosphereSupportSample{1.0f, 1.0f});
    require(sealed.effectiveOxygenDrainPerSecond < 0.0f, "sealed breathable must remain refill (negative drain)");
    auto sealedDecision = evaluateSurvivalPressure(full, sealed);
    require(!sealedDecision.seekAtmosphere(),
            "sealed refill with healthy O2 must not seekAtmosphere");

    // Hazard decision: Scorched ambient Advises shelter.
    SurvivalContext scorchedCtx = calm;
    scorchedCtx.hazardDamagePerSecond = scorched.hazardDamagePerSecond;
    auto hazardAdvise = evaluateSurvivalPressure(full, scorchedCtx);
    require(hazardAdvise.seekShelter() && hazardAdvise.shelter == SurvivalUrgency::Advise,
            "Scorched hazard must Advise seekShelter");

    // Energy decision: sprint projection depletes suit energy → conserveEnergy.
    SurvivalContext sprinting = calm;
    sprinting.sprinting = true;
    sprinting.moving = true;
    SurvivalMeters afterSprint = full;
    for (int i = 0; i < 20; ++i) {
        afterSprint = projectSurvivalMeters(afterSprint, sprinting, 0.5f, rates);
    }
    require(afterSprint.energy < 15.0f, "20s sprint projection must deplete energy into Urgent band");
    auto energyDecision = evaluateSurvivalPressure(afterSprint, sprinting);
    require(energyDecision.conserveEnergy(),
            "low energy while sprinting must produce conserveEnergy decision");
    require(energyDecision.energy == SurvivalUrgency::Urgent ||
            energyDecision.energy == SurvivalUrgency::Critical,
            "depleted sprint energy must be Urgent or Critical");

    // Rest recovers energy and clears conserve when above Advise band and not sprinting.
    SurvivalMeters rested = afterSprint;
    SurvivalContext resting = calm;
    for (int i = 0; i < 30; ++i) {
        rested = projectSurvivalMeters(rested, resting, 0.5f, rates);
    }
    require(rested.energy > 50.0f, "rest projection must recover energy above Advise band");
    auto restedDecision = evaluateSurvivalPressure(rested, resting);
    require(!restedDecision.conserveEnergy(),
            "recovered energy at rest must clear conserveEnergy");

    // Hunger decision: long idle projection drains hunger → seekFood; ration clears it.
    SurvivalMeters starving = full;
    SurvivalContext idle = calm;
    // 0.12/s * ~500s => ~60 drain → hunger ~40 (Advise). Push further to Urgent/Critical.
    for (int i = 0; i < 900; ++i) {
        starving = projectSurvivalMeters(starving, idle, 1.0f, rates);
    }
    require(starving.hunger < 20.0f, "long idle projection must enter hunger Urgent band");
    auto foodDecision = evaluateSurvivalPressure(starving, idle);
    require(foodDecision.seekFood(), "low hunger must produce seekFood decision");
    require(foodDecision.food == SurvivalUrgency::Urgent || foodDecision.food == SurvivalUrgency::Critical,
            "hunger <20 must be Urgent or Critical");

    const float afterRation = applySurvivalRation(starving.hunger, 40.0f);
    require(afterRation > starving.hunger + 20.0f, "ration must restore meaningful hunger");
    SurvivalMeters fed = starving;
    fed.hunger = afterRation;
    auto fedDecision = evaluateSurvivalPressure(fed, idle);
    // May still Advise if <50 after one ration from near-zero; force full clear.
    fed.hunger = applySurvivalRation(fed.hunger, 40.0f);
    fedDecision = evaluateSurvivalPressure(fed, idle);
    require(!fedDecision.seekFood(), "rations restoring hunger above 50 must clear seekFood");

    // Starvation damage path: empty hunger projects health loss (decision = Critical).
    SurvivalMeters emptyGut = full;
    emptyGut.hunger = 0.0f;
    auto starveCrit = evaluateSurvivalPressure(emptyGut, idle);
    require(starveCrit.food == SurvivalUrgency::Critical && starveCrit.lifeThreat(),
            "zero hunger must be Critical lifeThreat");
    SurvivalMeters afterStarveTick = projectSurvivalMeters(emptyGut, idle, 2.0f, rates);
    require(afterStarveTick.health < 100.0f - 2.0f,
            "starvation damage must reduce health over 2s at 1.5/s");
}


void testSuspicionLedgerPerSystemIsolationAndClaimPushAnnouncement() {
    // Part 24 / CP8: Suspicion is per stable system id. Raising one system must
    // not pollute another. Siege remains a pure consumer of the standing scalar.
    SuspicionLedger ledger;
    constexpr std::uint64_t kSystemA = 0xA11A11A11A11ULL;
    constexpr std::uint64_t kSystemB = 0xB22B22B22B22ULL;

    require(ledger.empty() && ledger.systemCount() == 0, "fresh ledger must start empty");
    require(ledger.find(kSystemA) == nullptr, "untouched system must not appear in find()");
    require(ledger.valueOrZero(kSystemA) == 0.0f, "valueOrZero must be 0 for missing systems");

    ledger.system(kSystemA).raise(40.0f);
    ledger.system(kSystemA).setClaimFloor(8.0f);
    require(std::abs(ledger.valueOrZero(kSystemA) - 40.0f) < 1e-4f, "system A standing not stored");
    require(ledger.find(kSystemB) == nullptr, "touching A must not mint B");
    require(ledger.valueOrZero(kSystemB) == 0.0f, "system B must remain zero while A is raised");
    require(ledger.systemCount() == 1, "ledger should hold exactly one touched system");

    ledger.system(kSystemB).raise(70.0f);
    require(std::abs(ledger.valueOrZero(kSystemA) - 40.0f) < 1e-4f, "raising B mutated A standing");
    require(std::abs(ledger.valueOrZero(kSystemB) - 70.0f) < 1e-4f, "system B standing not stored");
    require(ledger.systemCount() == 2, "ledger should hold both touched systems");

    ledger.decayAll(100.0f); // 6.0 drop at 0.06/s
    require(std::abs(ledger.valueOrZero(kSystemA) - 34.0f) < 1e-3f, "decayAll did not decay system A");
    require(std::abs(ledger.valueOrZero(kSystemB) - 64.0f) < 1e-3f, "decayAll did not decay system B");
    // claimFloor on A is 8; decay stayed above floor
    require(std::abs(ledger.claimFloorOrZero(kSystemA) - 8.0f) < 1e-5f, "claimFloorOrZero lost A floor");
    require(ledger.claimFloorOrZero(kSystemB) == 0.0f, "untouched floor on B should remain 0");

    // Claim-pushed announcement: claimed + Marked standing => Announced Register Action.
    // This is the portable Empire dispatch seam (Game rolls; director owns meaning).
    SurfaceSiegeTuning tuning{};
    tuning.registerAnnouncementSeconds = 0.5f;
    SurfaceSiegeDirector director(0xC1A17EULL, tuning);
    const float pushedStanding = ledger.valueOrZero(kSystemB); // ~64 => Marked
    require(director.bandFor(pushedStanding) == ImperialAttentionBand::Marked,
            "claim-push fixture standing is not Marked");
    require(director.requestEnforcement(pushedStanding, /*claimed=*/true),
            "claim-pushed Marked standing failed to schedule Register Action");
    require(director.state().type == ImperialEnforcementType::RegisterAction &&
            director.state().phase == RegisterActionPhase::Announced &&
            director.state().totalWaves == 3,
            "claim-pushed path did not enter announced three-wave Register Action");
    require(director.update(0.25f, true, true).empty(),
            "announcement window ended early on claim-push fixture");

    // Unclaimed high standing remains patrol-only (territorial rule).
    SurfaceSiegeDirector patrolDirector(0xC1A17EULL, tuning);
    require(patrolDirector.requestEnforcement(pushedStanding, /*claimed=*/false),
            "unclaimed Marked standing failed to schedule patrol");
    require(patrolDirector.state().type == ImperialEnforcementType::Patrol,
            "unclaimed claim-push incorrectly scheduled Register Action");
}


void testRegistryBeaconClaimDistinctIntactAndStableId() {
    RegistryBeaconAuthority a(0xBEAC001ULL);
    RegistryBeaconAuthority b(0xBEAC001ULL);

    require(!a.claimed(0) && !a.beaconIntact(0) && a.stableBeaconId(0) == 0,
            "empty claim slot must start clear");
    require(a.fileClaim(0, RegistryBeaconDomain::Spherical) != 0, "fileClaim must allocate stable id");
    require(a.claimed(0) && a.beaconIntact(0), "fileClaim must set claimed and beaconIntact distinctly true");
    const auto id0 = a.stableBeaconId(0);
    require(id0 != 0, "stable beacon id must be non-zero");
    require(a.fileClaim(0, RegistryBeaconDomain::Spherical) == id0,
            "re-file intact claim must keep the same stable identity");
    require(b.fileClaim(0, RegistryBeaconDomain::Spherical) == id0,
            "same seed/history must allocate the same beacon stable id");

    // Director seam: claimed true with beaconIntact false fails Register Action
    // without requiring Game to alias the two arguments.
    SurfaceSiegeTuning tuning{};
    tuning.registerAnnouncementSeconds = 0.25f;
    SurfaceSiegeDirector director(0x51E63EULL, tuning);
    require(director.requestEnforcement(62.0f, /*claimed=*/true),
            "Marked claim must schedule Register Action");
    require(director.state().phase == RegisterActionPhase::Announced, "expected Announced");
    require(director.update(0.05f, /*claimed=*/true, /*beaconIntact=*/false).empty(),
            "beaconIntact false must not emit spawns");
    require(director.state().phase == RegisterActionPhase::Failed,
            "distinct beaconIntact=false must fail active Register Action while claimed stays conceptually true");

    // destroyBeacon clears both filing and objective; Game notifies director separately.
    SurfaceSiegeDirector director2(0x51E63EULL, tuning);
    require(director2.requestEnforcement(62.0f, true), "second director schedule failed");
    const auto prior = a.destroyBeacon(0);
    require(prior == id0, "destroyBeacon must return prior intact stable id");
    require(!a.claimed(0) && !a.beaconIntact(0) && a.stableBeaconId(0) == 0,
            "destroyBeacon must clear claimed, intact, and stable id");
    director2.notifyBeaconDestroyed();
    require(director2.state().phase == RegisterActionPhase::Failed,
            "notifyBeaconDestroyed must fail Register Action after beacon loss");

    // Legacy schema-8 claimed flags migrate preexisting filed planets to intact
    // slots with stable IDs (world-layer preexisting migration; no ECS system).
    RegistryBeaconAuthority legacy(0x1E6AC401ULL);
    legacy.restoreClaimedFlags(true, false, true);
    require(legacy.claimed(0) && legacy.beaconIntact(0) && legacy.stableBeaconId(0) != 0,
            "legacy claimed[0] must migrate to intact+stable id");
    require(!legacy.claimed(1) && !legacy.beaconIntact(1) && legacy.stableBeaconId(1) == 0,
            "unclaimed legacy planet must stay clear");
    require(legacy.claimed(2) && legacy.beaconIntact(2) && legacy.stableBeaconId(2) != 0,
            "legacy claimed[2] must migrate to intact+stable id");
    require(legacy.stableBeaconId(0) != legacy.stableBeaconId(2),
            "migrated planet beacons must not collide stable ids");

    // Explicit divergence restore: claimed without intact is legal and fails siege.
    RegistryBeaconSlot diverged{};
    diverged.claimed = true;
    diverged.beaconIntact = false;
    diverged.stableBeaconId = 0;
    diverged.domain = RegistryBeaconDomain::Planar;
    RegistryBeaconAuthority probe(0xC0FFEE01ULL);
    require(probe.restoreSlot(1, diverged), "diverged claim-without-intact slot must restore");
    require(probe.claimed(1) && !probe.beaconIntact(1),
            "restored slot must keep claimed distinct from beaconIntact");
    SurfaceSiegeDirector director3(0x51E63EULL, tuning);
    require(director3.requestEnforcement(80.0f, probe.claimed(1)), "enforcement with claimed flag");
    director3.update(0.01f, probe.claimed(1), probe.beaconIntact(1));
    require(director3.state().phase == RegisterActionPhase::Failed,
            "Game-style distinct probe must fail when beaconIntact is false");
}


void testEarlyMetallurgyObtainabilityLadder() {
    // Harvest gates already encode the metallurgy ladder on block props.
    require(blockProperties(BlockType::Dirt).harvestTier == 0, "soft dirt must be improvised-mineable");
    require(blockProperties(BlockType::Stone).harvestTier == EarlyMetallurgy::kStone, "stone harvest gate");
    require(blockProperties(BlockType::CopperOre).harvestTier == EarlyMetallurgy::kStone, "copper requires stone head");
    require(blockProperties(BlockType::TinOre).harvestTier == EarlyMetallurgy::kStone, "tin requires stone head");
    require(blockProperties(BlockType::CoalOre).harvestTier == EarlyMetallurgy::kStone, "coal requires stone head");
    require(blockProperties(BlockType::IronOre).harvestTier == EarlyMetallurgy::kBronze, "iron requires bronze head");
    require(blockProperties(BlockType::SteelPlate).harvestTier == EarlyMetallurgy::kSteel, "steel plate requires steel head");

    require(!EarlyMetallurgy::canHarvest(EarlyMetallurgy::kImprovised, EarlyMetallurgy::kStone),
            "improvised must not mine stone/ores");
    require(EarlyMetallurgy::canHarvest(EarlyMetallurgy::kStone, EarlyMetallurgy::kStone),
            "stone head mines stone-tier materials");
    require(!EarlyMetallurgy::canHarvest(EarlyMetallurgy::kStone, EarlyMetallurgy::kBronze),
            "stone head must not mine iron");
    require(EarlyMetallurgy::canHarvest(EarlyMetallurgy::kBronze, EarlyMetallurgy::kBronze),
            "bronze head mines iron");

    // Closed link: improvised → stone with Stone blocks.
    auto stone = EarlyMetallurgy::evaluate(EarlyMetallurgy::kImprovised, EarlyMetallurgy::HeadKind::Stone,
                                           3, 0, 0, 0, 0, 0, 0);
    require(!stone.canApply, "stone head must require 4 Stone");
    stone = EarlyMetallurgy::evaluate(EarlyMetallurgy::kImprovised, EarlyMetallurgy::HeadKind::Stone,
                                      4, 0, 0, 0, 0, 0, 0);
    require(stone.canApply && stone.nextTier == EarlyMetallurgy::kStone &&
            stone.cost.mode == EarlyMetallurgy::CostMode::StoneBlocks,
            "4 Stone must install stone mining head");

    // Bronze is gated on stone and prefers AlloyCrucible Bronze Ingot output.
    auto bronze = EarlyMetallurgy::evaluate(EarlyMetallurgy::kImprovised, EarlyMetallurgy::HeadKind::Bronze,
                                            0, 9, 9, 9, 0, 0, 0);
    require(!bronze.canApply, "bronze must refuse improvised tier");
    bronze = EarlyMetallurgy::evaluate(EarlyMetallurgy::kStone, EarlyMetallurgy::HeadKind::Bronze,
                                       0, 0, 0, EarlyMetallurgy::kBronzeIngotCost, 0, 0, 0);
    require(bronze.canApply && bronze.nextTier == EarlyMetallurgy::kBronze &&
            bronze.cost.mode == EarlyMetallurgy::CostMode::BronzeIngots,
            "4 Bronze Ingot must install bronze head");
    bronze = EarlyMetallurgy::evaluate(EarlyMetallurgy::kStone, EarlyMetallurgy::HeadKind::Bronze,
                                       0, 3, 1, 0, 0, 0, 0);
    require(bronze.canApply && bronze.cost.mode == EarlyMetallurgy::CostMode::BronzeOres,
            "ore shortcut must remain for pre-machine bronze");

    // Steel prefers Steel Ingot; ore shortcut retained.
    auto steel = EarlyMetallurgy::evaluate(EarlyMetallurgy::kStone, EarlyMetallurgy::HeadKind::Steel,
                                           0, 0, 0, 0, 9, 9, 9);
    require(!steel.canApply, "steel must require bronze tier first");
    steel = EarlyMetallurgy::evaluate(EarlyMetallurgy::kBronze, EarlyMetallurgy::HeadKind::Steel,
                                      0, 0, 0, 0, 0, 0, EarlyMetallurgy::kSteelIngotCost);
    require(steel.canApply && steel.nextTier == EarlyMetallurgy::kSteel &&
            steel.cost.mode == EarlyMetallurgy::CostMode::SteelIngots,
            "2 Steel Ingot must install steel head");
    steel = EarlyMetallurgy::evaluate(EarlyMetallurgy::kBronze, EarlyMetallurgy::HeadKind::Steel,
                                      0, 0, 0, 0, 4, 2, 0);
    require(steel.canApply && steel.cost.mode == EarlyMetallurgy::CostMode::SteelOres,
            "ore shortcut must remain for pre-machine steel");

    // Industry recipes that feed the preferred path still exist and keep IDs stable.
    require(surfaceRecipe(SurfaceRecipeId::AlloyBronze) &&
            surfaceRecipe(SurfaceRecipeId::AlloyBronze)->outputItemId == static_cast<int>(IndustryItemId::BronzeIngot) &&
            surfaceRecipe(SurfaceRecipeId::AlloyBronze)->outputCount == EarlyMetallurgy::kBronzeIngotCost,
            "AlloyBronze batch must equal bronze-head ingot cost");
    require(surfaceRecipe(SurfaceRecipeId::AlloySteel) &&
            surfaceRecipe(SurfaceRecipeId::AlloySteel)->outputItemId == static_cast<int>(IndustryItemId::SteelIngot),
            "AlloySteel must produce Steel Ingot for the steel-head path");

    require(EarlyMetallurgy::headLabel(EarlyMetallurgy::kImprovised) == "Improvised tools", "improvised label");
    require(EarlyMetallurgy::headLabel(EarlyMetallurgy::kStone) == "Stone mining head", "stone label");
    require(EarlyMetallurgy::clampTier(-3) == 0 && EarlyMetallurgy::clampTier(99) == 5, "tier clamp bounds");
}



void testChunkVoxelSpansAdaptiveIdentityAndNoiseBasisCache() {
    // Homogeneous encode.
    {
        constexpr int e = 8;
        std::vector<BlockType> dense(static_cast<std::size_t>(e * e * e), BlockType::Stone);
        const auto spans = ChunkVoxelSpans::encodeAdaptive(dense.data(), e);
        require(spans.mode() == ChunkVoxelSpans::Mode::Homogeneous, "uniform cube must encode homogeneous");
        require(spans.homogeneousValue() == BlockType::Stone, "homogeneous value mismatch");
        for (int v = 0; v < e; ++v) for (int u = 0; u < e; ++u) for (int r = 0; r < e; ++r)
            require(spans.get(u, v, r) == BlockType::Stone, "homogeneous get mismatch");
    }

    // Stratified columns -> RLE, identical to dense source.
    {
        constexpr int e = 16;
        std::vector<BlockType> dense(static_cast<std::size_t>(e * e * e), BlockType::Air);
        for (int v = 0; v < e; ++v) for (int u = 0; u < e; ++u) {
            for (int r = 0; r < e; ++r) {
                BlockType t = BlockType::Air;
                if (r < 5) t = BlockType::Stone;
                else if (r < 7) t = BlockType::Dirt;
                else if (r == 7) t = BlockType::Grass;
                dense[static_cast<std::size_t>(ChunkVoxelSpans::flatIndex(u, v, r, e))] = t;
            }
        }
        const auto spans = ChunkVoxelSpans::encodeAdaptive(dense.data(), e);
        require(spans.mode() == ChunkVoxelSpans::Mode::RleColumns,
                "stratified terrain should prefer RLE columns");
        require(spans.estimatedBytes() < dense.size() * sizeof(BlockType),
                "RLE should beat dense bytes");
        for (int v = 0; v < e; ++v) for (int u = 0; u < e; ++u) for (int r = 0; r < e; ++r) {
            const auto expected = dense[static_cast<std::size_t>(ChunkVoxelSpans::flatIndex(u, v, r, e))];
            require(spans.get(u, v, r) == expected, "RLE logical voxel mismatch vs dense source");
        }
        require(spans.materialize() == dense, "materialize must restore identical logical voxels");
    }

    // Pathological checkerboard promotes to dense; still identical.
    {
        constexpr int e = 8;
        std::vector<BlockType> dense(static_cast<std::size_t>(e * e * e), BlockType::Air);
        for (int v = 0; v < e; ++v) for (int u = 0; u < e; ++u) for (int r = 0; r < e; ++r) {
            const bool odd = ((u + v + r) & 1) != 0;
            dense[static_cast<std::size_t>(ChunkVoxelSpans::flatIndex(u, v, r, e))] =
                odd ? BlockType::Stone : BlockType::Air;
        }
        const auto spans = ChunkVoxelSpans::encodeAdaptive(dense.data(), e);
        require(spans.mode() == ChunkVoxelSpans::Mode::Dense, "checkerboard must promote to dense");
        for (int v = 0; v < e; ++v) for (int u = 0; u < e; ++u) for (int r = 0; r < e; ++r) {
            const auto expected = dense[static_cast<std::size_t>(ChunkVoxelSpans::flatIndex(u, v, r, e))];
            require(spans.get(u, v, r) == expected, "dense adaptive get mismatch");
        }
    }

    // Live SurfaceChunkData path: adaptive voxels match snapshot.get for every halo cell.
    {
        PlanetSurface planet(0xC04C0F70ULL, PlanetClass::Temperate);
        JobSystem jobs(SerialJobs);
        SurfaceChunkCache cache(jobs, 2, 8U * 1024U * 1024U, 2);
        const PlanetChunkAddress address{CubeFace::PositiveZ, 0, 0, 0};
        cache.beginFrame();
        require(cache.request(planet, address, SurfaceChunkPriority::Visible),
                "chunk opt cache request failed");
        cache.sync(planet);
        const auto data = cache.find(address);
        require(data != nullptr, "chunk opt cache miss");
        require(data->voxels.extent() == SurfaceChunkData::HaloSize, "halo extent mismatch");
        require(data->voxels.mode() == ChunkVoxelSpans::Mode::RleColumns ||
                    data->voxels.mode() == ChunkVoxelSpans::Mode::Dense,
                "surface chunk should encode RLE or Dense");

        const auto snapshot = planet.snapshot();
        const int u0 = address.u * PlanetSurface::ChunkSize;
        const int v0 = address.v * PlanetSurface::ChunkSize;
        const int r0 = address.radial * PlanetSurface::ChunkSize;
        for (int lv = -SurfaceChunkData::Halo; lv <= PlanetSurface::ChunkSize; ++lv) {
            for (int lu = -SurfaceChunkData::Halo; lu <= PlanetSurface::ChunkSize; ++lu) {
                for (int lr = -SurfaceChunkData::Halo; lr <= PlanetSurface::ChunkSize; ++lr) {
                    SurfaceCellAddress world{address.face, u0 + lu, v0 + lv, r0 + lr};
                    if (world.radial >= 0 && world.radial < PlanetSurface::RadialLayers)
                        world = snapshot.normalize(world);
                    require(data->getWithHalo(lu, lv, lr) == snapshot.get(world),
                            "SurfaceChunkData adaptive voxels differ from snapshot");
                }
            }
        }
    }

    // Noise basis: deterministic across instances (P0-12 memo must not change samples).
    {
        const auto a = SphericalNoiseBasis::make(0x4E4F4953ULL, 0x4C41594FULL);
        const auto b = SphericalNoiseBasis::make(0x4E4F4953ULL, 0x4C41594FULL);
        const Vec3 d = normalize(Vec3{0.2f, 0.7f, -0.4f});
        require(std::abs(a.evaluate(d) - b.evaluate(d)) < 1e-7f, "noise basis not deterministic");
        PlanetSurface p0(0x4E4F4953ULL, PlanetClass::Temperate);
        PlanetSurface p1(0x4E4F4953ULL, PlanetClass::Temperate);
        for (int v = 0; v < 8; ++v) for (int u = 0; u < 8; ++u) {
            require(p0.surfaceRadial(CubeFace::PositiveZ, u, v) ==
                        p1.surfaceRadial(CubeFace::PositiveZ, u, v),
                    "noise-cached surface radials must match across instances");
            for (int r = 10; r < 18; ++r) {
                require(p0.get(CubeFace::PositiveZ, u, v, r) == p1.get(CubeFace::PositiveZ, u, v, r),
                        "noise-cached generated blocks must match across instances");
            }
        }
    }
}

void testChunkOccupancyExtremityAndAdaptiveRep() {
    // --- Unit: classify empty / homogeneous / full / mixed ---
    {
        auto empty = analyzeChunkOccupancy(2, 2, 2, [](int, int, int) { return BlockType::Air; });
        require(empty.kind == ChunkRepKind::Empty && empty.solidCount == 0 && !empty.extents.any,
                "all-air volume must classify Empty");
    }
    {
        auto homo = analyzeChunkOccupancy(2, 2, 2, [](int, int, int) { return BlockType::Stone; });
        require(homo.kind == ChunkRepKind::Homogeneous && homo.fill == BlockType::Stone,
                "uniform stone must classify Homogeneous");
        require(homo.extents.any && homo.extents.minX == 0 && homo.extents.maxX == 1 &&
                homo.extents.minY == 0 && homo.extents.maxY == 1 &&
                homo.extents.minZ == 0 && homo.extents.maxZ == 1,
                "homogeneous solid extents must span full volume");
    }
    {
        auto full = analyzeChunkOccupancy(2, 2, 2, [](int x, int y, int z) {
            return ((x + y + z) & 1) ? BlockType::Stone : BlockType::Dirt;
        });
        require(full.kind == ChunkRepKind::Full && full.solidCount == 8,
                "all-solid multi-material must classify Full");
    }
    {
        auto mixed = analyzeChunkOccupancy(4, 4, 4, [](int x, int y, int z) {
            return (y == 0 && x == 1 && z == 2) ? BlockType::Stone : BlockType::Air;
        });
        require(mixed.kind == ChunkRepKind::Mixed && mixed.solidCount == 1, "single solid must be Mixed");
        require(mixed.extents.minX == 1 && mixed.extents.maxX == 1 &&
                mixed.extents.minY == 0 && mixed.extents.maxY == 0 &&
                mixed.extents.minZ == 2 && mixed.extents.maxZ == 2,
                "extremity bounds must match the single occupied cell");
    }

    // --- World: extents update on edit (expand + boundary shrink rescan) ---
    World world(0x0CC01A11ULL, PlanetClass::Temperate);
    const int cx = 0, cy = 0, cz = 0;
    // Bulk clear — per-cell set(Air) thrash O(n²) occupancy rescans + full exterior
    // connectivity refresh on every solid→air transition (~minutes on Temperate).
    world.fillChunk(cx, cy, cz, BlockType::Air);
    require(world.chunkOccupancy(cx, cy, cz).kind == ChunkRepKind::Empty,
            "cleared chunk must be Empty after rebuild");

    world.set(3, 5, 7, BlockType::Stone);
    const auto afterPlace = world.chunkOccupancy(cx, cy, cz);
    require(afterPlace.kind == ChunkRepKind::Mixed && afterPlace.solidCount == 1,
            "placing one solid into Empty must become Mixed with solidCount 1");
    require(afterPlace.extents.minX == 3 && afterPlace.extents.maxX == 3 &&
            afterPlace.extents.minY == 5 && afterPlace.extents.maxY == 5 &&
            afterPlace.extents.minZ == 7 && afterPlace.extents.maxZ == 7,
            "incremental place must set extremity to the edited cell");

    world.set(4, 5, 7, BlockType::Stone);
    const auto expanded = world.chunkOccupancy(cx, cy, cz);
    require(expanded.solidCount == 2 && expanded.extents.maxX == 4 && expanded.extents.minX == 3,
            "incremental place must expand maxX extremity");

    world.set(3, 5, 7, BlockType::Air); // remove boundary minX cell -> rescan
    const auto shrunk = world.chunkOccupancy(cx, cy, cz);
    require(shrunk.solidCount == 1 && shrunk.extents.minX == 4 && shrunk.extents.maxX == 4,
            "boundary solid removal must rescan and shrink extremities");

    // --- SurfaceChunkData: occupancy + empty mesh short-circuit (via ChunkVoxelSpans) ---
    SurfaceChunkData packet{};
    packet.address = {CubeFace::PositiveY, 0, 0, 0};
    packet.revision = 1;
    const int haloCells = SurfaceChunkData::HaloSize * SurfaceChunkData::HaloSize * SurfaceChunkData::HaloSize;
    std::vector<BlockType> dense(static_cast<std::size_t>(haloCells), BlockType::Air);
    packet.voxels = ChunkVoxelSpans::encodeAdaptive(dense.data(), SurfaceChunkData::HaloSize);
    packet.recomputeOccupancy();
    require(packet.occupancy.kind == ChunkRepKind::Empty, "air SurfaceChunkData must be Empty");
    require(buildPlanetSurfaceChunkMesh(packet).vertexCount() == 0,
            "Empty adaptive chunk must emit zero mesh vertices");

    auto setCore = [&](int lu, int lv, int lr, BlockType t) {
        const int hu = lu + SurfaceChunkData::Halo;
        const int hv = lv + SurfaceChunkData::Halo;
        const int hr = lr + SurfaceChunkData::Halo;
        const int hi = ChunkVoxelSpans::flatIndex(hu, hv, hr, SurfaceChunkData::HaloSize);
        dense[static_cast<std::size_t>(hi)] = t;
    };
    for (int lr = 2; lr <= 4; ++lr)
        for (int lu = 1; lu <= 2; ++lu)
            for (int lv = 3; lv <= 3; ++lv)
                setCore(lu, lv, lr, BlockType::Stone);
    packet.voxels = ChunkVoxelSpans::encodeAdaptive(dense.data(), SurfaceChunkData::HaloSize);
    packet.recomputeOccupancy();
    require(packet.occupancy.kind == ChunkRepKind::Mixed, "partial stone core must be Mixed");
    require(packet.occupancy.extents.minX == 1 && packet.occupancy.extents.maxX == 2 &&
            packet.occupancy.extents.minY == 3 && packet.occupancy.extents.maxY == 3 &&
            packet.occupancy.extents.minZ == 2 && packet.occupancy.extents.maxZ == 4,
            "SurfaceChunkData extremity (U,V,R)=(X,Y,Z) mismatch");
    const int col = 1 + SurfaceChunkData::CoreSize * 3;
    require(packet.columnMinR[static_cast<std::size_t>(col)] == 2 &&
            packet.columnMaxR[static_cast<std::size_t>(col)] == 4,
            "per-column radial extremity mismatch");

    const auto mesh = buildPlanetSurfaceChunkMesh(packet);
    require(mesh.vertexCount() > 0, "occupied SurfaceChunkData must emit mesh geometry");

    // --- Cache build path stamps occupancy ---
    JobSystem jobs(SerialJobs);
    PlanetSurface planet(0x0CC01A11ULL, PlanetClass::Temperate);
    SurfaceChunkCache cache(jobs, 4, 4U * 1024U * 1024U, 4);
    cache.beginFrame();
    const PlanetChunkAddress addr{CubeFace::PositiveY, 0, 0, 0};
    require(cache.request(planet, addr, SurfaceChunkPriority::Visible), "cache request failed");
    cache.sync(planet);
    const auto cached = cache.find(addr);
    require(cached && cached->occupancy.cellCount == SurfaceChunkData::CoreSize *
                                                     SurfaceChunkData::CoreSize *
                                                     SurfaceChunkData::CoreSize,
            "cached packet missing occupancy cellCount");
    require(cached->occupancy.kind != ChunkRepKind::Empty || cached->occupancy.solidCount == 0,
            "cached occupancy kind inconsistent");
}


void testSphericalMacroAndMicroGreedyMeshing() {
    // Controlled 4x4x4 stone prism above terrain → true 2D greedy should collapse
    // each of the six faces toward one quad. Assert on the *delta* vs the virgin
    // chunk mesh so uneven heightfield tops do not drown the fixture signal.
    PlanetSurface planet(0x67EED401ULL, PlanetClass::Barren);
    const CubeFace face = CubeFace::PositiveZ;
    const int u0 = 8, v0 = 8;
    const int base = std::max({planet.surfaceRadial(face,u0,v0),
                               planet.surfaceRadial(face,u0+3,v0),
                               planet.surfaceRadial(face,u0,v0+3),
                               planet.surfaceRadial(face,u0+3,v0+3)}) + 3;
    require(base + 3 < PlanetSurface::RadialLayers, "greedy fixture escaped radial bounds");

    const PlanetChunkAddress addr{face, u0 / PlanetSurface::ChunkSize, v0 / PlanetSurface::ChunkSize, 0};
    JobSystem jobs(SerialJobs);
    SurfaceChunkCache cache(jobs, 4, 8U * 1024U * 1024U, 4);

    auto settle = [&]() {
        cache.beginFrame();
        require(cache.request(planet, addr, SurfaceChunkPriority::EditRemesh), "greedy cache request failed");
        settleSurfaceCache(cache, planet);
        const auto data = cache.find(addr);
        require(data != nullptr, "greedy fixture chunk missing");
        return data;
    };

    const auto beforeData = settle();
    const auto beforeCached = buildPlanetSurfaceChunkMesh(*beforeData);
    const auto beforeSnap = buildPlanetSurfaceChunkMesh(planet.snapshot(), addr);
    require(beforeCached.macroQuads == beforeSnap.macroQuads && beforeCached.microQuads == beforeSnap.microQuads,
            "cached vs snapshot greedy topology diverged (pre-prism)");

    for (int r = base; r < base + 4; ++r)
        for (int v = v0; v < v0 + 4; ++v)
            for (int u = u0; u < u0 + 4; ++u)
                planet.set({face,u,v,r}, BlockType::Stone, true);

    const auto afterData = settle();
    const auto cached = buildPlanetSurfaceChunkMesh(*afterData);
    const auto snap = buildPlanetSurfaceChunkMesh(planet.snapshot(), addr);
    require(cached.macroQuads == snap.macroQuads && cached.microQuads == snap.microQuads,
            "cached vs snapshot greedy topology diverged");

    const int deltaMacro = cached.macroQuads - beforeCached.macroQuads;
    // Naive emission would be 4*4*6 = 96 face quads for an isolated cube; greedy
    // merges each face into one rectangle (P0-19). Allow a small margin if the
    // prism kisses procedural / uneven neighbors.
    require(deltaMacro <= 24,
            "spherical macro greedy did not merge uniform face runs enough");
    require(deltaMacro >= 6, "spherical greedy under-emitted prism faces");
    require(cached.vertexCount() == cached.quads * 6, "greedy mesh vertex/quads contract");

    // MicroBrick greedy: solid 4x4x4 micro cube inside one refined cell.
    const int microBefore = cached.microQuads;
    const SurfaceCellAddress microCell{face, u0 + 1, v0 + 1, base + 1};
    for (int mr = 0; mr < MicroBrick::Resolution; ++mr)
        for (int mv = 0; mv < MicroBrick::Resolution; ++mv)
            for (int mu = 0; mu < MicroBrick::Resolution; ++mu) {
                const bool inside = mu >= 4 && mu < 8 && mr >= 4 && mr < 8 && mv >= 4 && mv < 8;
                planet.setMicro(microCell, mu, mr, mv, inside ? BlockType::Stone : BlockType::Air);
            }

    const auto data2 = settle();
    const auto microMesh = buildPlanetSurfaceChunkMesh(*data2);
    const auto microSnap = buildPlanetSurfaceChunkMesh(planet.snapshot(), addr);
    require(microMesh.microQuads == microSnap.microQuads,
            "cached vs snapshot micro greedy topology diverged");
    // Refined interior cell emits greedied micro faces plus tiled seams from
    // neighboring macro cells (up to 6*16*16). Assert growth and that we stay
    // under naive core (96) + full seams (1536).
    const int deltaMicro = microMesh.microQuads - microBefore;
    require(deltaMicro >= 6,
            "refined MicroBrick emitted no additional micro geometry");
    require(deltaMicro <= 1536 + 24,
            "MicroBrick greedy did not reduce micro face count enough");
}

void testSphericalMeshWorkerDeterminismAfterGreedy() {
    // Re-lock 0/1/2/4/8 worker determinism after spherical greedy (P0-59).
    const auto h0 = captureSurfaceMeshHashSerial();
    const auto h1 = captureSurfaceMeshHashForWorkers(1);
    const auto h2 = captureSurfaceMeshHashForWorkers(2);
    const auto h4 = captureSurfaceMeshHashForWorkers(4);
    const auto h8 = captureSurfaceMeshHashForWorkers(8);
    require(h0 == h1 && h1 == h2 && h2 == h4 && h4 == h8,
            "spherical greedy meshing changed across 0/1/2/4/8 worker counts");
}

bool scalarPlanarMacroCandidate(const WorldSnapshot& world, int x, int y, int z,
                                int nx, int ny, int nz) {
    if (!world.inBounds(x, y, z) || world.isRefined(x, y, z)) return false;
    if (!blockProperties(world.get(x, y, z)).solid) return false;
    const int qx = x + nx, qy = y + ny, qz = z + nz;
    if (!world.inBounds(qx, qy, qz)) return true;
    if (world.isRefined(qx, qy, qz)) return false;
    if (blockProperties(world.get(qx, qy, qz)).solid) return false;
    return world.isExteriorAir(qx, qy, qz);
}

bool scalarSnapshotMacroCandidate(const PlanetSurfaceSnapshot& planet, SurfaceCellAddress a,
                                  int du, int dv, int dr) {
    if (planet.hasMicroDetail(a) || !blockProperties(planet.get(a)).solid) return false;
    SurfaceCellAddress n{a.face, a.u + du, a.v + dv, a.radial + dr};
    if (n.radial >= 0 && n.radial < PlanetSurfaceSnapshot::RadialLayers) n = planet.normalize(n);
    if (planet.radialInBounds(n.radial) && planet.hasMicroDetail(n)) return false;
    return !blockProperties(planet.get(n)).solid;
}

bool scalarCachedMacroCandidate(const SurfaceChunkData& chunk, int lu, int lv, int lr,
                                int du, int dv, int dr) {
    const auto a = chunk.worldAddress(lu, lv, lr);
    if (chunk.hasMicroDetail(a) || !blockProperties(chunk.getLocal(lu, lv, lr)).solid) return false;
    const auto neighbor = chunk.worldAddress(lu + du, lv + dv, lr + dr);
    if (chunk.hasMicroDetail(neighbor)) return false;
    return !blockProperties(chunk.getWithHalo(lu + du, lv + dv, lr + dr)).solid;
}

void requirePlanarColumnAgrees(const WorldSnapshot& world, int axis, int a, int b,
                               int origin, int count, int core0, int core1) {
    FaceCullWord solid = packPredBits(count, [&](int i) {
        int x=0,y=0,z=0;
        const int p = origin + i;
        if (axis == 0) { x = p; y = a; z = b; }
        else if (axis == 1) { x = a; y = p; z = b; }
        else { x = a; y = b; z = p; }
        return world.inBounds(x, y, z) && blockProperties(world.get(x, y, z)).solid;
    });
    FaceCullWord refined = packPredBits(count, [&](int i) {
        int x=0,y=0,z=0;
        const int p = origin + i;
        if (axis == 0) { x = p; y = a; z = b; }
        else if (axis == 1) { x = a; y = p; z = b; }
        else { x = a; y = b; z = p; }
        return world.inBounds(x, y, z) && world.isRefined(x, y, z);
    });
    FaceCullWord emitNeighbor = packPredBits(count, [&](int i) {
        int x=0,y=0,z=0;
        const int p = origin + i;
        if (axis == 0) { x = p; y = a; z = b; }
        else if (axis == 1) { x = a; y = p; z = b; }
        else { x = a; y = b; z = p; }
        return !world.inBounds(x, y, z) || world.isExteriorAir(x, y, z);
    });
    const FaceCullWord pos = cullPlanarPos(solid, refined, emitNeighbor);
    const FaceCullWord neg = cullPlanarNeg(solid, refined, emitNeighbor);
    for (int c = core0; c < core1; ++c) {
        const int bit = c - origin;
        int x=0,y=0,z=0;
        if (axis == 0) { x = c; y = a; z = b; }
        else if (axis == 1) { x = a; y = c; z = b; }
        else { x = a; y = b; z = c; }
        const int dpos[3] = {axis==0?1:0, axis==1?1:0, axis==2?1:0};
        const int dneg[3] = {axis==0?-1:0, axis==1?-1:0, axis==2?-1:0};
        require(faceCullTest(pos, bit) == scalarPlanarMacroCandidate(world, x, y, z, dpos[0], dpos[1], dpos[2]),
                "planar bitmask + cull disagreed with scalar solid-solid candidate");
        require(faceCullTest(neg, bit) == scalarPlanarMacroCandidate(world, x, y, z, dneg[0], dneg[1], dneg[2]),
                "planar bitmask - cull disagreed with scalar solid-solid candidate");
    }
}

void testBitmaskFaceCullAgreesWithSolidSolid() {
    // Synthetic words: isolated bits, runs, checkerboard.
    const FaceCullWord isolated = faceCullBit(3) | faceCullBit(8) | faceCullBit(40);
    const FaceCullWord run = (FaceCullWord{0xF} << 10) | (FaceCullWord{1} << 20);
    FaceCullWord checker = 0;
    for (int i = 0; i < 48; i += 2) checker |= faceCullBit(i);
    for (FaceCullWord solid : {isolated, run, checker, FaceCullWord{~FaceCullWord{0}}}) {
        for (int i = 1; i < 63; ++i) {
            require(faceCullTest(cullSolidSolidPos(solid), i)
                        == scalarSolidSolidExposed(faceCullTest(solid, i), faceCullTest(solid, i + 1)),
                    "bitmask +axis solid-solid cull drifted from scalar oracle");
            require(faceCullTest(cullSolidSolidNeg(solid), i)
                        == scalarSolidSolidExposed(faceCullTest(solid, i), faceCullTest(solid, i - 1)),
                    "bitmask -axis solid-solid cull drifted from scalar oracle");
        }
        const FaceCullWord refined = solid << 1;
        for (int i = 1; i < 63; ++i) {
            require(faceCullTest(cullMacroPos(solid, refined), i)
                        == scalarMacroExposed(faceCullTest(solid, i), faceCullTest(refined, i),
                                              faceCullTest(solid, i + 1), faceCullTest(refined, i + 1)),
                    "bitmask macro + cull drifted from scalar oracle");
            require(faceCullTest(cullMacroNeg(solid, refined), i)
                        == scalarMacroExposed(faceCullTest(solid, i), faceCullTest(refined, i),
                                              faceCullTest(solid, i - 1), faceCullTest(refined, i - 1)),
                    "bitmask macro - cull drifted from scalar oracle");
        }
    }

    // Planar fixtures: 2x2x2 cube, sealed cavity, world-edge column, refined neighbor.
    {
        WorldSnapshot cube = emptySnapshot();
        auto put = [&](int x, int y, int z, BlockType t) {
            cube.blocks[static_cast<std::size_t>(cube.flatIndex(x, y, z))] = t;
        };
        for (int y = 2; y < 4; ++y) for (int z = 2; z < 4; ++z) for (int x = 2; x < 4; ++x)
            put(x, y, z, BlockType::Stone);
        for (int y = 0; y < 4; ++y) put(0, y, 5, BlockType::Stone); // world -X edge
        cube.computeExteriorAir();
        for (int y = 2; y < 4; ++y) for (int z = 2; z < 4; ++z)
            requirePlanarColumnAgrees(cube, 0, y, z, 0 - kFaceCullHalo, 32 + kFaceCullHalo * 2, 0, 32);
        for (int z = 2; z < 4; ++z) for (int x = 0; x < 4; ++x)
            requirePlanarColumnAgrees(cube, 1, x, z, 0 - kFaceCullHalo, 32 + kFaceCullHalo * 2, 0, 32);
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
            requirePlanarColumnAgrees(cube, 2, x, y, 0 - kFaceCullHalo, 32 + kFaceCullHalo * 2, 0, 32);

        WorldSnapshot cavity = emptySnapshot();
        auto put2 = [&](int x, int y, int z, BlockType t) {
            cavity.blocks[static_cast<std::size_t>(cavity.flatIndex(x, y, z))] = t;
        };
        for (int y = 5; y < 8; ++y) for (int z = 5; z < 8; ++z) for (int x = 5; x < 8; ++x)
            put2(x, y, z, BlockType::Stone);
        put2(6, 6, 6, BlockType::Air);
        cavity.computeExteriorAir();
        requirePlanarColumnAgrees(cavity, 0, 6, 6, -kFaceCullHalo, 34, 0, 32);

        WorldSnapshot refined = emptySnapshot();
        refined.blocks[static_cast<std::size_t>(refined.flatIndex(4, 4, 4))] = BlockType::Stone;
        refined.blocks[static_cast<std::size_t>(refined.flatIndex(5, 4, 4))] = BlockType::Stone;
        MicroBrick brick(BlockType::Stone);
        brick.set(0, 0, 0, BlockType::Air);
        refined.microBricks.emplace(refined.flatIndex(5, 4, 4), brick);
        refined.computeExteriorAir();
        requirePlanarColumnAgrees(refined, 0, 4, 4, -kFaceCullHalo, 34, 0, 32);

        const CpuMeshData cavityMesh = buildChunkMesh(cavity, 0, 0, 0);
        require(cavityMesh.macroQuads == 6, "bitmask planar cull emitted sealed-cavity faces");
    }

    // Spherical: 4x4x4 prism + cached halo packet vs prior scalar candidate.
    {
        PlanetSurface planet(0xB17A5C01ULL, PlanetClass::Barren);
        const CubeFace face = CubeFace::PositiveZ;
        const int u0 = 8, v0 = 8;
        const int base = std::max({planet.surfaceRadial(face, u0, v0),
                                   planet.surfaceRadial(face, u0 + 3, v0),
                                   planet.surfaceRadial(face, u0, v0 + 3),
                                   planet.surfaceRadial(face, u0 + 3, v0 + 3)}) + 3;
        require(base + 3 < PlanetSurface::RadialLayers, "bitmask spherical fixture escaped radial bounds");
        for (int r = base; r < base + 4; ++r)
            for (int v = v0; v < v0 + 4; ++v)
                for (int u = u0; u < u0 + 4; ++u)
                    planet.set({face, u, v, r}, BlockType::Stone, true);

        const PlanetChunkAddress addr{face, u0 / PlanetSurface::ChunkSize, v0 / PlanetSurface::ChunkSize, 0};
        const auto snap = planet.snapshot();
        JobSystem jobs(SerialJobs);
        SurfaceChunkCache cache(jobs, 4, 8U * 1024U * 1024U, 4);
        cache.beginFrame();
        require(cache.request(planet, addr, SurfaceChunkPriority::EditRemesh), "bitmask cache request failed");
        settleSurfaceCache(cache, planet);
        const auto data = cache.find(addr);
        require(data != nullptr, "bitmask spherical chunk missing");

        const int cu0 = addr.u * PlanetSurface::ChunkSize;
        const int cv0 = addr.v * PlanetSurface::ChunkSize;
        constexpr int S = PlanetSurface::ChunkSize;

        for (int lv = 0; lv < S; ++lv) for (int lu = 0; lu < S; ++lu) {
            const FaceCullColumn rCol = [&] {
                FaceCullColumn col{};
                col.solid = packPredBits(S + 2, [&](int i) {
                    return blockProperties(data->getWithHalo(lu, lv, i - 1)).solid;
                });
                col.refined = packPredBits(S + 2, [&](int i) {
                    return data->hasMicroDetail(data->worldAddress(lu, lv, i - 1));
                });
                return col;
            }();
            const FaceCullWord rPos = cullMacroPos(rCol.solid, rCol.refined);
            const FaceCullWord rNeg = cullMacroNeg(rCol.solid, rCol.refined);
            for (int lr = 0; lr < S; ++lr) {
                const int bit = faceCullBitIndex(lr);
                require(faceCullTest(rPos, bit) == scalarCachedMacroCandidate(*data, lu, lv, lr, 0, 0, 1),
                        "cached bitmask R+ disagreed with scalar halo cull");
                require(faceCullTest(rNeg, bit) == scalarCachedMacroCandidate(*data, lu, lv, lr, 0, 0, -1),
                        "cached bitmask R- disagreed with scalar halo cull");

                const SurfaceCellAddress a{face, cu0 + lu, cv0 + lv, lr};
                require(scalarSnapshotMacroCandidate(snap, a, 0, 0, 1)
                            == scalarCachedMacroCandidate(*data, lu, lv, lr, 0, 0, 1),
                        "snapshot vs cached scalar R+ halo contract drifted");
            }
        }

        // Sample U/V axes on the prism cells (includes interior solid-solid culls).
        for (int r = base; r < base + 4; ++r) for (int v = v0; v < v0 + 4; ++v) {
            FaceCullColumn col{};
            col.solid = packPredBits(S + 2, [&](int i) {
                return blockProperties(snap.get({face, cu0 - 1 + i, v, r})).solid;
            });
            col.refined = packPredBits(S + 2, [&](int i) {
                const SurfaceCellAddress a{face, cu0 - 1 + i, v, r};
                return snap.radialInBounds(a.radial) && snap.hasMicroDetail(a);
            });
            const FaceCullWord uPos = cullMacroPos(col.solid, col.refined);
            const FaceCullWord uNeg = cullMacroNeg(col.solid, col.refined);
            for (int u = u0; u < u0 + 4; ++u) {
                const int bit = faceCullBitIndex(u - cu0);
                const SurfaceCellAddress a{face, u, v, r};
                require(faceCullTest(uPos, bit) == scalarSnapshotMacroCandidate(snap, a, 1, 0, 0),
                        "snapshot bitmask U+ disagreed with scalar solid-solid cull");
                require(faceCullTest(uNeg, bit) == scalarSnapshotMacroCandidate(snap, a, -1, 0, 0),
                        "snapshot bitmask U- disagreed with scalar solid-solid cull");
            }
        }

        const auto cachedMesh = buildPlanetSurfaceChunkMesh(*data);
        const auto snapMesh = buildPlanetSurfaceChunkMesh(snap, addr);
        require(cachedMesh.macroQuads == snapMesh.macroQuads && cachedMesh.microQuads == snapMesh.microQuads,
                "bitmask mesher diverged cached vs snapshot topology");
    }
}

} // namespace


void testProceduralSubvoxelShaping() {
    constexpr std::uint64_t seed = 0xC0FFEE42ULL;
    PlanetSurface a(seed, PlanetClass::Temperate);
    PlanetSurface b(seed, PlanetClass::Temperate);

    require(a.journalCount() == 0, "virgin temperate planet must start with zero journals");
    require(a.microOverrideCount() == 0, "virgin planet must store zero micro bricks");

    // Find a steep surface column that activates procedural micro.
    SurfaceCellAddress detail{};
    bool found = false;
    for (int f = 0; f < PlanetSurface::FaceCount && !found; ++f) {
        const auto face = static_cast<CubeFace>(f);
        for (int v = 0; v < PlanetSurface::FaceResolution && !found; ++v) {
            for (int u = 0; u < PlanetSurface::FaceResolution && !found; ++u) {
                const int surf = a.surfaceRadial(face, u, v);
                // surfaceRadial returns top solid; generated column may differ if
                // edits exist — virgin uses generated height via hasMicroDetail.
                SurfaceCellAddress cell{face, u, v, surf};
                if (a.hasMicroDetail(cell)) {
                    detail = cell;
                    found = true;
                }
            }
        }
    }
    require(found, "expected at least one Temperate cell with procedural micro detail");
    require(!a.isRefined(detail), "procedural detail must not refine/journal the cell");
    require(a.journalCount() == 0, "procedural micro must keep virgin journals empty");

    // Determinism: identical seed → identical micro occupancy.
    int differ = 0;
    int carved = 0;
    const BlockType macro = a.get(detail);
    for (int mr = 0; mr < MicroBrick::Resolution; ++mr)
        for (int mv = 0; mv < MicroBrick::Resolution; ++mv)
            for (int mu = 0; mu < MicroBrick::Resolution; ++mu) {
                const BlockType ta = a.microGet(detail, mu, mr, mv);
                const BlockType tb = b.microGet(detail, mu, mr, mv);
                if (ta != tb) ++differ;
                if (ta != macro) ++carved;
            }
    require(differ == 0, "procedural micro not deterministic across planets with same seed");
    require(carved > 0, "active procedural cell should differ from pure macro somewhere");

    // Player setMicro overrides procedural and materializes a journal brick.
    BlockType proceduralValue = BlockType::Air;
    int pmu = 0, pmr = 0, pmv = 0;
    bool got = false;
    for (int mr = 0; mr < MicroBrick::Resolution && !got; ++mr)
        for (int mv = 0; mv < MicroBrick::Resolution && !got; ++mv)
            for (int mu = 0; mu < MicroBrick::Resolution && !got; ++mu) {
                const BlockType t = a.microGet(detail, mu, mr, mv);
                if (t != BlockType::Stone) {
                    proceduralValue = t;
                    pmu = mu; pmr = mr; pmv = mv;
                    got = true;
                }
            }
    require(got, "could not find a non-stone microcell to override");
    a.setMicro(detail, pmu, pmr, pmv, BlockType::Stone);
    require(a.isRefined(detail), "setMicro should refine the cell");
    require(a.microGet(detail, pmu, pmr, pmv) == BlockType::Stone,
            "player setMicro must win over procedural");
    require(a.journalCount() >= 1, "player micro edit must create a journal");

    // Non-overridden micros keep regenerable procedural underlay (zero storage).
    int stillProcedural = 0;
    for (int mr = 0; mr < MicroBrick::Resolution; ++mr)
        for (int mv = 0; mv < MicroBrick::Resolution; ++mv)
            for (int mu = 0; mu < MicroBrick::Resolution; ++mu) {
                if (mu == pmu && mr == pmr && mv == pmv) continue;
                if (a.microGet(detail, mu, mr, mv) != macro) ++stillProcedural;
            }
    require(stillProcedural > 0,
            "player override must not erase procedural underlay on sibling micros");
    require(a.microOverrideCount() == 1, "only the player delta should be journaled");

    // Mesher sees procedural detail on a virgin snapshot (no journals).
    PlanetSurface virgin(seed, PlanetClass::Temperate);
    const auto snap = virgin.snapshot();
    require(snap.microBricks.empty(), "virgin snapshot must carry zero stored micro bricks");
    require(snap.hasMicroDetail(detail), "snapshot must report procedural micro detail");
    const auto chunk = virgin.chunkOf(detail);
    const auto mesh = buildPlanetSurfaceChunkMesh(snap, chunk);
    require(!mesh.vertices.empty(), "procedural micro chunk mesh should emit geometry");
    require(mesh.microQuads > 0,
            "virgin procedural micro must emit microQuads (mesher hasMicroDetail path)");

    // Light hooks: Barren/Scorched only activate on steeper crust.
    PlanetSurface barren(seed, PlanetClass::Barren);
    int barrenActive = 0;
    for (int v = 0; v < PlanetSurface::FaceResolution; v += 4)
        for (int u = 0; u < PlanetSurface::FaceResolution; u += 4) {
            const int surf = barren.surfaceRadial(CubeFace::PositiveZ, u, v);
            if (barren.hasMicroDetail({CubeFace::PositiveZ, u, v, surf})) ++barrenActive;
        }
    require(barren.journalCount() == 0, "barren procedural micro must not journal");
    (void)barrenActive; // may be zero on flat seeds; hook exists via hasMicroDetail path
}


int main() {
    try {
        testPlanetSurfaceTerrainGenV2();
        testProceduralSubvoxelShaping();
        testCubeSphereRoundTrip();
        testPlanetSurfaceTopologyAndFrames();
        testSurfaceChunkJournalSharding();
        testPlanetSurfaceMicroVolumeAndPersistence();
        testSurfaceInfrastructureAndAtmosphere();
        testSurfaceNavigationAcrossSeamsAndCache();
        testSurfaceDoorAirlockSealAndPersistence();
        testPoweredAirlockInterlockAndPersistence();
        testMicroBrickStorage();
        testWorldDeterminismAndMicroReplay();
        testGreedyMeshingAndExteriorAir();
        testMicroMeshing();
        testJobSystem();
        testWorkerCountMeshDeterminism();
        testEditInfluenceSummary();
        testSurfaceWorldReadUsesCacheAndFallback();
        testSurfaceChunkCacheBudgetCancellationAndRebuild();
        testSurfaceChunkCacheRetainAndPriorityPreempt();
        testPlanetSurfaceMeshingAndRenderer();
        testChunkDirtyTrackingAndRendererIsolation();
        testAoAndMaterialRanges();
        testBasePowerAndSealedAtmosphere();
        testCachedSphericalMeshingUsesHalo();
        testSurfaceChunkTransactionalStore();
        testSurfaceDefenseAutomationAndPersistence();
        testAddressKeyedInfrastructureJournalRecords();
        testInfrastructureJournalCrossShardClosureAndCompaction();
        testSurfaceIndustryProcessingAndPersistence();
        testSurfaceExplicitLogisticsAndPersistence();
        testSurfaceAdvancedIndustryExtractorAndDefenseSupply();
        testSurfaceRegisterActionDirector();
        testWorldBiomeCatalogAndPlanetClassCoverage();
        testShipTransitRepairGateAndInterPlanetPhases();
        testPlanetEnvironmentOxygenDrainAndSuspicionSeams();
        testThreePlanetScenarioOpenExteriorPressure();
        testCampaignSiegeSaveEmbedRoundTrip();
        testSurvivalPressureHungerEnergyDecisions();
        testSuspicionLedgerPerSystemIsolationAndClaimPushAnnouncement();
        testRegistryBeaconClaimDistinctIntactAndStableId();
        testEarlyMetallurgyObtainabilityLadder();
        testChunkVoxelSpansAdaptiveIdentityAndNoiseBasisCache();
        testChunkOccupancyExtremityAndAdaptiveRep();
        testSphericalMacroAndMicroGreedyMeshing();
        testSphericalMeshWorkerDeterminismAfterGreedy();
        testBitmaskFaceCullAgreesWithSolidSolid();
        std::cout << "Elysium headless tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Elysium headless tests: FAIL: " << e.what() << "\n";
        return 1;
    }
}
