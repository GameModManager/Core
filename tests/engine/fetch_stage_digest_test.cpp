// A digest mismatch must not leave the bad bytes behind as a resumable
// partial. This file exists because it used to: FetchStage returned false on a
// mismatch and KEPT the .unfinished file, and the next attempt read that
// file's size as the HTTP Range offset. The staged file was already complete
// and already wrong, so the offset was EOF, the retry asked for zero bytes and
// failed identically - a retry loop that could never produce an archive.
//
// MO2's rule for the same situation, which this follows:
//   - resumeDownloadInt sends a Range header only when the download is NOT in
//     the error state, so an errored download restarts from byte 0
//     (references/modorganizer/src/downloadmanager.cpp:1082-1089);
//   - once the retries are spent it removes the partial outright
//     (downloadmanager.cpp:2429, `info->m_Output.remove()`).
// Discard and refetch - never resume a payload that has been judged bad.
#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/mod/model/mod.h"
#include "engine/source/registry.h"
#include "engine/source/update_policy.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

const char *kPayload = "these are not the bytes the pack named";

// A provider that writes a fixed payload and records the resume offset
// FetchStage handed it. The recording is the consumer proof: what the mismatch
// handling has to change is the offset the NEXT attempt downloads from, not
// merely whether the file is still there.
class TestDigestProvider final : public engine::Source::Interface {
public:
  std::string source_type() const override { return "test-digest"; }
  bool fetch(const engine::Mod &, engine::PipelineContext &ctx,
             const std::filesystem::path &dest_path) override {
    seen_dest        = dest_path.string();
    seen_resume_from = ctx.download_resume_from;
    std::ofstream(dest_path, std::ios::binary | std::ios::app) << kPayload;
    return true;
  }
  engine::Source::SourceDownloadInfo
  resolve_download_info(const engine::Mod &) const override {
    engine::Source::SourceDownloadInfo info;
    info.archive_name = "Pinned.7z";
    return info;
  }

  std::string seen_dest;
  int64_t seen_resume_from = -1;
};

// The registered provider, as the HTTP layer that would use the offset sees it.
TestDigestProvider &provider() {
  auto *iface = engine::Source::Registry::instance().provider_for("test-digest");
  REQUIRE(iface != nullptr);
  return static_cast<TestDigestProvider &>(*iface);
}

struct TempDir {
  std::filesystem::path root;
  TempDir() {
    root = std::filesystem::temp_directory_path() /
           ("gmm_fetch_digest_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

// A mod whose pin names `sha256`. Empty means "compute the payload's digest",
// which is how the accept path gets a pin it can satisfy.
engine::Mod pinned_mod(const std::string &id, const std::string &sha256) {
  engine::Mod mod;
  mod.id                   = id;
  mod.name                 = "Pinned";
  mod.download_source_type = "test-digest";
  mod.download_source_id   = id;
  mod.expected_sha256      = sha256;
  return mod;
}

}  // namespace

TEST_CASE(
    "a digest mismatch discards the partial instead of leaving a resumable orphan",
    "[engine][pipeline][fetch]") {
  engine::Source::Registry::instance().register_provider(
      std::make_unique<TestDigestProvider>());

  TempDir tmp;
  auto mods_dir = tmp.root / "mods";
  std::filesystem::create_directories(mods_dir);
  const std::filesystem::path final_archive = tmp.root / "downloads" / "Pinned.7z";
  const std::filesystem::path partial       = final_archive.string() + ".unfinished";

  // A digest no payload of this size can have. The payload is 40 bytes, so
  // "aaaa...a" (0xaa repeated) is not its sha256.
  const std::string wrong_pin(64, 'a');

  // --- Attempt 1: the payload does not match the pin. ---
  {
    engine::PipelineContext ctx;
    ctx.mods_dir = mods_dir;
    engine::FetchStage stage;
    engine::Mod mod = pinned_mod("digest-1", wrong_pin);
    REQUIRE_FALSE(stage.execute(mod, ctx));
    REQUIRE_FALSE(std::filesystem::exists(final_archive));
    // The load-bearing assertion: the known-bad bytes are gone, so nothing can
    // read them as a resume offset next time.
    REQUIRE_FALSE(std::filesystem::exists(partial));
  }

  // --- Attempt 2: the retry must download from byte 0, not resume from EOF. ---
  {
    engine::PipelineContext ctx;
    ctx.mods_dir = mods_dir;
    engine::FetchStage stage;
    engine::Mod mod = pinned_mod("digest-1", wrong_pin);
    REQUIRE_FALSE(stage.execute(mod, ctx));
    REQUIRE(provider().seen_resume_from == 0);
    REQUIRE(provider().seen_dest == partial.string());
  }

  // --- And a satisfied pin still lands on the final name. ---
  {
    // Hash exactly what the provider writes, so the archive is accepted.
    const std::string scratch = (tmp.root / "scratch.bin").string();
    {
      std::ofstream out(scratch, std::ios::binary);
      out << kPayload;
    }
    const std::string right_pin = engine::Source::compute_file_sha256(scratch);

    engine::PipelineContext ctx;
    ctx.mods_dir = mods_dir;
    engine::FetchStage stage;
    engine::Mod mod = pinned_mod("digest-1", right_pin);
    REQUIRE(stage.execute(mod, ctx));
    REQUIRE(std::filesystem::exists(final_archive));
    REQUIRE_FALSE(std::filesystem::exists(partial));
  }
}