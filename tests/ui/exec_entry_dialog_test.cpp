// Offscreen GUI regression test for Executables::Entry + the output-to-mod path matcher.
//   - Executables::Entry toJson/fromJson round-trip preserves the output-mod field.
//   - output_mod_for_path(): MO2 getByBinary parity — the launched binary's
//     absolute path is matched against each entry's game-relative path,
//     case-insensitively and symlink-canonicalized, first match wins, and
//     binaries outside the game dir never match (they fall back to Overwrite).
//     This is what routes toolbar-shortcut and Data-tab Execute launches into
//     the configured output mod.
//   - Executables::ContentWidget (the mode-agnostic editor extracted from
//     Executables::Dialog) constructs and exposes the initial entries.
//
// Hermetic: all file trees live under /tmp, no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/widgets/executables_entry.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

namespace {

std::filesystem::path root_dir;

QString qstr(const std::filesystem::path& p) {
    return QString::fromStdString(p.string());
}

}  // namespace

TEST_CASE("exec entry dialog", "[ui]") {
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);

    root_dir = std::filesystem::temp_directory_path() /
               ("gmm_exec_entry_test_" + std::to_string(QCoreApplication::applicationPid()));
    std::error_code ec;
    std::filesystem::remove_all(root_dir, ec);
    std::filesystem::create_directories(root_dir, ec);

    const auto game = root_dir / "game";
    std::filesystem::create_directories(game / "Data/CalienteTools/BodySlide", ec);

    // ---- Executables::Entry JSON round-trip ------------------------------------------
    {
        ui::Executables::Entry e;
        e.path = "Data/CalienteTools/BodySlide/BodySlide.exe";
        e.title = "BodySlide";
        e.arguments = "";
        e.start_in = "";
        e.output_mod = "Bodyslide OUT";
        e.icon_path = "";
        e.environment = {"WINEDEBUG=+file", "GMM_TEST_FLAG=1"};
        auto obj = e.toJson();
        auto back = ui::Executables::Entry::fromJson(obj);
        check(back.path == e.path && back.title == e.title && back.output_mod == "Bodyslide OUT",
              "Executables::Entry JSON round-trip preserves the output mod");
        check(back.output_mod == obj["mod"].toString(),
              "toJson stores the output mod under the 'mod' key");
        check(back.environment == e.environment,
              "Executables::Entry JSON round-trip preserves the environment list");
        check(obj["env"].toArray().size() == 2,
              "toJson stores the environment under the 'env' key");

        // Legacy / env-less entries survive a round-trip with an empty list.
        ui::Executables::Entry bare;
        bare.path = "Data/Tools/Tool.exe";
        check(ui::Executables::Entry::fromJson(bare.toJson()).environment.isEmpty(),
              "entry without env round-trips to an empty environment");
    }

    // ---- output_mod_for_path -------------------------------------------------
    {
        ui::Executables::Entry bodieslide;
        bodieslide.path = "Data/CalienteTools/BodySlide/BodySlide.exe";
        bodieslide.title = "BodySlide";
        bodieslide.output_mod = "Bodyslide OUT";

        ui::Executables::Entry skse;
        skse.path = "skse64_loader.exe";
        skse.title = "SKSE";
        skse.output_mod = "SKSE Scripts";

        ui::Executables::Entry no_output;
        no_output.path = "Data/Tools/Tool.exe";

        const QVector<ui::Executables::Entry> entries = {bodieslide, skse, no_output};

        // Exact merged-relative match (the Data-tab / toolbar-shortcut shape).
        check(ui::Executables::output_mod_for_path(entries, game,
                  qstr(game / "Data/CalienteTools/BodySlide/BodySlide.exe"))
                  == "Bodyslide OUT",
              "exact game-relative path resolves the configured output mod");

        // Case-insensitive match (Skyrim's fs is case-insensitive).
        check(ui::Executables::output_mod_for_path(entries, game,
                  qstr(game / "data/CALIENTETOOLS/BODYSLIDE/BodySlide.exe"))
                  == "Bodyslide OUT",
              "path match is case-insensitive");

        // Case-insensitive on the entry side too.
        check(ui::Executables::output_mod_for_path(entries, game, qstr(game / "SKSE64_Loader.exe"))
                  == "SKSE Scripts",
              "entry paths are matched case-insensitively");

        // A matching binary with no output mod configured -> no redirect.
        check(ui::Executables::output_mod_for_path(entries, game, qstr(game / "Data/Tools/Tool.exe")).isEmpty(),
              "entry without an output mod does not redirect");

        // Unknown binary -> Overwrite fallback.
        check(ui::Executables::output_mod_for_path(entries, game, qstr(game / "Data/SomeOther.exe")).isEmpty(),
              "unmatched binary falls back to Overwrite");

        // Binary outside the game dir never matches.
        check(ui::Executables::output_mod_for_path(entries, game,
                  QStringLiteral("/usr/bin/dolphin")).isEmpty(),
              "path escaping the game dir does not match");

        // First declared match wins (MO2 getByBinary semantics).
        ui::Executables::Entry dup = bodieslide;
        dup.output_mod = "Duplicate Target";
        const QVector<ui::Executables::Entry> with_dup = {bodieslide, dup};
        check(ui::Executables::output_mod_for_path(with_dup, game,
                  qstr(game / "Data/CalienteTools/BodySlide/BodySlide.exe"))
                  == "Bodyslide OUT",
              "first matching entry wins");

        // Empty inputs.
        check(ui::Executables::output_mod_for_path({}, game,
                  qstr(game / "Data/CalienteTools/BodySlide/BodySlide.exe")).isEmpty(),
              "empty entry list never redirects");
        check(ui::Executables::output_mod_for_path(entries, {}, "somewhere.exe").isEmpty(),
              "empty game dir never redirects");
        check(ui::Executables::output_mod_for_path(entries, game, QString()).isEmpty(),
              "empty path never redirects");
    }

    // ---- environment_for_path ------------------------------------------------
    {
        ui::Executables::Entry pandora;
        pandora.path = "Data/Pandora Behaviour Engine+/Pandora Behaviour Engine+.exe";
        pandora.title = "Pandora";
        pandora.environment = {"WINEDEBUG=+file", "GMM_CI_DEBUG=1"};

        ui::Executables::Entry skse;
        skse.path = "skse64_loader.exe";
        skse.title = "SKSE";

        ui::Executables::Entry no_env;
        no_env.path = "Data/Tools/Tool.exe";
        no_env.output_mod = "Tools OUT";

        const QVector<ui::Executables::Entry> entries = {pandora, skse, no_env};

        // Exact merged-relative path resolves the configured env.
        check(ui::Executables::environment_for_path(entries, game,
                  qstr(game / "Data/Pandora Behaviour Engine+/Pandora Behaviour Engine+.exe"))
                  == pandora.environment,
              "exact game-relative path resolves the configured environment");

        // Case-insensitive match (Windows fs semantics).
        check(ui::Executables::environment_for_path(entries, game,
                  qstr(game / "data/pandora behaviour engine+/pandora behaviour engine+.exe"))
                  == pandora.environment,
              "environment path match is case-insensitive");

        // Entry without env vars -> empty list (does NOT inherit from another
        // entry's match, and does not shadow a later configured one).
        check(ui::Executables::environment_for_path(entries, game,
                  qstr(game / "Data/Tools/Tool.exe")).isEmpty(),
              "matching entry without env vars returns an empty list");
        check(ui::Executables::environment_for_path(entries, game,
                  qstr(game / "skse64_loader.exe")).isEmpty(),
              "entry with empty environment does not redirect env");

        // Unmatched binary / outside the game dir / empty inputs.
        check(ui::Executables::environment_for_path(entries, game,
                  qstr(game / "Data/SomeOther.exe")).isEmpty(),
              "unmatched binary resolves no environment");
        check(ui::Executables::environment_for_path(entries, game,
                  QStringLiteral("/usr/bin/dolphin")).isEmpty(),
              "path escaping the game dir resolves no environment");
        check(ui::Executables::environment_for_path({}, game,
                  qstr(game / "Data/Pandora Behaviour Engine+/Pandora Behaviour Engine+.exe")).isEmpty(),
              "empty entry list resolves no environment");
        check(ui::Executables::environment_for_path(entries, {}, "somewhere.exe").isEmpty(),
              "empty game dir resolves no environment");
        check(ui::Executables::environment_for_path(entries, game, QString()).isEmpty(),
              "empty path resolves no environment");
    }

    // ---- Executables::ContentWidget extraction ----------------------------------
    {
        ui::Executables::Entry e;
        e.path = "Data/Tools/Tool.exe";
        e.title = "Tool";
        e.output_mod = "Tools OUT";
        ui::Executables::ContentWidget w(std::filesystem::path{}, {}, {e}, {}, nullptr);
        check(w.entries().size() == 1 && w.entries()[0].path == "Data/Tools/Tool.exe",
              "content widget exposes the initial entries");
        check(w.entries()[0].output_mod == "Tools OUT",
              "content widget preserves entry metadata");
    }

    // ---- symlink-canonicalized game dir --------------------------------------
    {
        // The ~/.steam symlink spelling must still match entries configured
        // against the canonical path.
        ui::Executables::Entry e;
        e.path = "Data/CalienteTools/BodySlide/BodySlide.exe";
        e.output_mod = "Bodyslide OUT";

        const auto link = root_dir / "game_link";
        std::filesystem::create_directory_symlink(game, link, ec);
        if (!ec) {
            check(ui::Executables::output_mod_for_path({e}, link,
                      qstr(game / "Data/CalienteTools/BodySlide/BodySlide.exe"))
                      == "Bodyslide OUT",
                  "symlinked game dir resolves against canonical entries");
            check(ui::Executables::output_mod_for_path({e}, game,
                      qstr(link / "Data/CalienteTools/BodySlide/BodySlide.exe"))
                      == "Bodyslide OUT",
                  "canonical game dir resolves against symlinked path");
        }
    }

    std::filesystem::remove_all(root_dir, ec);
}
