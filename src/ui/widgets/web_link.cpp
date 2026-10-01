#include "ui/widgets/web_link.h"

#include "ui/settings/settings.h"

#include <QDesktopServices>
#include <QProcess>
#include <QUrl>

namespace {

const char *kPlaceholder = "%1";

// Split a command line on whitespace. Done by hand rather than with
// QRegularExpression so this header pair stays on QtCore-only basics.
QStringList split_command(const QString &command) {
  QString flat = command;
  flat.replace(QLatin1Char('\t'), QLatin1Char(' '));
  flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
  return flat.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

}  // namespace

QStringList WebLink::custom_browser_argv(const QString &command, const QString &url) {
  const auto tokens = split_command(command);
  if (tokens.isEmpty())
    return {};
  QStringList out;
  bool placeholder = false;
  for (const auto &token : tokens) {
    if (token.contains(QLatin1String(kPlaceholder))) {
      placeholder = true;
      out.append(token.arg(url));
    } else {
      out.append(token);
    }
  }
  // No placeholder: append the URL, so "firefox" behaves like "firefox %1".
  if (!placeholder)
    out.append(url);
  return out;
}

bool WebLink::uses_custom_browser() {
  const auto &s = Settings::instance();
  return s.use_custom_browser() && !s.custom_browser_command().trimmed().isEmpty();
}

bool WebLink::open(const QString &url) {
  if (url.trimmed().isEmpty())
    return false;

  if (uses_custom_browser()) {
    const auto argv =
        custom_browser_argv(Settings::instance().custom_browser_command(), url);
    if (argv.isEmpty())
      return false;
    // Detached: the browser outlives us, and the user may close the game tab
    // while it is still starting up.
    if (!QProcess::startDetached(argv.front(), argv.mid(1)))
      return false;
    return true;
  }

  return QDesktopServices::openUrl(QUrl(url));
}
