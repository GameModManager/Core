#include "engine/pipeline/plugin_host/python_loader.h"
#include "engine/core/events/event_bus.h"
#include "engine/core/log/logger.h"
#include "engine/core/vfs/path_resolver.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/saves/save_reader.h"
#include "engine/pipeline/pipeline.h"
#include "engine/pipeline/plugin_host/deploy_strategy_registry.h"
#include "engine/pipeline/plugin_host/diagnose_registry.h"
#include "engine/pipeline/plugin_host/diagnostics_registry.h"
#include "engine/pipeline/plugin_host/file_mapper_registry.h"
#include "engine/pipeline/plugin_host/hook_registry.h"
#include "engine/pipeline/plugin_host/order_encoding_registry.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/pipeline/plugin_host/plugin_settings_registry.h"
#include "engine/pipeline/plugin_host/requirements_registry.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"
#include "engine/pipeline/plugin_host/tool_registry.h"
#include "engine/sort/abi_sort_provider.h"
#include "engine/sort/sort_registry.h"
#include "ui/preview/preview_registry.h"

#include "gmm_abi_v2.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include <vector>

namespace py = pybind11;

// -- Diagnostics bridge: a Python callable (plugin_name) -> list[str] bridged
//    to the ABI GmmDiagnosticsFn the engine registry expects. Providers are
//    owned here (they hold py::object) and cleared on interpreter shutdown.

namespace
{

struct PyDiagnosticsProvider
{
  py::object fn;
};

void py_diagnostics_bridge(const char* plugin_name, char* out_buffer,
                           size_t out_capacity, void* user_data)
{
  auto* provider = static_cast<PyDiagnosticsProvider*>(user_data);
  if (!provider)
    return;

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
      if (off + msg.size() + 1 > out_capacity)
        break;
      std::memcpy(out_buffer + off, msg.data(), msg.size());
      off += msg.size();
      out_buffer[off++] = '\0';
    }
    if (off < out_capacity)
      out_buffer[off] = '\0';
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python diagnostics bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();  // a broken provider must not crash the refresh
  }
}

std::vector<std::unique_ptr<PyDiagnosticsProvider>> g_py_providers;

// -- Event bridge: a Python callable (event_id: str, payload: dict) -> None
//    bridged to the bus. The host emits a JSON object string; the bridge
//    json.loads()s it so the Python handler receives a plain dict. Handlers
//    are owned here (they hold py::object) and cleared on interpreter
//    shutdown, matching the diagnostics providers above.

struct PyEventHandler
{
  py::object fn;
};

void py_event_bridge(const char* event_id, const char* json_payload, void* user_data)
{
  auto* handler = static_cast<PyEventHandler*>(user_data);
  if (!handler)
    return;

  py::gil_scoped_acquire acquire;
  try {
    py::object parsed =
        py::module_::import("json").attr("loads")(py::str(json_payload));
    handler->fn(py::str(event_id), parsed);
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python event bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();  // a broken handler must not crash the emitter
  }
}

std::vector<std::unique_ptr<PyEventHandler>> g_py_handlers;

// ---------------------------------------------------------------------------
// v2 interface bridges. Each provider holds the Python callable plus any
// persistent storage the bridge needs to marshal C-struct returns back to the
// engine. The engine copies returned arrays synchronously, but the const char*
// fields point into plugin-owned memory that must outlive the call, so we keep
// the backing strings alive in the provider until interpreter shutdown.
// ---------------------------------------------------------------------------

static char* dup_str(const std::string& s)
{
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (p) {
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
  }
  return p;
}

static std::string basename_of(const std::string& path)
{
  return std::filesystem::path(path).filename().string();
}

// -- Requirements (GmmRequirementsFn) --
struct PyRequirementsProvider
{
  py::object fn;
  std::vector<std::string> owned;
  std::vector<GmmPluginRequirement> array;
};
std::vector<std::unique_ptr<PyRequirementsProvider>> g_py_req_providers;

GmmPluginRequirement* py_requirements_bridge(size_t* out_count, void* user_data)
{
  auto* p = static_cast<PyRequirementsProvider*>(user_data);
  if (!p) {
    *out_count = 0;
    return nullptr;
  }
  py::gil_scoped_acquire acquire;
  try {
    py::object result = p->fn();
    p->array.clear();
    if (py::isinstance<py::list>(result)) {
      for (auto item : py::cast<py::list>(result)) {
        std::string type, name, message;
        if (py::isinstance<py::tuple>(item) && py::len(item) >= 3) {
          py::tuple t = py::cast<py::tuple>(item);
          type        = py::cast<std::string>(t[0]);
          name        = py::cast<std::string>(t[1]);
          message     = py::cast<std::string>(t[2]);
        } else if (py::isinstance<py::dict>(item)) {
          py::dict d = py::cast<py::dict>(item);
          type       = py::cast<std::string>(d["type"]);
          name       = py::cast<std::string>(d["name"]);
          message    = py::cast<std::string>(d["message"]);
        } else {
          continue;
        }
        p->owned.push_back(std::move(type));
        p->owned.push_back(std::move(name));
        p->owned.push_back(std::move(message));
        GmmPluginRequirement r{};
        r.type    = p->owned[p->owned.size() - 3].c_str();
        r.name    = p->owned[p->owned.size() - 2].c_str();
        r.message = p->owned[p->owned.size() - 1].c_str();
        p->array.push_back(r);
      }
    }
    *out_count = p->array.size();
    return p->array.data();
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python requirements bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();
    *out_count = 0;
    return nullptr;
  }
}

// -- Diagnostics v2 (GmmDiagnoseFn) --
struct PyGuidedFixProvider
{
  py::object fn;
};

void py_guided_fix_bridge(void* user_data)
{
  auto* fix = static_cast<PyGuidedFixProvider*>(user_data);
  if (!fix)
    return;
  py::gil_scoped_acquire acquire;
  try {
    fix->fn();
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python guided fix bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();
  }
}

struct PyDiagnoseProvider
{
  py::object fn;
  std::vector<std::string> owned;
  std::vector<GmmDiagnosticProblem> array;
  std::vector<std::unique_ptr<PyGuidedFixProvider>> fixes;
};
std::vector<std::unique_ptr<PyDiagnoseProvider>> g_py_diagnose_providers;

// Extract a short description string from a problem item returned by a Python
// diagnostics callable (accepts a bare string, a (short, full) tuple, or a
// dict with short_description / full_description).
static std::string diag_short_desc(py::handle item)
{
  try {
    if (py::isinstance<py::str>(item)) {
      return py::cast<std::string>(item);
    } else if (py::isinstance<py::tuple>(item) && py::len(item) >= 1) {
      return py::cast<std::string>(item[0]);
    } else if (py::isinstance<py::dict>(item)) {
      auto d = py::cast<py::dict>(item);
      if (d.contains("short_description"))
        return py::cast<std::string>(d["short_description"]);
      if (d.contains("full_description"))
        return py::cast<std::string>(d["full_description"]);
    }
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python diag_short_desc error: " +
                                    std::string(e.what()));
    PyErr_Clear();
  }
  return "";
}

// v1 DiagnosticsRegistry bridge: fill the buffer with short descriptions so the
// Plugins tab (PluginLoader::collect_diagnostics) can render them.
void py_diag_v1_bridge(const char* plugin_name, char* out_buffer, size_t out_capacity,
                       void* user_data)
{
  auto* p = static_cast<PyDiagnoseProvider*>(user_data);
  if (!p)
    return;
  py::gil_scoped_acquire acquire;
  try {
    py::object result = p->fn(py::str(plugin_name));
    std::vector<std::string> messages;
    if (py::isinstance<py::list>(result)) {
      for (auto item : py::cast<py::list>(result)) {
        std::string msg = diag_short_desc(item);
        if (!msg.empty())
          messages.push_back(std::move(msg));
      }
    }
    size_t off = 0;
    for (const auto& msg : messages) {
      if (off + msg.size() + 1 > out_capacity)
        break;
      std::memcpy(out_buffer + off, msg.data(), msg.size());
      off += msg.size();
      out_buffer[off++] = '\0';
    }
    if (off < out_capacity)
      out_buffer[off] = '\0';
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python diag v1 bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();
  }
}

