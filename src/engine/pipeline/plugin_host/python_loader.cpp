#include "engine/pipeline/plugin_host/python_loader.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/pipeline/plugin_host/diagnostics_registry.h"
#include "engine/core/events/event_bus.h"
#include "engine/core/log/logger.h"
#include "engine/game/registry/game_features/game_feature_registry.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;

// -- Diagnostics bridge: a Python callable (plugin_name) -> list[str] bridged
//    to the ABI GmmDiagnosticsFn the engine registry expects. Providers are
//    owned here (they hold py::object) and cleared on interpreter shutdown.

namespace {

struct PyDiagnosticsProvider {
    py::object fn;
};

void py_diagnostics_bridge(const char* plugin_name,
                           char* out_buffer,
                           size_t out_capacity,
                           void* user_data) {
    auto* provider = static_cast<PyDiagnosticsProvider*>(user_data);
    if (!provider) return;

    py::gil_scoped_acquire acquire;
    try {
        py::object result = provider->fn(py::str(plugin_name));
        std::vector<std::string> messages;
        if (py::isinstance<py::str>(result)) {
            messages.push_back(py::cast<std::string>(result));
        } else if (py::isinstance<py::list>(result)) {
            for (auto item : py::cast<py::list>(result))
                if (py::isinstance<py::str>(item))
                    messages.push_back(py::cast<std::string>(item));
        }
        size_t off = 0;
        for (const auto& msg : messages) {
            if (off + msg.size() + 1 > out_capacity) break;
            std::memcpy(out_buffer + off, msg.data(), msg.size());
            off += msg.size();
            out_buffer[off++] = '\0';
        }
        if (off < out_capacity) out_buffer[off] = '\0';
    } catch (const py::error_already_set&) {
        PyErr_Clear();  // a broken provider must not crash the refresh
    }
}

std::vector<std::unique_ptr<PyDiagnosticsProvider>> g_py_providers;

// -- Event bridge: a Python callable (event_id: str, payload: dict) -> None
//    bridged to the bus. The host emits a JSON object string; the bridge
//    json.loads()s it so the Python handler receives a plain dict. Handlers
//    are owned here (they hold py::object) and cleared on interpreter
//    shutdown, matching the diagnostics providers above.

struct PyEventHandler {
    py::object fn;
};

void py_event_bridge(const char* event_id,
                     const char* json_payload,
                     void* user_data) {
    auto* handler = static_cast<PyEventHandler*>(user_data);
    if (!handler) return;

    py::gil_scoped_acquire acquire;
    try {
        py::object parsed =
            py::module_::import("json").attr("loads")(py::str(json_payload));
        handler->fn(py::str(event_id), parsed);
    } catch (const py::error_already_set&) {
        PyErr_Clear();  // a broken handler must not crash the emitter
    }
}

std::vector<std::unique_ptr<PyEventHandler>> g_py_handlers;

}  // namespace

// -- gmm.RegistrationContext - Python-side wrapper --

class PyRegistrationContext {
public:
    PyRegistrationContext(engine::PluginLoader* loader, engine::PluginInfo* plugin)
        : loader_(loader), plugin_(plugin) {}

    void register_identity(uint32_t steam_appid,
                           const std::string& gog_id,
                           const std::string& epic_namespace,
                           const std::string& nexus_domain,
                           const std::string& display_name,
                           const std::string& exe_windows,
                           const std::string& exe_linux,
                           const std::string& exe_macos) {
        plugin_->steam_appid = steam_appid;
        plugin_->nexus_domain = nexus_domain;
        if (!display_name.empty())
            plugin_->game_display_name = display_name;
        plugin_->game_support = true;
        engine::Logger::instance().debug("Python plugin registered identity: appid=" +
            std::to_string(steam_appid) + " name=" +
            (display_name.empty() ? plugin_->game_id : display_name) +
            " nexus=" + nexus_domain);
    }

