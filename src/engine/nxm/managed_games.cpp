#include "engine/nxm/managed_games.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace engine {

ManagedGames::ManagedGames(const std::filesystem::path& json_path)
    : json_path_(json_path) {}

void ManagedGames::load() {
    entries_.clear();

    std::ifstream f(json_path_);
    if (!f.is_open()) return;

    try {
        nlohmann::json j;
        f >> j;

        if (!j.is_object()) return;

        for (auto& [game_id, val] : j.items()) {
            if (!val.is_object() || !val.contains("sources")) continue;

            ManagedGameEntry entry;
            entry.game_id = game_id;

            for (const auto& src : val["sources"]) {
                if (!src.is_object()) continue;
                GameSource source;
                if (src.contains("source_id"))    source.source_id    = src["source_id"].get<std::string>();
                if (src.contains("website_url"))  source.website_url  = src["website_url"].get<std::string>();
                if (src.contains("nexus_domain")) source.nexus_domain = src["nexus_domain"].get<std::string>();
                if (!source.source_id.empty()) {
                    entry.sources.push_back(source);
                }
            }

            if (!entry.sources.empty()) {
                entries_.push_back(entry);
            }
        }
    } catch (...) {
        entries_.clear();
    }
}

bool ManagedGames::save() const {
    nlohmann::json j;

    for (const auto& entry : entries_) {
        nlohmann::json src_array = nlohmann::json::array();
        for (const auto& src : entry.sources) {
            src_array.push_back({
                {"source_id",    src.source_id},
                {"website_url",  src.website_url},
                {"nexus_domain", src.nexus_domain}
            });
        }
        j[entry.game_id] = {{"sources", src_array}};
    }

    std::error_code ec;
    std::filesystem::create_directories(json_path_.parent_path(), ec);

    std::ofstream f(json_path_);
    if (!f.is_open()) return false;

    f << j.dump(2) << "\n";
    return f.good();
}

bool ManagedGames::is_managed(const std::string& game_id) const {
    for (const auto& e : entries_) {
        if (e.game_id == game_id) return true;
    }
    return false;
}

bool ManagedGames::has_source(const std::string& game_id,
                              const std::string& source_id) const {
    for (const auto& e : entries_) {
        if (e.game_id == game_id) {
            for (const auto& s : e.sources) {
                if (s.source_id == source_id) return true;
            }
        }
    }
    return false;
}

void ManagedGames::add_source(const std::string& game_id, const GameSource& source) {
    for (auto& e : entries_) {
        if (e.game_id == game_id) {
            // Don't add duplicates
            for (const auto& s : e.sources) {
                if (s.source_id == source.source_id) return;
            }
            e.sources.push_back(source);
            save();
            return;
        }
    }
    // Game not found — create new entry
    ManagedGameEntry entry;
    entry.game_id = game_id;
    entry.sources.push_back(source);
    entries_.push_back(entry);
    save();
}

void ManagedGames::remove_source(const std::string& game_id, const std::string& source_id) {
    for (auto& e : entries_) {
        if (e.game_id == game_id) {
            e.sources.erase(
                std::remove_if(e.sources.begin(), e.sources.end(),
                    [&](const GameSource& s) { return s.source_id == source_id; }),
                e.sources.end());
            // Remove the game entry entirely if no sources left
            if (e.sources.empty()) {
                entries_.erase(
                    std::remove_if(entries_.begin(), entries_.end(),
                        [&](const ManagedGameEntry& ge) { return ge.game_id == game_id; }),
                    entries_.end());
            }
            save();
            return;
        }
    }
}

std::string ManagedGames::game_id_for_domain(const std::string& nexus_domain) const {
    for (const auto& e : entries_) {
        for (const auto& s : e.sources) {
            if (s.nexus_domain == nexus_domain) return e.game_id;
        }
    }
    return {};
}

}  // namespace engine
