#include "engine/instance/instance.h"

#include "engine/instance/toml_utils.h"
#include "platform/platform.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace engine {

namespace {

  // All-occurrence literal replace; std::string has no replace_all.
  void replace_all(std::string &s, const std::string &from, const std::string &to) {
    if (from.empty())
      return;
    for (size_t pos = s.find(from); pos != std::string::npos;
         pos        = s.find(from, pos + to.size()))
      s.replace(pos, from.size(), to);
  }

  // Expands `$NAME` and `%NAME%` environment-variable tokens in one
  // left-to-right pass. A name that is not set in the environment is copied
  // through verbatim: substituting nothing for it would silently relocate
  // the folder - a bare `$UNSET/mods` would collapse to `/mods`, and a
  // relative leftover anchors at the instance root, so an unknown variable
  // must stay visible rather than vanish.
  std::string expand_env_tokens(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
      if (s[i] == '%') {
        const size_t end = s.find('%', i + 1);
        if (end != std::string::npos) {
          const std::string name = s.substr(i + 1, end - i - 1);
          if (const char *v = std::getenv(name.c_str())) {
            out += v;
          } else {
            out += s.substr(i, end - i + 1);
          }
          i = end + 1;
          continue;
        }
      } else if (s[i] == '$') {
        // `$NAME` runs to the first character that cannot be part of a name;
        // a bare `$` is an ordinary path character and is left alone.
        size_t stop = i + 1;
        while (
            stop < s.size() &&
            (std::isalnum(static_cast<unsigned char>(s[stop])) != 0 || s[stop] == '_'))
          ++stop;
        if (stop > i + 1) {
          const std::string name = s.substr(i + 1, stop - i - 1);
          if (const char *v = std::getenv(name.c_str())) {
            out += v;
          } else {
            out.append(s, i, stop - i);
          }
          i = stop;
          continue;
        }
      }
      out += s[i];
      ++i;
    }
    return out;
  }

}  // namespace

std::filesystem::path expand_instance_path(const std::filesystem::path &path,
                                           const std::filesystem::path &base) {
  if (path.empty())
    return {};

  std::string s = path.string();

  // The base directory, under both spellings: the one the Paths tab
  // advertises and MO2's %BASE_DIR%. Substituted only when there is a base
  // to substitute - with an empty base the token would otherwise expand to
  // nothing and leave a root-level "/mods".
  if (!base.empty()) {
    replace_all(s, "$BASE_DIRECTORY", base.string());
    replace_all(s, "%BASE_DIR%", base.string());
  }

  // A leading ~ is the home directory, matching the spelling the game
  // knowledge loader already accepts for a "game_mods_dir" declaration.
  if (s.size() > 1 && s.front() == '~' && s[1] == '/')
    s = safe_home_dir().string() + s.substr(1);

  s = expand_env_tokens(s);

  // A relative result is anchored at the instance root so it resolves to
  // the same directory no matter what the process working directory is.
  std::filesystem::path resolved(s);
  if (resolved.is_relative())
    resolved = base / resolved;
  const std::string normalized = resolved.lexically_normal().string();
  // lexically_normal keeps a trailing separator; a directory must compare
  // equal to the same directory spelled without one, or a plain string
  // comparison against a resolved path (the self-referential deploy guard,
  // for one) would miss.
  if (normalized.size() > 1 && normalized.back() == '/')
    return std::filesystem::path(normalized.substr(0, normalized.size() - 1));
  return std::filesystem::path(normalized);
}

Instance Instance::portable(const std::filesystem::path &root) {
  Instance inst;
  inst.info_.root     = root;
  inst.info_.portable = true;
  return inst;
}

Instance Instance::installed(const std::string &name,
                             const std::filesystem::path &instances_root) {
  Instance inst;
  inst.info_.root     = instances_root / name;
  inst.info_.portable = false;
  return inst;
}

Instance Instance::from_root(const std::filesystem::path &root) {
  Instance inst;
  inst.info_.root = root;
  return inst;
}

