#include "engine/pack/source_detector.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::Pack {

namespace {

// ---------------------------------------------------------------------------
// Small string helpers (ASCII-only; URLs and extensions are ASCII)
// ---------------------------------------------------------------------------

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trim_ascii(std::string_view s) {
    std::size_t begin = 0;
    while (begin < s.size() &&
           std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(begin, end - begin));
}

// Strip a URL query ('?...') and fragment ('#...'). Only applied to
// URL-shaped input (containing "://") - a plain filename may legally
// contain '?' or '#'.
std::string_view strip_url_suffix(std::string_view s) {
    const auto pos = s.find_first_of("?#");
    return (pos == std::string_view::npos) ? s : s.substr(0, pos);
}

struct UrlParts {
    std::string_view scheme;  // "" when no "://" present
    std::string_view host;    // authority minus userinfo/port
    std::string_view path;    // starts with '/' or "" (query/fragment kept)
};

UrlParts split_url(std::string_view s) {
    UrlParts parts;
    const auto scheme_end = s.find("://");
    if (scheme_end == std::string_view::npos) return parts;
    parts.scheme = s.substr(0, scheme_end);
    auto rest = s.substr(scheme_end + 3);
    const auto path_begin = rest.find_first_of("/?#");
    auto authority =
        (path_begin == std::string_view::npos) ? rest : rest.substr(0, path_begin);
    parts.path =
        (path_begin == std::string_view::npos) ? std::string_view{} : rest.substr(path_begin);
    if (const auto at = authority.rfind('@');
        at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }
    if (const auto colon = authority.find(':');
        colon != std::string_view::npos) {
        authority = authority.substr(0, colon);
    }
    parts.host = authority;
    return parts;
}

bool path_has_segment(std::string_view path_lower, std::string_view segment) {
    std::size_t pos = 0;
    while (pos < path_lower.size()) {
        while (pos < path_lower.size() && path_lower[pos] == '/') ++pos;
        if (pos >= path_lower.size()) break;
        auto end = path_lower.find('/', pos);
        if (end == std::string_view::npos) end = path_lower.size();
        if (path_lower.substr(pos, end - pos) == segment) return true;
        pos = end;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Zip content sniffing - reads the End Of Central Directory and walks entry
// names only (no decompression, no full-file reads). Classic (non-Zip64)
// archives only; Zip64 packs still route via their .gmmpack extension.
// ---------------------------------------------------------------------------

std::uint16_t read_u16le(const std::vector<char>& buf, std::size_t pos) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(buf[pos]) |
        (static_cast<unsigned char>(buf[pos + 1]) << 8));
}

std::uint32_t read_u32le(const std::vector<char>& buf, std::size_t pos) {
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(buf[pos]) |
        (static_cast<unsigned char>(buf[pos + 1]) << 8) |
        (static_cast<unsigned char>(buf[pos + 2]) << 16) |
        (static_cast<unsigned char>(buf[pos + 3]) << 24));
}

bool read_exact(std::ifstream& f, std::uint64_t offset, char* dst,
                std::size_t count) {
    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f) return false;
    f.read(dst, static_cast<std::streamsize>(count));
    return f.gcount() == static_cast<std::streamsize>(count);
}

// True when the file is a classic zip containing an entry whose name matches
// want_name_lower (ASCII case-insensitive - the gmmpack parser itself owns
// strict name validation).
bool zip_has_entry(const std::filesystem::path& path,
                   std::string_view want_name_lower) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto end_pos = f.tellg();
    if (end_pos == std::ifstream::pos_type(-1)) return false;
    const auto size = static_cast<std::uint64_t>(end_pos);
    if (size < 22) return false;  // smaller than the EOCD itself

    // The EOCD sits within the last (64k comment + 22 byte EOCD) bytes.
    const std::uint64_t window = size < 65557 ? size : 65557;
    const std::uint64_t tail_start = size - window;
    std::vector<char> tail(static_cast<std::size_t>(window));
    if (!read_exact(f, tail_start, tail.data(),
                    static_cast<std::size_t>(window))) {
        return false;
    }

    // Scan backwards for the EOCD signature PK\x05\x06.
    std::size_t eocd = tail.size();
    for (std::size_t i = tail.size() - 22;; --i) {
        if (tail[i] == '\x50' && tail[i + 1] == '\x4b' &&
            tail[i + 2] == '\x05' && tail[i + 3] == '\x06') {
            eocd = i;
            break;
        }
        if (i == 0) return false;
    }

    const std::uint16_t total = read_u16le(tail, eocd + 10);
    const std::uint32_t cd_size = read_u32le(tail, eocd + 12);
    const std::uint32_t cd_offset = read_u32le(tail, eocd + 16);
    const std::uint64_t cd_end =
        static_cast<std::uint64_t>(cd_offset) + cd_size;
    if (cd_end > size) return false;

    std::uint64_t pos = cd_offset;
    std::vector<char> header(46);
    for (std::uint16_t i = 0; i < total; ++i) {
        if (pos + 46 > cd_end) return false;
        if (!read_exact(f, pos, header.data(), 46)) return false;
        if (!(header[0] == '\x50' && header[1] == '\x4b' &&
              header[2] == '\x01' && header[3] == '\x02')) {
            return false;  // not a central-directory entry
        }
        const std::uint16_t name_len = read_u16le(header, 28);
        const std::uint16_t extra_len = read_u16le(header, 30);
        const std::uint16_t comment_len = read_u16le(header, 32);
        const std::uint64_t name_pos = pos + 46;
        const std::uint64_t next =
            name_pos + name_len + extra_len + comment_len;
        if (next > cd_end || next < pos) return false;
        if (name_len > 0 && name_len < 4096) {
            std::string name(name_len, '\0');
            if (!read_exact(f, name_pos, name.data(), name_len)) return false;
            if (lower_ascii(name) == want_name_lower) return true;
        }
        pos = next;
    }
    return false;
}

