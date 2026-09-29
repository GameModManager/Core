// Install-progress engine tests: the two-pass ArchiveExtractor percent (header
// pre-pass sums entry sizes, then bytes written are reported against it) and
// the InstallStage copy percent (files counted up front, then done/total).
// Both must report a real, monotonic 0-100% - the engine side of the
// MO2-style install progress popup.
#include "engine/mod/archive/archive_extractor.h"
#include "engine/mod/archive/sevenzip_backend.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/mod/model/mod.h"
#include "engine/core/log/logger.h"
#include "engine/core/util/process_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {

// CRC-32 (IEEE 802.3), required even for stored (uncompressed) zip entries.
std::uint32_t crc32(const std::string &data) {
  std::uint32_t table[256];
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k)
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    table[i] = c;
  }
  std::uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char b : data)
    crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

void write_u16(std::ofstream &f, std::uint16_t v) {
  f.write(reinterpret_cast<const char *>(&v), 2);
}
void write_u32(std::ofstream &f, std::uint32_t v) {
  f.write(reinterpret_cast<const char *>(&v), 4);
}

// Minimal store-only ZIP (method 0, no data descriptors). Hermetic: no
// external `zip` binary, and libarchive reads it back without issue.
bool write_store_zip(const std::filesystem::path &out,
                     const std::vector<std::pair<std::string, std::string>> &entries) {
  std::ofstream f(out, std::ios::binary);
  if (!f)
    return false;

  struct Central {
    std::uint32_t offset;
    std::string name;
    std::uint32_t crc;
    std::uint32_t size;
  };
  std::vector<Central> centrals;

  for (const auto &[name, data] : entries) {
    const std::uint32_t crc    = crc32(data);
    const std::uint32_t size   = static_cast<std::uint32_t>(data.size());
    const std::uint32_t offset = static_cast<std::uint32_t>(f.tellp());

    write_u32(f, 0x04034b50u);  // local file header signature
    write_u16(f, 20);           // version needed
    write_u16(f, 0);            // flags
    write_u16(f, 0);            // method: stored
    write_u16(f, 0);            // mod time
    write_u16(f, 0);            // mod date
    write_u32(f, crc);
    write_u32(f, size);  // compressed size
    write_u32(f, size);  // uncompressed size
    write_u16(f, static_cast<std::uint16_t>(name.size()));
    write_u16(f, 0);  // extra length
    f.write(name.data(), static_cast<std::streamsize>(name.size()));
    f.write(data.data(), static_cast<std::streamsize>(data.size()));

    centrals.push_back({offset, name, crc, size});
  }

  const std::uint32_t cd_offset = static_cast<std::uint32_t>(f.tellp());
  for (const auto &c : centrals) {
    write_u32(f, 0x02014b50u);  // central directory signature
    write_u16(f, 20);           // version made by
    write_u16(f, 20);           // version needed
    write_u16(f, 0);            // flags
    write_u16(f, 0);            // method
    write_u16(f, 0);            // mod time
    write_u16(f, 0);            // mod date
    write_u32(f, c.crc);
    write_u32(f, c.size);
    write_u32(f, c.size);
    write_u16(f, static_cast<std::uint16_t>(c.name.size()));
    write_u16(f, 0);  // extra
    write_u16(f, 0);  // comment
    write_u16(f, 0);  // disk start
    write_u16(f, 0);  // internal attrs
    write_u32(f, 0);  // external attrs
    write_u32(f, c.offset);
    f.write(c.name.data(), static_cast<std::streamsize>(c.name.size()));
  }
  const std::uint32_t cd_size = static_cast<std::uint32_t>(f.tellp()) - cd_offset;

  write_u32(f, 0x06054b50u);  // end of central directory
  write_u16(f, 0);            // disk number
  write_u16(f, 0);            // disk with cd
  write_u16(f, static_cast<std::uint16_t>(centrals.size()));
  write_u16(f, static_cast<std::uint16_t>(centrals.size()));
  write_u32(f, cd_size);
  write_u32(f, cd_offset);
  write_u16(f, 0);  // comment length
  return true;
}

