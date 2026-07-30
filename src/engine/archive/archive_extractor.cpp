#include "engine/archive/archive_extractor.h"

#include <archive.h>
#include <archive_entry.h>

#include <fstream>

namespace engine {

bool ArchiveExtractor::extract(const std::filesystem::path& archive,
                                const std::filesystem::path& dest_dir,
                                std::vector<ExtractedFile>& out_files) {
    struct archive* a = archive_read_new();
    if (!a) return false;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    int r = archive_read_open_filename(a, archive.string().c_str(), 10240);
    if (r != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname(entry);
        if (!name) continue;

        std::string name_str(name);
        if (name_str.empty()) continue;

        auto dest_path = dest_dir / name_str;

        // Strip trailing slash from dir entries for consistent detection
        bool is_dir = (name_str.back() == '/');
        if (is_dir) {
            std::error_code ec;
            std::filesystem::create_directories(dest_path, ec);
            if (ec) {
                archive_read_free(a);
                return false;
            }
            continue;
        }

        // Ensure parent directory exists
        std::error_code ec;
        std::filesystem::create_directories(dest_path.parent_path(), ec);
        if (ec) {
            archive_read_free(a);
            return false;
        }

        // Write file data
        std::ofstream out(dest_path, std::ios::binary);
        if (!out) {
            archive_read_free(a);
            return false;
        }

        char buf[32768];
        la_int64_t nread;
        while ((nread = archive_read_data(a, buf, sizeof(buf))) > 0) {
            out.write(buf, static_cast<std::streamsize>(nread));
            if (!out) {
                archive_read_free(a);
                return false;
            }
        }
        if (nread < 0) {
            // Read error
            archive_read_free(a);
            return false;
        }
        out.close();

        ExtractedFile ef;
        ef.archive_path = name_str;
        ef.dest_path = dest_path;
        out_files.push_back(std::move(ef));
    }

    archive_read_close(a);
    archive_read_free(a);
    return true;
}

}  // namespace engine