    void register_meta(const std::string& author,
                       const std::string& version,
                       const std::string& description) {
        plugin_->author = author;
        plugin_->version = version;
        plugin_->description = description;
    }

    void register_category(const std::string& category) {
        plugin_->category = category;
    }

    void register_settings(const std::vector<std::pair<std::string, std::string>>& settings) {
        plugin_->settings = settings;
    }

    // P1.5 typed settings tab — the pybind mirror of the ABI
    // register_settings_tab entry. Each entry is a (key, type, default,
    // options) tuple; options is None except for type "choice" (a list of
    // candidate choices) or "int" (the "min:max" range string).
    void register_settings_tab(const std::string& title,
                               const std::vector<py::tuple>& settings) {
        if (title.empty()) {
            engine::Logger::instance().warn(
                "register_settings_tab: empty title - ignored");
            return;
        }
        engine::PluginInfo::SettingTab tab;
        tab.title = title;
        for (const auto& t : settings) {
            if (t.size() < 3) continue;
            engine::PluginInfo::SettingTabEntry entry;
            entry.key = py::cast<std::string>(t[0]);
            entry.type = py::cast<std::string>(t[1]);
            entry.default_value = py::cast<std::string>(t[2]);
            if (t.size() >= 4 && !t[3].is_none()) {
                if (entry.type == "choice") {
                    for (const auto& o : py::cast<std::vector<std::string>>(t[3]))
                        entry.choices.push_back(o);
                } else if (entry.type == "int") {
                    entry.int_range = py::cast<std::string>(t[3]);
                }
            }
            tab.settings.push_back(std::move(entry));
        }
        plugin_->settings_tab = std::move(tab);
    }

    void register_diagnostics(py::object fn) {
        if (!py::isinstance<py::function>(fn)) {
            engine::Logger::instance().warn(
                "register_diagnostics: fn is not a callable - ignored");
            return;
        }
        auto provider = std::make_unique<PyDiagnosticsProvider>();
        provider->fn = std::move(fn);
        void* user_data = provider.get();
        g_py_providers.push_back(std::move(provider));
        engine::DiagnosticsRegistry::instance().register_provider(
            plugin_->game_id, py_diagnostics_bridge, user_data);
    }

    // P1.3 event subscription — the pybind mirror of the ABI subscribe_event
    // entry. fn is called as fn(event_id: str, payload: dict) whenever the
    // host emits the event; the JSON payload is decoded by the bridge so the
    // plugin sees a plain dict (see the event id doc in gmm_abi_v1.h).
    void subscribe_event(const std::string& event_id, py::object fn) {
        if (event_id.empty()) {
            engine::Logger::instance().warn(
                "subscribe_event: empty event_id - ignored");
            return;
        }
        if (!py::isinstance<py::function>(fn)) {
            engine::Logger::instance().warn(
                "subscribe_event: fn is not a callable - ignored");
            return;
        }
        auto handler = std::make_unique<PyEventHandler>();
        handler->fn = std::move(fn);
        void* user_data = handler.get();
        g_py_handlers.push_back(std::move(handler));
        engine::EventBus::instance().subscribe(
            event_id,
            [user_data](const std::string& eid, const std::string& payload) {
                py_event_bridge(eid.c_str(), payload.c_str(), user_data);
            },
            plugin_->path);
        engine::Logger::instance().debug(
            "Python plugin subscribed to event: " + event_id);
    }

