#!/usr/bin/env bash
# PathResolver enforcement gate (Phase 4 of the PathResolver architecture).
#
# Fails (exit non-zero) if any NON-TEST engine source file still calls one of
# the removed deprecated path-resolution shims:
#   resolve_path, normalize_ci_key, normalize_ci_full, find_file_ci
#
# This is a review gate, not a compile error: it catches future regressions
# where someone reintroduces a call to a deleted shim. Comments that merely
# mention the old names (e.g. in path_resolver.h's doc) are ignored, as are
# test sources (which are allowed to assert on the replacement behavior).
#
# Usage:
#   scripts/check_path_resolver_enforcement.sh
#
# Works both from the Core repo root (src/engine) and from the Workspace hub
# (projects/Core/src/engine).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Locate the engine tree: prefer Core-repo layout, fall back to workspace layout.
ENGINE_DIR="$ROOT/src/engine"
if [ ! -d "$ENGINE_DIR" ]; then
    ENGINE_DIR="$ROOT/projects/Core/src/engine"
fi
if [ ! -d "$ENGINE_DIR" ]; then
    echo "PathResolver enforcement: engine dir not found (looked in" >&2
    echo "  $ROOT/src/engine and $ROOT/projects/Core/src/engine)" >&2
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "PathResolver enforcement: python3 is required to run this gate" >&2
    exit 1
fi

python3 - "$ENGINE_DIR" <<'PY'
import sys, re, pathlib

engine_dir = pathlib.Path(sys.argv[1])
pattern = re.compile(r'\b(resolve_path|normalize_ci_key|normalize_ci_full|find_file_ci)\b')

# Directories / file-name patterns that are test code and exempt from the gate.
test_markers = ("test", "tests", "_test")

violations = 0
for path in engine_dir.rglob("*"):
    if path.suffix not in (".cpp", ".h"):
        continue
    parts = set(p.lower() for p in path.parts)
    if parts & set(test_markers):
        continue
    if "test" in path.name.lower() or path.name.lower().endswith("_test.cpp") \
       or path.name.lower().endswith("_test.h"):
        continue
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        continue
    for lineno, raw in enumerate(text.splitlines(), 1):
        # Drop any trailing line comment so doc references are not flagged.
        code = raw.split("//", 1)[0]
        stripped = code.strip()
        # Skip wholly-comment lines (//, /* ... */, doxygen '*' continuations).
        if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
            continue
        if pattern.search(code):
            print(f"VIOLATION: {path}:{lineno}: {raw.strip()}")
            violations += 1

if violations:
    print(f"\nPathResolver enforcement FAILED: {violations} violation(s) "
          f"found in engine/ (excluding tests).")
    sys.exit(1)
print("PathResolver enforcement OK: no deprecated shim calls in engine/.")
PY
