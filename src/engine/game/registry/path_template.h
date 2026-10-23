#pragma once

// v3 Phase P2 - %VAR% path templates (Workspace-k8bc).
//
// Game descriptors (TOML today, plugin hooks tomorrow) declare filesystem
// locations as templates like "%LOCALLOW%/IronGate/Valheim" instead of
// baking in one host's absolute path. The grammar (DX/VAR revision):
//
//   %%                     a literal percent sign
//   %[A-Za-z_][A-Za-z0-9_]*%   a token, case-sensitive
//   a lone % not followed by a token spelling stays literal ("100%" is safe)
//   an unterminated %-identifier (%LOCALLOW with no closing %) is malformed
//
// No recursion, no expressions: a token's resolved value is never rescanned
// for %, so a path containing a literal "%USERPROFILE%" survives intact.
//
// Expansion is target-driven, not host-driven: the target is the OS of the
// executable the instance launches (the same rule the .exe extension check
// uses), so a Windows exe under Proton resolves the Windows column inside
// the prefix. Tokens a target does not define (LOCALLOW on Linux, WINEPREFIX
// on native Windows) expand to a loud error - never to an empty path that
// would silently point at the game dir or $HOME.

#include <filesystem>
#include <string>

namespace engine {

class Platform;

// Which column of the token table to expand. Named for the executable's OS,
// not the host's: a Windows exe under Proton expands PathTarget::Windows.
enum class PathTarget {
  Windows,
  Linux,
  MacOS,
};

// Phase A - validate a template's grammar and token names without a
// Platform. Returns an empty string when the template is well-formed;
// otherwise a human-readable error naming the offending token. Unknown
// token names are rejected here so a descriptor with a typo refuses at
// load time, before any consumer resolves it.
[[nodiscard]] std::string
validate_path_template(const std::string &tmpl);

// Phase B - expand a validated template against a Platform for the given
// target. On success returns the expanded path and leaves error empty. On
// failure returns an empty path and fills error with a message naming the
// token, the target and the reason (undefined for this target, or the
// Platform returned an empty location).
[[nodiscard]] std::filesystem::path
expand_path_template(const std::string &tmpl, const Platform *platform,
                     PathTarget target, uint32_t steam_appid,
                     const std::filesystem::path &game_dir, std::string &error);

}  // namespace engine