    // P1.2 GameFeatureRegistry (MO2 IGameFeatures analogue): registers or
    // overrides a per-game behavior feature with priority + replace. The
    // game's own feature registers at the lowest priority; a plugin
    // registering the same feature_type at a higher priority overrides it.
    // Mirrors the register_game_feature C ABI in gmm_abi_v1.h.
    void register_game_feature(const std::string& game_id,
                               const std::string& feature_type,
                               int priority,
                               const std::vector<std::string>& folder_names,
                               const std::vector<std::string>& file_extensions) {
        std::string gid = game_id.empty() ? plugin_->game_id : game_id;
        if (gid.empty() || feature_type.empty()) {
            engine::Logger::instance().warn(
                "register_game_feature: empty game_id/feature_type - ignored");
            return;
        }
        if (feature_type == "mod_data_checker") {
            auto checker = std::make_shared<engine::ModDataCheckerFeature>(
                folder_names, file_extensions);
            engine::GameFeatureRegistry::instance().register_feature(
                gid, feature_type, priority, checker, plugin_->path);
        } else if (feature_type == "game_plugins") {
            // The game's vanilla plugin files (MO2 GamePlugins::gamePlugins()).
            auto feature = std::make_shared<engine::GamePluginsFeature>(folder_names);
            engine::GameFeatureRegistry::instance().register_feature(
                gid, feature_type, priority, feature, plugin_->path);
        } else {
            engine::Logger::instance().warn(
                "register_game_feature: unknown feature type '" + feature_type +
                "' - ignored");
            return;
        }
        engine::Logger::instance().debug("Python plugin registered game feature: " +
            feature_type + " (game=" + gid + ", priority=" +
            std::to_string(priority) + ")");
    }

    // P1.2 GameFeatureRegistry, key/value variant: the 7 structured-data
    // feature types (mod_data_content, data_archives, script_extender,
    // save_game_info, local_savegames, unmanaged_mods, bsa_invalidation).
    // Mirrors the register_game_feature_data C ABI in gmm_abi_v1.h; the kv
    // parse lives in the shared engine::register_game_feature_data so both
    // surfaces agree byte-for-byte.
    void register_game_feature_data(const std::string& game_id,
                                    const std::string& feature_type,
                                    int priority,
                                    py::dict data) {
        std::string gid = game_id.empty() ? plugin_->game_id : game_id;
        if (gid.empty() || feature_type.empty()) {
            engine::Logger::instance().warn(
                "register_game_feature_data: empty game_id/feature_type - ignored");
            return;
        }
        std::vector<std::pair<std::string, std::string>> kv;
        for (auto item : data) {
            kv.emplace_back(py::cast<std::string>(item.first),
                            py::cast<std::string>(py::str(item.second)));
        }
        if (!engine::register_game_feature_data(gid, feature_type, priority,
                                                std::move(kv), plugin_->path)) {
            return;  // register_game_feature_data already logged the reason
        }
        engine::Logger::instance().debug(
            "Python plugin registered game feature: " + feature_type +
            " (game=" + gid + ", priority=" + std::to_string(priority) + ")");
    }

    void register_stage_claim(const std::string& stage_name, int priority) {
        (void)stage_name;
        (void)priority;
    }

    void register_wildcard_stage_claim(const std::string& game_id,
                                       const std::string& stage_name,
                                       int priority) {
        (void)game_id;
        (void)stage_name;
        (void)priority;
    }

    void register_order_encoding_hook() {
    }

    void register_deploy_strategy() {
    }

    void register_image_diff() {
    }

    void register_tool(const std::string& tool_id, const std::string& kind) {
        engine::ExternalTool tool;
        tool.tool_id = tool_id;
        tool.game_id = plugin_->game_id;
        tool.display_name = tool_id;
        tool.kind = (kind == "workshop") ? engine::ToolKind::Workshop : engine::ToolKind::Advisory;
        loader_->tool_registry().register_tool(tool);
    }

    void register_capability(const std::string& capability,
                             const std::string& display_name,
                             const std::string& data_path,
                             const std::string& description,
                             const std::string& protocol_handler,
                             const std::string& website_domain,
                             const std::string& supported_platforms) {
        engine::CapabilityInfo info;
        info.game_id = plugin_->game_id;
        info.capability = capability;
        info.display_name = display_name.empty() ? capability : display_name;
        info.data_path = data_path;
        info.description = description;
        info.protocol_handler = protocol_handler;
        info.website_domain = website_domain;

        if (!supported_platforms.empty()) {
            std::string s = supported_platforms;
            size_t pos;
            while ((pos = s.find(',')) != std::string::npos) {
                info.supported_platforms.push_back(s.substr(0, pos));
                s.erase(0, pos + 1);
            }
            if (!s.empty()) info.supported_platforms.push_back(s);
        }

        loader_->capabilities().register_capability(info);
    }

