#include "engine/plugins/esp_header.h"

#include <cstring>
#include <fstream>

namespace engine {

namespace {
uint32_t le32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t le16(const unsigned char* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
}  // namespace

EspHeaderInfo read_esp_header(const std::filesystem::path& file_path) {
    EspHeaderInfo info;

    std::ifstream in(file_path, std::ios::binary);
    if (!in) return info;

    // Record header: type(4) size(4) flags(4) formid(4) timestamp(4) version(4)
    unsigned char header[24];
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (in.gcount() < static_cast<std::streamsize>(sizeof(header))) return info;
    if (std::memcmp(header, "TES4", 4) != 0) return info;

    const uint32_t data_size = le32(header + 4);
    const uint32_t flags = le32(header + 8);

    info.is_master = (flags & 0x0001u) != 0;
    info.is_light = (flags & 0x0200u) != 0;
    info.is_medium = (flags & 0x0400u) != 0;
    info.localized = (flags & 0x0080u) != 0;
    // Version stamp at bytes 20-24 (MO2's formVersion; 0 for plain or on some
    // tools that zero it - the UI hides the field then, matching MO2).
    info.form_version = le32(header + 20);

    // Sanity guard - a plugin header never legitimately exceeds a few KB.
    if (data_size > 16u * 1024 * 1024) return info;

    std::vector<unsigned char> data(data_size);
    if (data_size > 0) {
        in.read(reinterpret_cast<char*>(data.data()), data_size);
        if (static_cast<uint32_t>(in.gcount()) < data_size) return info;
    }

    // Iterate TES4 subrecords: type(4) size(2) payload(size).
    size_t pos = 0;
    while (pos + 6 <= data.size()) {
        char type[5] = {0};
        std::memcpy(type, data.data() + pos, 4);
        const uint16_t size = le16(data.data() + pos + 4);
        pos += 6;
        if (pos + size > data.size()) break;

        if (std::strcmp(type, "MAST") == 0 && size > 0) {
            std::string master(reinterpret_cast<const char*>(data.data() + pos), size);
            while (!master.empty() &&
                   (master.back() == '\0' || master.back() == '\r' ||
                    master.back() == '\n' || master.back() == ' ')) {
                master.pop_back();
            }
            if (!master.empty()) info.masters.push_back(master);
        } else if (std::strcmp(type, "HEDR") == 0 && size >= 8) {
            // HEDR: float header version, uint32 numRecords, uint32 nextObjectId.
            std::memcpy(&info.header_version, data.data() + pos,
                        sizeof(info.header_version));
            info.num_records = le32(data.data() + pos + 4);
        } else if (std::strcmp(type, "CNAM") == 0 && size > 0) {
            std::string author(reinterpret_cast<const char*>(data.data() + pos), size);
            while (!author.empty() && author.back() == '\0') author.pop_back();
            info.author = std::move(author);
        } else if (std::strcmp(type, "SNAM") == 0 && size > 0) {
            std::string desc(reinterpret_cast<const char*>(data.data() + pos), size);
            while (!desc.empty() && desc.back() == '\0') desc.pop_back();
            info.description = std::move(desc);
        } else if (std::strcmp(type, "DATA") == 0 && size >= 4) {
            // Some tools write the ESL/ESH/localized bits in the TES4 DATA
            // subrecord instead of the record header. OR them in so both
            // conventions are accepted.
            const uint32_t data_flags = le32(data.data() + pos);
            if (data_flags & 0x0200u) info.is_light = true;
            if (data_flags & 0x0400u) info.is_medium = true;
            if (data_flags & 0x0080u) info.localized = true;
        }
        pos += size;
    }

    info.valid = true;
    return info;
}

}  // namespace engine
