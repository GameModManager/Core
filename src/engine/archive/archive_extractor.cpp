#include "engine/archive/archive_extractor.h"

#include <zip.h>

#include <fstream>

namespace engine {

bool ArchiveExtractor::extract_zip(const std::filesystem::path& archive,
                                   const std::filesystem::path& dest_dir,
                                   std::vector<ExtractedFile>& out_files) {
    int err = 0;
    zip_t* za = zip_open(archive.string().c_str(), ZIP_RDONLY, &err);
    if (!za) return false;

    zip_int64_t num_entries = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < num_entries; ++i) {
        const char* name = zip_get_name(za, i, 0);
        if (!name) continue;

        std::string name_str(name);
        if (name_str.empty() || name_str.back() == '/') continue;

        auto entry_path = dest_dir / name_str;
        auto parent = entry_path.parent_path();
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) continue;

        zip_file_t* zf = zip_fopen(za, name, 0);
        if (!zf) continue;

        std::ofstream out(entry_path, std::ios::binary);
        if (!out) {
            zip_fclose(zf);
            continue;
        }

        char buf[8192];
        zip_int64_t n;
        while ((n = zip_fread(zf, buf, sizeof(buf))) > 0) {
            out.write(buf, n);
        }
        zip_fclose(zf);
        out.close();

        ExtractedFile ef;
        ef.archive_path = name_str;
        ef.dest_path = entry_path;
        out_files.push_back(std::move(ef));
    }

    zip_close(za);
    return true;
}

}  // namespace engine
