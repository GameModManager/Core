#pragma once

#include "engine/source/interface.h"
#include "engine/source/steam/workshop_client.h"

#include <memory>
#include <mutex>
#include <string>

namespace engine::Source::Steam {

class Provider : public Interface {
public:
  explicit Provider(const std::string &db_path, int rate_limit = 60,
                    int rate_window = 3600);

  std::string source_type() const override { return "steam"; }
  bool fetch(const ::engine::Mod &mod, ::engine::PipelineContext &ctx,
             const std::filesystem::path &dest_path) override;
  std::string display_name() const override;
  // The cooldown we enforce on our own requests - read on the UI thread for
  // the status bar, so it must not create the (SQLite-backed) client.
  SourceRateLimit rate_limit_readout() const override;

  // Push updated rate-limit values to the live client (no-op if client
  // hasn't been created yet; the values are remembered for it).
  void set_rate_limit(int limit, int window);
  int rate_limit() const { return rate_limit_; }
  int rate_window() const { return rate_window_; }

private:
  // Opens the client on first use. Guarded because the status bar reads the
  // cooldown from the UI thread while fetch() may be creating it on a worker.
  WorkshopClient *ensure_client() const;

  mutable std::mutex client_mutex_;
  mutable std::unique_ptr<WorkshopClient> client_;
  std::string db_path_;
  int rate_limit_  = 60;
  int rate_window_ = 3600;
};

}  // namespace engine::Source::Steam
