#pragma once

// Ported from FOMOD Plus (MIT), installer/lib/FileInstaller.h/.cpp. FOMOD
// Plus builds a fresh MO2 IFileTree with the selected files; GameModManager
// applies the same selection/priority/copy semantics directly to the
// extracted staging directory (Qt-free, std::filesystem).

#include "engine/mod/fomod/fomod_view_model.h"
#include "engine/mod/fomod/module_config.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine {

class FomodViewModel;

// Transforms the extracted staging directory into the final FOMOD install:
// collects (required + selected-plugin + matched-conditional) files, sorts by
// ascending priority (XML order tie-break, later wins on overwrite), copies
// them into a fresh tree, and swaps it in place of the staging dir.
class FomodFileInstaller {
public:
    explicit FomodFileInstaller(const std::filesystem::path& modRoot,
        const std::shared_ptr<FomodViewModel>& viewModel);

    // Applies the install to modRoot. Files whose source is missing from the
    // archive are skipped and appended to `missing` (the caller decides
    // whether to warn). Returns false when the transform fails on disk.
    bool apply(std::vector<std::string>* missing);

    // The wizard's choices serialized in the FOMOD Plus fomod.json shape
    // ({"steps":[{"name","groups":[{"name","plugins","deselected"}]}]}), used
    // for previous-choice restore on reinstall.
    [[nodiscard]] std::string generateFomodJson() const;

private:
    std::filesystem::path mModRoot;
    std::shared_ptr<FomodViewModel> mViewModel;
};

// The files the FOMOD install will place, in copy order (ascending priority,
// XML order tie-break). Shared by FomodFileInstaller and the install wizard,
// which preflights the list against the archive to warn about missing sources
// before the user commits to the install.
[[nodiscard]] std::vector<engine::File> collect_files_to_install(const FomodViewModel& view_model);

// The view model's current choices in the FOMOD Plus fomod.json shape. Shared
// by FomodFileInstaller and the install wizard's return value.
[[nodiscard]] std::string generate_fomod_json(const FomodViewModel& view_model);

}  // namespace engine
