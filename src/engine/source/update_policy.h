#pragma once

// updatePolicy engine - two-tier version resolution for mod sources.
//
// exact:  pin to a specific fileId/version/hash captured at pack authoring
//         time. Hash match proceeds silently; mismatch surfaces as a
//         non-blocking integrity warning and install proceeds with whatever
//         was actually obtained.
// latest: fetch whatever the source currently reports as newest. No
//         pack-declared hash to check; the resolved hash is recorded for
//         future incremental comparison.
//
// Source-agnostic - works for nexus, loverslab, modpub, steam_workshop,
// direct, or any future provider.

#include <cstdint>
#include <string>
#include <string_view>

namespace engine::Source {

// ---------------------------------------------------------------------------
// Update policy enum
// ---------------------------------------------------------------------------

enum class UpdatePolicy {
    Exact,   // pin to specific fileId/version/hash
    Latest,  // fetch whatever source currently has
};

// ---------------------------------------------------------------------------
// Source pin - pack-declared identity (from mods/<id>.json source block)
// ---------------------------------------------------------------------------

struct SourcePin {
    UpdatePolicy policy = UpdatePolicy::Exact;
    std::string version;
    std::string sha256;          // 64-char lowercase hex
    int64_t file_id = 0;
    int64_t file_size = 0;
    std::string file_name;
};

// ---------------------------------------------------------------------------
// Resolved identity - what the provider actually obtained
// ---------------------------------------------------------------------------

struct ResolvedIdentity {
    std::string version;
    std::string sha256;          // computed after download
    int64_t file_id = 0;
    int64_t file_size = 0;
};

// ---------------------------------------------------------------------------
// Verification result
// ---------------------------------------------------------------------------

enum class PinVerdict {
    Match,        // exact: hash matches - proceed silently
    Mismatch,     // exact: hash mismatch - non-blocking warning, install
                  //        proceeds with actual file
    NoPin,        // latest: nothing to check against
    Incomplete,   // exact: pin missing required fields (sha256/version)
};

struct VerifyResult {
    PinVerdict verdict = PinVerdict::NoPin;
    std::string message;  // human-readable outcome for diagnostics
};

// ---------------------------------------------------------------------------
// Core API
// ---------------------------------------------------------------------------

// Parse a source JSON object into a SourcePin. Extracts updatePolicy, version,
// sha256, fileId, fileSize, and fileName. Unknown or absent updatePolicy
// defaults to Exact. Returns an empty pin on malformed input.
SourcePin parse_source_pin(const std::string& source_json);

// Verify a resolved download against the declared pin.
// For Exact policy: compares sha256 if present, file_id if present, file_size
//   if present. At least one match field must be present; if sha256 is present
//   it takes precedence.
// For Latest policy: always returns NoPin (nothing to check).
VerifyResult verify_resolved(const SourcePin& pin, const ResolvedIdentity& resolved);

// Verify a downloaded file's SHA-256 against an expected hash.
// Returns true if hashes match, false otherwise.
// Returns false if the file cannot be read.
bool verify_file_hash(const std::string& expected_sha256,
                       const std::string& file_path);

// Compute SHA-256 of a file. Returns the 64-char lowercase hex string,
// or empty string on failure.
std::string compute_file_sha256(const std::string& file_path);

} // namespace engine::Source
