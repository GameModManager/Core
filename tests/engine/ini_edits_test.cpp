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
using engine::modpack::IniTweak;
using engine::modpack::load_ini_dir;
using engine::modpack::MergedTarget;
using engine::modpack::merge_ini_edits;
using engine::modpack::parse_ini_content;
using engine::modpack::parse_ini_edit_file;
using engine::modpack::retract_ini_edits;
using engine::modpack::retract_ini_tweak;
using engine::modpack::TweakStatus;

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

static IniTweak make_tweak(const std::string& id, const std::string& content,
                           TweakStatus status = TweakStatus::Recommended,
                           bool enabled = true,
                           std::optional<std::string> source = std::nullopt,
                           const std::string& name = "") {
    IniTweak t;
    t.id = id;
    t.name = name.empty() ? id + "-name" : name;
    t.status = status;
    t.enabled = enabled;
    t.source_mod_id = std::move(source);
    t.content = content;
    return t;
}

static IniEditFile make_file(const std::string& target,
                             std::vector<IniTweak> tweaks) {
    return IniEditFile{target, std::move(tweaks)};
}

// Legacy helper: hand-built key-level target without tweak provenance.
static MergedTarget single_target(const std::string& target,
                                  std::vector<IniEdit> edits) {
    return MergedTarget{target, std::move(edits), {}};
}

TEST_CASE("parse valid tweak file with pack-author and mod tweaks", "[ini]") {
    const std::string json = R"({
        "targetFile": "Skyrim.ini",
        "tweaks": [
            {"id": "aniso", "name": "Anisotropy", "status": "required",
             "enabled": true, "sourceModId": null,
             "content": "[Display]\niMaxAnisotropy=16\n"},
            {"id": "skyui-list", "name": "SkyUI list", "status": "recommended",
             "enabled": false, "sourceModId": "SkyUI_SE",
             "content": "[Archive]\nsResourceArchiveList2=a.esp\n"}
        ]
    })";
    std::string err;
    auto file = parse_ini_edit_file(json, &err);
    REQUIRE(file.has_value());
    CHECK(file->target_file == "Skyrim.ini");
    REQUIRE(file->tweaks.size() == 2);
    CHECK(file->tweaks[0].id == "aniso");
    CHECK(file->tweaks[0].status == TweakStatus::Required);
    CHECK(file->tweaks[0].enabled);
    CHECK(!file->tweaks[0].source_mod_id.has_value());
    CHECK(file->tweaks[1].status == TweakStatus::Recommended);
    CHECK(!file->tweaks[1].enabled);
    REQUIRE(file->tweaks[1].source_mod_id.has_value());
    CHECK(*file->tweaks[1].source_mod_id == "SkyUI_SE");
}

TEST_CASE("parse rejects invalid tweak documents", "[ini]") {
    std::string err;
    CHECK(!parse_ini_edit_file("not json", &err));
    CHECK(!err.empty());
    CHECK(!parse_ini_edit_file(R"({"tweaks": []})", &err));  // no targetFile
    CHECK(!parse_ini_edit_file(R"({"targetFile": "a.ini"})", &err));  // no tweaks
    CHECK(!parse_ini_edit_file(  // legacy edits[] shape is gone (hard break)
        R"({"targetFile": "a.ini", "edits": []})", &err));
    CHECK(!parse_ini_edit_file(  // missing id
        R"({"targetFile": "a.ini", "tweaks": [{"name": "n", "status": "required",
             "enabled": true, "sourceModId": null, "content": "[S]\nk=v\n"}]})",
        &err));
    CHECK(!parse_ini_edit_file(  // bad status
        R"({"targetFile": "a.ini", "tweaks": [{"id": "t", "name": "n",
             "status": "sometimes", "enabled": true, "sourceModId": null,
             "content": "[S]\nk=v\n"}]})",
        &err));
    CHECK(!parse_ini_edit_file(  // enabled not boolean
        R"({"targetFile": "a.ini", "tweaks": [{"id": "t", "name": "n",
             "status": "required", "enabled": "yes", "sourceModId": null,
             "content": "[S]\nk=v\n"}]})",
        &err));
    CHECK(!parse_ini_edit_file(  // empty content
        R"({"targetFile": "a.ini", "tweaks": [{"id": "t", "name": "n",
             "status": "required", "enabled": true, "sourceModId": null,
             "content": ""}]})",
        &err));
    CHECK(!parse_ini_edit_file(  // content without section
        R"({"targetFile": "a.ini", "tweaks": [{"id": "t", "name": "n",
             "status": "required", "enabled": true, "sourceModId": null,
             "content": "k=v\n"}]})",
        &err));
    CHECK(!parse_ini_edit_file(  // duplicate id
        R"({"targetFile": "a.ini", "tweaks": [
             {"id": "t", "name": "one", "status": "required",
              "enabled": true, "sourceModId": null, "content": "[S]\nk=1\n"},
             {"id": "t", "name": "two", "status": "required",
              "enabled": true, "sourceModId": null, "content": "[S]\nk=2\n"}]})",
        &err));
    CHECK(!parse_ini_edit_file(  // duplicate name
        R"({"targetFile": "a.ini", "tweaks": [
             {"id": "one", "name": "same", "status": "required",
              "enabled": true, "sourceModId": null, "content": "[S]\nk=1\n"},
             {"id": "two", "name": "same", "status": "required",
              "enabled": true, "sourceModId": null, "content": "[S]\nk=2\n"}]})",
        &err));
    CHECK(!parse_ini_edit_file("[]", &err));  // root not object
}

