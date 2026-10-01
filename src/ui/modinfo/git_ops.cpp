#include "ui/modinfo/git_ops.h"

#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <filesystem>
#include <system_error>

namespace ui::gitops {

namespace {

  constexpr const char *kGitExe = "git";

  // First non-empty trimmed line of `text`.
  QString first_line(const QString &text) {
    for (const QString &line : text.split(QLatin1Char('\n'))) {
      const QString trimmed = line.trimmed();
      if (!trimmed.isEmpty())
        return trimmed;
    }
    return {};
  }

  int count_lines(const QString &text) {
    int count = 0;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
      if (!line.trimmed().isEmpty())
        ++count;
    }
    return count;
  }

}  // namespace

QString Result::summary() const {
  if (!started)
    return error.isEmpty() ? QObject::tr("git is not available.") : error;
  if (ok)
    return {};
  const QString line = first_line(error);
  if (!line.isEmpty())
    return line;
  const QString out_line = first_line(out);
  if (!out_line.isEmpty())
    return out_line;
  return QObject::tr("git exited with code %1.").arg(exit_code);
}

bool available() {
  static const bool found =
      !QStandardPaths::findExecutable(QString::fromLatin1(kGitExe)).isEmpty();
  return found;
}

Result run(const std::filesystem::path &dir, const QStringList &args, int timeout_ms) {
  Result r;
  const QString exe = QStandardPaths::findExecutable(QString::fromLatin1(kGitExe));
  if (exe.isEmpty()) {
    r.error = QObject::tr("git was not found on PATH.");
    return r;
  }

  QProcess proc;
  // -C pins every call to the mod folder, so no stray cwd can redirect a
  // destructive command at a different repository.
  proc.setWorkingDirectory(QString::fromStdString(dir.string()));
  QStringList argv;
  argv << QStringLiteral("-C") << QStringLiteral(".");
  argv += args;
  proc.start(exe, argv);
  if (!proc.waitForStarted(timeout_ms)) {
    r.error = QObject::tr("git could not be started: %1").arg(proc.errorString());
    return r;
  }
  r.started = true;
  if (!proc.waitForFinished(timeout_ms)) {
    proc.kill();
    proc.waitForFinished(5000);
    r.error =
        QObject::tr("git did not finish within %1 seconds.").arg(timeout_ms / 1000);
    return r;
  }
  r.exit_code = proc.exitCode();
  // stdout carries the value (a sha, a branch name, a count) and stderr the
  // diagnosis. Both are kept so callers read the real one instead of guessing.
  r.out   = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
  r.error = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
  r.ok    = r.exit_code == 0;
  return r;
}

bool is_scoped_repository(const std::filesystem::path &dir) {
  if (dir.empty() || !available())
    return false;
  // A worktree or submodule has a .git FILE whose gitdir lives outside the
  // mod; reset/clean there would act on files the mod does not own.
  std::error_code ec;
  if (!std::filesystem::is_directory(dir / ".git", ec))
    return false;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(dir, ec);
  if (ec || canonical.empty())
    return false;

  const Result top =
      run(dir, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, 10000);
  if (!top.ok || top.out.isEmpty())
    return false;
  std::error_code top_ec;
  const std::filesystem::path reported = std::filesystem::weakly_canonical(
      std::filesystem::path(top.out.toStdString()), top_ec);
  if (top_ec)
    return false;
  // The toplevel git reports must BE the mod folder. Anything else means the
  // mod sits inside a larger repository and a reset would reach past it.
  return reported == canonical;
}

