// Nexus mod-update sweep decision logic (engine/source/nexus/update_check).
//
// The whole cost argument for this feature is that a settled library costs ONE
// request per check, not one per mod - Nexus meters the API hard and a key
// that keeps exceeding the limit gets banned. That argument lives entirely in
// due_for_query(), so it is tested here against fixture tables and no network.
//
// The other invariant under test is the one whose failure is worst for the
// user: a sweep the API REFUSED must never be reportable as "no updates".
// UpdateCheck::Report carries ok=false plus a human-readable message for
// exactly that, and the UI renders ok=false as a warning.
//
// The half of run() that talks to Nexus is covered through the injected
// FakeNetworkManager, against bodies shaped like the ones Nexus actually sends
// - NOT by reaching api.nexusmods.com, which would be both
// environment-dependent and up to 30s of DNS timeout, and would spend a
// metered key to do it. What the fake cannot tell us is whether those bodies
// match reality; that is why they are transcribed from the documented v1 shape
// rather than invented, and why MO2's own reader is cited beside each field.
#include "engine/source/nexus/update_check.h"

#include "engine/mod/meta/mod_meta.h"
#include "engine/network/network_manager.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using engine::Source::Nexus::UpdateCheck::InstalledMod;
using engine::Source::Nexus::UpdateCheck::Period;
using engine::Source::Nexus::UpdateCheck::UpdatedMod;
using engine::Source::Nexus::UpdateCheck::Verdict;

