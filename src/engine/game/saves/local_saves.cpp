#include "engine/game/saves/local_saves.h"

#include "engine/core/log/logger.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace engine {

namespace {

// ---------------------------------------------------------------------------
// Minimal line-preserving INI editor (Qt-free).
//
// MO2 uses Win32 GetPrivateProfileStringW / WriteRegistryValue, which operate
// on plain [Section] key=value text files, rewriting only the touched keys and
// leaving comments + unrelated keys alone. This is the same contract, enough
// for the sLocalSavePath / bUseMyGamesDirectory keys we manage. We never
// rewrite keys we do not own, so a game's other INI content survives.
// ---------------------------------------------------------------------------

std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

bool iequals(std::string a, std::string b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Current value of a key in a section ("" when absent). Empty section name =
// keys outside any section.
std::string ini_get(const fs::path& path, const std::string& section,
                    const std::string& key) {
    std::ifstream in(path);
    if (!in) return {};
    std::string line;
    bool in_section = section.empty();
    while (std::getline(in, line)) {
        auto t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') {
            auto close = t.find(']');
            if (close == std::string::npos) continue;
            auto name = trim(t.substr(1, close - 1));
            in_section = iequals(name, section);
            continue;
        }
        if (!in_section) continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        auto k = trim(t.substr(0, eq));
        if (!iequals(k, key)) continue;
        auto v = trim(t.substr(eq + 1));
        // Strip a trailing comment? Win32 does not treat ";" as comment in
        // values; leave the value as-is.
        return v;
    }
    return {};
}

// Set or remove a key. value == "" removes the key line (Win32 NULL-parity).
// Comments and unrelated lines are preserved byte-for-byte; new keys are
// inserted after the section header (or appended at the end when the section
// is new / absent). Rewrites the file only when something changed.
void ini_set(const fs::path& path, const std::string& section,
             const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }

    const bool removing = value.empty();

    // Locate section span.
    int section_header = -1;
    int section_end = -1;  // index of last line in section (inclusive)
    {
        int idx = 0;
        bool in_section = section.empty();
        for (size_t i = 0; i < lines.size(); ++i) {
            auto t = trim(lines[i]);
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            if (t[0] == '[') {
                auto close = t.find(']');
                if (close == std::string::npos) continue;
                auto name = trim(t.substr(1, close - 1));
                if (in_section) { section_end = static_cast<int>(i) - 1; break; }
                in_section = iequals(name, section);
                if (in_section) section_header = static_cast<int>(i);
                continue;
            }
            if (in_section) idx = static_cast<int>(i);
        }
        if (section_header >= 0 && section_end < 0)
            section_end = static_cast<int>(lines.size()) - 1;
    }

    // Remove the existing key line if present (within section or before first
    // header for the global case).
    bool found = false;
    {
        int lo = section_header >= 0 ? section_header + 1 : 0;
        int hi = section_header >= 0 ? section_end : -1;
        std::vector<std::string> kept;
        if (section.empty()) {
            // Global keys: only look at lines before the first section header.
            for (size_t i = 0; i < lines.size(); ++i) {
                auto t = trim(lines[i]);
                if (!t.empty() && t[0] == '[') break;
                auto eq = t.find('=');
                if (eq != std::string::npos &&
                    iequals(trim(t.substr(0, eq)), key)) {
                    found = true;
                    continue;
                }
                kept.push_back(lines[i]);
            }
            lines = kept;
        } else if (section_header >= 0) {
            for (int i = 0; i <= hi; ++i) {
                if (i <= section_header) { kept.push_back(lines[i]); continue; }
                auto t = trim(lines[i]);
                auto eq = t.find('=');
                if (eq != std::string::npos &&
                    iequals(trim(t.substr(0, eq)), key)) {
                    found = true;
                    continue;
                }
                kept.push_back(lines[i]);
            }
            // Append the section tail (lines after section_end).
            for (size_t i = static_cast<size_t>(hi + 1); i < lines.size(); ++i)
                kept.push_back(lines[i]);
            lines = kept;
        }
    }

    if (removing) {
        if (!found) return;  // nothing to do
    } else {
        // Build the key line "key=value" (MO2/Win32 format has no spaces).
        std::string kv = key + "=" + value;
        if (found) {
            // Replace the (already removed) key line: we removed it above, so
            // re-insert in the same section position - append after header.
        }
        if (section_header >= 0) {
            auto it = lines.begin() + section_header + 1;
            lines.insert(it, kv);
        } else {
            // New section: append header + key at the end (with a blank line
            // separator for readability, Win32 emits [Section]\nKey=Value).
            if (!lines.empty() && !lines.back().empty()) lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(kv);
        }
    }

    std::ofstream out(path, std::ios::trunc);
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i] << "\n";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// resolve_local_saves
// ---------------------------------------------------------------------------

LocalSavesConfig resolve_local_saves(
    const fs::path& my_games_dir,
    const fs::path& instance_root,
    const std::string& profile_sub_name,
    const std::string& ini_file_name,
    bool enabled) {
    LocalSavesConfig cfg;
    cfg.enabled = enabled;
    if (enabled && (my_games_dir.empty() || instance_root.empty() ||
                    profile_sub_name.empty() || ini_file_name.empty())) {
        cfg.enabled = false;
        Logger::instance().warn(
            "local saves: disabled - missing my_games_dir/instance/profile/ini input");
        return cfg;
    }
    cfg.ini_path = my_games_dir / ini_file_name;
    cfg.profile_saves_dir = instance_root / "profiles" / profile_sub_name / "saves";
    cfg.backup_path = instance_root / "profiles" / profile_sub_name / "savepath.ini";
    cfg.local_saves_dir = my_games_dir / kLocalSavesDummy;
    return cfg;
}

