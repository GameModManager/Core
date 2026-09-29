#include "engine/mod/archive/archive_extractor.h"
#include "engine/core/util/fs_utils.h"
#include "engine/core/util/process_utils.h"
#include "engine/core/util/thread_priority.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <system_error>

namespace engine {

namespace {

  // Total number of passwords extract() will ask for on one archive. libarchive
  // re-invokes the passphrase callback on every failed decryption attempt and
  // gives up only after 10000 of them, so this cap is what keeps a wrong
  // password from turning into thousands of prompts: the callback returns
  // nothing once the budget is spent, the reader fails, and extract() reports
  // the reason. Two is one typo's worth of margin over a single try.
  constexpr int kMaxPassphraseAttempts = 3;

  // Best-effort libarchive diagnostic for the last operation on `a`.
  std::string archive_error_message(struct archive *a) {
    const char *msg = archive_error_string(a);
    if (!msg || !*msg)
      return "unknown libarchive error";
    return msg;
  }

  // The password state shared by every reader created for one archive: the
  // size pre-pass, the extraction pass, and the unrar fallback. The stored
  // answer is reused by every later request within the same attempt, so one
  // prompt covers the whole archive; extract() clears it to force a re-prompt
  // when a reader rejected what it was given.
  //
  // The deque is deliberate: libarchive asks for a password once per encrypted
  // entry, and a value returned to an earlier request must stay valid while a
  // later one is handed out. Never holding a pointer into a std::string that a
  // later assignment can reallocate removes the question entirely.
  struct PassphraseSession {
    const PassphraseFn *ask = nullptr;  // null = no prompt available
    std::string archive_name;
    std::deque<std::string> supplied;
    bool asked    = false;  // the user was actually prompted
    bool canceled = false;  // the user dismissed the prompt
    int attempts  = 0;

    bool has_passphrase() const { return !supplied.empty(); }
    const std::string &passphrase() const { return supplied.back(); }
    void forget() { supplied.clear(); }
  };

  // libarchive's C callback. Returns the password to decrypt with, or null to
  // refuse - which is how a dismissed prompt, an exhausted attempt budget and
  // a headless run with no prompt all reach the reader, without it needing to
  // know which it was.
  const char *passphrase_callback(struct archive *, void *data) {
    auto *s = static_cast<PassphraseSession *>(data);
    if (!s || s->canceled || !s->ask)
      return nullptr;
    if (s->has_passphrase())
      return s->passphrase().c_str();
    if (s->attempts >= kMaxPassphraseAttempts)
      return nullptr;
    ++s->attempts;
    s->asked = true;
    std::string answer;
    // An empty answer is a dismissal, not a blank password: handing an empty
    // string back would leave the reader retrying against nothing.
    if (!(*s->ask)(s->archive_name, answer) || answer.empty()) {
      s->canceled = true;
      return nullptr;
    }
    s->supplied.push_back(std::move(answer));
    return s->passphrase().c_str();
  }

  // Ask for a password outside a libarchive reader - the unrar fallback, which
  // cannot prompt for itself (its stdin is /dev/null). Routed through the same
  // callback so the unrar path and the reader path spend one shared budget
  // rather than one each.
  bool request_passphrase(PassphraseSession &session) {
    return passphrase_callback(nullptr, &session) != nullptr;
  }

  // Attach the session to a fresh read handle. The handle is useless for an
  // encrypted archive without this, so both the pre-pass and the extraction
  // pass call it before opening.
  void attach_passphrase(struct archive *a, PassphraseSession &session) {
    if (session.ask)
      archive_read_set_passphrase_callback(a, &session, passphrase_callback);
  }

