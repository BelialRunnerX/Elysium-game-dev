# Elysium Game Dev

Canonical repository for **Elysium** — version control, design/library storage, and runtime builds.

**Owner tip:** Dylan / BelialRunnerX  
**Active prototype baseline:** `v0.20` (C++20 / EnTT / cube-sphere) — syncing into this repo.

## What this repo is for
- Source of truth for implementation (`src/`, CMake, tests)
- Design + library artifacts that should be versioned
- CI / headless test + (eventually) client runtime builds

## Design locks (do not regress)
- Synthesis: No Man's Sky × Minecraft × **Dwarf Fortress (depth critical)** × **Factorio (logistics inspiration)** × Diablo
- Art: CONCEPT v0.7 HUD/blocks; white/black/cyan weathered anime operatives
- Architecture: StableIds ≠ `entt::entity`; sparse cube-sphere; owner-thread commit; include-boundary CI

## Alpha target
Part 24 three-planet vertical slice.

## Build (headless)
```bash
cmake -S . -B build_headless -DELYSIUM_BUILD_CLIENT=OFF -DELYSIUM_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build_headless -j
ctest --test-dir build_headless --output-on-failure
```

## Related bots
Chief of Staff coordinates; Architecture / Presentation / Librarian specialists operate against this tip.
