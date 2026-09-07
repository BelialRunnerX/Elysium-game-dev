#pragma once

#include <cmath>

namespace elysium {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator/(Vec3 v, float s) { return {v.x / s, v.y / s, v.z / s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
inline Vec3& operator-=(Vec3& a, Vec3 b) { a = a - b; return a; }
inline Vec3& operator*=(Vec3& a, float s) { a = a * s; return a; }

inline float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float lengthSq(Vec3 v) { return dot(v, v); }
inline float length(Vec3 v) { return std::sqrt(lengthSq(v)); }
inline Vec3 normalize(Vec3 v) {
    const float l = length(v);
    return l > 0.00001f ? v / l : Vec3{};
}
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}

struct IVec3 {
    int x{};
    int y{};
    int z{};
};

inline bool operator==(const IVec3& a, const IVec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace elysium