// v2 DiagnoseRegistry bridge: return an array of GmmDiagnosticProblem.
GmmDiagnosticProblem* py_diag_v2_bridge(size_t* out_count, void* user_data)
{
  auto* p = static_cast<PyDiagnoseProvider*>(user_data);
  if (!p) {
    *out_count = 0;
    return nullptr;
  }
  py::gil_scoped_acquire acquire;
  try {
    py::object result = p->fn();
    p->array.clear();
    if (py::isinstance<py::list>(result)) {
      for (auto item : py::cast<py::list>(result)) {
        std::string short_desc, full_desc;
        int has_fix = 0;
        py::object fix_fn;
        if (py::isinstance<py::tuple>(item)) {
          py::tuple t = py::cast<py::tuple>(item);
          if (py::len(t) >= 1)
            short_desc = py::cast<std::string>(t[0]);
          if (py::len(t) >= 2)
            full_desc = py::cast<std::string>(t[1]);
          if (py::len(t) >= 3)
            has_fix = py::cast<int>(t[2]);
          if (py::len(t) >= 4)
            fix_fn = t[3];
        } else if (py::isinstance<py::dict>(item)) {
          auto d = py::cast<py::dict>(item);
          if (d.contains("short_description"))
            short_desc = py::cast<std::string>(d["short_description"]);
          if (d.contains("full_description"))
            full_desc = py::cast<std::string>(d["full_description"]);
          if (d.contains("has_guided_fix"))
            has_fix = py::cast<int>(d["has_guided_fix"]);
          if (d.contains("start_guided_fix"))
            fix_fn = d["start_guided_fix"];
        } else {
          continue;
        }
        p->owned.push_back(std::move(short_desc));
        p->owned.push_back(std::move(full_desc));
        GmmDiagnosticProblem prob{};
        prob.short_description = p->owned[p->owned.size() - 2].c_str();
        prob.full_description  = p->owned[p->owned.size() - 1].c_str();
        prob.has_guided_fix    = has_fix;
        if (has_fix && !fix_fn.is_none() && py::isinstance<py::function>(fix_fn)) {
          auto fix              = std::make_unique<PyGuidedFixProvider>();
          fix->fn               = fix_fn;
          prob.start_guided_fix = py_guided_fix_bridge;
          prob.user_data        = fix.get();
          p->fixes.push_back(std::move(fix));
        } else {
          prob.start_guided_fix = nullptr;
          prob.user_data        = nullptr;
        }
        p->array.push_back(prob);
      }
    }
    *out_count = p->array.size();
    return p->array.data();
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python diag v2 bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();
    *out_count = 0;
    return nullptr;
  }
}

// -- File mapper (GmmFileMapperFn) --
struct PyFileMapperProvider
{
  py::object fn;
  std::vector<std::string> owned;
  std::vector<GmmFileMapping> array;
};
std::vector<std::unique_ptr<PyFileMapperProvider>> g_py_file_mapper_providers;

GmmFileMapping* py_file_mapper_bridge(size_t* out_count, void* user_data)
{
  auto* p = static_cast<PyFileMapperProvider*>(user_data);
  if (!p) {
    *out_count = 0;
    return nullptr;
  }
  py::gil_scoped_acquire acquire;
  try {
    py::object result = p->fn();
    p->array.clear();
    if (py::isinstance<py::list>(result)) {
      for (auto item : py::cast<py::list>(result)) {
        std::string source, target;
        if (py::isinstance<py::tuple>(item) && py::len(item) >= 2) {
          py::tuple t = py::cast<py::tuple>(item);
          source      = py::cast<std::string>(t[0]);
          target      = py::cast<std::string>(t[1]);
        } else if (py::isinstance<py::dict>(item)) {
          py::dict d = py::cast<py::dict>(item);
          source     = py::cast<std::string>(d["source"]);
          target     = py::cast<std::string>(d["target"]);
        } else {
          continue;
        }
        p->owned.push_back(std::move(source));
        p->owned.push_back(std::move(target));
        GmmFileMapping m{};
        m.source = p->owned[p->owned.size() - 2].c_str();
        m.target = p->owned[p->owned.size() - 1].c_str();
        p->array.push_back(m);
      }
    }
    *out_count = p->array.size();
    return p->array.data();
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python file mapper bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();
    *out_count = 0;
    return nullptr;
  }
}

// -- Order encoding (GmmOrderEncodingFnV2) --
struct PyOrderEncodingProvider
{
  py::object fn;
};
std::vector<std::unique_ptr<PyOrderEncodingProvider>> g_py_order_encoding_providers;

int py_order_encoding_bridge(const char* const* ordered_mod_ids, size_t count,
                             const char* output_path, void* user_data)
{
  auto* p = static_cast<PyOrderEncodingProvider*>(user_data);
  if (!p)
    return 0;
  py::gil_scoped_acquire acquire;
  try {
    std::vector<std::string> ids;
    for (size_t i = 0; i < count; ++i)
      ids.emplace_back(ordered_mod_ids[i] ? ordered_mod_ids[i] : "");
    py::object result = p->fn(ids, py::str(output_path ? output_path : ""));
    return py::cast<int>(result) != 0 ? 1 : 0;
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python order encoding bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();
    return 0;
  }
}

// -- Deploy strategy (GmmDeployFnV2 / GmmRemoveFnV2) --
struct PyDeployProvider
{
  py::object deploy_fn;
  py::object remove_fn;
};
std::vector<std::unique_ptr<PyDeployProvider>> g_py_deploy_providers;

int py_deploy_bridge(const char* source, const char* target, void* user_data)
{
  auto* p = static_cast<PyDeployProvider*>(user_data);
  if (!p || p->deploy_fn.is_none())
    return 0;
  py::gil_scoped_acquire acquire;
  try {
    py::object result =
        p->deploy_fn(py::str(source ? source : ""), py::str(target ? target : ""));
    return py::cast<int>(result) != 0 ? 1 : 0;
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python deploy bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();
    return 0;
  }
}

int py_remove_bridge(const char* target, void* user_data)
{
  auto* p = static_cast<PyDeployProvider*>(user_data);
  if (!p || p->remove_fn.is_none())
    return 0;
  py::gil_scoped_acquire acquire;
  try {
    py::object result = p->remove_fn(py::str(target ? target : ""));
    return py::cast<int>(result) != 0 ? 1 : 0;
  } catch (const py::error_already_set& e) {
    engine::Logger::instance().warn("Python remove bridge error: " +
                                    std::string(e.what()));
    PyErr_Clear();
    return 0;
  }
}

// -- Hook (GmmHookFnV2) --
struct PyHookProvider
{
  py::object fn;
};
std::vector<std::unique_ptr<PyHookProvider>> g_py_hook_providers;

void py_hook_bridge(const char* tag, void* data, void* user_data)
{
  auto* p = static_cast<PyHookProvider*>(user_data);
  if (!p)
    return;
  py::gil_scoped_acquire acquire;
  try {
    const char* d = static_cast<const char*>(data);
    p->fn(py::str(tag ? tag : ""), py::str(d ? d : ""));
  } catch (const py::error_already_set&) {
    PyErr_Clear();
  }
}

// -- Tool (GmmToolInvokeFn) --
struct PyToolProvider
{
  py::object fn;
};
std::vector<std::unique_ptr<PyToolProvider>> g_py_tool_providers;

void py_tool_bridge(void* user_data)
{
  auto* p = static_cast<PyToolProvider*>(user_data);
  if (!p)
    return;
  py::gil_scoped_acquire acquire;
  try {
    p->fn();
  } catch (const py::error_already_set&) {
    PyErr_Clear();
  }
}

// -- Preview (GmmPreviewFn) --
// The Python callable returns the raw QWidget* as an int (e.g. obtained via
// shiboken6.getCppPointer(widget)[0] when building the widget with PySide6).
// The engine stores the void* exactly like a C plugin; the UI casts it back to
// QWidget*. The returned Python object is kept alive so the widget is not
// garbage-collected while embedded.
struct PyPreviewProvider
{
  py::object fn;
  py::object keep_alive;
};
std::vector<std::unique_ptr<PyPreviewProvider>> g_py_preview_providers;

void* py_preview_bridge(const char* file_path, void* preview_data, void* user_data)
{
  auto* p = static_cast<PyPreviewProvider*>(user_data);
  if (!p)
    return nullptr;
  py::gil_scoped_acquire acquire;
  try {
    py::object result = p->fn(py::str(file_path ? file_path : ""));
    p->keep_alive     = result;
    if (py::isinstance<py::int_>(result)) {
      return reinterpret_cast<void*>(py::cast<uintptr_t>(result));
    }
    return nullptr;
  } catch (const py::error_already_set&) {
    PyErr_Clear();
    return nullptr;
  }
}

// -- ModPage download (GmmModPageDownloadFn) --
struct PyModPageProvider
{
  py::object fn;
};
std::vector<std::unique_ptr<PyModPageProvider>> g_py_modpage_providers;

int py_modpage_bridge(const char* url, const char* output_path, void* user_data)
{
  auto* p = static_cast<PyModPageProvider*>(user_data);
  if (!p)
    return 0;
  py::gil_scoped_acquire acquire;
  try {
    py::object result =
        p->fn(py::str(url ? url : ""), py::str(output_path ? output_path : ""));
    return py::cast<int>(result) != 0 ? 1 : 0;
  } catch (const py::error_already_set&) {
    PyErr_Clear();
    return 0;
  }
}

// -- Save parser (GmmSaveParserFnV2) --
struct PySaveParserProvider
{
  py::object fn;
};
std::vector<std::unique_ptr<PySaveParserProvider>> g_py_save_parser_providers;

