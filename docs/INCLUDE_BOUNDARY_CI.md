# Include-boundary CI sketch (agent_00) — cheapest headless gate

**Status:** script landed at `scripts/check_include_boundaries.sh`; CTest `include_boundary` wired.

Goal: fail fast if `world` / `render_core` (or `core`) headers pull EnTT or raylib. Grep-only; no compile matrix required.

## What is allowed today (baseline)

| Include | Allowed locations |
|---|---|
| `<entt/entt.hpp>` | `src/ecs/*.cpp` only (currently `EcsWorld.cpp`) |
| `<raylib.h>` / raylib types | `src/render/RaylibGraphicsBackend.*`, `src/game/*`, `src/main.cpp` |

**Must stay clean:** `src/core/**`, `src/world/**`, and `src/render/` except `RaylibGraphicsBackend.*`.

`GraphicsBackend.hpp` / `WorldRenderer.*` / `PlanetSurfaceRenderer.*` are headless `render_core` — they must not see raylib or EnTT.

## Cheapest check (shell)

Drop as `scripts/check_include_boundaries.sh` (or a CTest that runs it). Exit nonzero on any hit.

```bash
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src"
fail=0

# EnTT: forbidden outside ecs translation units (headers anywhere under ecs also banned —
# public EcsWorld.hpp must stay EnTT-free).
while IFS= read -r f; do
  echo "FAIL EnTT leak: $f"
  fail=1
done < <(rg -l '#include\s*[<"]entt/' "$SRC/core" "$SRC/world" "$SRC/render" "$SRC/game" 2>/dev/null || true)
while IFS= read -r f; do
  echo "FAIL EnTT in ecs header (must stay impl-only): $f"
  fail=1
done < <(rg -l '#include\s*[<"]entt/' "$SRC/ecs" --glob '*.hpp' 2>/dev/null || true)

# raylib: forbidden in core/world and in non-backend render headers/sources
while IFS= read -r f; do
  echo "FAIL raylib leak: $f"
  fail=1
done < <(rg -l '#include\s*[<"]raylib\.h[>"]|#include\s*[<"]rlgl\.h[>"]' \
  "$SRC/core" "$SRC/world" 2>/dev/null || true)
while IFS= read -r f; do
  case "$(basename "$f")" in
    RaylibGraphicsBackend.hpp|RaylibGraphicsBackend.cpp) ;;
    *) echo "FAIL raylib in render_core path: $f"; fail=1 ;;
  esac
done < <(rg -l '#include\s*[<"]raylib\.h[>"]|#include\s*[<"]rlgl\.h[>"]' \
  "$SRC/render" 2>/dev/null || true)

# Optional soft checks (warn or fail once fixtures exist):
# - world/render must not #include "ecs/..."
# - world must not #include "render/..." or "game/..."
# - public APIs must not mention entt::entity

if [[ "$fail" -ne 0 ]]; then
  echo "include-boundary check FAILED"
  exit 1
fi
echo "include-boundary check OK"
```

## CMake / CTest hook (sketch)

```cmake
add_test(
  NAME include_boundary
  COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_include_boundaries.sh")
set_tests_properties(include_boundary PROPERTIES LABELS "architecture;headless")
```

Headless suite already links `elysium_render_core` without raylib/EnTT — keep that true. Do **not** FetchContent EnTT/raylib for the headless target.

## Negative fixture (when implementing)

Add `tests/architecture/bad_entt_in_world.hpp` that `#include <entt/entt.hpp>` and a test mode that greps a known-bad path, **or** keep the negative case as a checked-in comment in this doc and a CI job that asserts the script catches a temp file. Prefer the script staying dependency-free.

## Out of scope for this sketch

Splitting `elysium_world`, Present-phase HUD rules (see `PRESENT_PHASE_CONSTRAINTS.md`), MachineType/schema freezes.
