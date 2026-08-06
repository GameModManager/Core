#include "engine/saves/save_game.h"

#include <cstdio>

namespace engine {

namespace {

// 100ns ticks between 1601-01-01 (FILETIME epoch) and 1970-01-01 (Unix epoch).
constexpr std::uint64_t kFiletimeToUnixEpoch = 116444736000000000ULL;
constexpr std::uint64_t kTicksPerSecond = 10'000'000ULL;

}  // namespace

SaveEpochSeconds filetime_to_epoch(std::uint64_t filetime_100ns) {
    if (filetime_100ns < kFiletimeToUnixEpoch) {
        return 0;
    }
    return static_cast<SaveEpochSeconds>((filetime_100ns - kFiletimeToUnixEpoch) /
                                         kTicksPerSecond);
}

std::string SaveGame::display_name() const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ", #%u, Level %u, ", save_number, pc_level);
    std::string out = pc_name + buf + pc_location;
    return out;
}

bool SaveGame::has_script_extender_file() const {
    if (!file_path.has_extension()) {
        return false;
    }
    auto co = file_path;
    co.replace_extension(".skse");
    std::error_code ec;
    return std::filesystem::exists(co, ec);
}

}  // namespace engine
