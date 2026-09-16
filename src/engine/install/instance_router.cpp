#include "engine/install/instance_router.h"

#include "engine/install/game_match_validator.h"

namespace engine::Install
{

RouteDecision route_pack_install(const std::string& pack_game_id,
                                 bool has_active_instance,
                                 const std::string& active_instance_game_id)
{
  if (!has_active_instance)
    return {InstallRoute::CreateNewInstance, {}};
  GameMatchResult match = validate_game_match(pack_game_id, active_instance_game_id);
  if (match.matches)
    return {InstallRoute::PromptAppendOrNew, {}};
  // Mismatched (or undeclared) game: never append - route to a fresh
  // instance and carry the reason so the wizard can explain the redirect.
  return {InstallRoute::CreateNewInstance, match.error};
}

AppendPlan plan_append(const std::string& pack_game_id,
                       const std::string& instance_game_id)
{
  GameMatchResult match = validate_game_match(pack_game_id, instance_game_id);
  if (!match.matches)
    return {false, match.error};
  // Workspace-pe40 owns the append planning (append_install.h); the
  // game-match gate above stays here so no caller can append across games.
  return {true, {}};
}

}  // namespace engine::Install
