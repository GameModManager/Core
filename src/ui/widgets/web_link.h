#pragma once

#include <QString>
#include <QStringList>

// Opening a web link. Every "click a link to a site" path in the app funnels
// through open_web_link() instead of calling QDesktopServices directly, so the
// Workarounds > "Use a custom browser" setting has exactly one place it can
// take effect.
//
// The custom command is a program plus its own arguments, with "%1" standing
// for the URL - the convention MO2's shell URL handler uses. A command with no
// "%1" gets the URL appended as the last argument, so a bare "firefox" works
// the way a user expects.
class WebLink {
public:
  // argv for the custom browser: empty when `command` is blank (nothing to
  // run), otherwise the command split on whitespace with "%1" replaced by
  // `url`, or the URL appended when the command has no placeholder.
  [[nodiscard]] static QStringList custom_browser_argv(const QString &command,
                                                       const QString &url);

  // Open `url` in the user's browser. Returns true when the link was handed to
  // a browser (the custom one or the desktop default), false when it was not:
  // an empty URL, a custom command that could not be started, or no desktop
  // handler. A custom command that fails to start does NOT fall back to the
  // default handler - that would silently ignore the setting.
  [[nodiscard]] static bool open(const QString &url);

  // true when open() will use the configured custom browser for this call
  // rather than the desktop default. Exposed for the tooltip/test surface.
  [[nodiscard]] static bool uses_custom_browser();
};