int py_save_parser_bridge(const char* path, const char* game_id, GmmSaveDataV2* out,
                          void* user_data)
{
  auto* p = static_cast<PySaveParserProvider*>(user_data);
  if (!p || !out)
    return 0;
  py::gil_scoped_acquire acquire;
  try {
    py::object result =
        p->fn(py::str(path ? path : ""), py::str(game_id ? game_id : ""));
    if (result.is_none())
      return 0;
    auto d = py::cast<py::dict>(result);
    std::memset(out, 0, sizeof(*out));
    if (d.contains("file_path"))
      out->file_path = dup_str(py::cast<std::string>(d["file_path"]));
    if (d.contains("game_id"))
      out->game_id = dup_str(py::cast<std::string>(d["game_id"]));
    if (d.contains("creation_time"))
      out->creation_time = py::cast<int64_t>(d["creation_time"]);
    if (d.contains("pc_name"))
      out->pc_name = dup_str(py::cast<std::string>(d["pc_name"]));
    if (d.contains("pc_level"))
      out->pc_level = py::cast<int32_t>(d["pc_level"]);
    if (d.contains("pc_location"))
      out->pc_location = dup_str(py::cast<std::string>(d["pc_location"]));
    if (d.contains("save_number"))
      out->save_number = py::cast<uint32_t>(d["save_number"]);
    if (d.contains("plugins")) {
      auto pl           = py::cast<std::vector<std::string>>(d["plugins"]);
      out->plugin_count = std::min<uint32_t>(static_cast<uint32_t>(pl.size()), 256u);
      for (uint32_t i = 0; i < out->plugin_count; ++i)
        out->plugins[i] = dup_str(pl[i]);
    }
    if (d.contains("light_plugins")) {
      auto pl = py::cast<std::vector<std::string>>(d["light_plugins"]);
      out->light_plugin_count =
          std::min<uint32_t>(static_cast<uint32_t>(pl.size()), 256u);
      for (uint32_t i = 0; i < out->light_plugin_count; ++i)
        out->light_plugins[i] = dup_str(pl[i]);
    }
    return 1;
  } catch (const py::error_already_set&) {
    PyErr_Clear();
    return 0;
  }
}

// -- Sort provider (GmmSortFn) --
struct PySortProvider
{
  py::object fn;
  std::vector<std::string> owned;
  std::vector<const char*> array;
};
std::vector<std::unique_ptr<PySortProvider>> g_py_sort_providers;

const char* const* py_sort_bridge(const char* const* mod_folders, size_t count,
                                  void* user_data)
{
  auto* p = static_cast<PySortProvider*>(user_data);
  if (!p)
    return nullptr;
  py::gil_scoped_acquire acquire;
  try {
    std::vector<std::string> input;
    for (size_t i = 0; i < count; ++i)
      input.emplace_back(mod_folders[i] ? mod_folders[i] : "");
    py::object result = p->fn(input);
    p->owned.clear();
    p->array.clear();
    if (py::isinstance<py::list>(result)) {
      for (auto item : py::cast<py::list>(result)) {
        std::string s = py::cast<std::string>(item);
        p->owned.push_back(std::move(s));
        p->array.push_back(p->owned.back().c_str());
      }
    }
    p->array.push_back(nullptr);  // null terminator
    return p->array.data();
  } catch (const py::error_already_set&) {
    PyErr_Clear();
    return nullptr;
  }
}

// -- Stage claim (GmmStageFnV2) --
// The ABI callback receives opaque pointers; we pass them through to Python as
// int (pointer-sized) values.  The Python handler must return True/False.
struct PyStageClaimProvider
{
  py::object fn;
};
std::vector<std::unique_ptr<PyStageClaimProvider>> g_py_stage_claim_providers;

int py_stage_claim_bridge(void* mod, void* instance, void* conflicts, void* profile,
                          void* user_data)
{
  auto* p = static_cast<PyStageClaimProvider*>(user_data);
  if (!p)
    return 0;
  py::gil_scoped_acquire acquire;
  try {
    py::object result = p->fn(
        reinterpret_cast<uintptr_t>(mod), reinterpret_cast<uintptr_t>(instance),
        reinterpret_cast<uintptr_t>(conflicts), reinterpret_cast<uintptr_t>(profile));
    if (py::isinstance<py::bool_>(result))
      return py::cast<bool>(result) ? 1 : 0;
    if (py::isinstance<py::int_>(result))
      return py::cast<int>(result) != 0 ? 1 : 0;
    return 0;
  } catch (const py::error_already_set&) {
    PyErr_Clear();
    return 0;
  }
}

// -- Animation parser bridge (simplified) --
// The ABI GmmAnimationParserFnV2 fills a complex GmmAnimationDataV2 struct.
// For Python, we accept a simpler callable that returns a dict with frames,
// fps, canvas_width, canvas_height.  The bridge marshals it into the C struct.
struct PyAnimationParserProvider
{
  py::object fn;
};
std::vector<std::unique_ptr<PyAnimationParserProvider>> g_py_animation_parser_providers;

int py_animation_parser_bridge(const char* file_path, const char* base_dir,
                               GmmAnimationDataV2* out, void* user_data)
{
  auto* p = static_cast<PyAnimationParserProvider*>(user_data);
  if (!p || !out)
    return 0;
  py::gil_scoped_acquire acquire;
  try {
    py::object result =
        p->fn(py::str(file_path ? file_path : ""), py::str(base_dir ? base_dir : ""));
    if (result.is_none())
      return 0;
    auto d = py::cast<py::dict>(result);
    std::memset(out, 0, sizeof(*out));
    if (d.contains("fps"))
      out->fps = py::cast<float>(d["fps"]);
    if (d.contains("canvas_width"))
      out->canvas_width = py::cast<int32_t>(d["canvas_width"]);
    if (d.contains("canvas_height"))
      out->canvas_height = py::cast<int32_t>(d["canvas_height"]);

    // Parse frames: list of dicts with delay_ms and layers
    if (d.contains("frames")) {
      auto frames_list = py::cast<py::list>(d["frames"]);
      size_t count     = py::len(frames_list);
      if (count > 0) {
        out->frame_count = count;
        out->frames      = static_cast<GmmAnimationFrameV2*>(
            std::calloc(count, sizeof(GmmAnimationFrameV2)));
        for (size_t fi = 0; fi < count; ++fi) {
          auto fd     = py::cast<py::dict>(frames_list[fi]);
          auto& frame = out->frames[fi];
          if (fd.contains("delay_ms"))
            frame.delay_ms = py::cast<float>(fd["delay_ms"]);
          if (fd.contains("layers")) {
            auto layers_list = py::cast<py::list>(fd["layers"]);
            size_t lc        = py::len(layers_list);
            if (lc > 0) {
              frame.layer_count = lc;
              frame.layers      = static_cast<GmmAnimationLayerV2*>(
                  std::calloc(lc, sizeof(GmmAnimationLayerV2)));
              for (size_t li = 0; li < lc; ++li) {
                auto ld     = py::cast<py::dict>(layers_list[li]);
                auto& layer = frame.layers[li];
                if (ld.contains("x"))
                  layer.x = py::cast<int32_t>(ld["x"]);
                if (ld.contains("y"))
                  layer.y = py::cast<int32_t>(ld["y"]);
                if (ld.contains("width"))
                  layer.width = py::cast<int32_t>(ld["width"]);
                if (ld.contains("height"))
                  layer.height = py::cast<int32_t>(ld["height"]);
                if (ld.contains("rgba_pixels")) {
                  auto pixels = py::cast<std::vector<uint8_t>>(ld["rgba_pixels"]);
                  if (!pixels.empty()) {
                    layer.pixel_count = pixels.size();
                    layer.rgba_pixels =
                        static_cast<uint8_t*>(std::malloc(pixels.size()));
                    std::memcpy(layer.rgba_pixels, pixels.data(), pixels.size());
                  }
                }
              }
            }
          }
        }
      }
    }
    return 1;
  } catch (const py::error_already_set&) {
    PyErr_Clear();
    return 0;
  }
}

// Plugin paths loaded this session, used to scope registry teardown on
// shutdown.
std::vector<std::string> g_loaded_plugin_paths;

}  // namespace

// -- gmm.RegistrationContext - Python-side wrapper --

class PyRegistrationContext
{
public:
  PyRegistrationContext(engine::PluginLoader* loader, engine::PluginInfo* plugin)
      : loader_(loader), plugin_(plugin)
  {}

  // -- Fluent chaining: every register_* returns *this --

  PyRegistrationContext&
  register_identity(uint32_t steam_appid, const std::string& gog_id,
                    const std::string& epic_namespace, const std::string& nexus_domain,
                    const std::string& display_name, const std::string& exe_windows,
                    const std::string& exe_linux, const std::string& exe_macos)
  {
    plugin_->steam_appid  = steam_appid;
    plugin_->nexus_domain = nexus_domain;
    if (!display_name.empty())
      plugin_->game_display_name = display_name;
    plugin_->game_support = true;
    engine::Logger::instance().debug(
        "Python plugin registered identity: appid=" + std::to_string(steam_appid) +
        " name=" + (display_name.empty() ? plugin_->game_id : display_name) +
        " nexus=" + nexus_domain);
    return *this;
  }

  PyRegistrationContext& register_meta(const std::string& author,
                                       const std::string& version,
                                       const std::string& description)
  {
    plugin_->author      = author;
    plugin_->version     = version;
    plugin_->description = description;
    return *this;
  }

  PyRegistrationContext& register_category(const std::string& category)
  {
    plugin_->category = category;
    return *this;
  }

