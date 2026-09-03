// =============================================================================
// Network:: options adapter - build a NetworkOptions from UI Settings.
//
// Lives in the UI layer because Settings is a QSettings-backed singleton. The
// engine has no Qt dependency, so this translation has to happen on the UI
// side. Push the resulting struct via Network::instance().set_options() once
// at startup, then again whenever a network-affecting setting changes
// (offline_mode, use_proxy, proxy_host, proxy_port).
// =============================================================================

#pragma once

#include "engine/network/network_manager.h"

namespace engine::network {

// Build a NetworkOptions from the current Settings values. Caller passes
// each value explicitly (rather than taking a Settings&) so this header
// does not pull in <QSettings> / ui/settings.
NetworkOptions build_options_from_settings(bool offline_mode,
                                           bool use_proxy,
                                           const std::string& proxy_host,
                                           int proxy_port,
                                           bool nexus_queue_downloads,
                                           int workshop_rate_limit_per_hour,
                                           int default_timeout_seconds,
                                           int max_retries);

// Convenience: read everything from Settings and push it into Network::.
// Safe to call on every settings change.
void push_settings_to_network();

} // namespace engine::network