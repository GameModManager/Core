#pragma once

// Linux file type dispatcher - fork+exec based launch for each file type.
// Replaces the old std::system("cmd &") pattern in LinuxPlatform with
// proper fork+exec that avoids shell injection.

#include "engine/platform/file_type_dispatch.h"

namespace engine
{

class LinuxFileTypeDispatcher : public FileTypeDispatcher
{
public:
  [[nodiscard]] bool can_launch(FileType type) const override;

  [[nodiscard]] LaunchResult launch(const std::filesystem::path& file,
                                    const LaunchOptions& options = {}) const override;

private:
  // Ensure the file has +x permission. Returns false only if chmod fails.
  static bool ensure_executable(const std::filesystem::path& file);

  // Low-level fork+exec. The caller is responsible for being in a
  // fork-safe state (no Qt objects, single-threaded, etc.).
  static LaunchResult fork_exec(const std::filesystem::path& executable,
                                const std::vector<std::string>& argv,
                                const std::filesystem::path& working_dir,
                                bool background);
};

}  // namespace engine