  // Cheap pre-pass for the progress bar: sum the uncompressed sizes of every
  // file entry by walking the headers only (no data is read or decompressed).
  // Returns -1 when the archive cannot be opened - the caller then falls back to
  // an indeterminate bar. Shares the session with the extraction pass so an
  // encrypted archive is decrypted with the password the user just typed
  // instead of costing a second prompt.
  int64_t archive_total_size(const std::filesystem::path &archive,
                             PassphraseSession &session) {
    struct archive *a = archive_read_new();
    if (!a)
      return -1;
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    attach_passphrase(a, session);

    int64_t total = 0;
    if (archive_read_open_filename(a, archive.string().c_str(), 10240) == ARCHIVE_OK) {
      struct archive_entry *entry;
      int r;
      while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        if (archive_entry_filetype(entry) == AE_IFDIR)
          continue;
        la_int64_t sz = archive_entry_size(entry);
        if (sz > 0)
          total += sz;
      }
    } else {
      total = -1;
    }
    archive_read_close(a);
    archive_read_free(a);
    return total;
  }

  // RAR fallback via the unrar CLI - the Linux analog of MO2's UnRAR.exe on
  // Windows. libarchive's RAR5 reader refuses archives whose declared window
  // exceeds 64 MiB ("Declared dictionary size is not supported"), and WinRAR-made
  // mod archives routinely exceed it. unrar has no such cap. extract() only
  // reaches this for genuine RAR files (magic check), never for other formats
  // libarchive rejected. No byte-level progress is available, so the caller
  // switches to an indeterminate bar; on failure `error` holds the reason.
  bool extract_with_unrar(const std::filesystem::path &archive,
                          const std::filesystem::path &dest_dir,
                          std::vector<ExtractedFile> &out_files, std::string &error,
                          PassphraseSession &session) {
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
      error = "cannot create " + dest_dir.string() + ": " + ec.message();
      return false;
    }

    // -y + -o+ overwrite; -idq silences the percentage spam; the trailing '/'
    // makes unrar treat dest as a directory (it is created on the fly if
    // missing). unrar's stdin is /dev/null, so it can never prompt us itself:
    // the password goes on the command line, or -p- tells it there is none and
    // it must not ask. (The password is therefore in the child's argv, readable
    // from /proc for the life of the run. The CLI offers no other channel, and
    // nothing here writes it to the log or to disk.)
    for (;;) {
      std::vector<std::string> args = {
          "unrar", "x", "-o+", "-y", "-idq",
          session.has_passphrase() ? "-p" + session.passphrase() : std::string("-p-"),
          archive.string(), dest_dir.string() + "/"};
      CapturedProcess proc = run_captured(args);
      // ok only means fork+waitpid succeeded; a missing unrar still exits 127
      // via the execvp-failure path (real unrar uses its own codes 0-10), so
      // 127 means "not installed", not a genuine extraction failure.
      if (!proc.ok || proc.exit_code == 127) {
        error = "unrar not available (install the 'unrar' package)";
        return false;
      }
      if (proc.exit_code == 0)
        break;
      // Exit 11 is unrar's documented "wrong password". It is also the only way
      // to learn the archive is encrypted, which is why the first run is made
      // without a password: an unencrypted RAR exits 0 on that run and the user
      // is never asked about anything. Only when one is refused do we ask, and
      // then unrar gets a real -p<pw> to work with.
      if (proc.exit_code == 11 && !session.has_passphrase() &&
          request_passphrase(session)) {
        continue;
      }
      if (proc.exit_code == 11) {
        // Out of password, or out of attempts: extract() re-prompts on the next
        // round if the budget allows. Name the outcome, never the password.
        error = session.has_passphrase() ? "unrar rejected the password"
                                         : "the archive is password protected";
      } else {
        error = "unrar exited with code " + std::to_string(proc.exit_code) + ": " +
                proc.err;
      }
      return false;
    }

