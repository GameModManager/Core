#include "engine/plugin_host/python_loader.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/log/logger.h"

#include <filesystem>
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;

// -- gmm.RegistrationContext — Python-side wrapper --

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
        engine::Logger::instance().debug("Python plugin registered identity: appid=" +
            std::to_string(steam_appid) + " name=" +
            (display_name.empty() ? plugin_->game_id : display_name) +
            " nexus=" + nexus_domain);
    }

    void register_stage_claim(const std::string& stage_name, int priority) {
        engine::Logger::instance().debug("Python plugin registered stage claim: " + stage_name +
            " (game=" + plugin_->game_id + ", priority=" + std::to_string(priority) + ")");
    }

    void register_order_encoding_hook() {
        engine::Logger::instance().debug("Python plugin registered order encoding hook");
    }

    void register_deploy_strategy() {
        engine::Logger::instance().debug("Python plugin registered deploy strategy");
    }

    void register_image_diff() {
        engine::Logger::instance().debug("Python plugin registered image diff provider (stub)");
    }

    void register_tool(const std::string& tool_id, const std::string& kind) {
        engine::ExternalTool tool;
        tool.tool_id = tool_id;
        tool.game_id = plugin_->game_id;
        tool.display_name = tool_id;
        tool.kind = (kind == "workshop") ? engine::ToolKind::Workshop : engine::ToolKind::Advisory;
        loader_->tool_registry().register_tool(tool);

        engine::Logger::instance().debug("Python plugin registered tool: " + tool_id +
            " (" + kind + ")");
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

        Logger::instance().debug("Python plugin registered: " + path +
            " (game=" + info.game_id +
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
    s_interpreter.reset();
}
