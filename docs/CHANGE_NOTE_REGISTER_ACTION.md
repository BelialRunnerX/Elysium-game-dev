# CHANGE_NOTE_REGISTER_ACTION — Per-system SuspicionLedger

## Behavior

- **SuspicionLedger** owns sparse Empire standing keyed by stable `uint64_t` system id (no `entt::entity`, no dense maps).
- Existing **SuspicionState** slot API unchanged (raise / decay / claimFloor / reduceTowardFloor).
- **Game** routes all standing through `suspicionLedger_.system(galaxySeed_)` so the Alpha three-planet star system shares one standing slot, while other system ids remain isolated.
- Headless tests lock: A≠B isolation, `decayAll`, and claim-pushed Marked standing → Announced three-wave Register Action (unclaimed stays Patrol).

## Files touched

| File | Change |
|---|---|
| src/world/SurfaceSuspicion.hpp | SuspicionLedger API |
| src/world/SurfaceSuspicion.cpp | ledger impl |
| src/game/Game.hpp | SuspicionLedger + suspicion() helper |
| src/game/Game.cpp | wire producers/dispatch/save through helper |
| tests/headless_tests.cpp | testSuspicionLedgerPerSystemIsolationAndClaimPushAnnouncement |
| REGISTER_ACTION_GAP.md | Present/Partial/Missing inventory |
| CHANGE_NOTE_REGISTER_ACTION.md | this note |

## Constraints respected

- No EnTT outside ecs/
- No MachineType / schema renumbering
- Standing stays campaign/system path (not infrastructure journal)
- SurfaceSiegeDirector remains a pure consumer of standing scalars
- O₂ / prior SuspicionState seam left intact

## Empire pillar

Minimal Part 24 Empire path: **Present** (per-system Suspicion + dispatch + announced Register Action on claimed Marked/Hunted). See REGISTER_ACTION_GAP.md.
