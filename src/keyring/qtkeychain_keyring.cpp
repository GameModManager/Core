#include "keyring/qtkeychain_keyring.h"

#include <qt6keychain/keychain.h>

#include <QEventLoop>
#include <QMetaObject>
#include <QThread>

#include <type_traits>

namespace engine {

namespace {

constexpr const char* kService = "GameModManager";

// Runs fn on the main thread (blocking) when called from another thread.
// QtKeychain jobs require an event loop, and the Qt main thread has one.
template <typename F>
auto run_on_main(const QObject* ctx, F&& fn) -> std::invoke_result_t<F> {
    if (QThread::currentThread() == ctx->thread())
        return fn();
    if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
        QMetaObject::invokeMethod(const_cast<QObject*>(ctx), [&] { fn(); },
                                  Qt::BlockingQueuedConnection);
    } else {
        std::invoke_result_t<F> result{};
        const bool ok = QMetaObject::invokeMethod(
            const_cast<QObject*>(ctx), [&] { result = fn(); },
            Qt::BlockingQueuedConnection);
        if (!ok)
            return fn();
        return result;
    }
}

} // namespace

QtKeychainKeyring::QtKeychainKeyring() = default;
QtKeychainKeyring::~QtKeychainKeyring() = default;

bool QtKeychainKeyring::available() const {
    return run_on_main(this, [this] { return do_available(); });
}

bool QtKeychainKeyring::has(const std::string& name) const {
    return run_on_main(this, [this, &name] { return do_has(name); });
}

std::string QtKeychainKeyring::get(const std::string& name) const {
    return run_on_main(this, [this, &name] { return do_get(name); });
}

bool QtKeychainKeyring::set(const std::string& name, const std::string& value) {
    return run_on_main(this, [this, &name, &value] { return do_set(name, value); });
}

void QtKeychainKeyring::remove(const std::string& name) {
    run_on_main(this, [this, &name] { do_remove(name); });
}

// -----------------------------------------------------------------------
// Main-thread implementations - each runs one QtKeychain job synchronously
// by pumping a nested event loop until the job's finished signal arrives.
// -----------------------------------------------------------------------

bool QtKeychainKeyring::do_available() const {
    if (available_checked_)
        return available_cached_;

    QKeychain::Error err = QKeychain::OtherError;
    QEventLoop loop;
    QKeychain::ReadPasswordJob job(QString::fromLatin1(kService));
    // A key that is never written: EntryNotFound proves a real backend
    // answered; NoBackendAvailable means no OS keyring is running at all.
    job.setKey(QStringLiteral("gmm-availability-probe"));
    QObject::connect(&job, &QKeychain::Job::finished, &loop,
                     [&](QKeychain::Job* j) {
                         err = j->error();
                         loop.quit();
                     });
    job.start();
    loop.exec();

    available_cached_ = (err != QKeychain::NoBackendAvailable);
    available_checked_ = true;
    return available_cached_;
}

bool QtKeychainKeyring::do_has(const std::string& name) const {
    QKeychain::Error err = QKeychain::OtherError;
    QEventLoop loop;
    QKeychain::ReadPasswordJob job(QString::fromLatin1(kService));
    job.setKey(QString::fromStdString(name));
    QObject::connect(&job, &QKeychain::Job::finished, &loop,
                     [&](QKeychain::Job* j) {
                         err = j->error();
                         loop.quit();
                     });
    job.start();
    loop.exec();
    return err == QKeychain::NoError;
}

std::string QtKeychainKeyring::do_get(const std::string& name) const {
    QKeychain::Error err = QKeychain::OtherError;
    QString data;
    QEventLoop loop;
    QKeychain::ReadPasswordJob job(QString::fromLatin1(kService));
    job.setKey(QString::fromStdString(name));
    QObject::connect(&job, &QKeychain::Job::finished, &loop,
                     [&](QKeychain::Job* j) {
                         err = j->error();
                         if (err == QKeychain::NoError)
                             data =
                                 static_cast<QKeychain::ReadPasswordJob*>(j)
                                     ->textData();
                         loop.quit();
                     });
    job.start();
    loop.exec();
    if (err != QKeychain::NoError)
        return {};
    return std::string(data.toUtf8().constData(),
                       static_cast<size_t>(data.toUtf8().size()));
}

bool QtKeychainKeyring::do_set(const std::string& name, const std::string& value) {
    QKeychain::Error err = QKeychain::OtherError;
    QEventLoop loop;
    QKeychain::WritePasswordJob job(QString::fromLatin1(kService));
    job.setKey(QString::fromStdString(name));
    job.setTextData(QString::fromUtf8(value.data(),
                                      static_cast<int>(value.size())));
    QObject::connect(&job, &QKeychain::Job::finished, &loop,
                     [&](QKeychain::Job* j) {
                         err = j->error();
                         loop.quit();
                     });
    job.start();
    loop.exec();
    return err == QKeychain::NoError;
}

void QtKeychainKeyring::do_remove(const std::string& name) const {
    QEventLoop loop;
    QKeychain::DeletePasswordJob job(QString::fromLatin1(kService));
    job.setKey(QString::fromStdString(name));
    QObject::connect(&job, &QKeychain::Job::finished, &loop,
                     [&](QKeychain::Job* j) {
                         (void)j; // EntryNotFound on delete is fine
                         loop.quit();
                     });
    job.start();
    loop.exec();
}

} // namespace engine
