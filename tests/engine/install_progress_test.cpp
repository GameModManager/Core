// Install-progress engine tests: the two-pass ArchiveExtractor percent (header
// pre-pass sums entry sizes, then bytes written are reported against it) and
// the InstallStage copy percent (files counted up front, then done/total).
// Both must report a real, monotonic 0-100% - the engine side of the
// MO2-style install progress popup.
#include "engine/mod/archive/archive_extractor.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/mod/model/mod.h"
#include "engine/core/util/process_utils.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {

// CRC-32 (IEEE 802.3), required even for stored (uncompressed) zip entries.
std::uint32_t crc32(const std::string& data) {
    std::uint32_t table[256];
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data) crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void write_u16(std::ofstream& f, std::uint16_t v) {
    f.write(reinterpret_cast<const char*>(&v), 2);
}
void write_u32(std::ofstream& f, std::uint32_t v) {
    f.write(reinterpret_cast<const char*>(&v), 4);
}

// Minimal store-only ZIP (method 0, no data descriptors). Hermetic: no
// external `zip` binary, and libarchive reads it back without issue.
bool write_store_zip(const std::filesystem::path& out,
                     const std::vector<std::pair<std::string, std::string>>& entries) {
    std::ofstream f(out, std::ios::binary);
    if (!f) return false;

    struct Central {
        std::uint32_t offset;
        std::string name;
        std::uint32_t crc;
        std::uint32_t size;
    };
    std::vector<Central> centrals;

    for (const auto& [name, data] : entries) {
        const std::uint32_t crc = crc32(data);
        const std::uint32_t size = static_cast<std::uint32_t>(data.size());
        const std::uint32_t offset = static_cast<std::uint32_t>(f.tellp());

        write_u32(f, 0x04034b50u);  // local file header signature
        write_u16(f, 20);           // version needed
        write_u16(f, 0);            // flags
        write_u16(f, 0);            // method: stored
        write_u16(f, 0);            // mod time
        write_u16(f, 0);            // mod date
        write_u32(f, crc);
        write_u32(f, size);         // compressed size
        write_u32(f, size);         // uncompressed size
        write_u16(f, static_cast<std::uint16_t>(name.size()));
        write_u16(f, 0);            // extra length
        f.write(name.data(), static_cast<std::streamsize>(name.size()));
        f.write(data.data(), static_cast<std::streamsize>(data.size()));

        centrals.push_back({offset, name, crc, size});
    }

    const std::uint32_t cd_offset = static_cast<std::uint32_t>(f.tellp());
    for (const auto& c : centrals) {
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
        write_u16(f, 0);            // extra
        write_u16(f, 0);            // comment
        write_u16(f, 0);            // disk start
        write_u16(f, 0);            // internal attrs
        write_u32(f, 0);            // external attrs
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
    write_u16(f, 0);            // comment length
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
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    static int counter_;
};
int TempDir::counter_ = 0;

}  // namespace

