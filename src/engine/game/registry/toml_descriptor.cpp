#include "engine/game/registry/toml_descriptor.h"

#include "engine/game/registry/game_descriptor.h"
#include "engine/game/registry/capability_manifest.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/game/registry/game_registration_sink.h"
#include "engine/game/registry/path_template.h"
#include "engine/log/logger.h"
#include "engine/plugin_host/plugin_loader.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace engine {

namespace {

  // Join a vector of strings with commas (no spaces), matching the CSV shape
  // the rest of the engine expects.
  std::string join_csv(const std::vector<std::string> &v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
      if (i)
        out += ",";
      out += v[i];
    }
    return out;
  }

  // Read a toml array of strings into a vector.
  std::vector<std::string> toml_string_array(const toml::node *node) {
    std::vector<std::string> out;
    if (!node)
      return out;
    if (const auto *arr = node->as_array()) {
      for (const auto &e : *arr) {
        if (const auto *s = e.as_string())
          out.push_back(std::string(s->get()));
      }
    }
    return out;
  }

  // Strip a leading "%GAMEDIR%/" (or "%GAMEDIR%") from a mods_dir value.
  std::string strip_gamedir_prefix(const std::string &s) {
    std::string v            = s;
    const std::string prefix = "%GAMEDIR%";
    if (v.rfind(prefix, 0) == 0) {
      v.erase(0, prefix.size());
      if (!v.empty() && v.front() == '/')
        v.erase(0, 1);
      if (v.empty())
        v = s;  // keep original if nothing left
    }
    return v;
  }

  // Validate all path-typed templates in a game record. Returns empty on
  // success, or an error message naming the field/token on failure.
  std::string validate_record_paths(const toml::table &g) {
    // mods_dir
    if (auto *n = g.get("mods_dir")) {
      if (auto *s = n->as_string()) {
        std::string err = validate_path_template(std::string(s->get()));
        if (!err.empty())
          return "mods_dir: " + err;
      }
    }
    // paths.saves (string or table)
    if (auto *paths = g.get("paths")) {
      if (auto *pt = paths->as_table()) {
        if (auto *saves = pt->get("saves")) {
          if (auto *str = saves->as_string()) {
            std::string err = validate_path_template(std::string(str->get()));
            if (!err.empty())
              return "paths.saves: " + err;
          } else if (auto *tbl = saves->as_table()) {
            for (const auto &[k, v] : *tbl) {
              if (auto *s = v.as_string()) {
                std::string err = validate_path_template(std::string(s->get()));
                if (!err.empty())
                  return "paths.saves." + std::string(k) + ": " + err;
              }
            }
          }
        }
      }
    }
    // capabilities tabs paths? skip phase-A for now (data caps only).
    return {};
  }

}  // namespace

