#include "engine/core/instance/toml_utils.h"

#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace engine {

namespace {

  // Legacy files (pre-toml++ migration) stored the `executables` array as
  // JSON-style inline tables: {"path":"SkyrimSE.exe","env":[...]}. TOML inline
  // tables require `key = value` (bare keys), so a strict parse rejects them.
  // This bounded repair converts JSON-style inline tables to TOML syntax:
  //
  //   {"path":"SkyrimSE.exe"}  ->  { path = "SkyrimSE.exe" }
  //
  // It only runs when a strict parse fails, and only rewrites the `"key":`
  // separator pattern that is invalid TOML. Quoted strings are copied verbatim
  // (escapes included); a closing quote followed by ':' becomes the TOML key
  // separator. In valid TOML a quoted string is never followed by ':', so the
  // transform is a no-op on well-formed files.
  std::string repair_legacy_json_inline_tables(const std::string &content) {
    std::string out;
    out.reserve(content.size());
    const size_t n = content.size();
    size_t i       = 0;
    while (i < n) {
      const char c = content[i];
      if (c != '"') {
        out += c;
        ++i;
        continue;
      }
      // Copy the quoted string verbatim (handling backslash escapes).
      out += c;
      ++i;
      while (i < n) {
        out += content[i];
        if (content[i] == '\\' && i + 1 < n) {
          out += content[i + 1];
          i += 2;
          continue;
        }
        if (content[i] == '"') {
          ++i;
          break;
        }
        ++i;
      }
      // After a closing quote, skip whitespace; a following ':' is the
      // JSON key separator -> TOML's ` = `.
      size_t j = i;
      while (j < n && (content[j] == ' ' || content[j] == '\t'))
        ++j;
      if (j < n && content[j] == ':') {
        out += " =";
        i = j + 1;
      }
    }
    return out;
  }

}  // namespace

// Path-keyed cache of parsed instance.toml files (Workspace-2b4p): the file
// was parsed up to 3 times per launch (Instance::read_toml,
// InstanceSnapshot::capture, LaunchController::load_executables, ...), so
// repeat reads of an unchanged file are served from memory. Entries are
// validated by mtime+size; writers must call
// invalidate_instance_toml_cache() after a successful write.
namespace {

  struct TomlCacheEntry {
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    toml::table table;
  };

  std::mutex &toml_cache_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  std::unordered_map<std::string, TomlCacheEntry> &toml_cache() {
    static std::unordered_map<std::string, TomlCacheEntry> cache;
    return cache;
  }

  std::string toml_cache_key(const std::filesystem::path &path) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(path, ec);
    if (ec)
      return path.string();
    return abs.lexically_normal().string();
  }

}  // namespace

std::optional<toml::table> parse_instance_toml_content(const std::string &content) {
  try {
    return toml::parse(content);
  } catch (const toml::parse_error &) {
    // Legacy JSON-style inline tables (pre-toml++ migration).
    try {
      return toml::parse(repair_legacy_json_inline_tables(content));
    } catch (const toml::parse_error &) {
      return std::nullopt;
    }
  }
}

std::optional<toml::table> parse_instance_toml(const std::filesystem::path &path) {
  const std::string key = toml_cache_key(path);
  // Stat before reading: if the file changes mid-read the stored stamp stays
  // old and the next call re-parses instead of serving torn content.
  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(path, ec);
  const auto size  = ec ? 0 : std::filesystem::file_size(path, ec);
  if (!ec) {
    std::lock_guard<std::mutex> lock(toml_cache_mutex());
    const auto it = toml_cache().find(key);
    if (it != toml_cache().end() && it->second.mtime == mtime &&
        it->second.size == size) {
      return it->second.table;
    }
  }
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  auto tbl = parse_instance_toml_content(content);
  if (tbl && !ec) {
    std::lock_guard<std::mutex> lock(toml_cache_mutex());
    toml_cache()[key] = TomlCacheEntry{mtime, size, *tbl};
  }
  return tbl;
}

void invalidate_instance_toml_cache(const std::filesystem::path &path) {
  std::lock_guard<std::mutex> lock(toml_cache_mutex());
  toml_cache().erase(toml_cache_key(path));
}

void clear_instance_toml_cache() {
  std::lock_guard<std::mutex> lock(toml_cache_mutex());
  toml_cache().clear();
}

std::string serialize_instance_toml(const toml::table &tbl) {
  std::ostringstream ss;
  ss << toml::toml_formatter(tbl);
  ss << '\n';
  return ss.str();
}

}  // namespace engine