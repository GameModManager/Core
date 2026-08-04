// Install-progress engine tests: the two-pass ArchiveExtractor percent (header
// pre-pass sums entry sizes, then bytes written are reported against it) and
// the InstallStage copy percent (files counted up front, then done/total).
// Both must report a real, monotonic 0-100% - the engine side of the
// MO2-style install progress popup.
#include "engine/archive/archive_extractor.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/model/mod.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

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

int main() {
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
        assert(write_store_zip(archive, entries));

        std::vector<engine::ExtractedFile> files;
        std::string error;
        std::vector<std::pair<std::int64_t, std::int64_t>> progress;
        const std::int64_t expected_total = 65536 + 131072 + 5;
        assert(engine::ArchiveExtractor::extract(
            archive, tmp.root / "out", files, error,
            [&](std::int64_t done, std::int64_t total) {
                progress.emplace_back(done, total);
            }));
        assert(error.empty());
        assert(files.size() == 3);  // directory entries are not extracted files
        assert(std::filesystem::exists(tmp.root / "out" / "textures" / "a.dds"));
        assert(std::filesystem::exists(tmp.root / "out" / "meshes" / "b.nif"));
        assert(std::filesystem::exists(tmp.root / "out" / "readme.txt"));

        assert(!progress.empty());
        std::int64_t last_done = -1;
        for (const auto& [done, total] : progress) {
            assert(total == expected_total);
            assert(done >= last_done);
            last_done = done;
        }
        assert(progress.back().first == expected_total);
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
        assert(stage.execute(mod, ctx));
        assert(mod.state == engine::ModState::Installed);
        assert(std::filesystem::exists(mods / "Progress Mod" / "file0.txt"));
        assert(std::filesystem::exists(mods / "Progress Mod" / "sub" / "nested.bin"));

        assert(!percents.empty());
        int last = -1;
        for (int p : percents) {
            assert(p >= last);
            last = p;
        }
        assert(percents.back() == 100);
        assert(!statuses.empty());
        assert(statuses.front().find("Installing to Progress Mod") != std::string::npos);
        std::printf("PASS: install_progress — InstallStage copy reported %d%%\n",
                    percents.back());
    }

    return 0;
}
