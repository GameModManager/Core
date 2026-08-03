#include "engine/archive/archive_extractor.h"
#include "engine/fs_utils.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

namespace engine {

namespace {

// Best-effort libarchive diagnostic for the last operation on `a`.
std::string archive_error_message(struct archive* a) {
    const char* msg = archive_error_string(a);
    if (!msg || !*msg) return "unknown libarchive error";
    return msg;
}

}  // namespace

bool ArchiveExtractor::extract(const std::filesystem::path& archive,
                                const std::filesystem::path& dest_dir,
                                std::vector<ExtractedFile>& out_files,
                                std::string& error) {
    error.clear();
    struct archive* a = archive_read_new();
    if (!a) {
        error = "archive_read_new failed";
        return false;
    }

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    int r = archive_read_open_filename(a, archive.string().c_str(), 10240);
    if (r != ARCHIVE_OK) {
        error = "cannot open " + archive.string() + ": " +
                archive_error_message(a) +
                " (errno " + std::to_string(archive_errno(a)) + ")";
        archive_read_free(a);
        return false;
    }

    struct archive_entry* entry;
    bool failed = false;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname(entry);
        if (!name) continue;

        // Entry names are Windows-native (backslash separators), so normalize
        // them the same way engine::resolve_path does - this also makes
        // Windows-authored "dir\" entries count as directories below.
        // Traversal entries (absolute paths, "..") are never extracted: a
        // single hostile entry must not escape dest_dir, so extraction fails.
        std::string name_str = engine::normalize_separators(name);
        if (name_str.empty()) continue;

        const std::filesystem::path name_path(name_str);
        if (name_path.is_absolute() ||
            std::any_of(name_path.begin(), name_path.end(),
                        [](const auto& part) { return part == ".."; })) {
            error = "archive entry has an unsafe path: " + name_str;
            failed = true;
            break;
        }

        auto dest_path = dest_dir / name_path;

        // Strip trailing slash from dir entries for consistent detection.
        // RAR/WinRAR stores directory entries WITHOUT a trailing slash - the
        // directory-ness lives in the archive header, so trust libarchive's
        // filetype first and only fall back on the name heuristic.
        bool is_dir = (archive_entry_filetype(entry) == AE_IFDIR) ||
                      (!name_str.empty() && name_str.back() == '/');
        if (is_dir) {
            std::error_code ec;
            std::filesystem::create_directories(dest_path, ec);
            if (ec) {
                error = "cannot create directory " + dest_path.string() + ": " + ec.message();
                failed = true;
                break;
            }
            continue;
        }

        // Ensure parent directory exists
        std::error_code ec;
        std::filesystem::create_directories(dest_path.parent_path(), ec);
        if (ec) {
            error = "cannot create parent directory " +
                    dest_path.parent_path().string() + ": " + ec.message();
            failed = true;
            break;
        }

        // Write file data
        std::ofstream out(dest_path, std::ios::binary);
        if (!out) {
            error = "cannot open for writing " + dest_path.string() + ": " +
                    std::strerror(errno);
            failed = true;
            break;
        }

        char buf[32768];
        la_int64_t nread;
        while ((nread = archive_read_data(a, buf, sizeof(buf))) > 0) {
            out.write(buf, static_cast<std::streamsize>(nread));
            if (!out) {
                error = "write failed for " + dest_path.string() + ": " +
                        std::strerror(errno);
                failed = true;
                break;
            }
        }
        if (failed) break;
        if (nread < 0) {
            // Read error
            error = "read error in " + archive.string() + ": " +
                    archive_error_message(a);
            failed = true;
            break;
        }
        out.close();

        ExtractedFile ef;
        ef.archive_path = name_str;
        ef.dest_path = dest_path;
        out_files.push_back(std::move(ef));
    }

    // Loop ended not by reaching the end of the archive but on a header error
    // (corrupt/truncated archive) - report it instead of returning "success".
    if (!failed && r != ARCHIVE_EOF) {
        error = "archive header error in " + archive.string() + ": " +
                archive_error_message(a) +
                " (errno " + std::to_string(archive_errno(a)) + ")";
        failed = true;
    }

    archive_read_close(a);
    archive_read_free(a);
    return !failed;
}

}  // namespace engine
