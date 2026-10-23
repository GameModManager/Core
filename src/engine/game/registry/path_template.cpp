#include "engine/game/registry/path_template.h"

#include "platform/platform.h"

#include <cstdint>

namespace engine {

namespace {

  // A token name: [A-Za-z_][A-Za-z0-9_]*. Returns the length of the name
  // starting at s[0], or 0 when s does not start with a name character.
  size_t token_name_length(const std::string &s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
      return 0;
    size_t i = 1;
    while (i < s.size() &&
           (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
      ++i;
    return i;
  }

  // The token names the grammar knows. Case-sensitive by design: a descriptor
  // writing "%gamedir%" is a typo, not a case-insensitive match.
  bool is_known_token(const std::string &name) {
    return name == "GAMEDIR" || name == "USERPROFILE" || name == "DOCUMENTS" ||
           name == "APPDATA" || name == "LOCALAPPDATA" || name == "LOCALLOW" ||
           name == "PROGRAMDATA" || name == "STEAM" || name == "STEAM_USERDATA" ||
           name == "WINEPREFIX";
  }

  // Walk the template: on every % decide literal / token / malformed. Returns
  // an empty string when well-formed, else the error. Shared by validate
  // (which also rejects unknown names) and expand (which only needs the
  // token spans).
  std::string scan(const std::string &tmpl, bool reject_unknown) {
    for (size_t i = 0; i < tmpl.size(); ++i) {
      if (tmpl[i] != '%')
        continue;
      if (i + 1 < tmpl.size() && tmpl[i + 1] == '%') {
        ++i;  // %% - literal percent
        continue;
      }
      const size_t name_len = token_name_length(tmpl.substr(i + 1));
      if (name_len == 0)
        continue;  // lone % not followed by a token spelling: literal
      const std::string name = tmpl.substr(i + 1, name_len);
      if (i + 1 + name_len >= tmpl.size() || tmpl[i + 1 + name_len] != '%')
        return "unterminated token %" + name + " (missing closing %) in path template";
      if (reject_unknown && !is_known_token(name))
        return "unknown token %" + name + "% in path template";
      i += name_len + 1;
    }
    return {};
  }

  // True when the target is a Windows executable running under Proton (or any
  // Wine prefix) on a non-Windows host - the same rule the .exe extension
  // check and resolve_mygames_dir use.
  bool under_proton(const Platform &platform, PathTarget target) {
    return target == PathTarget::Windows && platform.platform_name() != "windows";
  }

  // Resolve one token for the target. Returns false (with error filled) when
  // the token is undefined for this target or the Platform returned nothing.
  bool resolve_token(const std::string &name, const Platform &platform,
                     PathTarget target, uint32_t steam_appid,
                     const std::filesystem::path &game_dir, std::filesystem::path &out,
                     std::string &error) {
    const bool proton = under_proton(platform, target);
    const std::string target_name =
        target == PathTarget::Windows
            ? "windows"
            : (target == PathTarget::Linux ? "linux" : "macos");
    const auto fail = [&](const std::string &reason) {
      error = "%" + name + "% is not defined for target " + target_name +
              (reason.empty() ? "" : " (" + reason + ")");
      return false;
    };

    if (name == "GAMEDIR") {
      out = game_dir;
      return true;
    }
    if (name == "USERPROFILE") {
      if (proton) {
        out = platform.game_userprofile_dir(steam_appid);
        return out.empty() ? fail("no Proton prefix user dir") : true;
      }
      out = platform.home_dir();
      return out.empty() ? fail("no home dir") : true;
    }
    if (name == "DOCUMENTS") {
      if (proton) {
        out = platform.game_documents_dir(steam_appid);
        return out.empty() ? fail("no Proton prefix Documents dir") : true;
      }
      out = platform.native_documents_dir();
      return out.empty() ? fail("no native Documents dir") : true;
    }
    if (name == "APPDATA") {
      if (proton) {
        const auto user = platform.game_userprofile_dir(steam_appid);
        if (user.empty())
          return fail("no Proton prefix user dir");
        out = user / "AppData" / "Roaming";
        return true;
      }
      out = platform.native_appdata_dir();
      return out.empty() ? fail("no native AppData dir") : true;
    }
    if (name == "LOCALAPPDATA") {
      if (proton) {
        out = platform.game_local_appdata_dir(steam_appid);
        return out.empty() ? fail("no Proton prefix Local AppData dir") : true;
      }
      out = platform.native_localappdata_dir();
      return out.empty() ? fail("no native Local AppData dir") : true;
    }
    if (name == "LOCALLOW") {
      if (proton) {
        const auto user = platform.game_userprofile_dir(steam_appid);
        if (user.empty())
          return fail("no Proton prefix user dir");
        out = user / "AppData" / "LocalLow";
        return true;
      }
      if (target == PathTarget::Windows) {
        out = platform.native_locallow_dir();
        return out.empty() ? fail("no native LocalLow dir") : true;
      }
      return fail("LocalLow only exists on Windows");
    }
    if (name == "PROGRAMDATA") {
      if (proton) {
        // Dual probe mirroring prefix_user_dir: Proton prefixes store drive_c
        // under a `pfx` subdirectory (compatdata/<appid>/pfx/drive_c/...).
        const auto prefix = platform.resolve_proton_prefix(steam_appid);
        if (prefix.empty())
          return fail("no Proton prefix");
        auto programdata = prefix / "drive_c" / "ProgramData";
        if (!std::filesystem::exists(programdata))
          programdata = prefix / "pfx" / "drive_c" / "ProgramData";
        out = programdata;
        return true;
      }
      if (target == PathTarget::Windows) {
        out = platform.native_programdata_dir();
        return out.empty() ? fail("no native ProgramData dir") : true;
      }
      if (target == PathTarget::MacOS) {
        out = platform.native_programdata_dir();
        return out.empty() ? fail("no /Library/Application Support") : true;
      }
      return fail("ProgramData only exists on Windows");
    }
    if (name == "STEAM") {
      out = platform.find_steam_root();
      return out.empty() ? fail("Steam not found") : true;
    }
    if (name == "STEAM_USERDATA") {
      out = platform.steam_userdata_dir();
      return out.empty() ? fail("Steam not found") : true;
    }
    if (name == "WINEPREFIX") {
      if (!proton)
        return fail("WINEPREFIX only applies to a Windows target under Proton");
      out = platform.resolve_proton_prefix(steam_appid);
      return out.empty() ? fail("no Proton prefix") : true;
    }
    // Unreachable: scan() rejects unknown names before expansion runs.
    error = "%" + name + "% is not a known token";
    return false;
  }

}  // namespace

std::string validate_path_template(const std::string &tmpl) {
  return scan(tmpl, /*reject_unknown=*/true);
}

std::filesystem::path expand_path_template(const std::string &tmpl,
                                           const Platform *platform, PathTarget target,
                                           uint32_t steam_appid,
                                           const std::filesystem::path &game_dir,
                                           std::string &error) {
  error.clear();
  if (!platform) {
    error = "no Platform to expand path template against";
    return {};
  }

  std::string result;
  for (size_t i = 0; i < tmpl.size(); ++i) {
    if (tmpl[i] != '%') {
      result += tmpl[i];
      continue;
    }
    if (i + 1 < tmpl.size() && tmpl[i + 1] == '%') {
      result += '%';
      ++i;
      continue;
    }
    const size_t name_len = token_name_length(tmpl.substr(i + 1));
    if (name_len == 0) {
      result += '%';  // lone % not followed by a token spelling: literal
      continue;
    }
    const std::string name = tmpl.substr(i + 1, name_len);
    if (i + 1 + name_len >= tmpl.size() || tmpl[i + 1 + name_len] != '%') {
      error = "unterminated token %" + name + " (missing closing %) in path template";
      return {};
    }
    std::filesystem::path value;
    if (!resolve_token(name, *platform, target, steam_appid, game_dir, value, error))
      return {};
    // No recursion: the resolved value is appended verbatim, never rescanned
    // for % (a path containing a literal "%USERPROFILE%" survives).
    result += value.string();
    i += name_len + 1;
  }
  return {result};
}

}  // namespace engine
