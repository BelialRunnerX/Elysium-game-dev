# CHUNK_OPT_GAP — chunk occupancy / meshing catalog (v0.20)

Honest inventory against the chunk-opt catalog. Source of truth is the code in `src/world/` / `src/render/` plus headless tests, not older PDFs.

---

## MESH

| Item | Status | Notes |
|---|---|---|
| P0-17 Hidden-face / solid-solid cull | **Present** | Planar + spherical; interior sealed cavities stay hidden (planar exterior-air) |
| P0-18 Greedy meshing | **Present** | Planar `VoxelMesher` 2D rectangle merge |
| P0-19 Spherical / face-grid greedy | **Present** | `PlanetSurfaceMesher` LOD0 macro in cube-sphere UVR before projection |
| P0-20 MicroBrick greedy | **Present** | Within-brick and **cross-brick within a chunk**; not across chunk / cube-face (cross-macro) seams |
| P0-21 Binary/bitmask face culling | **Present** | `FaceCullBitmasks.hpp`: `uint64_t` core+halo columns; shift/AND-NOT for solid-solid. Used by VoxelMesher macro, spherical LOD0 macro (snapshot + cached halo), and MicroBrick fill (per-brick columns with halo bit, including the stitched cross-brick greedy mask). Does not change greedy topology. Binary greedy merge (bit-run meshing) not in scope. |
| P0-22 Empty-chunk skip | **Present** | Cached mesher returns immediately on `occupancy.isEmpty()` |
| P0-50 Incremental micro | **Partial** | Chunk remesh still rebuilds all refined bricks in the occupancy AABB |
| P0-59 Worker mesh determinism | **Present** | Locked 0/1/2/4/8 after greedy; bitmask must not break this |

## STORAGE / OCCUPANCY

| Item | Status | Notes |
|---|---|---|
| P0-6 Extremity AABB | **Present** | `ChunkOccupancy` / cached mesher scan restriction |
| P0-7 Homogeneous chunk | **Present** | `ChunkVoxelSpans` + `ChunkRepKind` |
| P0-9 RLE runtime voxels | **Present** | Radial-column RLE in `ChunkVoxelSpans` |
| P0-10 Adaptive promotion | **Present** | Homogeneous → RLE → Dense |
| P0-12 Noise caching | **Present** | `SphericalNoiseBasis` memo by (seed, field label) |
| P0-35 / P0-55 Retain while rebuild | **Present** | `SurfaceChunkCache` keeps last valid packet |

## DRAW / CULL

| Item | Status | Notes |
|---|---|---|
| P0-24 Frustum-cull chunk bounds | **Present** | Compact AABB + sphere per cube-sphere chunk; `PlanetSurfaceRenderer::draw` classifies before `drawMesh`. No view set ⇒ draw-all (debug / existing tests). Planar `WorldRenderer` still draws every mesh. |
| P0-25 Planet-horizon culling | **Present** | Same pass. Conservative inner-shell occluder (`referenceRadius − ReferenceRadial − 0.5`); a bound is rejected only when center + 8 UVR prism corners are all hidden. Limb-straddling chunks stay visible (no pop). |

## STREAMING / LOD

| Item | Status | Notes |
|---|---|---|
| P0-28 Coherent spatial LOD | **Present** | Full / FieldNear / FieldFar + orbital shell. Owner: `PlanetSurfaceRenderer` streaming focus. `SurfaceChunkCache` rebuilds LOD0 packets only; it does not choose tier. |
| P0-29 LOD hysteresis | **Present** | Enter/exit cosine bands (`kLodFullExitBand` / `kLodNearExitBand` = 0.03) + one-rank slack on renderer focus scores. Dirty-bit deadzone `kLodFocusDirtyCosine` = 0.9995 is not an LOD band. See `CHANGE_NOTE_CHUNK_OPT_LOD_HYSTERESIS.md`. |

## Tests

- `testGreedyMeshingAndExteriorAir` — planar greedy + cavity cull
- `testSphericalMacroAndMicroGreedyMeshing` — face-grid greedy + cached/snapshot topology + cross-brick 2×2 pad
- `testSphericalMeshWorkerDeterminismAfterGreedy` — 0/1/2/4/8
- `testCachedSphericalMeshingUsesHalo` — one-cell halo seam
- `testBitmaskFaceCullAgreesWithSolidSolid` — bitmask vs prior scalar solid-solid on synthetic + planar + spherical/halo fixtures
- `testChunkOccupancyExtremityAndAdaptiveRep` / `testChunkVoxelSpansAdaptiveIdentityAndNoiseBasisCache`
- `testSurfaceChunkCacheRetainAndPriorityPreempt` / `testPlanetSurfaceMeshingAndRenderer` — retain, preempt, three-tier budgets
- `testLodHysteresisDoesNotFlipFlopInBand` / `testStreamingLodHysteresisNoFlipFlop` — P0-29 enter/exit bands
- `testChunkFrustumAndHorizonCull` — P0-24 frustum vs P0-25 horizon; draw-list matches classified sets; retain GPU / LOD residency

See `CHANGE_NOTE_CHUNK_OPT.md`, `CHANGE_NOTE_CHUNK_OPT_STORAGE.md`, `CHANGE_NOTE_CHUNK_OPT_MESH.md`, `CHANGE_NOTE_CHUNK_OPT_BITMASK.md`, `CHANGE_NOTE_CHUNK_OPT_MESH_CROSSBRICK.md`, `CHANGE_NOTE_CHUNK_OPT_STREAM.md`, `CHANGE_NOTE_CHUNK_OPT_LOD_HYSTERESIS.md`, `CHANGE_NOTE_CHUNK_OPT_CULL.md`.