    // Rebuild the extracted-file list by walking the tree: every regular file
    // under dest_dir came from the archive (it was created fresh above).
    auto it = std::filesystem::recursive_directory_iterator(dest_dir, ec);
    if (ec)
      return true;  // nothing listed; extraction itself still succeeded
    for (const auto &entry : it) {
      if (entry.is_directory())
        continue;
      std::error_code rel_ec;
      const std::filesystem::path rel =
          std::filesystem::relative(entry.path(), dest_dir, rel_ec);
      if (rel_ec)
        continue;
      ExtractedFile ef;
      ef.dest_path    = entry.path();
      ef.archive_path = engine::normalize_separators(rel.string());
      out_files.push_back(std::move(ef));
    }
    return true;
  }

  // One complete attempt at `archive`: the libarchive read pass, plus the unrar
  // fallback when this is a RAR libarchive could not read. The whole body
  // exists so extract() can run it again with a different password, which is
  // what makes a mistyped password recoverable rather than fatal.
  bool extract_once(const std::filesystem::path &archive,
                    const std::filesystem::path &dest_dir,
                    std::vector<ExtractedFile> &out_files, std::string &error,
                    const ExtractProgressFn &on_progress, PassphraseSession &session) {
    // Two-pass progress: sum entry sizes up front (header-only, cheap), then
    // report bytes written against that total while extracting.
    int64_t total_bytes = -1;
    if (on_progress)
      total_bytes = archive_total_size(archive, session);

    struct archive *a = archive_read_new();
    if (!a) {
      error = "archive_read_new failed";
      return false;
    }

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    attach_passphrase(a, session);

    int r = archive_read_open_filename(a, archive.string().c_str(), 10240);
    if (r != ARCHIVE_OK) {
      error = "cannot open " + archive.string() + ": " + archive_error_message(a) +
              " (errno " + std::to_string(archive_errno(a)) + ")";
      archive_read_free(a);
      return false;
    }

    struct archive_entry *entry;
    bool failed = false;
    // Bytes written so far; `last_reported` throttles progress updates to at
    // most one per ~128 KiB so a single huge file doesn't flood the callback.
    int64_t written       = 0;
    int64_t last_reported = -1;
    auto report_progress  = [&] {
      if (on_progress)
        on_progress(written, total_bytes);
    };
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
      const char *name = archive_entry_pathname(entry);
      if (!name)
        continue;

      // Entry names are Windows-native (backslash separators), so normalize
      // them the same way engine::resolve_path does - this also makes
      // Windows-authored "dir\" entries count as directories below.
      // Traversal entries (absolute paths, "..") are never extracted: a
      // single hostile entry must not escape dest_dir, so extraction fails.
      std::string name_str = engine::normalize_separators(name);
      if (name_str.empty())
        continue;

      const std::filesystem::path name_path(name_str);
      if (name_path.is_absolute() ||
          std::any_of(name_path.begin(), name_path.end(),
                      [](const auto &part) { return part == ".."; })) {
        error  = "archive entry has an unsafe path: " + name_str;
        failed = true;
        break;
      }

      auto dest_path = dest_dir / name_path;

      // Strip trailing slash from dir entries for consistent detection.
      // RAR/WinRAR stores directory entries WITHOUT a trailing slash - the
      // directory-ness lives in the archive header, so trust libarchive's
      // filetype first and only fall back on the name heuristic.
      bool is_dir = (archive_entry_filetype(entry) == AE_IFDIR) ||
                    (!name_str.empty() && name_str.back() == '/');
      if (is_dir) {
        std::error_code ec;
        std::filesystem::create_directories(dest_path, ec);
        if (ec) {
          error  = "cannot create directory " + dest_path.string() + ": " + ec.message();
          failed = true;
          break;
        }
        continue;
      }

      // Ensure parent directory exists
      std::error_code ec;
      std::filesystem::create_directories(dest_path.parent_path(), ec);
      if (ec) {
        error  = "cannot create parent directory " + dest_path.parent_path().string() +
                 ": " + ec.message();
        failed = true;
        break;
      }

      // Write file data
      std::ofstream out(dest_path, std::ios::binary);
      if (!out) {
        error =
            "cannot open for writing " + dest_path.string() + ": " + std::strerror(errno);
        failed = true;
        break;
      }

      char buf[32768];
      la_int64_t nread;
      while ((nread = archive_read_data(a, buf, sizeof(buf))) > 0) {
        out.write(buf, static_cast<std::streamsize>(nread));
        if (!out) {
          error  = "write failed for " + dest_path.string() + ": " + std::strerror(errno);
          failed = true;
          break;
        }
        written += nread;
        if (on_progress && written - last_reported >= (128 * 1024)) {
          last_reported = written;
          report_progress();
        }
      }
      if (failed)
        break;
      if (nread < 0) {
        // Read error
        error  = "read error in " + archive.string() + ": " + archive_error_message(a);
        failed = true;
        break;
      }
      out.close();
      report_progress();  // end of this file: keeps the bar moving on archives of many
                          // small files

      ExtractedFile ef;
      ef.archive_path = name_str;
      ef.dest_path    = dest_path;
      out_files.push_back(std::move(ef));
    }

    // Loop ended not by reaching the end of the archive but on a header error
    // (corrupt/truncated archive) - report it instead of returning "success".
    if (!failed && r != ARCHIVE_EOF) {
      error  = "archive header error in " + archive.string() + ": " +
               archive_error_message(a) + " (errno " + std::to_string(archive_errno(a)) +
               ")";
      failed = true;
    }

    archive_read_close(a);
    archive_read_free(a);
    if (!failed)
      return true;

    // RAR fallback: libarchive's RAR5 reader caps the declared dictionary at
    // 64 MiB ("Declared dictionary size is not supported"), which WinRAR-made
    // mod archives routinely exceed. Retry with the unrar CLI for genuine RAR
    // files only - never for other formats libarchive rejected. Keep the
    // libarchive diagnostic as the primary error and append unrar's if the
    // fallback fails too.
    if (is_rar_archive(archive)) {
      if (on_progress)
        on_progress(0, 0);  // indeterminate bar - no byte totals from unrar
      out_files.clear();    // drop any partial list from the libarchive pass
      std::string unrar_error;
      if (extract_with_unrar(archive, dest_dir, out_files, unrar_error, session)) {
        return true;
      }
      if (!unrar_error.empty())
        error += "; unrar fallback failed: " + unrar_error;
    }
    return false;
  }

}  // namespace

