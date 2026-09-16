#pragma once

// Instance router - decides where a pack install goes.
//
// When installing a pack the engine routes purely on game identity (no UI,
// no filesystem here - the wizard owns the dialogs and the instance
// creation):
//   - no active instance, or the pack's game differs from the active
//     instance's game -> CreateNewInstance (a mismatch never appends; the
//     decision carries the validator's error so the UI can explain why).
//   - pack's game matches the active instance -> PromptAppendOrNew, and the
//     wizard asks the user whether to append to the current instance or
//     create a new one.
//
// The append itself is Workspace-pe40's job; plan_append() is the declared
// seam it will implement (currently reports "not implemented").
//
// Engine layer - Qt-free (only <string>).

#include <string>

namespace engine::Install
{

// Where a pack install goes next.
enum class InstallRoute
{
  // Ask the user: append to the active instance or create a new one.
  PromptAppendOrNew,
  // Run the standard instance-creation wizard, then hand off to the
  // modpack install wizard.
  CreateNewInstance,
};

// Routing outcome: the route plus a human-readable note. notice is empty
// when routing needed no explanation (match, or simply no active
// instance); it carries the game-mismatch error when a mismatch forced
// CreateNewInstance, and the "not implemented" text from plan_append().
struct RouteDecision
{
  InstallRoute route = InstallRoute::CreateNewInstance;
  std::string notice;
};

// Routes a pack install. pack_game_id is the manifest's game
// (Pack::PackManifest::game_id); has_active_instance tells whether an
// instance is open, and active_instance_game_id is its game
// (Instance::Info::game_id, ignored when no instance is active).
[[nodiscard]] RouteDecision
route_pack_install(const std::string& pack_game_id, bool has_active_instance,
                   const std::string& active_instance_game_id = {});

// Append seam for Workspace-pe40: reservation for "add this pack's mods to
// the existing instance, respecting existing state". Returns ok == false
// until pe40 implements it; the game-match check is enforced here so the
// future implementation (and any early caller) can never append across
// games. Pure descriptor - no disk or network touched.
struct AppendPlan
{
  bool ok = false;
  std::string error;  // set when !ok
};

[[nodiscard]] AppendPlan plan_append(const std::string& pack_game_id,
                                     const std::string& instance_game_id);

}  // namespace engine::Install
