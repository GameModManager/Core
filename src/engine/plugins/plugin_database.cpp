#include "engine/plugins/plugin_database.h"

#include "engine/fs_utils.h"
#include "engine/instance/instance.h"
#include "engine/log/logger.h"
#include "engine/meta/mod_meta.h"
#include "engine/plugins/esp_header.h"
#include "engine/registry/game_features/game_feature_registry.h"
#include "engine/registry/game_knowledge.h"
#include "platform/platform_interface.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace engine {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

std::string hex2(uint32_t v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned>(v & 0xFF));
    return buf;
}

std::string hex3(uint32_t v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%03X", static_cast<unsigned>(v & 0xFFF));
    return buf;
}

void strip_cr(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

// MO2-parity same-origin asset detection (PluginList, pluginlist.cpp:271-280):
// a plugin loads <basename>.ini and any .bsa/.ba2 whose name starts with its
// base name (catches "SkyUI - Textures.bsa" / "- Voices.bsa" conventions).
// Scans the plugin's own folder (mod dir for mod plugins, game Data otherwise).
void scan_plugin_assets(GamePlugin& p) {
    const std::string base = to_lower(p.full_path.stem().string());
    if (base.empty()) return;

    std::error_code ec;
    const auto parent = p.full_path.parent_path();
    if (!std::filesystem::is_directory(parent, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string file = entry.path().filename().string();
        const std::string lower = to_lower(file);

        if (lower == base + ".ini") {
            p.has_ini = true;
            continue;
        }
        const bool is_archive =
            lower.size() >= 4 &&
            (lower.compare(lower.size() - 4, 4, ".bsa") == 0 ||
             lower.compare(lower.size() - 4, 4, ".ba2") == 0);
        if (is_archive && lower.compare(0, base.size(), base) == 0) {
            p.archives.push_back(file);
        }
    }
    std::sort(p.archives.begin(), p.archives.end());
}

}  // namespace

bool PluginDatabase::refresh(const std::filesystem::path& game_dir,
                             const std::filesystem::path& mods_dir,
                             const std::filesystem::path& meta_dir,
                             const std::string& disable_mechanism,
                             const std::string& game_native_plugins) {
    plugins_.clear();
    by_name_.clear();
    native_order_.clear();

    std::set<std::string> native_ci_set;
    for (const auto& tok : split_csv(game_native_plugins)) {
        native_order_.push_back(tok);
        native_ci_set.insert(to_lower(tok));
    }

    std::error_code ec;
    // Game Data files that are NOT shadowed by a mod. Mod files win the name
    // conflict (they are what the virtual Data serves).
    std::map<std::string, std::filesystem::path> game_data_files;
    if (std::filesystem::is_directory(game_dir / "Data", ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(game_dir / "Data", ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (is_plugin_file(entry.path())) {
                game_data_files[entry.path().filename().string()] = entry.path();
            }
        }
    }

    // Mod plugins, resolved by (descending mod priority, folder name) so the
    // highest-priority mod wins a name conflict.
    struct ModFolder {
        std::string name;
        int priority = -1;
    };
    std::vector<ModFolder> folders;
    if (std::filesystem::is_directory(mods_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::string folder = entry.path().filename().string();
            if (folder == "Overwrite" || folder == "MERGED" || folder == ".merged") continue;
            if (!disable_mechanism.empty() &&
                std::filesystem::exists(entry.path() / disable_mechanism, ec)) {
                continue;  // disabled mod contributes nothing to the virtual Data
            }
            int priority = -1;
            if (!meta_dir.empty()) priority = ModMeta::load(meta_dir, folder).priority();
            folders.push_back({folder, priority});
        }
    }
    std::sort(folders.begin(), folders.end(), [](const ModFolder& a, const ModFolder& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.name < b.name;
    });

    std::map<std::string, GamePlugin> mod_plugins;
    for (const auto& folder : folders) {
        for (const auto& entry : std::filesystem::directory_iterator(mods_dir / folder.name, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (!is_plugin_file(entry.path())) continue;
            GamePlugin p;
            p.name = entry.path().filename().string();
            p.owner_mod = folder.name;
            p.full_path = entry.path();
            p.mod_priority = folder.priority;
            mod_plugins[p.name] = std::move(p);  // later = higher priority -> wins
        }
    }

    std::set<std::string> mod_ci_names;
    for (const auto& [name, plugin] : mod_plugins) mod_ci_names.insert(to_lower(name));
    for (auto& [name, plugin] : mod_plugins) {
        game_data_files.erase(name);
        plugins_.push_back(std::move(plugin));
    }
    for (auto& [name, path] : game_data_files) {
        if (mod_ci_names.count(to_lower(name)) != 0) continue;
        GamePlugin p;
        p.name = name;
        p.full_path = path;
        plugins_.push_back(std::move(p));
    }

    // Game-native detection is case-insensitive: on-disk filenames can be
    // re-cased (e.g. "skyrim.esm") while the declared list keeps its canonical
    // case. A renamed native must keep its force-loaded band membership, or it
    // silently becomes a draggable user plugin.
    for (auto& p : plugins_) {
        p.is_game_native = native_ci_set.count(to_lower(p.name)) != 0;
        if (p.is_game_native) p.force_loaded = true;
    }

    // Creation Club content is identified by its "cc" filename prefix
    // (independent of Skyrim.ccc): it is always force-loaded and pinned above
    // user plugins. A user plugin named cc*.esp is treated as CC by design.
    for (auto& p : plugins_) {
        if (to_lower(p.name).rfind("cc", 0) == 0) {
            p.is_cc = true;
            p.force_loaded = true;
        }
    }

    parse_headers();
    for (auto& p : plugins_) scan_plugin_assets(p);
    rebuild_index();
    return true;
}

void PluginDatabase::rebuild_index() {
    by_name_.clear();
    by_name_ci_.clear();
    for (size_t i = 0; i < plugins_.size(); ++i) {
        by_name_[plugins_[i].name] = i;
        by_name_ci_[to_lower(plugins_[i].name)] = i;
    }
}

void PluginDatabase::load_creation_club(const std::filesystem::path& game_dir) {
    ccc_order_.clear();

    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (std::filesystem::is_directory(game_dir, ec)) {
        const auto root_ccc = find_file_ci(game_dir, "skyrim.ccc");
        if (!root_ccc.empty()) candidates.push_back(root_ccc);
    }
    const auto data_dir = game_dir / "Data";
    if (std::filesystem::is_directory(data_dir, ec)) {
        const auto data_ccc = find_file_ci(data_dir, "skyrim.ccc");
        if (!data_ccc.empty()) candidates.push_back(data_ccc);
    }
    if (candidates.empty()) return;

    for (const auto& ccc : candidates) {
        std::ifstream in(ccc);
        std::string line;
        while (std::getline(in, line)) {
            strip_cr(line);
            line = trim(line);
            if (line.empty()) continue;
            ccc_order_.push_back(line);
        }
        if (!ccc_order_.empty()) break;
    }

    for (auto& p : plugins_) {
        const std::string lower = to_lower(p.name);
        const bool in_ccc =
            std::any_of(ccc_order_.begin(), ccc_order_.end(),
                        [&lower](const std::string& c) { return to_lower(c) == lower; });
        if (in_ccc) {
            p.is_cc = true;
            p.force_loaded = true;
        }
    }
}

void PluginDatabase::parse_headers() {
    for (auto& p : plugins_) {
        const auto hdr = read_esp_header(p.full_path);
        if (hdr.valid) {
            p.is_master_flagged = hdr.is_master;
            p.is_light_flagged = hdr.is_light;
            p.is_medium_flagged = hdr.is_medium;
            p.masters = hdr.masters;
            p.form_version = hdr.form_version;
            p.header_version = hdr.header_version;
            p.has_no_records = hdr.num_records == 0;
            p.author = std::move(hdr.author);
            p.description = std::move(hdr.description);
        }
        // Extension fallbacks for games whose files don't carry the flags.
        const std::string ext = to_lower(p.full_path.extension().string());
        if (ext == ".esm") {
            p.has_master_ext = true;
        } else if (ext == ".esl") {
            p.has_master_ext = true;
            p.has_light_ext = true;
        } else if (ext == ".esh") {
            p.is_medium_flagged = true;
        }
    }
}

void PluginDatabase::sort_load_order() {
    if (plugins_.empty()) return;

    const size_t n = plugins_.size();
    std::vector<bool> taken(n, false);
    std::vector<size_t> ordered;

    const auto append = [&](size_t idx) {
        if (taken[idx]) return;
        ordered.push_back(idx);
        taken[idx] = true;
    };

    // 1. Game-native plugins: declared order first, then any natives not declared.
    for (const auto& name : native_order_) {
        const auto it = by_name_ci_.find(to_lower(name));
        if (it != by_name_ci_.end()) append(it->second);
    }
    for (size_t i = 0; i < n; ++i)
        if (plugins_[i].is_game_native) append(i);

    // 2. Creation Club plugins: ccc file order, then any CC not listed.
    for (const auto& name : ccc_order_) {
        const auto it = by_name_ci_.find(to_lower(name));
        if (it != by_name_ci_.end()) append(it->second);
    }
    for (size_t i = 0; i < n; ++i)
        if (plugins_[i].is_cc && !taken[i]) append(i);

    // 3. Mod plugins: topological sort by masters; ties broken by
    //    (mod_priority, name). Dependency cycles are appended in stable order.
    std::set<size_t> mod_indices;
    for (size_t i = 0; i < n; ++i) {
        if (plugins_[i].is_game_native || plugins_[i].is_cc) continue;
        mod_indices.insert(i);
    }

    const auto cmp = [&](size_t a, size_t b) {
        // Higher mod priority = more dominant = loads earlier.
        if (plugins_[a].mod_priority != plugins_[b].mod_priority)
            return plugins_[a].mod_priority > plugins_[b].mod_priority;
        return plugins_[a].name < plugins_[b].name;
    };

    std::map<std::string, std::vector<size_t>> dependents;
    std::map<std::string, size_t> indegree;
    for (size_t i : mod_indices) {
        size_t deg = 0;
        for (const auto& master : plugins_[i].masters) {
            const auto it = by_name_ci_.find(to_lower(master));
            if (it != by_name_ci_.end() && mod_indices.count(it->second) != 0) {
                dependents[to_lower(master)].push_back(i);
                ++deg;
            }
        }
        indegree[to_lower(plugins_[i].name)] = deg;
    }

    std::set<size_t, decltype(cmp)> ready(cmp);
    for (size_t i : mod_indices)
        if (indegree[to_lower(plugins_[i].name)] == 0) ready.insert(i);

    std::vector<size_t> result;
    while (!ready.empty()) {
        const size_t cur = *ready.begin();
        ready.erase(ready.begin());
        result.push_back(cur);
        for (size_t dep : dependents[to_lower(plugins_[cur].name)]) {
            if (--indegree[to_lower(plugins_[dep].name)] == 0) ready.insert(dep);
        }
    }
    std::set<size_t, decltype(cmp)> rest(cmp);
    for (size_t i : mod_indices)
        if (indegree[to_lower(plugins_[i].name)] > 0) rest.insert(i);
    for (size_t i : rest) result.push_back(i);
    for (size_t i : result) append(i);

    std::vector<GamePlugin> reordered;
    reordered.reserve(ordered.size());
    for (size_t i : ordered) reordered.push_back(plugins_[i]);
    plugins_ = std::move(reordered);
    rebuild_index();

    for (size_t i = 0; i < plugins_.size(); ++i) plugins_[i].priority = static_cast<int>(i);

    // A fresh topological sort must never move a pinned plugin - re-insert
    // every locked plugin at its locked priority.
    apply_locked_order();

    set_missing_masters();
}

bool PluginDatabase::reassert_band() {
    const size_t n = plugins_.size();
    if (n == 0) return false;

    std::vector<bool> taken(n, false);
    std::vector<size_t> ordered;
    const auto append = [&](size_t idx) {
        if (taken[idx]) return;
        ordered.push_back(idx);
        taken[idx] = true;
    };

    // Fixed band: declared natives (CI, so a re-cased file keeps its declared
    // slot), then any remaining natives; then declared CC, then remaining CC.
    // Everything else keeps its current (user/LOOT) relative order.
    for (const auto& name : native_order_) {
        const auto it = by_name_ci_.find(to_lower(name));
        if (it != by_name_ci_.end()) append(it->second);
    }
    for (size_t i = 0; i < n; ++i)
        if (plugins_[i].is_game_native) append(i);
    for (const auto& name : ccc_order_) {
        const auto it = by_name_ci_.find(to_lower(name));
        if (it != by_name_ci_.end()) append(it->second);
    }
    for (size_t i = 0; i < n; ++i)
        if (plugins_[i].is_cc && !taken[i]) append(i);
    for (size_t i = 0; i < n; ++i)
        if (!taken[i]) append(i);

    bool repaired = false;
    for (size_t i = 0; i < n; ++i) {
        if (ordered[i] != i) {
            repaired = true;
            break;
        }
    }
    if (!repaired) return false;

    std::vector<GamePlugin> reordered;
    reordered.reserve(ordered.size());
    for (size_t i : ordered) reordered.push_back(plugins_[i]);
    plugins_ = std::move(reordered);
    rebuild_index();
    for (size_t i = 0; i < plugins_.size(); ++i) plugins_[i].priority = static_cast<int>(i);
    return true;
}

bool PluginDatabase::apply_load_order(const std::vector<std::string>& order,
                                      std::string* error) {
    const size_t n = plugins_.size();
    if (n == 0) return true;

    // Resolve the requested order against the current list (case-insensitive,
    // like master lookups). Any name that matches no plugin aborts the whole
    // apply with an error - a partial reorder would corrupt the profile.
    std::vector<bool> taken(n, false);
    std::vector<size_t> user_order;
    user_order.reserve(order.size());
    const auto append = [&](size_t idx) {
        if (taken[idx]) return;
        user_order.push_back(idx);
        taken[idx] = true;
    };
    for (const auto& name : order) {
        const auto it = by_name_ci_.find(to_lower(name));
        if (it == by_name_ci_.end()) {
            if (error) *error = "Unknown plugin: " + name;
            return false;
        }
        append(it->second);
    }
    // Plugins the sort didn't mention keep their current relative order,
    // appended after the sorted ones.
    for (size_t i = 0; i < n; ++i)
        if (!taken[i]) append(i);

    // Fixed band first (game-native: declared order then the rest; CC: ccc
    // order then the rest), then the user band in the applied order.
    std::vector<bool> done(n, false);
    std::vector<size_t> ordered;
    ordered.reserve(n);
    const auto emit = [&](size_t idx) {
        if (done[idx]) return;
        ordered.push_back(idx);
        done[idx] = true;
    };
    for (const auto& name : native_order_) {
        const auto it = by_name_ci_.find(to_lower(name));
        if (it != by_name_ci_.end()) emit(it->second);
    }
    for (size_t i = 0; i < n; ++i)
        if (plugins_[i].is_game_native) emit(i);
    for (const auto& name : ccc_order_) {
        const auto it = by_name_ci_.find(to_lower(name));
        if (it != by_name_ci_.end()) emit(it->second);
    }
    for (size_t i = 0; i < n; ++i)
        if (plugins_[i].is_cc && !done[i]) emit(i);
    for (size_t idx : user_order) emit(idx);

    std::vector<GamePlugin> reordered;
    reordered.reserve(ordered.size());
    for (size_t idx : ordered) reordered.push_back(plugins_[idx]);
    plugins_ = std::move(reordered);
    rebuild_index();
    for (size_t i = 0; i < plugins_.size(); ++i) plugins_[i].priority = static_cast<int>(i);

    // Locked plugins are re-pinned at their locked priorities (an auto-sort
    // must never move them), then the fixed band is re-asserted and mod
    // indexes regenerated.
    apply_locked_order();
    set_missing_masters();
    generate_mod_indexes();
    return true;
}

void PluginDatabase::set_all_enabled() {
    // First run (no persisted profile): load everything whose masters are
    // present. A plugin whose required master is absent - not installed at all
    // - stays disabled: the game could not load it anyway, and enabling it
    // would only advertise a broken load order (MO2 parity: a missing master
    // blocks the dependent plugin). Natives/CC have no masters or only
    // game-native ones, so the fixed band is unaffected.
    for (auto& p : plugins_) {
        bool masters_present = true;
        for (const auto& master : p.masters) {
            if (by_name_ci_.find(to_lower(master)) == by_name_ci_.end()) {
                masters_present = false;
                break;
            }
        }
        p.enabled = masters_present;
    }
}

void PluginDatabase::set_missing_masters() {
    for (auto& p : plugins_) {
        p.missing_master = false;
        p.missing_masters.clear();
        for (const auto& master : p.masters) {
            if (by_name_ci_.find(to_lower(master)) == by_name_ci_.end()) {
                p.missing_master = true;
                p.missing_masters.push_back(master);
            }
        }
    }
}

void PluginDatabase::generate_mod_indexes() {
    uint32_t full_index = 0;
    uint32_t light_index = 0;
    uint32_t medium_index = 0;
    for (auto& p : plugins_) {
        if (p.is_light()) {
            p.mod_index = 0xFE000000u | light_index;
            p.mod_index_text = "FE:" + hex3(light_index);
            ++light_index;
        } else if (p.is_medium()) {
            p.mod_index = 0xFD000000u | medium_index;
            p.mod_index_text = "FD:" + hex2(medium_index);
            ++medium_index;
        } else {
            p.mod_index = full_index;
            p.mod_index_text = hex2(full_index);
            ++full_index;
        }
    }
}

bool PluginDatabase::set_enabled(const std::string& name, bool enabled, std::string* error) {
    const auto it = by_name_.find(name);
    if (it == by_name_.end()) {
        if (error) *error = "Unknown plugin: " + name;
        return false;
    }

    GamePlugin& plugin = plugins_[it->second];
    if (plugin.enabled == enabled) return true;

    if (enabled) {
        // Collect the transitive closure of disabled masters this enable
        // would touch, visiting every master (not just the first disabled
        // one). A plugin requiring an absent master blocks the whole enable.
        std::vector<GamePlugin*> chain;
        std::vector<bool> visited(plugins_.size(), false);
        std::vector<GamePlugin*> stack;
        stack.push_back(&plugin);
        while (!stack.empty()) {
            GamePlugin* cur = stack.back();
            stack.pop_back();
            if (cur->enabled) continue;
            const size_t idx = static_cast<size_t>(cur - plugins_.data());
            if (visited[idx]) continue;
            visited[idx] = true;
            chain.push_back(cur);
            for (const auto& master : cur->masters) {
                const auto mit = by_name_ci_.find(to_lower(master));
                if (mit == by_name_ci_.end()) {
                    if (error) *error = "Cannot enable " + cur->name + ": missing master " + master;
                    return false;
                }
                stack.push_back(&plugins_[mit->second]);
            }
        }
        for (GamePlugin* p : chain) p->enabled = true;
        return true;
    }

    // Disable.
    if (plugin.force_loaded) {
        if (error) *error = name + " is a core plugin and cannot be disabled";
        return false;
    }
    const std::string name_lower = to_lower(name);
    for (auto& other : plugins_) {
        if (!other.enabled || other.name == name) continue;
        const auto has_master = [&name_lower](const std::string& m) {
            return to_lower(m) == name_lower;
        };
        if (std::find_if(other.masters.begin(), other.masters.end(), has_master) != other.masters.end()) {
            if (error) *error = "Cannot disable " + name + ": " + other.name + " requires it as a master";
            return false;
        }
    }
    plugin.enabled = false;
    return true;
}

bool PluginDatabase::move_plugin(int from_row, int to_row, std::string* error) {
    const int n = static_cast<int>(plugins_.size());
    if (from_row < 0 || from_row >= n) {
        if (error) *error = "move_plugin: source row " + std::to_string(from_row) + " out of range";
        return false;
    }
    if (to_row < 0 || to_row >= n) to_row = n - 1;  // clamp to the last row

    if (plugins_[from_row].force_loaded) {
        if (error) *error = plugins_[from_row].name + " is a core plugin and cannot be moved";
        return false;
    }
    if (plugins_[from_row].locked) {
        if (error) *error = plugins_[from_row].name + " is locked and cannot be moved";
        return false;
    }

    // First row of the user band = one past the fixed (force-loaded) rows.
    int band_top = 0;
    while (band_top < n && plugins_[band_top].force_loaded) ++band_top;
    if (to_row < band_top) to_row = band_top;
    // A drop onto a locked row would displace the pinned plugin - reject it.
    if (plugins_[to_row].locked) {
        if (error) *error = plugins_[to_row].name + " is locked and cannot be displaced";
        return false;
    }
    if (from_row == to_row) return true;

    GamePlugin moved = std::move(plugins_[from_row]);
    plugins_.erase(plugins_.begin() + from_row);
    // to_row is the desired final index: after erasing from_row (which is
    // either < to_row or already equal-checked above), inserting at to_row
    // puts the plugin exactly there.
    plugins_.insert(plugins_.begin() + to_row, std::move(moved));

    rebuild_index();
    for (size_t i = 0; i < plugins_.size(); ++i) plugins_[i].priority = static_cast<int>(i);
    generate_mod_indexes();
    reassert_band();  // defense-in-depth: force-loaded rows always stay on top
    return true;
}

bool PluginDatabase::set_locked(const std::string& name, bool lock, std::string* error) {
    const auto it = by_name_ci_.find(to_lower(name));
    if (it == by_name_ci_.end()) {
        if (error) *error = name + " is not in the plugin list";
        return false;
    }
    GamePlugin& p = plugins_[it->second];
    if (lock && p.force_loaded) {
        if (error) *error = p.name + " is a core plugin and cannot be locked";
        return false;
    }
    p.locked = lock;
    if (lock) {
        locked_order_[p.name] = p.priority;
    } else {
        locked_order_.erase(p.name);
    }
    return true;
}

bool PluginDatabase::is_locked(const std::string& name) const {
    const auto it = by_name_ci_.find(to_lower(name));
    return it != by_name_ci_.end() && plugins_[it->second].locked;
}

void PluginDatabase::apply_locked_order() {
    if (locked_order_.empty()) return;

    // Locked plugins re-inserted in ascending locked priority, so earlier pins
    // claim their slots first and a later collision walks upward. Mirrors MO2
    // refreshLoadOrder (pluginlist.cpp:920).
    std::vector<std::pair<int, std::string>> locked;
    for (const auto& [name, prio] : locked_order_) locked.emplace_back(prio, name);
    std::sort(locked.begin(), locked.end());

    const size_t n = plugins_.size();
    std::set<std::string> placed;
    bool changed = false;

    for (const auto& [target, name] : locked) {
        const auto it = by_name_ci_.find(to_lower(name));
        if (it == by_name_ci_.end()) continue;  // plugin not installed (pin kept)
        size_t idx = it->second;
        if (plugins_[idx].force_loaded) continue;

        // Clamp into the user band (never above fixed game-native + CC rows).
        size_t band_top = 0;
        while (band_top < n && plugins_[band_top].force_loaded) ++band_top;
        size_t dest = static_cast<size_t>(target);
        if (dest < band_top) dest = band_top;
        if (dest >= n) dest = n - 1;

        // Walk upward past core rows and rows already claimed by an earlier
        // locked plugin. A not-yet-placed locked plugin may be displaced; its
        // own entry re-pins it.
        while (dest < n &&
               (plugins_[dest].force_loaded ||
                (plugins_[dest].locked && placed.count(plugins_[dest].name) != 0))) {
            ++dest;
        }
        if (dest >= n) dest = n - 1;

        placed.insert(name);
        if (idx == dest) continue;

        GamePlugin moved = std::move(plugins_[idx]);
        plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(idx));
        plugins_.insert(plugins_.begin() + static_cast<std::ptrdiff_t>(dest),
                        std::move(moved));
        changed = true;
    }

    if (changed) {
        rebuild_index();
        for (size_t i = 0; i < plugins_.size(); ++i) plugins_[i].priority = static_cast<int>(i);
        generate_mod_indexes();
        reassert_band();
    }
}

bool PluginDatabase::load_profile(const std::filesystem::path& profiles_dir,
                                  const std::string& profile_name,
                                  bool* repaired) {
    const auto dir = profiles_dir / profile_name;
    bool applied = false;
    if (repaired) *repaired = false;

    // Enable state: plugins.txt (* = enabled).
    if (std::filesystem::is_regular_file(dir / "plugins.txt")) {
        std::vector<bool> touched(plugins_.size(), false);
        std::ifstream in(dir / "plugins.txt");
        std::string line;
        while (std::getline(in, line)) {
            strip_cr(line);
            if (line.empty() || line[0] == '#') continue;
            const bool enabled = line[0] == '*';
            const std::string name = enabled ? line.substr(1) : line;
            const auto it = by_name_ci_.find(to_lower(name));
            if (it != by_name_ci_.end()) {
                plugins_[it->second].enabled = enabled;
                touched[it->second] = true;
            }
        }
        for (auto& p : plugins_)
            if (p.force_loaded) p.enabled = true;
        applied = true;

        // Plugins discovered after the profile was saved (a freshly installed
        // mod) are not in plugins.txt, so the file leaves them at their
        // disabled default. Default them to enabled instead - matching MO2's
        // "an installed mod's plugins load" behaviour - but only when every
        // required master is present AND active. A plugin whose master is
        // absent or was disabled by the user stays disabled until the
        // dependency is satisfied. Fixed-point so a chain of brand-new plugins
        // enables transitively (masters sort before dependents, but the loop
        // must not depend on that ordering).
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < plugins_.size(); ++i) {
                auto& p = plugins_[i];
                if (touched[i] || p.force_loaded || p.enabled) continue;
                bool masters_ok = true;
                for (const auto& master : p.masters) {
                    const auto mit = by_name_ci_.find(to_lower(master));
                    if (mit == by_name_ci_.end() || !plugins_[mit->second].enabled) {
                        masters_ok = false;
                        break;
                    }
                }
                if (masters_ok) {
                    p.enabled = true;
                    changed = true;
                }
            }
        }
    }

    // Order: loadorder.txt, first line = first loaded.
    if (std::filesystem::is_regular_file(dir / "loadorder.txt")) {
        std::ifstream in(dir / "loadorder.txt");
        std::vector<std::string> order;
        std::string line;
        while (std::getline(in, line)) {
            strip_cr(line);
            if (line.empty() || line[0] == '#') continue;
            order.push_back(line);
        }
        if (!order.empty()) {
            std::vector<bool> taken(plugins_.size(), false);
            std::vector<GamePlugin> reordered;
            for (const auto& name : order) {
                const auto it = by_name_ci_.find(to_lower(name));
                if (it != by_name_ci_.end() && !taken[it->second]) {
                    reordered.push_back(plugins_[it->second]);
                    taken[it->second] = true;
                }
            }
            for (size_t i = 0; i < plugins_.size(); ++i)
                if (!taken[i]) reordered.push_back(plugins_[i]);
            plugins_ = std::move(reordered);
            rebuild_index();
            for (size_t i = 0; i < plugins_.size(); ++i) plugins_[i].priority = static_cast<int>(i);
            applied = true;
        }
    }

    // Locked plugins: "Name|priority" lines; a locked plugin is re-pinned at
    // its recorded priority regardless of what plugins.txt/loadorder.txt say.
    if (std::filesystem::is_regular_file(dir / "lockedorder.txt")) {
        std::ifstream in(dir / "lockedorder.txt");
        std::string line;
        bool read_lock = false;
        while (std::getline(in, line)) {
            strip_cr(line);
            if (line.empty() || line[0] == '#') continue;
            const auto sep = line.rfind('|');
            if (sep == std::string::npos) continue;  // malformed - skip (MO2 parity)
            int prio = 0;
            try {
                prio = std::stoi(line.substr(sep + 1));
            } catch (...) {
                continue;
            }
            if (prio < 0) continue;
            const std::string name = line.substr(0, sep);
            read_lock = true;
            const auto it = by_name_ci_.find(to_lower(name));
            if (it == by_name_ci_.end()) {
                // Plugin not currently installed - keep the pin so it survives
                // a reinstall (MO2 readLockedOrderFrom parity).
                locked_order_[name] = prio;
                continue;
            }
            if (plugins_[it->second].force_loaded) continue;  // never lock core
            plugins_[it->second].locked = true;
            locked_order_[name] = prio;
            applied = true;
        }
        if (read_lock) apply_locked_order();
    }

    // A persisted loadorder.txt is a user/LOOT artifact and must never be able
    // to park a core plugin below user ones - heal the band if it tried.
    if (repaired && reassert_band()) *repaired = true;
    return applied;
}

