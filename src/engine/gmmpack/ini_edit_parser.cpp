#include "engine/gmmpack/ini_edit_parser.h"

namespace engine::gmmpack {

IniEntry parse_ini_entry(const nlohmann::json &j) {
  IniEntry ie;
  ie.target_file = j.value("targetFile", "");
  if (j.contains("tweaks")) {
    for (const auto &tj : j["tweaks"]) {
      IniTweak tweak;
      tweak.id      = tj.value("id", "");
      tweak.name    = tj.value("name", "");
      tweak.status  = tj.value("status", "recommended");
      tweak.enabled = tj.value("enabled", true);
      tweak.content = tj.value("content", "");
      if (tj.contains("sourceModId") && !tj["sourceModId"].is_null()) {
        tweak.source_mod_id     = tj["sourceModId"].get<std::string>();
        tweak.has_source_mod_id = true;
      }
      ie.tweaks.push_back(std::move(tweak));
    }
  }
  return ie;
}

modpack::IniEditFile to_edit_file(const IniEntry &entry) {
  modpack::IniEditFile file;
  file.target_file = entry.target_file;
  for (const auto &tweak : entry.tweaks) {
    modpack::IniTweak out;
    out.id      = tweak.id;
    out.name    = tweak.name;
    out.status  = tweak.status == "required" ? modpack::TweakStatus::Required
                                             : modpack::TweakStatus::Recommended;
    out.enabled = tweak.enabled;
    if (tweak.has_source_mod_id)
      out.source_mod_id = tweak.source_mod_id;
    out.content = tweak.content;
    file.tweaks.push_back(std::move(out));
  }
  return file;
}

}  // namespace engine::gmmpack