TEST_CASE("parse_ini_content handles sections, comments, blanks", "[ini]") {
    const std::string content =
        "; top comment\n"
        "# hash comment\n"
        "\n"
        "[Display]\n"
        "iSize W=1920 ; screen width\n"
        "bFull Screen=1\n";
    std::string err;
    std::vector<std::string> warnings;
    auto edits = parse_ini_content(content, &err, &warnings);
    REQUIRE(edits.has_value());
    CHECK(warnings.empty());
    REQUIRE(edits->size() == 2);
    CHECK((*edits)[0].section == "Display");
    CHECK((*edits)[0].key == "iSize W");
    CHECK((*edits)[0].value == "1920");  // trailing comment stripped
    CHECK((*edits)[1].key == "bFull Screen");
    CHECK((*edits)[1].value == "1");
}

TEST_CASE("parse_ini_content duplicate key last-wins with warning", "[ini]") {
    std::string err;
    std::vector<std::string> warnings;
    auto edits =
        parse_ini_content("[S]\nk=first\nk=second\n", &err, &warnings);
    REQUIRE(edits.has_value());
    REQUIRE(edits->size() == 1);
    CHECK((*edits)[0].value == "second");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("duplicate key") != std::string::npos);
}

TEST_CASE("parse_ini_content rejects bad lines", "[ini]") {
    std::string err;
    CHECK(!parse_ini_content("k=v\n", &err));  // key outside any section
    CHECK(!err.empty());
    CHECK(!parse_ini_content("[S]\njust words\n", &err));  // no '='
    CHECK(!err.empty());
    CHECK(!parse_ini_content("[S]\n= v\n", &err));  // empty key
    CHECK(!err.empty());
}

TEST_CASE("merge groups case-insensitive targets", "[ini]") {
    auto a = make_file("Skyrim.ini",
                       {make_tweak("t1", "[Display]\niSize W=2560\n")});
    auto b = make_file("skyrim.INI",
                       {make_tweak("t2", "[General]\nsLanguage=ENGLISH\n",
                                   TweakStatus::Recommended, true,
                                   std::string("M"))});
    auto merged = merge_ini_edits({a, b});
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].target_file == "Skyrim.ini");  // first-seen casing
    CHECK(merged[0].edits.size() == 2);
    CHECK(merged[0].conflicts.empty());
}

