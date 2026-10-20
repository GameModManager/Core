// The page-scrape update sweep end to end (LoversLab + ModPub
// Provider::check_updates), over the injected FakeNetworkManager.
//
// The same rules the Nexus sweep is tested for, applied to the scrape path:
// a readable page becomes a verdict in meta.ini, a FAILED scrape writes
// NOTHING (never "up to date"), a page that states nothing comparable gets no
// verdict at all, and mods without page provenance are not counted as
// checked. The page body for LoversLab is the committed capture
// (fixtures/loverslab_page.html) - softwareVersion "1.0", dateModified
// "2020-01-09T18:55:44+0000" - and the seeded meta state is what varies.
#include "engine/source/loverslab/provider.h"
#include "engine/source/modpub/provider.h"

#include "engine/mod/meta/mod_meta.h"
#include "engine/network/network_manager.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

struct TempDir {
  std::filesystem::path root;
  explicit TempDir(const char *tag) {
    std::error_code ec;
    root = std::filesystem::temp_directory_path() /
           ("gmm_page_sweep_" + std::string(tag) + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

std::string read_fixture(const char *name) {
  const std::filesystem::path p =
      std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// A LoversLab-sourced mod folder with page provenance, as the sweep reads it.
void seed_ll_mod(const std::filesystem::path &mods_dir, const std::string &folder,
                 const std::string &version, const std::string &date_modified) {
  std::error_code ec;
  std::filesystem::create_directories(mods_dir / folder, ec);
  std::ofstream out(mods_dir / folder / "meta.ini");
  out << "[General]\n"
      << "version=" << version << "\n"
      << "\n[GameModManager]\n"
      << "source_type = loverslab\n"
      << "\n[LoversLab]\n"
      << "fileid=6832\n"
      << "page_url=https://www.loverslab.com/files/file/6832-example/\n";
  if (!date_modified.empty())
    out << "date_modified=" << date_modified << "\n";
}

}  // namespace

TEST_CASE("loverslab sweep: a readable page becomes a verdict", "[engine][loverslab]") {
  using engine::network::FakeNetworkManager;

  const TempDir tmp("ll_ok");
  // The capture says 1.0 / 2020-01-09; the seeded state is what an install
  // BEFORE that looked like, so the page is genuinely newer.
  seed_ll_mod(tmp.root, "MyMod", "0.9", "2019-01-01T00:00:00+0000");
  // A non-LoversLab row in the same dir: the sweep must not count it.
  {
    std::error_code ec;
    std::filesystem::create_directories(tmp.root / "NexusMod", ec);
    std::ofstream out(tmp.root / "NexusMod" / "meta.ini");
    out << "[General]\nversion=1.0\n\n[GameModManager]\nsource_type = nexus\n";
  }

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));
  engine::network::Response page;
  page.http_code = 200;
  page.body      = read_fixture("loverslab_page.html");
  fakeptr->enqueue_response(page);

  engine::Source::LoversLab::Provider provider;
  engine::Source::UpdateSweepContext ctx;
  ctx.mods_dir      = tmp.root;
  const auto report = provider.check_updates(ctx);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);
  REQUIRE(report.total == 1);  // the nexus row is not the sweep's business
  REQUIRE(report.updates == 1);
  REQUIRE(report.message.empty());

  const auto meta = engine::ModMeta::load(tmp.root, "MyMod");
  REQUIRE(meta.get("General", "update_available") == "true");
  REQUIRE(meta.get("General", "newestversion") == "1.0");
  REQUIRE(meta.get("LoversLab", "date_modified") == "2020-01-09T18:55:44+0000");
}

TEST_CASE("loverslab sweep: same page state is stamped up to date, not an update",
          "[engine][loverslab]") {
  using engine::network::FakeNetworkManager;

  const TempDir tmp("ll_current");
  seed_ll_mod(tmp.root, "MyMod", "1.0", "2020-01-09T18:55:44+0000");

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));
  engine::network::Response page;
  page.http_code = 200;
  page.body      = read_fixture("loverslab_page.html");
  fakeptr->enqueue_response(page);

  engine::Source::LoversLab::Provider provider;
  engine::Source::UpdateSweepContext ctx;
  ctx.mods_dir      = tmp.root;
  const auto report = provider.check_updates(ctx);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);
  REQUIRE(report.total == 1);
  REQUIRE(report.updates == 0);
  const auto meta = engine::ModMeta::load(tmp.root, "MyMod");
  REQUIRE(meta.get("General", "update_available") == "false");
}

