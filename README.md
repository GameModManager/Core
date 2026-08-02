<div align="center">

# GameModManager

A cross-platform, plugin-driven game mod manager with an MO2-style UI. Drop in a game plugin - no core rebuild needed.

</div>

---

## Chapters

1. [Features](#features)
2. [Supported Games](#supported-games)
3. [Building from Source](#building-from-source)
4. [Running](#running)
5. [Architecture](#architecture)
6. [Writing a Game Plugin](#writing-a-game-plugin)
7. [Tests](#tests)
8. [Known Issues & TODO](#known-issues--todo)

---

## Features

- MO2-style virtual mod list with drag-and-drop reordering
- Plugin-based game support - each game is a separate shared library
- Multi-instance support (portable and installed)
- Multiple deploy strategies: 
    - [x] OverlayFS         (linux only)
    - [ ] symlink
    - [ ] hardlink
    - [ ] FUSE VFS         (linux only)
    - [ ] UVFS                  (windows only)
    - [ ] NTFS junction (windows only)
- Conflict detection with priority-ordered file overwrites
- Python scripting tier via pybind11
- Themeable via QSS token templates
- Cross-platform: Linux, Windows (macOS planned)

---

## Supported Games

| Game | Status | Plugin |
|------|--------|--------|
| The Binding of Isaac: Rebirth | Supported | `isaac.so` / `isaac.dll` |
| Skyrim Special Edition | Stubbed | `skyrimse.so` / `skyrimse.dll` |

Adding a new game requires only a new plugin file - no changes to the core binary.

---

## Building from Source

### Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| Qt 6 | ≥ 6.0 | UI framework |
| libzip | - | Archive extraction |
| sqlite3 | - | Mod cache |
| yaml-cpp | - | Config / masterlist parsing |
| libcurl | - | HTTP downloads |
| nlohmann/json | - | JSON parsing |
| pybind11 | 2.13.6 (fetched) | Python embedding |
| Python 3 | ≥ 3.10 | Plugin scripting |
| libfuse3 | (optional) | VFS/OverlayFS deploy |

### Linux (Ubuntu/Debian)

```bash
# Install dependencies
sudo apt-get update
sudo apt-get install -y \
    qt6-base-dev cmake g++ \
    libzip-dev libsqlite3-dev libyaml-cpp-dev \
    libcurl4-openssl-dev nlohmann-json3-dev \
    libfuse3-dev libpython3-dev \
    icoutils

# Clone with submodules
git clone --recurse-submodules https://github.com/PetricaT/GameModManager.git
cd GameModManager/Core

# Configure (Release recommended - Debug bloats the binary ~12x)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# The binary is at build/gamemodmanager
# Plugins are at build/plugins/
```

### Windows

```bash
# Install dependencies via vcpkg
vcpkg install qt6-base libzip sqlite3 yaml-cpp libcurl nlohmann-json pybind11 python3 --triplet x64-windows

# Clone with submodules
git clone --recurse-submodules https://github.com/PetricaT/GameModManager.git
cd GameModManager/Core

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# Build
cmake --build build --config Release
```

### macOS

> [!NOTE]
> macOS support is planned (Phase 4). The build should work but platform-specific paths and native launch are not yet implemented.

```bash
# Install dependencies via Homebrew
brew install qt@6 libzip sqlite3 yaml-cpp curl nlohmann-json python@3

# Clone with submodules
git clone --recurse-submodules https://github.com/PetricaT/GameModManager.git
cd GameModManager/Core

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)"

# Build
cmake --build build -j$(sysctl -n hw.ncpu)
```

---

## Running

```bash
# Normal launch (opens the last-used instance)
./build/gamemodmanager

# Launch a specific instance
./build/gamemodmanager --instance The_Binding_of_Isaac_Rebirth

# Headless launch (not yet implemented)
./build/gamemodmanager --launch
```

On first run, the app shows a game selection screen. After selecting a game, an instance is created under `~/.local/share/GameModManager/instances/`.

> [!IMPORTANT]
> The binary must be built in **Release** mode for reasonable size. A Debug build produces a ~28 MB binary (debug info from template-heavy headers); a Release+stripped build produces ~2.4 MB.

---

## Architecture

```
src/
├-- engine/          # Qt-free core - instance, pipeline, conflicts, plugins
│   ├-- archive/     # Zip extraction
│   ├-- cache/       # SQLite mod cache
│   ├-- deploy/      # Symlink, hardlink, junction, VFS, OverlayFS strategies
│   ├-- detect/      # Game detection, mod scanning
│   ├-- index/       # Conflict index
│   ├-- instance/    # Instance management (portable + installed)
│   ├-- log/         # Logger + crash handler
│   ├-- model/       # Profile model
│   ├-- notify/      # Notification backends
│   ├-- pipeline/    # 8-stage pipeline
│   ├-- plugin_host/ # dlopen loader + Python embedding
│   ├-- registry/    # Stage registry, hook registry, GameKnowledge
│   ├-- sort/        # Generic sort provider + ABI wrapper
│   ├-- theme/       # QSS token-template engine
│   ├-- tools/       # External tool integration
│   └-- workshop/    # Steam Workshop / remote cache
├-- platform/        # OS-specific (linux/, windows/, macos/)
├-- runtime/         # ProtonRuntime, NativeRuntime, WineRuntime
├-- ui/              # Qt Widgets - lives here, never in engine/
│   ├-- main_window/ # Main window, toolbar, status bar
│   ├-- widgets/     # Mod list, filter bar, console, etc.
│   ├-- panels/      # Tab panels
│   ├-- preview/     # ANM2 preview parser
│   └-- game_selection/ # First-run game picker
└-- main.cpp         # Entry point
```

**Key boundary:** `src/engine/` contains **zero Qt headers**. The engine is a pure C++ library; all Qt code lives in `src/ui/`. This separation keeps the engine testable headless and the UI replaceable.

### Plugin System

Game-specific logic lives in shared libraries under `external/plugins/`. Each plugin implements `gmm_register_v1()` from the ABI header and registers hooks, capabilities, and identity via the `GmmRegistrationCtx` callbacks. The engine discovers and loads plugins at startup via `dlopen`/`LoadLibrary`.

```
external/
├-- abi/             # Stable C ABI header (gmm_abi_v1.h)
├-- plugins/
│   ├-- CMakeLists.txt
│   ├-- isaac/       # Isaac game recognition plugin
│   ├-- isaac_sort/  # Isaac auto-sort + masterlist plugin
│   └-- skyrimse/    # Skyrim SE plugin (stubbed)
```

### Pipeline

The 8-stage pipeline processes mods from download to launch:

```
Fetch → Extract → Install → Stage → Resolve → Deploy → Sync → Launch
```

Each stage can be claimed by a plugin via `register_stage_claim`, allowing games to override specific stages while using the generic pipeline for the rest.

---

## Writing a Game Plugin

Create a C file in `external/plugins/yourgame/`:

```c
#include "gmm_abi_v1.h"

static const uint32_t STEAM_APPID = 12345;

extern "C" void gmm_register_v1(GmmRegistrationCtx* ctx) {
    ctx->register_identity(ctx, STEAM_APPID, NULL, NULL, NULL, NULL, NULL, NULL);

    ctx->register_hook(ctx, "conflict_extensions",
        ".dll,.bsa,.esp", NULL, 0, NULL);

    ctx->register_hook(ctx, "mods_subpath",
        "Data", NULL, 0, NULL);

    ctx->register_capability(ctx, "mods", "Plugins",
        "Data/", "ESP/ESM plugin files", NULL, NULL, NULL);
}

extern "C" uint32_t gmm_abi_version(void) {
    return GMM_ABI_VERSION;
}
```

Build it as a shared library and drop it into the `plugins/` directory. The core will pick it up on next launch - no recompilation needed.

See `external/plugins/isaac/isaac.cpp` for a complete example.

---

## Tests

```bash
cd build
ctest --output-on-failure
```

| Test | What it covers |
|------|---------------|
| `pipeline_test` | 8-stage pipeline with synthetic mods |
| `phase04_test` | Conflict index + profile model |
| `registry_test` | Stage/hook registry + pipeline wiring |
| `perf_benchmark` | 3000×50 mod stress test |
| `python_plugin_test` | Python plugin loading via pybind11 |

> [!NOTE]
> `pipeline_test` has a pre-existing linker failure (missing Logger symbols in the test target). All other tests pass headlessly.

---

## Known Issues & TODO

- [ ] `pipeline_test` link failure (Logger symbols missing from test target)
- [ ] macOS platform module (Phase 4)
- [ ] Source providers: Nexus, Steam Workshop, DirectURL, LocalArchive (Phase 5)
- [ ] NXM link routing (`nxm://` protocol handling)
- [ ] Non-Steam game detection (GOG, Epic via Heroic/Lutris)
- [ ] Settings dialog
- [ ] Instance switching - create new instance flow
- [ ] Portable instance renaming
