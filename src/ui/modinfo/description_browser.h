#pragma once

#include <QHash>
#include <QTextBrowser>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace ui {

// QTextBrowser subclass that knows how to load http(s) image resources
// inside BBCode descriptions ([img]https://...[/img] in the Nexus / Steam
// feeds). The stock QTextBrowser::loadResource() only resolves local file
// paths and searchPaths; for a remote URL it returns an invalid QVariant
// and the document silently shows a broken image.
//
// We use a per-instance QNetworkAccessManager and a small request/reply
// cache. Loading is asynchronous: the image arrives after setHtml()
// returns and we re-paint the affected document area via
// document()->markContentsDirty(). A 10 s timeout caps the wait so a
// stalled CDN can't permanently leave a half-loaded image in the cache.
//
// Everything that is not http/https (file://, qrc://, plain relative
// paths) falls through to QTextBrowser's default implementation, which
// already handles searchPaths and the current source directory.
class DescriptionBrowser : public QTextBrowser {
  Q_OBJECT
public:
  explicit DescriptionBrowser(QWidget *parent = nullptr);
  ~DescriptionBrowser() override;

  // Drop all cached images. Called when a new description is loaded so
  // stale entries from the previous mod don't linger.
  void clear_image_cache();

protected:
  QVariant loadResource(int type, const QUrl &name) override;

private:
  QNetworkAccessManager *nam_ = nullptr;
  // url -> reply (so we can disconnect on cache clear / teardown)
  QHash<QUrl, QNetworkReply *> in_flight_;
};

} // namespace ui
