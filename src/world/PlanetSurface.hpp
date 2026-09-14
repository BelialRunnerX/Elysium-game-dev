#pragma once

#include "core/Math.hpp"
#include "world/Block.hpp"
#include "world/CubeSphere.hpp"
#include "world/MicroBrick.hpp"
#include "world/PlanetEnvironment.hpp"
#include "world/PlanetTypes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace elysium {

// Authoritative storage address for the cube-sphere surface substrate. Face/U/V
// choose a surface column and radial chooses the one-metre shell layer.
struct SurfaceCellAddress {
    CubeFace face{CubeFace::PositiveZ};
    int u{};
    int v{};
    int radial{};

    bool operator==(const SurfaceCellAddress&) const = default;
};

// 1/16 m address inside one authoritative macro cell. Micro axes are U,
// radial/up, V respectively. The parent SurfaceCellAddress remains the stable
// save identity; this struct is transient query state.
struct SurfaceMicroAddress {
    SurfaceCellAddress cell{};
    int u{};
    int radial{};
    int v{};

    bool operator==(const SurfaceMicroAddress&) const = default;
};

struct SurfaceFrame {
    Vec3 position{};
    Vec3 up{};
    Vec3 right{};
    Vec3 forward{};
};

struct SurfaceRayHit {
    bool hit{};
    SurfaceCellAddress cell{};
    SurfaceCellAddress previous{};
    Vec3 point{};
    float distance{};
};

struct SurfaceMicroRayHit {
    bool hit{};
    SurfaceMicroAddress micro{};
    Vec3 point{};
    float distance{};
    BlockType type{BlockType::Air};
};

struct SurfaceSealedVolumeQuery {
    bool sealed{};
    bool truncated{};
    std::vector<int> cells;
};


struct EditInfluenceSummary {
    bool any{};
    int minU{};
    int minV{};
    int minRadial{};
    int maxU{};
    int maxV{};
    int maxRadial{};
    int addedSolidCells{};
    int removedSolidCells{};
    int refinedCells{};
    int microOverrides{};
    int maxOutwardDelta{};
    int maxInwardDepth{};

    bool intersects(int uBegin,int vBegin,int uEnd,int vEnd) const {
        return any && maxU>=uBegin && minU<uEnd && maxV>=vBegin && minV<vEnd;
    }
    int significanceScore() const {
        return addedSolidCells*3 + removedSolidCells*2 + refinedCells +
               std::min(64,microOverrides/8) + maxOutwardDelta*6 + maxInwardDepth*3;
    }
};

struct SurfaceChunkJournal {
    PlanetChunkAddress address{};
    std::unordered_map<int, BlockType> macroEdits;
    std::unordered_set<int> placedMarkers;
    std::unordered_map<int, MicroBrick> microBricks;
    EditInfluenceSummary influence{};

    bool empty() const { return macroEdits.empty() && placedMarkers.empty() && microBricks.empty(); }
    std::size_t persistentCellStateCount() const { return macroEdits.size() + microBricks.size(); }
    std::size_t microOverrideCount() const;
};

struct PlanetSurfaceSnapshot {
    static constexpr int FaceResolution = 64;
    static constexpr int RadialLayers = 32;
    static constexpr int ReferenceRadial = 16;
    static constexpr int FaceCount = 6;
    static constexpr int TotalCells = FaceCount * FaceResolution * FaceResolution * RadialLayers;

    // v0.10 snapshots are procedural read views, not whole-planet voxel copies.
    // Baseline cells are regenerated on demand from seed + generator contract;
    // only player deltas and refined cells are copied into worker-safe state.
    std::uint64_t seed{};
    PlanetClass planetClass{PlanetClass::Temperate};
    float referenceRadius{48.0f};
    std::array<std::uint8_t, FaceCount * FaceResolution * FaceResolution> generatedSurfaceRadials{};
    std::unordered_map<int, BlockType> edits;
    std::unordered_map<int, MicroBrick> microBricks;
    std::unordered_map<std::uint64_t, EditInfluenceSummary> editInfluenceSummaries;

