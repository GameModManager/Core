#include "engine/source/update_policy.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace engine::Source;

// ---------------------------------------------------------------------------
// parse_source_pin tests
// ---------------------------------------------------------------------------

TEST_CASE("parse_source_pin: exact policy with all fields", "[update_policy]") {
  auto pin = parse_source_pin(R"({
        "provider": "nexus",
        "updatePolicy": "exact",
        "version": "1.4.2",
        "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "fileId": 67890,
        "fileSize": 48213311,
        "fileName": "SkyUI-1.4.2.7z"
    })");

  REQUIRE(pin.policy == UpdatePolicy::Exact);
  REQUIRE(pin.version == "1.4.2");
  REQUIRE(pin.sha256 ==
          "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
  REQUIRE(pin.file_id == 67890);
  REQUIRE(pin.file_size == 48213311);
  REQUIRE(pin.file_name == "SkyUI-1.4.2.7z");
  std::printf("PASS: parse_source_pin — exact with all fields\n");
}

TEST_CASE("parse_source_pin: latest policy omits pin fields", "[update_policy]") {
  auto pin = parse_source_pin(R"({
        "provider": "nexus",
        "updatePolicy": "latest",
        "gameDomain": "skyrimspecialedition",
        "modId": 12345
    })");

  REQUIRE(pin.policy == UpdatePolicy::Latest);
  REQUIRE(pin.version.empty());
  REQUIRE(pin.sha256.empty());
  REQUIRE(pin.file_id == 0);
  REQUIRE(pin.file_size == 0);
  std::printf("PASS: parse_source_pin — latest omits pin fields\n");
}

TEST_CASE("parse_source_pin: missing updatePolicy defaults to exact",
          "[update_policy]") {
  auto pin = parse_source_pin(R"({
        "provider": "nexus",
        "version": "2.0"
    })");

  REQUIRE(pin.policy == UpdatePolicy::Exact);
  REQUIRE(pin.version == "2.0");
  std::printf("PASS: parse_source_pin — missing updatePolicy defaults to exact\n");
}

TEST_CASE("parse_source_pin: malformed JSON returns empty pin", "[update_policy]") {
  auto pin = parse_source_pin("not json at all {{{");
  REQUIRE(pin.policy == UpdatePolicy::Exact);
  REQUIRE(pin.version.empty());
  REQUIRE(pin.sha256.empty());
  REQUIRE(pin.file_id == 0);
  std::printf("PASS: parse_source_pin — malformed JSON returns empty pin\n");
}

TEST_CASE("parse_source_pin: empty string returns empty pin", "[update_policy]") {
  auto pin = parse_source_pin("");
  REQUIRE(pin.policy == UpdatePolicy::Exact);
  REQUIRE(pin.version.empty());
  std::printf("PASS: parse_source_pin — empty string returns empty pin\n");
}

TEST_CASE("parse_source_pin: non-object JSON returns empty pin", "[update_policy]") {
  auto pin = parse_source_pin(R"(["array", "not", "object"])");
  REQUIRE(pin.policy == UpdatePolicy::Exact);
  REQUIRE(pin.version.empty());
  std::printf("PASS: parse_source_pin — non-object JSON returns empty pin\n");
}

TEST_CASE("parse_source_pin: loverslab exact with sha256", "[update_policy]") {
  auto pin = parse_source_pin(R"({
        "provider": "loverslab",
        "resolution": "browser",
        "modId": "some-mod",
        "sectionSlug": "skyrimspecialedition",
        "updatePolicy": "exact",
        "version": "3.1.0",
        "sha256": "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"
    })");

  REQUIRE(pin.policy == UpdatePolicy::Exact);
  REQUIRE(pin.version == "3.1.0");
  REQUIRE(pin.sha256 ==
          "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
  REQUIRE(pin.file_id == 0);  // LoversLab doesn't have fileId
  std::printf("PASS: parse_source_pin — loverslab exact with sha256\n");
}

