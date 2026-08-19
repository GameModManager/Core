#include "engine/profile/profile.h"

#include "engine/core/log/logger.h"
#include "engine/profile/safe_write_file.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace engine::profile {

namespace {

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Parse a decimal integer without exceptions (malformed input -> false).
bool parse_int(const std::string& s, int& out) {
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }
    if (i >= s.size()) {
        return false;
    }
    long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (s[i] - '0');
        if (v > 1000000) {
            return false;  // sanity bound
        }
    }
    out = neg ? static_cast<int>(-v) : static_cast<int>(v);
    return true;
}

// ---------------------------------------------------------------------------
// settings.ini — minimal QSettings-compatible INI model
// ---------------------------------------------------------------------------
// QSettings IniFormat writes root-section keys at the top of the file (no
// [header]), then [Section] blocks. Comments start with ';' (QSettings also
// accepts '#'). Keys are preserved in file order so a save never reorders or
// drops keys the profile manager does not own (read-before-write).

std::vector<IniSection> parse_ini(const std::string& content) {
    std::vector<IniSection> sections;
    IniSection* current = nullptr;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') {
            continue;
        }
        if (t[0] == '[') {
            const auto end = t.find(']');
            if (end == std::string::npos) {
                continue;  // malformed header, skip
            }
            sections.push_back({trim(t.substr(1, end - 1)), {}});
            current = &sections.back();
            continue;
        }
        auto sep = t.find('=');
        if (sep == std::string::npos) {
            sep = t.find(':');  // QSettings also accepts "key: value"
        }
        if (sep == std::string::npos) {
            continue;  // malformed line, skip
        }
        const std::string key = trim(t.substr(0, sep));
        if (key.empty()) {
            continue;
        }
        if (current == nullptr) {
            sections.push_back({"", {}});
            current = &sections.back();
        }
        current->entries.emplace_back(key, trim(t.substr(sep + 1)));
    }
    return sections;
}