namespace {

// Own throwaway root, removed on scope exit. Never /tmp shared with another
// test or another binary.
struct TempDir {
  std::filesystem::path root;
  explicit TempDir(const char *tag) {
    std::error_code ec;
    root = std::filesystem::temp_directory_path() /
           ("gmm_nexus_upd_" + std::string(tag) + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

// A Nexus-sourced mod folder on disk, as the sweep reads it.
//
// The shape matters and used to be wrong: read_installed() identifies a mod by
// [GameModManager] source_type, which is where the real meta.ini puts it. An
// earlier version of this helper wrote `sourceType=nexus` under [General]
// instead, so read_installed() rejected every seeded mod and `installed` came
// back empty - which is the other half of why no test ever reached the part of
// run() that queries Nexus. A fixture that does not look like the thing it
// stands for is worse than no fixture: it proves the early return, not the path.
void seed_mod(const std::filesystem::path &mods_dir, const std::string &folder,
              long long mod_id, long long file_id) {
  std::error_code ec;
  std::filesystem::create_directories(mods_dir / folder, ec);
  std::ofstream out(mods_dir / folder / "meta.ini");
  out << "[General]\n"
      << "version=1.0\n"
      << "\n[GameModManager]\n"
      << "source_type = nexus\n";
  if (mod_id > 0)
    out << "\n[Nexusmods]\n"
        << "modid=" << mod_id << "\n"
        << "fileid=" << file_id << "\n";
}

InstalledMod mod(long long mod_id, int64_t last_checked) {
  InstalledMod m;
  m.mod_id       = mod_id;
  m.folder       = "m" + std::to_string(mod_id);
  m.last_checked = last_checked;
  return m;
}

}  // namespace

// A CAPTURED Nexus `files.json` reply, read from disk rather than typed here.
//
// The body was recorded verbatim from api.nexusmods.com for mod 6832 (SSE) and
// is committed unaltered. Nothing in it is invented, and that is the whole point:
// the fixture this replaced was hand-written to match the parser, so it proved
// the parser agreed with itself. Its shape - a `data` envelope wrapping
// `files` - is not what Nexus sends, and the parser was written to read it, so
// the pair passed 30 green tests while every one of the user's 64 mods came back
// "a file list this build cannot read".
//
// Three names in the capture are load bearing, and all three are what the old
// parser got wrong:
//
//   files        TOP LEVEL, sibling of file_updates. NO `data` envelope.
//                MO2 reads it exactly so: `resultData.toMap()` of the whole
//                reply, then indexes "files" in it (mainwindow.cpp:3313-3315).
//   file_id      the scalar id, 16268.
//   id           ALSO present, and it is `[16268, 1704]` - the PAIR of file id
//                and mod id. Reading it as a number is as wrong as reading
//                `files` under `data`; both yield an empty list, and an empty
//                list reads downstream as "up to date".
//
// There is no `available_mod_files` key either - provider.cpp spent a branch on
// it for the same endpoint.
std::string captured_files_body(const char *which) {
  const std::filesystem::path p =
      std::filesystem::path(__FILE__).parent_path() / "fixtures" / "nexus" / which;
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  std::string body((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  REQUIRE(!body.empty());
  return body;
}

// A MULTI-file list, so the per-file decisions (archived excluded, newest
// picked, OUR file's version read separately) have more than one row to act on
// - the capture above is a single-file mod and cannot show any of them.
//
// It is built from the captured entry's OWN field set: same outer shape, same
// keys, same value kinds. Only the ids and versions differ. It is therefore not
// a second source of truth and cannot confirm a wrong shape - every structural
// name it uses is one the capture proves. That distinction is the reason the
// old fixture was worthless and this one is not: the old one's names were
// invented to fit the code under test.
static const char *kMultiFileBody = R"({
  "file_updates": [
    {"old_file_id": 16268, "new_file_id": 207864,
     "old_file_name": "qasmoke2-1-0.rar", "new_file_name": "qasmoke2-0-9.rar",
     "uploaded_timestamp": 1483146448}
  ],
  "files": [
    {"id": [16268, 6832], "file_id": 16268, "version": "0.9", "mod_version": "0.9",
     "category_id": 4, "category_name": "OLD_VERSION", "is_primary": false,
     "uploaded_timestamp": 1483146448, "uploaded_time": "2016-12-31T01:07:28.000+00:00",
     "size": 8632, "size_kb": 8632, "size_in_bytes": 8838800, "file_name": "qasmoke2-1-0.rar",
     "description": "", "changelog_html": null},
    {"id": [207864, 6832], "file_id": 207864, "version": "2.0", "mod_version": "2.0",
     "category_id": 1, "category_name": "MAIN", "is_primary": true,
     "uploaded_timestamp": 1721451025, "uploaded_time": "2024-07-18T15:30:25.000+00:00",
     "size": 12000, "size_kb": 12000, "size_in_bytes": 12288000, "file_name": "qasmoke2-0-9.rar",
     "description": "", "changelog_html": null},
    {"id": [999999, 6832], "file_id": 999999, "version": "9.9.9", "mod_version": "9.9.9",
     "category_id": 7, "category_name": "ARCHIVED", "is_primary": false,
     "uploaded_timestamp": 1600000000, "uploaded_time": "2020-09-13T12:26:40.000+00:00",
     "size": 9999, "size_kb": 9999, "size_in_bytes": 9999999, "file_name": "qasmoke2-9-9.rar",
     "description": "", "changelog_html": null},
    {"id": [522942, 6832], "file_id": 522942, "version": "", "mod_version": "3.0",
     "category_id": 2, "category_name": "UPDATE", "is_primary": false,
     "uploaded_timestamp": 1750000000, "uploaded_time": "2025-06-15T20:26:40.000+00:00",
     "size": 4242, "size_kb": 4242, "size_in_bytes": 4343808, "file_name": "qasmoke2-3-0.rar",
     "description": "", "changelog_html": null}
  ]
})";

// THE regression, on the captured body. Before the fix this returned false for
// it, and for every other real reply, which is not "no updates" but "could not
// tell" - a distinction the user was told about 64 times and never once got an
// answer.
TEST_CASE("nexus update sweep: a captured Nexus file list is read and not discarded",
          "[engine][nexus]") {
  using engine::Source::Nexus::UpdateCheck::parse_files;

  const std::string body = captured_files_body("files_6832.json");

  std::vector<long long> available;
  std::string newest;
  std::string installed;
  REQUIRE(parse_files(body, 16268, available, newest, installed));

  // One file, one id - read from `file_id`. The sibling `id` is the PAIR
  // [16268, 1704]; had it been read as a number the list would be empty and
  // parse_files would have returned false here.
  REQUIRE(available.size() == 1);
  REQUIRE(available.front() == 16268);

  // category_id 1 is MAIN, so the file is offered.
  REQUIRE(newest == "v1.0");

  // And the version of the file we actually have, which is what the Version
  // column reads. For this mod that is genuinely v1.0 upstream - the capture
  // cannot show a correction on its own, which is what the backfill case below
  // is for.
  REQUIRE(installed == "v1.0");

  // An id Nexus no longer lists is "not stated", not "blank it out".
  std::vector<long long> avail2;
  std::string n2, i2;
  REQUIRE(parse_files(body, 123456, avail2, n2, i2));
  REQUIRE(i2.empty());
  REQUIRE(n2 == "v1.0");
}

// Both envelopes. The bare one is what Nexus sends; the `data` one is what
// /categories.json really does wrap in (categories.cpp:17), so a reply that
// arrives wrapped is still read rather than discarded.
TEST_CASE("nexus update sweep: a wrapped file list is read too, not only the bare one",
          "[engine][nexus]") {
  using engine::Source::Nexus::UpdateCheck::parse_files;

  const std::string bare    = captured_files_body("files_6832.json");
  const std::string wrapped = R"({"data":{"files":[
      {"id":[16268,6832],"file_id":16268,"version":"v1.0","category_id":1}]}})";

  for (const std::string *body : {&bare, &wrapped}) {
    std::vector<long long> available;
    std::string newest, installed;
    REQUIRE(parse_files(*body, 16268, available, newest, installed));
    REQUIRE(available.size() == 1);
    REQUIRE(available.front() == 16268);
    REQUIRE(installed == "v1.0");
  }

  // Neither shape, and no `files` anywhere: still a refusal. Tolerance must not
  // grow into "anything goes".
  std::vector<long long> none;
  std::string n, i;
  REQUIRE(parse_files(R"({"files":"not an array"})", 16268, none, n, i) == false);
  REQUIRE(parse_files(R"({"mods":{"files":[]}})", 16268, none, n, i) == false);
}

