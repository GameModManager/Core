#pragma once

// Source-agnostic download path routing for collections / modpacks.
//
// Answers one question per mod: can this source auto-download, or does the
// user need a browser flow? Routes on the manifest's declared resolution
// plus account status and source capabilities - never on provider identity,
// so any source with tiered access (free/premium, session/anonymous, ...) reuses
// the same decision. Nexus collections arrive through the nxm:// API; the
// Nexus premium-vs-free split is just one row in capabilities_for().
//
// Decision order in route_download():
//   1. declared ClientSubscription (Steam Workshop) -> ExternalClient, always.
//   2. declared Browser (page URL only, no direct link) -> Browser, always.
//   3. declared Api -> Auto when the backend + account allow it, else Browser
//      with a reason naming what is missing (auth, tier, or backend support).
//
// Account mapping is provider-specific but field-generic:
//   - Nexus: authenticated = API key configured,
//            can_auto_download = premium (direct download links).
//   - LoversLab: authenticated = session cookie present (no tier).
//   - direct: anonymous Auto needs neither field.
//
// Engine layer - Qt-free.

#include "engine/collection/manifest.h"

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace engine::Collection {

// Where a mod's archive actually comes from.
enum class DownloadPath {
  Auto,            // engine downloads it (API / direct URL / session cookie)
  Browser,         // user interaction required (manual click-through, login, upgrade)
  ExternalClient,  // outside GMM entirely (Steam Workshop subscription)
};

// What the user's account unlocks on a tiered source.
struct AccountStatus {
  bool authenticated     = false;  // logged in / key / session cookie present
  bool can_auto_download = false;  // tier grants unattended links (Nexus premium)
};

// What a source backend is capable of, independent of any one account.
struct SourceCapabilities {
  bool api_download     = true;   // backend can fetch without user interaction
  bool browser_fallback = true;   // a manual browser flow exists as fallback
  bool external_client  = false;  // download happens in an outside client
  bool needs_auth       = false;  // auto path requires an authenticated account
  bool needs_premium = false;  // auto path requires a tiered account (free vs premium)
};

// Routing verdict: the path plus a human-readable reason for the UI.
struct RouteOutcome {
  DownloadPath path = DownloadPath::Auto;
  std::string reason;
};

// Backend capabilities per provider id (SourceNexus::kProvider, ...).
// Unknown providers get a neutral default (API assumed, browser fallback).
inline SourceCapabilities capabilities_for(std::string_view provider) {
  SourceCapabilities caps;
  if (provider == SourceNexus::kProvider) {
    caps.needs_auth    = true;
    caps.needs_premium = true;  // free accounts get browser redirect, not direct links
  } else if (provider == SourceLoversLab::kProvider) {
    caps.needs_auth = true;  // session-cookie fetch; anonymous falls back to browser
  } else if (provider == SourceModPub::kProvider) {
    caps.needs_auth = true;
  } else if (provider == SourceSteamWorkshop::kProvider) {
    caps.api_download     = false;
    caps.browser_fallback = false;
    caps.external_client  = true;
  } else if (provider == SourceDirect::kProvider) {
    // Nothing needed: anonymous direct-URL fetch.
  }
  return caps;
}

inline RouteOutcome route_download(SourceResolution declared,
                                   const AccountStatus &account,
                                   const SourceCapabilities &caps) {
  if (declared == SourceResolution::ClientSubscription || caps.external_client) {
    return {DownloadPath::ExternalClient,
            "source downloads through an external client subscription"};
  }
  if (declared == SourceResolution::Browser) {
    return {DownloadPath::Browser, "entry provides only a page URL"};
  }
  // Declared Api: auto-download iff the backend supports it and the account
  // clears whatever gate the backend imposes.
  if (!caps.api_download) {
    return {DownloadPath::Browser, "source has no automatic download"};
  }
  if (caps.needs_auth && !account.authenticated) {
    return {DownloadPath::Browser, "automatic download requires login"};
  }
  if (caps.needs_premium && !account.can_auto_download) {
    return {DownloadPath::Browser,
            "automatic download requires a premium-tier account"};
  }
  return {DownloadPath::Auto, "automatic download available"};
}

// Variant convenience: derives the provider row and declared resolution from
// a manifest ModSource so callers route manifest entries directly.
inline RouteOutcome route_download(const ModSource &source,
                                   const AccountStatus &account) {
  const auto resolved = std::visit(
      [](const auto &s) -> std::pair<std::string_view, SourceResolution> {
        return {std::string_view(s.kProvider), s.resolution};
      },
      source);
  return route_download(resolved.second, account, capabilities_for(resolved.first));
}

}  // namespace engine::Collection