void PluginDatabase::save_profile(const std::filesystem::path& profiles_dir,
                                  const std::string& profile_name) const {
    const auto dir = profiles_dir / profile_name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    write_game_plugins_txt(dir / "plugins.txt");
    write_load_order_txt(dir / "loadorder.txt");
    std::ofstream locked(dir / "lockedorder.txt");
    if (locked) {
        locked << "# This file was automatically generated by GameModManager.\n";
        locked << "# Locked plugins keep their position through sorts.\n";
        locked << "# Format: <plugin>|<priority>\n";
        for (const auto& [name, prio] : locked_order_) {
            locked << name << '|' << prio << '\n';
        }
    }
}

bool PluginDatabase::write_game_plugins_txt(const std::filesystem::path& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "# This file is used by Skyrim to keep track of your downloaded content.\n";
    out << "# Please do not modify this file.\n";
    for (const auto& p : plugins_) {
        if (p.is_game_native || p.is_cc) continue;  // game-native + CC never appear here
        out << (p.enabled ? "*" : "") << p.name << "\n";
    }
    return out.good();
}

bool PluginDatabase::write_load_order_txt(const std::filesystem::path& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "# This file was automatically generated by GameModManager.\n";
    for (const auto& p : plugins_) out << p.name << "\n";
    return out.good();
}

