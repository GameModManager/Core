#!/usr/bin/env bash
# build-linux.sh — one-shot local build for Linux
# Mirrors the CI runner (.github/workflows/build-linux.yml).
#
# Usage:
#   ./build-linux.sh                      configure + build (Release; tests off)
#   ./build-linux.sh --tests              also run ctest after the build
#   ./build-linux.sh --clean              rm -rf build/ first — from-scratch build
#   ./build-linux.sh --debug              CMAKE_BUILD_TYPE=Debug
#   ./build-linux.sh --no-loot            -DGMM_WITH_LOOT=OFF (no cargo/rust needed)
#   ./build-linux.sh --update-submodules  git submodule update --init --recursive --remote
#   ./build-linux.sh --skip-check         skip the dependency-presence check
#   ./build-linux.sh --help               show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ ! -f CMakeLists.txt ]; then
    echo "Error: no CMakeLists.txt in $SCRIPT_DIR — run this from the Core repo root." >&2
    exit 1
fi

BUILD_DIR="build"
BUILD_TYPE="Release"
RUN_TESTS=0
CLEAN_FIRST=0
GMM_WITH_LOOT="ON"
SUBMODULE_REMOTE=0
SKIP_CHECK=0

for arg in "$@"; do
    case "$arg" in
        --clean)             CLEAN_FIRST=1 ;;
        --debug)             BUILD_TYPE="Debug" ;;
        --tests)             RUN_TESTS=1 ;;
        --no-loot)           GMM_WITH_LOOT="OFF" ;;
        --update-submodules) SUBMODULE_REMOTE=1 ;;
        --skip-check)        SKIP_CHECK=1 ;;
        --help|-h)           grep '^# ' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $arg (see ./build-linux.sh --help)" >&2; exit 1 ;;
    esac
done

check_deps() {
    echo "==> Checking Linux build dependencies..."
    local missing=()

    for tool in cmake g++ pkg-config python3 git; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            missing+=("$tool")
        fi
    done

    if ! command -v lrelease >/dev/null 2>&1 && ! command -v lrelease6 >/dev/null 2>&1; then
        missing+=("lrelease / lrelease6 (qttools6-dev)")
    fi

    for mod in Qt6Widgets libarchive sqlite3 yaml-cpp libcurl nlohmann_json; do
        if ! pkg-config --exists "$mod"; then
            missing+=("$mod (pkg-config)")
        fi
    done

    if [ "${#missing[@]}" -gt 0 ]; then
        echo "  Missing required dependencies:" >&2
        for m in "${missing[@]}"; do
            echo "    - $m" >&2
        done
        echo >&2
        echo "  On Debian/Ubuntu install the CI package set:" >&2
        echo "    sudo apt-get install -y qt6-base-dev qttools6-dev libkf6syntaxhighlighting-dev \\" >&2
        echo "      cmake g++ libzip-dev libsqlite3-dev libyaml-cpp-dev libcurl4-openssl-dev \\" >&2
        echo "      nlohmann-json3-dev libfuse3-dev libpython3-dev libxkbcommon-dev libarchive-dev icoutils" >&2
        echo "  (re-run with --skip-check to bypass)" >&2
        exit 1
    fi

    # Optional — the build degrades gracefully when these are missing.
    pkg-config --exists fuse3 >/dev/null 2>&1 \
        || echo "  WARN: fuse3 not found — FUSE/VFS deploy strategies disabled"
    command -v cargo >/dev/null 2>&1 \
        || echo "  WARN: cargo not found — LOOT sorting without libloot (GMM_WITH_LOOT auto-off)"
    command -v wrestool >/dev/null 2>&1 \
        || echo "  WARN: wrestool (icoutils) not found — .exe icon extraction disabled"
    command -v ccache >/dev/null 2>&1 \
        || echo "  WARN: ccache not found — compiler cache disabled (incremental builds slower)"
    echo "  OK"
}

get_nproc() {
    nproc
}

if [ "$CLEAN_FIRST" = "1" ]; then
    echo "==> Removing old build dirs (build/, build_plugins/)..."
    rm -rf "$BUILD_DIR" build_plugins
fi

if [ "$SKIP_CHECK" = "0" ]; then
    check_deps
fi

echo "==> Checking out submodules..."
if [ "$SUBMODULE_REMOTE" = "1" ]; then
    git submodule update --init --recursive --remote
else
    git submodule update --init --recursive
fi

echo "==> Configuring (CMAKE_BUILD_TYPE=$BUILD_TYPE, GMM_BUILD_TESTS=$([ "$RUN_TESTS" = "1" ] && echo ON || echo OFF))..."
CC_LAUNCHER=""
if command -v ccache >/dev/null 2>&1; then
    CC_LAUNCHER="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    echo "  ccache found - C/C++ compiler cache enabled"
fi
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DGMM_BUILD_TESTS=$([ "$RUN_TESTS" = "1" ] && echo ON || echo OFF) \
    -DGMM_WITH_LOOT="$GMM_WITH_LOOT" $CC_LAUNCHER

echo "==> Building ($(get_nproc) parallel jobs)..."
cmake --build "$BUILD_DIR" --parallel "$(get_nproc)"

if [ "$RUN_TESTS" = "1" ]; then
    echo "==> Running test suite..."
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

echo
echo "Build complete."
echo "  Binary:  $BUILD_DIR/gamemodmanager"
echo "  Plugins: $BUILD_DIR/plugins/"
echo "  Tests:   $([ "$RUN_TESTS" = "1" ] && echo "ran above" || echo "skipped")"