  // -- Batched categories (plural form) --
  PyRegistrationContext& register_categories(py::list categories)
  {
    plugin_->categories.clear();
    for (const auto& item : categories) {
      std::string s = py::cast<std::string>(item);
      if (s.empty())
        continue;
      plugin_->categories.push_back(s);
    }
    // Mirror the first entry into the legacy single-string `category` so
    // existing consumers (Plugins-tab group, primary category lookup) keep
    // working. When the list is empty, leave `category` untouched.
    if (!plugin_->categories.empty())
      plugin_->category = plugin_->categories.front();
    return *this;
  }

  PyRegistrationContext&
  register_settings(const std::vector<std::pair<std::string, std::string>>& settings)
  {
    plugin_->settings = settings;

    // Mirror into the process-wide settings registry so the host callbacks
    // can read/write these keys at runtime (v2 IPlugin settings persistence).
    const std::string basename = basename_of(plugin_->path);
    std::vector<const char*> keys, values;
    keys.reserve(settings.size());
    values.reserve(settings.size());
    for (const auto& kv : settings) {
      keys.push_back(kv.first.c_str());
      values.push_back(kv.second.c_str());
    }
    engine::PluginSettingsRegistry::instance().register_settings(
        basename, keys.data(), values.data(), settings.size());
    engine::PluginSettingsRegistry::instance().register_alias(plugin_->game_id,
                                                              basename);
    engine::PluginSettingsRegistry::instance().register_alias(plugin_->plugin_name,
                                                              basename);
    return *this;
  }

  // P1.5 typed settings tab - the pybind mirror of the ABI
  // register_settings_tab entry. Each entry is a (key, type, default,
  // options) tuple; options is None except for type "choice" (a list of
  // candidate choices) or "int" (the "min:max" range string).
  PyRegistrationContext& register_settings_tab(const std::string& title,
                                               py::object settings_obj)
  {
    if (title.empty()) {
      engine::Logger::instance().warn("register_settings_tab: empty title - ignored");
      return *this;
    }
    engine::PluginInfo::SettingTab tab;
    tab.title = title;
    std::vector<const char*> keys, types, defaults, options;
    py::list settings = settings_obj.cast<py::list>();
    for (const auto& item : settings) {
      py::tuple t = py::reinterpret_borrow<py::tuple>(item);
      if (py::len(t) < 3)
        continue;
      engine::PluginInfo::SettingTabEntry entry;
      entry.key           = py::cast<std::string>(t[0]);
      entry.type          = py::cast<std::string>(t[1]);
      entry.default_value = py::cast<std::string>(t[2]);
      std::string opt;
      if (py::len(t) >= 4 && !t[3].is_none()) {
        if (entry.type == "choice") {
          for (const auto& o : py::cast<std::vector<std::string>>(t[3]))
            entry.choices.push_back(o);
          std::string joined;
          for (size_t i = 0; i < entry.choices.size(); ++i) {
            if (i)
              joined += '\n';
            joined += entry.choices[i];
          }
          opt = joined;
        } else if (entry.type == "int") {
          entry.int_range = py::cast<std::string>(t[3]);
          opt             = entry.int_range;
        }
      }
      tab.settings.push_back(std::move(entry));
      keys.push_back(tab.settings.back().key.c_str());
      types.push_back(tab.settings.back().type.c_str());
      defaults.push_back(tab.settings.back().default_value.c_str());
      options.push_back(opt.empty() ? nullptr : opt.c_str());
    }
    // Capture size before moving `tab`; the moved-from vector is left
    // in a valid-but-unspecified state (size()==0 on libstdc++).
    const size_t settings_count = tab.settings.size();
    plugin_->settings_tab       = std::move(tab);

    const std::string basename = basename_of(plugin_->path);
    engine::PluginSettingsRegistry::instance().register_settings_tab(
        basename, title.c_str(), keys.data(), types.data(), defaults.data(),
        options.data(), settings_count);
    engine::PluginSettingsRegistry::instance().register_alias(plugin_->game_id,
                                                              basename);
    engine::PluginSettingsRegistry::instance().register_alias(plugin_->plugin_name,
                                                              basename);
    return *this;
  }

