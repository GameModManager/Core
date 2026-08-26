#!/usr/bin/env bash
# build-macos.sh — one-shot local build for macOS
#
# Usage:
#   ./build-macos.sh                      configure + build (Release; tests off)
#   ./build-macos.sh --tests              also run ctest after the build
#   ./build-macos.sh --clean              rm -rf build/ first — from-scratch build
#   ./build-macos.sh --debug              CMAKE_BUILD_TYPE=Debug
#   ./build-macos.sh --no-loot            -DGMM_WITH_LOOT=OFF (no cargo/rust needed)
#   ./build-macos.sh --update-submodules  git submodule update --init --recursive --remote
#   ./build-macos.sh --skip-check         skip the dependency-presence check
#   ./build-macos.sh --help               show this help

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
        *) echo "Unknown option: $arg (see ./build-macos.sh --help)" >&2; exit 1 ;;
    esac
done

check_deps() {
    echo "==> Checking macOS build dependencies..."
    local missing=()

    for tool in cmake g++ pkg-config python3 git; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            missing+=("$tool")
        fi
    done

    # lrelease comes from qt@6 brew package
    if ! command -v lrelease >/dev/null 2>&1; then
        # Try the Homebrew path
        local qt6_prefix
        qt6_prefix="$(brew --prefix qt@6 2>/dev/null || echo "")"
        if [ -n "$qt6_prefix" ] && [ -x "$qt6_prefix/bin/lrelease" ]; then
            export PATH="$qt6_prefix/bin:$PATH"
        else
            missing+=("lrelease (brew install qt@6)")
        fi
    fi

    # Check Qt6 via pkg-config or cmake
    if ! pkg-config --exists Qt6Widgets 2>/dev/null; then
        # On macOS, Qt6 may be installed via Homebrew — check cmake can find it
        if ! cmake --find-package -DNAME=Qt6 -DCOMPILER_ID=GNU -DLANGUAGE=CXX -DMODE=EXIST >/dev/null 2>&1; then
            missing+=("Qt6 (brew install qt@6)")
        fi
    fi

    for mod in libarchive sqlite3 yaml-cpp libcurl nlohmann_json; do
        if ! pkg-config --exists "$mod" 2>/dev/null; then
            missing+=("$mod (brew install $mod)")
        fi
    done

    if [ "${#missing[@]}" -gt 0 ]; then
        echo "  Missing required dependencies:" >&2
        for m in "${missing[@]}"; do
            echo "    - $m" >&2
        done
        echo >&2
        echo "  Install with Homebrew:" >&2
        echo "    brew install cmake qt@6 libarchive sqlite yaml-cpp curl nlohmann-json pkg-config python3" >&2
        echo "  (re-run with --skip-check to bypass)" >&2
        exit 1
    fi

    # Optional — the build degrades gracefully when these are missing.
    command -v cargo >/dev/null 2>&1 \
        || echo "  WARN: cargo not found — LOOT sorting without libloot (GMM_WITH_LOOT auto-off)"
    command -v ccache >/dev/null 2>&1 \
        || echo "  WARN: ccache not found — compiler cache disabled (incremental builds slower)"
    echo "  OK"
}

get_nproc() {
    sysctl -n hw.ncpu
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

# On macOS, help CMake find Homebrew Qt6
CMAKE_EXTRA_ARGS=""
if [ "$(uname -s)" = "Darwin" ]; then
    QT6_PREFIX="$(brew --prefix qt@6 2>/dev/null || echo "")"
    if [ -n "$QT6_PREFIX" ]; then
        CMAKE_EXTRA_ARGS="-DCMAKE_PREFIX_PATH=$QT6_PREFIX"
        echo "  Qt6 prefix: $QT6_PREFIX"
    fi
fi

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DGMM_BUILD_TESTS=$([ "$RUN_TESTS" = "1" ] && echo ON || echo OFF) \
    -DGMM_WITH_LOOT="$GMM_WITH_LOOT" $CC_LAUNCHER $CMAKE_EXTRA_ARGS

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