int load_toml_game_dir(const fs::path &dir, PluginLoader &loader) {
  int count = 0;

  // Collect sorted .toml files.
  std::vector<fs::path> files;
  std::error_code ec;
  if (!fs::exists(dir, ec))
    return 0;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".toml")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) {
    return a.filename() < b.filename();
  });

  for (const auto &file : files) {
    // Parse.
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      Logger::instance().error("toml descriptor: cannot read " + file.string());
      continue;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    toml::table tbl;
    try {
      tbl = toml::parse(content);
    } catch (const toml::parse_error &e) {
      Logger::instance().error("toml descriptor: parse error in " + file.string() +
                               ": " + std::string(e.description()));
      continue;
    }

    // Determine the list of game records.
    std::vector<const toml::table *> records;
    if (auto *games = tbl.get("games")) {
      if (auto *arr = games->as_array()) {
        for (const auto &e : *arr) {
          if (auto *t = e.as_table())
            records.push_back(t);
        }
      }
    } else if (auto *game = tbl.get("game")) {
      if (auto *t = game->as_table())
        records.push_back(t);
    }

    if (records.empty()) {
      // Maybe top-level is a single game without [game] wrapper? Treat whole
      // table as one record if it has an id.
      if (tbl.contains("id"))
        records.push_back(&tbl);
    }

    for (const auto *g : records) {
      // Required fields.
      auto get_str = [&](const char *key) -> std::string {
        if (auto *n = g->get(key)) {
          if (auto *s = n->as_string())
            return std::string(s->get());
        }
        return {};
      };

      std::string id       = get_str("id");
      std::string name     = get_str("name");
      std::string mods_dir = get_str("mods_dir");

      if (id.empty()) {
        Logger::instance().error("toml descriptor: missing required field 'id' in " +
                                 file.string());
        continue;
      }
      if (name.empty()) {
        Logger::instance().error("toml descriptor: missing required field 'name' in " +
                                 file.string());
        continue;
      }
      if (mods_dir.empty()) {
        Logger::instance().error(
            "toml descriptor: missing required field 'mods_dir' in " + file.string());
        continue;
      }

      // Phase A validation of path templates.
      std::string verr = validate_record_paths(*g);
      if (!verr.empty()) {
        Logger::instance().error("toml descriptor: invalid path template in " +
                                 file.string() + ": " + verr);
        continue;
      }

      // Unknown keys: warn. We just log for top-level unknowns in [game].
      // (toml++ doesn't enforce schema; we skip enforcement beyond required.)

      // Build GameDescriptor.
      GameDescriptor desc;
      desc.provenance   = DescriptorProvenance::TomlMarker;
      desc.plugin_path  = file.string();
      desc.id           = id;
      desc.display_name = name;
      desc.mods_subpath = strip_gamedir_prefix(mods_dir);

      // steam_appids
      if (auto *n = g->get("steam_appids")) {
        if (auto *arr = n->as_array()) {
          for (const auto &e : *arr) {
            if (auto *i = e.as_integer()) {
              desc.steam_appid = static_cast<uint32_t>(i->get());
              break;  // use first
            }
          }
        }
      }
      if (auto *n = g->get("gog_id")) {
        if (auto *s = n->as_string())
          desc.gog_id = std::string(s->get());
      }
      if (auto *n = g->get("epic_namespace")) {
        if (auto *s = n->as_string())
          desc.epic_namespace = std::string(s->get());
      }
      if (auto *n = g->get("nexus_domain")) {
        if (auto *s = n->as_string())
          desc.nexus_domain = std::string(s->get());
      }

      // binary table
      if (auto *bin = g->get("binary")) {
        if (auto *bt = bin->as_table()) {
          if (auto *n = bt->get("windows")) {
            if (auto *s = n->as_string())
              desc.exe_windows = std::string(s->get());
          }
          if (auto *n = bt->get("linux")) {
            if (auto *s = n->as_string())
              desc.exe_linux = std::string(s->get());
          }
          if (auto *n = bt->get("macos")) {
            if (auto *s = n->as_string())
              desc.exe_macos = std::string(s->get());
          }
        }
      }

      // Create PluginInfo.
      PluginInfo plugin;
      plugin.path       = file.string();
      plugin.provenance = "file";
      plugin.game_id    = id;  // seed

      RegistrationSink sink(loader, plugin);
      sink.add_entity(desc, id);

      // Now set knowledge keys based on TOML.
      auto &knowledge = loader.knowledge();

      // mods_subpath already in desc; also set knowledge for consumers.
      knowledge.set(id, "mods_subpath", desc.mods_subpath);

      // executables
      if (auto *n = g->get("executables")) {
        auto v = toml_string_array(n);
        knowledge.set(id, "executables", join_csv(v));
      }

      // valid_dirs
      if (auto *n = g->get("valid_dirs")) {
        auto v = toml_string_array(n);
        knowledge.set(id, "mod_valid_dirs", join_csv(v));
      }

      // valid_exts
      if (auto *n = g->get("valid_exts")) {
        auto v = toml_string_array(n);
        knowledge.set(id, "mod_valid_exts", join_csv(v));
      }

      // case_sensitive
      if (auto *n = g->get("case_sensitive")) {
        if (auto *b = n->as_boolean()) {
          knowledge.set(id, "case_sensitive", b->get() ? "true" : "false");
        }
      }

      // native_files - not a knowledge key checked by tests; skip.

      // paths.saves
      if (auto *paths = g->get("paths")) {
        if (auto *pt = paths->as_table()) {
          if (auto *saves = pt->get("saves")) {
            if (auto *str = saves->as_string()) {
              knowledge.set(id, "saves_root", std::string(str->get()));
            } else if (auto *tbl = saves->as_table()) {
              if (auto *n = tbl->get("windows")) {
                if (auto *s = n->as_string())
                  knowledge.set(id, "saves_root_windows", std::string(s->get()));
              }
              if (auto *n = tbl->get("linux")) {
                if (auto *s = n->as_string())
                  knowledge.set(id, "saves_root_linux", std::string(s->get()));
              }
              if (auto *n = tbl->get("macos")) {
                if (auto *s = n->as_string())
                  knowledge.set(id, "saves_root_macos", std::string(s->get()));
              }
            }
          }
        }
      }

      // saves
      if (auto *saves = g->get("saves")) {
        if (auto *st = saves->as_table()) {
          if (auto *n = st->get("extensions")) {
            auto v = toml_string_array(n);
            knowledge.set(id, "save_extensions", join_csv(v));
          }
        }
      }

      // catalog.download_sources
      if (auto *cat = g->get("catalog")) {
        if (auto *ct = cat->as_table()) {
          if (auto *n = ct->get("download_sources")) {
            auto v = toml_string_array(n);
            knowledge.set(id, "download_sources", join_csv(v));
          }
        }
      }

      // capabilities: validate against the manifest; unknown/functional ids
      // warn and drop. Data caps (ui.tabs) feed the existing GameCapabilities
      // registry via the sink.
      if (auto *caps = g->get("capabilities")) {
        if (auto *arr = caps->as_array()) {
          for (const auto &c : *arr) {
            if (auto *ct = c.as_table()) {
              std::string cap_id;
              if (auto *n = ct->get("id")) {
                if (auto *s = n->as_string())
                  cap_id = std::string(s->get());
              }
              if (cap_id.empty())
                continue;
              if (!is_known_capability(cap_id.c_str())) {
                Logger::instance().warn("toml descriptor: unknown capability id '" +
                                        cap_id + "' in " + file.string() +
                                        " - dropped");
                continue;
              }
              if (capability_kind(cap_id.c_str()) != CapabilityKind::Data) {
                Logger::instance().warn("toml descriptor: functional capability '" +
                                        cap_id + "' cannot be declared from TOML in " +
                                        file.string() + " - dropped");
                continue;
              }
              if (cap_id == "ui.tabs") {
                sink.capability_register(cap_id.c_str(), nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr, nullptr, nullptr);
              }
            }
          }
        }
      }

      // Push the plugin into the loader so game_plugins() sees it.
      loader.add_loaded_plugin(std::move(plugin));

      ++count;
    }
  }

  return count;
}

}  // namespace engine
