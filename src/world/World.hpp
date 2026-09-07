#pragma once

#include "core/Math.hpp"
#include "world/Block.hpp"
#include "world/MicroBrick.hpp"
#include "world/PlanetEnvironment.hpp"
#include "world/PlanetTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace elysium {

struct VoxelHit {
    IVec3 cell{};
    IVec3 normal{};
    float distance{};
    BlockType type{BlockType::Air};
};

struct MicroVoxelHit {
    IVec3 cell{};
    IVec3 micro{};
    IVec3 globalMicro{};
    IVec3 normal{};
    float distance{};
    BlockType type{BlockType::Air};
};

struct WorldEdit {
    IVec3 cell{};
    BlockType type{BlockType::Air};
};

struct SealedVolumeQuery {
    bool sealed{};
    bool truncated{};
    std::vector<int> cells;
};

// Immutable CPU-side view used by worker meshing. Graphics code never touches
// the authoritative World directly off-thread.
struct WorldSnapshot {
    static constexpr int Width = 64;
    static constexpr int Height = 32;
    static constexpr int Depth = 64;
    static constexpr int ChunkSize = 32;

    std::uint64_t revision{};
    std::vector<BlockType> blocks;
    std::unordered_map<int, MicroBrick> microBricks;
    std::vector<std::uint8_t> exteriorAir;

    bool inBounds(int x, int y, int z) const;
    int flatIndex(int x, int y, int z) const;
    BlockType get(int x, int y, int z) const;
    bool isRefined(int x, int y, int z) const;
    const MicroBrick* microBrick(int x, int y, int z) const;
    BlockType microGet(int x, int y, int z, int mx, int my, int mz) const;
    bool isExteriorAir(int x, int y, int z) const;
    void computeExteriorAir();
};

class World {
public:
    static constexpr int Width = 64;
    static constexpr int Height = 32;
    static constexpr int Depth = 64;
    static constexpr int ChunkSize = 32;
    static constexpr int ChunkCountX = (Width + ChunkSize - 1) / ChunkSize;
    static constexpr int ChunkCountY = (Height + ChunkSize - 1) / ChunkSize;
    static constexpr int ChunkCountZ = (Depth + ChunkSize - 1) / ChunkSize;
    static constexpr int ChunkCount = ChunkCountX * ChunkCountY * ChunkCountZ;

    World(std::uint64_t seed, PlanetClass planetClass);

    std::uint64_t seed() const { return seed_; }
    PlanetClass planetClass() const { return class_; }
    const PlanetEnvironment& environment() const;

    bool inBounds(int x, int y, int z) const;
    bool isSolid(int x, int y, int z) const;
    BlockType get(int x, int y, int z) const;
    BlockType baseline(int x, int y, int z) const;
    void set(int x, int y, int z, BlockType type);
    int surfaceY(int x, int z) const;

    std::optional<VoxelHit> raycast(Vec3 origin, Vec3 direction, float maxDistance) const;
    std::optional<MicroVoxelHit> raycastMicro(Vec3 origin, Vec3 direction, float maxDistance) const;
    float localHazardAt(Vec3 pos) const;
    SealedVolumeQuery sealedVolume(IVec3 start, int maxCells = 8192) const;

    bool isRefined(int x, int y, int z) const;
    const MicroBrick* microBrick(int x, int y, int z) const;
    BlockType microGet(int x, int y, int z, int mx, int my, int mz) const;
    void setMicro(int x, int y, int z, int mx, int my, int mz, BlockType type);
    void setMicroGlobal(int gx, int gy, int gz, BlockType type);
    void applySavedMicroEdit(int flatCellIndex, int microIndex, BlockType type);
    const std::unordered_map<int, MicroBrick>& microBricks() const { return microBricks_; }
    std::size_t microOverrideCount() const;

    const std::unordered_map<int, BlockType>& edits() const { return edits_; }
    void applySavedEdit(int flatIndex, BlockType type);
    IVec3 cellFromFlatIndex(int index) const;
    int flatIndex(int x, int y, int z) const;

    WorldSnapshot snapshot() const;
    std::uint64_t revision() const { return revision_; }
    std::uint64_t chunkRevision(int chunkX, int chunkY, int chunkZ) const;
    int dirtyChunkCountSince(std::uint64_t revision) const;

private:
    std::uint64_t seed_{};
    PlanetClass class_{};
    std::vector<BlockType> baseline_;
    std::vector<BlockType> blocks_;
    std::unordered_map<int, BlockType> edits_;
    std::unordered_map<int, MicroBrick> microBricks_;
    std::vector<std::uint8_t> exteriorAirCache_;
    std::array<std::uint64_t, ChunkCount> chunkRevisions_{};
    std::uint64_t revision_{1};

    void generate();
    int generatedHeight(int x, int z) const;
    MicroBrick& refineCell(int x, int y, int z);

    static int chunkIndex(int chunkX, int chunkY, int chunkZ);
    void beginEdit();
    void markChunkDirty(int chunkX, int chunkY, int chunkZ);
    void markCellAndSeamNeighborsDirty(int x, int y, int z);
    std::vector<std::uint8_t> computeExteriorAirMask() const;
    void refreshExteriorConnectivityAndDirty();
};

} // namespace elysium
