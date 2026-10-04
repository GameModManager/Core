#pragma once

#include <string>

// ---------------------------------------------------------------------------
// What a collection link click does
// ---------------------------------------------------------------------------
// A browser hands us two kinds of nxm:// link. A mod link names one file and
// goes straight into the download queue. A collection link names no file at
// all - it is the other kind, and Source::NxmLink::is_collection has already
// discriminated it, so nothing here has to infer a collection from a missing
// file id.
//
// A collection is a list of mods for exactly one game, so it needs a home
// before anything is fetched. The user chooses which: into the instance they
// are already looking at, into a brand new instance, or not at all. The choice
// is made before the fetch, so "not at all" costs nothing - no network, no
// adapter call, no directory.
//
// Append is the one option with a precondition. An instance is for exactly one
// game, so a collection has no business landing in an instance built for
// another one. When the active instance is already for the collection's game
// the answer is the obvious one and asking would be noise. When it is not, the
// user has to name an instance - never a default, never a guess, never a
// refusal in place of a question.
//
// Split out for the same reason install-method detection and the tray
// decisions are: the branches are the whole feature, and they have to be
// reachable in a test on a machine with no display. Nothing here includes Qt.

namespace ui::nxm {

// The three answers on offer.
enum class CollectionChoice {
  // Add the collection's mods to an instance that already exists.
  Append,
  // Create an instance for the collection and install into that.
  NewInstance,
  // Do nothing at all.
  Cancel,
};

enum class CollectionClickAction {
  // Nothing was fetched, created or written.
  DoNothing,
  // The active instance is for the collection's game, so it takes the mods and
  // there is nothing to ask.
  AppendToActive,
  // The active instance is for another game (or none is loaded), so an
  // instance of the collection's game has to be named before the fetch.
  AppendToInstance,
  // Run the ordinary instance-creation flow, then install into the instance it
  // made.
  CreateInstance,
};

// Everything the decision is made from.
struct CollectionClickFacts {
  CollectionChoice choice = CollectionChoice::Cancel;

  // The collection's game as our own game id - the link's nexus_domain
  // resolved through the managed games. Never empty for a link that got this
  // far, and an empty value can never be Appended to, because nothing can be
  // known to match it.
  std::string collection_game;

  // Instance::info().game_id of the active instance, empty when no instance is
  // loaded.
  std::string active_instance_game;
};

// The decision.
//
// Cancel and NewInstance are unconditional: the user either refused or asked
// for a new instance, and neither depends on what happens to be loaded. Append
// is the only conditional, and its condition is the game - never the file id,
// the revision, or anything else the link happens to carry.
inline CollectionClickAction
decide_collection_click(const CollectionClickFacts &facts) {
  if (facts.choice == CollectionChoice::Cancel)
    return CollectionClickAction::DoNothing;
  if (facts.choice == CollectionChoice::NewInstance)
    return CollectionClickAction::CreateInstance;
  if (!facts.active_instance_game.empty() &&
      facts.active_instance_game == facts.collection_game)
    return CollectionClickAction::AppendToActive;
  return CollectionClickAction::AppendToInstance;
}

}  // namespace ui::nxm