// The per-file decisions, which need more than the one file the capture holds.
// `id` is an array on every row here exactly as the capture proves it is, so a
// parser reading `id` instead of `file_id` still comes back empty.
TEST_CASE("nexus update sweep: archived files are excluded and the newest one is named",
          "[engine][nexus]") {
  using engine::Source::Nexus::UpdateCheck::parse_files;

  std::vector<long long> available;
  std::string newest, installed;
  REQUIRE(parse_files(kMultiFileBody, 16268, available, newest, installed));

  // 999999 is ARCHIVED (category_id 7) and 16268 is OLD_VERSION (category_id 4)
  // - neither is downloadable, and counting them would let a retracted file
  // pass as the newest one. Both are judged on `category_id`, as MO2 judges
  // them (nexusinterface.h:181-193), not on the name.
  REQUIRE(available.size() == 2);
  REQUIRE(available.front() == 207864);
  REQUIRE(available.back() == 522942);
  REQUIRE(newest == "3.0");

  // OURS is 16268 - a file the list has, but one Nexus has since marked OLD -
  // and its version is "0.9". Two things are being asserted here. That an
  // unavailable file is still the version we HAVE: it is the file on our disk,
  // and it is the oldest install there is, so it is the one most likely to be
  // carrying the fabricated "1.0". And that our version is not the newest one.
  REQUIRE(installed == "0.9");

  // The version of OUR file, read by its id - not the newest one.
  std::vector<long long> avail2;
  std::string n2, i2;
  REQUIRE(parse_files(kMultiFileBody, 522942, avail2, n2, i2));
  REQUIRE(i2 == "3.0");

  // An id Nexus does not list: "not stated", so the caller keeps what it has.
  std::vector<long long> avail3;
  std::string n3, i3;
  REQUIRE(parse_files(kMultiFileBody, 123456, avail3, n3, i3));
  REQUIRE(i3.empty());
  REQUIRE(n3 == "3.0");
}

// `version` is the file's own; `mod_version` is the mod-wide value, used only
// when the file states none. File 522942 has an empty `version` and picks up
// "3.0" from `mod_version`, which is why the newest version above is "3.0" and
// not "". MO2's order, downloadmanager.cpp:2274-2276.
TEST_CASE("nexus update sweep: mod_version answers only where version states nothing",
          "[engine][nexus]") {
  using engine::Source::Nexus::UpdateCheck::parse_files;

  // Empty `version`, so mod_version supplies it.
  std::vector<long long> a;
  std::string n, i;
  REQUIRE(parse_files(R"({"files":[
      {"file_id":10,"version":"","mod_version":"7.4","category_id":1}]})",
                      10, a, n, i));
  REQUIRE(i == "7.4");
  REQUIRE(n == "7.4");

  // `version` present: it wins, and mod_version is not consulted. "9.1" here is
  // deliberately different from the mod_version beside it so a swap cannot pass.
  a.clear();
  REQUIRE(parse_files(R"({"files":[
      {"file_id":10,"version":"9.1","mod_version":"7.4","category_id":1}]})",
                      10, a, n, i));
  REQUIRE(i == "9.1");
  REQUIRE(n == "9.1");

  // Neither stated: genuinely not stated, and the caller must leave the
  // recorded version alone rather than overwrite a good one with nothing.
  a.clear();
  REQUIRE(parse_files(R"({"files":[{"file_id":10,"category_id":1}]})", 10, a, n, i));
  REQUIRE(i.empty());
  REQUIRE(n.empty());
  REQUIRE(a.size() == 1);
}

// The sweep end to end over a faked network, which is the only way to reach the
// half of run() that actually talks to Nexus. It asserts what a real run must
// do: write a verdict and badge an update.
//
// One mod per case on purpose. run() walks the mods in directory_iterator
// order and the fake serves responses FIFO, so a multi-mod case would silently
// depend on the filesystem to decide which mod gets which answer.
TEST_CASE("nexus update sweep: a readable answer becomes a badge", "[engine][nexus]") {
  using engine::network::FakeNetworkManager;
  using engine::Source::Nexus::UpdateCheck::run;

  const TempDir tmp("sweep_ok");
  const int64_t now = 1700000000;
  seed_mod(tmp.root, "MyMod", 100, 207864);  // installed file id 207864

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));

  engine::network::Response bulk;
  bulk.http_code = 200;
  bulk.body      = R"([{"mod_id":100,"latest_file_update":1699999999}])";
  fakeptr->enqueue_response(bulk);

  engine::network::Response files;
  files.http_code = 200;
  files.body      = kMultiFileBody;  // newest non-archived file id is 522942
  fakeptr->enqueue_response(files);

  const auto report = run("skyrimspecialedition", "test-key", tmp.root, now);
  engine::network::set_instance(nullptr);

  REQUIRE(report.total == 1);
  REQUIRE(report.requests == 2);  // one bulk, one per-mod
  REQUIRE(report.checked == 1);
  REQUIRE(report.ok == true);
  REQUIRE(report.unreachable.empty());
  REQUIRE(report.message.empty());  // a complete answer needs no warning

  // THE thing the user could not see before: a verdict, in the meta.ini the
  // mod list reads to decide whether to paint a row red.
  const auto meta = engine::ModMeta::load(tmp.root, "MyMod");
  REQUIRE(meta.get("General", "nexus_update") == "true");  // 522942 > 207864
  REQUIRE(meta.get("General", "newestversion") == "3.0");
  REQUIRE(meta.get("Nexusmods", "lastnexusquery") == "2023-11-14T22:13:20Z");
  REQUIRE(report.updates == 1);

  // And the installed version, which the fixture seeded as the fabricated
  // "1.0" every MO2-style install used to write when it had no version to
  // record. Nexus names the version of file 207864 - the file we actually
  // have - so that is what lands, and it is what the Version column then
  // displays (mod_scanner.cpp:560). Same response, no extra request.
  REQUIRE(meta.get("General", "version") == "2.0");
}

