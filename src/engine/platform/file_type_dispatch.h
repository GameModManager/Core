#pragma once

// Platform-abstracted file type dispatch. Given a file path, classify it by
// extension and launch it with the right mechanism for the current OS.
//
// Usage: Platform implementations delegate to FileTypeDispatcher for the
// actual exec. This centralizes file-type awareness (chmod+x for scripts,
// java -jar for .jar, xdg-open for unknown types) instead of duplicating
// it per launch site.
//
// Qt-free: no Qt headers.
//
// The shared classification functions are defined inline in this header so
// both the engine and platform libraries can use them without link-order
// issues.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine
{

// File types the dispatcher recognizes. Classification is extension-based
// (fast) with optional MIME probing for extensionless files.
enum class FileType
{
  NativeExecutable,  // ELF, Mach-O, PE (.exe)
  Script,            // .sh, .bash, .bat, .cmd, .ps1, .zsh
  JavaArchive,       // .jar
  AppImage,          // .AppImage
  SharedLibrary,     // .dll, .so, .dylib (not launchable, recognized)
  Unknown            // anything else
};

// Human-readable name for a FileType (for logs).
inline const char* file_type_name(FileType t)
{
  switch (t) {
  case FileType::NativeExecutable:
    return "native-executable";
  case FileType::Script:
    return "script";
  case FileType::JavaArchive:
    return "java-archive";
  case FileType::AppImage:
    return "appimage";
  case FileType::SharedLibrary:
    return "shared-library";
  case FileType::Unknown:
    return "unknown";
  }
  return "unknown";
}

// Lowercase a string in-place.
inline std::string to_lower_str(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return std::tolower(c);
  });
  return s;
}

// Classify a file by extension (lowercase, dot-stripped). Returns Unknown for
// empty extensions or unrecognized ones. This is a pure extension lookup - no
// filesystem access, no MIME probing. Fast path for the common case.
inline FileType classify_by_extension(const std::filesystem::path& file)
{
  auto ext = to_lower_str(file.extension().string());

  // Strip the leading dot: ".exe" -> "exe"
  if (!ext.empty() && ext[0] == '.')
    ext = ext.substr(1);

  if (ext == "exe" || ext == "elf" || ext == "bin" || ext == "com")
    return FileType::NativeExecutable;

  if (ext == "sh" || ext == "bash" || ext == "zsh" || ext == "bat" || ext == "cmd" ||
      ext == "ps1" || ext == "vbs")
    return FileType::Script;

  if (ext == "jar")
    return FileType::JavaArchive;

  if (ext == "appimage")
    return FileType::AppImage;

  if (ext == "dll" || ext == "so" || ext == "dylib" || ext == "lib")
    return FileType::SharedLibrary;

  if (ext == "scr" || ext == "pif")
    return FileType::NativeExecutable;

  return FileType::Unknown;
}

struct LaunchOptions
{
  std::filesystem::path working_dir;  // empty = inherit parent's cwd
  std::vector<std::string> args;      // extra argv after the executable
  bool background = true;             // true = detach from terminal
};

struct LaunchResult
{
  bool ok     = false;
  int64_t pid = -1;  // child PID, -1 if unknown
};

// Platform-specific file type dispatcher. Each OS provides its own
// implementation. The engine never calls OS-specific APIs directly.
class FileTypeDispatcher
{
public:
  virtual ~FileTypeDispatcher() = default;

  // Classify a file. Default: extension-based. Override to add MIME
  // probing or other heuristics.
  [[nodiscard]] virtual FileType classify(const std::filesystem::path& file) const
  {
    return classify_by_extension(file);
  }

  // Can this file type be launched on the current platform?
  // SharedLibrary is never launchable. Unknown may be launchable via
  // xdg-open / ShellExecute / open, depending on platform.
  [[nodiscard]] virtual bool can_launch(FileType type) const = 0;

  // Launch a file with type-appropriate dispatch.
  // Returns false if the type isn't supported or exec fails.
  [[nodiscard]] virtual LaunchResult
  launch(const std::filesystem::path& file,
         const LaunchOptions& options = {}) const = 0;
};

// Factory: returns the platform-appropriate implementation.
// Linux: LinuxFileTypeDispatcher (fork+exec)
// macOS: MacOSFileTypeDispatcher (fork+exec via open)
// Windows: WindowsFileTypeDispatcher (ShellExecute)
std::unique_ptr<FileTypeDispatcher> create_file_type_dispatcher();

}  // namespace engine
