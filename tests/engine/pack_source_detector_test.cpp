// Test for engine::Pack::detect_pack_source.
#include "engine/pack/source_detector.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using engine::Pack::detect_pack_source;
using engine::Pack::Detection;
using engine::Pack::PackFormat;

void expect_format(const std::string &input, PackFormat format,
                   const std::string &format_id) {
  const Detection d = detect_pack_source(input);
  INFO(input);
  REQUIRE(d.format == format);
  REQUIRE(d.format_id == format_id);
  REQUIRE(d.known() == (format != PackFormat::Unknown));
  REQUIRE(!d.reason.empty());
}

// ---------------------------------------------------------------------------
// Minimal stored-zip writer: local headers (method 0) + central directory +
// EOCD. CRCs are zeroed - detection only reads entry names, never data.
// ---------------------------------------------------------------------------

void put_u16(std::vector<char> &out, std::uint16_t v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void put_u32(std::vector<char> &out, std::uint32_t v) {
  put_u16(out, static_cast<std::uint16_t>(v & 0xffff));
  put_u16(out, static_cast<std::uint16_t>((v >> 16) & 0xffff));
}

struct ZipEntry {
  std::string name;
  std::string data;
};

void write_test_zip(const std::filesystem::path &path,
                    const std::vector<ZipEntry> &entries) {
  std::vector<char> out;
  std::vector<std::uint32_t> offsets;
  for (const auto &e : entries) {
    offsets.push_back(static_cast<std::uint32_t>(out.size()));
    put_u32(out, 0x04034b50);  // local file header
    put_u16(out, 20);
    put_u16(out, 0);  // flags
    put_u16(out, 0);  // method: stored
    put_u16(out, 0);  // time
    put_u16(out, 0);  // date
    put_u32(out, 0);  // crc (unchecked by detection)
    put_u32(out, static_cast<std::uint32_t>(e.data.size()));
    put_u32(out, static_cast<std::uint32_t>(e.data.size()));
    put_u16(out, static_cast<std::uint16_t>(e.name.size()));
    put_u16(out, 0);  // extra length
    out.insert(out.end(), e.name.begin(), e.name.end());
    out.insert(out.end(), e.data.begin(), e.data.end());
  }
  const std::uint32_t cd_offset = static_cast<std::uint32_t>(out.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    put_u32(out, 0x02014b50);  // central directory entry
    put_u16(out, 20);          // version made by
    put_u16(out, 20);          // version needed
    put_u16(out, 0);           // flags
    put_u16(out, 0);           // method
    put_u16(out, 0);           // time
    put_u16(out, 0);           // date
    put_u32(out, 0);           // crc
    put_u32(out, static_cast<std::uint32_t>(entries[i].data.size()));
    put_u32(out, static_cast<std::uint32_t>(entries[i].data.size()));
    put_u16(out, static_cast<std::uint16_t>(entries[i].name.size()));
    put_u16(out, 0);  // extra
    put_u16(out, 0);  // comment
    put_u16(out, 0);  // disk
    put_u16(out, 0);  // internal attrs
    put_u32(out, 0);  // external attrs
    put_u32(out, offsets[i]);
    out.insert(out.end(), entries[i].name.begin(), entries[i].name.end());
  }
  const std::uint32_t cd_size = static_cast<std::uint32_t>(out.size()) - cd_offset;
  put_u32(out, 0x06054b50);  // EOCD
  put_u16(out, 0);           // disk
  put_u16(out, 0);           // cd disk
  put_u16(out, static_cast<std::uint16_t>(entries.size()));
  put_u16(out, static_cast<std::uint16_t>(entries.size()));
  put_u32(out, cd_size);
  put_u32(out, cd_offset);
  put_u16(out, 0);  // comment length

  std::ofstream f(path, std::ios::binary);
  REQUIRE(f);
  f.write(out.data(), static_cast<std::streamsize>(out.size()));
  f.close();
  REQUIRE(f);
}

// RAII scratch file under the system temp dir.
std::atomic<int> g_temp_counter{0};
struct TempFile {
  std::filesystem::path path;
  explicit TempFile(const std::string &suffix)
      : path(std::filesystem::temp_directory_path() /
             ("gmm_pack_detect_" + std::to_string(g_temp_counter++) + suffix)) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  TempFile(const TempFile &)            = delete;
  TempFile &operator=(const TempFile &) = delete;
};

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::ofstream f(path, std::ios::binary);
  REQUIRE(f);
  f << text;
  f.close();
  REQUIRE(f);
}

}  // namespace

