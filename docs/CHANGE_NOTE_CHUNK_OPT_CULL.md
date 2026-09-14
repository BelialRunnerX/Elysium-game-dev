# CHANGE_NOTE_CHUNK_OPT_CULL — P0-24 frustum + P0-25 planet-horizon

## Behavior

`PlanetSurfaceRenderer::draw` now rejects cube-sphere chunks from the **draw list** using compact world-space bounds **before** `IGraphicsBackend::drawMesh`. Meshing, GPU publication, retain-old-geometry, halo, worker jobs, and P0-29 LOD hysteresis are unchanged.

| Item | Policy |
|---|---|
| **P0-24** | Perspective frustum vs padded AABB (sphere is a fast reject that contains the AABB). |
| **P0-25** | Geometric horizon vs an **inner-shell** occluder (`referenceRadius − ReferenceRadial − 0.5`). A bound is `BehindHorizon` only when the center **and** all 8 AABB corners are hidden by a segment–sphere test. Camera-in-bound and camera-inside-occluder skip horizon. |

No `setView` ⇒ previous draw-all behaviour (headless tests, debug Full). `Game::drawSphericalSurface` supplies the play camera (75° vFOV, window aspect, raylib-like 0.01/1000 clip).

Orbital climate/cloud shells are a separate two-mesh path and are not chunk-culled.

## Conservative / no-pop

- Bounds are a 3×3×3 UVR sample of the chunk prism (captures cube-sphere bulge) plus `kChunkBoundPad` (1.25 m) for micro/numeric slack. Field skirts pull inward.
- Horizon uses the **inner voxel shell**, not mean surface radius, so limb hills cannot pop.
- A limb-straddling AABB with any visible corner stays `Visible` even if the center is occluded.

## Measurable step

- Isolated frustum: look-axis sphere/AABB kept; behind-camera and far-right rejected.
- Isolated horizon: antipode hidden, near-side kept; far-face chunk `BehindHorizon` with frustum disabled.
- Renderer: look-through-planet 170° view draws the classified visible set, horizon-culls the antipodal face, and does not destroy GPU meshes. Narrow frustum-only pass splits the 24-chunk set. `clearView` restores draw-all. Streaming 6/9 residency is unchanged after a culled draw.

## Files touched

| File | Change |
|---|---|
| `src/render/ChunkViewCull.hpp` / `.cpp` | bounds, frustum planes, horizon, `classifyChunkBound` |
| `src/render/PlanetSurfaceRenderer.hpp` / `.cpp` | `setView` / `clearView`; draw-list cull; bound cache |
| `src/game/Game.cpp` | spherical surface camera → `setView` |
| `CMakeLists.txt` | `ChunkViewCull.cpp` in `elysium_render_core` |
| `tests/headless_tests.cpp` | `testChunkFrustumAndHorizonCull` |
| `docs/CHUNK_OPT_GAP.md` | DRAW / CULL section |
| `docs/CHANGE_NOTE_CHUNK_OPT_CULL.md` | this note |

## Constraints respected

- No EnTT in `world/` (cull lives in `render_core`).
- No MachineType / schema renumber.
- `SurfaceChunkCache` retain / preempt / halo one-cell untouched.
- Worker mesh jobs still consume immutable snapshots / cache packets; determinism 0/1/2/4/8 re-locked by existing greedy tests.
- P0-29 hysteresis ranking is independent of the draw cull.

## Verification

`ctest` in `build_headless`: **PASS** (`elysium_headless` + `include_boundary`).
