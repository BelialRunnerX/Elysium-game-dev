# CHANGE_NOTE — Regenerable procedural subvoxel (16³) world shaping

## Behavior

**Zero-storage regenerable procedural micro** (preferred design):

1. `proceduralMicroActive` / `proceduralMicroBlock` in `ProceduralMicro.*` evaluate **O(1)** per micro query from `seed + PlanetClass + column height + slope + (mu,mr,mv)`.
2. Virgin terrain keeps **zero journal MicroBricks**. `PlanetSurface::microGet` / `resolveMicro` order: **player brick → else procedural → else macro baseline**.
3. `hasMicroDetail` is true when the cell is player-refined **or** procedural-active. Mesher (`PlanetSurfaceMesher`) and `SurfaceChunkData` occupancy gate on it so collision and visuals agree.
4. First `setMicro` calls `refineCell`, which journals **player deltas only** (no procedural sibling materialization into the MicroBrick). Non-overridden micros keep a **regenerable procedural underlay** via `microGet`. If the brick returns to zero overrides but procedural would resurrect, the empty brick is kept as a **tombstone**; otherwise it is erased.

**Temperate (primary):** rocky steps inside the surface metre, eroded cliff lips, cracked crust seams, rubble skirts / overhang chips on steep near-surface (incl. depth −1 air shell when slope ≥ 2).

**Barren / Scorched (light hooks):** steep crust only (`depth == 0 && slope >= 2`). Other catalog classes share the same light crust gate.

## GeneratorVersion / fingerprint

Terrain lane already owns **`PlanetSurface::GeneratorVersion = 2`** / **`ELYSPH02`** (`0x454C595350483032`). This lane **coordinates on that same bump** (does not advance to 3). Macro column heights/materials are unchanged by procedural micro; only micro occupancy near interesting surface cells gains regenerable detail. Saves/sidecars continue to gate on `kSurfaceGeneratorVersion` / `kSurfaceGeneratorFingerprint`.

## ECS migration

**N/A — no new ECS system or component.** World substrate only (`world/` + headless tests). No preexisting-entity migration.

## Files

| File | Change |
|---|---|
| `src/world/ProceduralMicro.hpp/.cpp` | regenerable micro occupancy API |
| `src/world/PlanetSurface.hpp/.cpp` | `hasMicroDetail`, `resolveMicro` / `microGet`, `refineCell` player-delta only, `setMicro` tombstones |
| `src/world/PlanetSurfaceMesher.cpp` | mesh via `hasMicroDetail` + `microGet` → `microQuads` |
| `src/world/SurfaceChunkCache.hpp/.cpp` | seed/class/heights on packets; cache `microGet` / `hasMicroDetail` / occupancy |
| `tests/headless_tests.cpp` | `testProceduralSubvoxelShaping` |
| `DESIGN_MAPPING.md` | Part 6 note |
| `CHANGE_NOTE_SUBVOXEL_SHAPE.md` | this note |

## Constraints

- No MachineType renumber
- No ECS migration (world substrate only)
- Include-boundary unchanged (world→world; no ecs/game/raylib)
- O(1) per micro query; no whole-brick scan unless meshing an active cell; journal stores player deltas only
- Player edits always win over regenerable procedural

## Tests (`testProceduralSubvoxelShaping`)

- Same seed → deterministic procedural micro across two `PlanetSurface` instances
- Virgin journals empty; procedural detail does **not** store MicroBricks
- `setMicro` overrides procedural, refines/journals **player deltas only**; siblings keep regenerable procedural underlay
- Virgin snapshot mesh path sees `hasMicroDetail`; chunk mesh emits geometry including **`microQuads > 0`**

## Soft Present follow-up

Mesh path **already** emits `microQuads` for virgin procedural cells via `hasMicroDetail` → `emitRefinedCell` / cached equivalent (same upload path as player MicroBrick edits). No second edit authority from HUD.

**Optional Present polish (not required for this lane):** stronger silhouette (edge highlight / Full-residency bias) so procedural rocky steps read more clearly at a glance — see `VISUAL_READABILITY_GAP.md` §4 / follow-up #5. Logic + mesher contract are Present-ready.

---

doc fix applied: Behavior §4 / Files / Constraints / Tests now match player-deltas-only + regenerable underlay + empty-brick tombstone (Architecture APPROVE DOC-ONLY).

## Follow-up (halo seam)

Cached packets must sample macro baselines through **halo-local** coordinates
(`SurfaceChunkData::microGetLocal`). Face-normalized world addresses on the
one-cell U/V halo previously defaulted macro to Air — see
`CHANGE_NOTE_FIX_HALO_MESH.md`.