TEST_CASE("pack source ids stay in sync with the adapter interface", "[engine]") {
  // engine/pack/adapter.h documents format_id() as "gmmpack" and
  // "nexus-collection".
  REQUIRE(engine::Pack::format_id_of(PackFormat::Gmmpack) == "gmmpack");
  REQUIRE(engine::Pack::format_id_of(PackFormat::NexusCollection) ==
          "nexus-collection");
  REQUIRE(engine::Pack::format_id_of(PackFormat::Unknown).empty());
}

TEST_CASE("pack source detects .gmmpack extension", "[engine]") {
  expect_format("modpack.gmmpack", PackFormat::Gmmpack, "gmmpack");
  expect_format("MODPACK.GMMPACK", PackFormat::Gmmpack, "gmmpack");
  expect_format("ModPack.GmmPack", PackFormat::Gmmpack, "gmmpack");
  expect_format("/tmp/packs/frostpunk overhaul.gmmpack", PackFormat::Gmmpack,
                "gmmpack");
  // Direct-download URL for a gmmpack (query/fragment are not local files).
  expect_format("https://cdn.example.com/packs/a.gmmpack?dl=1", PackFormat::Gmmpack,
                "gmmpack");
  expect_format("https://cdn.example.com/packs/a.gmmpack#frag", PackFormat::Gmmpack,
                "gmmpack");
  // Missing files are trusted - the reference may be validated before
  // anything is downloaded.
  const std::string missing =
      (std::filesystem::temp_directory_path() / "gmm_no_such_pack.gmmpack").string();
  expect_format(missing, PackFormat::Gmmpack, "gmmpack");
  // Close but not .gmmpack.
  expect_format("modpack.gmmpack.bak", PackFormat::Unknown, "");
  expect_format("modpack.zip", PackFormat::Unknown, "");
}

TEST_CASE("pack source detects nxm links as Nexus collections", "[engine]") {
  expect_format("nxm://skyrimspecialedition/mods/184625/files/781833",
                PackFormat::NexusCollection, "nexus-collection");
  expect_format("nxm://skyrimspecialedition/mods/184625/files/781833"
                "?key=WPsTiCS-cJMsRv29vXJX4g&expires=1785695383&user_id=44196692",
                PackFormat::NexusCollection, "nexus-collection");
  expect_format("nxm://skyrimspecialedition", PackFormat::NexusCollection,
                "nexus-collection");
  expect_format("NXM://skyrimspecialedition/mods/1/files/2",
                PackFormat::NexusCollection, "nexus-collection");
  expect_format("  nxm://skyrimspecialedition/mods/1/files/2  ",
                PackFormat::NexusCollection, "nexus-collection");
}