// THE VERSION COLUMN, correcting itself.
//
// A census of one real instance found 60 meta.ini files still carrying
// `version = 1.0` - a value this app fabricated at install time when it had
// nothing better to record, and printed in the Version column ever since. The
// code that wrote it is gone; the files are not, and they cannot be fixed by
// hand. SkyUI is the proof it is still wrong: `installationfile =
// SkyUI-12604-6-11-1778020881.zip` says 6.11 while the same meta says 1.0.
//
// The repair is the response the sweep has already paid for. It is the same
// source the Version column was always supposed to show, and using it costs no
// request that is not already being made - which is the only reason this is
// worth doing at all.
//
// HONEST LIMIT: the capture this repo has is mod 6832, whose real version
// genuinely IS "v1.0", so it cannot by itself show a wrong value being
// corrected. SkyUI (12604) and BodySlide (201) were not captured: no Nexus API
// key is reachable from here and the stored one is not readable, so inventing
// either reply would be the exact mistake this file exists to end. What is
// asserted below is therefore (a) the fabricated "1.0" being replaced by the
// string Nexus actually returns, on the captured reply, and (b) the same
// correction where installed and upstream genuinely differ in digit count.
TEST_CASE(
    "nexus update sweep: the fabricated installed version is replaced by Nexus's own",
    "[engine][nexus]") {
  using engine::network::FakeNetworkManager;
  using engine::Source::Nexus::UpdateCheck::run;

  const TempDir tmp("sweep_backfill");
  const int64_t now = 1700000000;
  seed_mod(tmp.root, "QASmoke2", 6832, 16268);  // seed_mod writes version=1.0

  // Start from the state the user's disk is actually in: the fabricated value,
  // and nothing else. If the sweep does not overwrite it, this is what stays.
  {
    engine::ModMeta meta = engine::ModMeta::load(tmp.root, "QASmoke2");
    REQUIRE(meta.get("General", "version") == "1.0");
  }

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));

  engine::network::Response bulk;
  bulk.http_code = 200;
  bulk.body      = R"([{"mod_id":6832,"latest_file_update":1699999999}])";
  fakeptr->enqueue_response(bulk);

  engine::network::Response files;
  files.http_code = 200;
  files.body      = captured_files_body("files_6832.json");
  fakeptr->enqueue_response(files);

  const auto report = run("skyrimspecialedition", "test-key", tmp.root, now);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);
  REQUIRE(report.checked == 1);
  REQUIRE(report.unreachable.empty());

  // The fabricated "1.0" is gone and what Nexus says about file 16268 is what
  // the Version column will now read (mod_scanner.cpp:560 reads [General]
  // version). This is the whole point: a value the sweep got for free, from a
  // request it was making anyway.
  const auto meta = engine::ModMeta::load(tmp.root, "QASmoke2");
  REQUIRE(meta.get("General", "version") == "v1.0");
}

// The shape the poisoned files actually take, where the installed version and
// the newest one are not merely spelled differently but genuinely differ, as
// SkyUI's "6.11" and "1.0" do. Installed file is 207864 here, which the list
// calls "2.0" while the newest offered is "3.0": a sweep that confuses the two
// writes "3.0" into the Version column and is wrong in a way no one can see.
TEST_CASE("nexus update sweep: the installed version is written, not the newest one",
          "[engine][nexus]") {
  using engine::network::FakeNetworkManager;
  using engine::Source::Nexus::UpdateCheck::run;

  const TempDir tmp("sweep_backfill_diverged");
  const int64_t now = 1700000000;
  seed_mod(tmp.root, "DivergedMod", 6832, 207864);  // version=1.0, file 207864

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));

  engine::network::Response bulk;
  bulk.http_code = 200;
  bulk.body      = R"([{"mod_id":6832,"latest_file_update":1699999999}])";
  fakeptr->enqueue_response(bulk);

  engine::network::Response files;
  files.http_code = 200;
  files.body      = kMultiFileBody;
  fakeptr->enqueue_response(files);

  const auto report = run("skyrimspecialedition", "test-key", tmp.root, now);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);
  REQUIRE(report.unreachable.empty());

  const auto meta = engine::ModMeta::load(tmp.root, "DivergedMod");
  // "2.0" - what WE have - and not "3.0", what Nexus offers. These are two
  // different strings and swapping them is the failure this whole path exists
  // to prevent.
  REQUIRE(meta.get("General", "version") == "2.0");
  REQUIRE(meta.get("General", "newestversion") == "3.0");
}

