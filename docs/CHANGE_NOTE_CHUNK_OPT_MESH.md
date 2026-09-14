# CHANGE_NOTE_CHUNK_OPT_MESH — Spherical + MicroBrick greedy meshing

## Behavior

- **PlanetSurfaceMesher LOD0 macro path** now runs true 2D greedy merging in cube-sphere **face-grid space** (P0-18 / P0-19) before projecting corners to planet-local 3D.
- Applies to both production `buildPlanetSurfaceChunkMesh(SurfaceChunkData)` and snapshot `buildPlanetSurfaceChunkMesh(PlanetSurfaceSnapshot)` so cached vs snapshot topology stays aligned (seam/halo contract preserved).
- **MicroBrick surfaces** within a refined cell are greedily merged per face plane (P0-20 / P0-50 incremental). Cross-brick micro merge remains deferred. **P0-21 binary/bitmask solid-solid culling is Present** on planar VoxelMesher, spherical LOD0 macro (snapshot + cached), and within-brick micro (see `CHANGE_NOTE_CHUNK_OPT_BITMASK.md`).
- Merged quads recompute AO only at rectangle corners (same policy as planar `VoxelMesher`).
- Occupancy empty short-circuit + extremity AABB restriction on the cached path are preserved.
- Macro `raycast` restored to **macro occupancy** so placement `previous` stays an air cell (micro hits remain on `raycastMicro` / `solidAt`).

## Measurable step

- Isolated 4³ stone prism above terrain: macro face quads drop from naive 96 toward ~6–24 (test-locked).
- Micro 4³ cube inside a refined cell: micro quads strongly reduced (test-locked ≤ 48).
- Headless mesh hash determinism across worker counts **0/1/2/4/8** re-locked after greedy.

## Files touched

| File | Change |
|---|---|
| `src/world/PlanetSurfaceMesher.cpp` | face-grid greedy macro + within-brick micro greedy |
| `src/world/PlanetSurface.cpp` | macro raycast placement previous fix |
| `tests/headless_tests.cpp` | `testSphericalMacroAndMicroGreedyMeshing`, `testSphericalMeshWorkerDeterminismAfterGreedy` |
| `CHUNK_OPT_GAP.md` | MESH section inventory |
| `CHANGE_NOTE_CHUNK_OPT_MESH.md` | this note |

## Constraints respected

- Cube-sphere seam correctness: greedy stays within one face chunk; halo neighbor tests unchanged; tiled macro→refined boundaries not greedied across micro.
- No EnTT in world.
- Planar `VoxelMesher` already had greedy; unchanged.
