#include "render/WorldRenderer.hpp"

#include <chrono>
#include <utility>

namespace elysium {

WorldRenderer::WorldRenderer(JobSystem& jobs, IGraphicsBackend& graphics)
    : jobs_(jobs), graphics_(graphics), chunks_(ChunkCount), pending_(ChunkCount) {
    targetChunkRevisions_.fill(0);
}

WorldRenderer::~WorldRenderer() {
    invalidate();
}

int WorldRenderer::slotIndex(int cx, int cy, int cz) {
    return cx + ChunkCountX * (cz + ChunkCountZ * cy);
}

void WorldRenderer::slotCoords(int slot, int& cx, int& cy, int& cz) {
    cy = slot / (ChunkCountX * ChunkCountZ);
    const int rem = slot - cy * ChunkCountX * ChunkCountZ;
    cz = rem / ChunkCountX;
    cx = rem - cz * ChunkCountX;
}

void WorldRenderer::invalidate() {
    for (auto& c : chunks_) {
        if (c.handle) graphics_.destroyMesh(c.handle);
        c = {};
    }
    // Futures are allowed to finish in the shared worker pool, but their results
    // no longer have a publish target after invalidation.
    for (auto& p : pending_) p.reset();
    latestSnapshot_.reset();
    targetChunkRevisions_.fill(0);
    snapshotRevision_ = 0;
    rebuiltChunksLastSync_ = 0;
    meshQuads_ = macroQuads_ = microQuads_ = triangleCount_ = 0;
}

void WorldRenderer::updateTargets(const World& world) {
    if (snapshotRevision_ == world.revision()) return;
    snapshotRevision_ = world.revision();
    latestSnapshot_ = std::make_shared<WorldSnapshot>(world.snapshot());
    for (int slot=0; slot<ChunkCount; ++slot) {
        int cx{},cy{},cz{};
        slotCoords(slot,cx,cy,cz);
        targetChunkRevisions_[static_cast<std::size_t>(slot)] = world.chunkRevision(cx,cy,cz);
    }
}

void WorldRenderer::sync(const World& world) {
    rebuiltChunksLastSync_ = 0;
    // Update target versions first. A completed old job must be rejected if an
    // edit landed before this owner-thread publication point.
    updateTargets(world);
    processCompleted();
    scheduleMissing();
    // Very small jobs may finish immediately; publishing in the same frame keeps
    // edit-to-visible latency low without blocking on unfinished work.
    processCompleted();
}

void WorldRenderer::draw() const {
    for (const auto& c : chunks_) {
        if (c.handle) graphics_.drawMesh(c.handle);
    }
}

int WorldRenderer::pendingJobs() const {
    int count = 0;
    for (const auto& p : pending_) if (p.has_value()) ++count;
    return count;
}

int WorldRenderer::dirtyChunks() const {
    int count = 0;
    for (int slot=0; slot<ChunkCount; ++slot) {
        if (chunks_[static_cast<std::size_t>(slot)].builtRevision != targetChunkRevisions_[static_cast<std::size_t>(slot)]) ++count;
    }
    return count;
}

void WorldRenderer::processCompleted() {
    using namespace std::chrono_literals;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        auto& pending = pending_[i];
        if (!pending) continue;
        if (pending->wait_for(0ms) != std::future_status::ready) continue;
        ChunkBuildResult result = pending->get();
        pending.reset();
        if (result.slot < 0 || result.slot >= ChunkCount) continue;
        if (result.chunkRevision != targetChunkRevisions_[static_cast<std::size_t>(result.slot)]) continue; // stale job rejection
        publish(result.slot, std::move(result.mesh), result.chunkRevision);
        ++rebuiltChunksLastSync_;
    }
}

void WorldRenderer::scheduleMissing() {
    if (!latestSnapshot_) return;
    for (int slot = 0; slot < ChunkCount; ++slot) {
        const std::uint64_t targetRevision = targetChunkRevisions_[static_cast<std::size_t>(slot)];
        if (chunks_[static_cast<std::size_t>(slot)].builtRevision == targetRevision) continue;
        if (pending_[static_cast<std::size_t>(slot)]) continue;

        int cx{},cy{},cz{};
        slotCoords(slot,cx,cy,cz);
        const auto snapshot = latestSnapshot_;
        pending_[static_cast<std::size_t>(slot)] = jobs_.submit([snapshot, slot, targetRevision, cx, cy, cz]() {
            ChunkBuildResult result{};
            result.slot = slot;
            result.chunkRevision = targetRevision;
            result.mesh = buildChunkMesh(*snapshot,cx,cy,cz);
            return result;
        });
    }
}

void WorldRenderer::publish(int slot, CpuMeshData&& data, std::uint64_t chunkRevision) {
    if (slot < 0 || slot >= static_cast<int>(chunks_.size())) return;
    auto& target = chunks_[static_cast<std::size_t>(slot)];
    if (target.handle) graphics_.destroyMesh(target.handle);

    target.handle = {};
    target.builtRevision = chunkRevision;
    target.quads = data.quads;
    target.macroQuads = data.macroQuads;
    target.microQuads = data.microQuads;
    target.triangles = data.triangleCount();
    if (!data.empty()) target.handle = graphics_.uploadMesh(data);
    recalcStats();
}

void WorldRenderer::recalcStats() {
    meshQuads_ = macroQuads_ = microQuads_ = triangleCount_ = 0;
    for (int slot=0; slot<ChunkCount; ++slot) {
        const auto& c = chunks_[static_cast<std::size_t>(slot)];
        // Preserve old valid GPU geometry while a dirty replacement is pending;
        // stats describe what is currently drawn, not only fully-current chunks.
        if (!c.handle && c.quads == 0) continue;
        meshQuads_ += c.quads;
        macroQuads_ += c.macroQuads;
        microQuads_ += c.microQuads;
        triangleCount_ += c.triangles;
    }
}

} // namespace elysium
