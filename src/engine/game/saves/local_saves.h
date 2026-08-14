#pragma once

// Per-profile local saves (MO2 GamebryoLocalSavegames parity):
//   - the game's own saves are redirected out of the per-game "My Games"
//     folder and into <instance>/profiles/<name>/saves;
//   - the game INI is rewritten (with a savepath.ini backup/restore) so the
//     game writes sLocalSavePath=__MO_Saves\ under My Games, and a launch-time
//     bind mount forwards that real path to the profile saves dir.
//
// This is the ILocalSavegames consumer that P2.2/P2.5 planned for: the
// LocalSavegamesFeature (registered by the game plugin) supplies the INI file
// name + saves subpath, and this module does the on-disk work. Everything here
// is filesystem + INI text mutation (no VFS, no overlay) so it is unit-testable
// with temp dirs; the match to the actual mount is the bind-mount pair you hand
// the overlay launcher.

#include <filesystem>
#include <string>
#include <utility>

namespace engine {

// INI section the game reads sLocalSavePath / bUseMyGamesDirectory from.
// Matches MO2's GetPrivateProfileStringW(L"General", ...) in
// gamebryolocalsavegames.cpp.
inline constexpr const char* kLocalSaveSection = "General";

// The directory name the game saves into when local saves are enabled, under
// the per-game My Games folder. MO2's GamebryoLocalSavegames calls this
// "localSavesDummy()" and returns it WITH a trailing backslash ("__MO_Saves\\");
// the INI value we write is that backslash-trailing form, while the on-disk
// directory name is this bare form.
inline constexpr const char* kLocalSavesDummy = "__MO_Saves";

// INI key names (MO2 GetPrivateProfileStringW calls).
inline constexpr const char* kLocalSavesPathKey = "sLocalSavePath";
inline constexpr const char* kLocalUseMyGamesKey = "bUseMyGamesDirectory";

// Per-profile local-saves configuration resolved for one launch.
struct LocalSavesConfig {
    bool enabled = false;                       // resolved from the toggle (default off)
    std::filesystem::path ini_path;             // absolute game INI path (MyGames/<ini>)
    std::filesystem::path backup_path;          // <profile>/savepath.ini (MO2 parity)
    std::filesystem::path profile_saves_dir;    // real destination (profile dir "saves")
    std::filesystem::path local_saves_dir;      // game-facing dir (MyGames/__MO_Saves)
};

// Resolve every path for a game's local-saves setup. `ini_file_name` comes
// from the registered LocalSavegamesFeature (e.g. "Skyrimcustom.ini" for
// Skyrim SE); `profile_sub_name` is the current profile folder name ("Default").
[[nodiscard]] LocalSavesConfig resolve_local_saves(
    const std::filesystem::path& my_games_dir,     // <documents>/My Games/<game>
    const std::filesystem::path& instance_root,    // <instance root>
    const std::string& profile_sub_name,           // e.g. "Default"
    const std::string& ini_file_name,              // e.g. "Skyrimcustom.ini"
    bool enabled);

// MO2 GamebryoLocalSavegames::prepareProfile equivalent. When `enable`:
//   - creates the local saves dir,
//   - backs up the current sLocalSavePath/bUseMyGamesDirectory into
//     savepath.ini (only once, when not already local),
//   - writes sLocalSavePath=__MO_Saves\ and bUseMyGamesDirectory=1 into the ini.
// When already local and `enabled` is false: restores the backed-up values
// and removes savepath.ini (MO2 deletes-on-no-backup parity).
// Returns whether the ini changed state (caller mounts on true).
[[nodiscard]] bool apply_local_saves(const LocalSavesConfig& cfg);

// The bind mount a launch needs so the game's writes to the dummy dir land in
// the profile saves dir: {source = profile saves dir, target = local saves
// dir to mount over}. Empty path when not enabled.
[[nodiscard]] std::pair<std::filesystem::path, std::filesystem::path>
    local_saves_mount(const LocalSavesConfig& cfg);

}  // namespace engine