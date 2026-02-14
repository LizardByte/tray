#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>

namespace screenshot {

  struct Options {
    std::optional<std::string> region;  // reserved for future ROI support
  };

  void initialize(const std::filesystem::path &rootDir);
  bool is_available(std::string *reason = nullptr);
  bool capture(const std::string &name, const Options &options = {});
  std::filesystem::path output_root();

}  // namespace screenshot
