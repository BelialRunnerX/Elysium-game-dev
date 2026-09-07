#!/usr/bin/env bash
# Architecture include-boundary gate (agent_00). Grep-only; no compile required.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src"
fail=0

if ! command -v rg >/dev/null 2>&1; then
  echo "FAIL: ripgrep (rg) required for include-boundary check"
  exit 1
fi

# EnTT: forbidden outside ecs translation units. Public ecs headers must stay EnTT-free.
while IFS= read -r f; do
  echo "FAIL EnTT leak: $f"
  fail=1
done < <(rg -l '#include\s*[<"]entt/' "$SRC/core" "$SRC/world" "$SRC/render" "$SRC/game" 2>/dev/null || true)
while IFS= read -r f; do
  echo "FAIL EnTT in ecs header (must stay impl-only): $f"
  fail=1
done < <(rg -l '#include\s*[<"]entt/' "$SRC/ecs" --glob '*.hpp' 2>/dev/null || true)

# raylib: forbidden in core/world and in non-backend render paths
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

# Soft hard-fails already useful: module back-edges by include
while IFS= read -r f; do
  echo "FAIL world/render must not include ecs/: $f"
  fail=1
done < <(rg -l '#include\s*"ecs/' "$SRC/world" "$SRC/render" "$SRC/core" 2>/dev/null || true)
while IFS= read -r f; do
  echo "FAIL world must not include render/ or game/: $f"
  fail=1
done < <(rg -l '#include\s*"(render|game)/' "$SRC/world" "$SRC/core" 2>/dev/null || true)

if [[ "$fail" -ne 0 ]]; then
  echo "include-boundary check FAILED"
  exit 1
fi
echo "include-boundary check OK"
