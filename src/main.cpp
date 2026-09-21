#include "core/core.h"

int main(int argc, char* argv[]) {
  Core::Application app(argc, argv);
  // No QWebEngineProfile setup here (Workspace-2vmp): touching the default
  // profile initializes Chromium, so the NoPersistentCookies policy is
  // applied lazily on first WebViewDescriptionRenderer construction
  // instead. Startups that never open a description view never pay for it.
  return app.run();
}
