# v0.20 Dependency Audit (CP0) — Architecture / agent_00

Merge-facing. No code rewrite in this pass. Source: `CMakeLists.txt` + include graph under `src/`.

## 1. Current CMake target graph

```
elysium_core
    ↑
elysium_world ──────────────────────────────┐
    ↑                                       │
elysium_render_core ←───────────────────────┘
    ↑
elysium_render_raylib  (client; + raylib)
elysium_ecs            (client; PUBLIC core, PRIVATE EnTT)

elysium_game → core + world + ecs + render_core + render_raylib + raylib
elysium → game
elysium_headless_tests → render_core + world + core   (no EnTT / raylib)
```

**Observed edges / leaks (doc-only):**

| Finding | Severity | Notes |
|---|---|---|
| No `world` → `ecs` / `render` / `game` includes | OK | No hard back-edges today |
| `<entt/entt.hpp>` only in `src/ecs/EcsWorld.cpp` | OK | Public `EcsWorld.hpp` stays EnTT-free |
| All libs use `PUBLIC src` | Risk | CMake cannot stop include-direction violations |
| Monolithic `elysium_world` | Risk | Industry, siege, nav, mesher, persistence share one target |
| `GraphicsBackend.hpp` → `world/VoxelMesher.hpp` → `World.hpp` | Soft leak | render_core tied to planar `World` via mesh types |
| `BaseInfrastructure.hpp` → `World.hpp` | Soft leak | Spherical infra pulls planar world header |
| `PlanetSurfaceMesher` lives in `world/` | Layering smell | CPU mesh builders belong beside render_core long-term |
| `Game.hpp` includes `<raylib.h>` + `RaylibGraphicsBackend` | Client leak | Composition root OK; do not copy this pattern into world/ecs |
| `InfrastructureJournal.cpp` → `SurfaceIndustry.hpp` | Coupling | Persistence knows industry payload shapes (acceptable if schema-owned) |

No forbidden **link** back-edges found. The real hazard is the flat include root + world megatarget.

## 2. Allowed vs forbidden dependency directions

**Allowed (downstream → upstream only):**

| From | May depend on |
|---|---|
| `core` | STL / threads only |
| `world` (voxel, addresses, infra, industry, nav, siege, persist) | `core` |
| `ecs` | `core` (+ EnTT **private** impl only) |
| `render_core` | `core`, **narrow** world read/mesh DTOs (`CpuMeshData`, address/snapshot views) |
| `render_raylib` | `render_core`, raylib |
| `game` / client | composition root: any module |
| headless tests | `core`, `world`, `render_core` |

**Forbidden:**

- `world` → `ecs`, `render_*`, `game`, raylib, `<entt/…>`
- `ecs` public headers → world/render types (use callbacks / stable-ID command DTOs only)
- `render_core` → raylib / EnTT / `game`
- Any module → dense `faceResolution²×6` working storage or planet-global inventory scans
- Industry / logistics / siege **owning** `entt::entity`, GPU handles, or territorial standing
- New 12-edge cube-sphere seam tables (use direction-space + `CubeSphere::project`)
- Worker threads performing structural ECS mutation, stable-ID minting, world edits, or save publication

**Phase contract (all agents):** Sense/Gather → Plan → Resolve → Commit → Persist → Present. Workers compute from snapshots; owner thread commits.

## 3. Explicit do-not-touch

### MachineType numeric IDs
Locked in `BaseInfrastructure.hpp`. **Do not renumber 0–19.**

| ID | Type |
|---:|---|
| 0–8 | BurnerGenerator … LogicController |
| 9–15 | Furnace … CargoLoader |
| 16–19 | Crusher, ChemicalVat, Fabricator, Extractor |

Append-only for new machines. Item IDs `1000 + MachineType` inherit the same freeze. Manufactured items 3006–3020: treat as stable unless Architecture revises the content ID table.

### Persistence schemas
| Boundary | Locked value | Rule |
|---|---|---|
| Infrastructure journal | schema **v5** (readers v1–v5) | Append fields via new schema version; never rewrite in place |
| Global save manifest | schema **v8** + `save_generation` | Keep v1–v8 readers |
| Spherical touched-chunk sidecar | payload **v8** | Coworker may remux into edit-journal codec; semantics stay address-keyed |
| Generator | version `1`, fingerprint `ELYSPH01` | Old saves outrank new generator behavior |

Portal geometry stays voxel-edit journal; infrastructure records stay semantic/stable-ID only (no dense column index, no EnTT/GPU identity).