TEST_CASE("parse_source_pin: steam_workshop forces latest", "[update_policy]") {
  auto pin = parse_source_pin(R"({
        "provider": "steam_workshop",
        "resolution": "client-subscription",
        "appId": 72850,
        "workshopItemId": 123456,
        "updatePolicy": "latest"
    })");

  REQUIRE(pin.policy == UpdatePolicy::Latest);
  std::printf("PASS: parse_source_pin — steam_workshop forces latest\n");
}

TEST_CASE("parse_source_pin: direct with exact", "[update_policy]") {
  auto pin = parse_source_pin(R"({
        "provider": "direct",
        "resolution": "api",
        "url": "https://example.com/mod-v2.zip",
        "updatePolicy": "exact",
        "version": "2.0.0",
        "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    })");

  REQUIRE(pin.policy == UpdatePolicy::Exact);
  REQUIRE(pin.version == "2.0.0");
  REQUIRE(pin.file_name.empty());  // no fileName in this JSON
  std::printf("PASS: parse_source_pin — direct with exact\n");
}

// ---------------------------------------------------------------------------
// verify_resolved tests
// ---------------------------------------------------------------------------

TEST_CASE("verify_resolved: exact match on sha256", "[update_policy]") {
  SourcePin pin;
  pin.policy = UpdatePolicy::Exact;
  pin.sha256 = "abc123";

  ResolvedIdentity resolved;
  resolved.sha256 = "abc123";

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Match);
  REQUIRE(result.message.find("verified") != std::string::npos);
  std::printf("PASS: verify_resolved — exact match on sha256\n");
}

TEST_CASE("verify_resolved: exact mismatch on sha256", "[update_policy]") {
  SourcePin pin;
  pin.policy = UpdatePolicy::Exact;
  pin.sha256 = "expected_hash";

  ResolvedIdentity resolved;
  resolved.sha256 = "actual_hash";

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Mismatch);
  REQUIRE(result.message.find("SHA-256 mismatch") != std::string::npos);
  std::printf("PASS: verify_resolved — exact mismatch on sha256\n");
}

TEST_CASE("verify_resolved: exact match on file_id", "[update_policy]") {
  SourcePin pin;
  pin.policy  = UpdatePolicy::Exact;
  pin.file_id = 67890;

  ResolvedIdentity resolved;
  resolved.file_id = 67890;

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Match);
  std::printf("PASS: verify_resolved — exact match on file_id\n");
}

TEST_CASE("verify_resolved: exact mismatch on file_id", "[update_policy]") {
  SourcePin pin;
  pin.policy  = UpdatePolicy::Exact;
  pin.file_id = 67890;

  ResolvedIdentity resolved;
  resolved.file_id = 99999;

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Mismatch);
  REQUIRE(result.message.find("fileId mismatch") != std::string::npos);
  std::printf("PASS: verify_resolved — exact mismatch on file_id\n");
}

TEST_CASE("verify_resolved: exact match on file_size", "[update_policy]") {
  SourcePin pin;
  pin.policy    = UpdatePolicy::Exact;
  pin.file_size = 48213311;

  ResolvedIdentity resolved;
  resolved.file_size = 48213311;

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Match);
  std::printf("PASS: verify_resolved — exact match on file_size\n");
}

TEST_CASE("verify_resolved: exact mismatch on file_size", "[update_policy]") {
  SourcePin pin;
  pin.policy    = UpdatePolicy::Exact;
  pin.file_size = 48213311;

  ResolvedIdentity resolved;
  resolved.file_size = 100;

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Mismatch);
  REQUIRE(result.message.find("fileSize mismatch") != std::string::npos);
  std::printf("PASS: verify_resolved — exact mismatch on file_size\n");
}

