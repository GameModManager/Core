// Test for engine::NxmRouter::parse.
#include "engine/nxm/nxm_router.h"

#include <cassert>
#include <iostream>

namespace {

void expect_link(const std::string& url, const std::string& domain, long long mod,
                 long long file, const std::string& key = "", long long expire = 0) {
    const auto link = engine::NxmRouter::parse(url);
    if (link.nexus_domain != domain || link.mod_id != mod || link.file_id != file ||
        link.key != key || link.expire != expire) {
        std::cerr << "FAIL: " << url << "\n"
                  << "  expected domain=" << domain << " mod=" << mod << " file=" << file
                  << " key=" << key << " expire=" << expire << "\n"
                  << "  got      domain=" << link.nexus_domain << " mod=" << link.mod_id
                  << " file=" << link.file_id << " key=" << link.key
                  << " expire=" << link.expire << "\n";
        std::abort();
    }
}

}  // namespace

int main() {
    // Real Nexus-site form:
    //   nxm://<game-domain>/mods/<mod>/files/<file>?key=...&expires=...&user_id=...
    expect_link("nxm://skyrimspecialedition/mods/184625/files/781833?key=WPsTiCS-cJMsRv29vXJX4g&expires=1785695383&user_id=44196692",
                "skyrimspecialedition", 184625, 781833,
                "WPsTiCS-cJMsRv29vXJX4g", 1785695383);
    expect_link("nxm://thebindingofisaacrebirth/mods/68/files/528",
                "thebindingofisaacrebirth", 68, 528);

    // Empty-authority mangling: nxm:///<game>/... (browser/portal layers).
    expect_link("nxm:///thebindingofisaacrebirth/mods/68/files/528",
                "thebindingofisaacrebirth", 68, 528);
    expect_link("nxm:///skyrimspecialedition/mods/123/files/456?key=abc&expire=1",
                "skyrimspecialedition", 123, 456, "abc", 1);

    // Modern form: nxm://<game-domain>/mods/<mod>/files/<file>
    expect_link("nxm://thebindingofisaacrebirth/mods/68/files/528",
                "thebindingofisaacrebirth", 68, 528);
    expect_link("nxm://skyrimspecialedition/mods/123/files/456?key=abc&expire=1",
                "skyrimspecialedition", 123, 456, "abc", 1);

    // Nexus-site form: nxm://nexus/<game-domain>/mods/<mod>/files/<file>
    expect_link("nxm://nexus/thebindingofisaacrebirth/mods/68/files/528",
                "thebindingofisaacrebirth", 68, 528);
    expect_link("nxm://nexus/skyrimspecialedition/mods/123/files/456&key=zz&expire=2",
                "skyrimspecialedition", 123, 456, "zz", 2);

    // Legacy NMM form (no game domain): domain stays "nexus", ids still parsed.
    const auto legacy = engine::NxmRouter::parse("nxm://nexus/mods/68?key=abc&expire=1");
    if (legacy.nexus_domain != "nexus" || legacy.mod_id != 68 || legacy.file_id != 0) {
        std::cerr << "FAIL: legacy NMM form\n";
        std::abort();
    }

    std::cout << "nxm_router_test: all assertions passed\n";
    return 0;
}
