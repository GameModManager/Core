#pragma once
#include "engine/mod/model/mod.h"
#include <string>

namespace engine {

struct ModInfo {
    virtual ~ModInfo() = default;
    std::string folder_name;
    std::string display_name;
    std::string version;
    int priority = -1;
    virtual ModType type() const = 0;
    virtual bool is_pseudo() const { return false; }
};

struct ModInfoRegular : ModInfo {
    ModType type() const override { return ModType::Regular; }
    std::string source_type;
    std::string source_id;
    bool enabled = true;
    bool is_fomod = false;
    bool root_override = false;
    bool invalid_data = false;
    bool no_metadata = false;
};

struct ModInfoForeign : ModInfo {
    ModType type() const override { return ModType::Foreign; }
};

struct ModInfoSeparator : ModInfo {
    ModType type() const override { return ModType::Separator; }
    std::string separator_color;
    bool folded = false;
    std::string parent_id;
};

struct ModInfoBackup : ModInfo {
    ModType type() const override { return ModType::Backup; }
};

struct ModInfoOverwrite : ModInfo {
    ModType type() const override { return ModType::Overwrite; }
    bool is_pseudo() const override { return true; }
};

struct WithConflictInfo {
    int conflict_wins = 0;
    int conflict_losses = 0;
    bool redundant = false;
};

}  // namespace engine
