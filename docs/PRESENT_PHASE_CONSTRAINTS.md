# Present-phase constraints — for Presentation bot

Canonical look: `/workspace/elysium/01_specs/art_direction/ART_DIRECTION.md` (CONCEPT v0.7).
Architecture owns dependency direction; Presentation owns HUD parity.

## Hard rules

1. **Present consumes snapshots only** — vitals, inventory, hotbar, Suspicion meters, minimap coords are read-only views published after Commit/Persist.
2. **No world mutation from UI** — clicks/hotkeys may *enqueue* intents/commands for the owner-thread Resolve/Commit path; HUD code must not call `World::set`, journal upserts, stable-ID minting, or save writers.
3. **No EnTT / no gameplay registry** — Presentation must not `#include <entt/…>` or hold `entt::entity`. Target actors/items by stable IDs already on the snapshot.
4. **No raylib types in world/ecs** — keep draw calls in `game` / `render_raylib`. Prefer backend-neutral draw lists if HUD moves out of `Game.cpp`.
5. **Oxygen / Health / Energy bars are not survival authority** — they display ECS vitals after the precomputed drain scalar was applied. Do not reimplement Barren drain or atmosphere seal logic in HUD.
6. **VOXEL / SUBVOXEL toggles** map to existing macro vs 16³ micro tools — do not invent a second edit pipeline from the inventory footer.
7. **Suspicion / claim floor** — display only; standing lives in campaign/`SuspicionState`, not in Present.

## Allowed Present inputs (examples)

- `PlayerSnapshot` (position, health, oxygen, energy, hunger)
- Inventory / hotbar DTOs with stable content IDs
- Atmosphere sample scalars already computed for HUD (pressure/O₂ %) — optional Present convenience, still not authoritative
- System Suspicion + claim floor floats from standing module
- Minimap: time, `x,y,z`, biome name from environment table

## Forbidden Present outputs

- Direct voxel/infra edits
- Raising Suspicion, changing claim state, spawning enemies
- Opening portals / cycling airlocks except via command buffer
- Planet-global queries or dense face buffers for minimap (use local/bounded reads)

## Integration

If HUD needs a new field, request a snapshot/DTO extension from Architecture / the owning simulation agent — do not reach into `SurfaceInfrastructure` or `EcsWorld` internals from Present.
