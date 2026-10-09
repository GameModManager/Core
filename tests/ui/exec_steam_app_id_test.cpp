// Per-executable Steam App ID override (MO2 Executable::steamAppID,
// editexecutablesdialog.cpp:402-404 + spawn.cpp:618-619).
//
// Two things have to hold, and the second is the one that matters:
//   1. the value survives the executables editor -> instance.toml -> read-back
//      round trip, so it is not lost between sessions;
//   2. it REACHES THE LAUNCH. A stored value nothing reads is a dead control,
//      so this drives the real resolver the launch path calls and asserts the
//      effective App ID changes - override set, override cleared, override
//      nonsense, and an unregistered binary.
//
// Hermetic: offscreen, throwaway XDG_CONFIG_HOME, no network.
#include "ui/controllers/launch_controller.h"
#include "ui/widgets/executables_entry.h"

#include <QJsonObject>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

bool contains(const std::vector<std::string> &v, const std::string &needle) {
  for (const auto &s : v) {
    if (s.find(needle) != std::string::npos)
      return true;
  }
  return false;
}
}  // namespace

TEST_CASE("the Steam App ID override reaches the launch decision", "[ui][mo2-parity]") {
  const std::filesystem::path game_dir = "/opt/game";

  QVector<ui::Executables::Entry> entries;
  auto with_app_id = [&](const char *path, const char *app_id) {
    ui::Executables::Entry e;
    e.path         = QString::fromLatin1(path);
    e.steam_app_id = QString::fromLatin1(app_id);
    entries.append(e);
  };
  with_app_id("skse64_loader.exe", "377110");
  with_app_id("NoOverride.exe", "");
  with_app_id("Nonsense.exe", "not-a-number");
  with_app_id("Padded.exe", "  22380  ");

  const QString full = QStringLiteral("/opt/game/skse64_loader.exe");

  SECTION("an override wins over the game's own id") {
    CHECK(ui::Executables::steam_app_id_for_path(entries, game_dir, full, 1234) ==
          377110u);
  }

  SECTION("an entry with no override keeps the game's id") {
    const QString other = QStringLiteral("/opt/game/NoOverride.exe");
    CHECK(ui::Executables::steam_app_id_for_path(entries, game_dir, other, 1234) ==
          1234u);
  }

  SECTION("a game the platform never registered still gets its override") {
    // The whole point of the field: a binary launched under an app id the
    // game itself does not carry.
    CHECK(ui::Executables::steam_app_id_for_path(entries, game_dir, full, 0) ==
          377110u);
  }

  SECTION("a non-numeric override falls back instead of failing the launch") {
    const QString bad = QStringLiteral("/opt/game/Nonsense.exe");
    CHECK(ui::Executables::steam_app_id_for_path(entries, game_dir, bad, 1234) ==
          1234u);
  }

  SECTION("surrounding whitespace does not make an id unreadable") {
    const QString padded = QStringLiteral("/opt/game/Padded.exe");
    CHECK(ui::Executables::steam_app_id_for_path(entries, game_dir, padded, 0) ==
          22380u);
  }

  SECTION("an unregistered binary falls back, it does not invent an id") {
    const QString stranger = QStringLiteral("/opt/game/NotListed.exe");
    CHECK(ui::Executables::steam_app_id_for_path(entries, game_dir, stranger, 1234) ==
          1234u);
    CHECK(ui::Executables::steam_app_id_for_path(entries, game_dir, stranger, 0) == 0u);
  }

  SECTION("path matching is case-insensitive, like every other resolver here") {
    const QString shouty = QStringLiteral("/opt/game/SKSE64_LOADER.EXE");
    CHECK(ui::Executables::steam_app_id_for_path(entries, game_dir, shouty, 0) ==
          377110u);
  }
}

TEST_CASE("the Steam App ID override survives the editor -> TOML -> editor round trip",
          "[ui][mo2-parity]") {
  // The persistence half. Without it the field would work once and silently
  // reset on the next launch, which is indistinguishable from the field not
  // existing at all.
  ui::Executables::Entry e;
  e.path         = QStringLiteral("skse64_loader.exe");
  e.title        = QStringLiteral("SKSE");
  e.steam_app_id = QStringLiteral("377110");

  const QJsonObject json = e.toJson();
  CHECK(json["steam"].toString() == QStringLiteral("377110"));
  const ui::Executables::Entry back = ui::Executables::Entry::fromJson(json);
  CHECK(back.steam_app_id == QStringLiteral("377110"));

  // An entry that never had one keeps the field empty rather than growing a
  // "0", which would resolve to app id 0 and stop the game from finding its
  // prefix.
  const ui::Executables::Entry legacy =
      ui::Executables::Entry::fromLegacyPath(QStringLiteral("bin/tool.exe"));
  CHECK(legacy.steam_app_id.isEmpty());
}

TEST_CASE("an executables array with a Steam App ID parses back out of instance.toml",
          "[ui][mo2-parity]") {
  const std::string content =
      "executables = [\n"
      "  { path = \"skse64_loader.exe\", title = \"SKSE\", steam = \"377110\" },\n"
      "  { path = \"bin/tool.exe\", title = \"Tool\" }\n"
      "]\n";
  const auto entries = ui::extract_executables(content);
  REQUIRE(entries.size() == 2);
  // The round trip through TOML keeps the value on the entry the ExecControls
  // bar builds, so the resolver above can see it after a restart.
  CHECK(contains(entries, "\"steam\":\"377110\""));
  // An entry without the key stays clean rather than picking up a stale value.
  CHECK(!contains(entries, "bin/tool.exe\",\"steam\""));
}