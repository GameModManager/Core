#pragma once

// Backward-compat wrapper - consumers should migrate to:
//   engine/source/nexus/http.h    (Nexus HTTP request)
//   engine/source/http_util.h     (encode_url_path)
#include "engine/source/http_util.h"
#include "engine/source/nexus/http.h"

namespace engine {

// Backward-compat aliases (deprecated - use Source::Nexus::Http::nexus_http_request / Source::Http::encode_url_path)
using Source::Http::encode_url_path;
using Source::Nexus::Http::nexus_http_request;

} // namespace engine
