#pragma once

#include "core/Math.hpp"

#include <cstdint>
#include <string>

namespace elysium {

enum class CubeFace : std::uint8_t {
    PositiveX = 0,
    NegativeX,
    PositiveY,
    NegativeY,
    PositiveZ,
    NegativeZ
};

struct FaceUv {
    CubeFace face{CubeFace::PositiveZ};
    float u{}; // [-1, 1]
    float v{}; // [-1, 1]
};

struct FaceCell {
    CubeFace face{CubeFace::PositiveZ};
    int u{};
    int v{};

    bool operator==(const FaceCell&) const = default;
};

struct PlanetChunkAddress {
    CubeFace face{CubeFace::PositiveZ};
    int u{};
    int v{};
    int radial{};

    bool operator==(const PlanetChunkAddress&) const = default;
};

// Pure cube-sphere transforms. These are storage-address helpers, not renderer
// coordinates; generation should evaluate fields from the returned shared 3D direction.
Vec3 faceUvToDirection(CubeFace face, float u, float v);
// Used for seam traversal: unlike faceUvToDirection(), u/v are allowed to move
// beyond [-1,1] so an off-face cell center can be projected onto its owner face.
Vec3 faceUvToDirectionUnclamped(CubeFace face, float u, float v);
FaceUv directionToFaceUv(Vec3 direction);
FaceCell directionToFaceCell(Vec3 direction, int faceResolution);
FaceCell wrapFaceCell(CubeFace face, int uCell, int vCell, int faceResolution);
Vec3 faceGridCellDirection(CubeFace face, int uCell, int vCell, int faceResolution);
Vec3 faceGridCornerDirection(CubeFace face, int uEdge, int vEdge, int faceResolution);
PlanetChunkAddress chunkAddress(CubeFace face, int uCell, int vCell, int radialCell, int chunkSize = 32);
std::uint64_t stableChunkKey(const PlanetChunkAddress& address);
std::string toString(CubeFace face);

} // namespace elysium