TEST_CASE("pack source detects nexusmods collection page URLs", "[engine]") {
  expect_format("https://www.nexusmods.com/skyrimspecialedition/collections/5777",
                PackFormat::NexusCollection, "nexus-collection");
  expect_format("https://nexusmods.com/skyrimspecialedition/collections/5777",
                PackFormat::NexusCollection, "nexus-collection");
  expect_format("HTTPS://WWW.NEXUSMODS.COM/SkyrimSpecialEdition/Collections/5777",
                PackFormat::NexusCollection, "nexus-collection");
  expect_format("https://www.nexusmods.com/skyrimspecialedition/collections/5777/"
                "my-collection-slug?tab=files",
                PackFormat::NexusCollection, "nexus-collection");
  expect_format("http://www.nexusmods.com/fallout4/collections/123#revisions",
                PackFormat::NexusCollection, "nexus-collection");
  // Nexus pages that are NOT collections are not packs.
  expect_format("https://www.nexusmods.com/skyrimspecialedition/mods/184625",
                PackFormat::Unknown, "");
  expect_format("https://www.nexusmods.com/skyrimspecialedition", PackFormat::Unknown,
                "");
  expect_format("https://www.nexusmods.com/", PackFormat::Unknown, "");
  // Lookalike hosts must not match.
  expect_format("https://evilinexusmods.com/g/collections/1", PackFormat::Unknown, "");
  expect_format("https://nexusmods.com.evil.com/g/collections/1", PackFormat::Unknown,
                "");
  expect_format("https://www.nexusmods.com:443/g/collections/1",
                PackFormat::NexusCollection, "nexus-collection");
}

TEST_CASE("pack source rejects non-zip .gmmpack files", "[engine]") {
  TempFile fake(".gmmpack");
  write_text(fake.path, "this is not a zip archive");
  const Detection d = detect_pack_source(fake.path.string());
  INFO(fake.path.string());
  REQUIRE(d.format == PackFormat::Unknown);
  REQUIRE(d.format_id.empty());
  REQUIRE(!d.reason.empty());

  TempFile real(".gmmpack");
  write_test_zip(real.path, {{"manifest.json", "{}"}, {"mods/skyui.json", "{}"}});
  expect_format(real.path.string(), PackFormat::Gmmpack, "gmmpack");
}

TEST_CASE("pack source sniffs misnamed pack zips", "[engine]") {
  TempFile renamed(".zip");
  write_test_zip(
      renamed.path,
      {{"manifest.json", "{}"}, {"tree.json", "{}"}, {"mods/skyui.json", "{}"}});
  const Detection d = detect_pack_source(renamed.path.string());
  INFO(renamed.path.string());
  REQUIRE(d.format == PackFormat::Gmmpack);
  REQUIRE(d.format_id == "gmmpack");

  TempFile no_ext("");  // no extension at all
  write_test_zip(no_ext.path, {{"manifest.json", "{}"}});
  expect_format(no_ext.path.string(), PackFormat::Gmmpack, "gmmpack");

  // Case-insensitive manifest match - the parser owns strict validation.
  TempFile upper(".zip");
  write_test_zip(upper.path, {{"Manifest.JSON", "{}"}});
  expect_format(upper.path.string(), PackFormat::Gmmpack, "gmmpack");

  // A zip without a pack manifest is not routable to any adapter.
  TempFile plain(".zip");
  write_test_zip(plain.path, {{"readme.txt", "hi"}});
  const Detection pz = detect_pack_source(plain.path.string());
  INFO(plain.path.string());
  REQUIRE(pz.format == PackFormat::Unknown);

  // Garbage and empty files are not packs.
  TempFile garbage(".bin");
  write_text(garbage.path, "\x50\x4b broken, no eocd here");
  REQUIRE(detect_pack_source(garbage.path.string()).format == PackFormat::Unknown);
  TempFile empty(".bin");
  write_text(empty.path, "");
  REQUIRE(detect_pack_source(empty.path.string()).format == PackFormat::Unknown);
}

TEST_CASE("pack source rejects junk input", "[engine]") {
  expect_format("", PackFormat::Unknown, "");
  expect_format("   ", PackFormat::Unknown, "");
  expect_format("notaurl", PackFormat::Unknown, "");
  expect_format("/tmp/definitely/not/a/pack.zip", PackFormat::Unknown, "");
  expect_format("ftp://example.com/pack.gmmpack", PackFormat::Gmmpack,
                "gmmpack");  // extension still routes direct downloads
  expect_format("modl://skyrimse/?url=https%3A%2F%2Fmod.pub%2Fx", PackFormat::Unknown,
                "");  // transport, not a pack
  expect_format("https://example.com/collections/1", PackFormat::Unknown,
                "");  // wrong host
}
