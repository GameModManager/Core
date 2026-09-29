// 7-Zip IArchive backend: routing, and the RAR cases libarchive cannot read.
//
// The fixtures under fixtures/sevenzip/ are hand-built RAR5 archives generated
// by the scripts beside them and validated against real unrar and real 7z.
// Three of them declare a dictionary libarchive's RAR5 reader refuses outright
// ("Declared dictionary size is not supported"), which is the whole reason this
// backend exists, so every assertion below is a case libarchive cannot serve.
#include "engine/mod/archive/archive_extractor.h"
#include "engine/mod/archive/sevenzip_backend.h"
#include "engine/core/log/logger.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/mod/model/mod.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// The password the encrypted fixture is sealed with. Asserted absent from
// every observable channel in the security test below.
const char kFixturePassword[] = "correct horse battery staple";

// The exact bytes each fixture's single entry carries, as the generator scripts
// beside them build them. Comparing against a literal that matches the fixture
// is what makes the extraction check worth anything: it would catch a decoder
// that produced the right length and the wrong content.
std::string repeated(const char *line) {
  std::string s;
  for (int i = 0; i < 64; ++i)
    s += line;
  return s;
}
const std::string kStoredPayload  = repeated("GMM 7-Zip backend fixture - RAR5 stored file.\n");
const std::string kEncryptedPayload = repeated("GMM 7-Zip backend fixture - ENCRYPTED RAR5 file.\n");

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "sevenzip" / name;
}

