// Regression test for the instance.toml executables-array parser (Issue #34
// bracket-depth bug, now superseded by toml++ — Issue #5e8). The array is
// parsed with a real TOML library, so a ']' inside a nested env array
// ("env":["WINEDEBUG=+file"]) or inside a quoted string can no longer
// truncate the section. The logic lives in extract_executables() (free
// function) so it is testable without a full MainWindow;
// load_executables() consumes it.
#include "ui/controllers/launch_controller.h"

#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}

bool contains(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v) {
        if (s.find(needle) != std::string::npos) return true;
    }
    return false;
}
}  // namespace

TEST_CASE("executables parser", "[ui]") {
    // Env array with a ']' inside a value, plus a second entry after it: the
    // old naive find(']') scan truncated at the env array's closing bracket
    // and dropped the second entry (and everything after it). toml++ parses
    // the whole array correctly.
    const std::string content =
        "executables = [\n"
        "  { path = \"skse64_loader.exe\", title = \"SKSE\","
        " env = [\"WINEDEBUG=+file\",\"GMM_X=1\"] },\n"
        "  { path = \"Data/Tools/Tool.exe\", title = \"Tool\" }\n"
        "]\n";
    const auto entries = ui::extract_executables(content);
    check(entries.size() == 2, "both entries extracted");
    check(contains(entries, "skse64_loader.exe"),
          "first entry present");
    check(contains(entries, "Data/Tools/Tool.exe"),
          "second entry present after nested env array");
    check(contains(entries, "WINEDEBUG=+file"),
          "env value inside the nested array preserved");

    // A ']' inside a quoted string must not terminate the array either.
    const std::string quoted =
        "executables = [{ path = \"a.exe\", args = \"-x ]\" },"
        "{ path = \"b.exe\" }]\n";
    const auto q = ui::extract_executables(quoted);
    check(q.size() == 2, "quoted-string array extracted");
    check(contains(q, "a.exe"), "first quoted-string entry present");
    check(contains(q, "b.exe"),
          "bracket inside a quoted string does not truncate");

    // Legacy JSON-style inline tables (pre-toml++ migration) are repaired and
    // parsed: {"path":"..."} -> { path = "..." }.
    const std::string legacy =
        "executables = [\n"
        "  {\"path\":\"skse64_loader.exe\",\"title\":\"SKSE\","
        "\"env\":[\"WINEDEBUG=+file\"]},\n"
        "  {\"path\":\"Data/Tools/Tool.exe\",\"title\":\"Tool\"}\n"
        "]\n";
    const auto legacy_entries = ui::extract_executables(legacy);
    check(legacy_entries.size() == 2, "legacy JSON-style entries extracted");
    check(contains(legacy_entries, "skse64_loader.exe"),
          "legacy first entry present");
    check(contains(legacy_entries, "Data/Tools/Tool.exe"),
          "legacy second entry present after nested env array");
    check(contains(legacy_entries, "WINEDEBUG=+file"),
          "legacy env value preserved");

    // Legacy plain-string entries are wrapped in {"path": "..."}.
    const std::string plain = "executables = [\"skse64_loader.exe\"]\n";
    const auto plain_entries = ui::extract_executables(plain);
    check(plain_entries.size() == 1, "plain string entry extracted");
    check(contains(plain_entries, "skse64_loader.exe"),
          "plain string wrapped with path");

    // Missing key / empty array / unparseable -> empty.
    check(ui::extract_executables("mod_order = [1,2]\n").empty(),
          "missing key yields empty");
    check(ui::extract_executables("executables = []\n").empty(),
          "empty array yields empty");
    check(ui::extract_executables("not toml at all [[[").empty(),
          "unparseable content yields empty");

    // Unrelated key must not match a prefix (e.g. "executables2").
    check(ui::extract_executables("executables2 = [\"a\"]\n").empty(),
          "prefix-similar key does not match");
}