std::filesystem::path Instance::path_for(InstanceKind kind) const {
  // A configured override may carry a path variable ($BASE_DIRECTORY,
  // %BASE_DIR%, $HOME, ...). Resolve it against the instance root here so
  // every consumer - and the plugin ABI - sees the same concrete directory
  // while the stored value stays exactly as configured, so a $BASE_DIRECTORY
  // override keeps following a relocation. An empty override keeps the
  // default under the root. Overrides may point anywhere; a directory
  // outside the instance is a valid configuration.
  const auto configured = [this](const std::filesystem::path &override_value,
                                 const char *leaf) {
    return override_value.empty() ? info_.root / leaf
                                  : expand_instance_path(override_value, info_.root);
  };

  switch (kind) {
  case InstanceKind::Mods:
    return configured(info_.mods_dir, "mods");
  case InstanceKind::Downloads:
    return configured(info_.downloads_dir, "downloads");
  case InstanceKind::Cache:
    return configured(info_.cache_dir, "cache");
  case InstanceKind::CacheArchives:
    return configured(info_.cache_dir, "cache") / "archives";
  case InstanceKind::CacheThumbnails:
    return configured(info_.cache_dir, "cache") / "thumbnails";
  case InstanceKind::Profiles:
    return configured(info_.profiles_dir, "profiles");
  case InstanceKind::Overwrite:
    return configured(info_.overwrite_dir, "overwrite");
  case InstanceKind::Plugins:
    return info_.root / "plugins";
  case InstanceKind::Logs:
    return info_.root / "logs";
  case InstanceKind::Config:
    return info_.root / "config";
  case InstanceKind::Masterlists:
    return info_.root / "masterlists";
  }
  return {};
}

void Instance::set_path_override(InstanceKind kind, const std::filesystem::path &path) {
  switch (kind) {
  case InstanceKind::Mods:
    info_.mods_dir = path;
    break;
  case InstanceKind::Downloads:
    info_.downloads_dir = path;
    break;
  case InstanceKind::Cache:
    info_.cache_dir = path;
    break;
  case InstanceKind::Profiles:
    info_.profiles_dir = path;
    break;
  case InstanceKind::Overwrite:
    info_.overwrite_dir = path;
    break;
  default:
    break;
  }
}

std::filesystem::path Instance::path_override(InstanceKind kind) const {
  switch (kind) {
  case InstanceKind::Mods:
    return info_.mods_dir;
  case InstanceKind::Downloads:
    return info_.downloads_dir;
  case InstanceKind::Cache:
    return info_.cache_dir;
  case InstanceKind::Profiles:
    return info_.profiles_dir;
  case InstanceKind::Overwrite:
    return info_.overwrite_dir;
  default:
    return {};
  }
}

std::filesystem::path Instance::toml_path() const {
  return info_.root / "instance.toml";
}

bool Instance::create_directories() const {
  std::error_code ec;
  std::filesystem::create_directories(info_.root, ec);
  if (ec)
    return false;

  const InstanceKind dirs[] = {
      InstanceKind::Mods,          InstanceKind::Overwrite,
      InstanceKind::Profiles,      InstanceKind::Downloads,
      InstanceKind::CacheArchives, InstanceKind::CacheThumbnails,
      InstanceKind::Plugins,       InstanceKind::Config,
  };
  for (auto kind : dirs) {
    std::filesystem::create_directories(path_for(kind), ec);
    if (ec)
      return false;
  }
  return true;
}

