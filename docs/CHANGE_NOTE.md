# CHANGE_NOTE — CP seam: Barren O2 drain + SuspicionState

## Behavior

- **PlanetEnvironment table** is now authoritative for both planar World and cube-sphere PlanetSurface via planetEnvironmentFor(PlanetClass) (Barren drains 1.8/s, Temperate ambient 0, Scorched drains + hazard).
- **effectiveOxygenDrain(envBaseline, atmosphereSample)** (and bool sealed-breathable overload) owns the Sense-step blend policy: sealed breathable refills at -7/s; partial pressure/O2 reduces drain; open exterior applies baseline. ECS vitals still receive only the precomputed scalar.
- **SuspicionState** owns raise / decay-toward-claimFloor / read for system standing (no entt::entity, no dense maps). Mining/placement/extractor paths still emit through Game wiring; SurfaceSiegeDirector remains a consumer of the standing scalar.
- Headless coverage asserts shared Barren/Temperate baselines, sealed atmosphere reducing effective drain, activity raising suspicion, and claimFloor clamping decay.

## Files touched

| File | Change |
|---|---|
| src/world/PlanetEnvironment.hpp | new — PlanetEnvironment, planetEnvironmentFor, AtmosphereSupportSample, effectiveOxygenDrain |
| src/world/PlanetEnvironment.cpp | new — baseline table + blend policy |
| src/world/SurfaceSuspicion.hpp | new — SuspicionState |
| src/world/SurfaceSuspicion.cpp | new — raise / decay / claimFloor |
| src/world/World.hpp | include shared env; remove local PlanetEnvironment struct |
| src/world/World.cpp | environment() delegates to planetEnvironmentFor |
| src/world/PlanetSurface.hpp | expose environment() for cube-sphere |
| src/game/Game.hpp | SuspicionState suspicion_ |
| src/game/Game.cpp | Sense path uses effectiveOxygenDrain; producers/decay/save wire through SuspicionState |
| tests/headless_tests.cpp | testPlanetEnvironmentOxygenDrainAndSuspicionSeams |
| CMakeLists.txt | add PlanetEnvironment.cpp, SurfaceSuspicion.cpp to elysium_world |
| CHANGE_NOTE.md | this note |

## Constraints respected

- No EnTT outside ecs/
- No MachineType / schema renumbering
- Suspicion stays in Game/campaign path (not infrastructure journal)
- Workers unchanged; owner-thread commit only
- loadGame: after Suspicion setValue + claimed_ restore, call setClaimFloor(claimFloor()) once so the floor applies before the first tick.


---

See also: `CHANGE_NOTE_REGISTER_ACTION.md` + `REGISTER_ACTION_GAP.md` (per-system SuspicionLedger / Part 24 Empire gap).
