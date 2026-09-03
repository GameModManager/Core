#include "ui/network/network_options_bridge.h"

#include "ui/settings/settings.h"

namespace engine::network {

NetworkOptions build_options_from_settings(bool offline_mode,
                                           bool use_proxy,
                                           const std::string& proxy_host,
                                           int proxy_port,
                                           bool nexus_queue_downloads,
                                           int workshop_rate_limit_per_hour,
                                           int default_timeout_seconds,
                                           int max_retries) {
    NetworkOptions opts;
    opts.offline_mode = offline_mode;
    opts.use_proxy = use_proxy;
    opts.proxy_host = proxy_host;
    opts.proxy_port = proxy_port > 0 ? proxy_port : 8080;
    opts.nexus_queue_downloads = nexus_queue_downloads;
    opts.workshop_rate_limit_per_hour =
        workshop_rate_limit_per_hour > 0 ? workshop_rate_limit_per_hour : 60;
    opts.default_timeout_seconds =
        default_timeout_seconds > 0 ? default_timeout_seconds : 30;
    opts.max_retries = max_retries >= 0 ? max_retries : 0;
    opts.max_parallel = 2;  // matches PipelineWorker kMaxConcurrentDownloads
    return opts;
}

void push_settings_to_network() {
    auto& s = Settings::instance();
    auto opts = build_options_from_settings(
        s.offline_mode(),
        s.use_proxy(),
        s.proxy_host().toStdString(),
        s.proxy_port(),
        s.nexus_queue_downloads(),
        s.workshop_rate_limit_per_hour(),
        /*default_timeout=*/30,
        /*max_retries=*/0);
    instance().set_options(std::move(opts));
}

} // namespace engine::network