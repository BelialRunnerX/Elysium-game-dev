#pragma once

#include "world/World.hpp"

#include <cstdint>
#include <vector>

namespace elysium {

struct MaterialRange {
    BlockType material{BlockType::Air};
    int firstVertex{};
    int vertexCount{};
    int quads{};
};

struct CpuMeshData {
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<std::uint8_t> colors;
    std::vector<MaterialRange> materialRanges;
    int quads{};
    int macroQuads{};
    int microQuads{};
    int aoDarkenedCorners{};

    int vertexCount() const { return static_cast<int>(vertices.size() / 3U); }
    int triangleCount() const { return quads * 2; }
    bool empty() const { return vertices.empty(); }
};

CpuMeshData buildChunkMesh(const WorldSnapshot& world, int chunkX, int chunkY, int chunkZ);

} // namespace elysium