struct TempDir {
  std::filesystem::path root;
  TempDir() {
    root = std::filesystem::temp_directory_path() /
           ("gmm_install_progress_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter_++));
    std::filesystem::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  static int counter_;
};
int TempDir::counter_ = 0;

// A RAR5 archive libarchive 3.8.x rejects ("Declared dictionary size is not
// supported"), so extract() falls back to the unrar CLI. Used by both fallback
// cases: the real-binary extraction and the stubbed password refusal.
void write_dict_128mib_rar(const std::filesystem::path &out) {
  // RAR5 signature + MAIN block + FILE "hello.txt" (stored, declared 128 MiB
  // dictionary) + ENDARC block.
  static const unsigned char kFixture[] = {
      0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00, 0xc5, 0x1a, 0x33, 0x32, 0x03,
      0x01, 0x00, 0x00, 0xd0, 0xee, 0xc8, 0x89, 0x17, 0x02, 0x02, 0x13, 0x04, 0x13,
      0x00, 0xb9, 0x14, 0x59, 0x8f, 0x80, 0x50, 0x00, 0x09, 0x68, 0x65, 0x6c, 0x6c,
      0x6f, 0x20, 0x72, 0x61, 0x72, 0x35, 0x20, 0x66, 0x61, 0x6c, 0x6c, 0x62, 0x61,
      0x63, 0x6b, 0x39, 0xf9, 0xb2, 0x81, 0x02, 0x05, 0x00,
  };
  std::ofstream f(out, std::ios::binary);
  f.write(reinterpret_cast<const char *>(kFixture), sizeof(kFixture));
}

// WinZip AES-256 zip holding one 2-byte file ("a.txt" -> "x\n"), encrypted
// with kFixturePassword. Embedded as bytes so the suite needs no `7z`, no
// network and no writable toolchain: writing an AES-encrypted zip by hand is
// not practical, and libarchive's zip reader is the one in this build that
// can decrypt an archive (its 7z reader reports encrypted content and
// encrypted headers as unsupported, and it cannot read encrypted RAR at all -
// which is what the unrar fallback below exists for).
const char kFixturePassword[] = "GMMtestPass1";
const unsigned char kEncryptedZip[] = {
    0x50, 0x4b, 0x03, 0x04, 0x33, 0x00, 0x01, 0x00, 0x63, 0x00, 0x13, 0x52,
    0x3d, 0x5d, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x0b, 0x00, 0x61, 0x2e, 0x74, 0x78, 0x74, 0x01,
    0x99, 0x07, 0x00, 0x02, 0x00, 0x41, 0x45, 0x03, 0x00, 0x00, 0xc3, 0xff,
    0x43, 0xe5, 0xdb, 0xa6, 0xd7, 0x23, 0xe3, 0x91, 0x6d, 0x44, 0x31, 0xeb,
    0x96, 0x18, 0x96, 0xda, 0xe1, 0xfe, 0x72, 0xd4, 0x08, 0xa1, 0x04, 0x02,
    0x0d, 0x39, 0xdb, 0x7d, 0x50, 0x4b, 0x01, 0x02, 0x3f, 0x03, 0x33, 0x00,
    0x01, 0x00, 0x63, 0x00, 0x13, 0x52, 0x3d, 0x5d, 0x00, 0x00, 0x00, 0x00,
    0x1e, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x2f, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x80, 0xa4, 0x81, 0x00, 0x00,
    0x00, 0x00, 0x61, 0x2e, 0x74, 0x78, 0x74, 0x0a, 0x00, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x5c, 0x52, 0xde, 0x76, 0xe2,
    0x4f, 0xdd, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x99, 0x07, 0x00, 0x02,
    0x00, 0x41, 0x45, 0x03, 0x00, 0x00, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x62, 0x00, 0x00, 0x00, 0x4c, 0x00,
    0x00, 0x00, 0x00, 0x00};

void write_encrypted_zip(const std::filesystem::path &out) {
  std::ofstream f(out, std::ios::binary);
  f.write(reinterpret_cast<const char *>(kEncryptedZip), sizeof(kEncryptedZip));
}

// Records every log line written while it is alive. Logger has no
// remove_callback, so a registered callback outlives the object that registered
// it and is invoked by every later test; the state is therefore held through a
// shared_ptr and switched off on scope exit, rather than relying on the
// LogCapture object still existing when the next test logs something.
struct LogCapture {
  struct State {
    std::vector<std::string> lines;
    bool active = true;
  };

  explicit LogCapture(const std::filesystem::path &dir) {
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

TEST_CASE("install progress", "[engine]") {
  // (a) ArchiveExtractor two-pass progress: the pre-pass sums entry sizes, so
  // the callback reports done against the real total, monotonically to 100%.
  {
    TempDir tmp;
    auto archive = tmp.root / "mod.zip";
    const std::vector<std::pair<std::string, std::string>> entries = {
        {"textures/", ""},       {"textures/a.dds", std::string(65536, 'a')},
        {"meshes/", ""},         {"meshes/b.nif", std::string(131072, 'b')},
        {"readme.txt", "hello"},
    };
    const bool zipped = write_store_zip(archive, entries);
    REQUIRE(zipped);

    std::vector<engine::ExtractedFile> files;
    std::string error;
    std::vector<std::pair<std::int64_t, std::int64_t>> progress;
    const std::int64_t expected_total = 65536 + 131072 + 5;
    const bool extracted =
        engine::ArchiveExtractor::extract(archive, tmp.root / "out", files, error,
                                          [&](std::int64_t done, std::int64_t total) {
                                            progress.emplace_back(done, total);
                                          });
    REQUIRE(extracted);
    REQUIRE(error.empty());
    REQUIRE(files.size() == 3);  // directory entries are not extracted files
    REQUIRE(std::filesystem::exists(tmp.root / "out" / "textures" / "a.dds"));
    REQUIRE(std::filesystem::exists(tmp.root / "out" / "meshes" / "b.nif"));
    REQUIRE(std::filesystem::exists(tmp.root / "out" / "readme.txt"));

    REQUIRE(!progress.empty());
    std::int64_t last_done = -1;
    for (const auto &[done, total] : progress) {
      REQUIRE(total == expected_total);
      REQUIRE(done >= last_done);
      last_done = done;
    }
    REQUIRE(progress.back().first == expected_total);
    std::printf(
        "PASS: install_progress — extractor two-pass reached %lld of %lld bytes\n",
        static_cast<long long>(progress.back().first),
        static_cast<long long>(expected_total));
  }

  // (b) InstallStage copy progress: on_stage_progress is monotonic and ends
  // at 100%, with the "Installing to <folder>…" status line.
  {
    TempDir tmp;
    auto staging = tmp.root / "staging";
    auto mods    = tmp.root / "mods";
    std::filesystem::create_directories(staging / "sub");
    for (int i = 0; i < 40; ++i) {
      std::ofstream(staging / ("file" + std::to_string(i) + ".txt"))
          << std::string(256, 'x');
    }
    std::ofstream(staging / "sub" / "nested.bin") << std::string(1024, 'n');
    std::filesystem::create_directories(mods);

    engine::Mod mod;
    mod.id      = "pm";
    mod.name    = "Progress Mod";
    mod.version = "1.0";
    mod.state   = engine::ModState::Extracted;
    engine::ModFile f;
    f.relative_path = staging.string();
    mod.files.push_back(f);

    engine::PipelineContext ctx;
    ctx.mods_dir = mods;
    std::vector<int> percents;
    std::vector<std::string> statuses;
    ctx.on_stage_progress = [&](int percent, const std::string &status) {
      percents.push_back(percent);
      statuses.push_back(status);
    };

    engine::InstallStage stage;
    const bool executed = stage.execute(mod, ctx);
    REQUIRE(executed);
    REQUIRE(mod.state == engine::ModState::Installed);
    REQUIRE(std::filesystem::exists(mods / "Progress Mod" / "file0.txt"));
    REQUIRE(std::filesystem::exists(mods / "Progress Mod" / "sub" / "nested.bin"));

    REQUIRE(!percents.empty());
    int last = -1;
    for (int p : percents) {
      REQUIRE(p >= last);
      last = p;
    }
    REQUIRE(percents.back() == 100);
    REQUIRE(!statuses.empty());
    REQUIRE(statuses.front().find("Installing to Progress Mod") != std::string::npos);
    std::printf("PASS: install_progress — InstallStage copy reported %d%%\n",
                percents.back());
  }

  // (c) A RAR libarchive cannot read is a case the 7-Zip backend owns, and
  // lives in sevenzip_archive_test. What is asserted here is the property that
  // still holds on this path: an archive that yields no entries is a failure
  // with a reason, never a successful install that installed nothing. The
  // 61-byte RAR5 below is truncated - 7-Zip's own CLI rejects it - and it
  // reaches the 7-Zip reader because routing is by content, not extension.
  {
    TempDir tmp;
    auto archive = tmp.root / "dict128mib.rar";
    write_dict_128mib_rar(archive);
    REQUIRE(engine::is_rar_archive(archive));
    REQUIRE(engine::route_archive(archive) == engine::ArchiveEngine::kSevenZip);

    std::vector<engine::ExtractedFile> files;
    std::string error;
    REQUIRE_FALSE(engine::ArchiveExtractor::extract(archive, tmp.root / "out", files,
                                                    error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(files.empty());
    std::printf("PASS: install_progress — a corrupt RAR5 fails with a reason\n");
  }
}

TEST_CASE("archive password", "[engine]") {
  // (c) Encrypted archive, correct password: the reader asks, the answer
  // installs the contents, and the user is asked exactly once no matter how
  // many encrypted entries the archive has.
  {
    TempDir tmp;
    auto archive = tmp.root / "secret.zip";
    write_encrypted_zip(archive);

    int prompts = 0;
    std::vector<std::string> archived_name;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = true;
    const bool extracted = engine::ArchiveExtractor::extract(
        archive, tmp.root / "out", files, error, {}, /*low_priority=*/false,
        [&](const std::string &name, std::string &passphrase) {
          ++prompts;
          archived_name.push_back(name);
          passphrase = kFixturePassword;
          return true;
        },
        &canceled);

    REQUIRE(extracted);
    REQUIRE_FALSE(canceled);
    REQUIRE(error.empty());
    REQUIRE(prompts == 1);
    REQUIRE(archived_name.size() == 1);
    REQUIRE(archived_name[0] == "secret.zip");
    REQUIRE(files.size() == 1);
    REQUIRE(files[0].archive_path == "a.txt");
    std::ifstream in(tmp.root / "out" / "a.txt", std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    REQUIRE(content == "x\n");
    // A wrong password is what an unencrypted archive must never do: this
    // path would fail loudly if libarchive asked about one it does not need.
    REQUIRE(error.find(kFixturePassword) == std::string::npos);
    std::printf("PASS: install_progress — encrypted zip installed after %d prompt\n",
                prompts);
  }

  // (c2) Wrong password: the install asks again instead of failing, and the
  // second answer is the one that is used. This is the retry - a mistyped
  // password must not cost the user the whole install.
  {
    TempDir tmp;
    auto archive = tmp.root / "secret.zip";
    write_encrypted_zip(archive);

    int prompts = 0;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = true;
    const bool extracted = engine::ArchiveExtractor::extract(
        archive, tmp.root / "out", files, error, {}, /*low_priority=*/false,
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
    std::printf("PASS: install_progress — wrong password re-prompted and recovered\n");
  }

  // (c3) The retry is bounded. A password that is never right must stop after
  // a few asks and report a real reason - not loop, and not open thousands of
  // prompts (libarchive itself calls back 10000 times before giving up).
  {
    TempDir tmp;
    auto archive = tmp.root / "secret.zip";
    write_encrypted_zip(archive);

    int prompts = 0;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = true;
    LogCapture log(tmp.root);
    const bool extracted = engine::ArchiveExtractor::extract(
        archive, tmp.root / "out", files, error, {}, /*low_priority=*/false,
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
    // The reason names the failure without repeating either password back.
    REQUIRE(error.find("always-wrong") == std::string::npos);
    REQUIRE(error.find(kFixturePassword) == std::string::npos);
    REQUIRE(files.empty());
    std::printf("PASS: install_progress — wrong password stopped after %d prompts\n",
                prompts);
  }

  // (c4) Dismissing the prompt cancels. There is nothing to report, so `error`
  // stays empty and the caller is told it was a cancel - the difference
  // between "I changed my mind" and "the install broke".
  {
    TempDir tmp;
    auto archive = tmp.root / "secret.zip";
    write_encrypted_zip(archive);

    int prompts = 0;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = false;
    const bool extracted = engine::ArchiveExtractor::extract(
        archive, tmp.root / "out", files, error, {}, /*low_priority=*/false,
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
    std::printf("PASS: install_progress — dismissed prompt reported as a cancel\n");
  }

  // (c5) An unencrypted archive never asks. The prompt callback here would
  // hand over a password that does not fit, so a false ask fails the install
  // loudly instead of passing by accident.
  {
    TempDir tmp;
    auto archive = tmp.root / "plain.zip";
    REQUIRE(write_store_zip(archive, {{"readme.txt", "hello"}}));

    int prompts = 0;
    std::vector<engine::ExtractedFile> files;
    std::string error;
    bool canceled = true;
    const bool extracted = engine::ArchiveExtractor::extract(
        archive, tmp.root / "out", files, error, {}, /*low_priority=*/false,
        [&](const std::string &, std::string &) {
          ++prompts;
          return false;
        },
        &canceled);

    REQUIRE(extracted);
    REQUIRE_FALSE(canceled);
    REQUIRE(prompts == 0);
    REQUIRE(files.size() == 1);
    std::printf("PASS: install_progress — unencrypted archive asked nothing\n");
  }

  // (c7) Nothing the user can see or the operator can grep for ever contains
  // the password. The stage logs the extractor's own diagnostic and the UI
  // shows it verbatim, so the error string and the log are the two channels
  // that have to be clean. Driven through an encrypted RAR5 with a wrong
  // password, which is the path that reaches 7-Zip and spends the retry budget.
  {
    TempDir tmp;
    auto archive = tmp.root / "encrypted.rar";
    std::filesystem::copy_file(
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "sevenzip" /
            "encrypted.rar",
        archive);

    engine::Mod mod;
    mod.id    = "leaky";
    mod.name  = "Leaky";
    mod.state = engine::ModState::Downloaded;
    engine::ModFile entry;
    entry.relative_path = archive.string();
    mod.files.push_back(entry);

    engine::PipelineContext ctx;
    ctx.mods_dir         = tmp.root / "mods";
    std::filesystem::create_directories(ctx.mods_dir);
    // Deliberately wrong: the fixture is sealed with a different password, so
    // the install runs the whole retry budget and then fails, which is the case
    // where a password is most likely to leak into a message.
    ctx.passphrase_query_cb = [](const std::string &, std::string &passphrase) {
      passphrase = "wrong-on-purpose";
      return true;
    };

    LogCapture log(tmp.root);
    engine::ExtractStage stage;
    const bool executed = stage.execute(mod, ctx);

    REQUIRE_FALSE(executed);
    // The install failed, it was not cancelled, and the user is told the
    // password was not accepted - never what it was.
    REQUIRE_FALSE(ctx.canceled);
    REQUIRE(ctx.error_message.find("wrong-on-purpose") == std::string::npos);
    REQUIRE(ctx.error_message.find(kFixturePassword) == std::string::npos);
    REQUIRE_FALSE(ctx.error_message.empty());
    REQUIRE(log.contains("ExtractStage: extraction failed"));
    REQUIRE_FALSE(log.contains("wrong-on-purpose"));
    REQUIRE_FALSE(log.contains(kFixturePassword));
    std::printf("PASS: install_progress — password absent from reason and log\n");
  }
}
