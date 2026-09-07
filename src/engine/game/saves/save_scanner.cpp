#include "engine/game/saves/save_scanner.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

#include "engine/game/saves/save_reader.h"
#include "engine/parallel/parallel.h"

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
    if (!parse_fn) {
        // Mirror the original guard: an empty std::function is treated as a
        // silent skip, not a crash. Enumerate to keep the directory walk but
        // emit no rows.
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        (void)it;
        return {};
    }

    // Phase 1: enumerate matching paths single-threaded. directory_iterator
    // is not safe to share across threads, so we collect first and then parse
    // in parallel (matches the file-by-file plan from Workspace-6kn7).
    std::vector<std::filesystem::path> paths;
    {
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
            paths.push_back(p);
        }
    }

    // Phase 2: parse in parallel. SaveParserRegistry::parse_save releases its
    // mutex before invoking the user fn, so concurrent parses don't serialize
    // on the registry. SaveReader has no shared state.
    std::vector<std::optional<SaveGame>> parsed(paths.size());
    parallel::for_each(paths.size(), [&](std::size_t i) {
        try {
            parsed[i].emplace(parse_fn(paths[i]));
        } catch (const SaveParseError&) {
            // Not a real save (e.g. the .skse co-save) or corrupt: skip it,
            // exactly like MO2's listSaves try/catch. Leave optional empty.
        }
    });

    // Phase 3: compact, sort on the calling thread.
    std::vector<SaveGame> out;
    out.reserve(parsed.size());
    for (auto& slot : parsed) {
        if (slot) {
            out.push_back(std::move(*slot));
        }
    }
    std::sort(out.begin(), out.end(), [](const SaveGame& a, const SaveGame& b) {
        return a.creation_time > b.creation_time;
    });
    return out;
}

}  // namespace engine