bool Instance::write_toml() const {
  // Read-modify-write: parse the existing file first so that app-owned
  // sections (e.g. `executables`) survive untouched.  A missing or
  // unparseable file starts from an empty table (fresh creation path).
  auto tbl = parse_instance_toml(toml_path());
  if (!tbl) {
    tbl = toml::table{};
  }

  tbl->insert_or_assign("game_id", info_.game_id);
  tbl->insert_or_assign("name", info_.display_name);
  tbl->insert_or_assign("portable", info_.portable);

  if (info_.steam_appid > 0) {
    tbl->insert_or_assign("steam_appid", static_cast<int64_t>(info_.steam_appid));
  } else {
    tbl->erase("steam_appid");
  }

  auto assign_or_erase = [&](const std::string &key, const std::filesystem::path &val) {
    if (val.empty())
      tbl->erase(key);
    else
      tbl->insert_or_assign(key, val.string());
  };

  assign_or_erase("game_dir", info_.game_dir);
  assign_or_erase("game_mods_dir", info_.game_mods_dir);

  // Per-folder overrides.
  assign_or_erase("mods_dir", info_.mods_dir);
  assign_or_erase("downloads_dir", info_.downloads_dir);
  assign_or_erase("cache_dir", info_.cache_dir);
  assign_or_erase("profiles_dir", info_.profiles_dir);
  assign_or_erase("overwrite_dir", info_.overwrite_dir);
  assign_or_erase("plugins_txt_path", info_.plugins_txt_path);

  auto assign_str_or_erase = [&](const std::string &key, const std::string &val) {
    if (val.empty())
      tbl->erase(key);
    else
      tbl->insert_or_assign(key, val);
  };

  auto assign_int_or_erase = [&](const std::string &key, int64_t val) {
    if (val == 0)
      tbl->erase(key);
    else
      tbl->insert_or_assign(key, val);
  };

  assign_str_or_erase("proton_runner", info_.proton_runner);
  assign_str_or_erase("deploy_strategy", info_.deploy_strategy);
  assign_str_or_erase("modpack_id", info_.modpack_id);
  assign_int_or_erase("modpack_revision", info_.modpack_revision);
  assign_str_or_erase("last_tab", info_.last_tab);

  // Per-instance appearance overrides (Workspace-1065): surgical key updates
  // inside [appearance] so unrelated keys survive; drop the section when it
  // ends up empty.
  {
    toml::table *app = (*tbl)["appearance"].as_table();
    if (!info_.appearance_theme.empty() || !info_.appearance_style.empty() ||
        !info_.appearance_icon_pack.empty()) {
      if (!app) {
        tbl->insert_or_assign("appearance", toml::table{});
        app = (*tbl)["appearance"].as_table();
      }
    }
    if (app) {
      auto assign_sub_or_erase = [&](const std::string &key, const std::string &val) {
        if (val.empty())
          app->erase(key);
        else
          app->insert_or_assign(key, val);
      };
      assign_sub_or_erase("theme", info_.appearance_theme);
      assign_sub_or_erase("style", info_.appearance_style);
      assign_sub_or_erase("icon_pack", info_.appearance_icon_pack);
      if (app->empty())
        tbl->erase("appearance");
    }
  }

  // Per-instance disabled plugins (Workspace-1065): nullopt leaves any
  // existing [plugins] content untouched (global fallback); a set value
  // (possibly empty) owns the `disabled` key.
  if (info_.plugins_disabled.has_value()) {
    toml::table *plug = (*tbl)["plugins"].as_table();
    if (!plug) {
      tbl->insert_or_assign("plugins", toml::table{});
      plug = (*tbl)["plugins"].as_table();
    }
    if (plug) {
      toml::array disabled;
      for (const auto &name : *info_.plugins_disabled)
        disabled.push_back(name);
      plug->insert_or_assign("disabled", disabled);
    }
  }

  // Per-instance plugin options (Workspace-1065): only our [plugin_options]
  // section is managed; an empty map erases it (all-fallback). Individual
  // entries fall back to globals at read time, so only overrides are stored.
  if (info_.plugin_options.empty()) {
    tbl->erase("plugin_options");
  } else {
    toml::table opts;
    for (const auto &[basename, settings] : info_.plugin_options) {
      toml::table sub;
      for (const auto &[key, value] : settings)
        sub.insert_or_assign(key, value);
      opts.insert_or_assign(basename, sub);
    }
    tbl->insert_or_assign("plugin_options", opts);
  }

  std::ofstream out(toml_path());
  if (!out)
    return false;
  out << serialize_instance_toml(*tbl);
  const bool ok = out.good();
  if (ok)
    invalidate_instance_toml_cache(toml_path());
  return ok;
}

