# CHANGE_NOTE_CHUNK_OPT_MESH_CROSSBRICK — P0-20 cross-MicroBrick greedy

## Behavior

- **PlanetSurfaceMesher** now greedily merges compatible micro faces **across adjacent refined MicroBricks inside one face chunk** (cached halo packet + snapshot paths).
- Material must match (same policy as within-brick greedy). Cached path still recomputes AO only at rectangle corners; snapshot path remains unshaded emit.
- Exposure is **P0-21 per-brick bitmask** solid-solid (halo extra bit), equivalent to the prior scalar neighbor test. Greedy only grows rectangles of already-exposed faces; it does not alter halo / seam culls.
- Macro→refined **tiled** boundaries stay per-subface and are not merged into refined micro quads.

## Remaining limits (cross-macro)

- Merge stays inside **one cube-sphere face chunk**. Chunk edges and cube-face seams still emit independently; the one-cell halo is sampling-only (no geometry from halo bricks).
- Binary/bitmask *greedy* (bit-run meshing) is still out of scope. P0-21 column bitmasks cannot pack a full-chunk micro axis into `uint64_t` (512+halo bits); exposure stays per-brick / per-cell.

## Measurable step

- Isolated 2×2 full-stone MicroBrick pad in air: within-brick-only would emit **16** outer quads; cross-brick merge emits **6** (test-locked `< 16` and `≤ 8`).
- Cached vs snapshot micro quad counts stay equal.
- Worker mesh hash determinism **0/1/2/4/8** re-locked by the existing greedy test.

## Files touched

| File | Change |
|---|---|
| `src/world/PlanetSurfaceMesher.cpp` | chunk-local cross-brick micro greedy (snapshot + cached); unclamped micro extents |
| `tests/headless_tests.cpp` | 2×2 pad assertion in `testSphericalMacroAndMicroGreedyMeshing` |
| `docs/CHUNK_OPT_GAP.md` | P0-20 Present; remaining cross-macro documented |
| `docs/CHANGE_NOTE_CHUNK_OPT_MESH.md` | cross-brick no longer deferred |
| `docs/CHANGE_NOTE_CHUNK_OPT_MESH_CROSSBRICK.md` | this note |

## Verification

`ctest` in `build_headless`: **PASS** (`elysium_headless` + `include_boundary`).

## Constraints respected

- Cube-sphere seam topology: greedy does not wrap U/V across faces.
- Halo one-cell contract unchanged.
- No EnTT in `world/`.
- No MachineType / schema renumber.