// Nexus stating no version at all must not blank a good recorded one. Writing
// nothing over something correct trades one wrong display for another, and the
// 1.0s this replaces are not the only wrong value a user can have on disk.
TEST_CASE("nexus update sweep: an unstated version never blanks a recorded one",
          "[engine][nexus]") {
  using engine::network::FakeNetworkManager;
  using engine::Source::Nexus::UpdateCheck::run;

  const TempDir tmp("sweep_backfill_blank");
  const int64_t now = 1700000000;
  seed_mod(tmp.root, "NoVerMod", 900, 10);  // version=1.0, installed file 10

  {
    engine::ModMeta meta = engine::ModMeta::load(tmp.root, "NoVerMod");
    meta.set("General", "version", "4.2");  // a version a real user recorded
    REQUIRE(meta.save(tmp.root, "NoVerMod"));
  }

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));

  engine::network::Response bulk;
  bulk.http_code = 200;
  bulk.body      = R"([{"mod_id":900,"latest_file_update":1699999999}])";
  fakeptr->enqueue_response(bulk);

  engine::network::Response files;
  files.http_code = 200;
  // File 10 is listed and states neither `version` nor `mod_version`.
  files.body = R"({"file_updates":[],"files":[{"id":[10,900],"file_id":10,
                     "category_id":1,"category_name":"MAIN"}]})";
  fakeptr->enqueue_response(files);

  const auto report = run("skyrimspecialedition", "test-key", tmp.root, now);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);
  REQUIRE(report.unreachable.empty());  // the mod WAS answered for
  REQUIRE(report.checked == 1);

  // The verdict still lands - the mod was checked - but the version is left
  // exactly as the user had it.
  const auto meta = engine::ModMeta::load(tmp.root, "NoVerMod");
  REQUIRE(meta.get("General", "version") == "4.2");
  REQUIRE(meta.get("General", "nexus_update") == "false");
}

// A mod the sweep could not answer for must be NAMED, with a reason, and must
// not be recorded as checked. The shipped bug's worst half was exactly here: a
// 200 carrying something unreadable was logged as "no file list for mod N" with
// no reason, then stamped as freshly checked, so the next sweep skipped it.
TEST_CASE("nexus update sweep: an unreadable answer is named and not stamped",
          "[engine][nexus]") {
  using engine::network::FakeNetworkManager;
  using engine::Source::Nexus::UpdateCheck::run;

  const TempDir tmp("sweep_bad");
  const int64_t now = 1700000000;
  seed_mod(tmp.root, "BrokenMod", 300, 207864);

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));

  engine::network::Response bulk;
  bulk.http_code = 200;
  bulk.body      = R"([{"mod_id":300,"latest_file_update":1699999999}])";
  fakeptr->enqueue_response(bulk);

  // A SUCCESSFUL request whose body is not a file list: the exact shape of the
  // bug, which the old code turned into a silent skip rather than a report.
  engine::network::Response unreadable;
  unreadable.http_code = 200;
  unreadable.body      = "<html>maintenance</html>";
  fakeptr->enqueue_response(unreadable);

  const auto report = run("skyrimspecialedition", "test-key", tmp.root, now);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);  // the sweep ran; it is not a whole-sweep failure
  REQUIRE(report.unreachable.size() == 1);
  REQUIRE(report.unreachable.front().folder == "BrokenMod");
  REQUIRE(report.unreachable.front().mod_id == "300");
  REQUIRE_FALSE(report.unreachable.front().reason.empty());
  // A partial answer must never be renderable as "up to date".
  REQUIRE_FALSE(report.message.empty());
  REQUIRE(report.updates == 0);

  const auto meta = engine::ModMeta::load(tmp.root, "BrokenMod");
  REQUIRE(meta.get("General", "nexus_update").empty());
  // Not stamped: a mod we could not reach must not look recently checked, or
  // the next sweep's window would skip it and the miss would become permanent.
  REQUIRE(meta.get("Nexusmods", "lastnexusquery").empty());
}

// The other way a mod gets no verdict WITHOUT the body being unreadable: Nexus
// answers fine, but our own meta.ini never recorded which file is installed, so
// there is nothing to compare against. This is the last path where a sweep could
// answer "no update" for a mod nobody checked - and answering it that way is
// worse than answering nothing, because it is indistinguishable from a real
// result and gets stored as one.
TEST_CASE("nexus update sweep: an undecidable mod is named, never called current",
          "[engine][nexus]") {
  using engine::network::FakeNetworkManager;
  using engine::Source::Nexus::UpdateCheck::run;

  const TempDir tmp("sweep_nofileid");
  const int64_t now = 1700000000;
  seed_mod(tmp.root, "NoFileIdMod", 400, 0);  // a Nexus mod we cannot place on Nexus

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));

  engine::network::Response bulk;
  bulk.http_code = 200;
  bulk.body      = R"([{"mod_id":400,"latest_file_update":1699999999}])";
  fakeptr->enqueue_response(bulk);

  // A perfectly good file list - the parse succeeds, so this is NOT the
  // unreadable-body path; there is simply no installed id to judge it by.
  engine::network::Response files;
  files.http_code = 200;
  files.body      = kMultiFileBody;
  fakeptr->enqueue_response(files);

  const auto report = run("skyrimspecialedition", "test-key", tmp.root, now);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);
  REQUIRE(report.checked == 1);  // the request was made and the body was fine
  REQUIRE(report.unreachable.size() == 1);
  REQUIRE(report.unreachable.front().folder == "NoFileIdMod");
  REQUIRE_FALSE(report.unreachable.front().reason.empty());
  REQUIRE(report.updates == 0);
  REQUIRE_FALSE(report.message.empty());

  const auto meta = engine::ModMeta::load(tmp.root, "NoFileIdMod");
  // The whole point: no verdict at all. "false" here would be a fabricated
  // result, and the mod list would read it as a mod that was checked.
  REQUIRE(meta.get("General", "nexus_update").empty());
  REQUIRE(meta.get("Nexusmods", "lastnexusquery").empty());
}

