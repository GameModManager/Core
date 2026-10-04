#pragma once

#include <filesystem>
#include <string>

#include "engine/pipeline/stage.h"
#include "engine/source/update_policy.h"

namespace engine {

// Check a freshly downloaded archive against the digest the pack declared for
// it (Mod::expected_sha256 / expected_file_size). Only the bytes actually on
// disk and the filesystem's own size are used as evidence - the file id the pin
// also carries is the pin restated, so comparing it would be a tautology, not a
// verification.
//
// Returns NoPin when the pack declared nothing to check (no digest, or a
// "latest" policy, which is a promise to take whatever the source has now),
// Incomplete when it declared a digest the file cannot be hashed against, and
// Match or Mismatch otherwise. Mismatch is the answer the caller must treat as
// fatal: those bytes are not the ones the pack named.
Source::VerifyResult verify_downloaded_archive(const Mod &mod,
                                               const std::filesystem::path &archive);

class FetchStage : public Stage {
public:
  bool execute(Mod &mod, PipelineContext &ctx) override;
  std::string name() const override { return "Fetch"; }
  std::string description() const override {
    return "Downloads the mod archive into the instance download cache";
  }
  std::string condition() const override { return "Archive downloaded"; }
};

}  // namespace engine