std::string read_file(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Self-cleaning temp dir: a REQUIRE that aborts the test still runs this
// destructor, so an abort leaves nothing behind.
struct TempDir {
  std::filesystem::path root;
  TempDir() {
    root = std::filesystem::temp_directory_path() /
           ("gmm_7zip_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++));
    std::filesystem::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  static int counter_;
};
int TempDir::counter_ = 0;

// Records every log line written while alive, so "the password never reaches
// the log" is an assertion rather than a claim. Logger has no remove_callback,
// so a registered callback outlives the object that registered it; the state is
// therefore held through a shared_ptr and the capture is switched off on scope
// exit rather than relying on the LogCapture object still existing.
struct LogCapture {
  struct State {
    std::vector<std::string> lines;
    bool active = true;
  };

  LogCapture() {
    state_ = std::make_shared<State>();
    engine::Logger::instance().add_callback(
        [state = state_](engine::LogLevel, const std::string &, const std::string &message) {
          if (state->active)
            state->lines.push_back(message);
        });
  }
  ~LogCapture() { state_->active = false; }
  bool contains(const std::string &needle) const {
    return std::any_of(state_->lines.begin(), state_->lines.end(), [&](const std::string &line) {
      return line.find(needle) != std::string::npos;
    });
  }

  std::shared_ptr<State> state_;
};

}  // namespace

// The routing table, asserted rather than implied. Each case names the format
// and the engine it must land on; a change to the table breaks this loudly
// instead of quietly moving a format onto the wrong reader.
TEST_CASE("archive routing", "[engine][7zip]") {
  TempDir tmp;

  SECTION("RAR goes to 7-Zip") {
    const auto rar5 = fixture("big_dict.rar");
    REQUIRE(engine::route_archive(rar5) == engine::ArchiveEngine::kSevenZip);
    REQUIRE(engine::sevenzip_handler_for(rar5) == "Rar5");
  }

  SECTION("an encrypted RAR5 is identified the same way") {
    const auto enc = fixture("encrypted.rar");
    REQUIRE(engine::route_archive(enc) == engine::ArchiveEngine::kSevenZip);
    REQUIRE(engine::sevenzip_handler_for(enc) == "Rar5");
  }

  SECTION("7z goes to 7-Zip") {
    // The 6-byte 7z magic, which is what route_archive keys on. Written here
    // rather than shipped as a fixture because the routing decision only reads
    // the signature.
    const auto seven = tmp.root / "sig.7z";
    {
      std::ofstream f(seven, std::ios::binary);
      f.write("7z\xBC\xAF\x27\x1C", 6);
      f << std::string(64, 'x');
    }
    REQUIRE(engine::route_archive(seven) == engine::ArchiveEngine::kSevenZip);
    REQUIRE(engine::sevenzip_handler_for(seven) == "7z");
  }

  SECTION("everything else stays on libarchive") {
    // The incumbent, and the formats it already handles. Written as signatures
    // and one zip, because routing must not depend on a file being a complete,
    // valid archive.
    const auto zip = tmp.root / "plain.zip";
    {
      std::ofstream f(zip, std::ios::binary);
      f << "PK\x03\x04" << std::string(64, 'x');
    }
    REQUIRE(engine::route_archive(zip) == engine::ArchiveEngine::kLibarchive);
    REQUIRE(engine::sevenzip_handler_for(zip).empty());

    const auto tar = tmp.root / "plain.tar";
    {
      std::ofstream f(tar, std::ios::binary);
      f.write("ustar", 5);
      f << std::string(64, 'x');
    }
    REQUIRE(engine::route_archive(tar) == engine::ArchiveEngine::kLibarchive);
  }

  SECTION("a truncated or absent file routes to libarchive, not to a guess") {
    REQUIRE(engine::route_archive(tmp.root / "does-not-exist.rar") ==
            engine::ArchiveEngine::kLibarchive);
    const auto empty = tmp.root / "empty.rar";
    { std::ofstream f(empty, std::ios::binary); }
    REQUIRE(engine::route_archive(empty) == engine::ArchiveEngine::kLibarchive);
  }

  SECTION("routing is by content, not by filename") {
    // A RAR named .zip is still a RAR. Routing on the extension would send
    // this to libarchive and lose the large-dictionary case.
    const auto misnamed = tmp.root / "actually-rar.zip";
    std::filesystem::copy_file(fixture("big_dict.rar"), misnamed);
    REQUIRE(engine::route_archive(misnamed) == engine::ArchiveEngine::kSevenZip);
  }
}

// The case the whole migration exists for: a RAR5 declaring a dictionary
// larger than 64 MiB, which libarchive refuses with "Declared dictionary size
// is not supported". 4 GiB is the far end of the same axis.
TEST_CASE("RAR5 with a large dictionary extracts through 7-Zip", "[engine][7zip]") {
  for (const char *name : {"big_dict.rar", "huge_dict.rar"}) {
    CAPTURE(name);
    TempDir tmp;
    const auto archive = fixture(name);
    REQUIRE(engine::route_archive(archive) == engine::ArchiveEngine::kSevenZip);

    std::vector<engine::ExtractedFile> files;
    std::string error;
    std::vector<std::pair<std::int64_t, std::int64_t>> progress;
    const bool extracted =
        engine::ArchiveExtractor::extract(archive, tmp.root / "out", files, error,
                                          [&](std::int64_t done, std::int64_t total) {
                                            progress.emplace_back(done, total);
                                          },
                                          /*low_priority=*/false);

    REQUIRE(extracted);
    REQUIRE(error.empty());
    REQUIRE(files.size() == 1);
    REQUIRE(files[0].archive_path == "hello.txt");
    REQUIRE(std::filesystem::exists(tmp.root / "out" / "hello.txt"));
    // Content matches what unrar and 7z independently extract from the same
    // fixture, so this is not "7-Zip read its own output".
    REQUIRE(read_file(tmp.root / "out" / "hello.txt") == kStoredPayload);

    // Progress is real, monotonic, and ends at the declared total.
    REQUIRE(!progress.empty());
    std::int64_t last = -1;
    for (const auto &[done, total] : progress) {
      REQUIRE(total == static_cast<std::int64_t>(kStoredPayload.size()));
      REQUIRE(done >= last);
      last = done;
    }
    REQUIRE(progress.back().first == static_cast<std::int64_t>(kStoredPayload.size()));
  }
}

// The security property: the password reaches the reader in this process and
// nowhere else. The unrar fallback used to put it in a child's argv, readable
// from /proc for the whole extraction.
TEST_CASE("an encrypted RAR5 extracts with an in-process password",
          "[engine][7zip]") {
  TempDir tmp;
  const auto archive = fixture("encrypted.rar");
  REQUIRE(engine::route_archive(archive) == engine::ArchiveEngine::kSevenZip);

  int prompts = 0;
  std::vector<std::string> prompted_name;
  std::vector<engine::ExtractedFile> files;
  std::string error;
  bool canceled = true;
  LogCapture log;

  const bool extracted = engine::ArchiveExtractor::extract(
      archive, tmp.root / "out", files, error, {}, /*low_priority=*/false,
      [&](const std::string &name, std::string &passphrase) {
        ++prompts;
        prompted_name.push_back(name);
        passphrase = kFixturePassword;
        return true;
      },
      &canceled);

  REQUIRE(extracted);
  REQUIRE_FALSE(canceled);
  REQUIRE(error.empty());
  REQUIRE(prompts >= 1);
  REQUIRE(prompted_name.front() == "encrypted.rar");
  REQUIRE(files.size() == 1);
  REQUIRE(files[0].archive_path == "secret.txt");
  REQUIRE(read_file(tmp.root / "out" / "secret.txt") == kEncryptedPayload);

  // The security argument, asserted rather than asserted-about. Four channels:
  // the reason the UI shows, the log the operator greps, this process's argv,
  // and its environment block. The last two are where the unrar fallback put
  // it; there is no subprocess here at all, so they are checked to prove it.
  REQUIRE(error.find(kFixturePassword) == std::string::npos);
  REQUIRE_FALSE(log.contains(kFixturePassword));
  REQUIRE(std::string(read_file("/proc/self/cmdline")).find(kFixturePassword) ==
          std::string::npos);
  REQUIRE(std::string(read_file("/proc/self/environ")).find(kFixturePassword) ==
          std::string::npos);

  std::printf("PASS: sevenzip — encrypted RAR5 installed, password never left the process\n");
}

// The retry budget and the cancel outcome must survive the engine change: they
// were built for the libarchive path and are shared.
TEST_CASE("encrypted RAR5 retries and cancels", "[engine][7zip]") {
  SECTION("a mistyped password is asked again and then succeeds") {
    TempDir tmp;
    int prompts = 0;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = true;
    const bool extracted = engine::ArchiveExtractor::extract(
        fixture("encrypted.rar"), tmp.root / "out", files, error, {},
        /*low_priority=*/false,
        [&](const std::string &, std::string &passphrase) {
          ++prompts;
          passphrase = (prompts == 1) ? "typo" : kFixturePassword;
          return true;
        },
        &canceled);

    REQUIRE(extracted);
    REQUIRE_FALSE(canceled);
    REQUIRE(prompts == 2);
    REQUIRE(files.size() == 1);
  }

  SECTION("the retry is bounded and never repeats a password back") {
    TempDir tmp;
    int prompts = 0;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = true;
    LogCapture log;
    const bool extracted = engine::ArchiveExtractor::extract(
        fixture("encrypted.rar"), tmp.root / "out", files, error, {},
        /*low_priority=*/false,
        [&](const std::string &, std::string &passphrase) {
          ++prompts;
          passphrase = "always-wrong";
          return true;
        },
        &canceled);

    REQUIRE_FALSE(extracted);
    REQUIRE_FALSE(canceled);
    REQUIRE(prompts > 1);
    REQUIRE(prompts <= 5);
    REQUIRE_FALSE(error.empty());
    REQUIRE(files.empty());
    REQUIRE(error.find("always-wrong") == std::string::npos);
    REQUIRE(error.find(kFixturePassword) == std::string::npos);
    REQUIRE_FALSE(log.contains("always-wrong"));
    REQUIRE_FALSE(log.contains(kFixturePassword));
  }

  SECTION("dismissing the prompt is a cancel, and leaves nothing on disk") {
    TempDir tmp;
    int prompts = 0;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = false;
    const bool extracted = engine::ArchiveExtractor::extract(
        fixture("encrypted.rar"), tmp.root / "out", files, error, {},
        /*low_priority=*/false,
        [&](const std::string &, std::string &) {
          ++prompts;
          return false;  // the user closed the dialog
        },
        &canceled);

    REQUIRE_FALSE(extracted);
    REQUIRE(canceled);
    REQUIRE(prompts == 1);
    REQUIRE(error.empty());
    REQUIRE(files.empty());
    REQUIRE(!std::filesystem::exists(tmp.root / "out" / "secret.txt"));
  }

  SECTION("headless with no prompt available fails with a reason") {
    TempDir tmp;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = true;
    const bool extracted = engine::ArchiveExtractor::extract(
        fixture("encrypted.rar"), tmp.root / "out", files, error, {},
        /*low_priority=*/false, {}, &canceled);

    REQUIRE_FALSE(extracted);
    REQUIRE_FALSE(canceled);
    REQUIRE_FALSE(error.empty());
    REQUIRE(error.find("password") != std::string::npos);
    REQUIRE(files.empty());
  }
}

// The traversal guard is ours, not 7-Zip's: 7-Zip hands "../escape.txt" and
// "/etc/gmm-probe" back verbatim, and neither archive is one libarchive could
// reach, so this is only testable through the new path.
TEST_CASE("a traversal entry name is refused", "[engine][7zip]") {
  for (const char *name : {"traversal.rar", "abs.rar"}) {
    CAPTURE(name);
    TempDir tmp;
    const auto archive = fixture(name);
    const auto sentinel = tmp.root / "escape.txt";

    std::vector<engine::ExtractedFile> files;
    std::string error;
    const bool extracted = engine::ArchiveExtractor::extract(
        archive, tmp.root / "out", files, error, {}, /*low_priority=*/false);

    REQUIRE_FALSE(extracted);
    REQUIRE_FALSE(error.empty());
    REQUIRE(error.find("unsafe path") != std::string::npos);
    REQUIRE(files.empty());
    // Nothing escaped: neither a "../" write into the temp root nor an
    // absolute write outside it.
    REQUIRE(!std::filesystem::exists(sentinel));
    REQUIRE(!std::filesystem::exists("/etc/gmm-probe"));
  }
}

// The password has to survive the whole pipeline, not just the extractor, and
// the staging dir the stage creates has to be cleaned up on a cancel.
TEST_CASE("the install pipeline reads an encrypted RAR5", "[engine][7zip]") {
  TempDir tmp;
  auto archive = tmp.root / "encrypted.rar";
  std::filesystem::copy_file(fixture("encrypted.rar"), archive);

  engine::Mod mod;
  mod.id    = "enc";
  mod.name  = "Encrypted Mod";
  mod.state = engine::ModState::Downloaded;
  engine::ModFile entry;
  entry.relative_path = archive.string();
  mod.files.push_back(entry);

  engine::PipelineContext ctx;
  ctx.mods_dir = tmp.root / "mods";
  std::filesystem::create_directories(ctx.mods_dir);
  ctx.low_priority_extraction = false;
  ctx.passphrase_query_cb = [](const std::string &, std::string &passphrase) {
    passphrase = kFixturePassword;
    return true;
  };

  LogCapture log;
  engine::ExtractStage stage;
  REQUIRE(stage.execute(mod, ctx));
  // ExtractStage stops at the staging directory; InstallStage is what moves the
  // files into the mods tree, so the whole path is exercised rather than half
  // of it.
  engine::InstallStage install;
  REQUIRE(install.execute(mod, ctx));

  // InstallStage names the folder after the mod's display name, which
  // ExtractStage left alone because the test set one.
  const auto installed = ctx.mods_dir / "Encrypted Mod" / "secret.txt";
  REQUIRE(std::filesystem::exists(installed));
  REQUIRE(read_file(installed) == kEncryptedPayload);
  // The routing decision is logged, so "which engine read this" is answerable
  // from the log without re-deriving it.
  REQUIRE(log.contains("7-Zip"));
  REQUIRE_FALSE(log.contains(kFixturePassword));
}
