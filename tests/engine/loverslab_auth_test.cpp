// LoversLabAuth keyring storage + LoversLabProvider URL-parsing tests (Qt-free).
// No network is involved: the injected keyring is a FileKeyring over a temp
// dir and the Content-Disposition parsing is pure string handling.
#include "engine/download/curl_download.h"
#include "engine/loverslab_auth.h"
#include "engine/source/loverslab_provider.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;


#define CHECK(cond, msg)                                               \
    do {                                                               \
        INFO(msg);                                                     \
        REQUIRE(cond);                                                 \
    } while (0)

static fs::path temp_dir(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 ("gmm_loverslab_test_" + std::string(tag) + "_" +
                  std::to_string(getpid()));
    fs::create_directories(p);
    return p;
}

TEST_CASE("loverslab auth", "[engine]") {
    // LoversLabAuth's config_dir() follows XDG_CONFIG_HOME; redirect it to a
    // temp dir BEFORE the singleton is first touched so nothing writes to the
    // real user config.
    fs::path config = temp_dir("config");
    setenv("XDG_CONFIG_HOME", config.c_str(), 1);

    // ---- LoversLabAuth with injected keyring ----------------------
    {
        fs::path primary_dir = temp_dir("primary");
        engine::LoversLabAuth::instance().set_keyring(
            std::make_unique<engine::FileKeyring>(primary_dir));

        auto& auth = engine::LoversLabAuth::instance();
        CHECK(!auth.has_cookie(), "no cookie initially");
        // __cf_bm is a Cloudflare challenge cookie and must be stripped by the
        // sanitizer on both store and read.
        const std::string raw_cookie = "memberID=12345; pass_hash=abc; __cf_bm=xyz";
        const std::string clean_cookie = "memberID=12345; pass_hash=abc";
        auth.set_cookie(raw_cookie);
        CHECK(auth.has_cookie(), "cookie present after set");
        CHECK(auth.get_cookie() == clean_cookie,
              "get returns the sanitized cookie (cf/__cf cookies dropped)");
        CHECK(fs::exists(primary_dir / "keyring_loverslab_cookie.dat"),
              "cookie stored in injected keyring");
        auth.clear_cookie();
        CHECK(!auth.has_cookie(), "cookie gone after clear");
    }

    // ---- LoversLabAuth fallback to internal file storage ----------
    {
        auto& auth = engine::LoversLabAuth::instance();
        auth.set_keyring(nullptr);  // force the file fallback path
        auth.set_cookie("fallback=xyz");
        CHECK(auth.has_cookie(), "fallback has cookie");
        CHECK(auth.get_cookie() == "fallback=xyz",
              "fallback get returns cookie");
        CHECK(fs::exists(config / "GameModManager" / "keyring_loverslab_cookie.dat"),
              "fallback wrote its file");
        auth.clear_cookie();
        CHECK(!auth.has_cookie(), "fallback cleared");
    }

    // ---- redact() -------------------------------------------------
    {
        CHECK(engine::LoversLabAuth::redact("") == "<empty>", "empty redact");
        const std::string c = "memberID=12345; pass_hash=abcdef";
        CHECK(engine::LoversLabAuth::redact(c) ==
                  "memberID=… (" + std::to_string(c.size()) + " bytes)",
              "redact keeps only the first cookie name and size");
        const std::string bare = "no-equals-sign";
        CHECK(engine::LoversLabAuth::redact(bare) ==
                  "<cookie>=… (" + std::to_string(bare.size()) + " bytes)",
              "redact without '=' names the cookie generically");
    }

    // ---- LoversLabProvider URL parsing ----------------------------
    {
        CHECK(engine::LoversLabProvider::is_loverslab_url(
                  "https://www.loverslab.com/files/file/4242-slug/?do=download&r=7"),
              "valid file-page URL accepted");
        CHECK(engine::LoversLabProvider::is_loverslab_url(
                  "https://loverslab.com/files/file/4242/"),
              "bare loverslab.com host accepted");
        CHECK(!engine::LoversLabProvider::is_loverslab_url(
                  "https://evil-loverslab.com/files/file/4242/"),
              "suffix-spoof host rejected");
        CHECK(!engine::LoversLabProvider::is_loverslab_url(
                  "https://www.loverslab.com/topic/4242/"),
              "non-file-page rejected");

        CHECK(engine::LoversLabProvider::extract_file_id(
                  "https://www.loverslab.com/files/file/4242-slug/?do=download") == "4242",
              "file id before slug");
        CHECK(engine::LoversLabProvider::extract_file_id(
                  "https://www.loverslab.com/files/file/999/") == "999",
              "bare id");
        CHECK(engine::LoversLabProvider::extract_file_id(
                  "https://www.loverslab.com/files/file/abc-slug/").empty(),
              "non-numeric id rejected");
        CHECK(engine::LoversLabProvider::extract_file_id(
                  "https://www.loverslab.com/topic/4242/").empty(),
              "no file marker -> empty");

        CHECK(engine::LoversLabProvider::mod_page_url(
                  "https://www.loverslab.com/files/file/4242-slug/?do=download&r=7") ==
                  "https://www.loverslab.com/files/file/4242-slug/",
              "download query stripped -> page URL");
        CHECK(engine::LoversLabProvider::mod_page_url(
                  "https://www.loverslab.com/files/file/4242/") ==
                  "https://www.loverslab.com/files/file/4242/",
              "bare page link unchanged");
    }

    // ---- Content-Disposition / percent-decode parsing -------------
    {
        CHECK(engine::download::parse_content_disposition_filename(
                  "attachment; filename=\"mod.7z\"") == "mod.7z",
              "quoted filename");
        CHECK(engine::download::parse_content_disposition_filename(
                  "attachment; filename=mod.7z") == "mod.7z",
              "unquoted filename");
        CHECK(engine::download::parse_content_disposition_filename(
                  "attachment; filename*=UTF-8''a%20b.7z") == "a b.7z",
              "RFC 5987 filename* decoded");
        CHECK(engine::download::parse_content_disposition_filename(
                  "inline; filename=\"../etc/passwd\"") == "passwd",
              "directory components stripped");
        CHECK(engine::download::parse_content_disposition_filename("").empty(),
              "empty header -> empty");
        CHECK(engine::download::percent_decode("a%20b%2Fc") == "a b/c",
              "percent decode");
        CHECK(engine::download::percent_decode("plain") == "plain",
              "no escapes unchanged");
    }

    // ---- sanitize_cookie() ----------------------------------------
    {
        using S = engine::LoversLabAuth;
        CHECK(S::sanitize_cookie("a=1; b=2; c=3") == "a=1; b=2; c=3",
              "standard ;-joined form unchanged");
        CHECK(S::sanitize_cookie("a=1\nb=2\nc=3") == "a=1; b=2; c=3",
              "newline-separated lines joined");
        CHECK(S::sanitize_cookie("a=1\r\nb=2") == "a=1; b=2",
              "CRLF-separated lines joined");
        CHECK(S::sanitize_cookie("ips4_member_id\t\"3430506\"\n"
                                 "ips4_loggedIn\t\"1785797880\"") ==
                  "ips4_member_id=3430506; ips4_loggedIn=1785797880",
              "devtools tab-separated Request Cookies block normalized");
        CHECK(S::sanitize_cookie("a=\"quoted\"; b=2") == "a=quoted; b=2",
              "quoted values unquoted");
        CHECK(S::sanitize_cookie("a=1; cf_clearance=xyz; b=2") == "a=1; b=2",
              "cf_clearance dropped");
        CHECK(S::sanitize_cookie("a=1; __cf_bm=xyz; b=2") == "a=1; b=2",
              "__cf_* dropped");
        CHECK(S::sanitize_cookie("malformed; a=1; =2; ;b=3") == "a=1; b=3",
              "malformed / empty tokens skipped");
        CHECK(S::sanitize_cookie("  a=1  ;  b = 2  ") == "a=1; b=2",
              "surrounding whitespace trimmed");
        CHECK(S::sanitize_cookie("") == "", "empty stays empty");
        CHECK(S::sanitize_cookie("   \n  ") == "", "whitespace-only stays empty");
        CHECK(S::sanitize_cookie("ips4_dataLayerLogin=eyJ0IjoxfQ==; a=b") ==
                  "ips4_dataLayerLogin=eyJ0IjoxfQ==; a=b",
              "values containing '=' kept intact (first '=' splits)");

        // JSON export form: [{"name": "...", "value": "..."}, ...].
        const std::string json =
            "[ {\"name\": \"ips4_member_id\", \"value\": \"3430506\"}, "
            "{\"name\": \"ips4_IPSSessionFront\", \"value\": \"4lj0j9\"} ]";
        CHECK(S::sanitize_cookie(json) ==
                  "ips4_member_id=3430506; ips4_IPSSessionFront=4lj0j9",
              "JSON export form parsed");
        const std::string json_cf =
            "[ {\"name\": \"cf_clearance\", \"value\": \"abc\"}, "
            "{\"name\": \"ips4_member_id\", \"value\": \"3430506\"} ]";
        CHECK(S::sanitize_cookie(json_cf) == "ips4_member_id=3430506",
              "JSON form drops cf_clearance");
        const std::string json_no_match =
            "[ {\"name\": \"ips4_member_id\", \"domain\": \"loverslab.com\"} ]";
        CHECK(S::sanitize_cookie(json_no_match).empty(),
              "JSON form with no value pairs yields nothing");
    }

}
