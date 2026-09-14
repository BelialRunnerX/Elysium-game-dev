#pragma once

#include "core/Math.hpp"
#include "world/CubeSphere.hpp"

#include <algorithm>
#include <cstdint>

namespace elysium {

// Compact world-space bounds for one cube-sphere surface chunk (catalog P0-24).
// AABB is the cartesian box of sampled UVR corners / edge midpoints / centers;
// the sphere is a padded bounding sphere of those samples. Draw culling uses
// these instead of binding mesh state.
struct ChunkWorldBound {
    Vec3 center{};
    float radius{};
    Vec3 aabbMin{};
    Vec3 aabbMax{};
    // Actual UVR prism corners (on the cube-sphere shells). Horizon tests
    // these rather than cartesian AABB corners, which stick into empty sky
    // around a spherical patch and would never reject the far face.
    Vec3 prismCorners[8]{};
    int prismCornerCount{};
    bool valid{};
};

enum class ChunkCullReason : std::uint8_t {
    Visible = 0,
    OutsideFrustum = 1,
    BehindHorizon = 2
};

// Perspective camera in planet-local space. Matches Game's Camera3D contract
// (vertical fovy, look along `forward`, OpenGL-style near/far).
struct ChunkViewCamera {
    Vec3 eye{};
    Vec3 forward{0, 0, 1};
    Vec3 up{0, 1, 0};
    float fovyDegrees{75.0f};
    float aspect{1.0f};
    float nearPlane{0.01f};
    float farPlane{1000.0f};
    // Geometric occluder for P0-25. Zero disables horizon rejection.
    // Conservative default is the inner voxel shell, not mean surface radius,
    // so terrain that pokes above a mean sphere cannot pop at the limb.
    float occluderRadius{};
    bool enableFrustum{true};
    bool enableHorizon{true};
};

struct FrustumPlane {
    Vec3 n{};
    float d{}; // n·x + d >= 0 is the inside half-space
    float distance(Vec3 p) const { return dot(n, p) + d; }
};

struct Frustum {
    FrustumPlane planes[6]{};
};

inline constexpr float kChunkBoundPad = 1.25f;

// Inner voxel-shell radius minus a tiny pad. Using the mean reference radius
// as the occluder would hide limb hills on a small planet; shrinking is the
// conservative no-pop choice (under-cull rather than over-cull).
inline float planetHorizonOccluderRadius(float referenceRadius,
                                         int referenceRadial = 16,
                                         float pad = 0.5f) {
    return std::max(1.0f, referenceRadius - static_cast<float>(referenceRadial) - pad);
}

inline ChunkViewCamera makePlanetSurfaceView(Vec3 eye, Vec3 forward, Vec3 up,
                                             float fovyDegrees, float aspect,
                                             float referenceRadius,
                                             int referenceRadial = 16) {
    ChunkViewCamera cam;
    cam.eye = eye;
    cam.forward = forward;
    cam.up = up;
    cam.fovyDegrees = fovyDegrees;
    cam.aspect = std::max(0.01f, aspect);
    cam.occluderRadius = planetHorizonOccluderRadius(referenceRadius, referenceRadial);
    return cam;
}

ChunkWorldBound makePlanetChunkBound(const PlanetChunkAddress& address,
                                     float referenceRadius,
                                     int faceResolution = 64,
                                     int chunkSize = 32,
                                     int referenceRadial = 16,
                                     float pad = kChunkBoundPad);

Frustum makePerspectiveFrustum(const ChunkViewCamera& camera);

bool sphereIntersectsFrustum(Vec3 center, float radius, const Frustum& frustum);
bool aabbIntersectsFrustum(Vec3 aabbMin, Vec3 aabbMax, const Frustum& frustum);

// True when the open segment (eye, point) hits the occluder sphere before
// reaching the point. Points on the near shell are visible.
bool pointHiddenByPlanetSphere(Vec3 eye, Vec3 point, float occluderRadius);

// Conservative: every AABB corner and the center must be hidden, and the
// camera must sit outside both the occluder and the bound.
bool aabbFullyBehindHorizon(Vec3 aabbMin, Vec3 aabbMax, Vec3 eye, float occluderRadius);
bool boundFullyBehindHorizon(const ChunkWorldBound& bound, Vec3 eye, float occluderRadius);

ChunkCullReason classifyChunkBound(const ChunkWorldBound& bound, const ChunkViewCamera& view);

inline bool chunkBoundContains(const ChunkWorldBound& bound, Vec3 p) {
    if (!bound.valid) return false;
    return p.x >= bound.aabbMin.x && p.x <= bound.aabbMax.x &&
           p.y >= bound.aabbMin.y && p.y <= bound.aabbMax.y &&
           p.z >= bound.aabbMin.z && p.z <= bound.aabbMax.z;
}

} // namespace elysium