TEST_CASE("merge: pack-author beats mod-attributed, last wins ties",
          "[ini]") {
    auto a = make_file("x.ini",
                       {make_tweak("mod-k", "[S]\nk=mod-val\n",
                                   TweakStatus::Recommended, true,
                                   std::string("SomeMod")),
                        make_tweak("j-first", "[S]\nj=first\n",
                                   TweakStatus::Recommended, true,
                                   std::string("A")),
                        make_tweak("j-second", "[S]\nj=second\n",
                                   TweakStatus::Recommended, true,
                                   std::string("B"))});
    auto b = make_file(
        "x.ini", {make_tweak("author-k", "[S]\nk=author-val\n",
                             TweakStatus::Required, true, std::nullopt)});
    auto merged = merge_ini_edits({a, b});
    REQUIRE(merged.size() == 1);
    REQUIRE(merged[0].edits.size() == 2);
    CHECK(merged[0].edits[0].value == "author-val");  // k: author wins
    CHECK(merged[0].edits[1].value == "second");  // j: last mod tweak wins
    REQUIRE(merged[0].conflicts.size() == 2);
    CHECK(merged[0].conflicts[0].key == "j");
    CHECK(merged[0].conflicts[0].winner.value == "second");
    CHECK(merged[0].conflicts[0].winner_tweak_id == std::string("j-second"));
    CHECK(merged[0].conflicts[1].key == "k");
    CHECK(merged[0].conflicts[1].winner.value == "author-val");
    CHECK(merged[0].conflicts[1].winner_tweak_id == std::string("author-k"));
    REQUIRE(merged[0].conflicts[1].overridden.size() == 1);
    CHECK(*merged[0].conflicts[1].overridden[0].source_mod_id == "SomeMod");
    REQUIRE(merged[0].conflicts[1].overridden_tweak_id.size() == 1);
    CHECK(merged[0].conflicts[1].overridden_tweak_id[0] == "mod-k");
}

TEST_CASE("merge: identical duplicates are not conflicts", "[ini]") {
    auto a = make_file("x.ini", {make_tweak("t1", "[S]\nk=v\n")});
    auto b = make_file("X.INI", {make_tweak("t2", "[s]\nK=v\n")});
    auto merged = merge_ini_edits({a, b});
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].edits.size() == 1);
    CHECK(merged[0].conflicts.empty());
}

TEST_CASE("merge: disabled tweaks are filtered", "[ini]") {
    auto f = make_file("x.ini",
                       {make_tweak("on", "[S]\nk=kept\n"),
                        make_tweak("off", "[S]\nj=dropped\n",
                                   TweakStatus::Recommended, false)});
    auto merged = merge_ini_edits({f});
    REQUIRE(merged.size() == 1);
    REQUIRE(merged[0].edits.size() == 1);
    CHECK(merged[0].edits[0].key == "k");
    CHECK(merged[0].edits[0].tweak_id == std::string("on"));
}

TEST_CASE("merge carries tweak provenance into edits", "[ini]") {
    auto f = make_file(
        "x.ini", {make_tweak("t1", "[S]\nk=v\n", TweakStatus::Required, true,
                             std::string("M"))});
    auto merged = merge_ini_edits({f});
    REQUIRE(merged.size() == 1);
    REQUIRE(merged[0].edits.size() == 1);
    CHECK(merged[0].edits[0].tweak_id == std::string("t1"));
    CHECK(merged[0].edits[0].source_mod_id == std::string("M"));
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
    auto f = make_file(
        "Skyrim.ini",
        {make_tweak("t1", "[display]\nisize w=2560\n")});  // CI match
    auto merged = merge_ini_edits({f});
    REQUIRE(merged.size() == 1);
    auto out = apply_ini_edits(ini, merged[0]);
    CHECK(out.user_modified.empty());
    REQUIRE(out.applied.size() == 1);
    CHECK(out.applied[0].had_prior);
    CHECK(out.applied[0].prior_value == "1920");
    CHECK(out.applied[0].applied_value == "2560");
    CHECK(out.applied[0].tweak_id == std::string("t1"));
    // Layout preserved: comment kept, key prefix kept, others untouched.
    CHECK(out.text.find("; top comment\n") == 0);
    CHECK(out.text.find("iSize W=2560 ; screen width\n") != std::string::npos);
    CHECK(out.text.find("bFull Screen=1\n") != std::string::npos);
    CHECK(out.text.find("sLanguage=FRENCH\n") != std::string::npos);
}

