#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace engine {

// Which registration path produced a GameDescriptor. v3 Phase P0 only ever
// builds it from the compiled ABI adapters (v1 register_identity / v2
// register_game); Python and TOML markers arrive in later phases.
enum class DescriptorProvenance {
  CompiledV1,
  CompiledV2,
  CompiledV3,
  Python,
  TomlMarker,
};

// What a game IS: the identity a game plugin declares, independent of what
// registering it does (that is RegistrationSink's job). One descriptor per
// register_identity (v1) / register_game (v2) call.
//
// Optionality (v3 spec revision): REQUIRED fields are plain strings - empty
// means "not supplied", a state only the ABI adapters can produce (the ABI
// passes nullable char*). OPTIONAL fields are std::optional / vectors -
// empty means absent. Consumers must treat an empty REQUIRED field as
// "unknown", never as a real value.
struct GameDescriptor {
  DescriptorProvenance provenance = DescriptorProvenance::CompiledV2;
  std::string plugin_path;  // the .so (or .py) that declared this game

  // -- REQUIRED --
  std::string id;            // canonical game_id ("SkyrimSpecialEdition")
  std::string display_name;  // "Skyrim Special Edition"
  std::string mods_subpath;  // deploy target subpath ("Data")

  // -- OPTIONAL --
  std::optional<uint32_t> steam_appid;
  std::optional<std::string> gog_id;
  std::optional<std::string> epic_namespace;
  std::optional<std::string> nexus_domain;
  std::optional<std::string> exe_windows;
  std::optional<std::string> exe_linux;
  std::optional<std::string> exe_macos;
};

}  // namespace engine
