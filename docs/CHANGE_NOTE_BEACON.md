# CHANGE_NOTE_BEACON — Distinct beaconIntact + stable beacon id

## Behavior

- **`RegistryBeaconAuthority`** (world layer) owns per-planet claim slots with **distinct** `claimed` vs `beaconIntact`, plus a non-zero **stable beacon id** allocated on file (`mix64`, no EnTT).
- **`fileClaim(planet, domain)`** sets both flags and keeps the same stable id on re-file while intact.
- **`destroyBeacon(planet)`** clears claim + intact + id (structures/edits remain world responsibility). Game also calls `surfaceSiege().notifyBeaconDestroyed()`.
- **`Game::updateEmpire`** passes `registryBeacons_.claimed(i)` and `registryBeacons_.beaconIntact(i)` separately into `SurfaceSiegeDirector::update` (no longer aliases one bool for both).
- **Schema 8 unchanged:** save still writes three claimed bools. Load reseats `RegistryBeaconAuthority(galaxySeed_)` then **`restoreClaimedFlags`** migrates preexisting filed planets to intact objectives with allocated stable ids.

## ECS migration (ECS_MIGRATION_RULE.md)

**N/A this pass — no new ECS system / components.** Beacon remains `BlockType::RegistryBeacon` under world authority.

When a later pass appends a MachineType or ECS beacon component:

1. Add components to **preexisting** claim entities / archetypes / save rehydration (defaults OK).
2. Map existing `RegistryBeaconSlot::stableBeaconId` onto the machine/ECS object rather than minting a second identity.
3. Cover with a fixture that restores schema-8 claimed flags (or prior slots) *before* system attach and asserts participation.
4. Document that migration in the new CHANGE_NOTE.

## Files touched

| File | Change |
|---|---|
| `src/world/RegistryBeaconClaim.hpp` | new — slot + authority API |
| `src/world/RegistryBeaconClaim.cpp` | new — file/destroy/restore/migrate |
| `src/game/Game.hpp` | `registryBeacons_` replaces `claimed_[]` |
| `src/game/Game.cpp` | place/mine/empire/HUD/save-load wiring |
| `tests/headless_tests.cpp` | `testRegistryBeaconClaimDistinctIntactAndStableId` |
| `CMakeLists.txt` | add `RegistryBeaconClaim.cpp` to `elysium_world` |
| `BEACON_GAP.md` | Present/Partial/Missing inventory |
| `REGISTER_ACTION_GAP.md` | Partial → Present for distinct probe |
| `CHANGE_NOTE_BEACON.md` | this note |

## Constraints respected

- No MachineType renumber (and no append this pass)
- No ShipTransit / industry recipes / Presentation HUD edits
- No EnTT outside `ecs/`
- No schema bump
- Include boundaries: world module free of raylib/EnTT/game
- ECS preexisting-entity rule documented for future machine migration

## Beacon / Empire status

Distinct `beaconIntact` vs claim + minimal stable identity: **Present**. Full beacon-as-machine remains **Missing** — see `BEACON_GAP.md`.
