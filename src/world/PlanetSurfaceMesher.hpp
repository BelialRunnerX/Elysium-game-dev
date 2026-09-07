#pragma once

#include "world/PlanetSurface.hpp"
#include "world/SurfaceChunkCache.hpp"
#include "world/VoxelMesher.hpp"

namespace elysium {

// Builds one curved cube-sphere surface chunk in planet-local 3D coordinates.
// The snapshot remains whole-planet so seam neighbors are sampled from the same
// deterministic state, but only columns owned by the requested chunk emit
// geometry. This lets player edits rebuild one or two surface chunks rather
// than the complete six-face shell.
CpuMeshData buildPlanetSurfaceChunkMesh(const PlanetSurfaceSnapshot& planet,
                                        const PlanetChunkAddress& chunk);

// v0.11 direct LOD0 meshing path. The reconstructed cache packet carries the
// complete core plus a one-cell halo, so hidden-face/refined-boundary decisions
// do not need a whole-planet snapshot. This is the production streaming path.
CpuMeshData buildPlanetSurfaceChunkMesh(const SurfaceChunkData& chunk);

// Coarse direct-field style proxy for a surface chunk. This intentionally drops
// caves, microvoxels and small facade detail while preserving the deterministic
// terrain/player-edited silhouette at distance. Runtime streaming keeps one of
// these lightweight packets for non-resident LOD0 chunks.
CpuMeshData buildPlanetSurfaceFieldChunkMesh(const PlanetSurfaceSnapshot& planet,
                                             const PlanetChunkAddress& chunk,
                                             int step = 4,
                                             bool addTransitionSkirts = true,
                                             float skirtDepth = 0.75f,
                                             int influencedStep = 2);

// Orbit representation generated directly from deterministic climate/surface
// fields. It intentionally ignores voxel residency and small player edits.
CpuMeshData buildPlanetOrbitalClimateShellMesh(const PlanetSurfaceSnapshot& planet,
                                               int step = 8,
                                               float shellOffset = 0.10f);

// Sparse deterministic cloud layer above the orbital climate shell. Vertex
// alpha is carried through CpuMeshData so backends may blend it when supported.
CpuMeshData buildPlanetOrbitalCloudShellMesh(const PlanetSurfaceSnapshot& planet,
                                             int step = 8,
                                             float cloudAltitude = 2.0f);

// Headless/debug convenience that concatenates all surface chunks into one CPU
// packet. Runtime rendering uses buildPlanetSurfaceChunkMesh instead.
CpuMeshData buildPlanetSurfaceMesh(const PlanetSurfaceSnapshot& planet);

} // namespace elysium
