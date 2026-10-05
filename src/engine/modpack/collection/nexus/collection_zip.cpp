#include "engine/modpack/collection/nexus/collection_zip.h"

#include "engine/log/logger.h"
#include "engine/mod/archive/archive_extractor.h"
#include "engine/network/network_manager.h"
#include "engine/network/nexus_v2/premium.h"
#include "engine/source/nexus/auth.h"
#include "engine/source/nexus/account.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace engine::Collection::Nexus {

namespace {

  // Where the collection.json lives inside the archive. Nexus always writes it
  // at the root; the fallback is for archives that wrapped everything in one
  // folder.
  const char *kJsonNames[] = {"collection.json"};

  // A guard against a download that returns something enormous. A collection
  // archive is a JSON file plus a handful of documents and images; anything
  // past this is not one, and streaming it to disk would be a way to fill a
  // disk rather than to read a collection.
  constexpr int64_t kMaxZipBytes = 64 * 1024 * 1024;

  std::string read_whole_file(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
      return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

}  // anonymous namespace

bool fetch_collection_json(const std::string &download_link, std::string &out_json,
                           std::string &out_error) {
  out_json.clear();
  out_error.clear();
  if (download_link.empty()) {
    out_error = "Nexus reported no download link for this revision";
    return false;
  }

  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "gmm-collection-nexus-fetch";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    out_error = "cannot create a temporary directory for the collection archive";
    return false;
  }
  const std::filesystem::path zip = dir / "collection.zip";

  network::DownloadRequest req;
  req.url        = download_link;
  req.dest       = zip;
  req.caller     = NET_CALLER;
  req.long_lived = true;
  // The GraphQL download link is served by Nexus's CDN, which authenticates the
  // same way every other Nexus endpoint does. Without a key the CDN answers
  // 401, which surfaces below as an ordinary HTTP failure the caller reports.
  if (const std::string api_key = Source::Nexus::Auth::instance().get_api_key();
      !api_key.empty()) {
    req.headers.push_back("apikey: " + api_key);
  }

  const network::DownloadResult res = network::instance().download(req);

  // Same rate-limit bookkeeping the API-key path does for every Nexus response.
  if (res.response_headers.size() > 20)
    Source::Nexus::Account::parse_rate_limits(res.response_headers);

  if (!res.ok) {
    out_error = "could not download the collection archive (HTTP " +
                std::to_string(res.http_code) + ")" +
                (res.error.empty() ? std::string() : ": " + res.error);
    std::filesystem::remove_all(dir, ec);
    return false;
  }

  std::error_code size_ec;
  const auto size = std::filesystem::file_size(zip, size_ec);
  if (!size_ec && size > kMaxZipBytes) {
    out_error = "the downloaded collection archive is implausibly large (" +
                std::to_string(size) + " bytes)";
    std::filesystem::remove_all(dir, ec);
    return false;
  }

  // The archive is read through the same libarchive-backed extractor the
  // install pipeline uses, rather than a second unzip of our own.
  // ponytail: the whole archive is extracted, not just collection.json. A
  // collection zip is metadata plus a few documents, so the cost is a few MB;
  // a single-entry reader is worth adding only if real archives turn out to
  // bundle mod assets.
  const std::filesystem::path unpacked = dir / "unpacked";
  std::vector<ExtractedFile> files;
  std::string extract_error;
  if (!ArchiveExtractor::extract(zip, unpacked, files, extract_error, {}, false)) {
    out_error = "could not read the collection archive: " + extract_error;
    std::filesystem::remove_all(dir, ec);
    return false;
  }

  // Locate collection.json: at the root, or one level down.
  std::filesystem::path found;
  for (const char *name : kJsonNames) {
    std::error_code probe_ec;
    const std::filesystem::path at_root = unpacked / name;
    if (std::filesystem::is_regular_file(at_root, probe_ec)) {
      found = at_root;
      break;
    }
    for (const auto &entry : std::filesystem::directory_iterator(unpacked, probe_ec)) {
      if (!entry.is_regular_file())
        continue;
      if (entry.path().filename() == name) {
        found = entry.path();
        break;
      }
    }
    if (!found.empty())
      break;
  }

  if (found.empty()) {
    out_error = "the collection archive contains no collection.json";
    std::filesystem::remove_all(dir, ec);
    return false;
  }

  out_json = read_whole_file(found);
  std::filesystem::remove_all(dir, ec);
  if (out_json.empty()) {
    out_error = "collection.json in the archive is empty or unreadable";
    return false;
  }

  Logger::instance().debug("[NexusCollection] read collection.json from " +
                           download_link + " (" + std::to_string(out_json.size()) +
                           " bytes)");
  return true;
}

}  // namespace engine::Collection::Nexus