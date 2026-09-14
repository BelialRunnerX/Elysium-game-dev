# CHANGE_NOTE_CHUNK_OPT_STREAM — P0-28 coherent spatial LOD

## Behavior (already on v0.20 tip)

Streaming focus on `PlanetSurfaceRenderer` is a **three-tier spatial LOD**, not a binary resident/evict switch:

| Tier | Packet | Typical budget (surface play) |
|---|---|---|
| **Full** (LOD0) | Editable macro/micro voxel mesh from `SurfaceChunkCache` | nearest 7 chunks |
| **FieldNear** | Direct-field silhouette, step 4 | next 9 chunks |
| **FieldFar** | Direct-field silhouette, step 8 | remainder |
| **Orbital shell** | Climate + cloud shells, no voxel LOD0 | `setOrbitalShellOnly()` |

`Game` calls `setStreamingFocus(player, 7, 9)` in spherical surface mode. Debug `setFullDetail()` restores every chunk to Full. Orbital preview does not request LOD0.

## Ownership

- **`PlanetSurfaceRenderer` owns focus → tier.** Rank is cosine similarity of chunk-center direction vs focus direction (tie-break: lower slot).
- **`SurfaceChunkCache` does not choose LOD.** It reconstructs LOD0 packets for Visible / Prefetch / EditRemesh, keeps last-valid geometry while rebuilding (P0-35/P0-55), and preempts lower-priority pending work. Halo stays one-cell.

## P0-29

Single-threshold ranking chatter is addressed in `CHANGE_NOTE_CHUNK_OPT_LOD_HYSTERESIS.md`. Hysteresis lives on this same renderer focus path; the cache contract is unchanged.

## Files (pre-existing)

| File | Role |
|---|---|
| `src/render/PlanetSurfaceRenderer.hpp` / `.cpp` | Full / FieldNear / FieldFar + orbital shell |
| `src/world/PlanetSurfaceMesher.cpp` | LOD0 greedy vs field/orbital packets |
| `src/world/SurfaceChunkCache.*` | LOD0 residency, retain, preempt, halo |
| `tests/headless_tests.cpp` | `testPlanetSurfaceMeshingAndRenderer` three-tier budgets |
