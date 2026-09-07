#include "render/RaylibGraphicsBackend.hpp"

#include <cstring>
#include <vector>

namespace elysium {

RaylibGraphicsBackend::~RaylibGraphicsBackend() {
    std::vector<std::uint32_t> handles;
    handles.reserve(models_.size());
    for (const auto& [id,_] : models_) handles.push_back(id);
    for (const auto id : handles) destroyMesh(GraphicsMeshHandle{id});
}

GraphicsMeshHandle RaylibGraphicsBackend::uploadMesh(const CpuMeshData& data) {
    if (data.empty()) return {};
    Mesh mesh{};
    mesh.vertexCount = data.vertexCount();
    mesh.triangleCount = data.triangleCount();
    mesh.vertices = static_cast<float*>(MemAlloc(sizeof(float) * data.vertices.size()));
    mesh.normals = static_cast<float*>(MemAlloc(sizeof(float) * data.normals.size()));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(sizeof(unsigned char) * data.colors.size()));
    std::memcpy(mesh.vertices,data.vertices.data(),sizeof(float)*data.vertices.size());
    std::memcpy(mesh.normals,data.normals.data(),sizeof(float)*data.normals.size());
    std::memcpy(mesh.colors,data.colors.data(),sizeof(unsigned char)*data.colors.size());
    UploadMesh(&mesh,false);
    Model model=LoadModelFromMesh(mesh);
    const std::uint32_t id=nextHandle_++;
    models_.emplace(id,model);
    return GraphicsMeshHandle{id};
}

void RaylibGraphicsBackend::destroyMesh(GraphicsMeshHandle handle) {
    if (!handle) return;
    const auto it=models_.find(handle.value);
    if(it==models_.end()) return;
    UnloadModel(it->second);
    models_.erase(it);
}

void RaylibGraphicsBackend::drawMesh(GraphicsMeshHandle handle) const {
    if (!handle) return;
    const auto it=models_.find(handle.value);
    if(it==models_.end()) return;
    DrawModel(it->second,Vector3{0,0,0},1.0f,WHITE);
}

} // namespace elysium
