#pragma once

// Qt-free byte cursor + compression handling for Bethesda save files. Port of
// MO2's GamebryoSaveGame::FileWrapper
// (REFERENCES/modorganizer-game_bethesda/src/gamebryo/gamebryosavegame.cpp).
//
// Design: the whole save file is read into memory once (saves are a few MB at
// most), then all reads - raw file region or the decompressed data region -
// go through the same primitives. This mirrors MO2's behavior (its FileWrapper
// also switches its read source between the QFile and a decompressed
// QDataStream) without any streaming complexity.
//
// Compression (set by the parser after reading the screenshot, MO2
// setCompressionType + openCompressedData):
//   type 0 - data region is raw in the file
//   type 1 - a sequence of independent zlib/gzip streams; each stream is read
//            from the file at a 16-byte-aligned offset, its decompressed
//            output concatenated (MO2 readNextChunk + CHUNK 16384)
//   type 2 - one LZ4 block (u32 uncompressed size, u32 compressed size, bytes)

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine {

// Thrown on any malformed/truncated input. Mirrors MO2's std::runtime_error
// ("unexpected end of file") so listSaves can skip+log bad files.
class SaveParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SaveReader {
public:
    // Loads `path`, verifies the first `expected_magic` bytes, positions the
    // cursor just past the magic. Throws SaveParseError on open failure or
    // magic mismatch.
    SaveReader(const std::filesystem::path& path, const std::string& expected_magic);

    // --- raw little-endian primitives; all throw SaveParseError on EOF ---
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    float f32();
    void skip(std::size_t count);
    std::string read_bytes(std::size_t count);

    // u16 byte-length + payload, decoded as UTF-8. MO2 FileWrapper
    // read<QString> (WSTRING, UTF8). The header strings are single-byte in
    // real saves ("Vanilla Vanny" = 13 ASCII bytes), so a plain UTF-8 decode
    // is correct; MO2's LOCAL8BIT variant for the LE location field decodes
    // to UTF-8 on any modern Linux locale anyway.
    std::string wstring();

    // Reads the compression header for the data region and switches all
    // subsequent reads to the decompressed buffer. `type` is the u16 read by
    // the SE parser after width/height. Throws SaveParseError on unknown
    // compression or a failed decompress.
    void begin_compressed(std::uint16_t type);

    [[nodiscard]] std::size_t position() const { return pos_; }
    [[nodiscard]] std::size_t size() const { return buf_.size(); }

private:
    static std::vector<std::uint8_t> inflate_chunks(std::uint64_t start,
                                                    std::uint64_t total_uncompressed,
                                                    const std::vector<std::uint8_t>& file);
    static std::vector<std::uint8_t> lz4_decompress(const std::string& compressed,
                                                    std::uint32_t uncompressed_size);

    std::vector<std::uint8_t> buf_;   // file content, or decompressed region
    std::vector<std::uint8_t> file_;  // full raw file (kept for chunked reads)
    std::size_t pos_ = 0;
};

}  // namespace engine