TEST_CASE("apply appends missing key and section", "[ini]") {
    const std::string ini = "[Display]\niSize W=1920\n";
    auto f = make_file("x.ini", {make_tweak("t1",
                                            "[Display]\nbBorderless=1\n"
                                            "[Archive]\nsResourceArchiveList=a.ba2\n")});
    auto merged = merge_ini_edits({f});
    REQUIRE(merged.size() == 1);
    auto out = apply_ini_edits(ini, merged[0]);
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
        single_target("x.ini", {{{"Display", "iSize W", "2560", std::nullopt,
                                 std::nullopt}}});
    auto first = apply_ini_edits(ini, merged);
    // User changes the value after us.
    std::string changed = first.text;
    const size_t pos = changed.find("2560");
    REQUIRE(pos != std::string::npos);
    changed.replace(pos, 4, "1280");
    // Pack update wants a new value: flagged, not overwritten.
    auto merged2 =
        single_target("x.ini", {{{"Display", "iSize W", "3440", std::nullopt,
                                 std::nullopt}}});
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
    auto m1 = single_target("x.ini", {{{"S", "k", "v1", std::nullopt,
                                       std::string("t1")}}});
    auto m2 = single_target("x.ini", {{{"S", "k", "v2", std::nullopt,
                                       std::string("t1")}}});
    auto first = apply_ini_edits(ini, m1);
    auto second = apply_ini_edits(first.text, m2, first.applied);
    REQUIRE(second.applied.size() == 1);
    CHECK(second.applied[0].prior_value == "orig");
    CHECK(second.applied[0].had_prior);
    CHECK(second.applied[0].tweak_id == std::string("t1"));
    // Retract restores the true original, not the intermediate.
    auto ret = retract_ini_edits(second.text, second.applied, std::nullopt);
    CHECK(ret.text.find("k=orig\n") != std::string::npos);
}

TEST_CASE("retract restores prior value and removes added keys", "[ini]") {
    const std::string ini = "[S]\nk=orig\n";
    auto merged = single_target(
        "x.ini", {{{"S", "k", "v1", std::string("ModA"), std::string("t1")},
                   {"S", "added", "yes", std::string("ModA"),
                    std::string("t1")},
                   {"S", "other", "kept", std::string("ModB"),
                    std::string("t2")}}});
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
    auto merged = single_target(
        "x.ini", {{{"S", "k", "v1", std::string("M"), std::string("t1")}}});
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
        "x.ini", {{{"S", "k", "v1", std::nullopt, std::string("t1")},
                   {"S", "m", "v2", std::string("M"), std::string("t2")}}});
    auto applied = apply_ini_edits(ini, merged);
    auto ret = retract_ini_edits(applied.text, applied.applied, std::nullopt);
    CHECK(ret.text.find("k=orig\n") != std::string::npos);
    CHECK(ret.text.find("m=v2\n") != std::string::npos);
}

TEST_CASE("retract_ini_tweak removes only that tweak's edits", "[ini]") {
    const std::string ini = "[S]\nk=orig\nj=keep\n";
    auto f = make_file("x.ini",
                       {make_tweak("t1", "[S]\nk=v1\n"),
                        make_tweak("t2", "[S]\nj=v2\n")});
    auto merged = merge_ini_edits({f});
    REQUIRE(merged.size() == 1);
    auto applied = apply_ini_edits(ini, merged[0]);
    REQUIRE(applied.applied.size() == 2);
    auto ret = retract_ini_tweak(applied.text, applied.applied, "t1");
    CHECK(ret.text.find("k=orig\n") != std::string::npos);
    CHECK(ret.text.find("j=v2\n") != std::string::npos);
    REQUIRE(ret.applied.size() == 1);
    CHECK(ret.applied[0].tweak_id == std::string("t2"));
}

TEST_CASE("retract_ini_tweak leaves user-changed values and reports them",
          "[ini]") {
    const std::string ini = "[S]\nk=orig\n";
    auto f = make_file("x.ini", {make_tweak("t1", "[S]\nk=v1\n")});
    auto merged = merge_ini_edits({f});
    auto applied = apply_ini_edits(ini, merged[0]);
    std::string changed = applied.text;
    changed.replace(changed.find("v1"), 2, "zz");
    auto ret = retract_ini_tweak(changed, applied.applied, "t1");
    CHECK(ret.text.find("k=zz\n") != std::string::npos);
    REQUIRE(ret.skipped.size() == 1);
    CHECK(ret.skipped[0].actual == "zz");
    CHECK(ret.applied.empty());
}

