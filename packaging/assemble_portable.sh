#!/usr/bin/env bash
# packaging/assemble_portable.sh
# Assembles a cross-platform portable distribution from per-platform CI artifacts.
#
# Usage:
#   ./assemble_portable.sh <linux_artifact_dir> <windows_artifact_dir> <output_dir>
#
# The output directory contains the §3 portable layout:
#   GameModManager-Portable/
#     gamemodmanager          (Linux ELF)
#     gamemodmanager.exe      (Windows PE)
#     runtime/linux/          (Linux Qt .so's + CPython)
#     runtime/windows/        (Windows Qt .dll's + VC++ runtime)
#     plugins/linux/          (*.so from Linux build)
#     plugins/windows/        (*.dll from Windows build)
#     tools/linux/            (bundled external tools: wrestool, etc.)
#     instance.toml
#     config/
#     mods/
#     downloads/
#     cache/
#     logs/
#
# This script only copies what's available — if only a Linux artifact is
# provided, the output is a single-platform portable. The receiving machine
# runs whichever binary matches its OS.

set -euo pipefail

LINUX_DIR="${1:-}"
WINDOWS_DIR="${2:-}"
OUTPUT_DIR="${3:-GameModManager-Portable}"

if [ -z "$LINUX_DIR" ] && [ -z "$WINDOWS_DIR" ]; then
    echo "Usage: $0 <linux_artifact_dir> <windows_artifact_dir> <output_dir>"
    echo "  At least one platform artifact directory must be provided."
    exit 1
fi

echo "Assembling portable distribution → $OUTPUT_DIR"

# Create directory structure
mkdir -p "$OUTPUT_DIR"/{runtime/{linux,windows},plugins/{linux,windows},tools/linux,config,mods,downloads,cache,logs}

# -- Linux artifacts --
if [ -n "$LINUX_DIR" ] && [ -d "$LINUX_DIR" ]; then
    echo "  Packaging Linux artifacts from $LINUX_DIR"

    # Binary
    if [ -f "$LINUX_DIR/gamemodmanager" ]; then
        cp "$LINUX_DIR/gamemodmanager" "$OUTPUT_DIR/gamemodmanager"
        chmod +x "$OUTPUT_DIR/gamemodmanager"
    fi

    # Runtime libraries (Qt .so's, CPython, etc.)
    if [ -d "$LINUX_DIR/runtime/linux" ]; then
        cp -a "$LINUX_DIR/runtime/linux/"* "$OUTPUT_DIR/runtime/linux/" 2>/dev/null || true
    fi

    # Plugins
    for f in "$LINUX_DIR"/plugins/*.so; do
        [ -f "$f" ] && cp "$f" "$OUTPUT_DIR/plugins/linux/"
    done

    # External tools (wrestool for .exe icon extraction)
    if [ -f "$LINUX_DIR/tools/linux/wrestool" ]; then
        cp "$LINUX_DIR/tools/linux/wrestool" "$OUTPUT_DIR/tools/linux/"
        chmod +x "$OUTPUT_DIR/tools/linux/wrestool"
    elif command -v wrestool &>/dev/null; then
        cp "$(command -v wrestool)" "$OUTPUT_DIR/tools/linux/"
        chmod +x "$OUTPUT_DIR/tools/linux/wrestool"
    fi
fi

# -- Windows artifacts --
if [ -n "$WINDOWS_DIR" ] && [ -d "$WINDOWS_DIR" ]; then
    echo "  Packaging Windows artifacts from $WINDOWS_DIR"

    # Binary
    if [ -f "$WINDOWS_DIR/gamemodmanager.exe" ]; then
        cp "$WINDOWS_DIR/gamemodmanager.exe" "$OUTPUT_DIR/gamemodmanager.exe"
    fi

    # Runtime libraries (Qt .dll's, VC++ runtime, CPython)
    if [ -d "$WINDOWS_DIR/runtime/windows" ]; then
        cp -a "$WINDOWS_DIR/runtime/windows/"* "$OUTPUT_DIR/runtime/windows/" 2>/dev/null || true
    fi

    # Also copy DLLs that sit beside the exe
    for f in "$WINDOWS_DIR"/*.dll; do
        [ -f "$f" ] && cp "$f" "$OUTPUT_DIR/runtime/windows/"
    done

    # Plugins
    for f in "$WINDOWS_DIR"/plugins/*.dll; do
        [ -f "$f" ] && cp "$f" "$OUTPUT_DIR/plugins/windows/"
    done
fi

# -- Shared files (identical on both platforms) --
cat > "$OUTPUT_DIR/instance.toml" <<EOF
# GameModManager portable instance
# This file is shared across platforms — both binaries read it.
game_id = ""
game_name = ""
created = ""
cross_platform = $([ -n "$LINUX_DIR" ] && [ -n "$WINDOWS_DIR" ] && echo "true" || echo "false")
EOF

echo "Portable distribution assembled: $OUTPUT_DIR"
echo "  Linux:   $([ -f "$OUTPUT_DIR/gamemodmanager" ] && echo "yes" || echo "no")"
echo "  Windows: $([ -f "$OUTPUT_DIR/gamemodmanager.exe" ] && echo "yes" || echo "no")"
