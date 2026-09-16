#include "engine/modpack/ini_edits.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using engine::modpack::AppliedEdit;
using engine::modpack::apply_ini_edits;
using engine::modpack::applied_state_from_json;
using engine::modpack::applied_state_to_json;
using engine::modpack::IniEdit;
using engine::modpack::IniEditFile;
using engine::modpack::load_ini_dir;
using engine::modpack::MergedTarget;
using engine::modpack::merge_ini_edits;
using engine::modpack::parse_ini_edit_file;
using engine::modpack::retract_ini_edits;

static std::string write_tmp_ini_dir() {
    static int n = 0;
    fs::path dir =
        fs::temp_directory_path() / ("gmm_ini_test_" + std::to_string(++n));
    fs::create_directories(dir);
    return dir.string();
}

static void write_file(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

static MergedTarget single_target(const std::string& target,
                                  std::vector<IniEdit> edits) {
    return MergedTarget{target, std::move(edits), {}};
}

TEST_CASE("parse valid edit file with pack-author and mod edits", "[ini]") {
    const std::string json = R"({
        "targetFile": "Skyrim.ini",
        "edits": [
            {"section": "Display", "key": "iSize W",
             "value": "2560", "sourceModId": null},
            {"section": "General", "key": "sLanguage",
             "value": "ENGLISH", "sourceModId": "SkyUI_SE"}
        ]
    })";
    std::string err;
    auto file = parse_ini_edit_file(json, &err);
    REQUIRE(file.has_value());
    CHECK(file->target_file == "Skyrim.ini");
    REQUIRE(file->edits.size() == 2);
    CHECK(!file->edits[0].source_mod_id.has_value());
    REQUIRE(file->edits[1].source_mod_id.has_value());
    CHECK(*file->edits[1].source_mod_id == "SkyUI_SE");
}

TEST_CASE("parse rejects invalid documents", "[ini]") {
    std::string err;
    CHECK(!parse_ini_edit_file("not json", &err));
    CHECK(!err.empty());
    CHECK(!parse_ini_edit_file(R"({"edits": []})", &err));  // no targetFile
    CHECK(!parse_ini_edit_file(R"({"targetFile": "a.ini"})", &err));  // no edits
    CHECK(!parse_ini_edit_file(
        R"({"targetFile": "a.ini", "edits": [{"section": "S", "value": "1",
             "sourceModId": null}]})",
        &err));  // missing key
    CHECK(!parse_ini_edit_file(
        R"({"targetFile": "a.ini", "edits": [{"section": "S", "key": "  ",
             "value": "1", "sourceModId": null}]})",
        &err));  // empty key
    CHECK(!parse_ini_edit_file(
        R"({"targetFile": "a.ini", "edits": [{"section": "S", "key": "k",
             "value": "1", "sourceModId": 42}]})",
        &err));  // bad sourceModId type
    CHECK(!parse_ini_edit_file("[]", &err));  // root not object
}

TEST_CASE("merge groups case-insensitive targets", "[ini]") {
    IniEditFile a{"Skyrim.ini",
                  {{"Display", "iSize W", "2560", std::nullopt}}};
    IniEditFile b{"skyrim.INI",
                  {{"General", "sLanguage", "ENGLISH", std::string("M")}}};
    auto merged = merge_ini_edits({a, b});
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].target_file == "Skyrim.ini");  // first-seen casing
    CHECK(merged[0].edits.size() == 2);
    CHECK(merged[0].conflicts.empty());
}

TEST_CASE("merge: pack-author beats mod-attributed, last wins ties",
          "[ini]") {
    IniEditFile a{"x.ini",
                  {{"S", "k", "mod-val", std::string("SomeMod")},
                   {"S", "j", "first", std::string("A")},
                   {"S", "j", "second", std::string("B")}}};
    IniEditFile b{"x.ini", {{{"S", "k", "author-val", std::nullopt}}}};
    // Note: b's edit arrives after a's; author still wins over mod.
    auto merged = merge_ini_edits({a, b});
    REQUIRE(merged.size() == 1);
    REQUIRE(merged[0].edits.size() == 2);
    // k: author wins
    CHECK(merged[0].edits[0].value == "author-val");
    // j: last mod edit wins
    CHECK(merged[0].edits[1].value == "second");
    // Conflicts follow first-conflict order: j clashed within file a, k when
    // file b arrived.
    REQUIRE(merged[0].conflicts.size() == 2);
    CHECK(merged[0].conflicts[0].key == "j");
    CHECK(merged[0].conflicts[0].winner.value == "second");
    CHECK(merged[0].conflicts[1].key == "k");
    CHECK(merged[0].conflicts[1].winner.value == "author-val");
    REQUIRE(merged[0].conflicts[1].overridden.size() == 1);
    CHECK(*merged[0].conflicts[1].overridden[0].source_mod_id == "SomeMod");
}