    int flatIndex(CubeFace face, int u, int v, int radial) const;
    bool radialInBounds(int radial) const;
    SurfaceCellAddress normalize(SurfaceCellAddress address) const;
    BlockType get(CubeFace face, int u, int v, int radial) const;
    BlockType get(SurfaceCellAddress address) const;
    BlockType baseline(SurfaceCellAddress address) const;
    bool isRefined(SurfaceCellAddress address) const;
    bool hasMicroDetail(SurfaceCellAddress address) const;
    const MicroBrick* microBrick(SurfaceCellAddress address) const;
    BlockType microGet(SurfaceCellAddress address, int mu, int mr, int mv) const;
    BlockType microGet(const SurfaceMicroAddress& address) const;
    // Fill 16^3 micros for one macro cell (hoists column/slope once).
    void sampleMicroBrick(SurfaceCellAddress address,
                          std::array<BlockType, MicroBrick::CellCount>& out) const;
    int surfaceRadial(CubeFace face, int u, int v) const;
    bool hasEditInfluence(CubeFace face, int uBegin, int vBegin, int uEnd, int vEnd) const;
    EditInfluenceSummary editInfluence(const PlanetChunkAddress& address) const;
    Vec3 boundaryPosition(CubeFace face, int uEdge, int vEdge, int radialBoundary) const;
    Vec3 cellCenterPosition(SurfaceCellAddress address) const;
    Vec3 microBoundaryPosition(SurfaceCellAddress address, int uEdge, int radialEdge, int vEdge) const;
    Vec3 microCellCenterPosition(const SurfaceMicroAddress& address) const;
    SurfaceMicroAddress locateMicro(Vec3 planetLocalPosition) const;
    bool solidAt(Vec3 planetLocalPosition) const;
};

// Six-face deterministic voxel substrate. v0.6 makes it the authoritative
// near-player macro + micro world for spherical play. Legacy World remains as a
// compatibility/testing substrate while systems are migrated incrementally.
class PlanetSurface {
public:
    static constexpr int FaceResolution = PlanetSurfaceSnapshot::FaceResolution;
    static constexpr int RadialLayers = PlanetSurfaceSnapshot::RadialLayers;
    static constexpr int ReferenceRadial = PlanetSurfaceSnapshot::ReferenceRadial;
    static constexpr int ChunkSize = 32;
    static constexpr int ChunksPerFaceAxis = FaceResolution / ChunkSize;
    static constexpr int RadialChunks = RadialLayers / ChunkSize;
    static constexpr int FaceCount = 6;
    static constexpr int TotalCells = FaceCount * FaceResolution * FaceResolution * RadialLayers;
    static constexpr int ChunkCount = FaceCount * ChunksPerFaceAxis * ChunksPerFaceAxis * RadialChunks;
    // Save compatibility data. Any change to deterministic spherical baseline
    // generation must intentionally advance these rather than silently applying
    // sparse player deltas to a different world.
    static constexpr int GeneratorVersion = 2;
    static constexpr std::uint64_t GeneratorFingerprint = 0x454C595350483032ULL; // ELYSPH02

    PlanetSurface(std::uint64_t seed, PlanetClass planetClass, float referenceRadius = 48.0f);

    std::uint64_t seed() const { return seed_; }
    PlanetClass planetClass() const { return class_; }
    const PlanetEnvironment& environment() const { return planetEnvironmentFor(class_); }
    float referenceRadius() const { return referenceRadius_; }
    std::uint64_t revision() const { return revision_; }

    SurfaceCellAddress normalize(SurfaceCellAddress address) const;
    bool radialInBounds(int radial) const;
    BlockType get(SurfaceCellAddress address) const;
    BlockType get(CubeFace face, int u, int v, int radial) const;
    BlockType baseline(SurfaceCellAddress address) const;
    void set(SurfaceCellAddress address, BlockType type, bool placedByPlayer = false);

    bool isRefined(SurfaceCellAddress address) const;
    // Player brick OR regenerable procedural micro detail (mesher/collision gate).
    bool hasMicroDetail(SurfaceCellAddress address) const;
    const MicroBrick* microBrick(SurfaceCellAddress address) const;
    BlockType microGet(SurfaceCellAddress address, int mu, int mr, int mv) const;
    BlockType microGet(const SurfaceMicroAddress& address) const;
    void setMicro(SurfaceCellAddress address, int mu, int mr, int mv, BlockType type);
    void setMicroIndex(SurfaceCellAddress address, int microIndex, BlockType type);
    void applySavedMicroEdit(int flatCellIndex, int microIndex, BlockType type);
    std::size_t microOverrideCount() const;
    std::size_t macroEditCount() const;
    std::size_t placedMarkerCount() const;
    std::size_t journalCount() const { return journals_.size(); }
    const std::unordered_map<std::uint64_t, SurfaceChunkJournal>& journals() const { return journals_; }
    const SurfaceChunkJournal* journal(const PlanetChunkAddress& address) const;