std::string serialize_ini(const std::vector<IniSection>& sections) {
    std::string out;
    for (const auto& section : sections) {
        if (!section.name.empty()) {
            out += "[" + section.name + "]\n";
        }
        for (const auto& [key, value] : section.entries) {
            out += key + "=" + value + "\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// modlist.txt
// ---------------------------------------------------------------------------

// Parse modlist.txt content into file-order entries. Empty lines, comments
// ('#') and malformed lines are skipped; the first occurrence of a mod wins
// (MO2 behavior). Bare names (no +/-/* prefix) are tolerated as enabled.
std::vector<ModListEntry> parse_modlist(const std::string& content) {
    std::vector<ModListEntry> entries;
    std::unordered_set<std::string> seen;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        bool enabled = true;
        bool foreign = false;
        std::string mod_id;
        switch (t[0]) {
        case '-':
            enabled = false;
            mod_id = trim(t.substr(1));
            break;
        case '*':
            foreign = true;
            mod_id = trim(t.substr(1));
            break;
        case '+':
            mod_id = trim(t.substr(1));
            break;
        default:
            mod_id = t;
            break;
        }
        if (mod_id.empty()) {
            continue;
        }
        if (!seen.insert(mod_id).second) {
            continue;
        }
        entries.push_back({mod_id, enabled, foreign, 0});
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Generic line-based files (plugins.txt, loadorder.txt, archives.txt)
// ---------------------------------------------------------------------------

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    if (!std::filesystem::exists(path)) {
        return lines;
    }
    std::istringstream stream(read_file(path));
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string t = trim(line);
        if (!t.empty()) {
            lines.push_back(t);
        }
    }
    return lines;
}

bool write_lines(const std::filesystem::path& path, const std::vector<std::string>& lines) {
    std::string content;
    for (const auto& line : lines) {
        content += line;
        content += '\n';
    }
    return safe_write_file(path, content);
}

}  // namespace

// ---------------------------------------------------------------------------
// Profile
// ---------------------------------------------------------------------------

Profile::Profile(std::filesystem::path directory, std::chrono::milliseconds modlist_delay)
    : directory_(std::move(directory)),
      modlist_writer_([this] { do_write_modlist(); }, modlist_delay) {
    // Load settings.ini into the ordered INI model (missing file -> empty).
    if (std::filesystem::exists(settings_path())) {
        const std::string content = read_file(settings_path());
        if (!content.empty()) {
            ini_ = parse_ini(content);
        }
    }
}

Profile::~Profile() = default;

// --- settings.ini ----------------------------------------------------------

std::string Profile::get_setting(const std::string& key) const {
    for (const auto& section : ini_) {
        if (!section.name.empty()) {
            continue;  // root section only
        }
        for (const auto& [k, v] : section.entries) {
            if (k == key) {
                return v;
            }
        }
    }
    return {};
}

void Profile::set_setting(const std::string& key, const std::string& value) {
    for (auto& section : ini_) {
        if (!section.name.empty()) {
            continue;
        }
        for (auto& [k, v] : section.entries) {
            if (k == key) {
                v = value;
                return;
            }
        }
        section.entries.emplace_back(key, value);
        return;
    }
    // No root section yet — it must serialize before any [Section] block.
    ini_.insert(ini_.begin(), IniSection{"", {{key, value}}});
}

bool Profile::get_setting_bool(const std::string& key) const {
    const std::string v = get_setting(key);
    return v == "true" || v == "1";
}

void Profile::set_setting_bool(const std::string& key, bool value) {
    set_setting(key, value ? "true" : "false");
}

bool Profile::local_saves() const { return get_setting_bool("LocalSaves"); }
void Profile::set_local_saves(bool value) { set_setting_bool("LocalSaves", value); }

bool Profile::local_settings() const { return get_setting_bool("LocalSettings"); }
void Profile::set_local_settings(bool value) { set_setting_bool("LocalSettings", value); }

bool Profile::automatic_archive_invalidation() const {
    return get_setting_bool("AutomaticArchiveInvalidation");
}
void Profile::set_automatic_archive_invalidation(bool value) {
    set_setting_bool("AutomaticArchiveInvalidation", value);
}

bool Profile::save_settings() {
    if (!safe_write_file(settings_path(), serialize_ini(ini_))) {
        Logger::instance().error("failed to write settings.ini: " + settings_path().string());
        return false;
    }
    return true;
}

// --- modlist.txt -----------------------------------------------------------

void Profile::refresh_mod_status(const std::vector<std::string>& known_mods,
                                 const std::vector<std::string>& foreign_mods) {
    std::lock_guard lock(mods_mutex_);

    std::string content;
    if (std::filesystem::exists(modlist_path())) {
        content = read_file(modlist_path());
    }
    const auto file_entries = parse_modlist(content);
    const int num_file = static_cast<int>(file_entries.size());

    // File order -> priority. The file's first line is the highest priority
    // mod (MO2 writes the list in reverse priority order and inverts on
    // load), so line i gets priority num_file-1-i.
    std::unordered_map<std::string, int> priority;
    std::unordered_map<std::string, bool> enabled;
    std::unordered_map<std::string, bool> foreign;
    for (int i = 0; i < num_file; ++i) {
        priority[file_entries[i].mod_id] = num_file - 1 - i;
        enabled[file_entries[i].mod_id] = file_entries[i].enabled;
        foreign[file_entries[i].mod_id] = file_entries[i].foreign;
    }

    // Mods not in the file: foreign mods get the lowest priorities (below all
    // file mods), managed (new) mods the highest (above all file mods), both
    // enabled by default — MO2's refreshModStatus behavior.
    std::unordered_set<std::string> foreign_set(foreign_mods.begin(), foreign_mods.end());
    int next_high = num_file;
    int next_low = -1;
    bool modified = false;
    for (const auto& id : known_mods) {
        if (priority.count(id) != 0) {
            continue;
        }
        if (foreign_set.count(id) != 0) {
            priority[id] = next_low--;
            foreign[id] = true;
        } else {
            priority[id] = next_high++;
        }
        enabled[id] = true;
        modified = true;
    }

    // Shift so the minimum priority is 0 (foreign mods may have gone negative).
    int min_priority = 0;
    for (const auto& [id, p] : priority) {
        min_priority = std::min(min_priority, p);
    }
    if (min_priority < 0) {
        for (auto& [id, p] : priority) {
            p -= min_priority;
        }
    }

    // Rebuild mods_ sorted by priority ascending.
    mods_.clear();
    mods_.reserve(priority.size());
    for (const auto& [id, p] : priority) {
        mods_.push_back(ModListEntry{id, enabled[id], foreign[id], p});
    }
    std::sort(mods_.begin(), mods_.end(),
              [](const ModListEntry& a, const ModListEntry& b) { return a.priority < b.priority; });

    // Persist newly added mods (delayed, batched) so the file converges.
    if (modified) {
        modlist_writer_.write();
    }
}

std::vector<ModListEntry> Profile::mods() const {
    std::lock_guard lock(mods_mutex_);
    return mods_;
}

int Profile::priority_of(const std::string& mod_id) const {
    std::lock_guard lock(mods_mutex_);
    for (const auto& m : mods_) {
        if (m.mod_id == mod_id) {
            return m.priority;
        }
    }
    return -1;
}

void Profile::set_mod_enabled(const std::string& mod_id, bool enabled) {
    bool changed = false;
    {
        std::lock_guard lock(mods_mutex_);
        for (auto& m : mods_) {
            if (m.mod_id == mod_id) {
                if (m.enabled != enabled) {
                    m.enabled = enabled;
                    changed = true;
                }
                break;
            }
        }
    }
    if (changed) {
        modlist_writer_.write();
    }
}

bool Profile::set_mod_priority(const std::string& mod_id, int new_priority) {
    std::lock_guard lock(mods_mutex_);
    auto it = std::find_if(mods_.begin(), mods_.end(),
                           [&](const ModListEntry& m) { return m.mod_id == mod_id; });
    if (it == mods_.end()) {
        return false;
    }
    const int old_priority = it->priority;
    new_priority = std::clamp(new_priority, 0, static_cast<int>(mods_.size()) - 1);
    if (new_priority == old_priority) {
        return false;
    }

    auto entry = std::move(*it);
    mods_.erase(it);
    mods_.insert(mods_.begin() + new_priority, std::move(entry));
    for (int i = 0; i < static_cast<int>(mods_.size()); ++i) {
        mods_[i].priority = i;
    }
    modlist_writer_.write();
    return true;
}

void Profile::write_modlist() { modlist_writer_.write(); }
void Profile::write_modlist_now() { modlist_writer_.write_immediately(); }
void Profile::cancel_modlist_write() { modlist_writer_.cancel(); }

void Profile::do_write_modlist() {
    std::lock_guard lock(mods_mutex_);
    if (!std::filesystem::exists(directory_)) {
        return;
    }

    std::string content = "# This file was automatically generated by GameModManager.\r\n";
    // mods_ is sorted ascending by priority; the file's first line must be the
    // highest priority mod, so iterate in reverse.
    for (auto it = mods_.rbegin(); it != mods_.rend(); ++it) {
        content += it->foreign ? '*' : (it->enabled ? '+' : '-');
        content += it->mod_id;
        content += "\r\n";
    }
    if (!safe_write_file(modlist_path(), content)) {
        Logger::instance().error("failed to write modlist: " + modlist_path().string());
    }
}

// --- plugins.txt / loadorder.txt / lockedorder.txt / archives.txt ----------

std::vector<std::string> Profile::read_plugins() const { return read_lines(plugins_path()); }
bool Profile::write_plugins(const std::vector<std::string>& plugins) {
    return write_lines(plugins_path(), plugins);
}

std::vector<std::string> Profile::read_load_order() const { return read_lines(loadorder_path()); }
bool Profile::write_load_order(const std::vector<std::string>& order) {
    return write_lines(loadorder_path(), order);
}

std::vector<LockedPlugin> Profile::read_locked_order() const {
    std::vector<LockedPlugin> result;
    for (const auto& line : read_lines(lockedorder_path())) {
        const auto sep = line.find('|');
        if (sep == std::string::npos) {
            continue;  // malformed line, skip
        }
        const std::string name = trim(line.substr(0, sep));
        int priority = 0;
        if (name.empty() || !parse_int(trim(line.substr(sep + 1)), priority)) {
            continue;  // malformed line, skip
        }
        result.push_back({name, priority});
    }
    return result;
}

bool Profile::write_locked_order(const std::vector<LockedPlugin>& locked) {
    std::string content;
    for (const auto& entry : locked) {
        content += entry.name + "|" + std::to_string(entry.priority) + "\n";
    }
    return safe_write_file(lockedorder_path(), content);
}

std::vector<std::string> Profile::read_archives() const { return read_lines(archives_path()); }
bool Profile::write_archives(const std::vector<std::string>& archives) {
    return write_lines(archives_path(), archives);
}

}  // namespace engine::profile