bool Instance::read_toml() {
  auto tbl = parse_instance_toml(toml_path());
  if (!tbl)
    return false;

  if (auto v = (*tbl)["game_id"].value<std::string>()) {
    info_.game_id = *v;
  }
  if (auto v = (*tbl)["name"].value<std::string>()) {
    info_.display_name = *v;
  }
  if (auto v = (*tbl)["game_dir"].value<std::string>()) {
    info_.game_dir = *v;
  }
  if (auto v = (*tbl)["game_mods_dir"].value<std::string>()) {
    info_.game_mods_dir = *v;
  }
  if (auto v = (*tbl)["mods_dir"].value<std::string>()) {
    info_.mods_dir = *v;
  }
  if (auto v = (*tbl)["downloads_dir"].value<std::string>()) {
    info_.downloads_dir = *v;
  }
  if (auto v = (*tbl)["cache_dir"].value<std::string>()) {
    info_.cache_dir = *v;
  }
  if (auto v = (*tbl)["profiles_dir"].value<std::string>()) {
    info_.profiles_dir = *v;
  }
  if (auto v = (*tbl)["overwrite_dir"].value<std::string>()) {
    info_.overwrite_dir = *v;
  }
  if (auto v = (*tbl)["plugins_txt_path"].value<std::string>()) {
    info_.plugins_txt_path = *v;
  }
  if (auto v = (*tbl)["proton_runner"].value<std::string>()) {
    info_.proton_runner = *v;
  }
  if (auto v = (*tbl)["deploy_strategy"].value<std::string>()) {
    info_.deploy_strategy = *v;
  }
  if (auto v = (*tbl)["modpack_id"].value<std::string>()) {
    info_.modpack_id = *v;
  }
  if (auto v = (*tbl)["modpack_revision"].value<int64_t>()) {
    info_.modpack_revision = *v;
  }
  if (auto v = (*tbl)["last_tab"].value<std::string>()) {
    info_.last_tab = *v;
  }
  // Per-instance appearance overrides (Workspace-1065). Missing keys stay
  // empty (= follow the global Settings value); an explicitly empty string
  // is also treated as unset.
  if (const toml::table *app = (*tbl)["appearance"].as_table()) {
    if (auto v = (*app)["theme"].value<std::string>())
      info_.appearance_theme = *v;
    if (auto v = (*app)["style"].value<std::string>())
      info_.appearance_style = *v;
    if (auto v = (*app)["icon_pack"].value<std::string>())
      info_.appearance_icon_pack = *v;
  }
  // Per-instance disabled plugins (Workspace-1065). The key's presence (even
  // as an empty array) marks an explicit override; a missing section/key
  // leaves nullopt (= global fallback).
  if (const toml::table *plug = (*tbl)["plugins"].as_table()) {
    if (const toml::array *disabled = (*plug)["disabled"].as_array()) {
      std::vector<std::string> names;
      for (const auto &node : *disabled) {
        if (auto v = node.value<std::string>())
          names.push_back(*v);
      }
      info_.plugins_disabled = std::move(names);
    }
  }
  // Per-instance plugin options (Workspace-1065). Dotted keys
  // (plugin1.option1 = "value") and nested tables ([plugin_options."a.so"])
  // both parse to nested tables; only string values are kept.
  if (const toml::table *opts = (*tbl)["plugin_options"].as_table()) {
    for (auto &&[basename, node] : *opts) {
      if (const toml::table *sub = node.as_table()) {
        for (auto &&[key, val] : *sub) {
          if (auto v = val.value<std::string>())
            info_.plugin_options[std::string(basename)][std::string(key)] = *v;
        }
      }
    }
  }
  if (auto v = (*tbl)["portable"].value<bool>()) {
    info_.portable = *v;
  }
  if (auto v = (*tbl)["steam_appid"].value<int64_t>()) {
    info_.steam_appid = static_cast<uint32_t>(*v);
  }
  return true;
}

bool Instance::write_key(const std::string &key, const std::string &value) const {
  auto path = toml_path();
  // Read-modify-write: parse the full file (legacy repair included) so
  // app-owned sections like `executables` survive untouched. A missing or
  // unparseable file starts from an empty table.
  auto tbl = parse_instance_toml(path);
  if (!tbl) {
    tbl = toml::table{};
  }
  if (value.empty()) {
    tbl->erase(key);
  } else {
    tbl->insert_or_assign(key, value);
  }

  std::ofstream out(path);
  if (!out)
    return false;
  out << serialize_instance_toml(*tbl);
  const bool ok = out.good();
  if (ok)
    invalidate_instance_toml_cache(path);
  return ok;
}

std::string Instance::to_instance_name(const std::string &display_name) {
  static const std::string invalid = R"(\/:*?"<>|)";
  std::string result;
  result.reserve(display_name.size());
  for (char c : display_name) {
    // Control characters (incl. NUL) are never filesystem-safe.
    if (static_cast<unsigned char>(c) < 0x20 || c == '\x7f')
      continue;
    if (invalid.find(c) != std::string::npos)
      continue;
    result += c;
  }
  // Trim dots and whitespace at both ends: leading dots would hide the
  // folder on Unix, trailing dots/spaces are illegal on Windows, and
  // trimming is what turns ".", ".." and "..." into "" (degenerate input).
  const auto is_trim = [](unsigned char c) {
    return c == '.' || c == ' ' || c == '\t';
  };
  while (!result.empty() && is_trim(result.front()))
    result.erase(result.begin());
  while (!result.empty() && is_trim(result.back()))
    result.pop_back();
  return result;
}

std::filesystem::path
Instance::resolve_portable_root(const std::filesystem::path &exe_dir) {
  auto toml = exe_dir / "instance.toml";
  if (std::filesystem::exists(toml)) {
    return exe_dir;
  }
  return {};
}

bool Instance::is_portable(const std::filesystem::path &exe_dir) {
  return std::filesystem::exists(exe_dir / "instance.toml");
}

}  // namespace engine
