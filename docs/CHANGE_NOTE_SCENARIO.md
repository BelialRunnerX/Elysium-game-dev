# CHANGE_NOTE_SCENARIO — Three-planet hazardous pressure (lane H)

## Behavior

- **Vertical-slice roster** remains Temperate (0) / Barren (1) / Scorched (2) via existing `PlanetClass`, Game `initWorlds`, and `ShipTransit::classForVerticalSliceIndex` (phase graph untouched).
- **`PlanetEnvironment` baselines** already differ meaningfully and stay authoritative for planar + cube-sphere:
  - Temperate: O₂ `0/s`, hazard `0`
  - Barren: O₂ `1.8/s` (vacuum), hazard `0`
  - Scorched: O₂ `0.65/s`, hazard `0.7/s` (hazardous pressure)
- **Open exterior:** `effectiveOxygenDrain` + suit-meter integration lock that Barren drains O₂ over 10s while Temperate does not; Scorched drains some O₂ and damages health via ambient hazard.
- **Hazardous pressure:** Scorched ambient hazard locked; planar `World::localHazardAt` near Magma rises above ambient (Temperate/Barren local hazard stay ~0).

## Files touched

| File | Change |
|---|---|
| `tests/headless_tests.cpp` | `testThreePlanetScenarioOpenExteriorPressure` (+ `<algorithm>` for `std::clamp`) |
| `SCENARIO_GAP.md` | Present/Partial/Missing inventory + status |
| `CHANGE_NOTE_SCENARIO.md` | this note |

No `PlanetEnvironment.cpp` retune (values already correct). No ShipTransit phase edits. No Presentation edits.

## ECS migration (owner lock)

**N/A — no new ECS system or component.** Scenario pressure remains world-layer (`PlanetEnvironment` / `effectiveOxygenDrain` / `World::localHazardAt`). Game continues to feed precomputed scalars into existing `EcsWorld::updateVitals`; headless avoids EnTT by integrating the same scalars. No preexisting-entity component migration required (`ECS_MIGRATION_RULE.md`).

## Constraints respected

- Do not edit ShipTransit phase graph (planet list constants unchanged)
- Do not touch Presentation
- No EnTT outside `ecs/`; headless stays dependency-free of EnTT
- No MachineType / schema renumber
- Include boundaries unchanged

## Scenario pillar status

**Minimal three-planet pressure scenario: Present** (Temperate safe / Barren vacuum O₂ / Scorched hazardous). See `SCENARIO_GAP.md`.
