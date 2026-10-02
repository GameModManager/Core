#include "core/core.h"
#include "ui/modinfo/description_renderer.h"

int main(int argc, char *argv[]) {
  // Before Core::Application, so before any Qt object exists: Chromium's GPU
  // process must be told not to pick Vulkan before WebEngine starts it.
  ui::configure_chromium_flags();
  Core::Application app(argc, argv);
  // No QWebEngineProfile setup here (Workspace-2vmp): touching the default
  // profile initializes Chromium, so the NoPersistentCookies policy is
  // applied lazily on first WebViewDescriptionRenderer construction
  // instead. Startups that never open a description view never pay for it.
  return app.run();
}