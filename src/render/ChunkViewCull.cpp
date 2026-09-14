#include "render/ChunkViewCull.hpp"

#include <cmath>

namespace elysium {
namespace {

constexpr float kPi = 3.14159265358979323846f;

Vec3 safeNormalize(Vec3 v, Vec3 fallback) {
    const float l = length(v);
    return l > 1.0e-6f ? v / l : fallback;
}

void includePoint(ChunkWorldBound& bound, Vec3 p) {
    if (!bound.valid) {
        bound.valid = true;
        bound.aabbMin = bound.aabbMax = p;
        bound.center = p;
        bound.radius = 0.0f;
        return;
    }
    bound.aabbMin.x = std::min(bound.aabbMin.x, p.x);
    bound.aabbMin.y = std::min(bound.aabbMin.y, p.y);
    bound.aabbMin.z = std::min(bound.aabbMin.z, p.z);
    bound.aabbMax.x = std::max(bound.aabbMax.x, p.x);
    bound.aabbMax.y = std::max(bound.aabbMax.y, p.y);
    bound.aabbMax.z = std::max(bound.aabbMax.z, p.z);
}

Vec3 aabbCenter(Vec3 mn, Vec3 mx) {
    return {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
}

Vec3 aabbCorner(Vec3 mn, Vec3 mx, int i) {
    return {
        (i & 1) ? mx.x : mn.x,
        (i & 2) ? mx.y : mn.y,
        (i & 4) ? mx.z : mn.z
    };
}

} // namespace

ChunkWorldBound makePlanetChunkBound(const PlanetChunkAddress& address,
                                     float referenceRadius,
                                     int faceResolution,
                                     int chunkSize,
                                     int referenceRadial,
                                     float pad) {
    ChunkWorldBound bound{};
    if (faceResolution <= 0 || chunkSize <= 0) return bound;

    const int u0 = address.u * chunkSize;
    const int v0 = address.v * chunkSize;
    const int r0 = address.radial * chunkSize;
    const int u1 = u0 + chunkSize;
    const int v1 = v0 + chunkSize;
    const int r1 = r0 + chunkSize;

    // 3x3x3 UVR samples capture cube-sphere bulge (patch centers sit outside
    // the 8-corner cartesian box). Pad covers micro (1/16 m) and numeric slack;
    // field skirts pull inward so they stay inside.
    for (int ir = 0; ir < 3; ++ir) {
        const int r = r0 + ir * (r1 - r0) / 2;
        const float radius = referenceRadius + static_cast<float>(r - referenceRadial);
        for (int iv = 0; iv < 3; ++iv) {
            const int v = v0 + iv * (v1 - v0) / 2;
            for (int iu = 0; iu < 3; ++iu) {
                const int u = u0 + iu * (u1 - u0) / 2;
                const Vec3 d = faceGridCornerDirection(address.face, u, v, faceResolution);
                includePoint(bound, d * radius);
            }
        }
    }
    if (!bound.valid) return bound;

    pad = std::max(0.0f, pad);
    bound.aabbMin -= Vec3{pad, pad, pad};
    bound.aabbMax += Vec3{pad, pad, pad};
    bound.center = aabbCenter(bound.aabbMin, bound.aabbMax);
    bound.radius = 0.0f;
    for (int i = 0; i < 8; ++i)
        bound.radius = std::max(bound.radius, length(aabbCorner(bound.aabbMin, bound.aabbMax, i) - bound.center));

    bound.prismCornerCount = 0;
    for (int ir = 0; ir < 2 && bound.prismCornerCount < 8; ++ir) {
        const int r = ir ? r1 : r0;
        const float radius = referenceRadius + static_cast<float>(r - referenceRadial);
        for (int iv = 0; iv < 2; ++iv) {
            const int v = iv ? v1 : v0;
            for (int iu = 0; iu < 2; ++iu) {
                const int u = iu ? u1 : u0;
                const Vec3 d = faceGridCornerDirection(address.face, u, v, faceResolution);
                bound.prismCorners[bound.prismCornerCount++] = d * radius;
            }
        }
    }
    return bound;
}

Frustum makePerspectiveFrustum(const ChunkViewCamera& camera) {
    Frustum frustum{};
    const Vec3 f = safeNormalize(camera.forward, Vec3{0, 0, 1});
    Vec3 r = cross(f, camera.up);
    if (lengthSq(r) < 1.0e-8f) r = cross(f, Vec3{0, 1, 0});
    if (lengthSq(r) < 1.0e-8f) r = cross(f, Vec3{1, 0, 0});
    r = safeNormalize(r, Vec3{1, 0, 0});
    const Vec3 u = safeNormalize(cross(r, f), Vec3{0, 1, 0});

    const float fovy = std::clamp(camera.fovyDegrees, 1.0f, 179.0f) * (kPi / 180.0f);
    const float aspect = std::max(0.01f, camera.aspect);
    const float nearZ = std::max(1.0e-4f, camera.nearPlane);
    const float farZ = std::max(nearZ + 1.0e-3f, camera.farPlane);

    const float tanHalfV = std::tan(fovy * 0.5f);
    const float tanHalfH = tanHalfV * aspect;
    const float halfV = std::atan(tanHalfV);
    const float halfH = std::atan(tanHalfH);
    const float cV = std::cos(halfV);
    const float sV = std::sin(halfV);
    const float cH = std::cos(halfH);
    const float sH = std::sin(halfH);

    auto setPlane = [&](int i, Vec3 n, Vec3 point) {
        n = safeNormalize(n, Vec3{0, 0, 1});
        frustum.planes[i].n = n;
        frustum.planes[i].d = -dot(n, point);
    };

    // Inward normals. Index: 0 near, 1 far, 2 left, 3 right, 4 bottom, 5 top.
    setPlane(0, f, camera.eye + f * nearZ);
    setPlane(1, f * -1.0f, camera.eye + f * farZ);
    setPlane(2, r * cH + f * sH, camera.eye);
    setPlane(3, r * -cH + f * sH, camera.eye);
    setPlane(4, u * cV + f * sV, camera.eye);
    setPlane(5, u * -cV + f * sV, camera.eye);
    return frustum;
}

bool sphereIntersectsFrustum(Vec3 center, float radius, const Frustum& frustum) {
    radius = std::max(0.0f, radius);
    for (int i = 0; i < 6; ++i) {
        if (frustum.planes[i].distance(center) < -radius) return false;
    }
    return true;
}

bool aabbIntersectsFrustum(Vec3 aabbMin, Vec3 aabbMax, const Frustum& frustum) {
    for (int i = 0; i < 6; ++i) {
        const auto& plane = frustum.planes[i];
        const Vec3 px{
            plane.n.x >= 0.0f ? aabbMax.x : aabbMin.x,
            plane.n.y >= 0.0f ? aabbMax.y : aabbMin.y,
            plane.n.z >= 0.0f ? aabbMax.z : aabbMin.z
        };
        if (plane.distance(px) < 0.0f) return false;
    }
    return true;
}

bool pointHiddenByPlanetSphere(Vec3 eye, Vec3 point, float occluderRadius) {
    if (occluderRadius <= 0.0f) return false;
    const Vec3 delta = point - eye;
    const float lenSq = lengthSq(delta);
    if (lenSq < 1.0e-8f) return false;
    const float len = std::sqrt(lenSq);
    const Vec3 dir = delta / len;
    const float halfB = dot(eye, dir);
    const float c = lengthSq(eye) - occluderRadius * occluderRadius;
    const float disc = halfB * halfB - c;
    if (disc <= 0.0f) return false;
    const float sqrtD = std::sqrt(disc);
    const float t0 = -halfB - sqrtD;
    const float t1 = -halfB + sqrtD;
    const float lo = 1.0e-3f;
    const float hi = len - 1.0e-3f;
    auto hits = [&](float t) { return t > lo && t < hi; };
    return hits(t0) || hits(t1);
}

bool aabbFullyBehindHorizon(Vec3 aabbMin, Vec3 aabbMax, Vec3 eye, float occluderRadius) {
    ChunkWorldBound bound{};
    bound.valid = true;
    bound.aabbMin = aabbMin;
    bound.aabbMax = aabbMax;
    bound.center = aabbCenter(aabbMin, aabbMax);
    bound.prismCornerCount = 0;
    return boundFullyBehindHorizon(bound, eye, occluderRadius);
}

bool boundFullyBehindHorizon(const ChunkWorldBound& bound, Vec3 eye, float occluderRadius) {
    if (!bound.valid || occluderRadius <= 0.0f) return false;
    if (length(eye) <= occluderRadius + 0.25f) return false;

    if (!pointHiddenByPlanetSphere(eye, bound.center, occluderRadius))
        return false;
    if (bound.prismCornerCount > 0) {
        for (int i = 0; i < bound.prismCornerCount; ++i) {
            if (!pointHiddenByPlanetSphere(eye, bound.prismCorners[i], occluderRadius))
                return false;
        }
        return true;
    }
    for (int i = 0; i < 8; ++i) {
        if (!pointHiddenByPlanetSphere(eye, aabbCorner(bound.aabbMin, bound.aabbMax, i), occluderRadius))
            return false;
    }
    return true;
}

ChunkCullReason classifyChunkBound(const ChunkWorldBound& bound, const ChunkViewCamera& view) {
    if (!bound.valid) return ChunkCullReason::Visible;

    if (view.enableFrustum) {
        const Frustum frustum = makePerspectiveFrustum(view);
        if (!sphereIntersectsFrustum(bound.center, bound.radius, frustum) ||
            !aabbIntersectsFrustum(bound.aabbMin, bound.aabbMax, frustum)) {
            return ChunkCullReason::OutsideFrustum;
        }
    }
    if (view.enableHorizon && view.occluderRadius > 0.0f) {
        if (boundFullyBehindHorizon(bound, view.eye, view.occluderRadius))
            return ChunkCullReason::BehindHorizon;
    }
    return ChunkCullReason::Visible;
}

} // namespace elysium
