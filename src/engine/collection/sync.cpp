#include "engine/collection/sync.h"

#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace engine::Collection {

RevisionRelation compare_revision(int64_t installed, int64_t incoming) {
    if (incoming < installed) return RevisionRelation::Older;
    if (incoming > installed) return RevisionRelation::Newer;
    return RevisionRelation::Same;
}

void tag(::engine::Mod& mod,
         const std::string& collection_id,
         int64_t revision) {
    mod.collection_id = collection_id;
    mod.collection_revision = revision;
    mod.in_collection = true;
}

void untag(::engine::Mod& mod) {
    mod.collection_id.clear();
    mod.collection_revision = 0;
    mod.in_collection = false;
}

bool belongs_to(const ::engine::Mod& mod, const std::string& collection_id) {
    return mod.in_collection && mod.collection_id == collection_id;
}

std::string entry_version(const ModEntry& entry) {
    return std::visit([](const auto& s) { return s.version; }, entry.source);
}

CollectionDiff diff_revision(const std::vector<::engine::Mod>& installed,
                             const Manifest& incoming) {
    CollectionDiff diff;

    // Installed mods tracked to THIS collection, by mod id.
    std::unordered_map<std::string, std::string> installed_versions;
    for (const auto& mod : installed) {
        if (belongs_to(mod, incoming.id)) {
            installed_versions.emplace(mod.id, mod.version);
        }
    }

    std::unordered_set<std::string> incoming_ids;
    for (const auto& entry : incoming.mods) {
        incoming_ids.insert(entry.id);
        auto it = installed_versions.find(entry.id);
        if (it == installed_versions.end()) {
            diff.added.push_back(entry.id);
        } else if (it->second != entry_version(entry)) {
            diff.updated.push_back(entry.id);
        } else {
            diff.unchanged.push_back(entry.id);
        }
    }

    for (const auto& [id, version] : installed_versions) {
        (void)version;
        if (incoming_ids.count(id) == 0) {
            diff.removed.push_back(id);
        }
    }

    return diff;
}

} // namespace engine::Collection
