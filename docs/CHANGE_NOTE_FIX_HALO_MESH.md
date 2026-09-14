# CHANGE_NOTE — Fix cached halo meshing vs snapshot topology

## Root cause

`SurfaceChunkData::microGet` resolved the macro baseline only when the queried
cell's **face matched the chunk face**. Cross-face one-cell halo samples
(normalized onto an adjacent cube face) fell through to `BlockType::Air`, so
seam-adjacent procedural/refined micro occupancy disagreed with
`PlanetSurfaceSnapshot::microGet` / `get`. That diverged cached LOD0 topology
(`quads` / `microQuads` / vertices) from the snapshot reference in
`testCachedSphericalMeshingUsesHalo`.

Occupancy / mesher hot paths that already had local halo coordinates still went
through the world-address `microGet`, reintroducing the same seam bug during
boundary occlusion and AO.

## Fix

1. **`SurfaceChunkData::microGetLocal(lu,lv,lr,mu,mr,mv)`** — macro from
   `getWithHalo`; procedural keys from `worldAddress` (correct across seams).
2. **`localCoordsFor`** — maps a (possibly wrapped) world address back into the
   packet's halo frame; `microGet` delegates to `microGetLocal` when in-halo.
3. **Cached mesher** (`cachedBoundaryMicro`, `cachedMicroOffset`,
   `emitCachedRefined`) and **`recomputeOccupancy`** use `microGetLocal`.
4. **Greedy test** (`testSphericalMacroAndMicroGreedyMeshing`) — assert prism
   *delta* vs virgin chunk mesh (heightfield tops were drowning an absolute
   `macroQuads <= 24` bound); micro bound accounts for tiled macro→micro seams.

## Prior failure (verified)

`testPlanetSurfaceMicroVolumeAndPersistence` — `setMicro` still refines
(`isRefined`) and journals player deltas; procedural underlay remains
zero-storage via `microGet`. No further change required.

## ECS / constraints

- **ECS_MIGRATION:** N/A (world substrate only)
- MachineType renumber: not touched
- Include-boundary: unchanged (world→world)

## Files

| File | Change |
|---|---|
| `src/world/SurfaceChunkCache.hpp/.cpp` | `microGetLocal`, `localCoordsFor`, occupancy via local micro |
| `src/world/PlanetSurfaceMesher.cpp` | cached micro queries use `microGetLocal` |
| `tests/headless_tests.cpp` | greedy fixture delta assertions |
| `CHANGE_NOTE_FIX_HALO_MESH.md` | this note |
| `CHANGE_NOTE_SUBVOXEL_SHAPE.md` | cross-ref seam macro fix |

## Tests

- `testCachedSphericalMeshingUsesHalo`
- `testPlanetSurfaceMicroVolumeAndPersistence`
- `testSphericalMacroAndMicroGreedyMeshing`
- full `ctest` 2/2

## Follow-up (headless hang)

Post-halo procedural micro cost caused `elysium_headless` timeouts — see
`CHANGE_NOTE_FIX_HEADLESS_HANG.md` (densify-once meshing + occupancy fast path).
