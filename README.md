# GameModManager

A cross-platform, plugin-driven game mod manager. Game-specific logic lives in
shared libraries loaded at runtime, so adding a new game never requires a core
rebuild.

> [!WARNING]
> This project is under active development and may break between releases.

![Main UI Preview](./assets/GMM2.png)

---

## Overview

GameModManager is a mod manager built around a stable C ABI and a Qt-free
engine core. It runs on Linux and Windows, with macOS planned. The UI follows
the familiar MO2-style layout, while the engine keeps platform-specific logic
behind a clean abstraction layer.

### Notable Features

- **Plugin-driven game support** - each game is a separate shared library
  loaded via a stable C ABI (`dlopen`/`LoadLibrary`). No core rebuild to add a
  game.
- **Cross-platform deploy strategies** - OverlayFS (Linux default), symlink,
  hardlink, and FUSE VFS, with Windows USVFS and NTFS junction planned.
- **MO2-style mod list** - virtual mod list with drag-and-drop reordering, a
  fixed game-native band, separators, and conflict status icons.
- **Conflict detection** - priority-ordered file overwrites with resolution
  dialogs.
- **Downloads** - Nexus and Steam Workshop sources, per-file progress and
  pause, `nxm://` link routing via IPC.
- **Headless CLI** - launch games and handle `nxm://` / `gmm://` links without
  the UI.
- **Themeable UI** - QSS token templates (Dark and Nord bundled).
- **Python scripting** - embedded via pybind11.
- **Multi-instance** - portable and global instances.

---

## Supported Games

| Game | Status | Plugin |
|------|--------|--------|
| The Binding of Isaac: Rebirth | Supported | `TheBindingOfIsaacRebirth.so` / `.dll` |
| Skyrim Special Edition | Supported | `SkyrimSpecialEdition.so` / `.dll` |

Tool plugins (non-game): `ImageDiff`, `IsaacModSorter`.

Adding a new game requires only a new plugin file.

---

## Building from Source

### Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| Qt 6 | >= 6.0 | UI framework |
| Qt6Keychain | >= 0.14 | OS keyring backend |
| libzip | - | Archive extraction |
| sqlite3 | - | Mod cache |
| yaml-cpp | - | Config / masterlist parsing |
| libcurl | - | HTTP downloads |
| nlohmann/json | - | JSON parsing |
| pybind11 | 2.13.6 (fetched) | Python embedding |
| Python 3 | >= 3.10 | Plugin scripting |
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
    qtkeychain-qt6-dev \
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
vcpkg install qt6-base qtkeychain libzip sqlite3 yaml-cpp libcurl nlohmann-json pybind11 python3 --triplet x64-windows

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
> macOS support is planned. The build should work, but platform-specific paths
> and native launch are not yet implemented.

```bash
# Install dependencies via Homebrew
brew install qt@6 qtkeychain libzip sqlite3 yaml-cpp curl nlohmann-json python@3

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

## Thank You

GameModManager would not exist without the incredible work of the modding
community. We stand on the shoulders of giants, and we want to give credit where
it is due.

### ModOrganizer 2 Community

The single biggest inspiration for this project. MO2's architecture, plugin
system, virtual filesystem, and UX have shaped GameModManager at every level.

- [ModOrganizer2 / modorganizer](https://github.com/ModOrganizer2/modorganizer) - the core MO2 application
- [ModOrganizer2 / usvfs](https://github.com/ModOrganizer2/usvfs) - the USVFS virtual filesystem layer
- [ModOrganizer2 / modorganizer-archive](https://github.com/ModOrganizer2/modorganizer-archive) - archive handling
- [ModOrganizer2 / modorganizer-game_bethesda](https://github.com/ModOrganizer2/modorganizer-game_bethesda) - Bethesda game support
- [ModOrganizer2 / modorganizer-installer_fomod](https://github.com/ModOrganizer2/modorganizer-installer_fomod) - FOMOD installer
- [ModOrganizer2 / modorganizer-installer_fomod_csharp](https://github.com/ModOrganizer2/modorganizer-installer_fomod_csharp) - C# FOMOD installer
- [ModOrganizer2 / modorganizer-installer_quick](https://github.com/ModOrganizer2/modorganizer-installer_quick) - quick installer
- [ModOrganizer2 / modorganizer-lootcli](https://github.com/ModOrganizer2/modorganizer-lootcli) - LOOT CLI integration
- [ModOrganizer2 / modorganizer-nxmhandler](https://github.com/ModOrganizer2/modorganizer-nxmhandler) - NXM link handler
- [ModOrganizer2 / modorganizer-preview_dds](https://github.com/ModOrganizer2/modorganizer-preview_dds) - DDS preview

### Plugin & Tool Authors

- [gabriel-andreescu / modorganizer-preview_nif](https://github.com/gabriel-andreescu/modorganizer-preview_nif) - NIF 3D mesh preview (the basis for our NIF viewer)
- [Exit-9B / modorganizer-bsplugins](https://github.com/Exit-9B/modorganizer-bsplugins) - Bethesda plugin manager (BSPlugins)
- [aglowinthefield / mo2-fomod-plus](https://github.com/aglowinthefield/mo2-fomod-plus) - enhanced FOMOD installer

### NIFSkope

The NIF file format library and tools. Our shader lighting math is derivative of
NIFSkope's work and is used under its BSD license.

- [NIF File Format Library and Tools](https://github.com/niftools/nifskope) - NIFSkope

### Nexus Mods

- [Nexus-Mods / NexusMods.App](https://github.com/Nexus-Mods/NexusMods.App) - the Nexus Mods app, a source of inspiration for modern mod management UX

### Other Mod Managers

- [ChrisDKN / Amethyst-Mod-Manager](https://github.com/ChrisDKN/Amethyst-Mod-Manager) - an open-source mod manager we reference for ideas

### Game Studios

- **Bethesda Game Studios** - for creating the Elder Scrolls and Fallout series, and for the modding-friendly file formats (NIF, BSA/BA2, ESP/ESM) that this project supports
- **Valve** - for Steam, the Steamworks SDK, and for fostering a modding ecosystem
- **Edmund McMillen & Nicalis** - for The Binding of Isaac, and for the ANM2 animation format our viewer supports

### The Modding Community

To every mod author, every tool creator, every wiki contributor, and every
player who has ever installed a mod - thank you. This project exists because of
you.