// ---------------------------------------------------------------------------
// apply_local_saves (MO2 prepareProfile parity)
// ---------------------------------------------------------------------------

bool apply_local_saves(const LocalSavesConfig& cfg) {
    if (cfg.ini_path.empty() || cfg.backup_path.empty() ||
        cfg.profile_saves_dir.empty() || cfg.local_saves_dir.empty()) {
        Logger::instance().warn("local saves: incomplete config, no-op");
        return false;
    }

    const std::string dummy_value = std::string(kLocalSavesDummy) + "\\";
    const std::string present = "1";

    // Get the current state (Win32 default "SKIP_ME" when absent).
    std::string current_path = ini_get(cfg.ini_path, kLocalSaveSection, kLocalSavesPathKey);
    std::string current_mygames = ini_get(cfg.ini_path, kLocalSaveSection, kLocalUseMyGamesKey);
    const bool key_present = !current_path.empty();

    const std::string path_norm = trim(current_path);
    // alreadyEnabled: the game INI already points at our dummy dir. MO2
    // compares exactly ("__MO_Saves\\"); be lenient about a missing trailing
    // separator since some games normalize it.
    const bool already_enabled =
        iequals(path_norm, dummy_value) || iequals(path_norm, kLocalSavesDummy);

    if (cfg.enabled) {
        // Create the local saves dir AND the profile dir that will back it.
        // The latter is the bind-mount source - it must exist for the mount to
        // install (MO2 creates the profile saves dir the same way via
        // createTarget on the VFS mapping).
        std::error_code ec;
        fs::create_directories(cfg.local_saves_dir, ec);
        if (ec) {
            Logger::instance().error("local saves: create dir " +
                cfg.local_saves_dir.string() + " failed: " + ec.message());
            return false;
        }
        fs::create_directories(cfg.profile_saves_dir, ec);
        if (ec) {
            Logger::instance().error("local saves: create profile dir " +
                cfg.profile_saves_dir.string() + " failed: " + ec.message());
            return false;
        }
        if (!already_enabled) {
            // Back up the game's current values once, into savepath.ini.
            if (key_present) {
                std::ofstream backup(cfg.backup_path, std::ios::app);
                if (backup) {
                    backup << "[" << kLocalSaveSection << "]\n";
                    backup << kLocalSavesPathKey << "=" << current_path << "\n";
                    if (!current_mygames.empty())
                        backup << kLocalUseMyGamesKey << "=" << current_mygames << "\n";
                }
            }
            ini_set(cfg.ini_path, kLocalSaveSection, kLocalSavesPathKey, dummy_value);
            ini_set(cfg.ini_path, kLocalSaveSection, kLocalUseMyGamesKey, present);
            Logger::instance().debug(
                "local saves: redirected game saves to " + cfg.local_saves_dir.string());
            return true;
        }
        return false;  // already local - nothing changed
    }

    // Disabling.
    if (!already_enabled) return false;  // nothing to undo

    std::string saved_path;
    std::string saved_mygames;
    bool has_backup = false;
    if (fs::exists(cfg.backup_path)) {
        saved_path = ini_get(cfg.backup_path, kLocalSaveSection, kLocalSavesPathKey);
        saved_mygames = ini_get(cfg.backup_path, kLocalSaveSection, kLocalUseMyGamesKey);
        has_backup = true;
    }

    if (has_backup) {
        if (!saved_path.empty())
            ini_set(cfg.ini_path, kLocalSaveSection, kLocalSavesPathKey, saved_path);
        else
            ini_set(cfg.ini_path, kLocalSaveSection, kLocalSavesPathKey, "");
        if (!saved_mygames.empty())
            ini_set(cfg.ini_path, kLocalSaveSection, kLocalUseMyGamesKey, saved_mygames);
        else
            ini_set(cfg.ini_path, kLocalSaveSection, kLocalUseMyGamesKey, "");
        std::error_code ec;
        fs::remove(cfg.backup_path, ec);
    } else {
        // No backup: delete the keys outright (MO2 WriteRegistryValue NULL).
        ini_set(cfg.ini_path, kLocalSaveSection, kLocalSavesPathKey, "");
        ini_set(cfg.ini_path, kLocalSaveSection, kLocalUseMyGamesKey, "");
    }
    Logger::instance().debug("local saves: restored game save path from backup");
    return true;
}

// ---------------------------------------------------------------------------
// local_saves_mount
// ---------------------------------------------------------------------------

std::pair<fs::path, fs::path> local_saves_mount(const LocalSavesConfig& cfg) {
    if (!cfg.enabled || cfg.profile_saves_dir.empty() || cfg.local_saves_dir.empty())
        return {};
    // {source = real profile saves dir, target = game-facing My Games dummy dir}
    return {cfg.profile_saves_dir, cfg.local_saves_dir};
}

}  // namespace engine