// The failure the user actually hit, end to end. A real run reported
// "0 updates from 0 queried / 64 mods, 1 requests": one bulk request, no
// per-mod calls, and every mod answered from the cache.
//
// The cache said "checked" only because an earlier sweep STAMPED every mod it
// failed to answer for. That write was already removed, but the stamps it left
// on disk are indistinguishable from honest ones, so the ordinary sweep keeps
// skipping them forever. This is why a forced path has to exist at all, and
// this is the test that says it forces.
//
// The seeded stamp is deliberately FRESHER than the window entry, so the
// cached path has every reason to skip: this is not "the window missed it",
// it is "the cache says it was just asked".
TEST_CASE("nexus update sweep: force re-asks despite a fresh lastnexusquery stamp",
          "[engine][nexus]") {
  using engine::network::FakeNetworkManager;
  using engine::Source::Nexus::UpdateCheck::run;

  const TempDir tmp("force");
  const int64_t now = 1700000000;
  seed_mod(tmp.root, "StaleStamp", 500, 207864);

  // Stamp it as checked a second ago - exactly the lie the shipped bug wrote.
  {
    engine::ModMeta meta = engine::ModMeta::load(tmp.root, "StaleStamp");
    meta.set("Nexusmods", "lastnexusquery", "2023-11-14T22:13:19Z");
    REQUIRE(meta.save(tmp.root, "StaleStamp"));
  }

  engine::network::Response bulk;
  bulk.http_code = 200;
  // The window DOES mention the mod, but says it last moved BEFORE our stamp.
  bulk.body = R"([{"mod_id":500,"latest_file_update":1699000000}])";

  engine::network::Response files;
  files.http_code = 200;
  files.body      = kMultiFileBody;  // newest live file 522942 > our 207864

  // --- cached: the cache speaks, so nothing is asked ---
  {
    auto fake     = std::make_unique<FakeNetworkManager>();
    auto *fakeptr = fake.get();
    engine::network::set_instance(std::move(fake));
    fakeptr->enqueue_response(bulk);
    fakeptr->enqueue_response(files);  // must NOT be consumed

    const auto report = run("skyrimspecialedition", "test-key", tmp.root, now);
    engine::network::set_instance(nullptr);

    REQUIRE(report.ok == true);
    REQUIRE(report.total == 1);
    REQUIRE(report.requests == 1);  // bulk only
    REQUIRE(report.checked == 0);   // the stamp suppressed the per-mod call
    REQUIRE(report.updates == 0);
  }

  // --- forced: the stamp is ignored and the mod is asked about ---
  // Re-stamp first. The cached run above RE-STAMPED the mod it answered from
  // the cache (that is the loop at the end of run()), so without this the
  // forced block would start from a zero gap - where full_recheck_required is
  // already true and force is indistinguishable from the ordinary path. The
  // test would then pass with force wired up to nothing, which is exactly the
  // regression it exists to catch.
  {
    engine::ModMeta meta = engine::ModMeta::load(tmp.root, "StaleStamp");
    meta.set("Nexusmods", "lastnexusquery", "2023-11-14T22:13:19Z");
    REQUIRE(meta.save(tmp.root, "StaleStamp"));
  }
  {
    auto fake     = std::make_unique<FakeNetworkManager>();
    auto *fakeptr = fake.get();
    engine::network::set_instance(std::move(fake));
    fakeptr->enqueue_response(bulk);
    fakeptr->enqueue_response(files);

    const auto report =
        run("skyrimspecialedition", "test-key", tmp.root, now, /*force=*/true);
    engine::network::set_instance(nullptr);

    REQUIRE(report.ok == true);
    REQUIRE(report.requests == 2);  // bulk + the per-mod call force bought
    REQUIRE(report.checked == 1);
    REQUIRE(report.updates == 1);  // and it found the update that was there
    REQUIRE(report.unreachable.empty());

    const auto meta = engine::ModMeta::load(tmp.root, "StaleStamp");
    REQUIRE(meta.get("General", "nexus_update") == "true");
    REQUIRE(meta.get("General", "newestversion") == "3.0");
    // The fabricated install-time version is repaired on the same pass.
    REQUIRE(meta.get("General", "version") == "2.0");
  }
}

// The cost claim: one bulk request answers every mod, and a mod absent from
// the window (or untouched since we looked) owes nothing further.
TEST_CASE("nexus update sweep: only mods whose page moved owe a request") {
  const int64_t day = 86400;
  const int64_t now = 1700000000;
  std::vector<InstalledMod> installed{
      mod(10, now - day),  // looked yesterday, nothing in the window
      mod(11, now - day),  // looked yesterday, Nexus says it moved
      mod(12, 0),          // never checked at all
  };

  std::vector<UpdatedMod> updated{{11, now - 3600}};

  const auto due =
      engine::Source::Nexus::UpdateCheck::due_for_query(installed, updated, false);
  std::vector<long long> ids;
  for (const auto &m : due)
    ids.push_back(m.mod_id);

  // 11 (moved) and 12 (never checked). NOT 10 - a settled library must not
  // spend one request per mod per check, which is the entire reason this
  // design exists.
  REQUIRE(ids.size() == 2);
  REQUIRE(ids[0] == 11);
  REQUIRE(ids[1] == 12);
}

