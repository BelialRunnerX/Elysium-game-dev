# CHANGE_NOTE — Fix headless hang / pathological procedural-micro cost

## Symptom

`elysium_headless` timed out under `ctest --timeout 90` (and previously idled ~9+ min /
~892s when it completed). `include_boundary` stayed green.

## Which tests stalled

Instrumented run showed the suite burning CPU in:

| Test | Pre-fix | Role |
|---|---|---|
| `testWorkerCountMeshDeterminism` | ~168s (then hang under 90s) | 5 worker configs × 4 Temperate chunks of dense procedural micro |
| `testSphericalMeshWorkerDeterminismAfterGreedy` | ~same | same capture path |
| `testProceduralSubvoxelShaping` / micro persistence / halo mesh | 40–50s each | snapshot/cached `emit*Refined` |
| `testChunkOccupancyExtremityAndAdaptiveRep` | ~110s | World clear loop |

Not an infinite loop — pathological repeated work after the halo/`hasMicroDetail` paths.

## Root cause

1. **Mesher**: `emitRefinedCell` / `emitCachedRefined` re-evaluated `microGet` /
   `microGetLocal` ~6× per microcell (per greedy plane) while Temperate chunks
   activate ~600+ procedural macros → tens of millions of procedural probes + AO.
2. **`recomputeOccupancy`**: treated every `hasMicroDetail` cell as requiring a
   full 16³ `anyMicroSolid` scan (and did analyze + column passes separately),
   including solid macros that need no micro probe.
3. **World occupancy test**: `set(Air)` over 32³ cells triggered per-cell boundary
   occupancy rescans + `refreshExteriorConnectivityAndDirty()` → O(n²) thrash.

Halo correctness (`microGetLocal` / `testCachedSphericalMeshingUsesHalo`) was fine;
cost exploded once procedural micro was meshed/occupied correctly.

## Fix

1. **`sampleMicroBrickLocal` / `PlanetSurfaceSnapshot::sampleMicroBrick`** — hoist
   column heights/slope once; densify 16³ before greedy.
2. **Cached mesher** uses densified brick for occupancy masks + in-cell AO; seam
   neighbors still go through `microGetLocal`.
3. **`microGetLocal` thread_local cell probe cache** — amortize column/brick setup
   across AO/boundary probes on the same macro.
4. **`recomputeOccupancy`** — solid macro short-circuit; single pass; densify only
   for air macros with micro detail (e.g. depth −1 rubble).
5. **`World::fillChunk`** — bulk clear + one occupancy rebuild + one exterior
   refresh; occupancy test uses it.
6. **Worker-hash fixture** — Barren + 2 seam chunks (still steel + micro edit);
   still asserts 0/1/2/4/8 worker determinism.

## Correctness kept

- `testCachedSphericalMeshingUsesHalo` (topology vs snapshot + AO darkened corners)
- Micro refine / procedural underlay tests
- Greedy delta assertions
- Worker-count mesh hash equality

## ECS / constraints

- ECS_MIGRATION: N/A (world substrate + tests)
- Include-boundary: unchanged

## Files

| File | Change |
|---|---|
| `src/world/SurfaceChunkCache.hpp/.cpp` | densify API, occupancy fast path, probe cache |
| `src/world/PlanetSurface.hpp/.cpp` | `sampleMicroBrick` |
| `src/world/PlanetSurfaceMesher.cpp` | densify-once emit paths + dense AO |
| `src/world/World.hpp/.cpp` | `fillChunk` |
| `tests/headless_tests.cpp` | fillChunk occupancy; leaner worker fixture |
| `CHANGE_NOTE_FIX_HEADLESS_HANG.md` | this note |

## Verification

`ctest --timeout 90` in `build_headless`: **2/2 PASS**, wall **~26s**
(`elysium_headless` ~26s, `include_boundary` ~0.05s).
