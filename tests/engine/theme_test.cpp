// Test for engine::ThemeManager: scanning, discovery, tokens, rendering.
#include "engine/platform/theme/theme_manager.h"
#include "engine/core/log/logger.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#endif

namespace fs = std::filesystem;

namespace {

fs::path test_root() {
    return fs::temp_directory_path() / ("gmm_theme_test_" + std::to_string(::getpid()));
}

void write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << content;
}

std::string read_all(const fs::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("theme", "[engine]") {
    // Isolate from any real user theme dirs.
    auto root = test_root();
    fs::remove_all(root);
    fs::create_directories(root / "app" / "themes" / "Dark");
    fs::create_directories(root / "app" / "themes" / "Nord");
    fs::create_directories(root / "app" / "themes" / "Shared");
    fs::create_directories(root / "themes" / "Shared");
    fs::create_directories(root / "empty_user");

    setenv("XDG_DATA_HOME", (root / "empty_user").c_str(), 1);
    setenv("HOME", (root / "empty_user").c_str(), 1);

    write(root / "app" / "themes" / "Dark" / "dark.qss",
        "QWidget { color: $fg; background: $bg; }");
    write(root / "app" / "themes" / "Dark" / "tokens.json",
        "{ \"$fg\": \"#e6e6eb\", \"$bg\": \"#1e1f24\" }");
    write(root / "app" / "themes" / "Dark" / "theme.json",
        "{ \"base_style\": \"Fusion\" }");
    write(root / "app" / "themes" / "Nord" / "nord.qss",
        "QWidget { color: $fg; }");
    write(root / "app" / "themes" / "Shared" / "shared.qss",
        "FROM_APP");
    write(root / "themes" / "Shared" / "shared.qss",
        "FROM_SUBMODULE");

    // scan_themes: finds theme subdirs, names = dir names, sorted.
    {
        engine::ThemeManager tm;
        tm.scan_themes(root / "app" / "themes");
        REQUIRE(tm.themes().size() == 3);
        REQUIRE(tm.themes()[0].name == "Dark");
        REQUIRE(tm.themes()[1].name == "Nord");
        REQUIRE(tm.themes()[2].name == "Shared");

        const auto* dark = tm.find_theme("Dark");
        REQUIRE((dark && dark->qss_path.filename() == "dark.qss"));
        REQUIRE((dark && !dark->tokens_path.empty()));
        REQUIRE((dark && dark->tokens_path.filename() == "tokens.json"));
        // theme.json metadata is parsed into base_style, not treated as tokens.
        REQUIRE((dark && dark->base_style == "Fusion"));
        const auto* nord = tm.find_theme("Nord");
        REQUIRE((nord && nord->qss_path.filename() == "nord.qss"));
        REQUIRE((nord && nord->tokens_path.empty()));  // no tokens file -> fine
        REQUIRE((nord && nord->base_style.empty()));   // no theme.json -> style-agnostic
    }

    // base_style parsing edge cases: theme.json without the key, and a
    // theme.json with a value that is not a valid Qt style name (parsing
    // still succeeds; availability is the UI layer's concern).
    {
        write(root / "app" / "themes" / "Shared" / "theme.json",
            "{ \"author\": \"gmm\" }");
        write(root / "app" / "themes" / "Nord" / "theme.json",
            "{ \"base_style\": \"NoSuchStyle\" }");
        engine::ThemeManager tm;
        tm.scan_themes(root / "app" / "themes");
        const auto* shared = tm.find_theme("Shared");
        REQUIRE((shared && shared->base_style.empty()));
        const auto* nord = tm.find_theme("Nord");
        REQUIRE((nord && nord->base_style == "NoSuchStyle"));
        // tokens_path must not point at theme.json.
        REQUIRE((nord && nord->tokens_path.empty()));
    }

    // Token substitution with prefix-safe ordering.
    {
        engine::ThemeManager tm;
        tm.scan_themes(root / "app" / "themes");
        const auto* dark = tm.find_theme("Dark");
        REQUIRE(tm.load_tokens(dark->tokens_path));
        REQUIRE(tm.apply_template("a $bg b") == "a #1e1f24 b");

        // Longer keys win over prefix keys: $bg vs $bgAlt.
        tm.set_token("$bg", "#1e1f24");
        tm.set_token("$bgAlt", "#26272e");
        REQUIRE(tm.apply_template("$bgAlt|$bg") == "#26272e|#1e1f24");
    }

    // load_tokens clears the previous token map (no stale-token leak).
    {
        engine::ThemeManager tm;
        tm.scan_themes(root / "app" / "themes");
        const auto* dark = tm.find_theme("Dark");
        REQUIRE(tm.load_tokens(dark->tokens_path));
        REQUIRE(tm.tokens().size() == 2);

        const auto* nord = tm.find_theme("Nord");
        write(root / "app" / "themes" / "Nord" / "tokens.json", "{ \"$fg\": \"#d8dee9\" }");
        // tokens_path is only filled at scan time, so re-scan to pick it up.
        tm.scan_themes(root / "app" / "themes");
        nord = tm.find_theme("Nord");
        REQUIRE((nord && !nord->tokens_path.empty()));
        REQUIRE(tm.load_tokens(nord->tokens_path));
        REQUIRE(tm.tokens().size() == 1);
        REQUIRE(tm.tokens().count("$fg") == 1);
        REQUIRE(tm.tokens().count("$bg") == 0);
    }

    // render_theme writes the fully-substituted QSS.
    {
        engine::ThemeManager tm;
        tm.scan_themes(root / "app" / "themes");
        const auto* dark = tm.find_theme("Dark");
        tm.load_tokens(dark->tokens_path);
        auto out = root / "out" / "rendered.qss";
        fs::create_directories(out.parent_path());
        REQUIRE(tm.render_theme(dark->qss_path, out));
        auto content = read_all(out);
        REQUIRE(content.find("#1e1f24") != std::string::npos);
        REQUIRE(content.find("$bg") == std::string::npos);
    }

    // discover_themes: scans all search dirs, first occurrence wins, sorted.
    {
        engine::ThemeManager tm;
        tm.discover_themes(root / "app");
        const auto* shared = tm.find_theme("Shared");
        REQUIRE(shared);
        // app_dir/themes precedes app_dir/../themes.
        REQUIRE(shared->qss_path.string().find((root / "app" / "themes").string()) == 0);
        // All three themes present, sorted.
        REQUIRE(tm.themes().size() == 3);
        REQUIRE(tm.themes()[0].name == "Dark");
    }

    // theme_search_dirs: user dir first, then portable, submodule, share, bundled.
    // The submodule/share/bundled entries traverse via ".." so they only compare
    // equal after normalization.
    {
        auto dirs = engine::theme_search_dirs(root / "app");
        REQUIRE(dirs.size() == 5);
        auto norm = [](const fs::path& p) { return p.lexically_normal(); };
        REQUIRE(norm(dirs[0]) == norm(root / "empty_user" / "GameModManager" / "themes"));
        REQUIRE(norm(dirs[1]) == norm(root / "app" / "themes"));
        REQUIRE(norm(dirs[2]) == norm(root / "themes"));
        REQUIRE(norm(dirs[3]) == norm(root / "share" / "GameModManager" / "themes"));
        REQUIRE(norm(dirs[4]) == norm(root / "resources" / "themes"));
    }

    std::cout << "theme_test: all assertions passed\n";
    fs::remove_all(root);
}