TEST_CASE("merge: identical duplicates are not conflicts", "[ini]") {
    IniEditFile a{"x.ini", {{{"S", "k", "v", std::nullopt}}}};
    IniEditFile b{"X.INI", {{{"s", "K", "v", std::nullopt}}}};
    auto merged = merge_ini_edits({a, b});
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].edits.size() == 1);
    CHECK(merged[0].conflicts.empty());
}

TEST_CASE("apply updates value preserving layout and comments", "[ini]") {
    const std::string ini =
        "; top comment\n"
        "\n"
        "[Display]\n"
        "iSize W=1920 ; screen width\n"
        "bFull Screen=1\n"
        "\n"
        "[General]\n"
        "sLanguage=FRENCH\n";
    auto merged = single_target(
        "Skyrim.ini",
        {{{"display", "isize w", "2560", std::nullopt}}});  // CI match
    auto out = apply_ini_edits(ini, merged);
    CHECK(out.user_modified.empty());
    REQUIRE(out.applied.size() == 1);
    CHECK(out.applied[0].had_prior);
    CHECK(out.applied[0].prior_value == "1920");
    CHECK(out.applied[0].applied_value == "2560");
    // Layout preserved: comment kept, key prefix kept, others untouched.
    CHECK(out.text.find("; top comment\n") == 0);
    CHECK(out.text.find("iSize W=2560 ; screen width\n") != std::string::npos);
    CHECK(out.text.find("bFull Screen=1\n") != std::string::npos);
    CHECK(out.text.find("sLanguage=FRENCH\n") != std::string::npos);
}

TEST_CASE("apply appends missing key and section", "[ini]") {
    const std::string ini = "[Display]\niSize W=1920\n";
    auto merged = single_target(
        "x.ini", {{{"Display", "bBorderless", "1", std::nullopt},
                   {"Archive", "sResourceArchiveList", "a.ba2", std::nullopt}}});
    auto out = apply_ini_edits(ini, merged);
    CHECK(out.text.find("bBorderless=1\n") != std::string::npos);
    CHECK(out.text.find("[Archive]\n") != std::string::npos);
    CHECK(out.text.find("sResourceArchiveList=a.ba2\n") != std::string::npos);
    // New key sits inside [Display], before the new section.
    CHECK(out.text.find("bBorderless=1\n") < out.text.find("[Archive]"));
    CHECK(out.applied.size() == 2);
    CHECK(!out.applied[0].had_prior);
}

TEST_CASE("apply flags user-modified keys instead of overwriting", "[ini]") {
    const std::string ini = "[Display]\niSize W=1920\n";
    auto merged =
        single_target("x.ini", {{{"Display", "iSize W", "2560", std::nullopt}}});
    auto first = apply_ini_edits(ini, merged);
    // User changes the value after us.
    std::string changed = first.text;
    const size_t pos = changed.find("2560");
    REQUIRE(pos != std::string::npos);
    changed.replace(pos, 4, "1280");
    // Pack update wants a new value: flagged, not overwritten.
    auto merged2 =
        single_target("x.ini", {{{"Display", "iSize W", "3440", std::nullopt}}});
    auto second = apply_ini_edits(changed, merged2, first.applied);
    REQUIRE(second.user_modified.size() == 1);
    CHECK(second.user_modified[0].expected == "2560");
    CHECK(second.user_modified[0].actual == "1280");
    CHECK(second.text.find("iSize W=1280\n") != std::string::npos);
    // User independently adopting the new value is not a conflict.
    std::string adopted = first.text;
    adopted.replace(adopted.find("2560"), 4, "3440");
    auto clean = apply_ini_edits(adopted, merged2, first.applied);
    CHECK(clean.user_modified.empty());
    CHECK(clean.text.find("iSize W=3440\n") != std::string::npos);
}

TEST_CASE("apply keeps original prior across re-applies", "[ini]") {
    const std::string ini = "[S]\nk=orig\n";
    auto m1 = single_target("x.ini", {{{"S", "k", "v1", std::nullopt}}});
    auto m2 = single_target("x.ini", {{{"S", "k", "v2", std::nullopt}}});
    auto first = apply_ini_edits(ini, m1);
    auto second = apply_ini_edits(first.text, m2, first.applied);
    REQUIRE(second.applied.size() == 1);
    CHECK(second.applied[0].prior_value == "orig");
    CHECK(second.applied[0].had_prior);
    // Retract restores the true original, not the intermediate.
    auto ret = retract_ini_edits(second.text, second.applied, std::nullopt);
    CHECK(ret.text.find("k=orig\n") != std::string::npos);
}

