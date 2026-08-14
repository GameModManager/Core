#include "engine/source/nexus_account.h"

#include "engine/log/logger.h"
#include "engine/source/nexus_auth.h"
#include "engine/source/nexus_http.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>

namespace engine {

namespace {
constexpr const char* kValidateUrl =
    "https://api.nexusmods.com/v1/users/validate.json";
} // namespace

void parse_rate_limits(const std::string& headers) {
    auto find_header = [&](const std::string& name) -> int64_t {
        auto pos = headers.find(name + ": ");
        if (pos == std::string::npos) {
            // try lowercase
            std::string lower = name;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            pos = headers.find(lower + ": ");
            if (pos == std::string::npos) return -1;
        }
        pos = headers.find(':', pos) + 1;
        while (pos < headers.size() && headers[pos] == ' ') ++pos;
        int64_t val = 0;
        while (pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9') {
            val = val * 10 + (headers[pos] - '0');
            ++pos;
        }
        return val;
    };

    // Nexus reports two budgets, each with its own reset (MO2
    // NexusInterface::parseLimits reads the same plain headers). Prefer the
    // authenticated variants (they reflect the API-key quota), fall back to
    // the plain ones.
    auto pick = [&](const std::string& primary, const std::string& fallback) -> int64_t {
        int64_t v = find_header(primary);
        return v >= 0 ? v : find_header(fallback);
    };

    int64_t daily_limit     = pick("x-rl-authenticated-daily-limit", "x-rl-daily-limit");
    int64_t daily_remaining = pick("x-rl-authenticated-daily-remaining", "x-rl-daily-remaining");
    int64_t daily_reset     = pick("x-rl-authenticated-daily-reset", "x-rl-daily-reset");
    int64_t hourly_limit    = pick("x-rl-authenticated-hourly-limit", "x-rl-hourly-limit");
    int64_t hourly_remaining = pick("x-rl-authenticated-hourly-remaining", "x-rl-hourly-remaining");
    int64_t hourly_reset    = pick("x-rl-authenticated-hourly-reset", "x-rl-hourly-reset");

    if (daily_limit > 0 || hourly_limit > 0) {
        NexusAuth::instance().update_rate_limit(
            static_cast<int>(hourly_limit),
            static_cast<int>(hourly_remaining),
            hourly_reset,
            static_cast<int>(daily_limit),
            static_cast<int>(daily_remaining),
            daily_reset);
    }
}

NexusValidateResult validate_nexus_account() {
    NexusValidateResult result;
    auto& auth = NexusAuth::instance();

    if (!auth.has_api_key()) {
        result.message = "No API key stored - enter one first.";
        return result;
    }
    const std::string api_key = auth.get_api_key();
    if (api_key.empty()) {
        result.message = "Stored API key is empty.";
        return result;
    }

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("apikey: " + api_key).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");

    std::string response;
    std::string resp_headers;
    long http_code = 0;
    const bool ok = nexus_http_request(kValidateUrl, "", response, http_code,
                                       headers, &resp_headers, 10);
    curl_slist_free_all(headers);

    if (resp_headers.size() > 20)  // sanity check - don't parse empty/trivial
        parse_rate_limits(resp_headers);

    if (!ok) {
        result.message = "Failed to reach Nexus (network error).";
        return result;
    }
    if (http_code == 401 || http_code == 403) {
        result.message =
            "API key rejected by Nexus (HTTP " + std::to_string(http_code)
            + "). Check your key at nexusmods.com/users/myaccount?tab=api.";
        return result;
    }
    if (http_code != 200) {
        result.message = "Nexus returned HTTP " + std::to_string(http_code) + ".";
        return result;
    }

    try {
        auto j = nlohmann::json::parse(response);

        NexusUserInfo info;
        const auto& uid = j["user_id"];
        if (uid.is_string())
            info.user_id = uid.get<std::string>();
        else if (uid.is_number_integer())
            info.user_id = std::to_string(uid.get<long long>());

        info.name = j.value("name", "");

        const bool premium   = j.value("is_premium", false);
        const bool supporter = j.value("is_supporter", false);
        if (premium)
            info.account_type = NexusUserInfo::AccountType::Premium;
        else if (supporter)
            info.account_type = NexusUserInfo::AccountType::Supporter;
        else
            info.account_type = NexusUserInfo::AccountType::Regular;

        auth.set_user_info(info);
        result.ok = true;
    } catch (const std::exception& e) {
        result.message = "Failed to parse Nexus response: " + std::string(e.what());
        Logger::instance().error("NexusAccount: " + result.message);
    }

    return result;
}

} // namespace engine
