#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

enum class ModState {
    Downloaded,
    Extracted,
    Installed,
    Staged,
    Deployed,
};

struct ModFile {
    std::string relative_path;
    uint64_t size = 0;
};

struct Mod {
    std::string id;
    std::string name;
    std::string version;
    ModState state = ModState::Downloaded;
    std::vector<ModFile> files;
};

}  // namespace engine