TEST_CASE("loverslab sweep: a failed scrape writes NOTHING", "[engine][loverslab]") {
  using engine::network::FakeNetworkManager;

  const TempDir tmp("ll_fail");
  seed_ll_mod(tmp.root, "MyMod", "0.9", "2019-01-01T00:00:00+0000");

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));
  engine::network::Response page;
  page.http_code = 403;  // the login/NSFW/Cloudflare gate, as the site sends it
  page.body      = "<html><body>Access denied</body></html>";
  fakeptr->enqueue_response(page);

  engine::Source::LoversLab::Provider provider;
  engine::Source::UpdateSweepContext ctx;
  ctx.mods_dir      = tmp.root;
  const auto report = provider.check_updates(ctx);
  engine::network::set_instance(nullptr);

  // "Could not check" is reported, never recorded as an answer.
  REQUIRE(report.ok == true);
  REQUIRE(report.total == 1);
  REQUIRE(report.updates == 0);
  REQUIRE_FALSE(report.message.empty());

  const auto meta = engine::ModMeta::load(tmp.root, "MyMod");
  REQUIRE(meta.get("General", "update_available").empty());
  REQUIRE(meta.get("General", "newestversion").empty());
}

TEST_CASE("loverslab sweep: a page that states nothing gets no verdict",
          "[engine][loverslab]") {
  using engine::network::FakeNetworkManager;

  const TempDir tmp("ll_nobasis");
  seed_ll_mod(tmp.root, "MyMod", "0.9", "");

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));
  engine::network::Response page;
  page.http_code = 200;
  // A page with no JSON-LD and no og: meta: nothing on it to compare against.
  page.body = "<html><head><title>Mod page</title></head><body></body></html>";
  fakeptr->enqueue_response(page);

  engine::Source::LoversLab::Provider provider;
  engine::Source::UpdateSweepContext ctx;
  ctx.mods_dir      = tmp.root;
  const auto report = provider.check_updates(ctx);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);
  REQUIRE(report.total == 1);
  REQUIRE(report.updates == 0);
  const auto meta = engine::ModMeta::load(tmp.root, "MyMod");
  REQUIRE(meta.get("General", "update_available").empty());
}

TEST_CASE("loverslab sweep: a mod with no page provenance is not counted",
          "[engine][loverslab]") {
  const TempDir tmp("ll_noprovenance");
  std::error_code ec;
  std::filesystem::create_directories(tmp.root / "Legacy", ec);
  std::ofstream out(tmp.root / "Legacy" / "meta.ini");
  out << "[General]\nversion=1.0\n\n[GameModManager]\nsource_type = loverslab\n";

  engine::Source::LoversLab::Provider provider;
  engine::Source::UpdateSweepContext ctx;
  ctx.mods_dir      = tmp.root;
  const auto report = provider.check_updates(ctx);

  REQUIRE(report.ok == true);
  REQUIRE(report.total == 0);
  REQUIRE(report.updates == 0);
}

TEST_CASE("modpub sweep: a readable page becomes a verdict", "[engine][modpub]") {
  using engine::network::FakeNetworkManager;

  const TempDir tmp("mp_ok");
  std::error_code ec;
  std::filesystem::create_directories(tmp.root / "PubMod", ec);
  {
    std::ofstream out(tmp.root / "PubMod" / "meta.ini");
    out << "[General]\n"
        << "version=1.0\n"
        << "\n[GameModManager]\n"
        << "source_type = modpub\n"
        << "\n[ModPub]\n"
        << "mod_id=42\n"
        << "page_url=https://mod.pub/somegame/42/mod-name/\n"
        << "date_modified=2025-01-01T00:00:00+0000\n";
  }

  auto fake     = std::make_unique<FakeNetworkManager>();
  auto *fakeptr = fake.get();
  engine::network::set_instance(std::move(fake));
  engine::network::Response page;
  page.http_code = 200;
  // The JSON-LD shape mod.pub pages carry (transcribed from the documented
  // schema.org SoftwareApplication block the parser reads - ModPub's finder
  // matches @type SoftwareApplication, NOT the WebApplication shape Invision
  // pages like LoversLab emit). The parse gate requires name AND description,
  // so both are stated, as real pages state them.
  page.body = "<html><head><script type=\"application/ld+json\">"
              "{\"@type\":\"SoftwareApplication\",\"name\":\"PubMod\","
              "\"description\":\"A mod on mod.pub\",\"softwareVersion\":\"2.5\","
              "\"dateModified\":\"2026-01-15T10:00:00+0000\"}"
              "</script></head><body></body></html>";
  fakeptr->enqueue_response(page);

  engine::Source::ModPub::Provider provider;
  engine::Source::UpdateSweepContext ctx;
  ctx.mods_dir      = tmp.root;
  const auto report = provider.check_updates(ctx);
  engine::network::set_instance(nullptr);

  REQUIRE(report.ok == true);
  REQUIRE(report.total == 1);
  REQUIRE(report.updates == 1);
  const auto meta = engine::ModMeta::load(tmp.root, "PubMod");
  REQUIRE(meta.get("General", "update_available") == "true");
  REQUIRE(meta.get("General", "newestversion") == "2.5");
  REQUIRE(meta.get("ModPub", "date_modified") == "2026-01-15T10:00:00+0000");
}