TEST_CASE("verify_resolved: sha256 mismatch overrides file_id match",
          "[update_policy]") {
  SourcePin pin;
  pin.policy  = UpdatePolicy::Exact;
  pin.sha256  = "expected";
  pin.file_id = 67890;

  ResolvedIdentity resolved;
  resolved.sha256  = "actual";  // different
  resolved.file_id = 67890;     // same

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Mismatch);
  REQUIRE(result.message.find("SHA-256 mismatch") != std::string::npos);
  std::printf("PASS: verify_resolved — sha256 mismatch overrides file_id match\n");
}

TEST_CASE("verify_resolved: latest always returns NoPin", "[update_policy]") {
  SourcePin pin;
  pin.policy = UpdatePolicy::Latest;

  ResolvedIdentity resolved;
  resolved.sha256 = "anything";

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::NoPin);
  REQUIRE(result.message.find("latest") != std::string::npos);
  std::printf("PASS: verify_resolved — latest always returns NoPin\n");
}

TEST_CASE("verify_resolved: exact with no comparable fields", "[update_policy]") {
  SourcePin pin;
  pin.policy = UpdatePolicy::Exact;
  // All fields default to empty/zero

  ResolvedIdentity resolved;
  resolved.sha256 = "something";

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Incomplete);
  std::printf(
      "PASS: verify_resolved — exact with no comparable fields returns Incomplete\n");
}

TEST_CASE("verify_resolved: exact with pin but empty resolved", "[update_policy]") {
  SourcePin pin;
  pin.policy = UpdatePolicy::Exact;
  pin.sha256 = "abc123";

  ResolvedIdentity resolved;
  // All empty/zero

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Incomplete);
  std::printf(
      "PASS: verify_resolved — exact with pin but empty resolved returns Incomplete\n");
}

// ---------------------------------------------------------------------------
// compute_file_sha256 / verify_file_hash tests
// ---------------------------------------------------------------------------

TEST_CASE("compute_file_sha256: known content", "[update_policy]") {
  // echo -n "hello world" | sha256sum
  // = b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
  auto tmp = std::filesystem::temp_directory_path() / "gmm_update_policy_test_sha";
  std::ofstream f(tmp);
  f << "hello world";
  f.close();

  auto hash = compute_file_sha256(tmp.string());
  REQUIRE(hash == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  std::printf("PASS: compute_file_sha256 — known content matches\n");
}

TEST_CASE("compute_file_sha256: empty file", "[update_policy]") {
  auto tmp = std::filesystem::temp_directory_path() / "gmm_update_policy_test_empty";
  std::ofstream f(tmp);
  f.close();

  auto hash = compute_file_sha256(tmp.string());
  // SHA-256 of empty string
  REQUIRE(hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  std::printf("PASS: compute_file_sha256 — empty file matches known hash\n");
}

TEST_CASE("compute_file_sha256: nonexistent file returns empty", "[update_policy]") {
  auto hash = compute_file_sha256("/nonexistent/path/file.bin");
  REQUIRE(hash.empty());
  std::printf("PASS: compute_file_sha256 — nonexistent file returns empty\n");
}

TEST_CASE("verify_file_hash: matching hash", "[update_policy]") {
  auto tmp = std::filesystem::temp_directory_path() / "gmm_update_policy_test_verify";
  std::ofstream f(tmp);
  f << "test data for hash verification";
  f.close();

  auto hash = compute_file_sha256(tmp.string());
  REQUIRE(verify_file_hash(hash, tmp.string()));

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  std::printf("PASS: verify_file_hash — matching hash returns true\n");
}

TEST_CASE("verify_file_hash: mismatching hash", "[update_policy]") {
  auto tmp =
      std::filesystem::temp_directory_path() / "gmm_update_policy_test_verify_mismatch";
  std::ofstream f(tmp);
  f << "test data";
  f.close();

  REQUIRE_FALSE(verify_file_hash(
      "0000000000000000000000000000000000000000000000000000000000000000",
      tmp.string()));

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  std::printf("PASS: verify_file_hash — mismatching hash returns false\n");
}

TEST_CASE("verify_file_hash: empty expected returns false", "[update_policy]") {
  auto tmp =
      std::filesystem::temp_directory_path() / "gmm_update_policy_test_verify_empty";
  std::ofstream f(tmp);
  f << "data";
  f.close();

  REQUIRE_FALSE(verify_file_hash("", tmp.string()));

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  std::printf("PASS: verify_file_hash — empty expected returns false\n");
}

// ---------------------------------------------------------------------------
// Integration: parse + verify round-trip
// ---------------------------------------------------------------------------

TEST_CASE("integration: parse exact nexus source, verify match", "[update_policy]") {
  // Step 1: parse a Nexus source block with exact policy
  auto pin = parse_source_pin(R"({
        "provider": "nexus",
        "resolution": "api",
        "gameDomain": "skyrimspecialedition",
        "modId": 12345,
        "fileId": 67890,
        "version": "1.4.2",
        "fileName": "SkyUI-1.4.2.7z",
        "fileSize": 48213311,
        "sha256": "aaaa1111",
        "updatePolicy": "exact"
    })");

  REQUIRE(pin.policy == UpdatePolicy::Exact);

  // Step 2: provider resolved the same file
  ResolvedIdentity resolved;
  resolved.sha256    = "aaaa1111";
  resolved.file_id   = 67890;
  resolved.file_size = 48213311;

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Match);
  std::printf("PASS: integration — parse + verify exact match round-trip\n");
}

