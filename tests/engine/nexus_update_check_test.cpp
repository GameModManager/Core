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
// exactly that, and the UI renders ok=false as a warning. run()'s two
// zero-network refusals (no domain, no key) are the reachable way to assert
// that contract without touching api.nexusmods.com - which is also why this
// file never issues a request: the only reachable paths here return before
// the first curl call, and a test that reaches the network would be both
// environment-dependent and up to 30s of DNS timeout.
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

// A real Nexus v1 `files.json` body, field for field. This is the shape Nexus
// documents and the shape MO2 parses (mainwindow.cpp:3329, nexusinterface.cpp:
// 132, downloadmanager.cpp:1880 all read `file_id`). Two names in it are load
// bearing: the download set is `data.files[]`, and each entry's id is
// `file_id` - there is no `available_mod_files` and no `id`.
//
// It is reproduced here verbatim rather than abstracted because the bug this
// pins down was a parser reading fields Nexus has never sent: every HTTP 200
// was billed, logged as a request, and then discarded as unparseable, so the
// sweep reported nothing for any mod and the user saw no badge anywhere.
static const char *kRealFilesBody = R"({
  "data": {
    "mod_id": 6832,
    "files": [
      {"file_id": 207864, "name": "A Mod", "version": "1.0", "category_name": null},
      {"file_id": 522942, "name": "A Mod", "version": "2.0", "category_name": "MAIN"},
      {"file_id": 999999, "name": "A Mod", "version": "9.9.9", "category_name": "ARCHIVED"}
    ],
    "file_updates": [
      {"old_file_id": 207864, "new_file_id": 522942,
       "old_file_name": "a-1.0.7z", "new_file_name": "a-2.0.7z",
       "uploaded_timestamp": 1721451025}
    ]
  }
})";

// The regression that shipped the broken sweep: an unparseable file list must
// be LOUD, never an empty answer dressed up as "nothing changed".
TEST_CASE("nexus update sweep: a real Nexus file list is read and not discarded") {
  using engine::Source::Nexus::UpdateCheck::parse_files;

  std::vector<long long> available;
  std::string newest;
  REQUIRE(parse_files(kRealFilesBody, available, newest));

  // The archived file is excluded: it is not downloadable, and counting it
  // would let a retracted file pass as the newest one.
  REQUIRE(available.size() == 2);
  REQUIRE(available.front() == 207864);
  REQUIRE(available.back() == 522942);
  REQUIRE(newest == "2.0");

  // The exact regression shape: the same list under the key name Nexus does
  // not use yields nothing, and "nothing" must read as a refusal, not as a
  // mod that is up to date.
  const char *wrong_key = R"({"data":{"files":[{"id":522942,"version":"2.0"}]}})";
  std::vector<long long> none;
  std::string no_version;
  REQUIRE(parse_files(wrong_key, none, no_version) == false);
  REQUIRE(none.empty());
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
  files.body      = kRealFilesBody;  // newest non-archived file id is 522942
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
  REQUIRE(meta.get("General", "newestversion") == "2.0");
  REQUIRE(meta.get("Nexusmods", "lastnexusquery") == "2023-11-14T22:13:20Z");
  REQUIRE(report.updates == 1);
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
  files.body      = kRealFilesBody;
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
    REQUIRE(meta.get("General", "ignoredversion") == "2.1");
  }

  REQUIRE(set_ignored_version(tmp.root, "NexusMod", false));
  {
    const auto meta = engine::ModMeta::load(tmp.root, "NexusMod");
    REQUIRE(meta.get("General", "ignoredversion").empty());
  }
}
