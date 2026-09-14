# FIX_LOG

## 2026-09-14T12:05:34Z (07:05 CT) — headless hang / procedural micro cost
- Hung/slow: `elysium_headless` (Timeout 90s); hot tests worker-mesh determinism,
  procedural/halo meshing, World occupancy clear thrash.
- Root: densify-missing micro greedy (~6× probes), occupancy 16³ on solid macros,
  per-cell World clear O(n²)+exterior refresh.
- Fix: sampleMicroBrick(Local), dense AO, occupancy short-circuit, fillChunk,
  Barren 2-chunk worker fixture. Halo + micro refine correctness kept.
- `ctest --timeout 90`: 2/2 PASS in ~26s. Note: `CHANGE_NOTE_FIX_HEADLESS_HANG.md`.

## 2026-09-14T11:28:00Z (06:28 CT) — halo mesh + greedy fixture
- Root cause: `SurfaceChunkData::microGet` used Air for cross-face halo macros → cached LOD0 diverged from snapshot.
- Fix: `microGetLocal` / `localCoordsFor`; mesher + occupancy use local path.
- Also: greedy test asserts prism/micro *deltas*; prior micro-refine case verified green.
- `ctest` 2/2 PASS (`elysium_headless`, `include_boundary`).
- Notes: `CHANGE_NOTE_FIX_HALO_MESH.md`, `CHANGE_NOTE_SUBVOXEL_SHAPE.md` follow-up.


## 2026-09-07T11:15:03Z (06:15 CT) — idle / green
- Watched for `pipeline/VALIDATE_FAILURES.md` (~2 min).
- File present: "No current failures — last cycles PASS."
- Rebuild `build_headless` + `ctest`: **PASS** (elysium_headless, include_boundary); 0 failed / 2.
- No code changes. FIX stage idle.