Status status(const std::filesystem::path &dir) {
  Status s;
  if (!available()) {
    s.error = QObject::tr("git is not available on this system.");
    return s;
  }
  // HEAD is explicit on both: `git rev-parse --abbrev-ref` with no revision
  // prints nothing and still exits 0, which would look like an unnamed branch.
  const Result branch = run(dir,
                            {QStringLiteral("rev-parse"),
                             QStringLiteral("--abbrev-ref"), QStringLiteral("HEAD")},
                            10000);
  if (!branch.ok || branch.out.isEmpty()) {
    s.error = branch.summary();
    return s;
  }
  s.branch = branch.out;
  s.ok     = true;

  const Result commit = run(
      dir,
      {QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")},
      10000);
  if (commit.ok)
    s.commit = commit.out;

  const Result remote = run(dir,
                            {QStringLiteral("config"), QStringLiteral("--get"),
                             QStringLiteral("remote.origin.url")},
                            10000);
  if (remote.ok)
    s.remote = remote.out;

  // "git rev-list --left-right --count HEAD...@{upstream}" prints
  // "<ahead>\t<behind>" - left is the HEAD-only side, right the upstream-only
  // side - and FAILS when the branch has no upstream, which is how "unknown"
  // stays distinguishable from "up to date".
  const Result counts =
      run(dir,
          {QStringLiteral("rev-list"), QStringLiteral("--left-right"),
           QStringLiteral("--count"), QStringLiteral("HEAD...@{upstream}")},
          20000);
  if (counts.ok) {
    s.has_upstream          = true;
    const QStringList parts = counts.out.split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() == 2) {
      s.ahead  = parts.at(0).toInt();
      s.behind = parts.at(1).toInt();
    }
  }
  return s;
}

QStringList upstream_branches(const std::filesystem::path &dir) {
  const Result r = run(dir,
                       {QStringLiteral("branch"), QStringLiteral("-r"),
                        QStringLiteral("--format=%(refname:short)")},
                       20000);
  if (!r.ok)
    return {};
  QStringList out;
  for (const QString &line : r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    const QString trimmed = line.trimmed();
    if (!trimmed.isEmpty())
      out << trimmed;
  }
  out.sort();
  return out;
}

int discardable_change_count(const std::filesystem::path &dir) {
  // --untracked-files=all so a whole untracked DIRECTORY counts as one entry,
  // the way `git clean -fd` treats it, and the same -e exclusions the reset
  // uses so the count matches what the reset really discards.
  const Result r = run(dir,
                       {QStringLiteral("status"), QStringLiteral("--porcelain"),
                        QStringLiteral("--untracked-files=all"), QStringLiteral("--"),
                        QStringLiteral("."), QStringLiteral(":(exclude)meta.ini"),
                        QStringLiteral(":(exclude)metadata.xml")},
                       20000);
  return r.ok ? count_lines(r.out) : 0;
}

Result fetch(const std::filesystem::path &dir) {
  if (!available()) {
    Result r;
    r.error = QObject::tr("git was not found on PATH.");
    return r;
  }
  return run(dir, {QStringLiteral("fetch"), QStringLiteral("--quiet"),
                   QStringLiteral("origin")});
}

Result pull_ff(const std::filesystem::path &dir) {
  if (!available()) {
    Result r;
    r.error = QObject::tr("git was not found on PATH.");
    return r;
  }
  // --ff-only: a diverged branch fails instead of merging behind the user's
  // back, so a conflict is never something they did not choose.
  return run(dir, {QStringLiteral("pull"), QStringLiteral("--ff-only"),
                   QStringLiteral("--quiet")});
}

Result reset_to_upstream(const std::filesystem::path &dir) {
  if (!available()) {
    Result r;
    r.error = QObject::tr("git was not found on PATH.");
    return r;
  }
  // Re-checked here, not just at the call site: this is the last guard before
  // an unrecoverable discard.
  if (!is_scoped_repository(dir)) {
    Result r;
    r.started = true;
    r.error =
        QObject::tr("Refusing to reset: %1 is not the root of its own repository.")
            .arg(QString::fromStdString(dir.string()));
    return r;
  }
  const Result fetched = fetch(dir);
  if (!fetched.ok)
    return fetched;
  const Result reset = run(dir,
                           {QStringLiteral("reset"), QStringLiteral("--hard"),
                            QStringLiteral("@{upstream}")},
                           60000);
  if (!reset.ok)
    return reset;
  // -e keeps the mod's own metadata: meta.ini holds the sidecar (sources,
  // categories, notes) and is untracked, so a bare clean would delete it.
  return run(dir, {QStringLiteral("clean"), QStringLiteral("-fd"), QStringLiteral("-e"),
                   QStringLiteral("meta.ini"), QStringLiteral("-e"),
                   QStringLiteral("metadata.xml")});
}

}  // namespace ui::gitops
