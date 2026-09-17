#include "core/core.h"

#include <QDebug>

#ifdef GMM_HAS_WEBENGINE
#include <QWebEngineProfile>
#endif

int main(int argc, char *argv[]) {
  Core::Application app(argc, argv);
#ifdef GMM_HAS_WEBENGINE
  // MO2 pattern: untrusted remote content must not persist cookies.
  // Runs after the QApplication exists (QWebEngineProfile needs one).
  QWebEngineProfile::defaultProfile()->setPersistentCookiesPolicy(
      QWebEngineProfile::NoPersistentCookies);
#else
  qDebug() << "[Main] GMM_HAS_WEBENGINE not defined, WebEngine unavailable";
#endif
  return app.run();
}
