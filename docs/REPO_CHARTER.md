# Repository charter — Elysium-game-dev

## Role
This GitHub repo is the **canonical** place for Elysium version management, durable file storage, and runtime/CI builds.

## Layout (target)
```
/
  README.md
  CMakeLists.txt
  src/
  tests/
  docs/
  scripts/
  .github/workflows/
```

## Rules
1. Do not renumber MachineType 0–19 or break save/sidecar schemas without Architecture approval.
2. Large Condensed Library zips: prefer GitHub Releases or LFS — not unbounded main-branch blobs.
3. DF-depth systems stay in scope; Factorio informs logistics clarity only.
4. Presentation Present-phase is snapshot-only.
