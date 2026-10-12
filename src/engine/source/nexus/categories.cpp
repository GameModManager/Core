#include "engine/source/nexus/categories.h"

#include "engine/log/logger.h"
#include "engine/source/nexus/auth.h"
#include "engine/source/nexus/http.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace engine::Source::Nexus {

CategoryList parse_categories(const std::string &body) {
  CategoryList list;
  try {
    const auto j = nlohmann::json::parse(body);
    // Nexus wraps every legacy-API payload in "data".
    if (!j.contains("data") || !j["data"].is_object() ||
        !j["data"].contains("categories") || !j["data"]["categories"].is_array())
      return list;
    for (const auto &row : j["data"]["categories"]) {
      if (!row.is_object() || !row.contains("category_id") ||
          !row["category_id"].is_number_integer())
        continue;
      NexusCategory cat;
      cat.id   = row["category_id"].get<int>();
      cat.name = row.contains("name") && row["name"].is_string()
                     ? row["name"].get<std::string>()
                     : std::string();
      // "parent_category" is the owning category's id, or the literal false
      // when this category is a root one. Anything else (null, a string, a
      // missing field) is not something to guess a parent from, so the row
      // becomes a root instead.
      if (row.contains("parent_category") && row["parent_category"].is_number_integer())
        cat.parent_id = row["parent_category"].get<int>();
      if (cat.id == 0)
        continue;  // 0 is "None" locally, never a real Nexus category
      list.categories.push_back(std::move(cat));
    }
  } catch (...) {
    return list;  // unparseable: ok stays false
  }
  list.ok = !list.categories.empty();
  return list;
}

// Fetches the categories of one Nexus game domain (e.g. "skyrimspecialedition").
// Needs a configured API key; ok=false without one, on an HTTP error, or on an
// unparseable body. Never throws.
CategoryList fetch_categories(const std::string &nexus_domain) {
  CategoryList list;
  if (nexus_domain.empty())
    return list;

  const std::string api_key = Auth::instance().get_api_key();
  if (api_key.empty())
    return list;

  // MO2 fetches the category list from the game-info resource, not from a
  // categories sub-resource: NXMRequestInfo::TYPE_GAMEINFO builds
  // "%1/games/%2" (references/modorganizer/src/nexusinterface.cpp:992-994) and
  // CategoriesDialog::nxmGameInfoAvailable reads the "categories" array out of
  // that reply (categoriesdialog.cpp:363-377). Same URL, and the categories come
  // under the same "data" envelope every other v1 call in this provider uses.
  const std::string url = "https://api.nexusmods.com/v1/games/" + nexus_domain;
  curl_slist *headers   = nullptr;
  headers               = curl_slist_append(headers, ("apikey: " + api_key).c_str());
  headers               = curl_slist_append(headers, "Accept: application/json");

  std::string response;
  long http_code = 0;
  const bool ok =
      Http::nexus_http_request(url, "", response, http_code, headers, nullptr);
  curl_slist_free_all(headers);

  if (!ok || http_code != 200) {
    Logger::instance().error(
        "[NexusCategories] category request failed for game=" + nexus_domain +
        " (HTTP " + std::to_string(http_code) + ")");
    return list;
  }
  return parse_categories(response);
}

}  // namespace engine::Source::Nexus