    void register_tab(const std::string& capability,
                      const std::string& display_name,
                      const std::string& data_path,
                      const std::string& description,
                      const std::string& protocol_handler,
                      const std::string& website_domain,
                      const std::string& supported_platforms,
                      const std::string& insert_before,
                      const std::string& insert_after) {
        engine::CapabilityInfo info;
        info.game_id = plugin_->game_id;
        info.capability = capability;
        info.display_name = display_name.empty() ? capability : display_name;
        info.data_path = data_path;
        info.description = description;
        info.protocol_handler = protocol_handler;
        info.website_domain = website_domain;
        info.insert_before = insert_before;
        info.insert_after = insert_after;

        if (!supported_platforms.empty()) {
            std::string s = supported_platforms;
            size_t pos;
            while ((pos = s.find(',')) != std::string::npos) {
                info.supported_platforms.push_back(s.substr(0, pos));
                s.erase(0, pos + 1);
            }
            if (!s.empty()) info.supported_platforms.push_back(s);
        }

        loader_->capabilities().register_capability(info);
    }

    [[nodiscard]] std::string game_id() const { return plugin_->game_id; }

private:
    engine::PluginLoader* loader_;
    engine::PluginInfo* plugin_;
};

// -- Embedded gmm module --

PYBIND11_EMBEDDED_MODULE(gmm, m) {
    m.doc() = "GameModManager Python plugin API";

    py::class_<PyRegistrationContext>(m, "RegistrationContext")
        .def("register_identity", &PyRegistrationContext::register_identity,
             py::arg("steam_appid") = 0,
             py::arg("gog_id") = "",
             py::arg("epic_namespace") = "",
             py::arg("nexus_domain") = "",
             py::arg("display_name") = "",
             py::arg("exe_windows") = "",
             py::arg("exe_linux") = "",
             py::arg("exe_macos") = "")
        .def("register_stage_claim", &PyRegistrationContext::register_stage_claim,
             py::arg("stage_name"), py::arg("priority") = 0)
        .def("register_wildcard_stage_claim",
             &PyRegistrationContext::register_wildcard_stage_claim,
             py::arg("game_id") = "",
             py::arg("stage_name"),
             py::arg("priority") = 0)
        .def("register_meta", &PyRegistrationContext::register_meta,
             py::arg("author") = "",
             py::arg("version") = "",
             py::arg("description") = "")
        .def("register_category", &PyRegistrationContext::register_category,
             py::arg("category") = "")
        .def("register_settings", &PyRegistrationContext::register_settings,
             py::arg("settings"))
        .def("register_settings_tab", &PyRegistrationContext::register_settings_tab,
             py::arg("title"), py::arg("settings"))
        .def("register_diagnostics", &PyRegistrationContext::register_diagnostics,
             py::arg("fn"))
        .def("subscribe_event", &PyRegistrationContext::subscribe_event,
             py::arg("event_id"), py::arg("fn"))
        .def("register_game_feature", &PyRegistrationContext::register_game_feature,
             py::arg("game_id") = "",
             py::arg("feature_type"),
             py::arg("priority") = 0,
             py::arg("folder_names") = std::vector<std::string>{},
             py::arg("file_extensions") = std::vector<std::string>{})
        .def("register_game_feature_data",
             &PyRegistrationContext::register_game_feature_data,
             py::arg("game_id") = "",
             py::arg("feature_type"),
             py::arg("priority") = 0,
             py::arg("data") = py::dict())
        .def("register_order_encoding_hook", &PyRegistrationContext::register_order_encoding_hook)
        .def("register_deploy_strategy", &PyRegistrationContext::register_deploy_strategy)
        .def("register_image_diff", &PyRegistrationContext::register_image_diff)
        .def("register_tool", &PyRegistrationContext::register_tool,
             py::arg("tool_id"), py::arg("kind"))
        .def("register_capability", &PyRegistrationContext::register_capability,
             py::arg("capability"),
             py::arg("display_name") = "",
             py::arg("data_path") = "",
             py::arg("description") = "",
             py::arg("protocol_handler") = "",
             py::arg("website_domain") = "",
             py::arg("supported_platforms") = "")
        .def("register_tab", &PyRegistrationContext::register_tab,
             py::arg("capability"),
             py::arg("display_name") = "",
             py::arg("data_path") = "",
             py::arg("description") = "",
             py::arg("protocol_handler") = "",
             py::arg("website_domain") = "",
             py::arg("supported_platforms") = "",
             py::arg("insert_before") = "",
             py::arg("insert_after") = "")
        .def_property_readonly("game_id", &PyRegistrationContext::game_id);
}

