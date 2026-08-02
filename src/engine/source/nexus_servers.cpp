#include "engine/source/nexus_servers.h"

#include "engine/nexus_auth.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace engine {

namespace {
constexpr int kMaxSamples = 5;

std::filesystem::path storage_file() {
    return NexusAuth::config_dir() / "nexus_servers.json";
}

int today() {
    return static_cast<int>(std::time(nullptr) / 86400);
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    std::string h = haystack, n = needle;
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}
} // namespace

int NexusServer::average_speed() const {
    if (speeds.empty()) return 0;
    long long total = 0;
    for (int s : speeds) total += s;
    return static_cast<int>(total / static_cast<long long>(speeds.size()));
}

NexusServers& NexusServers::instance() {
    static NexusServers s;
    return s;
}

void NexusServers::load_locked() const {
    servers_.clear();
    std::ifstream f(storage_file());
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        if (!j.is_array()) return;
        for (const auto& e : j) {
            NexusServer s;
            s.name = e.value("name", "");
            if (s.name.empty()) continue;
            s.premium = e.value("premium", false);
            s.preferred = e.value("preferred", 0);
            s.last_seen = e.value("last_seen", 0);
            if (e.contains("speeds") && e["speeds"].is_array())
                for (const auto& v : e["speeds"])
                    s.speeds.push_back(v.get<int>());
            servers_.push_back(std::move(s));
        }
    } catch (const std::exception&) {
        servers_.clear();  // corrupt file - start fresh rather than crash
    }
}

void NexusServers::save_locked() const {
    std::error_code ec;
    std::filesystem::create_directories(NexusAuth::config_dir(), ec);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : servers_) {
        arr.push_back({{"name", s.name},
                       {"premium", s.premium},
                       {"preferred", s.preferred},
                       {"last_seen", s.last_seen},
                       {"speeds", s.speeds}});
    }
    std::ofstream f(storage_file(), std::ios::trunc);
    if (f) f << arr.dump(2) << "\n";
}

std::vector<NexusServer> NexusServers::all() const {
    std::lock_guard<std::mutex> g(mutex_);
    if (!loaded_) load_locked();
    loaded_ = true;
    return servers_;
}

int NexusServers::preferred_rank(const std::string& name) const {
    std::lock_guard<std::mutex> g(mutex_);
    if (!loaded_) load_locked();
    loaded_ = true;
    for (const auto& s : servers_)
        if (s.name == name) return s.preferred;
    return 0;
}

std::vector<NexusServer> NexusServers::known() const {
    std::vector<NexusServer> out;
    for (const auto& s : all())
        if (s.preferred <= 0) out.push_back(s);
    return out;
}

std::vector<NexusServer> NexusServers::preferred() const {
    std::vector<NexusServer> out;
    for (const auto& s : all())
        if (s.preferred > 0) out.push_back(s);
    std::stable_sort(out.begin(), out.end(),
                     [](const NexusServer& a, const NexusServer& b) {
                         return a.preferred > b.preferred;
                     });
    return out;
}

void NexusServers::record_discovered(const std::string& name, bool premium) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> g(mutex_);
    if (!loaded_) load_locked();
    loaded_ = true;

    for (auto& s : servers_) {
        if (s.name == name) {
            s.premium = premium;
            s.last_seen = today();
            save_locked();
            return;
        }
    }

    NexusServer s;
    s.name = name;
    s.premium = premium;
    s.last_seen = today();
    // MO2: the CDN entry starts in the Preferred list (preferred=1) so it
    // outranks anonymous mirrors before the user expresses a preference.
    if (contains_ci(name, "CDN")) s.preferred = 1;
    servers_.push_back(std::move(s));
    save_locked();
}

void NexusServers::record_speed(const std::string& name, double bps) {
    if (name.empty() || bps <= 0) return;
    std::lock_guard<std::mutex> g(mutex_);
    if (!loaded_) load_locked();
    loaded_ = true;

    for (auto& s : servers_) {
        if (s.name == name) {
            s.last_seen = today();
            if (s.speeds.size() >= static_cast<size_t>(kMaxSamples)) {
                // Rolling buffer: drop the oldest sample.
                std::rotate(s.speeds.begin(), s.speeds.begin() + 1, s.speeds.end());
                s.speeds.back() = static_cast<int>(bps);
            } else {
                s.speeds.push_back(static_cast<int>(bps));
            }
            save_locked();
            return;
        }
    }

    // Speed for a server we've never seen in a link list (e.g. the effective
    // host of a redirect) - register it as a known server.
    NexusServer s;
    s.name = name;
    s.last_seen = today();
    s.speeds.push_back(static_cast<int>(bps));
    servers_.push_back(std::move(s));
    save_locked();
}

void NexusServers::set_preferred(const std::vector<std::string>& ordered_names) {
    std::lock_guard<std::mutex> g(mutex_);
    if (!loaded_) load_locked();
    loaded_ = true;

    for (auto& s : servers_) s.preferred = 0;

    const int count = static_cast<int>(ordered_names.size());
    for (int i = 0; i < count; ++i) {
        const int rank = count - i;  // top of the list = highest rank
        for (auto& s : servers_) {
            if (s.name == ordered_names[i]) {
                s.preferred = rank;
                break;
            }
        }
    }
    save_locked();
}

void NexusServers::clear_all() {
    std::lock_guard<std::mutex> g(mutex_);
    servers_.clear();
    loaded_ = true;
    save_locked();
}

} // namespace engine
