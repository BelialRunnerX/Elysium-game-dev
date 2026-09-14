# CHANGE_NOTE_CHUNK_OPT — P0 extremity bounds + adaptive chunk kind

## Behavior

- **`ChunkOccupancy` / `ChunkAdaptiveSummary`** (new): catalog P0-6 extremity AABB plus P0-7/P0-10 foundational `ChunkRepKind::{Empty,Homogeneous,Full,Mixed}` classification helpers, including incremental solid edit apply with boundary-shrink rescan.
- **`World`**: per-chunk occupancy summaries rebuilt after generate; updated on macro/micro edits via `noteCellOccupancyEdit` (expand cheap; boundary removal rescans that chunk).
- **`SurfaceChunkData`**: after adaptive `ChunkVoxelSpans` encode, `recomputeOccupancy()` fills core extents, kind, and per-column radial min/max (`columnMinR`/`columnMaxR`).
- **Cached spherical mesher**: Empty occupancy returns immediately (P0-22 adjacent); otherwise triple-loop restricted to occupancy AABB (U=X, V=Y, Radial=Z).
- Integrates with sibling **`ChunkVoxelSpans`** payload (Homogeneous/RLE/Dense) without fighting that lane.

## Files touched

| File | Change |
|---|---|
| `src/world/ChunkOccupancy.hpp` | new — extents, kinds, analyze/apply helpers |
| `src/world/ChunkOccupancy.cpp` | new — classify + incremental edit |
| `src/world/SurfaceChunkCache.hpp` | occupancy + column radial extremities on packet |
| `src/world/SurfaceChunkCache.cpp` | `recomputeOccupancy`; call from `buildChunk` |
| `src/world/World.hpp` / `World.cpp` | per-chunk occupancy; edit hooks |
| `src/world/PlanetSurfaceMesher.cpp` | Empty short-circuit + extent-restricted scan |
| `CMakeLists.txt` | `ChunkOccupancy.cpp` in `elysium_world` |
| `tests/headless_tests.cpp` | `testChunkOccupancyExtremityAndAdaptiveRep` |
| `CHUNK_OPT_GAP.md` | Present/Partial/Missing inventory |
| `CHANGE_NOTE_CHUNK_OPT.md` | this note |

## ECS migration (owner lock)

**N/A — no new ECS system or component.** Occupancy lives on world/chunk packets (`World`, `SurfaceChunkData`). No preexisting-entity migration (`ECS_MIGRATION_RULE.md`).

## Constraints respected

- Prefer `world/` / `render_core` — no `Game.cpp` HUD edits
- No MachineType / schema renumber
- Include-boundary clean (no EnTT/raylib in world)
- Did not fight `ChunkVoxelSpans` adaptive payload lane

## Verification

`ctest` in `build_headless`: **PASS** (`elysium_headless` + `include_boundary`).
