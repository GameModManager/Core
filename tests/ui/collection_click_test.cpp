// What a collection link click does.
//
// The branches here are the whole feature, and two of them would put a
// Fallout 4 collection into a Skyrim instance or quietly do the user's work
// for them, so they are pinned rather than left to a GUI check:
//
//   - Append takes the active instance only when that instance is for the
//     collection's game;
//   - anything else about the active instance means the user has to name an
//     instance - never a silent pick, and never a refusal in place of asking;
//   - Cancel does nothing, whatever happens to be loaded, because the choice is
//     made before the collection is fetched;
//   - New Instance does not depend on what is loaded either.
//
// Qt-free and display-free, so this runs anywhere. Nothing here needs a
// QApplication, a window, or a network.
#include "ui/nxm/collection_click.h"

#include <catch2/catch_test_macros.hpp>

using ui::nxm::CollectionChoice;
using ui::nxm::CollectionClickAction;
using ui::nxm::CollectionClickFacts;
using ui::nxm::decide_collection_click;

namespace {

// A Fallout 4 collection with a Skyrim Special Edition instance loaded.
CollectionClickFacts mismatch() {
  CollectionClickFacts facts;
  facts.choice               = CollectionChoice::Append;
  facts.collection_game      = "Fallout4";
  facts.active_instance_game = "SkyrimSE";
  return facts;
}

}  // namespace

TEST_CASE("Append takes the active instance when the game matches", "[ui][nxm]") {
  CollectionClickFacts facts = mismatch();
  facts.active_instance_game = "Fallout4";

  CHECK(decide_collection_click(facts) == CollectionClickAction::AppendToActive);
}

TEST_CASE("Append across games requires the user to name an instance", "[ui][nxm]") {
  // A Fallout 4 collection, a Skyrim Special Edition instance. The only safe
  // answers are asking or refusing - quietly choosing either instance is the
  // defect this pins.
  CHECK(decide_collection_click(mismatch()) == CollectionClickAction::AppendToInstance);

  SECTION("no instance loaded at all asks the same way") {
    CollectionClickFacts facts = mismatch();
    facts.active_instance_game.clear();
    CHECK(decide_collection_click(facts) == CollectionClickAction::AppendToInstance);
  }

  SECTION("a collection whose game is unknown is never appended to blindly") {
    CollectionClickFacts facts = mismatch();
    facts.collection_game.clear();
    facts.active_instance_game.clear();
    CHECK(decide_collection_click(facts) == CollectionClickAction::AppendToInstance);
  }
}

TEST_CASE("Cancel does nothing, whatever is loaded", "[ui][nxm]") {
  CollectionClickFacts facts;
  facts.choice = CollectionChoice::Cancel;

  // No instance, then an instance of the collection's own game: the answer
  // cannot depend on either, or "not now" would sometimes fetch a collection.
  CHECK(decide_collection_click(facts) == CollectionClickAction::DoNothing);

  facts.collection_game      = "Fallout4";
  facts.active_instance_game = "Fallout4";
  CHECK(decide_collection_click(facts) == CollectionClickAction::DoNothing);
}

TEST_CASE("New Instance does not depend on what is loaded", "[ui][nxm]") {
  CollectionClickFacts facts = mismatch();
  facts.choice               = CollectionChoice::NewInstance;
  CHECK(decide_collection_click(facts) == CollectionClickAction::CreateInstance);

  // Not even a matching active instance changes the answer: the user asked
  // for a new instance, so the mods belong in a new one.
  facts.active_instance_game = "Fallout4";
  CHECK(decide_collection_click(facts) == CollectionClickAction::CreateInstance);
}
