#include "engine/filetree/data_archive.h"

#include <archive.h>
#include <archive_entry.h>

#include <string>
#include <utility>

namespace engine {

namespace {

// libarchive listing backend (MO2's ArchiveFileTree readFileList). Paths are
// normalized to '/'-separated form; entries whose names are "." or ".." and
// empty-name entries are dropped (MO2 drops those in makeTree). A directory is
// identified by the archive's file type, with a trailing-slash fallback for
// archives that do not record directory entries as such.
class LibArchiveDataArchive final : public DataArchive {
public:
    explicit LibArchiveDataArchive(std::filesystem::path path)
        : m_path(std::move(path)) {}

    bool list(std::vector<ArchiveEntryInfo>& out,
              std::string* error) const override {
        struct archive* a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);

        const int open_rc = archive_read_open_filename(a, m_path.c_str(), 10240);
        if (open_rc != ARCHIVE_OK) {
            if (error) {
                *error = std::string("cannot open archive: ") +
                         (archive_error_string(a) ? archive_error_string(a) : "unknown");
            }
            archive_read_free(a);
            return false;
        }

        int64_t index = 0;
        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            std::string path;
            const char* utf8 = archive_entry_pathname_utf8(entry);
            if (utf8 != nullptr) {
                path = utf8;
            } else if (const char* raw = archive_entry_pathname(entry); raw != nullptr) {
                path = raw;
            }
            if (path.empty()) continue;

            // Normalize to '/'-separated, drop "." / ".." / empty components
            // (MO2 makeTree drops these). Directories keep a trailing slash so
            // the grouped tree can tell them apart.
            std::string norm;
            bool is_dir = archive_entry_filetype(entry) == AE_IFDIR;
            if (path.back() == '/') {
                is_dir = true;
                path.pop_back();
            }
            for (std::size_t i = 0; i < path.size();) {
                std::size_t next = path.find('/', i);
                if (next == std::string::npos) next = path.size();
                std::string comp = path.substr(i, next - i);
                if (comp == "." || comp == "..") {
                    i = next + 1;
                    continue;
                }
                if (!norm.empty()) norm += '/';
                norm += comp;
                i = next + 1;
            }
            if (norm.empty()) continue;

            ArchiveEntryInfo info;
            info.path = std::move(norm);
            info.is_dir = is_dir;
            info.size = archive_entry_size(entry);
            info.index = index++;
            out.push_back(std::move(info));
        }

        archive_read_free(a);
        return true;
    }

private:
    std::filesystem::path m_path;
};

}  // namespace

std::shared_ptr<DataArchive> open_libarchive(const std::filesystem::path& archive) {
    return std::make_shared<LibArchiveDataArchive>(archive);
}

}  // namespace engine
