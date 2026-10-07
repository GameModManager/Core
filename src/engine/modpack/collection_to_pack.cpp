#include "engine/modpack/collection_to_pack.h"

#include <cctype>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace engine::modpack {

namespace {

  namespace Coll = engine::Collection;
  namespace Gmm  = engine::gmmpack;

  std::string resolution_name(Coll::SourceResolution resolution) {
    switch (resolution) {
    case Coll::SourceResolution::Api:
      return "api";
    case Coll::SourceResolution::Browser:
      return "browser";
    case Coll::SourceResolution::ClientSubscription:
      return "client-subscription";
    }
    return "browser";
  }

  std::string update_policy_name(Coll::UpdatePolicy policy) {
    // All three states, spelled the way the pack schema spells them.
    return std::string(Coll::to_string(policy));
  }

  Gmm::ModCategory convert_category(Coll::ModCategory category) {
    switch (category) {
    case Coll::ModCategory::Required:
      return Gmm::ModCategory::Required;
    case Coll::ModCategory::Recommended:
      return Gmm::ModCategory::Recommended;
    case Coll::ModCategory::Optional:
      return Gmm::ModCategory::Optional;
    }
    return Gmm::ModCategory::Optional;
  }

  std::optional<std::string> non_empty(const std::string &value) {
    if (value.empty())
      return std::nullopt;
    return value;
  }

  // Numeric provider ids stay numeric, slugs stay strings.
  std::variant<int64_t, std::string> convert_mod_id(const std::string &id) {
    bool numeric = !id.empty();
    for (char c : id) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        numeric = false;
        break;
      }
    }
    if (numeric) {
      try {
        return static_cast<int64_t>(std::stoll(id));
      } catch (const std::exception &) {
      }
    }
    return id;
  }

  Gmm::ModSource convert_source(const Coll::ModSource &source) {
    return std::visit(
        [](const auto &src) -> Gmm::ModSource {
          using T = std::decay_t<decltype(src)>;
          if constexpr (std::is_same_v<T, Coll::SourceNexus>) {
            Gmm::ModSourceNexus out;
            out.resolution  = resolution_name(src.resolution);
            out.game_domain = src.game_domain;
            out.mod_id      = src.mod_id;
            if (src.file_id > 0)
              out.file_id = src.file_id;
            out.version   = non_empty(src.version);
            out.file_name = non_empty(src.file_name);
            if (src.file_size > 0)
              out.file_size = src.file_size;
            out.sha256        = non_empty(src.sha256);
            out.md5           = non_empty(src.md5);
            out.update_policy = update_policy_name(src.update_policy);
            return out;
          } else if constexpr (std::is_same_v<T, Coll::SourceDirect>) {
            Gmm::ModSourceDirect out;
            out.resolution    = resolution_name(src.resolution);
            out.url           = src.url;
            out.version       = non_empty(src.version);
            out.file_name     = non_empty(src.file_name);
            out.sha256        = non_empty(src.sha256);
            out.update_policy = update_policy_name(src.update_policy);
            return out;
          } else if constexpr (std::is_same_v<T, Coll::SourceLoversLab>) {
            Gmm::ModSourceLoversLab out;
            out.resolution    = resolution_name(src.resolution);
            out.mod_id        = convert_mod_id(src.mod_id);
            out.section_slug  = src.section_slug;
            out.version       = non_empty(src.version);
            out.file_name     = non_empty(src.file_name);
            out.sha256        = non_empty(src.sha256);
            out.update_policy = update_policy_name(src.update_policy);
            return out;
          } else if constexpr (std::is_same_v<T, Coll::SourceModPub>) {
            Gmm::ModSourceModPub out;
            out.resolution    = resolution_name(src.resolution);
            out.mod_id        = convert_mod_id(src.mod_id);
            out.version       = non_empty(src.version);
            out.file_name     = non_empty(src.file_name);
            out.sha256        = non_empty(src.sha256);
            out.update_policy = update_policy_name(src.update_policy);
            return out;
          } else {
            Gmm::ModSourceSteamWorkshop out;
            out.app_id           = src.app_id;
            out.workshop_item_id = src.workshop_item_id;
            out.version          = non_empty(src.version);
            return out;
          }
        },
        source);
  }

  std::string rule_name(Coll::RuleType type) {
    switch (type) {
    case Coll::RuleType::Before:
      return "before";
    case Coll::RuleType::After:
      return "after";
    case Coll::RuleType::Requires:
      return "requires";
    case Coll::RuleType::Conflicts:
      return "conflicts";
    case Coll::RuleType::Recommends:
      return "recommends";
    case Coll::RuleType::Provides:
      return "provides";
    }
    return "requires";
  }

}  // namespace

gmmpack::Gmmpack manifest_to_gmmpack(const Coll::Manifest &manifest) {
  Gmm::Gmmpack pack;
  pack.manifest.gmmpack_schema   = "1.0.0";
  pack.manifest.id               = manifest.id;
  pack.manifest.revision         = static_cast<int>(manifest.revision);
  pack.manifest.info.name        = manifest.info.name;
  pack.manifest.info.author      = manifest.info.author;
  pack.manifest.info.description = manifest.info.description;
  pack.manifest.info.gmm_game_id = manifest.info.game_id;
  pack.manifest.info.homepage    = manifest.info.homepage;
  pack.manifest.info.created_at  = manifest.info.created_at;
  pack.manifest.info.updated_at  = manifest.info.updated_at;
  for (const auto &tool : manifest.tools) {
    Gmm::ManifestTool entry;
    entry.id       = tool.id;
    entry.name     = tool.name;
    entry.homepage = tool.homepage;
    pack.manifest.tools.push_back(std::move(entry));
  }
  for (const auto &rule : manifest.rules) {
    Gmm::ManifestRule entry;
    entry.type = rule_name(rule.type);
    entry.from = rule.from;
    entry.to   = rule.to;
    entry.note = rule.note;
    pack.manifest.rules.push_back(std::move(entry));
  }
  pack.manifest.load_order.plugin_hint = manifest.load_order.plugin_hint;
  for (const auto &group : manifest.choice_groups) {
    Gmm::ChoiceGroup entry;
    entry.id   = group.id;
    entry.name = group.name;
    entry.mode =
        group.mode == Coll::ChoiceMode::ExactlyOne ? "exactly-one" : "at-most-one";
    entry.member_mod_ids = group.member_mod_ids;
    pack.manifest.choice_groups.push_back(std::move(entry));
  }
  for (const auto &mod : manifest.mods) {
    Gmm::ModEntry entry;
    entry.id       = mod.id;
    entry.name     = mod.name;
    entry.phase    = mod.phase;
    entry.category = convert_category(mod.category);
    entry.source   = convert_source(mod.source);
    if (!mod.installer_choices.type.empty()) {
      Gmm::InstallerChoices choices;
      choices.type            = mod.installer_choices.type;
      choices.selections      = mod.installer_choices.selections;
      entry.installer_choices = std::move(choices);
    }
    pack.mods.push_back(std::move(entry));
  }
  return pack;
}

}  // namespace engine::modpack