TEST_CASE("install progress", "[engine]") {
    // (a) ArchiveExtractor two-pass progress: the pre-pass sums entry sizes, so
    // the callback reports done against the real total, monotonically to 100%.
    {
        TempDir tmp;
        auto archive = tmp.root / "mod.zip";
        const std::vector<std::pair<std::string, std::string>> entries = {
            {"textures/", ""},
            {"textures/a.dds", std::string(65536, 'a')},
            {"meshes/", ""},
            {"meshes/b.nif", std::string(131072, 'b')},
            {"readme.txt", "hello"},
        };
        const bool zipped = write_store_zip(archive, entries);
        REQUIRE(zipped);

        std::vector<engine::ExtractedFile> files;
        std::string error;
        std::vector<std::pair<std::int64_t, std::int64_t>> progress;
        const std::int64_t expected_total = 65536 + 131072 + 5;
        const bool extracted = engine::ArchiveExtractor::extract(
            archive, tmp.root / "out", files, error,
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
        for (const auto& [done, total] : progress) {
            REQUIRE(total == expected_total);
            REQUIRE(done >= last_done);
            last_done = done;
        }
        REQUIRE(progress.back().first == expected_total);
        std::printf("PASS: install_progress — extractor two-pass reached %lld of %lld bytes\n",
                    static_cast<long long>(progress.back().first),
                    static_cast<long long>(expected_total));
    }

    // (b) InstallStage copy progress: on_stage_progress is monotonic and ends
    // at 100%, with the "Installing to <folder>…" status line.
    {
        TempDir tmp;
        auto staging = tmp.root / "staging";
        auto mods = tmp.root / "mods";
        std::filesystem::create_directories(staging / "sub");
        for (int i = 0; i < 40; ++i) {
            std::ofstream(staging / ("file" + std::to_string(i) + ".txt"))
                << std::string(256, 'x');
        }
        std::ofstream(staging / "sub" / "nested.bin") << std::string(1024, 'n');
        std::filesystem::create_directories(mods);

        engine::Mod mod;
        mod.id = "pm";
        mod.name = "Progress Mod";
        mod.version = "1.0";
        mod.state = engine::ModState::Extracted;
        engine::ModFile f;
        f.relative_path = staging.string();
        mod.files.push_back(f);

        engine::PipelineContext ctx;
        ctx.mods_dir = mods;
        std::vector<int> percents;
        std::vector<std::string> statuses;
        ctx.on_stage_progress = [&](int percent, const std::string& status) {
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

    // (c) RAR fallback regression: libarchive 3.8.x rejects RAR5 archives whose
    // declared dictionary exceeds 64 MiB ("Declared dictionary size is not
    // supported") - a routine reality for WinRAR-made mod archives - so
    // extract() must fall back to the unrar CLI. The fixture bytes below were
    // validated against bsdtar (rejects with exactly that error) and unrar 7.x
    // (extracts cleanly), so succeeding here proves the fallback was taken,
    // not that libarchive happened to read the archive.
    {
        TempDir tmp;
        auto archive = tmp.root / "dict128mib.rar";
        {
            std::ofstream f(archive, std::ios::binary);
            // RAR5 signature + MAIN block + FILE "hello.txt" (stored, declared
            // 128 MiB dictionary) + ENDARC block.
            static const unsigned char kFixture[] = {
                0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00,
                0xc5, 0x1a, 0x33, 0x32, 0x03, 0x01, 0x00, 0x00,
                0xd0, 0xee, 0xc8, 0x89, 0x17, 0x02, 0x02, 0x13,
                0x04, 0x13, 0x00, 0xb9, 0x14, 0x59, 0x8f, 0x80,
                0x50, 0x00, 0x09, 0x68, 0x65, 0x6c, 0x6c, 0x6f,
                0x2e, 0x74, 0x78, 0x74, 0x68, 0x65, 0x6c, 0x6c,
                0x6f, 0x20, 0x72, 0x61, 0x72, 0x35, 0x20, 0x66,
                0x61, 0x6c, 0x6c, 0x62, 0x61, 0x63, 0x6b, 0x39,
                0xf9, 0xb2, 0x81, 0x02, 0x05, 0x00,
            };
            f.write(reinterpret_cast<const char*>(kFixture), sizeof(kFixture));
        }
        REQUIRE(engine::is_rar_archive(archive));

        // With unrar unreachable (PATH stripped), extract() must fail with the
        // "not available" diagnostic, not "exited with code 127": execvp failure
        // surfaces as exit_code 127 with ok == true.
        {
            const char* old_path = std::getenv("PATH");
            struct PathGuard {
                std::string value;
                bool had_value = false;
                ~PathGuard() {
                    if (had_value) setenv("PATH", value.c_str(), 1);
                    else unsetenv("PATH");
                }
            } path_guard{old_path ? std::string(old_path) : std::string(),
                         old_path != nullptr};
            REQUIRE(path_guard.had_value);
            setenv("PATH", "/nonexistent-gmm-test", 1);
            std::vector<engine::ExtractedFile> missing_files;
            std::string missing_error;
            REQUIRE_FALSE(engine::ArchiveExtractor::extract(
                archive, tmp.root / "out-missing", missing_files, missing_error));
            REQUIRE(missing_error.find("not available") != std::string::npos);
        }  // PATH restored here even if an assertion above fails

        // The real install path needs `unrar` on PATH too; skip gracefully when
        // it is absent so the rest of the suite still runs elsewhere. `ok` only
        // means fork+waitpid succeeded - a missing binary still exits 127 via
        // the execvp-failure path, so require exit_code == 0 as well.
        const bool unrar_present = [] {
            const engine::CapturedProcess p = engine::run_captured({"unrar", "--version"});
            return p.ok && p.exit_code == 0;
        }();
        if (!unrar_present) {
            SKIP("unrar not on PATH");
        }

        std::vector<engine::ExtractedFile> files;
        std::string error;
        const bool extracted =
            engine::ArchiveExtractor::extract(archive, tmp.root / "out", files, error);
        if (!extracted) {
            FAIL("unrar fallback failed: " + error);
        }
        REQUIRE(files.size() == 1);
        REQUIRE(files[0].archive_path == "hello.txt");
        REQUIRE(std::filesystem::exists(tmp.root / "out" / "hello.txt"));
        std::ifstream in(tmp.root / "out" / "hello.txt", std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        REQUIRE(content == "hello rar5 fallback");
        std::printf("PASS: install_progress — unrar fallback extracted '%s' (%zu bytes)\n",
                    files[0].archive_path.c_str(), content.size());
    }
}
