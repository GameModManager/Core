#include "engine/game/saves/save_scanner.h"

#include <algorithm>
#include <cctype>

#include "engine/game/saves/save_reader.h"

namespace engine {

namespace {

bool extension_matches(const std::filesystem::path& file,
                       const std::vector<std::string>& extensions) {
    const std::string actual = file.extension().string();
    for (const std::string& ext : extensions) {
        std::string want = ext;
        if (!want.empty() && want[0] == '.') {
            want = want.substr(1);
        }
        if (want.size() != actual.size() - 1) {
            continue;
        }
        bool match = true;
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(want[i])) !=
                std::tolower(static_cast<unsigned char>(actual[i + 1]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<SaveGame> scan_saves(const std::filesystem::path& dir,
                                 const std::vector<std::string>& extensions,
                                 const SaveParseFn& parse_fn) {
    std::vector<SaveGame> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    std::filesystem::directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        const std::filesystem::directory_entry& entry = *it;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const std::filesystem::path& p = entry.path();
        if (!extension_matches(p, extensions)) {
            continue;
        }
        try {
            if (!parse_fn) continue;
            out.push_back(parse_fn(p));
        } catch (const SaveParseError&) {
            // Not a real save (e.g. the .skse co-save) or corrupt: skip it,
            // exactly like MO2's listSaves try/catch.
        }
    }
    std::sort(out.begin(), out.end(), [](const SaveGame& a, const SaveGame& b) {
        return a.creation_time > b.creation_time;
    });
    return out;
}

}  // namespace engine