// -- Interpreter lifecycle --

static std::unique_ptr<py::scoped_interpreter> s_interpreter;

bool engine::python_init() {
    if (s_interpreter) return true;

    try {
        s_interpreter = std::make_unique<py::scoped_interpreter>();
        Logger::instance().debug("Python interpreter initialized");
        return true;
    } catch (const std::exception& e) {
        Logger::instance().error("Failed to initialize Python: " + std::string(e.what()));
        return false;
    }
}

bool engine::python_load_plugin(PluginLoader* loader, const std::string& path) {
    if (!s_interpreter) {
        Logger::instance().error("Python not initialized, cannot load: " + path);
        return false;
    }

    if (loader->is_loaded(path)) {
        Logger::instance().warn("Python plugin already loaded: " + path);
        return true;
    }

    try {
        py::gil_scoped_acquire acquire;

        std::string module_name = std::filesystem::path(path).stem().string();
        std::string plugin_dir = std::filesystem::path(path).parent_path().string();

        // Add plugin directory to sys.path for relative imports
        py::module_ sys = py::module_::import("sys");
        py::list path_list = sys.attr("path");
        path_list.insert(0, plugin_dir);

        // Import the plugin module
        py::module_ plugin_module = py::module_::import(module_name.c_str());

        // Check for register() function
        if (!py::hasattr(plugin_module, "register")) {
            Logger::instance().error("Python plugin missing register(): " + path);
            return false;
        }

        py::object register_fn = plugin_module.attr("register");

        // Build PluginInfo
        engine::PluginInfo info;
        info.path = path;
        info.game_id = module_name;
        info.game_display_name = info.game_id;  // fallback, overridden by register_identity
        info.abi_version = 0;
        info.loaded = true;

        // Create context and call register()
        PyRegistrationContext ctx(loader, &info);
        register_fn(ctx);

        info.registered = true;
        loader->add_loaded_plugin(std::move(info));

        Logger::instance().debug("Python plugin registered: " + info.game_display_name +
            " (" + path + ", game=" + info.game_id +
            ", appid=" + std::to_string(info.steam_appid) + ")");
        return true;

    } catch (const py::error_already_set& e) {
        Logger::instance().error("Python plugin error: " + path + " - " + e.what());
        return false;
    } catch (const std::exception& e) {
        Logger::instance().error("Failed to load Python plugin: " + path + " - " + e.what());
        return false;
    }
}

void engine::python_shutdown() {
    {
        // Destroy Python-side event handlers + diagnostics providers with the
        // GIL held. The event bus is cleared FIRST so no bus callback can
        // still run against a destroyed handler during interpreter teardown.
        py::gil_scoped_acquire acquire;
        EventBus::instance().clear();
        DiagnosticsRegistry::instance().clear();
        g_py_providers.clear();
        g_py_handlers.clear();
        GameFeatureRegistry::instance().clear();
    }
    s_interpreter.reset();
}