TEST_CASE("retract_ini_tweak ignores entries without provenance", "[ini]") {
    const std::string ini = "[S]\nk=orig\n";
    // Legacy hand-built edit with no tweak_id: tweak retract must not touch.
    auto merged =
        single_target("x.ini", {{{"S", "k", "v1", std::nullopt, std::nullopt}}});
    auto applied = apply_ini_edits(ini, merged);
    auto ret = retract_ini_tweak(applied.text, applied.applied, "t1");
    CHECK(ret.text.find("k=v1\n") != std::string::npos);
    REQUIRE(ret.applied.size() == 1);
}

TEST_CASE("toggle off/on round-trips through retract and re-apply", "[ini]") {
    const std::string ini = "[S]\nk=orig\n";
    auto on = make_file("x.ini", {make_tweak("t1", "[S]\nk=v1\n")});
    auto off = make_file("x.ini", {make_tweak(
                                       "t1", "[S]\nk=v1\n",
                                       TweakStatus::Recommended, false)});
    // Enable: apply.
    auto applied = apply_ini_edits(ini, merge_ini_edits({on})[0]);
    REQUIRE(applied.text.find("k=v1\n") != std::string::npos);
    // Disable: retract by tweak; the key is gone, the record is dropped.
    auto ret = retract_ini_tweak(applied.text, applied.applied, "t1");
    CHECK(ret.text.find("k=orig\n") != std::string::npos);
    CHECK(ret.applied.empty());
    // Re-enable: re-merge (now non-empty) and apply on the retracted text.
    auto remerged = merge_ini_edits({on});
    REQUIRE(remerged.size() == 1);
    auto reapplied = apply_ini_edits(ret.text, remerged[0], ret.applied);
    CHECK(reapplied.text.find("k=v1\n") != std::string::npos);
    REQUIRE(reapplied.applied.size() == 1);
    CHECK(reapplied.applied[0].prior_value == "orig");
}

TEST_CASE("applied state JSON round-trips with tweak ids", "[ini]") {
    std::vector<AppliedEdit> state = {
        {"Skyrim.ini", "Display", "iSize W", true, "1920", "2560",
         std::nullopt, std::string("aniso")},
        {"Skyrim.ini", "General", "sLanguage", false, "", "ENGLISH",
         std::string("SkyUI_SE"), std::string("skyui-list")},
    };
    const std::string json = applied_state_to_json(state);
    std::string err;
    auto back = applied_state_from_json(json, &err);
    REQUIRE(back.size() == 2);
    CHECK(back[0].target_file == "Skyrim.ini");
    CHECK(back[0].prior_value == "1920");
    CHECK(!back[0].source_mod_id.has_value());
    CHECK(back[0].tweak_id == std::string("aniso"));
    CHECK(*back[1].source_mod_id == "SkyUI_SE");
    CHECK(back[1].tweak_id == std::string("skyui-list"));
    CHECK(!back[1].had_prior);
    CHECK(applied_state_from_json("bogus", &err).empty());
    CHECK(!err.empty());
}

TEST_CASE("applied state JSON loads pre-tweak files without tweakId",
          "[ini]") {
    const std::string json = R"([{"targetFile": "x.ini", "section": "S",
        "key": "k", "hadPrior": true, "priorValue": "o",
        "appliedValue": "v", "sourceModId": null}])";
    std::string err;
    auto back = applied_state_from_json(json, &err);
    REQUIRE(back.size() == 1);
    CHECK(!back[0].tweak_id.has_value());
    CHECK(back[0].applied_value == "v");
}

TEST_CASE("load_ini_dir reads files and reports bad ones", "[ini]") {
    const fs::path dir = write_tmp_ini_dir();
    write_file(dir / "b.json", R"({"targetFile": "B.ini", "tweaks": []})");
    write_file(dir / "a.json",
               R"({"targetFile": "A.ini", "tweaks": [
                    {"id": "t1", "name": "T1", "status": "required",
                     "enabled": true, "sourceModId": null,
                     "content": "[S]\nk=v\n"}]})");
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
    auto merged = single_target("x.ini", {{{"S", "k", "new", std::nullopt,
                                           std::string("t1")}}});
    auto out = apply_ini_edits(ini, merged);
    CHECK(out.text.find("k=new\r\n") != std::string::npos);
}