  // -- v2 IPlugin requirements --
  PyRegistrationContext& register_requirements(py::object fn)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_requirements: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyRequirementsProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_req_providers.push_back(std::move(provider));
    engine::RequirementsRegistry::instance().register_requirements(
        plugin_->path, py_requirements_bridge, user_data);
    engine::Logger::instance().debug("Python plugin registered requirements");
    return *this;
  }

  // -- v2 IPluginDiagnose (registers into both the v1 DiagnosticsRegistry for
  //    the Plugins tab and the v2 DiagnoseRegistry) --
  PyRegistrationContext& register_diagnostics(py::object fn,
                                              const std::string& game_id = "")
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_diagnostics: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyDiagnoseProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_diagnose_providers.push_back(std::move(provider));

    std::string gid = game_id.empty() ? plugin_->game_id : game_id;
    engine::DiagnosticsRegistry::instance().register_provider(
        plugin_->game_id, py_diag_v1_bridge, user_data);
    engine::DiagnoseRegistry::instance().register_diagnostics(gid, py_diag_v2_bridge,
                                                              user_data, plugin_->path);
    engine::Logger::instance().debug("Python plugin registered diagnostics for game=" +
                                     gid);
    return *this;
  }

  PyRegistrationContext& subscribe_event(const std::string& event_id, py::object fn)
  {
    if (event_id.empty()) {
      engine::Logger::instance().warn("subscribe_event: empty event_id - ignored");
      return *this;
    }
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "subscribe_event: fn is not a callable - ignored");
      return *this;
    }
    auto handler    = std::make_unique<PyEventHandler>();
    handler->fn     = std::move(fn);
    void* user_data = handler.get();
    g_py_handlers.push_back(std::move(handler));
    engine::EventBus::instance().subscribe(
        event_id,
        [user_data](const std::string& eid, const std::string& payload) {
          py_event_bridge(eid.c_str(), payload.c_str(), user_data);
        },
        plugin_->path);
    engine::Logger::instance().debug("Python plugin subscribed to event: " + event_id);
    return *this;
  }

  PyRegistrationContext&
  register_game_feature(const std::string& game_id, const std::string& feature_type,
                        int priority, const std::vector<std::string>& folder_names,
                        const std::vector<std::string>& file_extensions)
  {
    std::string gid = game_id.empty() ? plugin_->game_id : game_id;
    if (gid.empty() || feature_type.empty()) {
      engine::Logger::instance().warn(
          "register_game_feature: empty game_id/feature_type - ignored");
      return *this;
    }
    if (feature_type == "mod_data_checker") {
      auto checker = std::make_shared<engine::ModDataCheckerFeature>(folder_names,
                                                                     file_extensions);
      engine::Game::Features::Registry::instance().register_feature(
          gid, feature_type, priority, checker, plugin_->path);
    } else if (feature_type == "game_plugins") {
      auto feature = std::make_shared<engine::GamePluginsFeature>(folder_names);
      engine::Game::Features::Registry::instance().register_feature(
          gid, feature_type, priority, feature, plugin_->path);
    } else {
      engine::Logger::instance().warn("register_game_feature: unknown feature type '" +
                                      feature_type + "' - ignored");
      return *this;
    }
    engine::Logger::instance().debug(
        "Python plugin registered game feature: " + feature_type + " (game=" + gid +
        ", priority=" + std::to_string(priority) + ")");
    return *this;
  }

  PyRegistrationContext& register_game_feature_data(const std::string& game_id,
                                                    const std::string& feature_type,
                                                    int priority, py::dict data)
  {
    std::string gid = game_id.empty() ? plugin_->game_id : game_id;
    if (gid.empty() || feature_type.empty()) {
      engine::Logger::instance().warn(
          "register_game_feature_data: empty game_id/feature_type - ignored");
      return *this;
    }
    std::vector<std::pair<std::string, std::string>> kv;
    for (auto item : data) {
      kv.emplace_back(py::cast<std::string>(item.first),
                      py::cast<std::string>(py::str(item.second)));
    }
    if (!engine::register_game_feature_data(gid, feature_type, priority, std::move(kv),
                                            plugin_->path)) {
      return *this;
    }
    engine::Logger::instance().debug(
        "Python plugin registered game feature: " + feature_type + " (game=" + gid +
        ", priority=" + std::to_string(priority) + ")");
    return *this;
  }

  // -- v2 IPluginFileMapper --
  PyRegistrationContext& register_file_mapper(const std::string& game_id, py::object fn)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_file_mapper: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyFileMapperProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_file_mapper_providers.push_back(std::move(provider));

    std::string gid = game_id.empty() ? plugin_->game_id : game_id;
    engine::PluginFileMapper m;
    m.game_id   = gid;
    m.fn        = reinterpret_cast<void*>(py_file_mapper_bridge);
    m.user_data = user_data;
    plugin_->file_mappers.push_back(std::move(m));
    engine::FileMapperRegistry::instance().register_mapper(gid, py_file_mapper_bridge,
                                                           user_data, plugin_->path);
    engine::Logger::instance().debug("Python plugin registered file mapper for game=" +
                                     gid);
    return *this;
  }

  // -- v2 IPluginSaveParser --
  PyRegistrationContext& register_save_parser(const std::string& game_id, py::object fn,
                                              int priority = 0)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_save_parser: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PySaveParserProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_save_parser_providers.push_back(std::move(provider));

    std::string gid = game_id.empty() ? plugin_->game_id : game_id;
    if (gid.empty()) {
      engine::Logger::instance().warn("register_save_parser: empty game_id - ignored");
      return *this;
    }
    engine::SaveParserRegistry::instance().register_parser(
        gid, priority,
        [user_data](const std::filesystem::path& path,
                    const std::string& g) -> engine::SaveGame {
          GmmSaveDataV2 c_out = {};
          if (!py_save_parser_bridge(path.string().c_str(), g.c_str(), &c_out,
                                     user_data)) {
            throw engine::SaveParseError("python v2 parser returned 0");
          }
          engine::SaveGame out;
          out.file_path     = c_out.file_path ? c_out.file_path : "";
          out.game_id       = c_out.game_id ? c_out.game_id : g;
          out.creation_time = c_out.creation_time;
          out.pc_name       = c_out.pc_name ? c_out.pc_name : "";
          out.pc_level      = c_out.pc_level;
          out.pc_location   = c_out.pc_location ? c_out.pc_location : "";
          out.save_number   = c_out.save_number;
          for (uint32_t i = 0; i < c_out.plugin_count && i < 256; ++i) {
            out.plugins.push_back(c_out.plugins[i] ? c_out.plugins[i] : "");
            std::free(c_out.plugins[i]);
          }
          for (uint32_t i = 0; i < c_out.light_plugin_count && i < 256; ++i) {
            out.light_plugins.push_back(c_out.light_plugins[i] ? c_out.light_plugins[i]
                                                               : "");
            std::free(c_out.light_plugins[i]);
          }
          std::free(c_out.file_path);
          std::free(c_out.game_id);
          std::free(c_out.pc_name);
          std::free(c_out.pc_location);
          return out;
        },
        user_data, plugin_->path);
    engine::Logger::instance().debug("Python plugin registered save parser for game=" +
                                     gid);
    return *this;
  }

  // -- v2 IPluginGame hook --
  PyRegistrationContext& register_hook(const std::string& tag, const std::string& data,
                                       py::object fn, int priority = 0)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn("register_hook: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyHookProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_hook_providers.push_back(std::move(provider));

    std::string game_id   = plugin_->game_id;
    std::string hook_tag  = tag.empty() ? "" : tag;
    std::string hook_data = data;

    loader_->knowledge().set(game_id, hook_tag, hook_data);
    ::HookRegistry::instance().register_hook(hook_tag.c_str(), py_hook_bridge, priority,
                                             user_data, plugin_->path.c_str());
    engine::Logger::instance().debug("Python plugin registered hook: " + hook_tag +
                                     " (game=" + game_id + ")");
    return *this;
  }

  // -- v2 IPluginGame order encoding --
  PyRegistrationContext& register_order_encoding(py::object fn)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_order_encoding: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyOrderEncodingProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_order_encoding_providers.push_back(std::move(provider));

    const std::string& game_id        = plugin_->game_id;
    plugin_->order_encoding_fn        = py_order_encoding_bridge;
    plugin_->order_encoding_user_data = user_data;
    engine::OrderEncodingRegistry::instance().register_provider(
        game_id, py_order_encoding_bridge, user_data, plugin_->path);
    engine::Logger::instance().debug(
        "Python plugin registered order encoding (game=" + game_id + ")");
    return *this;
  }

  // -- v2 IPluginGame deploy strategy --
  PyRegistrationContext& register_deploy_strategy(py::object deploy_fn,
                                                  py::object remove_fn)
  {
    if (!py::isinstance<py::function>(deploy_fn)) {
      engine::Logger::instance().warn(
          "register_deploy_strategy: deploy_fn is not a callable - ignored");
      return *this;
    }
    auto provider       = std::make_unique<PyDeployProvider>();
    provider->deploy_fn = std::move(deploy_fn);
    provider->remove_fn = remove_fn;
    void* user_data     = provider.get();
    g_py_deploy_providers.push_back(std::move(provider));

    const std::string& game_id = plugin_->game_id;
    plugin_->deploy_fn         = py_deploy_bridge;
    plugin_->remove_fn =
        py::isinstance<py::function>(remove_fn) ? py_remove_bridge : nullptr;
    plugin_->deploy_user_data = user_data;
    engine::DeployStrategyRegistry::instance().register_provider(
        game_id, py_deploy_bridge,
        py::isinstance<py::function>(remove_fn) ? py_remove_bridge : nullptr, user_data,
        plugin_->path);
    engine::Logger::instance().debug(
        "Python plugin registered deploy strategy (game=" + game_id + ")");
    return *this;
  }

  // -- v2 IPluginTool --
  PyRegistrationContext& register_tool(const std::string& tool_id,
                                       const std::string& kind,
                                       py::object fn = py::none())
  {
    engine::ExternalTool tool;
    tool.tool_id         = tool_id;
    tool.game_id         = plugin_->game_id;
    tool.display_name    = tool_id;
    std::string kind_str = kind.empty() ? "advisory" : kind;
    tool.kind            = (kind_str == "workshop") ? engine::ToolKind::Workshop
                                                    : engine::ToolKind::Advisory;

    if (py::isinstance<py::function>(fn)) {
      auto provider   = std::make_unique<PyToolProvider>();
      provider->fn    = std::move(fn);
      void* user_data = provider.get();
      g_py_tool_providers.push_back(std::move(provider));

      tool.invoke_fn        = py_tool_bridge;
      tool.invoke_user_data = user_data;

      engine::PluginToolRegistry::instance().register_tool(
          tool_id, kind_str, py_tool_bridge, user_data, plugin_->path);
    }
    loader_->tool_registry().register_tool(tool);
    return *this;
  }

  // -- v2 IPluginPreview --
  PyRegistrationContext& register_preview(const std::string& extension, py::object fn)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_preview: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyPreviewProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_preview_providers.push_back(std::move(provider));

    engine::PluginPreview p;
    p.file_extension = extension;
    p.fn             = reinterpret_cast<void*>(py_preview_bridge);
    p.user_data      = user_data;
    plugin_->previews.push_back(std::move(p));

    ui::preview::Registry::instance().register_preview(
        extension, py_preview_bridge, nullptr, user_data, plugin_->path);
    engine::Logger::instance().debug("Python plugin registered preview for extension=" +
                                     extension);
    return *this;
  }

  // -- v2 IPluginModPage --
  PyRegistrationContext& register_modpage(const std::string& url, py::object fn)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_modpage: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyModPageProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_modpage_providers.push_back(std::move(provider));

    engine::PluginModPage m;
    m.url       = url;
    m.fn        = reinterpret_cast<void*>(py_modpage_bridge);
    m.user_data = user_data;
    plugin_->modpages.push_back(std::move(m));
    engine::Logger::instance().debug("Python plugin registered modpage for url=" + url);
    return *this;
  }

  // -- v2 IPluginGame sort provider --
  PyRegistrationContext& register_sort_provider(py::object fn)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_sort_provider: fn is not a callable - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PySortProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_sort_providers.push_back(std::move(provider));

    std::string gid = plugin_->game_id;
    if (gid.empty()) {
      engine::Logger::instance().warn(
          "register_sort_provider: empty game_id - ignored");
      return *this;
    }
    engine::SortRegistry::instance().register_provider(
        gid, std::make_unique<engine::AbiSortProvider>(gid.c_str(), py_sort_bridge,
                                                       user_data));
    engine::Logger::instance().debug(
        "Python plugin registered sort provider for game=" + gid);
    return *this;
  }

  // -- v2 IPluginGame identity (register_game) --
  PyRegistrationContext&
  register_game(const std::string& game_id, const std::string& display_name,
                uint32_t steam_appid = 0, const std::string& nexus_domain = "",
                const std::string& gog_id = "", const std::string& epic_namespace = "",
                const std::string& exe_windows = "", const std::string& exe_linux = "",
                const std::string& exe_macos = "")
  {
    if (!game_id.empty())
      plugin_->game_id = game_id;
    if (!display_name.empty())
      plugin_->game_display_name = display_name;
    plugin_->steam_appid    = steam_appid;
    plugin_->nexus_domain   = nexus_domain;
    plugin_->gog_id         = gog_id;
    plugin_->epic_namespace = epic_namespace;
    plugin_->exe_windows    = exe_windows;
    plugin_->exe_linux      = exe_linux;
    plugin_->exe_macos      = exe_macos;
    plugin_->game_support   = true;
    engine::Logger::instance().debug(
        "Python plugin registered game: id=" + plugin_->game_id +
        " name=" + plugin_->game_display_name + " nexus=" + nexus_domain);
    return *this;
  }

  // -- Stage claim (wired to StageRegistry) --
  // Positional order is (stage_name, fn, priority); sibling
  // register_wildcard_stage_claim and register_animation_parser place fn
  // later in their positional list (because they have more leading args).
  // All four functions are kwargs-only in practice: every pybind11 binding
  // declares the args via py::arg(...) with defaults, so callers should
  // pass them by keyword and ignore position. Example:
  //   ctx.register_stage_claim(stage_name="loot_sort", fn=fn, priority=200)
  PyRegistrationContext& register_stage_claim(const std::string& stage_name,
                                              py::object fn, int priority = 100)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_stage_claim: fn is not a callable - ignored");
      return *this;
    }
    if (stage_name.empty()) {
      engine::Logger::instance().warn(
          "register_stage_claim: empty stage_name - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyStageClaimProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_stage_claim_providers.push_back(std::move(provider));

    std::string game_id = plugin_->game_id;
    loader_->stage_registry().register_claim(
        game_id, stage_name,
        [user_data](engine::Mod& mod, engine::PipelineContext& ctx) -> bool {
          return py_stage_claim_bridge(reinterpret_cast<void*>(&mod),
                                       reinterpret_cast<void*>(ctx.instance),
                                       reinterpret_cast<void*>(ctx.conflict_index),
                                       reinterpret_cast<void*>(ctx.profile),
                                       user_data) != 0;
        },
        priority, plugin_->path);
    engine::Logger::instance().debug("Python plugin registered stage claim: " +
                                     stage_name + " (game=" + game_id + ")");
    return *this;
  }

  // -- Wildcard stage claim (wired to StageRegistry) --
  PyRegistrationContext& register_wildcard_stage_claim(const std::string& game_id,
                                                       const std::string& stage_name,
                                                       py::object fn,
                                                       int priority = 100)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_wildcard_stage_claim: fn is not a callable - ignored");
      return *this;
    }
    if (stage_name.empty()) {
      engine::Logger::instance().warn(
          "register_wildcard_stage_claim: empty stage_name - ignored");
      return *this;
    }
    auto provider   = std::make_unique<PyStageClaimProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_stage_claim_providers.push_back(std::move(provider));

    std::string gid = game_id.empty() ? plugin_->game_id : game_id;
    loader_->stage_registry().register_claim(
        gid, stage_name,
        [user_data](engine::Mod& mod, engine::PipelineContext& ctx) -> bool {
          return py_stage_claim_bridge(reinterpret_cast<void*>(&mod),
                                       reinterpret_cast<void*>(ctx.instance),
                                       reinterpret_cast<void*>(ctx.conflict_index),
                                       reinterpret_cast<void*>(ctx.profile),
                                       user_data) != 0;
        },
        priority, plugin_->path);
    engine::Logger::instance().debug("Python plugin registered wildcard stage claim: " +
                                     stage_name + " (game=" + gid + ")");
    return *this;
  }

  // -- Animation parser (wired to GameFeatureRegistry) --
  // Note: `extension` is currently used as a logging breadcrumb only. The
  // Game::Features::Registry keys this feature as (game_id, "animation_parser")
  // with no extension discriminator, so registering two parsers for the
  // same game at different extensions collides and priority wins. Document
  // the parameter, but do not lie about per-extension routing - the host's
  // extension dispatch (in the UI preview widget) is what selects which
  // parser is invoked for a given file.
  PyRegistrationContext& register_animation_parser(const std::string& game_id,
                                                   const std::string& extension,
                                                   py::object fn, int priority = 100)
  {
    if (!py::isinstance<py::function>(fn)) {
      engine::Logger::instance().warn(
          "register_animation_parser: fn is not a callable - ignored");
      return *this;
    }
    if (extension.empty()) {
      engine::Logger::instance().warn(
          "register_animation_parser: empty extension - parser will receive "
          "every animation file the UI routes to this game_id");
    }
    auto provider   = std::make_unique<PyAnimationParserProvider>();
    provider->fn    = std::move(fn);
    void* user_data = provider.get();
    g_py_animation_parser_providers.push_back(std::move(provider));

    std::string gid = game_id.empty() ? plugin_->game_id : game_id;
    auto feature    = std::make_shared<engine::AnimationParserFeature>(
        [user_data](const std::string& file_path, const std::string& base_dir)
            -> std::optional<engine::AnimationParserFeature::AnimationData> {
          GmmAnimationDataV2 c_out = {};
          if (!py_animation_parser_bridge(file_path.c_str(), base_dir.c_str(), &c_out,
                                          user_data)) {
            return std::nullopt;
          }
          engine::AnimationParserFeature::AnimationData data;
          data.fps           = c_out.fps;
          data.canvas_width  = c_out.canvas_width;
          data.canvas_height = c_out.canvas_height;
          data.raw_animation = c_out.raw_animation;
          for (size_t fi = 0; fi < c_out.frame_count; ++fi) {
            auto& cf = c_out.frames[fi];
            engine::AnimationParserFeature::Frame frame;
            frame.delay_ms = cf.delay_ms;
            for (size_t li = 0; li < cf.layer_count; ++li) {
              auto& cl = cf.layers[li];
              engine::AnimationParserFeature::LayerItem layer;
              layer.x      = cl.x;
              layer.y      = cl.y;
              layer.width  = cl.width;
              layer.height = cl.height;
              if (cl.rgba_pixels && cl.pixel_count > 0) {
                layer.rgba_pixels.assign(cl.rgba_pixels,
                                         cl.rgba_pixels + cl.pixel_count);
              }
              frame.layers.push_back(std::move(layer));
            }
            data.frames.push_back(std::move(frame));
          }
          free(c_out.frames);
          return data;
        });
    engine::Game::Features::Registry::instance().register_feature(
        gid, engine::AnimationParserFeature::type_key(), priority, std::move(feature),
        plugin_->path);
    engine::Logger::instance().debug(
        "Python plugin registered animation parser (game=" + gid +
        ", ext=" + extension + ")");
    return *this;
  }

  void register_image_diff() {}

  // -- Batched tabs (plural form) --
  PyRegistrationContext& register_tabs(py::list tabs)
  {
    for (const auto& item : tabs) {
      if (py::isinstance<py::dict>(item)) {
        auto d = py::cast<py::dict>(item);
        std::string cap =
            d.contains("capability") ? py::cast<std::string>(d["capability"]) : "";
        std::string dn =
            d.contains("display_name") ? py::cast<std::string>(d["display_name"]) : "";
        std::string dp =
            d.contains("data_path") ? py::cast<std::string>(d["data_path"]) : "";
        std::string desc =
            d.contains("description") ? py::cast<std::string>(d["description"]) : "";
        std::string ph = d.contains("protocol_handler")
                             ? py::cast<std::string>(d["protocol_handler"])
                             : "";
        std::string wd = d.contains("website_domain")
                             ? py::cast<std::string>(d["website_domain"])
                             : "";
        std::string sp = d.contains("supported_platforms")
                             ? py::cast<std::string>(d["supported_platforms"])
                             : "";
        std::string ib = d.contains("insert_before")
                             ? py::cast<std::string>(d["insert_before"])
                             : "";
        std::string ia =
            d.contains("insert_after") ? py::cast<std::string>(d["insert_after"]) : "";
        register_tab(cap, dn, dp, desc, ph, wd, sp, ib, ia);
      } else if (py::isinstance<py::tuple>(item)) {
        auto t = py::cast<py::tuple>(item);
        if (py::len(t) >= 1) {
          std::string cap = py::cast<std::string>(t[0]);
          std::string dn  = py::len(t) >= 2 ? py::cast<std::string>(t[1]) : "";
          std::string dp  = py::len(t) >= 3 ? py::cast<std::string>(t[2]) : "";
          register_tab(cap, dn, dp, "", "", "", "", "", "");
        }
      }
    }
    return *this;
  }

  PyRegistrationContext& register_capability(const std::string& capability,
                                             const std::string& display_name,
                                             const std::string& data_path,
                                             const std::string& description,
                                             const std::string& protocol_handler,
                                             const std::string& website_domain,
                                             const std::string& supported_platforms)
  {
    engine::CapabilityInfo info;
    info.game_id          = plugin_->game_id;
    info.capability       = capability;
    info.display_name     = display_name.empty() ? capability : display_name;
    info.data_path        = data_path;
    info.description      = description;
    info.protocol_handler = protocol_handler;
    info.website_domain   = website_domain;

    if (!supported_platforms.empty()) {
      std::string s = supported_platforms;
      size_t pos;
      while ((pos = s.find(',')) != std::string::npos) {
        info.supported_platforms.push_back(s.substr(0, pos));
        s.erase(0, pos + 1);
      }
      if (!s.empty())
        info.supported_platforms.push_back(s);
    }

    loader_->capabilities().register_capability(info);
    return *this;
  }

  PyRegistrationContext&
  register_tab(const std::string& capability, const std::string& display_name,
               const std::string& data_path, const std::string& description,
               const std::string& protocol_handler, const std::string& website_domain,
               const std::string& supported_platforms, const std::string& insert_before,
               const std::string& insert_after)
  {
    engine::CapabilityInfo info;
    info.game_id          = plugin_->game_id;
    info.capability       = capability;
    info.display_name     = display_name.empty() ? capability : display_name;
    info.data_path        = data_path;
    info.description      = description;
    info.protocol_handler = protocol_handler;
    info.website_domain   = website_domain;
    info.insert_before    = insert_before;
    info.insert_after     = insert_after;

    if (!supported_platforms.empty()) {
      std::string s = supported_platforms;
      size_t pos;
      while ((pos = s.find(',')) != std::string::npos) {
        info.supported_platforms.push_back(s.substr(0, pos));
        s.erase(0, pos + 1);
      }
      if (!s.empty())
        info.supported_platforms.push_back(s);
    }

    loader_->capabilities().register_capability(info);
    return *this;
  }

  // -- Resolve file (host service) --
  std::string resolve_file(const std::string& root, const std::string& relative_path)
  {
    if (root.empty() || relative_path.empty())
      return "";
    try {
      const auto gf =
          engine::vfs::PathResolver(std::filesystem::path(root)).resolve(relative_path);
      if (!gf)
        return "";
      return gf->absolute().string();
    } catch (...) {
      return "";
    }
  }

  [[nodiscard]] std::string game_id() const { return plugin_->game_id; }