TEST_CASE("nexus update sweep: a full recheck asks about every mod") {
  const int64_t now = 1700000000;
  std::vector<InstalledMod> installed{mod(10, now - 86400), mod(11, now - 86400)};

  const auto due =
      engine::Source::Nexus::UpdateCheck::due_for_query(installed, {}, true);
  REQUIRE(due.size() == 2);
}

TEST_CASE("nexus update sweep: window width covers the gap, never narrower") {
  using engine::Source::Nexus::UpdateCheck::period_for;
  REQUIRE(period_for(0) == Period::Month);     // never checked
  REQUIRE(period_for(-500) == Period::Month);  // clock went backwards
  REQUIRE(period_for(3600) == Period::Day);
  REQUIRE(period_for(86400) == Period::Week);
  REQUIRE(period_for(29 * 86400) == Period::Week);

  // A month is the widest the endpoint offers, so past it the window can no
  // longer speak for the cache and a full recheck takes over.
  using engine::Source::Nexus::UpdateCheck::full_recheck_required;
  REQUIRE(full_recheck_required(0));
  REQUIRE(full_recheck_required(-1));
  REQUIRE(full_recheck_required(29 * 86400) == false);
  REQUIRE(full_recheck_required(31 * 86400));
}

// The comparison itself. Nexus file ids rise with upload order, so "is a newer
// file still on offer" is a max() - no version-string parsing, which stays
// correct for the many Nexus mods whose version reads "Update 3".
TEST_CASE("nexus update sweep: file-id comparison never guesses Update") {
  using engine::Source::Nexus::UpdateCheck::evaluate;

  auto current = evaluate(100, {50, 100});
  REQUIRE(current.state == Verdict::Current);

  auto newer = evaluate(100, {50, 100, 250});
  REQUIRE(newer.state == Verdict::Update);
  REQUIRE(newer.newest_file_id == 250);

  // No installed id, or no readable file list: Unknown, never Update. Calling
  // an unread answer "update" would nag; calling it "current" would lie.
  REQUIRE(evaluate(0, {250}).state == Verdict::Unknown);
  REQUIRE(evaluate(100, {}).state == Verdict::Unknown);
}

// MO2 ModInfoRegular::ignoreUpdate: the ignore records the version turned away
// from and suppresses only while the newest version is STILL that one.
TEST_CASE("nexus update sweep: ignore suppresses one version, not the next") {
  using engine::Source::Nexus::UpdateCheck::update_visible;

  REQUIRE(update_visible(true, "2.0", ""));
  REQUIRE(update_visible(true, "2.0", "2.0") == false);  // still the ignored one
  REQUIRE(update_visible(true, "3.0", "2.0"));           // a genuinely newer one
  REQUIRE(update_visible(false, "3.0", "") == false);    // no update at all
}

TEST_CASE("nexus update sweep: an unreadable updated.json is not an empty one") {
  using engine::Source::Nexus::UpdateCheck::parse_updated;
  std::vector<UpdatedMod> out;

  REQUIRE(parse_updated(
      R"([{"mod_id":123,"latest_file_update":1700000000},
          {"mod_id":456,"latest_file_update":1700000001}])",
      out));
  REQUIRE(out.size() == 2);
  REQUIRE(out[0].mod_id == 123);
  REQUIRE(out[1].latest_file_update == 1700000001);

  // The two that must stay apart downstream: an empty list ("nothing moved")
  // and an unreadable body ("we do not know"). Reporting the second as the
  // first is how a rate-limited or proxied response would read as an
  // up-to-date library.
  std::vector<UpdatedMod> guarded;
  REQUIRE(parse_updated("[]", guarded));
  REQUIRE(guarded.empty());

  std::vector<UpdatedMod> broken;
  REQUIRE(parse_updated("<html>502 Bad Gateway</html>", broken) == false);
  REQUIRE(broken.empty());  // and leaves `out` untouched
}

// The refusal contract. UpdateCheck::Report is what the UI renders, and its
// whole job on the failure side is to be UNRENDERABLE as "no updates": ok
// false means "we do not know", and it always carries text so the warning can
// never come out blank. The two refusals below are run()'s only zero-network
// ones, so this needs no key and no request; the HTTP branches (429 / non-200
// / unread body) carry the same ok=false plus a message that says outright
// that whether updates exist is unknown, and are the ones that need a server.
TEST_CASE("nexus update sweep: a refused check never reports as success") {
  using engine::Source::Nexus::UpdateCheck::run;
  const TempDir tmp("refuse");
  const int64_t now = 1700000000;

  const auto no_domain = run("", "some-key", tmp.root, now);
  REQUIRE(no_domain.ok == false);
  REQUIRE(no_domain.message.empty() == false);
  REQUIRE(no_domain.rate_limited == false);

  const auto no_key = run("skyrimspecialedition", "", tmp.root, now);
  REQUIRE(no_key.ok == false);
  REQUIRE(no_key.message.empty() == false);

  // A sweep with nothing of ours on Nexus is a COMPLETED sweep that found
  // nothing, and it cost zero requests. Reporting it as a failure would put
  // "update check failed" over an instance that simply has no Nexus mods.
  seed_mod(tmp.root, "PlainMod", 0, 0);
  const auto nothing = run("skyrimspecialedition", "some-key", tmp.root, now);
  REQUIRE(nothing.ok == true);
  REQUIRE(nothing.message.empty());
  REQUIRE(nothing.updates == 0);
  REQUIRE(nothing.requests == 0);
  REQUIRE(nothing.total == 0);
}

