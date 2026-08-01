// Test for engine::ThemeManager: scanning, discovery, tokens, rendering.
#include "engine/theme/theme_manager.h"
#include "engine/log/logger.h"

#include <cassert>
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

int main() {
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
        assert(tm.themes().size() == 3);
        assert(tm.themes()[0].name == "Dark");
        assert(tm.themes()[1].name == "Nord");
        assert(tm.themes()[2].name == "Shared");

        const auto* dark = tm.find_theme("Dark");
        assert(dark && dark->qss_path.filename() == "dark.qss");
        assert(dark && !dark->tokens_path.empty());
        const auto* nord = tm.find_theme("Nord");
        assert(nord && nord->qss_path.filename() == "nord.qss");
        assert(nord && nord->tokens_path.empty());  // no tokens file -> fine
    }

    // Token substitution with prefix-safe ordering.
    {
        engine::ThemeManager tm;
        tm.scan_themes(root / "app" / "themes");
        const auto* dark = tm.find_theme("Dark");
        assert(tm.load_tokens(dark->tokens_path));
        assert(tm.apply_template("a $bg b") == "a #1e1f24 b");

        // Longer keys win over prefix keys: $bg vs $bgAlt.
        tm.set_token("$bg", "#1e1f24");
        tm.set_token("$bgAlt", "#26272e");
        assert(tm.apply_template("$bgAlt|$bg") == "#26272e|#1e1f24");
    }

    // load_tokens clears the previous token map (no stale-token leak).
    {
        engine::ThemeManager tm;
        tm.scan_themes(root / "app" / "themes");
        const auto* dark = tm.find_theme("Dark");
        assert(tm.load_tokens(dark->tokens_path));
        assert(tm.tokens().size() == 2);

        const auto* nord = tm.find_theme("Nord");
        write(root / "app" / "themes" / "Nord" / "tokens.json", "{ \"$fg\": \"#d8dee9\" }");
        assert(tm.load_tokens(nord->tokens_path));
        assert(tm.tokens().size() == 1);
        assert(tm.tokens().count("$fg") == 1);
        assert(tm.tokens().count("$bg") == 0);
    }

    // render_theme writes the fully-substituted QSS.
    {
        engine::ThemeManager tm;
        tm.scan_themes(root / "app" / "themes");
        const auto* dark = tm.find_theme("Dark");
        tm.load_tokens(dark->tokens_path);
        auto out = root / "out" / "rendered.qss";
        fs::create_directories(out.parent_path());
        assert(tm.render_theme(dark->qss_path, out));
        auto content = read_all(out);
        assert(content.find("#1e1f24") != std::string::npos);
        assert(content.find("$bg") == std::string::npos);
    }

    // discover_themes: scans all search dirs, first occurrence wins, sorted.
    {
        engine::ThemeManager tm;
        tm.discover_themes(root / "app");
        const auto* shared = tm.find_theme("Shared");
        assert(shared);
        // app_dir/themes precedes app_dir/../themes.
        assert(shared->qss_path.string().find((root / "app" / "themes").string()) == 0);
        // All three themes present, sorted.
        assert(tm.themes().size() == 3);
        assert(tm.themes()[0].name == "Dark");
    }

    // theme_search_dirs: user dir first, then portable, submodule, share, bundled.
    {
        auto dirs = engine::theme_search_dirs(root / "app");
        assert(dirs.size() == 5);
        assert(dirs[0] == root / "empty_user" / "GameModManager" / "themes");
        assert(dirs[1] == root / "app" / "themes");
        assert(dirs[2] == root / "themes");
        assert(dirs[3] == root / "share" / "GameModManager" / "themes");
        assert(dirs[4] == root / "resources" / "themes");
    }

    std::cout << "theme_test: all assertions passed\n";
    fs::remove_all(root);
    return 0;
}
