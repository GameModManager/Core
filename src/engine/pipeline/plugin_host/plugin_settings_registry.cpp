#include "engine/pipeline/plugin_host/plugin_settings_registry.h"

#include <QSettings>

namespace engine {

PluginSettingsRegistry& PluginSettingsRegistry::instance() {
    static PluginSettingsRegistry inst;
    return inst;
}

void PluginSettingsRegistry::register_settings(const std::string& plugin_id,
                                              const char* const* keys,
                                              const char* const* values,
                                              size_t count) {
    if (plugin_id.empty() || !keys || !values || count == 0) return;

    const std::string basename = resolve(plugin_id);
    std::lock_guard<std::mutex> lock(mutex_);

    Store& store = stores_[basename];
    for (size_t i = 0; i < count; ++i) {
        if (!keys[i] || !values[i]) continue;
        PluginSettingDef def;
        def.key = keys[i];
        def.default_value = values[i];
        store.by_key[def.key] = std::move(def);
    }
}

void PluginSettingsRegistry::register_settings_tab(const std::string& plugin_id,
                                                  const char* title,
                                                  const char* const* keys,
                                                  const char* const* types,
                                                  const char* const* defaults,
                                                  const char* const* options,
                                                  size_t count) {
    if (plugin_id.empty() || !title || !keys || !types || count == 0) return;

    const std::string basename = resolve(plugin_id);
    std::lock_guard<std::mutex> lock(mutex_);

    Store& store = stores_[basename];
    store.title = title;
    for (size_t i = 0; i < count; ++i) {
        if (!keys[i] || !types[i]) continue;
        PluginSettingDef def;
        def.key = keys[i];
        def.type = types[i];
        if (defaults && defaults[i]) def.default_value = defaults[i];
        if (options && options[i]) {
            if (def.type == "choice") {
                // newline-separated candidate choices
                std::string opts = options[i];
                size_t pos = 0;
                while ((pos = opts.find('\n')) != std::string::npos) {
                    def.choices.emplace_back(opts.substr(0, pos));
                    opts.erase(0, pos + 1);
                }
                if (!opts.empty()) def.choices.emplace_back(std::move(opts));
            } else if (def.type == "int") {
                def.int_range = options[i];
            }
        }
        store.by_key[def.key] = std::move(def);
    }
}

void PluginSettingsRegistry::register_alias(const std::string& alias,
                                           const std::string& basename) {
    if (alias.empty() || basename.empty() || alias == basename) return;
    std::lock_guard<std::mutex> lock(mutex_);
    aliases_[alias] = basename;
}

std::string PluginSettingsRegistry::get_setting(const std::string& plugin_id,
                                               const std::string& key) const {
    const std::string basename = resolve(plugin_id);
    QSettings settings("GameModManager", "GameModManager");
    const QString stored =
        settings.value(QString("plugins/settings/%1/%2").arg(
                          QString::fromStdString(basename),
                          QString::fromStdString(key)))
            .toString();
    if (!stored.isEmpty()) return stored.toStdString();

    // Fall back to the registered default when no value has been persisted.
    std::lock_guard<std::mutex> lock(mutex_);
    auto sit = stores_.find(basename);
    if (sit != stores_.end()) {
        auto kit = sit->second.by_key.find(key);
        if (kit != sit->second.by_key.end()) return kit->second.default_value;
    }
    return "";
}

void PluginSettingsRegistry::set_setting(const std::string& plugin_id,
                                        const std::string& key,
                                        const std::string& value) {
    const std::string basename = resolve(plugin_id);
    QSettings settings("GameModManager", "GameModManager");
    settings.setValue(QString("plugins/settings/%1/%2").arg(
                         QString::fromStdString(basename),
                         QString::fromStdString(key)),
                     QString::fromStdString(value));
}

void PluginSettingsRegistry::clear_plugin(const std::string& plugin_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string basename = resolve(plugin_id);
    stores_.erase(basename);
    // Drop aliases that pointed at this plugin.
    for (auto it = aliases_.begin(); it != aliases_.end();) {
        if (it->second == basename)
            it = aliases_.erase(it);
        else
            ++it;
    }
}

void PluginSettingsRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    stores_.clear();
    aliases_.clear();
}

std::string PluginSettingsRegistry::resolve(const std::string& plugin_id) const {
    if (stores_.count(plugin_id)) return plugin_id;
    auto it = aliases_.find(plugin_id);
    if (it != aliases_.end()) return it->second;
    return plugin_id;
}

// -- Host callbacks (C ABI) ---------------------------------------------------

namespace {
// Engine-owned result buffer for gmm_host_get_setting. The ABI contract says the
// plugin must copy the returned string, so a thread-local static is safe even
// under concurrent calls from different plugin threads.
thread_local std::string g_host_get_buffer;
}  // namespace

extern "C" {

const char* gmm_host_get_setting(const char* plugin_name, const char* key) {
    if (!plugin_name || !key) return "";
    g_host_get_buffer =
        PluginSettingsRegistry::instance().get_setting(plugin_name, key);
    return g_host_get_buffer.c_str();
}

void gmm_host_set_setting(const char* plugin_name, const char* key,
                          const char* value) {
    if (!plugin_name || !key) return;
    PluginSettingsRegistry::instance().set_setting(
        plugin_name, key, value ? value : "");
}

}  // extern "C"

}  // namespace engine
