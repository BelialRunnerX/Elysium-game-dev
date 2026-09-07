#include "world/CubeSphere.hpp"

#include "core/Determinism.hpp"

#include <algorithm>
#include <cmath>

namespace elysium {
namespace {

Vec3 cubeVector(CubeFace face, float u, float v) {
    switch (face) {
        case CubeFace::PositiveX: return { 1.0f, v, -u};
        case CubeFace::NegativeX: return {-1.0f, v,  u};
        case CubeFace::PositiveY: return { u,  1.0f, -v};
        case CubeFace::NegativeY: return { u, -1.0f,  v};
        case CubeFace::PositiveZ: return { u, v,  1.0f};
        case CubeFace::NegativeZ: return {-u, v, -1.0f};
    }
    return {0,0,1};
}

int uvToCell(float uv, int resolution) {
    if (resolution <= 0) return 0;
    const float scaled = (uv + 1.0f) * 0.5f * static_cast<float>(resolution);
    return std::clamp(static_cast<int>(std::floor(scaled)),0,resolution-1);
}

} // namespace

Vec3 faceUvToDirection(CubeFace face, float u, float v) {
    return normalize(cubeVector(face,std::clamp(u,-1.0f,1.0f),std::clamp(v,-1.0f,1.0f)));
}

Vec3 faceUvToDirectionUnclamped(CubeFace face, float u, float v) {
    return normalize(cubeVector(face,u,v));
}

FaceUv directionToFaceUv(Vec3 direction) {
    const Vec3 d = normalize(direction);
    const float ax = std::abs(d.x);
    const float ay = std::abs(d.y);
    const float az = std::abs(d.z);

    // Deterministic seam ownership: X wins exact ties, then Y, then Z.
    if (ax >= ay && ax >= az) {
        if (d.x >= 0.0f) {
            return {CubeFace::PositiveX, -d.z / ax, d.y / ax};
        }
        return {CubeFace::NegativeX, d.z / ax, d.y / ax};
    }
    if (ay >= az) {
        if (d.y >= 0.0f) {
            return {CubeFace::PositiveY, d.x / ay, -d.z / ay};
        }
        return {CubeFace::NegativeY, d.x / ay, d.z / ay};
    }
    if (d.z >= 0.0f) {
        return {CubeFace::PositiveZ, d.x / az, d.y / az};
    }
    return {CubeFace::NegativeZ, -d.x / az, d.y / az};
}

FaceCell directionToFaceCell(Vec3 direction, int faceResolution) {
    const auto uv = directionToFaceUv(direction);
    return {uv.face,uvToCell(uv.u,faceResolution),uvToCell(uv.v,faceResolution)};
}

FaceCell wrapFaceCell(CubeFace face, int uCell, int vCell, int faceResolution) {
    if (faceResolution <= 0) return {face,0,0};
    if (uCell >= 0 && uCell < faceResolution && vCell >= 0 && vCell < faceResolution)
        return {face,uCell,vCell};
    const float u = ((static_cast<float>(uCell)+0.5f)/static_cast<float>(faceResolution))*2.0f-1.0f;
    const float v = ((static_cast<float>(vCell)+0.5f)/static_cast<float>(faceResolution))*2.0f-1.0f;
    return directionToFaceCell(faceUvToDirectionUnclamped(face,u,v),faceResolution);
}

Vec3 faceGridCellDirection(CubeFace face, int uCell, int vCell, int faceResolution) {
    if (faceResolution <= 0) return faceUvToDirection(face, 0.0f, 0.0f);
    const float inv = 1.0f / static_cast<float>(faceResolution);
    const float u = (static_cast<float>(uCell) + 0.5f) * inv * 2.0f - 1.0f;
    const float v = (static_cast<float>(vCell) + 0.5f) * inv * 2.0f - 1.0f;
    return faceUvToDirection(face, u, v);
}

Vec3 faceGridCornerDirection(CubeFace face, int uEdge, int vEdge, int faceResolution) {
    if (faceResolution <= 0) return faceUvToDirection(face,0.0f,0.0f);
    const float inv = 1.0f/static_cast<float>(faceResolution);
    const float u = static_cast<float>(uEdge)*inv*2.0f-1.0f;
    const float v = static_cast<float>(vEdge)*inv*2.0f-1.0f;
    return faceUvToDirection(face,u,v);
}

PlanetChunkAddress chunkAddress(CubeFace face, int uCell, int vCell, int radialCell, int chunkSize) {
    const auto divFloor = [chunkSize](int value) {
        int q = value / chunkSize;
        const int r = value % chunkSize;
        if (r != 0 && ((r < 0) != (chunkSize < 0))) --q;
        return q;
    };
    return {face, divFloor(uCell), divFloor(vCell), divFloor(radialCell)};
}

std::uint64_t stableChunkKey(const PlanetChunkAddress& address) {
    std::uint64_t h = mix64(0x454C595349554D43ULL ^ static_cast<std::uint64_t>(address.face));
    h = mix64(h ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(address.u)));
    h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(address.v)) << 1U));
    h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(address.radial)) << 2U));
    return h;
}

std::string toString(CubeFace face) {
    switch (face) {
        case CubeFace::PositiveX: return "+X";
        case CubeFace::NegativeX: return "-X";
        case CubeFace::PositiveY: return "+Y";
        case CubeFace::NegativeY: return "-Y";
        case CubeFace::PositiveZ: return "+Z";
        case CubeFace::NegativeZ: return "-Z";
    }
    return "?";
}

} // namespace elysium
