#pragma once

#include <filesystem>

namespace Deploy {

class Interface {
public:
  virtual ~Interface() = default;
  virtual bool deploy(const std::filesystem::path &source,
                      const std::filesystem::path &target) = 0;
  virtual bool remove(const std::filesystem::path &target) = 0;
};

} // namespace Deploy
