#include "engine/saves/save_reader.h"

#include <lz4.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace engine {

namespace {

[[noreturn]] void throw_eof() {
    throw SaveParseError("unexpected end of file");
}

std::uint16_t rd16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at]) |
           (static_cast<std::uint16_t>(b[at + 1]) << 8);
}

}  // namespace

SaveReader::SaveReader(const std::filesystem::path& path,
                       const std::string& expected_magic) {
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) {
        throw SaveParseError("failed to open " + path.string());
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        throw SaveParseError("failed to size " + path.string());
    }
    file_.resize(static_cast<std::size_t>(size));
    if (!file_.empty() &&
        std::fread(file_.data(), 1, file_.size(), f) != file_.size()) {
        std::fclose(f);
        throw SaveParseError("short read on " + path.string());
    }
    std::fclose(f);

    if (file_.size() < expected_magic.size() ||
        std::memcmp(file_.data(), expected_magic.data(), expected_magic.size()) != 0) {
        throw SaveParseError("wrong file format - expected " + expected_magic +
                             " for " + path.string());
    }
    buf_ = file_;
    pos_ = expected_magic.size();
}

std::uint8_t SaveReader::u8() {
    if (pos_ + 1 > buf_.size()) {
        throw_eof();
    }
    return buf_[pos_++];
}

std::uint16_t SaveReader::u16() {
    if (pos_ + 2 > buf_.size()) {
        throw_eof();
    }
    std::uint16_t v = rd16(buf_, pos_);
    pos_ += 2;
    return v;
}

std::uint32_t SaveReader::u32() {
    if (pos_ + 4 > buf_.size()) {
        throw_eof();
    }
    std::uint32_t v = static_cast<std::uint32_t>(buf_[pos_]) |
                      (static_cast<std::uint32_t>(buf_[pos_ + 1]) << 8) |
                      (static_cast<std::uint32_t>(buf_[pos_ + 2]) << 16) |
                      (static_cast<std::uint32_t>(buf_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
}

std::uint64_t SaveReader::u64() {
    std::uint64_t lo = u32();
    std::uint64_t hi = u32();
    return lo | (hi << 32);
}

float SaveReader::f32() {
    std::uint32_t bits = u32();
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

void SaveReader::skip(std::size_t count) {
    if (pos_ + count > buf_.size()) {
        throw_eof();
    }
    pos_ += count;
}

std::string SaveReader::read_bytes(std::size_t count) {
    if (pos_ + count > buf_.size()) {
        throw_eof();
    }
    std::string out(reinterpret_cast<const char*>(buf_.data() + pos_), count);
    pos_ += count;
    return out;
}

std::string SaveReader::wstring() {
    std::uint16_t len = u16();
    std::string bytes = read_bytes(len);
    // UTF-8 decode (validating): real header strings are single-byte ASCII;
    // non-ASCII bytes are passed through as-is like QString::fromUtf8.
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size();) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < bytes.size() &&
                   (static_cast<unsigned char>(bytes[i + 1]) & 0xC0) == 0x80) {
            out.push_back(static_cast<char>(c));
            out.push_back(bytes[i + 1]);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < bytes.size() &&
                   (static_cast<unsigned char>(bytes[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(bytes[i + 2]) & 0xC0) == 0x80) {
            out.push_back(static_cast<char>(c));
            out.push_back(bytes[i + 1]);
            out.push_back(bytes[i + 2]);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < bytes.size() &&
                   (static_cast<unsigned char>(bytes[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(bytes[i + 2]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(bytes[i + 3]) & 0xC0) == 0x80) {
            out.append(bytes, i, 4);
            i += 4;
        } else {
            // Invalid UTF-8: keep the byte (fromUtf8 replaces with U+FFFD; a
            // lone byte is closer to the source than a replacement for our
            // display purposes).
            out.push_back(static_cast<char>(c));
            ++i;
        }
    }
    return out;
}

void SaveReader::begin_compressed(std::uint16_t type) {
    if (type == 0) {
        // Uncompressed data region: keep reading from the file cursor.
        return;
    }
    if (type == 1) {
        // MO2 openCompressedData type 1: u64 chunk start, u64 uncompressed
        // size, then independent zlib streams at 16-byte-aligned offsets.
        std::uint64_t chunk_start = u64();
        std::uint64_t total = u64();
        buf_ = inflate_chunks(chunk_start, total, file_);
        pos_ = 0;
        return;
    }
    if (type == 2) {
        // MO2 openCompressedData type 2: u32 uncompressed size, u32
        // compressed size, LZ4 block.
        std::uint32_t uncompressed_size = u32();
        std::uint32_t compressed_size = u32();
        std::string compressed = read_bytes(compressed_size);
        buf_ = lz4_decompress(compressed, uncompressed_size);
        pos_ = 0;
        return;
    }
    throw SaveParseError("unknown save compression type " + std::to_string(type));
}

std::vector<std::uint8_t> SaveReader::inflate_chunks(
    std::uint64_t start, std::uint64_t total_uncompressed,
    const std::vector<std::uint8_t>& file) {
    constexpr std::size_t kChunk = 16384;
    constexpr std::uint64_t kAlign = 16;

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(total_uncompressed, file.size())));
    std::uint64_t pos = start;
    std::array<std::uint8_t, kChunk> inbuf{};
    std::array<std::uint8_t, kChunk> outbuf{};

    while (out.size() < total_uncompressed && pos < file.size()) {
        z_stream zs{};
        if (inflateInit2(&zs, 15 + 32) != Z_OK) {
            throw SaveParseError("zlib init failed for save data");
        }
        bool stream_end = false;
        bool no_input = false;
        while (!stream_end) {
            if (zs.avail_in == 0) {
                std::size_t n = std::min<std::uint64_t>(
                    kChunk, file.size() - static_cast<std::size_t>(pos) - zs.total_in);
                if (n == 0) {
                    no_input = true;
                    break;
                }
                std::memcpy(inbuf.data(), file.data() + static_cast<std::size_t>(pos) + zs.total_in, n);
                zs.next_in = inbuf.data();
                zs.avail_in = static_cast<uInt>(n);
            }
            zs.next_out = outbuf.data();
            zs.avail_out = static_cast<uInt>(outbuf.size());
            int rc = inflate(&zs, Z_NO_FLUSH);
            std::size_t produced = outbuf.size() - zs.avail_out;
            out.insert(out.end(), outbuf.begin(), outbuf.begin() + produced);
            if (rc == Z_STREAM_END) {
                stream_end = true;
            } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
                inflateEnd(&zs);
                throw SaveParseError("bad zlib stream in save data");
            }
        }
        inflateEnd(&zs);
        if (no_input) {
            break;  // truncated file; parsed fields above will surface the error
        }
        std::uint64_t consumed = zs.total_in;
        std::uint64_t next = pos + consumed;
        if (next % kAlign != 0) {
            next += kAlign - (next % kAlign);
        }
        if (next == pos) {
            break;  // no forward progress — guard against an infinite loop
        }
        pos = next;
    }
    return out;
}

std::vector<std::uint8_t> SaveReader::lz4_decompress(
    const std::string& compressed, std::uint32_t uncompressed_size) {
    std::vector<std::uint8_t> out(uncompressed_size);
    int n = LZ4_decompress_safe(compressed.data(), reinterpret_cast<char*>(out.data()),
                                static_cast<int>(compressed.size()),
                                static_cast<int>(uncompressed_size));
    if (n < 0 || static_cast<std::uint32_t>(n) != uncompressed_size) {
        throw SaveParseError("LZ4 decompression failed in save data");
    }
    return out;
}

}  // namespace engine