TEST_CASE("integration: parse exact nexus source, verify mismatch", "[update_policy]") {
  auto pin = parse_source_pin(R"({
        "provider": "nexus",
        "updatePolicy": "exact",
        "sha256": "expected_hash_here",
        "version": "1.0.0"
    })");

  ResolvedIdentity resolved;
  resolved.sha256 = "different_hash_here";

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Mismatch);
  std::printf("PASS: integration — parse + verify exact mismatch round-trip\n");
}

TEST_CASE("integration: parse latest source, skip verification", "[update_policy]") {
  auto pin = parse_source_pin(R"({
        "provider": "nexus",
        "updatePolicy": "latest",
        "gameDomain": "skyrimspecialedition",
        "modId": 12345
    })");

  REQUIRE(pin.policy == UpdatePolicy::Latest);

  // After download, the resolved hash is recorded but never compared
  ResolvedIdentity resolved;
  resolved.sha256  = "whatever_was_downloaded";
  resolved.version = "1.5.0";

  auto result = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::NoPin);
  std::printf("PASS: integration — parse + verify latest skips verification\n");
}

TEST_CASE("integration: full file-hash verification", "[update_policy]") {
  // Create a test archive file
  auto tmp = std::filesystem::temp_directory_path() / "gmm_update_policy_integration";
  {
    std::ofstream f(tmp);
    f << "mod archive content for integration test";
  }

  // Compute its hash
  auto actual_hash = compute_file_sha256(tmp.string());
  REQUIRE(!actual_hash.empty());

  // Parse an exact pin declaring that hash
  auto j =
      R"({"updatePolicy":"exact","sha256":")" + actual_hash + R"(","version":"2.0"})";
  auto pin = parse_source_pin(j);
  REQUIRE(pin.policy == UpdatePolicy::Exact);
  REQUIRE(pin.sha256 == actual_hash);

  // Verify the hash matches
  REQUIRE(verify_file_hash(pin.sha256, tmp.string()));

  // Also verify via ResolvedIdentity
  ResolvedIdentity resolved;
  resolved.sha256 = actual_hash;
  auto result     = verify_resolved(pin, resolved);
  REQUIRE(result.verdict == PinVerdict::Match);

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  std::printf("PASS: integration — full file-hash verification round-trip\n");
}

}  // namespace