bool file_starts_with_pk(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[2] = {0, 0};
    f.read(magic, 2);
    return f.gcount() == 2 && magic[0] == '\x50' && magic[1] == '\x4b';
}

// ---------------------------------------------------------------------------
// Detection outcome helpers
// ---------------------------------------------------------------------------

Detection make_detection(PackFormat format, std::string reason) {
    Detection d;
    d.format = format;
    d.format_id = std::string(format_id_of(format));
    d.reason = std::move(reason);
    return d;
}

Detection make_unknown(std::string reason) {
    Detection d;
    d.reason = std::move(reason);
    return d;
}

// A probe returns a Detection when decisive, std::nullopt to pass the input
// on to the next probe. Probes run in kProbes order: cheap string matches
// first, filesystem access last.
using Probe = std::optional<Detection> (*)(std::string_view);

// Nexus collections come through the nxm:// API, never as raw files, so any
// nxm:// link offered as a pack reference routes to the Nexus adapter. The
// adapter itself validates the link shape and reports precise errors.
std::optional<Detection> probe_nxm(std::string_view input) {
    if (!starts_with(lower_ascii(input), "nxm://")) return std::nullopt;
    return make_detection(
        PackFormat::NexusCollection,
        "nxm:// link resolves through the Nexus API, not as a file");
}

// https://www.nexusmods.com/<game>/collections/<id>[?...] page URLs.
std::optional<Detection> probe_nexus_web(std::string_view input) {
    const UrlParts parts = split_url(input);
    if (parts.scheme.empty()) return std::nullopt;
    const std::string scheme = lower_ascii(parts.scheme);
    if (scheme != "http" && scheme != "https") return std::nullopt;
    const std::string host = lower_ascii(parts.host);
    if (host != "nexusmods.com" && host != "www.nexusmods.com") {
        return std::nullopt;
    }
    if (!path_has_segment(lower_ascii(parts.path), "collections")) {
        return std::nullopt;
    }
    return make_detection(
        PackFormat::NexusCollection,
        "nexusmods.com/collections/ page resolves through the Nexus API");
}

// .gmmpack extension (a .gmmpack is a zip). A missing file is trusted - the
// reference may be validated before anything is downloaded. An existing file
// must at least be a zip; the gmmpack parser owns manifest validation.
std::optional<Detection> probe_gmmpack_extension(std::string_view input) {
    std::string_view candidate = input;
    if (input.find("://") != std::string_view::npos) {
        candidate = strip_url_suffix(input);
    }
    if (!ends_with(lower_ascii(candidate), ".gmmpack")) return std::nullopt;

    std::error_code ec;
    const std::filesystem::path path(candidate);
    if (std::filesystem::is_regular_file(path, ec)) {
        if (!file_starts_with_pk(path)) {
            return make_unknown(
                "file has a .gmmpack extension but is not a zip archive");
        }
    }
    return make_detection(PackFormat::Gmmpack,
                          "file with .gmmpack extension (zip archive)");
}

// Last resort: an existing file whose content proves it is a pack. Currently
// a zip whose central directory lists manifest.json at the root - i.e. a
// .gmmpack saved under the wrong name. Zip magic alone is not enough: a
// plain zip is not routable to any adapter.
std::optional<Detection> probe_zip_sniff(std::string_view input) {
    if (input.find("://") != std::string_view::npos) return std::nullopt;
    const std::filesystem::path path(input);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
    if (!zip_has_entry(path, "manifest.json")) return std::nullopt;
    return make_detection(
        PackFormat::Gmmpack,
        "zip archive containing manifest.json (gmmpack content)");
}

constexpr Probe kProbes[] = {
    probe_nxm,
    probe_nexus_web,
    probe_gmmpack_extension,
    probe_zip_sniff,
};

}  // namespace

std::string_view format_id_of(PackFormat format) {
    switch (format) {
        case PackFormat::Gmmpack: return "gmmpack";
        case PackFormat::NexusCollection: return "nexus-collection";
        case PackFormat::Unknown: return "";
    }
    return "";
}

Detection detect_pack_source(const std::string& url_or_path) {
    const std::string input = trim_ascii(url_or_path);
    if (input.empty()) return make_unknown("empty pack reference");
    for (const Probe probe : kProbes) {
        if (auto decision = probe(input)) return *decision;
    }
    return make_unknown(
        "unsupported pack reference (expected a .gmmpack file, an nxm:// "
        "link, or a nexusmods.com/collections/ URL)");
}

}  // namespace engine::Pack
