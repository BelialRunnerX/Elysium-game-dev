# REGISTER_ACTION_GAP — Part 24 Empire pillar (v0.20)

Honest inventory after the O₂/SuspicionState seam. Source criteria: Part 24 / ALPHA CP8 —

> **Empire:** Per-system Suspicion and dispatch; announced register action if claim is pushed.

Alpha note: `What_Is_Built.pdf` may still say combat/Empire incomplete while README claims siege exists. **This tree’s source of truth for Empire dispatch is the code below** (not the PDF).

---

## 1. Inventory (code + tests today)

### SurfaceSiegeDirector (`src/world/SurfaceSiege.{hpp,cpp}`)
| Capability | Status | Notes |
|---|---|---|
| Attention bands Quiet/Noted/Marked/Hunted (25/50/75 tuning) | **Present** | `bandFor`; headless locked |
| Dispatch → Patrol (unclaimed / Noted) | **Present** | `requestEnforcement` |
| Dispatch → Register Action (claimed + Marked/Hunted) | **Present** | 3 / 5 waves; Praetor finale |
| Announced prep window (~10 min prod; injectable in tests) | **Present** | Phase `Announced` before first wave |
| Stable-ID spawn requests + reconcile / destroy | **Present** | No `entt::entity` |
| Beacon loss → Failed (no voxel erase) | **Present** | `notifyBeaconDestroyed` / `beaconIntact` |
| Cleared / Failed terminal + clear | **Present** | |
| Narrow serialize/restore codec v1 | **Present** | Not yet embedded in campaign save |
| Tests | **Present** | `testSurfaceRegisterActionDirector` |

### SuspicionState / SuspicionLedger (`src/world/SurfaceSuspicion.{hpp,cpp}`)
| Capability | Status | Notes |
|---|---|---|
| raise / decay-to-floor / claimFloor / reduceTowardFloor | **Present** | Seam from prior CHANGE_NOTE |
| **Per-system map keyed by stable system id** | **Present** (this pass) | `SuspicionLedger`; sparse `unordered_map` |
| Isolation across system ids | **Present** (this pass) | Headless: raise A ≠ B |
| Persist standing in campaign journal | **Partial** | Game save still one float for active system |
| Tests | **Present** | Seam test + `testSuspicionLedgerPerSystemIsolationAndClaimPushAnnouncement` |

### Registry Beacon / claim (Game + BlockType)
| Capability | Status | Notes |
|---|---|---|
| Placeable Registry Beacon block | **Present** | Hotbar / starter kit |
| Place → planet `claimed_[i]=true` + refresh claim floor | **Present** | Planar + spherical |
| Mine beacon → claim released; edits remain | **Present** | Message + `claimed_=false` |
| Structural claim floor from placed matter | **Present** | `Game::claimFloor()` |
| Director `beaconIntact` distinct from claim flag | **Partial** | Game currently passes `claimed_` for both args |
| Beacon as stable machine object (not block-only) | **Missing** | DESIGN_MAPPING pending |
| Tests (beacon ↔ director fail) | **Present** | Director unit test; no full Game fixture |

### Game Empire wiring (`Game::updateEmpire`)
| Capability | Status | Notes |
|---|---|---|
| Suspicion producers (mine/place/extractor telemetry) | **Present** | Via `raiseSuspicion` → ledger slot |
| Periodic dispatch roll when standing ≥ 25 | **Present** | Deterministic `hash01` |
| Announcement / wave HUD messages | **Present** | Client-only Present |
| Spawn Imperial archetypes via ECS command buffer | **Present** | Drone/Lictor/Adept/Praetor roles |
| Active siege embedded in save | **Missing** | Director codec exists; campaign save omits it |
| Per-planet siege directors | **Present** | Array sized to `PlanetCount` |
| Standing keyed by system (galaxy seed) | **Present** (this pass) | `suspicionLedger_.system(galaxySeed_)` |

---

## 2. Present / Partial / Missing vs Part 24 Empire criteria

| Criterion | Verdict | Evidence |
|---|---|---|
| Per-system Suspicion | **Present** (minimal) | `SuspicionLedger` keyed by `uint64_t` system id; Alpha uses `galaxySeed_` |
| Dispatch | **Present** | Game roll + `SurfaceSiegeDirector::requestEnforcement` |
| Announced register action if claim is pushed | **Present** (minimal) | Claimed + Marked/Hunted → `Announced` Register Action; headless claim-push fixture locks it. Push itself is filing a beacon while standing is already in-band (or raised by industry/mining); there is no separate “instant announce on place” bypass of the dispatch roll in Game — the **portable** seam is director `requestEnforcement(standing, claimed=true)`. |
| Beacon-loss fails claim without deleting edits | **Present** (director + Game claim flag) | Director never touches voxels; Game clears claim only |
| Multi-system campaign persistence of all slots | **Partial / Missing** | Single active-system float in save |
| Court / Favor content / customs | **Missing** | Out of Alpha (post-slice) |
| Full siege AI targeting beacon/turrets | **Missing** | REGISTER_ACTIONS.md next-work |

**Stale DESIGN_MAPPING note:** Part 14 “Pending → full register action siege state machine” is **obsolete** — the director state machine is Present. Pending that remains accurate: Court, Favor content, beacon-as-machine, campaign embed of siege state. Per-system map is no longer pending after this pass.

---

## 3. Empire pillar status (clear statement)

**Status: Minimal Part 24 Empire demo path is sufficient in this tree**, with the per-system ledger now locking CP8 isolation at the world layer.

Playable loop already supported by code:

1. Activity / Extractor telemetry raises standing on the active system id.
2. Registry Beacon files a claim.
3. Dispatch roll (or headless `requestEnforcement`) at Marked+ with claim → **Announced** Register Action → waves via stable IDs.
4. Beacon loss fails the action without erasing constructions.

**Not Done for full CP8 polish:** campaign save of multi-system ledger + active siege blob; distinct beaconIntact probe; Court. Those are follow-ups, not blockers for a scripted Alpha Empire beat.

`What_Is_Built` “Empire blocked / Suspicion read by nothing” is **false for v0.20 tip** — consumers are `Game::updateEmpire` + `SurfaceSiegeDirector`.

---

## 4. This pass change

- Added `SuspicionLedger` (stable system id → `SuspicionState`).
- Wired `Game` standing through `suspicionLedger_.system(galaxySeed_)`.
- Headless: isolation + claim-pushed Announced Register Action fixture.
- No MachineType renumber, no schema break, no EnTT outside `ecs/`, no DF-depth shallowing.