// The ignore is persisted per mod folder in that folder's own meta.ini, so it
// survives a restart and travels with the mod rather than the profile.
//
// The key is asserted under MO2's spelling, not ours. Writing the key we read
// only proves the round trip; what matters is that the value lands where MO2
// will find it (modinforegular.cpp:96) and where an MO2-written file will be
// read back.
TEST_CASE("nexus update sweep: ignore is written to the mod's own meta.ini") {
  using engine::Source::Nexus::UpdateCheck::set_ignored_version;
  const TempDir tmp("ignore");
  seed_mod(tmp.root, "NexusMod", 123, 456);

  // Nothing to record: refusing is better than writing an empty ignore, which
  // would suppress nothing and look like a working feature.
  REQUIRE(set_ignored_version(tmp.root, "NexusMod", true) == false);

  {
    engine::ModMeta meta;
    meta.set("General", "newestversion", "2.1");
    REQUIRE(meta.save(tmp.root, "NexusMod"));
  }
  REQUIRE(set_ignored_version(tmp.root, "NexusMod", true));

  {
    const auto meta = engine::ModMeta::load(tmp.root, "NexusMod");
    REQUIRE(meta.get("General", "ignoredVersion") == "2.1");
  }

  REQUIRE(set_ignored_version(tmp.root, "NexusMod", false));
  {
    const auto meta = engine::ModMeta::load(tmp.root, "NexusMod");
    REQUIRE(meta.get("General", "ignoredVersion").empty());
  }
}

// The casing bug itself, at the consumer rather than at the key.
//
// An ignore is worthless if only this app can read it: the user sets "ignore
// 2.0" in MO2, the instance is imported, and the sweep re-badges the very file
// the user said to leave alone. Reading only the lowercase spelling did exactly
// that, silently - no error, just an update badge the user could not dismiss.
//
// So this drives the whole path: an MO2-spelled key on disk, through
// read_installed(), into update_visible(), out as the count the UI renders. One
// mod per case for the reason given on the sweep case above.
TEST_CASE("nexus update sweep: an ignore written by MO2 is honoured",
          "[engine][nexus]") {
  using engine::network::FakeNetworkManager;
  using engine::Source::Nexus::UpdateCheck::run;

  const TempDir tmp("sweep_mo2_ignore");
  const int64_t now = 1700000000;
  seed_mod(tmp.root, "Mo2Mod", 100, 207864);

  // Exactly what MO2 writes: newestVersion + ignoredVersion, both camel, and
  // the version the body below is going to report as newest.
  {
    engine::ModMeta meta = engine::ModMeta::load(tmp.root, "Mo2Mod");
    meta.set("General", "newestVersion", "3.0");
    meta.set("General", "ignoredVersion", "3.0");
    REQUIRE(meta.save(tmp.root, "Mo2Mod"));
  }

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));

  engine::network::Response bulk;
  bulk.http_code = 200;
  bulk.body      = R"([{"mod_id":100,"latest_file_update":1699999999}])";
  fakeptr->enqueue_response(bulk);

  engine::network::Response files;
  files.http_code = 200;
  files.body      = kMultiFileBody;  // newest non-archived version is "3.0"
  fakeptr->enqueue_response(files);

  const auto report = run("skyrimspecialedition", "test-key", tmp.root, now);
  engine::network::set_instance(nullptr);

  // Same body, same mod, same two requests as the badge case above - and no
  // update, because that is the version the user turned away from. The check
  // itself still ran and still recorded its answer: suppressing a badge is not
  // the same as skipping the mod.
  REQUIRE(report.ok == true);
  REQUIRE(report.checked == 1);
  REQUIRE(report.requests == 2);
  REQUIRE(report.unreachable.empty());
  REQUIRE(report.updates == 0);
  REQUIRE(engine::ModMeta::load(tmp.root, "Mo2Mod").get("General", "nexus_update") ==
          "true");  // an update exists; the user is just ignoring it
}

// A meta.ini that still carries only the old lowercase spelling must keep
// working. This is the one place back-compat is load bearing: those files are on
// users' disks right now, and the fix must not read them as "nothing ignored".
TEST_CASE("nexus update sweep: the legacy lowercase ignore still suppresses") {
  using engine::Source::Nexus::UpdateCheck::read_ignored_version;
  using engine::Source::Nexus::UpdateCheck::update_visible;

  engine::ModMeta legacy;
  legacy.set("General", "newestversion", "2.0");
  legacy.set("General", "ignoredversion", "2.0");
  REQUIRE(read_ignored_version(legacy) == "2.0");
  REQUIRE(update_visible(true, "2.0", read_ignored_version(legacy)) == false);

  // MO2's spelling wins when a file somehow carries both, so the value this app
  // maintains is the one that decides.
  engine::ModMeta both;
  both.set("General", "ignoredVersion", "2.0");
  both.set("General", "ignoredversion", "9.9");
  REQUIRE(read_ignored_version(both) == "2.0");
}