bool ArchiveExtractor::extract(const std::filesystem::path &archive,
                               const std::filesystem::path &dest_dir,
                               std::vector<ExtractedFile> &out_files,
                               std::string &error, const ExtractProgressFn &on_progress,
                               bool low_priority, const PassphraseFn &on_passphrase,
                               bool *canceled) {
  error.clear();
  if (canceled)
    *canceled = false;

  PassphraseSession session;
  session.ask          = on_passphrase ? &on_passphrase : nullptr;
  session.archive_name = archive.filename().string();

  // Lower CPU priority up front so the whole extraction (header pre-pass, the
  // read/decompress loop, and the unrar fallback) yields to the rest of the
  // system. Runs on the worker thread that drives the pipeline. Best-effort:
  // a failure here is non-fatal and extraction proceeds at normal priority.
  if (low_priority) {
    set_low_priority();
  }

  for (;;) {
    out_files.clear();
    std::string attempt_error;
    const bool ok = extract_once(archive, dest_dir, out_files, attempt_error, on_progress,
                                 session);
    if (ok) {
      error.clear();
      return true;
    }
    error = std::move(attempt_error);

    // A dismissed prompt ends the install. Not a failure: there is nothing to
    // report, the download keeps its state and the user simply did not want
    // to install this. Whatever the reader said on its way out is dropped with
    // it - it describes the abandoned attempt, not a problem the user has.
    if (session.canceled) {
      out_files.clear();
      error.clear();
      if (canceled)
        *canceled = true;
      return false;
    }

    // Retry only when the user was asked for a password and the reader still
    // failed: that means the archive is encrypted and what was supplied was
    // rejected, so ask again rather than failing on a typo. A pass that never
    // asked was never encrypted and its reason is the real one. The attempt
    // budget bounds the loop; forgetting the rejected answer is what makes the
    // next attempt prompt instead of replaying the same password.
    if (!session.asked || session.attempts >= kMaxPassphraseAttempts)
      return false;
    session.forget();
  }
}

bool is_rar_archive(const std::filesystem::path &archive) {
  std::ifstream f(archive, std::ios::binary);
  if (!f)
    return false;
  char magic[7] = {};
  f.read(magic, 7);
  static constexpr char kRar4[7] = {'R', 'a', 'r', '!', '\x1a', '\x07', '\x00'};
  static constexpr char kRar5[7] = {'R', 'a', 'r', '!', '\x1a', '\x07', '\x01'};
  return std::memcmp(magic, kRar4, 7) == 0 || std::memcmp(magic, kRar5, 7) == 0;
}

}  // namespace engine
