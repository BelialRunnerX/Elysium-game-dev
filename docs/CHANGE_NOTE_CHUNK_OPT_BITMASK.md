# CHANGE_NOTE_CHUNK_OPT_BITMASK — P0-21 binary/bitmask face culling

## Behavior

- **Solid-solid hidden-face culls** on the hot mesher paths now pack occupancy into `uint64_t` columns (core + one-cell halo) and decide exposure with shifts / AND-NOT.
- Applies to:
  - Planar `VoxelMesher` macro greedy (also keeps the exterior-air / OOB emit rule).
  - Spherical `PlanetSurfaceMesher` LOD0 macro greedy (snapshot + cached halo packet).
  - MicroBrick greedy fill (halo bit for the neighboring macro / brick; stitched across adjacent refined bricks in one face chunk).
- **Greedy merge (P0-17/18/19) is unchanged.** Bitmasks only replace the per-cell neighbor solidity test that fills the greedy mask. Visible topology matches the prior scalar cull for the same voxels.
- Halo one-cell contract is the extra bit on each end of the word: cube-face wraps, mantle (`radial < 0` → Stone), and sky (`radial >= RadialLayers` → Air) stay in the packed column.

## Equivalence

A core cell emits a greedy macro face iff:

- self is solid and not refined, and
- the neighbor along that axis is neither solid nor refined
- planar only: neighbor is exterior air **or** out of world bounds (sealed cavities stay hidden)

Bitwise form (`bit i` = local `i - halo`):

- `+axis`: `(solid & ~refined) & ~((solid | refined) >> 1)` [planar: `& (emitNeighbor >> 1)`]
- `-axis`: same with `<< 1`

## Deferred / Partial

- Cross-chunk / cube-face (cross-macro) micro merge is still not done; within-chunk cross-brick merge uses the same per-brick bitwise solid-solid test.
- Binary greedy meshing (merging via bit operations instead of `GreedyMaskCell`) is out of scope; rectangle growth is the existing 2D greedy.

## Files touched

| File | Change |
|---|---|
| `src/world/FaceCullBitmasks.hpp` | new — word layout, shift culls, scalar oracles, pack helpers |
| `src/world/VoxelMesher.cpp` | planar macro columns packed; shift cull before greedy |
| `src/world/PlanetSurfaceMesher.cpp` | snapshot + cached LOD0 macro + within-brick micro shift culls |
| `tests/headless_tests.cpp` | `testBitmaskFaceCullAgreesWithSolidSolid` |
| `CMakeLists.txt` | list already-present world TUs the headless harness links |
| `CHUNK_OPT_GAP.md` | P0-21 Present |
| `CHANGE_NOTE_CHUNK_OPT_MESH.md` | bitmask no longer deferred |
| `CHANGE_NOTE_CHUNK_OPT_BITMASK.md` | this note |

## Constraints respected

- No EnTT in `world/`.
- No MachineType / schema renumber.
- Halo one-cell sampling still drives seam/mantle/sky neighbors (now as packed bits).
- Worker mesh determinism 0/1/2/4/8 re-locked by existing greedy tests.

## Verification

`ctest` in `build_headless`: **PASS** (`elysium_headless` + `include_boundary`).
