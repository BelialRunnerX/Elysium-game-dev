#pragma once

#include "render/GraphicsBackend.hpp"

#include <cstdint>
#include <unordered_map>

#include <raylib.h>

namespace elysium {

class RaylibGraphicsBackend final : public IGraphicsBackend {
public:
    RaylibGraphicsBackend() = default;
    ~RaylibGraphicsBackend() override;

    GraphicsMeshHandle uploadMesh(const CpuMeshData& data) override;
    void destroyMesh(GraphicsMeshHandle handle) override;
    void drawMesh(GraphicsMeshHandle handle) const override;

private:
    std::uint32_t nextHandle_{1};
    std::unordered_map<std::uint32_t,Model> models_;
};

} // namespace elysium
