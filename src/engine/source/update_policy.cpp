#include "engine/source/update_policy.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace engine::Source {

// ---------------------------------------------------------------------------
// parse_source_pin
// ---------------------------------------------------------------------------

SourcePin parse_source_pin(const std::string& source_json) {
    SourcePin pin;
    try {
        auto j = nlohmann::json::parse(source_json);
        if (!j.is_object()) return pin;

        // updatePolicy: "exact" or "latest", default "exact"
        if (j.contains("updatePolicy")) {
            auto policy_str = j["updatePolicy"].get<std::string>();
            if (policy_str == "latest")
                pin.policy = UpdatePolicy::Latest;
            else
                pin.policy = UpdatePolicy::Exact;  // "exact" or unknown → exact
        }

        // Common fields across all providers
        if (j.contains("version") && j["version"].is_string())
            pin.version = j["version"].get<std::string>();
        if (j.contains("sha256") && j["sha256"].is_string())
            pin.sha256 = j["sha256"].get<std::string>();
        if (j.contains("fileId") && j["fileId"].is_number_integer())
            pin.file_id = j["fileId"].get<int64_t>();
        if (j.contains("fileSize") && j["fileSize"].is_number_integer())
            pin.file_size = j["fileSize"].get<int64_t>();
        if (j.contains("fileName") && j["fileName"].is_string())
            pin.file_name = j["fileName"].get<std::string>();

    } catch (const nlohmann::json::exception&) {
        // Malformed JSON → return empty pin (all defaults)
    }
    return pin;
}

// ---------------------------------------------------------------------------
// verify_resolved
// ---------------------------------------------------------------------------

VerifyResult verify_resolved(const SourcePin& pin, const ResolvedIdentity& resolved) {
    VerifyResult result;

    if (pin.policy == UpdatePolicy::Latest) {
        result.verdict = PinVerdict::NoPin;
        result.message = "latest policy - no pin to verify";
        return result;
    }

    // Exact policy: check sha256, file_id, file_size in priority order
    bool has_sha256  = !pin.sha256.empty();
    bool has_file_id = pin.file_id > 0;
    bool has_size    = pin.file_size > 0;

    if (!has_sha256 && !has_file_id && !has_size) {
        result.verdict = PinVerdict::Incomplete;
        result.message = "exact policy but pin has no hash, fileId, or fileSize";
        return result;
    }

    bool any_match = false;

    if (has_sha256 && !resolved.sha256.empty()) {
        if (pin.sha256 == resolved.sha256) {
            any_match = true;
        } else {
            result.verdict = PinVerdict::Mismatch;
            result.message = "SHA-256 mismatch: expected " + pin.sha256 +
                             " but got " + resolved.sha256;
            return result;
        }
    }

    if (has_file_id && resolved.file_id > 0) {
        if (pin.file_id == resolved.file_id) {
            any_match = true;
        } else if (!has_sha256) {
            // fileId mismatch and no sha256 to override → mismatch
            result.verdict = PinVerdict::Mismatch;
            result.message = "fileId mismatch: expected " + std::to_string(pin.file_id) +
                             " but got " + std::to_string(resolved.file_id);
            return result;
        }
    }

    if (has_size && resolved.file_size > 0) {
        if (pin.file_size == resolved.file_size) {
            any_match = true;
        } else if (!has_sha256 && !has_file_id) {
            result.verdict = PinVerdict::Mismatch;
            result.message = "fileSize mismatch: expected " + std::to_string(pin.file_size) +
                             " but got " + std::to_string(resolved.file_size);
            return result;
        }
    }

    if (any_match) {
        result.verdict = PinVerdict::Match;
        result.message = "pin verified successfully";
    } else {
        // Pin fields present but none could be compared (resolved has no data)
        result.verdict = PinVerdict::Incomplete;
        result.message = "exact policy pin could not be verified - no comparable data in resolved identity";
    }

    return result;
}

// ---------------------------------------------------------------------------
// compute_file_sha256
// ---------------------------------------------------------------------------

std::string compute_file_sha256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) return {};

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    char buf[8192];
    while (file.good()) {
        file.read(buf, sizeof(buf));
        auto count = file.gcount();
        if (count > 0) {
            if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(count)) != 1) {
                EVP_MD_CTX_free(ctx);
                return {};
            }
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);

    // Convert to lowercase hex
    std::string result;
    result.reserve(hash_len * 2);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < hash_len; ++i) {
        result.push_back(hex[hash[i] >> 4]);
        result.push_back(hex[hash[i] & 0x0f]);
    }
    return result;
}

// ---------------------------------------------------------------------------
// verify_file_hash
// ---------------------------------------------------------------------------

bool verify_file_hash(const std::string& expected_sha256,
                       const std::string& file_path) {
    if (expected_sha256.empty()) return false;
    std::string actual = compute_file_sha256(file_path);
    if (actual.empty()) return false;
    return actual == expected_sha256;
}

} // namespace engine::Source