### EnTT include boundary
- **Only** `src/ecs/*` translation units may `#include <entt/entt.hpp>`.
- Public APIs expose stable IDs, snapshots, and command buffers — never `entt::entity` / `entt::registry`.
- Negative fixture target for future audit: world/render header pulling EnTT must fail CI.

## 4. Recommended seam — Barren O₂ drain + system Suspicion

Slice-critical gaps; place them without breaking the five rules.

### Barren oxygen drain
**Today:** `PlanetEnvironment::oxygenDrainPerSecond` on planar `World` (`Barren` = `1.8f`); `Game` blends room atmosphere support then calls `EcsWorld::updateVitals(drain, …)`. Spherical path uses the same Game glue.

**Recommended ownership:**
1. **Environment table (world / planet-class data)** — authoritative baseline drain/hazard keyed by `PlanetClass` (expose the same table for cube-sphere; do not leave Barren drain as Game-only magic). Sparse, no dense fields.
2. **Atmosphere query (world / `SurfaceInfrastructure`)** — Sense/Gather: sealed/breathable sample at player address (existing bounded flood-fill seal rule).
3. **Vitals apply (ecs)** — Commit: apply a **precomputed** `oxygenDrainPerSecond` scalar. ECS must not include planet/infra headers.
4. **Blend policy (narrow survival helper or game Sense step)** — `effectiveDrain = f(baselineEnv, atmosphereSample)`; not inside EnTT components and not inside industry.

Do **not** put Barren drain inside Register Action / siege, Network Storage, or renderer.

### System Suspicion ownership
**Today:** `Game::suspicion_` / `raiseSuspicion` / `claimFloor`; mining & placement call Game; industry emits `extractorSuspicionGenerated` telemetry that Game applies; `SurfaceSiegeDirector` reads Suspicion for dispatch.

**Recommended ownership:**
1. **Campaign / Empire standing module** (future agent_30 surface; prototype may live as `world/SurfaceSuspicion` or `campaign/` next to siege) owns system Suspicion + claim floor. Stable system/planet keys only — **no** `entt::entity`, **no** dense column maps.
2. **Producers emit activity events** — mining, construction, Extractor telemetry (`float` / tagged activity). Industry stays emit-only.
3. **Siege / Register Action** — consume standing + claim flags; schedule stable-ID spawn requests; never mutate standing as a side effect of ECS combat.
4. **Persist** — standing belongs in campaign/system save (not infrastructure journal schema v5). Do not overload machine sidecar records.

**Immediate next code workstream (suggested):**
1. Lift `PlanetEnvironment` (or equivalent) so spherical Barren/Scorched/Temperate share the planar baseline table.
2. Extract `effectiveOxygenDrain(env, atmosphereSample)` out of `Game.cpp` into a headless-testable helper.
3. Introduce a tiny `SuspicionState` (raise / decay-to-floor / claimFloor) fed by activity events + industry telemetry; leave `Game` as the temporary wiring until Empire agent lands.
4. Keep EnTT vitals and siege director as pure consumers of those scalars.

## Handoff
- File: `03_prototypes/v0.20/DEPENDENCY_AUDIT.md` (this document)
- Code changes: none
- Follow-up: CMake/include audit negative fixture (agent_00 owned); split world megatarget when a real back-edge appears

## 5. Present-phase / Presentation ownership (2026-09-07)

- Canonical art/HUD: `/workspace/elysium/01_specs/art_direction/ART_DIRECTION.md` (CONCEPT v0.7 GUI).
- Owner: Presentation bot — HUD parity only; Present consumes snapshots.
- **Forbidden:** UI/HUD/render Present code mutating world, minting stable IDs, writing saves, or including EnTT; world/ecs depending on Presentation, CONCEPT assets, or raylib HUD helpers.
- Oxygen/Health/Energy bars are Present of vitals snapshots — not alternate survival authority.
- VOXEL / SUBVOXEL toggles map to existing macro vs 16³ micro tools; do not fork a second edit path from UI.

## 6. Design-pillar gate (owner lock 2026-09-07)

Authority: `01_specs/DESIGN_SYNTHESIS.md` (+ ART_DIRECTION pillars).

- **DF-depth is critical** — do not approve designs that shallow fortress/civilization systems to make room for factory polish.
- **Factorio** informs industry/logistics clarity (local networks, throughput) — not a license to delete DF-roadmap agents.
- Alpha still stages **Part 24** first; DF-depth agents stay on the roadmap (behavioral LOD OK for remote detail; identity/history/systems remain).
