#pragma once

#include <toml++/toml.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace engine {

// Parse an instance.toml file with toml++. Legacy files written before the
// toml++ migration stored the `executables` array as JSON-style inline tables
// ({"path":"SkyrimSE.exe"}), which is not valid TOML. When a strict parse
// fails, a bounded repair pass converts JSON-style inline tables to TOML
// syntax and re-parses. Returns nullopt when the file cannot be read or
// parsed.
[[nodiscard]] std::optional<toml::table> parse_instance_toml(
    const std::filesystem::path& path);

// Content overload (tests, in-memory callers).
[[nodiscard]] std::optional<toml::table> parse_instance_toml_content(
    const std::string& content);

// Serialize a table back to TOML text (trailing newline included).
[[nodiscard]] std::string serialize_instance_toml(const toml::table& tbl);

}  // namespace engine