// Settings > Workarounds > Network: the custom browser must actually receive
// the URL.
//
// A getter round-trip would prove nothing here, so the case runs a real
// program: a throwaway shell script records the argv it was started with, and
// the case asserts the URL arrived. The script lives in the case's own temp
// root and is removed with it.
#include "ui/settings/settings.h"
#include "ui/widgets/web_link.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char *kUrl = "https://example.invalid/mod/1?tab=files";

fs::path case_root() {
  const fs::path root = fs::temp_directory_path() / "gmm_web_link_cfg";
  fs::remove_all(root);
  fs::create_directories(root / "config");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  return root;
}

// Records every argument, one per line. Run through /bin/sh so the case never
// depends on the executable bit.
fs::path write_recorder(const fs::path &dir, const char *name) {
  const fs::path script = dir / name;
  const fs::path out    = dir / (std::string(name) + ".argv");
  std::ofstream sh(script);
  sh << "#!/bin/sh\n"
     << ": > '" << out.string() << "'\n"
     << "for a in \"$@\"; do echo \"$a\" >> '" << out.string() << "'\n"
     << "done\n";
  sh.close();
  return script;
}

std::vector<std::string> read_argv(const fs::path &file) {
  std::vector<std::string> out;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line))
    out.push_back(line);
  return out;
}

// startDetached hands back no handle, so poll. Waiting only for the FILE is a
// race: the script truncates it with ": >" before writing a line, so "exists"
// can be true with zero content. Wait for the expected line count instead.
// 5 s is a generous upper bound for a fork of /bin/sh on a loaded machine.
std::vector<std::string> wait_for_argv(const fs::path &file, size_t min_lines) {
  std::vector<std::string> out;
  QElapsedTimer timer;
  timer.start();
  do {
    if (fs::exists(file)) {
      out = read_argv(file);
      if (out.size() >= min_lines)
        return out;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  } while (timer.elapsed() < 5000);
  return out;
}

}  // namespace

TEST_CASE("custom browser receives the URL", "[ui][settings]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = case_root();
  int argc            = 1;
  char argv0[]        = "web_link_test";
  char *argv[]        = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &s = Settings::instance();

  SECTION("command shaping") {
    // "%1" is the placeholder, exactly like the MO2 shell URL handler.
    const auto shaped = WebLink::custom_browser_argv("firefox --new-tab %1", kUrl);
    REQUIRE(shaped.size() == 3);
    CHECK(shaped[0] == "firefox");
    CHECK(shaped[1] == "--new-tab");
    CHECK(shaped[2] == kUrl);

    // No placeholder: the URL is appended, so a bare program name works.
    const auto bare = WebLink::custom_browser_argv("firefox", kUrl);
    REQUIRE(bare.size() == 2);
    CHECK(bare[0] == "firefox");
    CHECK(bare[1] == kUrl);

    CHECK(WebLink::custom_browser_argv("   ", kUrl).isEmpty());
  }

  SECTION("a bare command receives the URL as its last argument") {
    const fs::path script = write_recorder(root, "bare.sh");
    s.set_use_custom_browser(true);
    s.set_custom_browser_command(("/bin/sh " + script.string()).c_str());
    CHECK(WebLink::uses_custom_browser());
    CHECK(WebLink::open(QString::fromUtf8(kUrl)));

    // sh runs the script with the URL as its only argument: the script path is
    // sh's own argument, not one the script sees.
    const auto seen = wait_for_argv(root / "bare.sh.argv", 1);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == kUrl);
  }

  SECTION("a %1 command receives the URL where the placeholder is") {
    const fs::path script = write_recorder(root, "ph.sh");
    s.set_use_custom_browser(true);
    // The placeholder is in the MIDDLE, so this distinguishes substitution
    // from appending: appended, the URL would land last, after "--end".
    s.set_custom_browser_command(
        ("/bin/sh " + script.string() + " -- %1 --end").c_str());
    CHECK(WebLink::open(QString::fromUtf8(kUrl)));

    const auto seen = wait_for_argv(root / "ph.sh.argv", 3);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "--");
    CHECK(seen[1] == kUrl);
    CHECK(seen[2] == "--end");
  }

  SECTION("off by default: the desktop handler is used instead") {
    // No custom command, and the setting off: open() must not try to run
    // anything.
    s.set_use_custom_browser(false);
    s.set_custom_browser_command(QString());
    CHECK(!WebLink::uses_custom_browser());
    // A custom command that is configured but switched off is equally inert.
    s.set_custom_browser_command("definitely-not-a-real-program-xyz");
    CHECK(!WebLink::uses_custom_browser());
  }

  fs::remove_all(root);
}
