#pragma once

#include <string>

#include "engine/pipeline/stage.h"

namespace engine {

namespace Source {

class InstallationManager : public Stage {
public:
  bool execute(Mod &mod, PipelineContext &ctx) override;
  std::string name() const override { return "Install"; }
  std::string description() const override {
    return "Copies the staged files into the instance mods folder";
  }
  std::string condition() const override {
    return "Files copied to mods folder";
  }
};

} // namespace Source

// Backward-compatible alias
using InstallStage = Source::InstallationManager;

} // namespace engine
