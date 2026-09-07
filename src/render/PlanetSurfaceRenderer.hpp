#pragma once

#include "core/JobSystem.hpp"
#include "render/GraphicsBackend.hpp"
#include "world/PlanetSurface.hpp"
#include "world/PlanetSurfaceMesher.hpp"
#include "world/SurfaceChunkCache.hpp"

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <vector>

namespace elysium {

// Runtime representation tier for one cube-sphere surface chunk.
// Field is a cheap direct-field silhouette packet; Full is the editable
// macro/micro voxel packet. Only a bounded set of chunks around the player need
// Full residency during surface play.
enum class SurfaceRenderDetail : std::uint8_t { FieldFar = 0, FieldNear = 1, Full = 2 };

// Chunked cube-sphere renderer with bounded high-detail residency. Workers build
// CPU packets while all graphics publication stays on the owner thread. A local
// focus promotes the nearest N chunks to LOD0 and demotes the rest to field LOD;
// orbital/debug views can request all chunks at full detail.
class PlanetSurfaceRenderer {
public:
    PlanetSurfaceRenderer(JobSystem& jobs, IGraphicsBackend& graphics);
    ~PlanetSurfaceRenderer();
    PlanetSurfaceRenderer(const PlanetSurfaceRenderer&) = delete;
    PlanetSurfaceRenderer& operator=(const PlanetSurfaceRenderer&) = delete;

    void sync(const PlanetSurface& planet);
    void draw() const;
    void drawOrbitalShell() const;
    void invalidate();

    // Promote only the nearest highDetailBudget chunks to full editable detail.
    // The remaining chunks retain cheap field proxies so planet silhouette and
    // large player-authored height changes survive outside the residency bubble.
    void setStreamingFocus(Vec3 planetLocalPosition, int highDetailBudget = 7, int nearFieldBudget = 9);
    void setFullDetail();
    void setOrbitalShellOnly();

    bool ready() const;
    bool pending() const { return pendingJobs() > 0; }
    int pendingJobs() const;
    int dirtyChunks() const;
    int rebuiltChunksLastSync() const { return rebuiltChunksLastSync_; }
    int quads() const { return quads_; }
    int triangles() const { return triangles_; }
    int fullDetailChunks() const;
    int fieldDetailChunks() const;
    int nearFieldChunks() const;
    int farFieldChunks() const;
    int targetFullDetailChunks() const;
    int targetNearFieldChunks() const;
    bool streaming() const { return streaming_; }
    bool orbitalShellOnly() const { return orbitalShellOnly_; }
    int orbitalClimateQuads() const { return orbitalClimateQuads_; }
    int orbitalCloudQuads() const { return orbitalCloudQuads_; }
    SurfaceChunkCacheStats cpuCacheStats() const { return chunkCache_.stats(); }
    const SurfaceChunkCache& cpuChunkCache() const { return chunkCache_; }

private:
    struct ChunkModel {
        GraphicsMeshHandle handle{};
        std::uint64_t builtRevision{};
        SurfaceRenderDetail detail{SurfaceRenderDetail::FieldFar};
        bool initialized{};
        int quads{};
        int triangles{};
    };

    struct BuildResult {
        int slot{};
        std::uint64_t revision{};
        SurfaceRenderDetail detail{SurfaceRenderDetail::FieldFar};
        CpuMeshData mesh;
    };

    JobSystem& jobs_;
    IGraphicsBackend& graphics_;
    SurfaceChunkCache chunkCache_;
    std::vector<ChunkModel> chunks_;
    std::vector<std::optional<std::future<BuildResult>>> pending_;
    std::shared_ptr<const PlanetSurfaceSnapshot> latestSnapshot_;
    std::array<std::uint64_t, PlanetSurface::ChunkCount> targetRevisions_{};
    std::array<SurfaceRenderDetail, PlanetSurface::ChunkCount> targetDetails_{};
    std::uint64_t snapshotRevision_{};
    bool streaming_{};
    Vec3 focusDirection_{0,0,1};
    int highDetailBudget_{7};
    int nearFieldBudget_{9};
    std::array<int, PlanetSurface::ChunkCount> editInfluenceCounts_{};
    bool detailTargetsDirty_{true};
    int quads_{};
    int triangles_{};
    int rebuiltChunksLastSync_{};
    bool orbitalShellOnly_{};
    GraphicsMeshHandle orbitalClimateHandle_{};
    GraphicsMeshHandle orbitalCloudHandle_{};
    std::uint64_t orbitalSeed_{};
    PlanetClass orbitalClass_{PlanetClass::Temperate};
    float orbitalRadius_{};
    int orbitalClimateQuads_{};
    int orbitalCloudQuads_{};

    static int slotIndex(int face, int cu, int cv, int cr = 0);
    static PlanetChunkAddress slotAddress(int slot);
    static Vec3 chunkCenterDirection(const PlanetChunkAddress& address);
    void updateTargets(const PlanetSurface& planet);
    void updateDetailTargets();
    void processCompleted();
    void scheduleMissing();
    void publish(int slot, CpuMeshData&& mesh, std::uint64_t revision, SurfaceRenderDetail detail);
    void recalcStats();
    void ensureOrbitalShell(const PlanetSurface& planet);
    void updateCpuResidency(const PlanetSurface& planet);
};

} // namespace elysium
