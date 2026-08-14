// Regression test for the instance.toml executables-array parser (Issue #34).
// The array-end scan must be bracket-depth-aware: a ']' inside a nested env
// array ("env":["WINEDEBUG=+file"]) or inside a quoted string must not
// truncate the section, or every entry after it is corrupted on the next
// save. The fixed logic lives in extract_toml_array() (free function) so it
// is testable without a full MainWindow; load_executables() consumes it.
#include "ui/controllers/launch_controller.h"

#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}  // namespace

TEST_CASE("executables parser", "[ui]") {
    // Env array with a ']' inside a value, plus a second entry after it: the
    // old naive find(']') scan truncated at the env array's closing bracket
    // and dropped the second entry (and everything after it).
    const std::string content =
        "executables = [\n"
        "  {\"path\":\"skse64_loader.exe\",\"title\":\"SKSE\","
        "\"env\":[\"WINEDEBUG=+file\",\"GMM_X=1\"]},\n"
        "  {\"path\":\"Data/Tools/Tool.exe\",\"title\":\"Tool\"}\n"
        "]\n";
    const auto section = ui::extract_toml_array(content, "executables");
    check(!section.empty(), "executables array extracted");
    check(section.find("skse64_loader.exe") != std::string::npos,
          "first entry present");
    check(section.find("Data/Tools/Tool.exe") != std::string::npos,
          "second entry present after nested env array");
    check(section.find("WINEDEBUG=+file") != std::string::npos,
          "env value inside the nested array preserved");

    // A ']' inside a quoted string must not terminate the array either.
    const std::string quoted =
        "executables = [{\"path\":\"a.exe\",\"args\":\"-x ]\"},"
        "{\"path\":\"b.exe\"}]\n";
    const auto q = ui::extract_toml_array(quoted, "executables");
    check(!q.empty(), "quoted-string array extracted");
    check(q.find("a.exe") != std::string::npos,
          "first quoted-string entry present");
    check(q.find("b.exe") != std::string::npos,
          "bracket inside a quoted string does not truncate");

    // Missing key / unterminated array / empty array -> empty.
    check(ui::extract_toml_array("mod_order = [1,2]\n", "executables").empty(),
          "missing key yields empty");
    check(ui::extract_toml_array(
              "executables = [{\"path\":\"a.exe\"}\n", "executables")
              .empty(),
          "unterminated array yields empty");
    check(ui::extract_toml_array("executables = []\n", "executables").empty(),
          "empty array yields empty section");

    // Unrelated key must not match a prefix (e.g. "executables2").
    check(ui::extract_toml_array("executables2 = [\"a\"]\n", "executables")
              .empty(),
          "prefix-similar key does not match");
}
