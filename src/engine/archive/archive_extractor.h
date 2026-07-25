#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

struct ExtractedFile {
    std::string archive_path;
    std::filesystem::path dest_path;
};

class ArchiveExtractor {
public:
    static bool extract_zip(const std::filesystem::path& archive,
                            const std::filesystem::path& dest_dir,
                            std::vector<ExtractedFile>& out_files);
};

}  // namespace engine
