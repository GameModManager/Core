#pragma once

// Shared Platform base for test doubles.
//
// Every pure virtual of engine::Platform is implemented once, inertly, here.
// A test stub then derives from InertPlatform and declares only the behaviour
// it is actually asserting on, instead of restating the whole pure contract -
// which is what the nine stubs used to do, and which made every new pure
// virtual a nine-file edit.
//
// A real adaptor (LinuxPlatform / MacOSPlatform / WindowsPlatform) getting a
// base default instead of an override is a P1 bug, so this file is for test
// doubles only. platform_contract_scan.py checks the three real adaptors
// against every pure virtual and fails if one is missing.

#include "platform/platform.h"

#include <filesystem>
#include <string>
#include <vector>

namespace gmm_test {

class InertPlatform : public engine::Platform {
public:
  [[nodiscard]] std::string platform_name() const override { return "test"; }
  [[nodiscard]] std::filesystem::path data_dir() const override { return {}; }
  [[nodiscard]] std::filesystem::path config_dir() const override { return {}; }
  [[nodiscard]] std::filesystem::path cache_dir() const override { return {}; }
  [[nodiscard]] std::filesystem::path find_steam_root() const override { return {}; }
  [[nodiscard]] bool
  launch_executable(const std::filesystem::path &,
                    const std::vector<std::string> &) const override {
    return false;
  }
  [[nodiscard]] std::filesystem::path home_dir() const override { return {}; }
  [[nodiscard]] std::filesystem::path temp_dir() const override { return {}; }

  [[nodiscard]] bool
  register_protocol_handler(engine::ProtocolHandler,
                            const std::filesystem::path &) const override {
    return false;
  }
  [[nodiscard]] bool
  unregister_protocol_handler(engine::ProtocolHandler) const override {
    return false;
  }
  [[nodiscard]] bool
  is_protocol_handler_registered(engine::ProtocolHandler) const override {
    return false;
  }
  [[nodiscard]] std::string
  current_protocol_handler(engine::ProtocolHandler) const override {
    return {};
  }
};

}  // namespace gmm_test