private:
  engine::PluginLoader* loader_;
  engine::PluginInfo* plugin_;
};

// -- Embedded gmm module --

// Forward declaration for the Plugin base class trampoline
struct PyPluginBase;

PYBIND11_EMBEDDED_MODULE(gmm, m)
{
  m.doc() = "GameModManager Python plugin API - version-agnostic";

  py::class_<PyRegistrationContext>(m, "RegistrationContext", py::dynamic_attr())
      // Fluent chaining: every method returns self
      .def("register_identity", &PyRegistrationContext::register_identity,
           py::arg("steam_appid") = 0, py::arg("gog_id") = "",
           py::arg("epic_namespace") = "", py::arg("nexus_domain") = "",
           py::arg("display_name") = "", py::arg("exe_windows") = "",
           py::arg("exe_linux") = "", py::arg("exe_macos") = "",
           py::return_value_policy::reference)
      .def("register_game", &PyRegistrationContext::register_game, py::arg("game_id"),
           py::arg("display_name") = "", py::arg("steam_appid") = 0,
           py::arg("nexus_domain") = "", py::arg("gog_id") = "",
           py::arg("epic_namespace") = "", py::arg("exe_windows") = "",
           py::arg("exe_linux") = "", py::arg("exe_macos") = "",
           py::return_value_policy::reference)
      .def("register_meta", &PyRegistrationContext::register_meta,
           py::arg("author") = "", py::arg("version") = "", py::arg("description") = "",
           py::return_value_policy::reference)
      .def("register_category", &PyRegistrationContext::register_category,
           py::arg("category") = "", py::return_value_policy::reference)
      .def("register_categories", &PyRegistrationContext::register_categories,
           py::arg("categories"), py::return_value_policy::reference)
      .def("register_settings", &PyRegistrationContext::register_settings,
           py::arg("settings"), py::return_value_policy::reference)
      .def("register_settings_tab", &PyRegistrationContext::register_settings_tab,
           py::return_value_policy::reference)
      .def("register_requirements", &PyRegistrationContext::register_requirements,
           py::arg("fn"), py::return_value_policy::reference)
      .def("register_diagnostics", &PyRegistrationContext::register_diagnostics,
           py::arg("fn"), py::arg("game_id") = "", py::return_value_policy::reference)
      .def("register_file_mapper", &PyRegistrationContext::register_file_mapper,
           py::arg("game_id") = "", py::arg("fn"), py::return_value_policy::reference)
      .def("register_save_parser", &PyRegistrationContext::register_save_parser,
           py::arg("game_id") = "", py::arg("fn"), py::arg("priority") = 0,
           py::return_value_policy::reference)
      .def("register_hook", &PyRegistrationContext::register_hook, py::arg("tag"),
           py::arg("data") = "", py::arg("fn"), py::arg("priority") = 0,
           py::return_value_policy::reference)
      .def("register_order_encoding", &PyRegistrationContext::register_order_encoding,
           py::arg("fn"), py::return_value_policy::reference)
      .def("register_deploy_strategy", &PyRegistrationContext::register_deploy_strategy,
           py::arg("deploy_fn"), py::arg("remove_fn") = py::none(),
           py::return_value_policy::reference)
      .def("register_tool", &PyRegistrationContext::register_tool, py::arg("tool_id"),
           py::arg("kind"), py::arg("fn") = py::none(),
           py::return_value_policy::reference)
      .def("register_preview", &PyRegistrationContext::register_preview,
           py::arg("extension"), py::arg("fn"), py::return_value_policy::reference)
      .def("register_modpage", &PyRegistrationContext::register_modpage, py::arg("url"),
           py::arg("fn"), py::return_value_policy::reference)
      .def("register_sort_provider", &PyRegistrationContext::register_sort_provider,
           py::arg("fn"), py::return_value_policy::reference)
      .def("subscribe_event", &PyRegistrationContext::subscribe_event,
           py::arg("event_id"), py::arg("fn"), py::return_value_policy::reference)
      .def("register_game_feature", &PyRegistrationContext::register_game_feature,
           py::arg("game_id") = "", py::arg("feature_type"), py::arg("priority") = 0,
           py::arg("folder_names")    = std::vector<std::string>{},
           py::arg("file_extensions") = std::vector<std::string>{},
           py::return_value_policy::reference)
      .def("register_game_feature_data",
           &PyRegistrationContext::register_game_feature_data, py::arg("game_id") = "",
           py::arg("feature_type"), py::arg("priority")                           = 0,
           py::arg("data") = py::dict(), py::return_value_policy::reference)
      .def("register_image_diff", &PyRegistrationContext::register_image_diff,
           py::return_value_policy::reference)
      .def("register_capability", &PyRegistrationContext::register_capability,
           py::arg("capability"), py::arg("display_name") = "",
           py::arg("data_path") = "", py::arg("description") = "",
           py::arg("protocol_handler") = "", py::arg("website_domain") = "",
           py::arg("supported_platforms") = "", py::return_value_policy::reference)
      .def("register_tab", &PyRegistrationContext::register_tab, py::arg("capability"),
           py::arg("display_name") = "", py::arg("data_path") = "",
           py::arg("description") = "", py::arg("protocol_handler") = "",
           py::arg("website_domain") = "", py::arg("supported_platforms") = "",
           py::arg("insert_before") = "", py::arg("insert_after") = "",
           py::return_value_policy::reference)
      .def("register_tabs", &PyRegistrationContext::register_tabs, py::arg("tabs"),
           py::return_value_policy::reference)
      .def("register_stage_claim", &PyRegistrationContext::register_stage_claim,
           py::arg("stage_name"), py::arg("fn"), py::arg("priority") = 100,
           py::return_value_policy::reference)
      .def("register_wildcard_stage_claim",
           &PyRegistrationContext::register_wildcard_stage_claim, py::arg("game_id"),
           py::arg("stage_name"), py::arg("fn"), py::arg("priority") = 100,
           py::return_value_policy::reference)
      .def("register_animation_parser",
           &PyRegistrationContext::register_animation_parser, py::arg("game_id"),
           py::arg("extension"), py::arg("fn"), py::arg("priority") = 100,
           py::return_value_policy::reference)
      .def("resolve_file", &PyRegistrationContext::resolve_file, py::arg("root"),
           py::arg("relative_path"))
      .def_property_readonly("game_id", &PyRegistrationContext::game_id);

  // -- Plugin base class: define in Python for clean subclassing --
  //
  // Example:
  //   class MyPlugin(gmm.Plugin):
  //       def game_info(self):
  //           return {"game_id": "skyrim", "display_name": "Skyrim SE",
  //                   "steam_appid": 489830}
  //       def tabs(self):
  //           return [{"capability": "plugins", "display_name": "Plugins",
  //                    "data_path": "Data/"}]
  //       def features(self, ctx):
  //           ctx.register_tool("loot", "advisory")
  //
  //   gmm.register(MyPlugin)
  //
  py::exec(R"(
class _PluginBase:
    """Base class for declarative GMM plugins.

    Subclass and override the methods you need.  The loader calls:
      - game_info() -> dict with game_id, display_name, steam_appid, etc.
      - tabs() -> list of tab dicts
      - categories() -> list of category strings
      - features(ctx) -> register tools, parsers, etc. on ctx
      - hooks(ctx) -> register game-dependent hooks on ctx
      - events(ctx) -> subscribe to events on ctx
    """
    def game_info(self):
        raise NotImplementedError("game_info() must be returned by subclass")

    def tabs(self):
        return []

    def categories(self):
        return []

    def features(self, ctx):
        pass

    def hooks(self, ctx):
        pass

    def events(self, ctx):
        pass

Plugin = _PluginBase
del _PluginBase
)",
           m.attr("__dict__"), m.attr("__dict__"));

  // Storage for Plugin classes registered via gmm.register()
  // The loader reads this after importing the module.
  m.attr("_registered_plugins") = py::list();

  m.def(
      "register",
      [m](py::object plugin_cls_or_instance) {
        // Validate that a class subclass gmm.Plugin before registering
        if (PyType_Check(plugin_cls_or_instance.ptr())) {
          py::object plugin_base = m.attr("Plugin");
          if (!PyObject_IsSubclass(plugin_cls_or_instance.ptr(), plugin_base.ptr())) {
            engine::Logger::instance().warn(
                "gmm.register(): class does not subclass gmm.Plugin - "
                "registering anyway");
          }
        }
        py::list registry = m.attr("_registered_plugins");
        registry.append(plugin_cls_or_instance);
      },
      py::arg("plugin"),
      "Register a Plugin class or instance.  If a class is passed, the "
      "loader instantiates it and calls game_info/tabs/features/etc.");
}

