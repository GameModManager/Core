#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace engine::proton {

struct TweakResult {
  bool corefonts_installed = false;
  bool font_smoothing_set  = false;
  bool dpi_set             = false;
  int detected_dpi         = 96;
  std::string error;  // non-empty on failure
};

// Apply all proton tweaks (corefonts, font smoothing, DPI) to a prefix.
// Idempotent: checks current state before writing.
// steam_appid is needed for protontricks routing.
// If force_dpi > 0, skips auto-detection.
[[nodiscard]] TweakResult apply_proton_tweaks(const std::filesystem::path &prefix,
                                              uint32_t steam_appid, int force_dpi = 0);

// Individual checks (also idempotent):
[[nodiscard]] bool ensure_corefonts(const std::filesystem::path &prefix,
                                    uint32_t steam_appid);
[[nodiscard]] bool ensure_font_smoothing(const std::filesystem::path &prefix,
                                         uint32_t steam_appid);
[[nodiscard]] bool ensure_dpi(const std::filesystem::path &prefix, uint32_t steam_appid,
                              int force_dpi = 0);

// Detect system DPI using Qt's QScreen.
[[nodiscard]] int detect_system_dpi();

}  // namespace engine::proton
