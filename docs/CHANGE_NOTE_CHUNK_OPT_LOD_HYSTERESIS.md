# CHANGE_NOTE_CHUNK_OPT_LOD_HYSTERESIS — P0-29 enter/exit bands

## Behavior

Streaming LOD promotions/demotions now use **Schmitt-trigger enter/exit bands** so a focus that oscillates near a budget edge does not flip-flop chunk tiers.

Owned by **`PlanetSurfaceRenderer` streaming focus** (`assignStreamingLodDetails` in `LodHysteresis.hpp`). `SurfaceChunkCache` is unchanged: retain-old-geometry, priority preemption, one-cell halo, and worker determinism are not on this path.

Score = `dot(chunkCenterDirection, focusDirection)` (1 = underfoot). Nominal **enter** cutoffs remain the Nth / (N+M)th ranked scores for `highDetailBudget` / `nearFieldBudget`.

| Transition | Enter | Exit (stay until) |
|---|---|---|
| → Full | rank `< N` | previous Full **and** (`score >= Nth − 0.03` **or** rank `< N + 1`) |
| → FieldNear | rank `< N+M` (and not Full) | previous Full/Near **and** (`score >= (N+M)th − 0.03` **or** rank `< N+M + 1`) |
| FieldFar | otherwise | — |

### Widths

| Constant | Value | Meaning |
|---|---|---|
| `kLodFullExitBand` | **0.03** cosine | Full demote band below the Nth score (~1.5–3° off-axis) |
| `kLodNearExitBand` | **0.03** cosine | FieldNear demote band below the (N+M)th score |
| `kLodHysteresisRankSlack` | **1** rank | Nth/(N+1)th pair cannot swap tiers on a one-rank jitter |
| `kLodFocusDirtyCosine` | **0.9995** | Separate dirty-bit deadzone on `setStreamingFocus` (~1.8°). Not an LOD band. |

First assignment after entering streaming, changing budgets, `setFullDetail()`, or `invalidate()` uses **enter thresholds only** so debug-Full residency and bubble resizes do not sticky. A move well outside the band still demotes (antipodal focus is not retained).

Chunks inside the Full exit band may briefly overshoot the Full budget (sticky Nth plus a new enter). Large focus / budget changes re-arm enter-only and restore exact N / M.

## Measurable step

- Zero-band ranking flip-flops the Nth/(N+1)th pair every oscillation; default bands keep the original Full (or FieldNear) for the whole in-band sequence (`testLodHysteresisDoesNotFlipFlopInBand`).
- Renderer wiring: two adjacent PositiveZ chunks, foci straddling their bisector past the 0.9995 deadzone but inside 0.03 cosine — Full does not demote (`testStreamingLodHysteresisNoFlipFlop`). Antipode still demotes.
- Existing three-tier budget, retain, preempt, halo, and 0/1/2/4/8 mesh tests stay green.

## Files touched

| File | Change |
|---|---|
| `src/render/LodHysteresis.hpp` | new — `SurfaceRenderDetail`, widths, `assignStreamingLodDetails` |
| `src/render/PlanetSurfaceRenderer.hpp` / `.cpp` | streaming focus uses hysteresis; `streamingTargetDetail` |
| `tests/headless_tests.cpp` | flip-flop + renderer in-band tests |
| `docs/CHUNK_OPT_GAP.md` | STREAMING section; P0-29 Present |
| `docs/CHANGE_NOTE_CHUNK_OPT_STREAM.md` | P0-28 ownership |
| `docs/CHANGE_NOTE_CHUNK_OPT_LOD_HYSTERESIS.md` | this note |

## Constraints respected

- No EnTT in `world/` (policy lives in `render/`).
- No MachineType / schema renumber.
- `SurfaceChunkCache` retain / preempt / halo one-cell untouched.
- Worker mesh jobs still consume immutable snapshots / cache packets; determinism 0/1/2/4/8 re-locked by existing greedy tests.

## Verification

`ctest` in `build_headless`: **PASS** (`elysium_headless` + `include_boundary`).
