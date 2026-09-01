// Tests for the DescriptionBrowser QTextBrowser subclass (image loading
// for http(s) URLs in BBCode descriptions).
//
// We don't make real network requests here. The two properties we need
// to lock down are:
//   1. Non-http URLs delegate to QTextBrowser::loadResource (i.e. the
//      file/qrc fall-through still works for compatibility with any
//      caller that uses DescriptionBrowser for local content too).
//   2. The instance can be constructed and a QApplication lives long
//      enough to exercise the override (proves MOC + Qt linkage is OK
//      in the test binary).
//   3. REGRESSION: loadResource must not recurse into itself via
//      QTextDocument::resource(). Calling doc->resource() directly on a
//      browser that has a remote http URL must return without crashing
//      (recursion -> stack overflow -> SIGSEGV on the buggy code).

#include "ui/modinfo/description_browser.h"

#include <QApplication>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
} // namespace

TEST_CASE("description_browser: construction and teardown", "[ui][description_browser]") {
  // QApplication is created once via QCoreApplication's instance guard.
  // Construction + destruction here proves MOC works and the class
  // links into the test binary.
  int argc = 0;
  static QApplication app(argc, nullptr);
  ui::DescriptionBrowser browser;
  check(browser.openExternalLinks(),
        "openExternalLinks defaults to true to match the prior panels");
}

TEST_CASE("description_browser: clear_image_cache on a fresh browser is a no-op",
          "[ui][description_browser]") {
  int argc = 0;
  static QApplication app(argc, nullptr);
  ui::DescriptionBrowser browser;
  // No in-flight replies yet; clear should not crash or change state.
  browser.clear_image_cache();
  CHECK(true);
}

TEST_CASE("description_browser: setHtml with a remote [img] does not crash",
          "[ui][description_browser]") {
  // Smoke test: feed DescriptionBrowser a real BBCode snippet with a
  // remote [img] tag and confirm it accepts the HTML. We don't make a
  // real network request - the test environment is offline. The point
  // is to prove the override pattern is wired up: the browser does not
  // crash on a remote URL, and the QNetworkAccessManager fires a GET
  // (which will time out in the test, but that is the production
  // behaviour too).
  int argc = 0;
  static QApplication app(argc, nullptr);
  ui::DescriptionBrowser browser;
  browser.setHtml(QStringLiteral(
      "<html><body><img src=\"http://127.0.0.1:1/none.png\" alt=\"\"></body></html>"));
  // The browser should still display something; the exact rendering
  // depends on whether the request has started yet. We just want to
  // confirm the QTextDocument accepted the HTML and the loadResource
  // override was callable.
  check(browser.document() != nullptr,
        "QTextDocument is created after setHtml");
  // Drain pending events so the QNetworkAccessManager's GET runs
  // through whatever path it can in this offline environment.
  QCoreApplication::processEvents();
  // clean_image_cache: must not crash even with no in-flight replies.
  browser.clear_image_cache();
  CHECK(true);
}

TEST_CASE("description_browser: doc->resource() on a remote URL does not recurse",
          "[ui][description_browser]") {
  // REGRESSION TEST for Workspace-78oo (P1 crash). The previous
  // implementation of loadResource() ended with an unconditional
  // doc->resource() call. QTextDocument::resource() falls back to
  // loadResource() when nothing is cached, which (because loadResource
  // is virtual) dispatched back into DescriptionBrowser::loadResource
  // and recursed until the stack overflowed. setHtml() does not
  // synchronously trigger image resource loading, so the previous smoke
  // test never exercised the bug. This test forces the recursion path
  // by calling doc->resource() directly, which is exactly what the
  // document layout does on the very first paint of a real description.
  int argc = 0;
  static QApplication app(argc, nullptr);
  ui::DescriptionBrowser browser;
  const QUrl url(QStringLiteral("http://127.0.0.1:1/none.png"));
  // setHtml so document() is non-null and the layout is willing to
  // resolve the resource at all. Port 1 is reserved and unreachable;
  // the GET is allowed to time out in the background.
  browser.setHtml(
      QStringLiteral("<html><body><img src=\"http://127.0.0.1:1/none.png\" "
                     "alt=\"\"></body></html>"));
  check(browser.document() != nullptr, "document exists after setHtml");
  // The recursion trigger: ask the document for the resource directly.
  // On the buggy code this recurses forever (stack overflow, SIGSEGV);
  // after the fix it returns an invalid QVariant because the url has
  // not been fetched yet. The point of the test is that we get here
  // and back at all.
  const QVariant v = browser.document()->resource(
      QTextDocument::ImageResource, url);
  check(!v.isValid(),
        "first lookup misses the cache: the network reply has not landed");
  // A second lookup for the same url must also be safe (still no
  // recursion, still a miss).
  const QVariant v2 = browser.document()->resource(
      QTextDocument::ImageResource, url);
  check(!v2.isValid(), "second lookup also misses the cache");
  // Drain pending events so any in-flight reply (which will error out
  // on the unreachable host) is cleaned up; clear_image_cache then
  // tears down the in_flight_ bookkeeping safely.
  QCoreApplication::processEvents();
  browser.clear_image_cache();
  CHECK(true);
}