TEST_CASE("retract restores prior value and removes added keys", "[ini]") {
    const std::string ini = "[S]\nk=orig\n";
    auto merged = single_target(
        "x.ini", {{{"S", "k", "v1", std::string("ModA")},
                   {"S", "added", "yes", std::string("ModA")},
                   {"S", "other", "kept", std::string("ModB")}}});
    auto applied = apply_ini_edits(ini, merged);
    auto ret = retract_ini_edits(applied.text, applied.applied,
                                 std::string("ModA"));
    CHECK(ret.text.find("k=orig\n") != std::string::npos);
    CHECK(ret.text.find("added") == std::string::npos);
    CHECK(ret.text.find("other=kept\n") != std::string::npos);
    // Only ModB's record remains.
    REQUIRE(ret.applied.size() == 1);
    CHECK(*ret.applied[0].source_mod_id == "ModB");
}

TEST_CASE("retract leaves user-changed values and reports them", "[ini]") {
    const std::string ini = "[S]\nk=orig\n";
    auto merged = single_target("x.ini", {{{"S", "k", "v1", std::string("M")}}});
    auto applied = apply_ini_edits(ini, merged);
    std::string changed = applied.text;
    changed.replace(changed.find("v1"), 2, "zz");
    auto ret =
        retract_ini_edits(changed, applied.applied, std::string("M"));
    CHECK(ret.text.find("k=zz\n") != std::string::npos);
    REQUIRE(ret.skipped.size() == 1);
    CHECK(ret.skipped[0].actual == "zz");
    CHECK(ret.applied.empty());
}

TEST_CASE("retract pack-author edits with nullopt source", "[ini]") {
    const std::string ini = "[S]\nk=orig\n";
    auto merged = single_target(
        "x.ini", {{{"S", "k", "v1", std::nullopt},
                   {"S", "m", "v2", std::string("M")}}});
    auto applied = apply_ini_edits(ini, merged);
    auto ret = retract_ini_edits(applied.text, applied.applied, std::nullopt);
    CHECK(ret.text.find("k=orig\n") != std::string::npos);
    CHECK(ret.text.find("m=v2\n") != std::string::npos);
}

TEST_CASE("applied state JSON round-trips", "[ini]") {
    std::vector<AppliedEdit> state = {
        {"Skyrim.ini", "Display", "iSize W", true, "1920", "2560",
         std::nullopt},
        {"Skyrim.ini", "General", "sLanguage", false, "", "ENGLISH",
         std::string("SkyUI_SE")},
    };
    const std::string json = applied_state_to_json(state);
    std::string err;
    auto back = applied_state_from_json(json, &err);
    REQUIRE(back.size() == 2);
    CHECK(back[0].target_file == "Skyrim.ini");
    CHECK(back[0].prior_value == "1920");
    CHECK(!back[0].source_mod_id.has_value());
    CHECK(*back[1].source_mod_id == "SkyUI_SE");
    CHECK(!back[1].had_prior);
    CHECK(applied_state_from_json("bogus", &err).empty());
    CHECK(!err.empty());
}

TEST_CASE("load_ini_dir reads files and reports bad ones", "[ini]") {
    const fs::path dir = write_tmp_ini_dir();
    write_file(dir / "b.json", R"({"targetFile": "B.ini", "edits": []})");
    write_file(dir / "a.json",
               R"({"targetFile": "A.ini", "edits": [
                    {"section": "S", "key": "k", "value": "v",
                     "sourceModId": null}]})");
    write_file(dir / "bad.json", "definitely not json");
    write_file(dir / "notes.txt", "ignored");
    auto loaded = load_ini_dir(dir);
    REQUIRE(loaded.files.size() == 2);
    CHECK(loaded.files[0].target_file == "A.ini");  // filename order
    CHECK(loaded.files[1].target_file == "B.ini");
    REQUIRE(loaded.errors.size() == 1);
    CHECK(loaded.errors[0].find("bad.json") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("load_ini_dir missing dir is empty success", "[ini]") {
    auto loaded = load_ini_dir("/nonexistent/gmm_ini_dir_xyz");
    CHECK(loaded.files.empty());
    CHECK(loaded.errors.empty());
}

TEST_CASE("CRLF documents keep CRLF", "[ini]") {
    const std::string ini = "[S]\r\nk=orig\r\n";
    auto merged = single_target("x.ini", {{{"S", "k", "new", std::nullopt}}});
    auto out = apply_ini_edits(ini, merged);
    CHECK(out.text.find("k=new\r\n") != std::string::npos);
}
