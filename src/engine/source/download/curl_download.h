#pragma once

// Backward-compat wrapper — consumers should migrate to engine/source/download/manager.h
#include "engine/source/download/manager.h"

namespace engine::download {

// Backward-compat aliases (deprecated — use Source::DownloadManager::*)
using engine::Source::DownloadManager::curl_download;
using engine::Source::DownloadManager::Progress;
using engine::Source::DownloadManager::Options;
using engine::Source::DownloadManager::parse_content_disposition_filename;
using engine::Source::DownloadManager::percent_decode;
using engine::Source::DownloadManager::capture_content_disposition;

} // namespace engine::download
