#include "engine/deploy/root_override.h"

#include <cctype>

namespace engine {

namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// True if path starts with the exact segment `segment` (boundary-sensitive:
// "Data/x" yes, "Datax/y" no). Empty segment matches nothing.
bool starts_with_segment(const std::string& path, const std::string& segment) {
    if (segment.empty()) return false;
    const auto norm = lower(path);
    const auto seg = lower(segment);
    if (norm == seg) return true;
    if (norm.size() <= seg.size()) return false;
    return norm.compare(0, seg.size(), seg) == 0 && norm[seg.size()] == '/';
}

}  // namespace

ClassifiedPath classify_registry_path(
    const std::string& rel_path,
    const std::vector<std::pair<std::string, int>>& owners,
    const std::unordered_set<std::string>& root_override_mods,
    const std::string& deploy_prefix)
{
    for (const auto& [owner, _] : owners) {
        if (!root_override_mods.count(owner)) continue;
        // Root-override owner: a leading <deploy_prefix>/ segment is data
        // content (it lands in the game's data dir), everything else is root
        // content.
        if (starts_with_segment(rel_path, deploy_prefix)) {
            ClassifiedPath out;
            out.space = DeploySpace::Data;
            // starts_with_segment already proved byte [prefix.size()] is '/'
            // when the path is longer than the prefix.
            out.display_path = (rel_path.size() > deploy_prefix.size())
                ? rel_path.substr(deploy_prefix.size() + 1)
                : std::string();
            return out;
        }
        return {DeploySpace::Root, rel_path};
    }
    return {DeploySpace::Data, rel_path};
}

}  // namespace engine
