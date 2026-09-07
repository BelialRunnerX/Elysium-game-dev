#pragma once

#include "core/JobSystem.hpp"
#include "render/GraphicsBackend.hpp"
#include "world/VoxelMesher.hpp"
#include "world/World.hpp"

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <vector>

namespace elysium {

class WorldRenderer {
public:
    WorldRenderer(JobSystem& jobs, IGraphicsBackend& graphics);
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    // Worker threads create CPU mesh packets. sync() is called on the graphics
    // owner thread and is the only place backend resources are uploaded/replaced.
    // v0.3 tracks target revisions per chunk so an edit normally rebuilds only
    // its chunk plus seam/exterior-connectivity dependants.
    void sync(const World& world);
    void draw() const;
    void invalidate();

    int visibleFaces() const { return meshQuads_; } // compatibility name for HUD callers
    int meshQuads() const { return meshQuads_; }
    int macroQuads() const { return macroQuads_; }
    int microQuads() const { return microQuads_; }
    int triangleCount() const { return triangleCount_; }
    int pendingJobs() const;
    int dirtyChunks() const;
    int rebuiltChunksLastSync() const { return rebuiltChunksLastSync_; }

private:
    struct ChunkModel {
        GraphicsMeshHandle handle{};
        std::uint64_t builtRevision{};
        int quads{};
        int macroQuads{};
        int microQuads{};
        int triangles{};
    };

    struct ChunkBuildResult {
        int slot{};
        std::uint64_t chunkRevision{};
        CpuMeshData mesh;
    };

    JobSystem& jobs_;
    IGraphicsBackend& graphics_;
    std::vector<ChunkModel> chunks_;
    std::vector<std::optional<std::future<ChunkBuildResult>>> pending_;
    std::shared_ptr<const WorldSnapshot> latestSnapshot_;
    std::array<std::uint64_t, World::ChunkCount> targetChunkRevisions_{};
    std::uint64_t snapshotRevision_{};
    int meshQuads_{};
    int macroQuads_{};
    int microQuads_{};
    int triangleCount_{};
    int rebuiltChunksLastSync_{};

    static constexpr int ChunkCountX = World::ChunkCountX;
    static constexpr int ChunkCountY = World::ChunkCountY;
    static constexpr int ChunkCountZ = World::ChunkCountZ;
    static constexpr int ChunkCount = World::ChunkCount;

    static int slotIndex(int cx, int cy, int cz);
    static void slotCoords(int slot, int& cx, int& cy, int& cz);
    void updateTargets(const World& world);
    void processCompleted();
    void scheduleMissing();
    void publish(int slot, CpuMeshData&& mesh, std::uint64_t chunkRevision);
    void recalcStats();
};

} // namespace elysium