// -- Interpreter lifecycle --

static std::unique_ptr<py::scoped_interpreter> s_interpreter;

bool engine::python_init()
{
  if (s_interpreter)
    return true;

  try {
    s_interpreter = std::make_unique<py::scoped_interpreter>();
    Logger::instance().debug("Python interpreter initialized");
    return true;
  } catch (const std::exception& e) {
    Logger::instance().error("Failed to initialize Python: " + std::string(e.what()));
    return false;
  }
}

bool engine::python_load_plugin(PluginLoader* loader, const std::string& path)
{
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
    std::string plugin_dir  = std::filesystem::path(path).parent_path().string();

    // Add plugin directory to sys.path for relative imports, then restore on
    // every exit path (success, error_already_set, std::exception) so
    // subsequent plugin loads cannot pick up modules from this plugin's dir
    // and identically-named plugins in different dirs cannot shadow each
    // other.
    py::module_ sys                 = py::module_::import("sys");
    py::list path_list              = sys.attr("path");
    const size_t original_path_size = py::len(path_list);
    path_list.insert(0, plugin_dir);

    // RAII guard: pop our prepended entry(ies) on scope exit. Holds a
    // reference to the list and its original size; pop(0) while the list
    // is longer than original_path_size. Safe to run from the destructor
    // during stack unwinding because the GIL is held by the caller via
    // py::gil_scoped_acquire above.
    struct SysPathGuard
    {
      py::list& list;
      size_t baseline;
      ~SysPathGuard()
      {
        try {
          while (py::len(list) > baseline)
            list.attr("pop")(0);
        } catch (...) {
          // Never throw from a destructor.
        }
      }
    };
    SysPathGuard path_guard{path_list, original_path_size};

    // Import the plugin module
    py::module_ plugin_module = py::module_::import(module_name.c_str());

    // Build PluginInfo
    engine::PluginInfo info;
    info.path              = path;
    info.game_id           = module_name;
    info.game_display_name = info.game_id;  // fallback, overridden by register_identity
    info.abi_version       = 0;
    info.loaded            = true;

    // Create context
    PyRegistrationContext ctx(loader, &info);

    // -- Pattern 1: Legacy def register(ctx): function --
    if (py::hasattr(plugin_module, "register")) {
      py::object register_fn = plugin_module.attr("register");
      register_fn(ctx);
    }
    // -- Pattern 2: Plugin base class via gmm.register(MyPlugin) --
    else {
      py::module_ gmm     = py::module_::import("gmm");
      py::list registered = gmm.attr("_registered_plugins");
      if (py::len(registered) > 0) {
        // Take the last registered plugin (most recent gmm.register() call)
        // and pop it BEFORE invoking any user method. If we crashed inside
        // game_info()/tabs()/features() and left the entry in the list, the
        // next broken plugin (no register() and no gmm.register()) would
        // silently inherit this plugin's identity.
        py::object plugin_cls_or_instance =
            registered.attr("pop")(py::len(registered) - 1);
        // Drop any stray leftover entries (defensive: only the last one is
        // semantically the active plugin for THIS load).
        registered.attr("clear")();

        // Validate the registered entry is a gmm.Plugin subclass when given
        // a class. Instances are not type-checked (they may pre-instantiate
        // a Plugin and pass the instance). A non-Plugin class would silently
        // crash on game_info() etc.; warn loudly instead.
        if (PyType_Check(plugin_cls_or_instance.ptr())) {
          py::object plugin_base = gmm.attr("Plugin");
          if (!PyObject_IsSubclass(plugin_cls_or_instance.ptr(), plugin_base.ptr())) {
            engine::Logger::instance().warn(
                "python_load_plugin: class registered via gmm.register() "
                "does not subclass gmm.Plugin - ignored");
            return false;
          }
        }

        py::object plugin_instance;
        // Check if it's a class or an instance
        if (PyType_Check(plugin_cls_or_instance.ptr())) {
          // It's a class (type object) - instantiate it
          plugin_instance = plugin_cls_or_instance();
        } else {
          // It's already an instance
          plugin_instance = plugin_cls_or_instance;
        }

        // Call game_info() to get game identity
        if (py::hasattr(plugin_instance, "game_info")) {
          py::object gi = plugin_instance.attr("game_info")();
          if (py::isinstance<py::dict>(gi)) {
            auto d = py::cast<py::dict>(gi);
            if (d.contains("game_id"))
              info.game_id = py::cast<std::string>(d["game_id"]);
            if (d.contains("display_name"))
              info.game_display_name = py::cast<std::string>(d["display_name"]);
            if (d.contains("steam_appid"))
              info.steam_appid = py::cast<uint32_t>(d["steam_appid"]);
            if (d.contains("nexus_domain"))
              info.nexus_domain = py::cast<std::string>(d["nexus_domain"]);
            if (d.contains("gog_id"))
              info.gog_id = py::cast<std::string>(d["gog_id"]);
            if (d.contains("epic_namespace"))
              info.epic_namespace = py::cast<std::string>(d["epic_namespace"]);
            if (d.contains("exe_windows"))
              info.exe_windows = py::cast<std::string>(d["exe_windows"]);
            if (d.contains("exe_linux"))
              info.exe_linux = py::cast<std::string>(d["exe_linux"]);
            if (d.contains("exe_macos"))
              info.exe_macos = py::cast<std::string>(d["exe_macos"]);
            if (info.steam_appid > 0 || !info.nexus_domain.empty())
              info.game_support = true;
          }
        }

        // Call tabs() and register them
        if (py::hasattr(plugin_instance, "tabs")) {
          py::object tabs_result = plugin_instance.attr("tabs")();
          if (py::isinstance<py::list>(tabs_result)) {
            ctx.register_tabs(tabs_result.cast<py::list>());
          }
        }

        // Call categories() and register them
        if (py::hasattr(plugin_instance, "categories")) {
          py::object cats_result = plugin_instance.attr("categories")();
          if (py::isinstance<py::list>(cats_result)) {
            ctx.register_categories(cats_result.cast<py::list>());
          }
        }

        // Call features(ctx) to register tools, parsers, etc.
        if (py::hasattr(plugin_instance, "features")) {
          plugin_instance.attr("features")(ctx);
        }

        // Call hooks(ctx) to register game-dependent hooks
        if (py::hasattr(plugin_instance, "hooks")) {
          plugin_instance.attr("hooks")(ctx);
        }

        // Call events(ctx) to subscribe to events
        if (py::hasattr(plugin_instance, "events")) {
          plugin_instance.attr("events")(ctx);
        }

        Logger::instance().debug("Python plugin loaded via Plugin base class: " +
                                 info.game_id);
      } else {
        Logger::instance().error(
            "Python plugin missing register() or gmm.register(): " + path);
        return false;
      }
    }

    info.registered = true;
    // Capture fields needed by the post-move debug log before moving info.
    const std::string display_name = info.game_display_name;
    const std::string game_id      = info.game_id;
    const uint32_t steam_appid     = info.steam_appid;
    loader->add_loaded_plugin(std::move(info));
    g_loaded_plugin_paths.push_back(path);

    Logger::instance().debug("Python plugin registered: " + display_name + " (" + path +
                             ", game=" + game_id +
                             ", appid=" + std::to_string(steam_appid) + ")");
    return true;

  } catch (const py::error_already_set& e) {
    Logger::instance().error("Python plugin error: " + path + " - " + e.what());
    return false;
  } catch (const std::exception& e) {
    Logger::instance().error("Failed to load Python plugin: " + path + " - " +
                             e.what());
    return false;
  }
}

