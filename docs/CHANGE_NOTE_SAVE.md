# CHANGE_NOTE_SAVE — Part 24 Save pillar seam

## Behavior

- Campaign save remains **`ELYSIUM_SAVE 8`** (no schema v9).
- After spherical `surface_manifest` blocks and **before `END`**, the writer emits an append-only Empire section:

```text
siege_count <N>
siege <planetIndex>
ELYSIUM_SURFACE_SIEGE 1
<director serializeState payload>
```

- Only non-idle directors are emitted (`phase == Idle && actionId == 0` omitted); order is ascending `planetIndex`.
- Loader accepts an optional `siege_count` section before `END`; absence leaves freshly `initWorlds()` directors idle.
- After `ecs_.clearEnemies()`, `reconcileCampaignSiegeAfterRestore` runs per planet so WaveActive blobs without live ECS actors advance via existing director rules (known v1 limitation — no Imperial actor rehydrate yet).
- Multi-system `suspicion_ledger_*` is **not** implemented this pass (still single `state` float for active system).

## Files touched

| File | Change |
|---|---|
| `src/world/SurfaceSiege.hpp` | Campaign embed eligibility + append/restore/reconcile API |
| `src/world/SurfaceSiege.cpp` | Embed codec implementation |
| `src/game/Game.cpp` | `saveGame` append embed; `loadGame` optional `siege_count` + post-clear reconcile |
| `tests/headless_tests.cpp` | `testCampaignSiegeSaveEmbedRoundTrip` (mid-Announced + sparse + WaveActive reconcile) |
| `SAVE_GAP.md` | Present/Partial/Missing inventory + status |
| `CHANGE_NOTE_SAVE.md` | this note |
| `CAMPAIGN_SIEGE_SAVE_EMBED.md` | design authority (unchanged contract) |

## Constraints respected

- No schema v9; append-only inside v8 per Architecture sketch
- No Presentation / ShipTransit / mining-recipe edits
- No MachineType / item-ID renumber
- No EnTT outside `ecs/`; siege embed uses stable IDs only
- No DF-depth shallowing — director codec unchanged; campaign only embeds it
- **ECS migration (owner lock):** N/A — no new ECS system. No component added to preexisting entities. Siege restore does not spawn actors; WaveActive IDs are reconciled as missing after `clearEnemies`.

## Save pillar status

**Minimal Part 24 Save path: Present** (changed chunks + base objects under v8, plus active siege embed). Suspicion ledger multi-slot save and ship hull save remain Missing — see `SAVE_GAP.md`.