    int surfaceRadial(CubeFace face, int u, int v) const;
    Vec3 boundaryPosition(CubeFace face, int uEdge, int vEdge, int radialBoundary) const;
    Vec3 cellCenterPosition(SurfaceCellAddress address) const;
    Vec3 microBoundaryPosition(SurfaceCellAddress address, int uEdge, int radialEdge, int vEdge) const;
    Vec3 microCellCenterPosition(const SurfaceMicroAddress& address) const;
    SurfaceCellAddress locate(Vec3 planetLocalPosition) const;
    SurfaceCellAddress locateUnclamped(Vec3 planetLocalPosition) const;
    SurfaceMicroAddress locateMicro(Vec3 planetLocalPosition) const;
    SurfaceRayHit raycast(Vec3 origin, Vec3 direction, float maxDistance, float step = 0.10f) const;
    SurfaceMicroRayHit raycastMicro(Vec3 origin, Vec3 direction, float maxDistance, float step = 0.018f) const;
    bool solidAt(Vec3 planetLocalPosition) const;
    bool capsuleCollides(Vec3 feet, float radius = 0.32f, float height = 1.75f) const;
    bool groundedAt(Vec3 feet, float probe = 0.09f) const;
    float surfaceBoundaryRadius(Vec3 direction) const;
    SurfaceFrame surfaceFrame(Vec3 planetLocalPosition) const;
    Vec3 gravityDirectionAt(Vec3 planetLocalPosition) const;
    Vec3 cameraRelative(Vec3 planetLocalPosition, Vec3 cameraPlanetLocal) const;
    SurfaceSealedVolumeQuery sealedVolume(SurfaceCellAddress start, int maxCells = 8192) const;

    PlanetChunkAddress chunkOf(SurfaceCellAddress address) const;
    std::uint64_t chunkRevision(const PlanetChunkAddress& address) const;
    std::size_t persistentCellStateCount() const;
    std::size_t materializedBaselineCellCount() const { return 0; }
    int chunkEditCount(const PlanetChunkAddress& address) const;
    bool chunkHasEditInfluence(const PlanetChunkAddress& address) const { return chunkEditCount(address) > 0; }
    EditInfluenceSummary editInfluenceSummary(const PlanetChunkAddress& address) const;
    bool playerPlaced(SurfaceCellAddress address) const;
    int flatIndex(SurfaceCellAddress address) const;
    SurfaceCellAddress cellFromFlatIndex(int index) const;
    void applySavedEdit(int flatIndex, BlockType type);
    void applySavedPlacedMarker(int flatIndex);

    PlanetSurfaceSnapshot snapshot() const;

private:
    std::uint64_t seed_{};
    PlanetClass class_{};
    float referenceRadius_{48.0f};
    // One byte per surface column is the only resident deterministic baseline
    // cache. The 32 radial voxel layers are regenerated on demand.
    std::array<std::uint8_t, FaceCount * FaceResolution * FaceResolution> generatedSurfaceRadials_{};
    // Procedural baseline is never materialized as a full planet array. Player
    // state is sharded by stable PlanetChunkAddress so touched chunks can be
    // journaled, loaded and compacted independently.
    std::unordered_map<std::uint64_t, SurfaceChunkJournal> journals_;
    std::array<std::uint64_t, ChunkCount> chunkRevisions_{};
    std::uint64_t revision_{1};

    SurfaceChunkJournal* findJournal(const PlanetChunkAddress& address);
    const SurfaceChunkJournal* findJournal(const PlanetChunkAddress& address) const;
    SurfaceChunkJournal& ensureJournal(const PlanetChunkAddress& address);
    SurfaceChunkJournal& ensureJournal(SurfaceCellAddress address);
    void compactJournal(const PlanetChunkAddress& address);
    void rebuildJournalInfluence(const PlanetChunkAddress& address);

    int generatedSurfaceRadial(CubeFace face, int u, int v) const;
    int columnSlope(CubeFace face, int u, int v) const;
    BlockType generatedBlock(CubeFace face, int u, int v, int radial, int surface) const;
    BlockType resolveMicro(SurfaceCellAddress address, int mu, int mr, int mv,
                           const MicroBrick* playerBrick) const;
    int chunkSlot(const PlanetChunkAddress& address) const;
    void markDirty(SurfaceCellAddress address);
    MicroBrick& refineCell(SurfaceCellAddress address);
    bool gasPassable(SurfaceCellAddress address) const;
};

} // namespace elysium
