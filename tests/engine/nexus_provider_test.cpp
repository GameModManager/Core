// Test for NexusProvider::parse_mod_info — the pure JSON mapping behind the
// Mod Info Nexus tab "Refresh" button (mods/{game}/mods/{id}.json).
#include "engine/source/nexus_provider.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static void require(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

int main() {
    using engine::ModInfoResult;
    using engine::NexusProvider;

    // --- Realistic mods/{game}/mods/{id}.json response. ---
    const std::string body =
        R"({
          "name": "RaceMenu",
          "summary": "Character creation overhaul",
          "version": "0.4.20",
          "newest_version": "0.4.21",
          "available": true,
          "category_id": 21,
          "description": "[size=3][b]RaceMenu[/b][/size]\r\n A description.",
          "author": "expired6978"
        })";
    ModInfoResult r = NexusProvider::parse_mod_info(body);
    require(r.available, "available parsed");
    require(r.name == "RaceMenu", "name parsed");
    require(r.version == "0.4.20", "version parsed");
    require(r.newest_version == "0.4.21", "newest_version parsed");
    require(r.category_id == "21", "category_id parsed");
    require(r.author == "expired6978", "author parsed");
    require(r.description.find("[size=3]") != std::string::npos,
            "BBCode description parsed verbatim");

    // --- Unavailable mod: available=false but fields still present. ---
    const std::string hidden =
        R"({"name": "Hidden Mod", "available": false, "version": "1.0"})";
    ModInfoResult h = NexusProvider::parse_mod_info(hidden);
    require(!h.available, "unavailable flag honored");
    require(h.name == "Hidden Mod" && h.version == "1.0",
            "fields still parsed when unavailable");

    // --- Garbage / non-object input -> empty result. ---
    ModInfoResult bad = NexusProvider::parse_mod_info("not json {");
    require(!bad.available && bad.name.empty(), "garbage yields empty result");
    ModInfoResult arr = NexusProvider::parse_mod_info("[1,2,3]");
    require(!arr.available, "non-object JSON yields empty result");

    std::printf("nexus_provider_test: all checks passed\n");
    return 0;
}