void engine::python_shutdown()
{
  {
    // Destroy Python-side handlers + providers with the GIL held. Registries
    // are cleared FIRST (scoped by plugin path) so no callback can still run
    // against a destroyed provider during interpreter teardown.
    py::gil_scoped_acquire acquire;
    EventBus::instance().clear();
    DiagnosticsRegistry::instance().clear();
    Game::Features::Registry::instance().clear();
    PluginSettingsRegistry::instance().clear();
    SortRegistry::instance().clear();
    for (const auto& p : g_loaded_plugin_paths) {
      DiagnoseRegistry::instance().clear_plugin(p);
      FileMapperRegistry::instance().clear_plugin(p);
      RequirementsRegistry::instance().clear_plugin(p);
      OrderEncodingRegistry::instance().clear_plugin(p);
      DeployStrategyRegistry::instance().clear_plugin(p);
      ::HookRegistry::instance().clear_plugin(p.c_str());
      PluginToolRegistry::instance().clear_plugin(p);
      SaveParserRegistry::instance().clear_plugin(p);
    }
    g_loaded_plugin_paths.clear();
    g_py_providers.clear();
    g_py_handlers.clear();
    g_py_req_providers.clear();
    g_py_diagnose_providers.clear();
    g_py_file_mapper_providers.clear();
    g_py_save_parser_providers.clear();
    g_py_hook_providers.clear();
    g_py_order_encoding_providers.clear();
    g_py_deploy_providers.clear();
    g_py_tool_providers.clear();
    g_py_preview_providers.clear();
    g_py_modpage_providers.clear();
    g_py_sort_providers.clear();
    g_py_stage_claim_providers.clear();
    g_py_animation_parser_providers.clear();
  }
  s_interpreter.reset();
}
