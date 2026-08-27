#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// v2 IPlugin settings persistence registry (MO2 IPlugin settings parity).
//
// A v2 plugin declares its user-facing options through the ABI
// register_settings / register_settings_tab callbacks. The engine captures the
// declarations here, keyed by the plugin's basename (the same key the UI's
// Settings facade uses: plugins/settings/<basename>/<key>), so the host can
// read/write a plugin's settings at runtime and the values persist through the
// same QSettings store the Plugins-tab UI edits.
//
// This header is Qt-free on purpose: only the .cpp pulls in <QSettings>, so
// including it does not force Qt onto every translation unit (keeps the engine
// free of an engine->ui link cycle). QtCore (QSettings) is linked PRIVATE on
// gmm_engine; QtCore is not the UI library, so no cycle is introduced.
namespace engine {

// One declared setting: its key, optional type (empty for plain
// register_settings key:value pairs; "bool"|"int"|"string"|"choice" for a
// typed settings tab), default value, and type-specific metadata.
struct PluginSettingDef {
    std::string key;
    std::string type;               // "" | "bool" | "int" | "string" | "choice"
    std::string default_value;
    std::vector<std::string> choices;  // "choice": candidate values
    std::string int_range;              // "int": "min:max", empty = default
};

// Process-wide registry of plugin-declared settings. A plugin registers its
// settings via the v2 ABI callbacks; the engine stores the declarations and
// serves get/set through the host callbacks (gmm_host_get_setting /
// gmm_host_set_setting) so a plugin can read/write its own settings at runtime.
//
// Lifetime: declarations are dropped on plugin unload (clear_plugin) before
// dlclose, so no dangling references survive. Persisted values live in QSettings
// and intentionally outlive a single load (user edits persist across reloads).
class PluginSettingsRegistry {
public:
    static PluginSettingsRegistry& instance();

    // Register plain key:value settings (v2 register_settings). keys[i] pairs
    // with values[i] (the default); count = pair count. NULL keys/values or
    // count = 0 = no-op.
    void register_settings(const std::string& plugin_id,
                           const char* const* keys,
                           const char* const* values,
                           size_t count);

    // Register a typed settings tab (v2 register_settings_tab). keys[i] pairs
    // with types[i]/defaults[i]/options[i]; count = entry count. options
    // carries only what each type needs (newline candidates for choice,
    // min:max for int, else NULL).
    void register_settings_tab(const std::string& plugin_id,
                               const char* title,
                               const char* const* keys,
                               const char* const* types,
                               const char* const* defaults,
                               const char* const* options,
                               size_t count);

    // Map an alternate identifier (plugin_name / game_id) to the canonical
    // basename key, so host_get_setting("MyPlugin", key) resolves to the same
    // store the UI persists under the plugin's filename.
    void register_alias(const std::string& alias, const std::string& basename);

    // Host callbacks — read/write a plugin's setting at runtime. plugin_id may
    // be the basename or any registered alias. get_setting returns the persisted
    // value, or the registered default when unset, or "" when unknown.
    std::string get_setting(const std::string& plugin_id, const std::string& key) const;
    void set_setting(const std::string& plugin_id, const std::string& key,
                     const std::string& value);

    // Drop every declaration registered under the given plugin id (basename or
    // alias). Persisted QSettings values are intentionally left intact.
    void clear_plugin(const std::string& plugin_id);

    // Drop all declarations (process shutdown / full reload).
    void clear();

private:
    PluginSettingsRegistry() = default;

    // Resolve a plugin id (basename or alias) to the canonical basename used as
    // the QSettings key prefix. Unknown ids pass through unchanged.
    std::string resolve(const std::string& plugin_id) const;

    struct Store {
        std::string title;                       // settings_tab title (may be empty)
        std::map<std::string, PluginSettingDef> by_key;
    };

    mutable std::mutex mutex_;
    std::map<std::string, Store> stores_;        // keyed by basename
    std::map<std::string, std::string> aliases_; // alias -> basename
};

// Host callbacks exposed to plugins at runtime (C ABI). A plugin may dlsym
// these from the engine and call them outside registration to read/write its
// own settings. Returned string is engine-owned; the plugin must copy it.
extern "C" {
const char* gmm_host_get_setting(const char* plugin_name, const char* key);
void gmm_host_set_setting(const char* plugin_name, const char* key,
                          const char* value);
}

}  // namespace engine
