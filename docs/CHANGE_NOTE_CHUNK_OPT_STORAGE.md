# CHANGE_NOTE_CHUNK_OPT_STORAGE — P0 noise cache + RLE adaptive chunk voxels

## Behavior

- **`ChunkVoxelSpans`** (P0-7 / P0-9 / P0-10): adaptive runtime voxel payload for `SurfaceChunkData`.
  Encode-time state machine: **Homogeneous → RLE columns (radial axis) → Dense**.
  RLE packs coherent runs per (U,V) column; pathological high-entropy cubes promote to dense when packed run bytes ≥ 85% of dense size.
- **`SurfaceChunkData`** stores `ChunkVoxelSpans voxels` instead of a dense `vector<BlockType>`. `getWithHalo` / meshing / occupancy read identical logical voxels through the adaptive API. Authoritative edits remain in `PlanetSurface` journals (packets stay reconstructable/read-only after encode).
- **`SphericalNoiseBasis` + memoized `sphericalField`** (P0-12): cache axes/phases keyed by `(seed, field label)` so neighboring columns / biomes / materials reuse deterministic noise bases without recomputing hashes. Sample values stay identical to the prior direct-hash path.
- Headless coverage asserts homogeneous/RLE/dense identity vs dense sources, live cache packet voxels == `snapshot.get` for every halo cell, and cross-instance noise/block determinism.

## Files touched

| File | Change |
|---|---|
| `src/world/ChunkVoxelSpans.hpp` | new — adaptive spans + `SphericalNoiseBasis` |
| `src/world/ChunkVoxelSpans.cpp` | new — encode/get/materialize + basis evaluate |
| `src/world/SurfaceChunkCache.hpp` | `blocks` → `ChunkVoxelSpans voxels` |
| `src/world/SurfaceChunkCache.cpp` | encode adaptive on build; `getWithHalo` via spans |
| `src/world/PlanetSurface.cpp` | P0-12 thread-local basis memo in `sphericalField` |
| `tests/headless_tests.cpp` | `testChunkVoxelSpansAdaptiveIdentityAndNoiseBasisCache` |
| `CMakeLists.txt` | `ChunkVoxelSpans.cpp` in `elysium_world` |
| `CHANGE_NOTE_CHUNK_OPT_STORAGE.md` | this note |

## Constraints respected

- Representation changes preserve **identical logical voxels** (asserted cell-by-cell)
- No EnTT outside `ecs/`; no MachineType renumber
- Workers still build immutable snapshot packets; owner-thread publish unchanged
- Authoritative save/journal path unchanged (procedural baseline + deltas only)
- ctest PASS required

## Catalog mapping

- P0-7 Homogeneous chunk representation
- P0-9 RLE-based runtime voxel data
- P0-10 Adaptive representation promotion
- P0-12 Noise caching (basis memo by seed + field label)
