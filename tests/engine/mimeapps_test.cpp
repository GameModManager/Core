// Regression test for KDE-aware nxm:// / gmm:// handler registration.
//
// Guards against the bug where `xdg-mime default` only writes the generic
// ~/.config/mimeapps.list, while GIO and KService resolve the default from
// kde-mimeapps.list first under XDG_CURRENT_DESKTOP=KDE -- leaving stale
// competitors in charge of the scheme.
//
// Linux-only.

#include "platform/linux/linux_platform.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// Mirrors the real-world stale state: the generic config already names
// gamemodmanager, but the KDE-specific file still pins amethyst and carries a
// spec-invalid [Added Associations] group (the GIO warning).
static void seed_isolated_home(const fs::path& root) {
    fs::create_directories(root / ".config");
    fs::create_directories(root / ".local/share/applications");

    {
        std::ofstream f(root / ".config" / "mimeapps.list");
        f << "[Added Associations]\n"
          << "text/plain=org.kde.kwrite.desktop;\n"
          << "x-scheme-handler/nxm=gamemodmanager-nxm.desktop;\n"
          << "\n"
          << "[Default Applications]\n"
          << "application/pdf=app.zen_browser.zen.desktop;\n"
          << "x-scheme-handler/nxm=gamemodmanager-nxm.desktop\n"
          << "\n"
          << "[Removed Associations]\n"
          << "inode/directory=org.kde.kate.desktop;\n";
    }
    {
        std::ofstream f(root / ".config" / "kde-mimeapps.list");
        f << "[Default Applications]\n"
          << "\n"
          << "x-scheme-handler/nxm=amethystmodmanager-nxm.desktop\n"
          << "[Added Associations]\n"
          << "x-scheme-handler/nxm=amethystmodmanager-nxm.desktop\n";
    }
    {
        std::ofstream f(root / ".local/share/applications" / "mimeapps.list");
        f << "[Default Applications]\n"
          << "x-scheme-handler/modl=com.fluorine.manager.nxm-handler.desktop;\n"
          << "\n"
          << "x-scheme-handler/nxm=amethystmodmanager-nxm.desktop\n"
          << "[Added Associations]\n"
          << "x-scheme-handler/modl=com.fluorine.manager.nxm-handler.desktop;\n"
          << "x-scheme-handler/nxm=amethystmodmanager-nxm.desktop\n";
    }
}

int main() {
    const std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const fs::path root =
        fs::temp_directory_path() / ("gmm_mimeapps_test_" + std::to_string(::getpid()));
    seed_isolated_home(root);

    ::setenv("HOME", root.c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root / ".config").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / ".local/share").c_str(), 1);
    ::setenv("XDG_CURRENT_DESKTOP", "KDE", 1);

    // 1. Stale KDE state must be detected as NOT registered.
    check(!engine::LinuxPlatform::is_nxm_handler_registered(),
          "detected NOT registered while kde-mimeapps.list pins amethyst");

    // 2. Registration must fix all three mimeapps files.
    check(engine::LinuxPlatform::register_nxm_handler(root / "gamemodmanager"),
          "register_nxm_handler succeeds");
    check(fs::exists(root / ".local/share/applications/gamemodmanager-nxm.desktop"),
          "nxm desktop file written");

    const std::string kde = read_file(root / ".config/kde-mimeapps.list");
    check(kde.find("x-scheme-handler/nxm=gamemodmanager-nxm.desktop") != std::string::npos,
          "kde-mimeapps.list default updated to gamemodmanager");
    check(kde.find("amethystmodmanager-nxm.desktop") == std::string::npos,
          "kde-mimeapps.list no longer pins amethyst");
    check(kde.find("[Added Associations]") == std::string::npos,
          "kde-mimeapps.list invalid [Added Associations] group dropped");

    const std::string generic = read_file(root / ".config/mimeapps.list");
    check(generic.find("x-scheme-handler/nxm=gamemodmanager-nxm.desktop") != std::string::npos,
          "generic mimeapps.list keeps gamemodmanager default");
    check(generic.find("application/pdf=app.zen_browser.zen.desktop;") != std::string::npos,
          "generic mimeapps.list unrelated Default Applications preserved");
    check(generic.find("text/plain=org.kde.kwrite.desktop;") != std::string::npos,
          "generic mimeapps.list unrelated Added Associations preserved");
    check(generic.find("org.kde.kate.desktop") != std::string::npos,
          "generic mimeapps.list Removed Associations preserved");

    const std::string data = read_file(root / ".local/share/applications/mimeapps.list");
    check(data.find("x-scheme-handler/nxm=gamemodmanager-nxm.desktop") != std::string::npos,
          "data mimeapps.list default updated to gamemodmanager");
    check(data.find("amethystmodmanager-nxm.desktop") == std::string::npos,
          "data mimeapps.list no longer pins amethyst");
    check(data.find("com.fluorine.manager.nxm-handler.desktop") != std::string::npos,
          "data mimeapps.list unrelated entries preserved");

    // 3. Detection now sees us as the default.
    check(engine::LinuxPlatform::is_nxm_handler_registered(),
          "is_nxm_handler_registered() true after registration");

    // 4. gmm:// scheme gets the same treatment.
    check(engine::LinuxPlatform::register_gmm_handler(root / "gamemodmanager"),
          "register_gmm_handler succeeds");
    check(engine::LinuxPlatform::is_gmm_handler_registered(),
          "gmm handler registered after registration");
    const std::string kde2 = read_file(root / ".config/kde-mimeapps.list");
    check(kde2.find("x-scheme-handler/gmm=gamemodmanager-gmm.desktop") != std::string::npos,
          "kde-mimeapps.list gmm default set");

    // 5. Non-KDE desktops resolve from the generic file.
    ::setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
    check(engine::LinuxPlatform::is_nxm_handler_registered(),
          "detection works on non-KDE desktops (generic file)");

    // Restore the environment.
    ::unsetenv("XDG_CURRENT_DESKTOP");
    ::unsetenv("XDG_DATA_HOME");
    ::unsetenv("XDG_CONFIG_HOME");
    if (!old_home.empty()) {
        ::setenv("HOME", old_home.c_str(), 1);
    }

    std::error_code cleanup_ec;
    fs::remove_all(root, cleanup_ec);

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "mimeapps_test: all checks passed\n";
    return 0;
}
