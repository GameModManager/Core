#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class Platform;

// Per-instance context for running wine/protontricks tools against a game's
// Proton prefix. All discovery goes through `platform`; when `steam_appid` is
// non-zero the tool runs inside that game's prefix.
struct ProtonToolRequest {
    const Platform* platform = nullptr;
    uint32_t steam_appid = 0;
    std::filesystem::path game_dir;
    // Selected runner (display name or absolute path to a `proton` script).
    // Empty = automatic.
    std::string runner_override;
};

// Run a winetricks-style command against the game's prefix, detached:
//   - `protontricks <appid> <args...>` when protontricks is on PATH and an
//     appid is known (preferred - it locates the prefix itself and wires up
//     the correct wine build),
//   - `proton runinprefix <args>` for wine builtins (winecfg, regedit) when no
//     protontricks,
//   - system `winetricks <args>` with WINEPREFIX set otherwise.
// `args` may be a category verb (`dlls` - opens the "Install a Windows DLL or
// component" picker), a wine builtin (`winecfg`, `regedit`), an install verb
// (`vcrun2022`, ...), or empty (winetricks main GUI).
// Returns the child PID, or -1 on failure.
int64_t run_proton_tool(const ProtonToolRequest& request,
                        const std::vector<std::string>& args);

// Run an arbitrary Windows executable inside the game's prefix, detached.
// Returns the child PID, or -1 on failure.
int64_t run_proton_exe(const ProtonToolRequest& request,
                       const std::filesystem::path& exe);

// True when something on this machine can configure/install packages in a
// Proton prefix (protontricks, or winetricks + wine/Proton). Used by the UI
// to warn before offering install actions.
bool proton_tooling_available(const ProtonToolRequest& request);

// Resolve a per-instance runner override to an absolute `proton` script path.
// Empty when the override is empty or unresolvable.
std::filesystem::path resolve_proton_runner(const ProtonToolRequest& request);

}  // namespace engine
