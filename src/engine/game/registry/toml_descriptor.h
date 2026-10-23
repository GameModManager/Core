#pragma once

#include "engine/plugin_host/plugin_loader.h"

#include <filesystem>

namespace engine {

// v3 Phase P2 - load plaintext TOML game descriptors from a directory.
//
// Scans `dir` for *.toml files (sorted by filename for determinism), parses
// each into a GameDescriptor + data-capability bindings, and registers them
// through the SAME RegistrationSink the compiled plugins use. Returns the
// number of game records successfully registered. A parse error or a missing
// required field refuses that record/file and logs loudly; other files and
// records still load.
[[nodiscard]] int load_toml_game_dir(const std::filesystem::path &dir,
                                      PluginLoader &loader);

}  // namespace engine
