#pragma once

#include "world/VoxelMesher.hpp"

#include <cstdint>

namespace elysium {

// Opaque renderer-resource identity. It is deliberately backend-local and must
// never be serialized or confused with world/ECS stable IDs.
struct GraphicsMeshHandle {
    std::uint32_t value{};
    friend bool operator==(GraphicsMeshHandle, GraphicsMeshHandle) = default;
    explicit operator bool() const { return value != 0; }
};

class IGraphicsBackend {
public:
    virtual ~IGraphicsBackend() = default;

    // Called only on the graphics owner thread. Worker jobs may build
    // CpuMeshData, but they never call these methods.
    virtual GraphicsMeshHandle uploadMesh(const CpuMeshData& data) = 0;
    virtual void destroyMesh(GraphicsMeshHandle handle) = 0;
    virtual void drawMesh(GraphicsMeshHandle handle) const = 0;
};

} // namespace elysium