const GamePlugin* PluginDatabase::find(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &plugins_[it->second];
}

std::filesystem::path PluginDatabase::resolve_plugins_txt_target(
    const GameKnowledge& knowledge,
    const std::string& game_id,
    uint32_t steam_appid,
    const PlatformInterface* platform,
    const std::filesystem::path& override_path) {
    if (!override_path.empty()) return override_path;

    const std::string subdir = knowledge.get(game_id, "localappdata_folder", "");
    if (subdir.empty()) return {};  // game has no plugin support - skip
    if (!platform) return {};
    const auto base = platform->game_local_appdata_dir(steam_appid);
    if (base.empty()) return {};
    return base / subdir / "Plugins.txt";
}

bool PluginDatabase::write_plugins_txt_for_launch(
    const std::filesystem::path& game_dir,
    const std::filesystem::path& instance_root,
    const std::string& game_id,
    uint32_t steam_appid,
    const GameKnowledge& knowledge,
    const PlatformInterface* platform) {
    Instance inst = Instance::from_root(instance_root);
    std::filesystem::path override_path;
    if (inst.read_toml()) override_path = inst.info().plugins_txt_path;

    const auto target = resolve_plugins_txt_target(knowledge, game_id, steam_appid,
                                                   platform, override_path);
    if (target.empty()) {
        Logger::instance().debug(
            "PluginDatabase: no plugins.txt target for game " + game_id + " - skipping");
        return false;
    }

    const std::string disable_mechanism = disable_mechanism_for(knowledge, game_id);
    const std::string game_native = native_plugins_csv(knowledge, game_id);

    PluginDatabase db;
    if (!db.refresh(game_dir, inst.path_for(InstanceKind::Mods),
                    inst.path_for(InstanceKind::Meta), disable_mechanism, game_native)) {
        Logger::instance().warn("PluginDatabase: plugin discovery failed");
        return false;
    }
    db.load_creation_club(game_dir);
    db.sort_load_order();

    const auto profiles_dir = inst.path_for(InstanceKind::Profiles);
    if (!db.load_profile(profiles_dir, kDefaultProfile)) {
        // First run: no persisted state - enable everything so installed mods load.
        db.set_all_enabled();
        db.save_profile(profiles_dir, kDefaultProfile);
    }
    db.set_missing_masters();
    db.generate_mod_indexes();

    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    const bool ok = db.write_game_plugins_txt(target);
    db.write_load_order_txt(profiles_dir / kDefaultProfile / "loadorder.txt");
    if (ok) {
        Logger::instance().debug("PluginDatabase: wrote " +
                                 std::to_string(db.plugins().size()) +
                                 " plugins to " + target.string());
    } else {
        Logger::instance().error("PluginDatabase: failed to write " + target.string());
    }
    return ok;
}

}  // namespace engine
