#include "ui/controllers/launch_controller.h"
#include "platform/platform.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/controllers/queue_controller.h"

#include <QApplication>
#include "ui/instance_options/instance_options_widget.h"
#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include "engine/core/events/event_bus.h"
#include "engine/core/instance/instance.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/core/instance/toml_utils.h"
#include "engine/core/log/logger.h"
#include "engine/core/trace/trace_recorder.h"
#include "engine/core/util/debug_env.h"
#include "engine/core/util/fs_utils.h"
#include "engine/deploy/launch/proton_tools.h"
#include "engine/deploy/strategy.h"
#include "engine/game/detect/mod_scanner.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/overwrite/overwrite_utils.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "ui/main_window/deploy_worker.h"
#include "ui/main_window/main_window.h"
#include "ui/instance_options/instance_options_panel.h"
#include "ui/settings/settings.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/executables_dialog.h"
#include "ui/widgets/main_toolbar.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/right_panel.h"

namespace ui {

namespace {

// Reap the subreaper supervisor forked by engine::launcher_game(). It never
// execs (stays "[gamemodmanager]"), so if the watchdog stops without
// waitpid()ing it, it remains a zombie forever. A cgroup-empty result means
// the game and its descendants are gone, so the supervisor exits as soon as
// its reap loop hits ECHILD; poll briefly so a stray reparented daemon can't
// hang the UI thread. Returns the supervisor's exit code (WEXITSTATUS), or -1
// when it could not be reaped.
int reap_supervisor(pid_t pid) {
  if (pid <= 0)
    return -1;
  using namespace std::chrono;
  for (int attempt = 0; attempt < 20; ++attempt) {
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (r < 0 && errno == ECHILD)
      return -1;
    std::this_thread::sleep_for(milliseconds(100));
  }
  engine::Logger::instance().warn("Watchdog: supervisor " +
                                  std::to_string(pid) +
                                  " not reaped after 2s (stray child?)");
  return -1;
}

} // anonymous namespace

// Splits an Executables::Entry "args" string into argv tokens the way a shell would:
// whitespace-separated, double quotes group tokens (quotes removed, backslash
// escapes the next char). Empty input -> empty vector. This is the exact argv
// the launched process sees after the executable path (Issue #34).
std::vector<std::string> split_arguments(const QString &args) {
  std::vector<std::string> out;
  const std::string s = args.toStdString();
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
    if (i >= s.size())
      break;
    std::string token;
    if (s[i] == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size())
          ++i;
        token += s[i++];
      }
      if (i < s.size())
        ++i; // consume closing quote
    } else {
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
        token += s[i++];
    }
    if (!token.empty())
      out.push_back(token);
  }
  return out;
}

// Resolves an Executables::Entry "start_in" working directory against the game dir.
// Empty -> empty (the engine defaults to game_dir); relative -> game_dir +
// start_in; absolute -> as-is. The engine normalizes/validates further and
// downgrades a broken cwd to game_dir rather than aborting the launch.
std::filesystem::path resolve_start_in(const std::filesystem::path &game_dir,
                                       const QString &start_in) {
  if (start_in.isEmpty())
    return {};
  std::filesystem::path p(start_in.toStdString());
  if (!p.is_absolute())
    p = game_dir / p;
  return p;
}

// Quotes a value for the .desktop Exec= line. The XDG spec field-splits the
// line on spaces, so a path containing spaces must be double-quoted
// (backslash escapes inside the quotes). % escaping (field-code expansion) is
// applied by the caller. Unquoted values are left untouched.
QString quote_for_desktop(const QString &s) {
  if (!s.contains(' ') && !s.contains('\t') && !s.contains('"'))
    return s;
  QString q = s;
  q.replace("\\", "\\\\");
  q.replace("\"", "\\\"");
  return "\"" + q + "\"";
}

// Quotes a value for the shell wrapper script generated by
// write_desktop_wrapper: double quotes with backslash escapes for the shell
// metacharacters that can appear inside a path.
QString shell_quote(const QString &s) {
  QString q = s;
  q.replace("\\", "\\\\");
  q.replace("\"", "\\\"");
  q.replace("$", "\\$");
  q.replace("`", "\\`");
  return "\"" + q + "\"";
}

LaunchController::LaunchController(MainWindow *w, QObject *parent)
    : QObject(parent), w_(w) {}

// ---------------------------------------------------------------------------
// Executables persistence (instance.toml `executables` array)
// ---------------------------------------------------------------------------

// Converts an Executables::Entry to a TOML inline table. The array is stored as valid
// TOML (bare keys, `=` separators) — the pre-toml++ JSON-style inline tables
// ({"path":"..."}) were invalid TOML and are migrated on read.
toml::table exec_entry_to_toml(const Executables::Entry &e) {
  toml::table t;
  t.emplace("path", e.path.toStdString());
  t.emplace("title", e.title.toStdString());
  t.emplace("args", e.arguments.toStdString());
  t.emplace("cwd", e.start_in.toStdString());
  t.emplace("mod", e.output_mod.toStdString());
  t.emplace("icon", e.icon_path.toStdString());
  auto env = toml::array{};
  for (const auto &v : e.environment)
    env.push_back(v.toStdString());
  t.emplace("env", std::move(env));
  t.is_inline(true);
  return t;
}

// Converts a TOML inline table back to the compact JSON string consumed by
// ExecControlsBar (the format Executables::Entry::toJson() produces).
std::string toml_table_to_json(const toml::table &t) {
  QJsonObject obj;
  for (const auto &[k, v] : t) {
    const auto key = QString::fromStdString(std::string(k.str()));
    if (auto s = v.value<std::string>()) {
      obj[key] = QString::fromStdString(*s);
    } else if (auto b = v.value<bool>()) {
      obj[key] = *b;
    } else if (auto i = v.value<int64_t>()) {
      obj[key] = static_cast<double>(*i);
    } else if (auto d = v.value<double>()) {
      obj[key] = *d;
    } else if (auto arr = v.as_array()) {
      QJsonArray ja;
      for (const auto &e : *arr) {
        if (auto s = e.value<std::string>())
          ja.append(QString::fromStdString(*s));
      }
      obj[key] = ja;
    }
  }
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact))
      .toStdString();
}

// Parses the `executables` array from instance.toml content and returns each
// entry as a JSON string (the format consumed by ExecControlsBar). Handles
// TOML inline tables and legacy plain-string entries; legacy JSON-style
// inline tables are repaired by engine::parse_instance_toml_content. Empty
// when the key is missing or the content is unparseable.
std::vector<std::string> extract_executables(const std::string &content) {
  std::vector<std::string> out;
  auto tbl = engine::parse_instance_toml_content(content);
  if (!tbl)
    return out;
  auto arr = (*tbl)["executables"].as_array();
  if (!arr)
    return out;
  out.reserve(arr->size());
  for (const auto &elem : *arr) {
    if (auto entry = elem.as_table()) {
      out.push_back(toml_table_to_json(*entry));
    } else if (auto s = elem.value<std::string>()) {
      // Legacy plain string -> wrap in JSON {"path": "..."}.
      out.push_back("{\"path\":\"" + *s + "\"}");
    }
  }
  return out;
}

std::vector<std::string> seed_executable_candidates(
    const std::filesystem::path &game_dir, const std::string &declared,
    const std::vector<std::filesystem::path> &extra_roots) {
  auto names = engine::filter_existing_executables(game_dir, declared);
  for (const auto &root : extra_roots) {
    for (const auto &name :
         engine::filter_existing_executables(root, declared)) {
      const auto base = (root / name).filename();
      bool seen = false;
      for (const auto &n : names) {
        if (std::filesystem::path(n).filename() == base) {
          seen = true;
          break;
        }
      }
      if (!seen)
        names.push_back((root / name).string());
    }
  }
  return names;
}

void LaunchController::save_executables() {
  if (w_->current_instance_root_.empty())
    return;

  auto toml_path = w_->current_instance_root_ / "instance.toml";

  // Read-modify-write: preserve every other key in the file.
  auto tbl = engine::parse_instance_toml(toml_path);
  if (!tbl)
    tbl = toml::table{};

  // Collect JSON objects from the combo
  auto entries = w_->right_panel_->exec_controls()->executable_entries();
  auto arr = toml::array{};
  for (const auto &e : entries)
    arr.push_back(exec_entry_to_toml(e));
  tbl->insert_or_assign("executables", std::move(arr));

  std::ofstream out(toml_path);
  if (!out)
    return;
  out << engine::serialize_instance_toml(*tbl);
}

void LaunchController::load_executables() {
  w_->saved_executables_.clear();
  if (w_->current_instance_root_.empty())
    return;

  auto toml_path = w_->current_instance_root_ / "instance.toml";
  auto tbl = engine::parse_instance_toml(toml_path);
  if (!tbl)
    return;

  auto arr = (*tbl)["executables"].as_array();
  if (!arr)
    return;

  w_->saved_executables_.reserve(arr->size());
  for (const auto &elem : *arr) {
    if (auto entry = elem.as_table()) {
      w_->saved_executables_.push_back(toml_table_to_json(*entry));
    } else if (auto s = elem.value<std::string>()) {
      // Legacy plain string -> wrap in JSON {"path": "..."}.
      w_->saved_executables_.push_back("{\"path\":\"" + *s + "\"}");
    }
  }
}

void LaunchController::populate_executables() {
  if (!w_->knowledge_ || w_->current_game_id_.empty())
    return;

  // Prefer saved executables list (persists user additions across restarts)
  QStringList exec_list;
  if (!w_->saved_executables_.empty()) {
    for (const auto &s : w_->saved_executables_)
      exec_list.append(QString::fromStdString(s));
  } else {
    // First launch - seed from the game plugin's known executables, keeping
    // only names that physically exist in the game dir (Workspace-6su). The
    // scan is the platform filter: a Windows .exe simply does not exist on a
    // macOS game dir and vice versa. Nothing found -> empty list (sentinel-
    // only combo), user adds entries manually. On macOS the game's .app
    // bundle may also live outside game_dir (Workspace-421): Steam can
    // install it into ~/Applications or /Applications, so those roots are
    // scanned as fallbacks and their hits stored as absolute paths.
    const std::string declared =
        w_->knowledge_->get(w_->current_game_id_, "executables", "");
    std::vector<std::filesystem::path> fallback_roots;
#ifdef __APPLE__
    fallback_roots.push_back(engine::safe_home_dir() / "Applications");
    fallback_roots.push_back("/Applications");
#endif
    for (const auto &name : seed_executable_candidates(
             w_->current_game_dir_, declared, fallback_roots))
      exec_list.append(QString::fromStdString(name));
  }

  auto icon_cache = w_->cache_thumbnails_dir_path();
  // Restore the last selected executable for this instance. On a fresh
  // instance the selection is empty - just populate the list and let the
  // user pick. Staging is passed so merged-view (mod-provided) executables
  // still get icons after a deploy.
  auto staging = w_->current_instance_root_.empty()
                     ? std::filesystem::path()
                     : w_->current_instance_root_ / ".gmm_staging";
  w_->right_panel_->exec_controls()->set_executables(
      exec_list, w_->pending_exec_selection_, w_->current_game_dir_, icon_cache,
      staging);

  // Persist immediately on first run so future launches use the saved list
  if (w_->saved_executables_.empty())
    save_executables();

  // Toolbar shortcuts reference executables by path (Issue #34): pins that
  // have no matching entry yet get a minimal entry materialized so the
  // reference resolves and stays editable; legacy per-shortcut icons are
  // folded into the referenced entries. Runs after the combo is populated.
  materialize_toolbar_shortcuts();
}

void LaunchController::materialize_toolbar_shortcuts() {
  if (w_->toolbar_shortcut_paths_.isEmpty())
    return;
  auto *bar = w_->right_panel_->exec_controls();
  auto entries = bar->executable_entries();

  bool changed = false;
  for (const auto &rel_path : w_->toolbar_shortcut_paths_) {
    bool found = false;
    for (auto &e : entries) {
      if (e.path.compare(rel_path, Qt::CaseInsensitive) != 0)
        continue;
      found = true;
      // Fold a legacy per-shortcut icon (pre-#34 instance.toml) into the
      // referenced entry when the entry has none of its own, so the icon
      // survives on the single source of truth.
      auto lit = pending_toolbar_icons_.find(rel_path);
      if (lit != pending_toolbar_icons_.end() && e.icon_path.isEmpty()) {
        e.icon_path = lit.value();
        changed = true;
      }
      pending_toolbar_icons_.erase(lit);
      break;
    }
    if (!found) {
      // The pinned executable's entry was deleted: materialize a minimal
      // path-only entry so the pin resolves and stays editable.
      Executables::Entry e = Executables::Entry::fromLegacyPath(rel_path);
      auto lit = pending_toolbar_icons_.find(rel_path);
      if (lit != pending_toolbar_icons_.end()) {
        e.icon_path = lit.value();
        pending_toolbar_icons_.erase(lit);
      }
      entries.append(e);
      changed = true;
    }
  }

  if (!changed)
    return;

  // Rebuild the combo preserving the selection, then persist.
  auto prev = bar->current_executable();
  bar->clear_executables();
  for (const auto &e : entries)
    bar->add_entry(e);
  if (!prev.isEmpty())
    bar->select_executable(prev);
  save_executables();
  engine::Logger::instance().debug(
      "Toolbar shortcuts: materialized executable entries from pins");
}

void LaunchController::launch_game() {
  // Re-entry guard: a fast double-click / Enter on the focused Run button can
  // fire run_clicked twice while the first launch is still in flight. The
  // overlay covers the mouse but not the keyboard, so guard explicitly.
  if (w_->running_process_pid_ > 0) {
    engine::Logger::instance().debug(
        "Launch skipped - game already running (pid " +
        std::to_string(w_->running_process_pid_) + ")");
    return;
  }

  // Game-less instance (Workspace-wk8): prompt for the path BEFORE the
  // executable check - the exec combo is empty without a game dir, so the
  // "No executable selected" warning would fire misleadingly. A successful
  // pick reloads the instance (populating the exec list) and falls through.
  if (w_->current_game_dir_.empty()) {
    if (!w_->prompt_for_game_path())
      return; // user canceled - nothing to launch into
  }

  auto entry = w_->right_panel_->exec_controls()->current_entry();
  if (entry.path.isEmpty() || entry.path == kAddNewEntryText) {
    QMessageBox::warning(w_, tr("Launch"), tr("No executable selected."));
    return;
  }

  // Resolve against the canonical game dir spelling so the path matches the
  // overlay mountpoint (game_dir commonly goes through ~/.steam ->
  // ~/.local/share/Steam). Entry paths are merged-view (deploy-relative);
  // the namespace-local overlay makes them reachable at launch even though
  // they may not exist physically here. Reachability is validated after
  // deploy in launch_with_executable - entries are never auto-removed.
  std::error_code ce;
  auto canon_game =
      std::filesystem::weakly_canonical(w_->current_game_dir_, ce);
  auto exec_path =
      (ce || canon_game.empty() ? w_->current_game_dir_ : canon_game) /
      entry.path.toStdString();

  // Output-to-mod routing: resolve the target mod folder, auto-creating it
  // (when it doesn't exist yet).
  const auto output_mod_dir = ensure_output_mod_dir(entry.output_mod);
  // Full Executables::Entry config (args, cwd, env) rides along with the launch so the
  // game receives the exact command line the user configured (Issue #34).
  launch_with_executable(QString::fromStdString(exec_path.string()),
                         output_mod_dir, entry.arguments, entry.start_in,
                         entry.environment);
}

std::filesystem::path
LaunchController::ensure_output_mod_dir(const QString &mod_name) {
  if (mod_name.isEmpty())
    return {};
  auto output_mod_dir = w_->mods_dir_path() / mod_name.toStdString();
  std::error_code ec;
  if (!std::filesystem::is_directory(output_mod_dir, ec)) {
    std::filesystem::create_directories(output_mod_dir, ec);
    if (ec) {
      engine::Logger::instance().error("Failed to create output mod folder " +
                                       output_mod_dir.string() + ": " +
                                       ec.message());
      return {};
    }
    auto metadata_file = w_->knowledge_
                             ? w_->knowledge_->get(w_->current_game_id_,
                                                   "metadata_file", "meta.ini")
                             : "meta.ini";
    engine::ModMeta::write_game_metadata(output_mod_dir, metadata_file,
                                         mod_name.toStdString(), "1.0", "0");
    engine::Logger::instance().debug("Output-to-mod: created mod folder " +
                                     output_mod_dir.string());
  }
  return output_mod_dir;
}

void LaunchController::launch_with_executable(
    const QString &full_path, const std::filesystem::path &output_mod_dir,
    const QString &arguments, const QString &start_in,
    const QStringList &environment) {
  // Re-entry guard (also covers toolbar shortcuts, which call this directly).
  if (w_->running_process_pid_ > 0) {
    engine::Logger::instance().debug(
        "Launch skipped - game already running (pid " +
        std::to_string(w_->running_process_pid_) + ")");
    return;
  }
  // A previous launch's deploy is still running (P8.4: the deploy now runs
  // on the worker thread). The continuation will launch or clean up.
  if (w_->launch_prep_pending_) {
    engine::Logger::instance().debug(
        "Launch skipped - deploy already in progress");
    return;
  }

  auto &trace = engine::TraceRecorder::instance();
  trace.begin_flow("launch");

  auto exec_path = std::filesystem::path(full_path.toStdString());

  // Output-to-mod routing (MO2 getByBinary parity): an explicit target from
  // the exec-controls combo wins; otherwise the launched binary's configured
  // output mod is resolved from the executable entries, so toolbar shortcuts
  // and Data-tab Execute honor it too. Unmatched binaries fall back to
  // Overwrite capture.
  std::filesystem::path effective_output = output_mod_dir;
  if (effective_output.empty() && !w_->current_game_dir_.empty()) {
    const QString mod_name = Executables::output_mod_for_path(
        w_->right_panel_->exec_controls()->executable_entries(),
        w_->current_game_dir_, full_path);
    if (!mod_name.isEmpty())
      effective_output = ensure_output_mod_dir(mod_name);
  }
  if (!effective_output.empty()) {
    const QString folder =
        QString::fromStdString(effective_output.filename().string());
    bool disabled = false;
    for (const auto &m : w_->mod_model_->mods()) {
      if (m.id.compare(folder, Qt::CaseInsensitive) == 0) {
        disabled = !m.enabled;
        break;
      }
    }
    if (disabled) {
      engine::Logger::instance().error("Launch blocked - output mod '" +
                                       folder.toStdString() + "' is disabled");
      QMessageBox::warning(
          w_, tr("Launch"),
          tr("The designated write target \"%1\" is not enabled.\n\n"
             "Enable the mod and try again.")
              .arg(folder));
      trace.end_flow("launch", false, "Output mod disabled");
      return;
    }
  }

  // Show the lock overlay before launching - the game must not outrun it
  auto binary_name = QString::fromStdString(exec_path.filename().string());
  show_game_lock_overlay(binary_name, 0);

  // Ensure disk order matches UI before launching
  trace.begin_stage("launch", "Sync disk order");
  w_->mod_list_->sync_priorities();
  trace.end_stage("launch", true, "Disk order matches UI");

  // Delayed disable (plugin-declared capability): apply any deferred
  // disable/enable operations to disk NOW, synchronously on the UI thread,
  // before the DeployWorker starts. The deploy reads on-disk sentinels, so it
  // must see the reconciled state; the flush completing before the worker
  // starts means there is no concurrency issue.
  flush_deferred_disable_queue();

  // Read steam_appid from game plugin hooks - 0 if not registered
  uint32_t steam_appid = 0;
  if (w_->knowledge_) {
    auto id_str = w_->knowledge_->get(w_->current_game_id_, "steam_appid", "");
    if (!id_str.empty()) {
      try {
        steam_appid = std::stoul(id_str);
      } catch (...) {
      }
    }
  }

  // Build a launch-prep snapshot and run the deploy on the worker thread
  // (P8.4). engine::prepare_launch_params deploys all enabled mods into
  // .gmm_staging and returns the assembled params; the worker emits
  // prepared() only after that finished, so launch_game() (in
  // on_launch_params_prepared) provably never starts before the staging
  // tree is complete.
  trace.begin_stage("launch", "Prepare launch environment");
  engine::LaunchPrepRequest req;
  req.instance_root = w_->current_instance_root_;
  req.game_dir = w_->current_game_dir_;
  req.executable = exec_path;
  req.knowledge = w_->knowledge_ ? *w_->knowledge_ : engine::GameKnowledge();
  req.game_id = w_->current_game_id_;
  req.steam_appid = steam_appid;
  req.is_windows_exe = (exec_path.extension().string() == ".exe" ||
                        exec_path.extension().string() == ".EXE");
  req.local_saves_enabled = Settings::instance().local_saves();
  req.platform = w_->platform_;

  // Per-executable config (Issue #34): args/cwd/env ride on the launch-prep
  // snapshot and are applied by the engine at exec time. Empty start_in
  // leaves cwd empty -> the engine defaults to game_dir.
  for (const auto &e : environment)
    req.environment.push_back(e.toStdString());
  req.args = split_arguments(arguments);
  if (!start_in.isEmpty())
    req.cwd = resolve_start_in(w_->current_game_dir_, start_in);

  if (!w_->launch_deploy_thread_) {
    w_->launch_deploy_thread_ = new ui::DeployThread(w_);
    connect(w_->launch_deploy_thread_->worker(), &ui::DeployWorker::progress,
            this, &LaunchController::on_deploy_progress);
    connect(w_->launch_deploy_thread_->worker(), &ui::DeployWorker::prepared,
            this, &LaunchController::on_launch_params_prepared);
  }

  w_->launch_prep_pending_ = true;
  w_->output_mod_dir_ = effective_output;
  w_->output_session_scratch_.clear();
  w_->launch_deploy_thread_->start(std::move(req));
  // Returns immediately; the launch continues in on_launch_params_prepared.
}

void LaunchController::flush_deferred_disable_queue() {
  if (w_->deferred_disable_queue_.empty())
    return;
  if (!w_->knowledge_ || w_->current_game_id_.empty() ||
      w_->current_game_dir_.empty())
    return;

  auto mods_subpath =
      w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  if (mods_subpath.empty()) {
    engine::Logger::instance().warn(
        "Cannot flush deferred disable queue: mods_subpath is empty");
    w_->deferred_disable_queue_.clear();
    return;
  }

  engine::Logger::instance().debug(
      "Flushing " + std::to_string(w_->deferred_disable_queue_.size()) +
      " deferred disable/enable operations before launch");

  // Apply the queued toggles (latest state per mod wins - already deduplicated
  // by sync_mod_enable_state / switch_profile). The deploy worker starts only
  // after this returns, so it reads the reconciled on-disk sentinels.
  for (const auto &op : w_->deferred_disable_queue_) {
    auto mod_folder = w_->resolve_mod_folder(op.mod_id, mods_subpath);
    if (op.enabled) {
      (void)engine::ModScanner::enable_mod(*w_->knowledge_,
                                           w_->current_game_id_, mod_folder);
    } else {
      (void)engine::ModScanner::disable_mod(*w_->knowledge_,
                                            w_->current_game_id_, mod_folder);
    }
  }

  w_->deferred_disable_queue_.clear();
  engine::Logger::instance().debug("Deferred disable queue flushed");
}

void LaunchController::on_deploy_progress(int files_done, int files_total) {
  if (!w_->launch_prep_pending_)
    return;
  if (files_total > 0) {
    w_->game_lock_label_->setText(
        tr("Deploying mods… %1/%2").arg(files_done).arg(files_total));
  }
}

void LaunchController::on_launch_params_prepared(engine::LaunchParams lparams) {
  if (!w_->launch_prep_pending_)
    return;
  w_->launch_prep_pending_ = false;

  auto &trace = engine::TraceRecorder::instance();
  auto exec_path = lparams.executable;
  auto binary_name = QString::fromStdString(exec_path.filename().string());

  // The user may have switched instances while the deploy ran: the staging
  // tree belongs to the OLD instance. Drop the stale result - never launch
  // into the wrong game.
  if (lparams.game_dir != w_->current_game_dir_) {
    engine::Logger::instance().warn(
        "Launch abandoned - instance changed while mods were deploying");
    trace.end_flow("launch", false, "Instance changed mid-deploy");
    hide_game_lock_overlay();
    return;
  }
  trace.end_stage("launch", true, "Launch environment prepared");

  lparams.platform = w_->platform_;
  lparams.overwrite_dir = w_->overwrite_dir_path();

  // MO2-equivalent plugin order: build + write the game's Plugins.txt (and
  // the instance profile) right before launch. No-op for games without
  // plugin support (no localappdata_folder hook).
  trace.begin_stage("launch", "Sync plugin order");
  engine::PluginDatabase::write_plugins_txt_for_launch(
      w_->current_game_dir_, w_->current_instance_root_, w_->current_game_id_,
      lparams.steam_appid,
      w_->knowledge_ ? *w_->knowledge_ : engine::GameKnowledge(),
      w_->platform_);
  trace.end_stage("launch", true, "Plugin order synced");

  // Output-to-mod: capture into a per-launch scratch dir, relay on exit.
  // Empty output_mod_dir_ = default Overwrite capture (toolbar shortcuts).
  w_->output_session_scratch_.clear();
  if (!w_->output_mod_dir_.empty()) {
    auto scratch_base = w_->cache_dir_path();
    if (scratch_base.empty())
      scratch_base = w_->current_game_dir_ / "cache";
    auto session = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    w_->output_session_scratch_ =
        scratch_base / "exec-output" / ("sess-" + std::to_string(session));
    std::error_code ec;
    std::filesystem::create_directories(w_->output_session_scratch_, ec);
    if (ec) {
      engine::Logger::instance().error("Failed to create output scratch dir: " +
                                       ec.message());
      w_->output_session_scratch_.clear();
      w_->output_mod_dir_.clear();
    } else {
      lparams.output_capture_dir = w_->output_session_scratch_;
      engine::Logger::instance().debug("Output-to-mod: capturing to " +
                                       w_->output_session_scratch_.string());
    }
  }

  if (!lparams.extra_lowerdirs.empty())
    w_->staging_dir_ = lparams.extra_lowerdirs.back();

  // Merged-view existence check, AFTER deploy so staging is populated: the
  // file may be game-native (physical), live-overlay (mounted), or a
  // deployed mod file under .gmm_staging. Entries are kept either way - a
  // "missing" file usually just means its mod is disabled.
  if (!engine::merged_view_file_exists(w_->current_game_dir_, w_->staging_dir_,
                                       exec_path)) {
    trace.end_flow("launch", false, "Executable not found");
    hide_game_lock_overlay();
    if (!w_->output_session_scratch_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(w_->output_session_scratch_, ec);
      w_->output_session_scratch_.clear();
    }
    w_->output_mod_dir_.clear();
    QMessageBox::warning(
        w_, tr("Launch"),
        tr("The selected executable is not reachable in the game "
           "directory.\n%1\n\n"
           "If it belongs to a mod, make sure that mod is enabled.")
            .arg(QString::fromStdString(exec_path.string())));
    return;
  }
  trace.end_stage("launch", true, "Overlay/staging paths ready");

  trace.begin_stage("launch", "Launch executable");
  auto lresult = engine::launch_game(lparams);

  if (lresult.pid <= 0) {
    trace.end_stage("launch", false, "launch_game returned no PID");
    hide_game_lock_overlay();
    QMessageBox::warning(w_, tr("Launch"), tr("Failed to launch game."));
    trace.end_flow("launch", false, "Failed to launch game");
    if (!w_->output_session_scratch_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(w_->output_session_scratch_, ec);
      w_->output_session_scratch_.clear();
    }
    w_->output_mod_dir_.clear();
    return;
  }
  trace.end_stage("launch", true,
                  lresult.overlay_launched
                      ? "Launched via OverlayFS / LD_PRELOAD overlay"
                      : "Launched via Native/Proton runtime");

  w_->overlay_launched_ = lresult.overlay_launched;
  w_->running_process_pid_ = lresult.pid;
  w_->cgroup_path_ = lresult.cgroup_path;
  w_->launch_time_ = std::filesystem::file_time_type::clock::now();

  // P1.3 event bus: mirror MO2 onAboutToRun — emitted only once the launch
  // actually succeeded (a PID exists) so a failed launch is not reported.
  engine::EventBus::instance().dispatch(engine::events::kGameLaunched,
                                        engine::json_obj({
                                            {"exe", exec_path.string()},
                                            {"args", ""},
                                        }));

  // Update overlay with actual PID now that we have it
  w_->game_lock_label_->setText(
      tr("The game is running: %1 (%2)").arg(binary_name).arg(lresult.pid));

  // Monitor process - stays Running until the watchdog sees the game exit
  trace.begin_stage("launch", "Monitor process");

  if (!w_->process_watch_timer_) {
    w_->process_watch_timer_ = new QTimer(w_);
    w_->process_watch_timer_->setInterval(2000);
    connect(w_->process_watch_timer_, &QTimer::timeout, this,
            [this]() { check_running_process(); });
  }
  w_->process_watch_timer_->start();
}

void LaunchController::check_running_process() {
  auto &trace = engine::TraceRecorder::instance();
  if (w_->running_process_pid_ <= 0) {
    engine::Logger::instance().warn("Watchdog: pid <= 0, stopping timer");
    if (w_->process_watch_timer_)
      w_->process_watch_timer_->stop();
    return;
  }

#ifdef _WIN32
  HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                            static_cast<DWORD>(w_->running_process_pid_));
  bool alive = false;
  if (proc) {
    DWORD exit_code;
    if (GetExitCodeProcess(proc, &exit_code) && exit_code == STILL_ACTIVE)
      alive = true;
    CloseHandle(proc);
  }
  if (!alive) {
    w_->running_process_pid_ = -1;
    if (w_->process_watch_timer_)
      w_->process_watch_timer_->stop();
    w_->queue_->flush_pending_changes();
    hide_game_lock_overlay();
    trace.end_stage("launch", true, "Game exited");
    trace.end_flow("launch", true, "Game session finished");
    engine::EventBus::instance().dispatch(
        engine::events::kGameFinished, engine::json_obj({{"exit_code", "0"}}));
    if (!w_->staging_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(w_->staging_dir_, ec);
      w_->staging_dir_.clear();
    }
    // Delay capture so any child/spawned processes finish writing
    auto t = w_->launch_time_;
    QTimer::singleShot(Settings::instance().overlay_capture_delay_ms(), this,
                       [this, t]() { do_capture_overwrite(t); });
  }
#else
  // ---- cgroup v2 path (primary) ----
  if (!w_->cgroup_path_.empty()) {
    if ((w_->process_tree_checkbox_ &&
         w_->process_tree_checkbox_->isChecked()) &&
        !engine::cgroup_is_empty({w_->cgroup_path_}))
      refresh_process_tree();

    if (engine::cgroup_is_empty({w_->cgroup_path_})) {
      // A Steam handoff reparents the game's processes to the subreaper
      // supervisor, which stays alive until its last child exits.  The
      // cgroup can be empty in that window while the game still runs.
      // Don't declare the game exited while the supervisor is waiting on
      // reparented children - that would hide the lock overlay and clear
      // the launch guard mid-session.
      //
      // But early-reap the supervisor first: once its last child is
      // reaped it _exit(0)s, and a zombie supervisor still answers
      // kill(pid,0)==0 - which would misclassify a cleanly-exited game
      // as "reparented" forever, leaving the lock overlay up with zero
      // processes.  Same rule as the PGID zombie-gate war story:
      // waitpid(WNOHANG) before trusting kill() liveness.
      bool supervisor_gone = false;
      if (w_->running_process_pid_ > 0) {
        int st;
        pid_t r =
            waitpid(static_cast<pid_t>(w_->running_process_pid_), &st, WNOHANG);
        supervisor_gone = (r == static_cast<pid_t>(w_->running_process_pid_)) ||
                          (r < 0 && errno == ECHILD);
      }
      if (!supervisor_gone && w_->running_process_pid_ > 0 &&
          (kill(static_cast<pid_t>(w_->running_process_pid_), 0) == 0 ||
           errno == EPERM)) {
        engine::Logger::instance().debug(
            "Watchdog: cgroup empty but supervisor alive (reparented game) - "
            "continuing");
        if (w_->process_tree_checkbox_ &&
            w_->process_tree_checkbox_->isChecked())
          refresh_process_tree();
        return;
      }
      engine::Logger::instance().debug(
          "Watchdog: cgroup empty, game fully exited");
      int supervisor_exit =
          reap_supervisor(static_cast<pid_t>(w_->running_process_pid_));
      w_->queue_->flush_pending_changes();
      hide_game_lock_overlay();
      trace.end_stage("launch", true, "Game exited");
      trace.end_flow("launch", true, "Game session finished");
      engine::cgroup_remove({w_->cgroup_path_});
      w_->running_process_pid_ = -1;
      w_->cgroup_path_.clear();
      if (w_->process_watch_timer_)
        w_->process_watch_timer_->stop();
      // P1.3 event bus: mirror MO2 onFinishedRun.
      engine::EventBus::instance().dispatch(
          engine::events::kGameFinished,
          engine::json_obj({{"exit_code", std::to_string(supervisor_exit)}}));
      if (!w_->staging_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(w_->staging_dir_, ec);
        w_->staging_dir_.clear();
      }
      auto t = w_->launch_time_;
      QTimer::singleShot(Settings::instance().overlay_capture_delay_ms(), this,
                         [this, t]() { do_capture_overwrite(t); });
    }
    return;
  }

  // ---- fallback: subreaper + PGID / PPID walk ----
  // Try to reap the root child on every tick.  is_process_group_alive()
  // / kill(-pgid,0) considers zombie processes "alive" (they still have
  // a task_struct entry), which would gate cleanup and cause a permanent
  // zombie.  Early-reaping prevents that.
  int reap_status;
  pid_t early_reaped = waitpid(static_cast<pid_t>(w_->running_process_pid_),
                               &reap_status, WNOHANG);
  if (early_reaped > 0) {
    engine::Logger::instance().debug("Watchdog: early-reaped child PID " +
                                     std::to_string(early_reaped));
  }

  // Linux: track the entire process group (PGID), not a single PID.
  int64_t pgid = w_->running_process_pid_;

  if (engine::is_process_group_alive(pgid)) {
    if (w_->process_tree_checkbox_ && w_->process_tree_checkbox_->isChecked())
      refresh_process_tree();
    return;
  }

  // PGID scan found nothing - try PPID descendant walk.
  // This finds processes that created new sessions via setsid().
  {
    auto descendants = engine::get_process_descendants(pgid);
    bool found_alive = false;
    for (int64_t dpid : descendants) {
      if (dpid == pgid)
        continue;
      if (kill(static_cast<pid_t>(dpid), 0) == 0 || errno == EPERM) {
        found_alive = true;
        break;
      }
    }

    if (w_->process_tree_checkbox_ && w_->process_tree_checkbox_->isChecked())
      refresh_process_tree();

    if (found_alive)
      return;
  }

  engine::Logger::instance().debug("Watchdog: process group " +
                                   std::to_string(pgid) +
                                   " fully exited, scheduling capture in 3s");

  // Safety reap - covers the case where the root PID was already collected
  // above but a second waitpid on the same PID is harmless (returns ECHILD).
  pid_t reap_result = waitpid(static_cast<pid_t>(pgid), &reap_status, WNOHANG);
  if (reap_result == pgid) {
    engine::Logger::instance().debug("Watchdog: reaped child PID " +
                                     std::to_string(pgid));
  } else if (reap_result < 0 && errno != ECHILD) {
    engine::Logger::instance().error("Watchdog: waitpid(" +
                                     std::to_string(pgid) +
                                     ") failed: " + std::strerror(errno));
  }

  w_->queue_->flush_pending_changes();
  hide_game_lock_overlay();
  trace.end_stage("launch", true, "Game exited");
  trace.end_flow("launch", true, "Game session finished");
  w_->running_process_pid_ = -1;
  w_->cgroup_path_.clear();
  if (w_->process_watch_timer_)
    w_->process_watch_timer_->stop();
  // P1.3 event bus: mirror MO2 onFinishedRun (fallback PGID path).
  engine::EventBus::instance().dispatch(
      engine::events::kGameFinished,
      engine::json_obj(
          {{"exit_code", WIFEXITED(reap_status)
                             ? std::to_string(WEXITSTATUS(reap_status))
                             : "-1"}}));
  if (!w_->staging_dir_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(w_->staging_dir_, ec);
    w_->staging_dir_.clear();
  }
  auto t = w_->launch_time_;
  QTimer::singleShot(Settings::instance().overlay_capture_delay_ms(), this,
                     [this, t]() { do_capture_overwrite(t); });
#endif
}

void LaunchController::copy_process_tree() {
  if (!w_->process_tree_)
    return;

  QStringList lines;
  std::function<void(QTreeWidgetItem *, int)> walk = [&](QTreeWidgetItem *item,
                                                         int depth) {
    QString indent(depth * 2, ' ');
    lines << indent + item->text(0) + "  (PID " + item->text(1) + "  " +
                 item->text(2) + ")";
    for (int i = 0; i < item->childCount(); ++i)
      walk(item->child(i), depth + 1);
  };

  for (int i = 0; i < w_->process_tree_->topLevelItemCount(); ++i)
    walk(w_->process_tree_->topLevelItem(i), 0);

  QApplication::clipboard()->setText(lines.join("\n"));
}

void LaunchController::refresh_process_tree() {
  if (!w_->process_tree_)
    return;
  w_->process_tree_->clear();

  if (w_->running_process_pid_ <= 0)
    return;

  struct ProcInfo {
    pid_t pid;
    pid_t ppid;
    std::string name;
    char state;
  };
  std::vector<ProcInfo> procs;

  // When cgroup is available, restrict the scan to its members only.
  // Otherwise scan all of /proc and use PPID walk from the root PID.
  std::unordered_set<pid_t> cgroup_set;
  bool use_cgroup = !w_->cgroup_path_.empty();
  if (use_cgroup) {
    for (int64_t p : engine::cgroup_members({w_->cgroup_path_}))
      cgroup_set.insert(static_cast<pid_t>(p));
  }

  DIR *dir = opendir("/proc");
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (entry->d_type != DT_DIR)
      continue;
    pid_t pid = atol(entry->d_name);
    if (pid <= 0)
      continue;

    if (use_cgroup && !cgroup_set.count(pid))
      continue;

    std::string spath = "/proc/" + std::to_string(pid) + "/stat";
    FILE *f = fopen(spath.c_str(), "r");
    if (!f)
      continue;
    char buf[4096] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char *open_paren = strchr(buf, '(');
    char *close_paren = strrchr(buf, ')');
    if (!open_paren || !close_paren)
      continue;

    std::string comm(open_paren + 1, close_paren - open_paren - 1);
    char *p = close_paren + 1;
    while (*p == ' ')
      ++p;
    char state = *p;

    while (*p && *p != ' ')
      ++p;
    while (*p == ' ')
      ++p;
    pid_t ppid = static_cast<pid_t>(atol(p));

    procs.push_back({pid, ppid, comm, state});
  }
  closedir(dir);

  if (procs.empty())
    return;

  // When using cgroup, the PID list is already complete (no PPID walk needed).
  // Otherwise, run PPID descendant walk from the root PID.
  if (!use_cgroup) {
    pid_t root_pid = static_cast<pid_t>(w_->running_process_pid_);
    if (root_pid <= 0)
      return;

    std::unordered_set<pid_t> descendants;
    descendants.insert(root_pid);
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &pr : procs) {
        if (descendants.count(pr.pid))
          continue;
        if (descendants.count(pr.ppid)) {
          descendants.insert(pr.pid);
          changed = true;
        }
      }
    }

    procs.erase(std::remove_if(procs.begin(), procs.end(),
                               [&](const ProcInfo &pr) {
                                 return !descendants.count(pr.pid);
                               }),
                procs.end());

    if (procs.empty())
      return;
  }

  std::unordered_map<pid_t, QTreeWidgetItem *> items;

  for (const auto &pr : procs) {
    auto *item = new QTreeWidgetItem;
    item->setText(0, QString::fromStdString(pr.name));
    item->setText(1, QString::number(pr.pid));
    item->setText(2, QString(QChar(pr.state)));
    items[pr.pid] = item;
  }

  for (const auto &pr : procs) {
    auto *item = items[pr.pid];
    auto parent_it = items.find(pr.ppid);
    if (parent_it != items.end()) {
      parent_it->second->addChild(item);
    } else {
      w_->process_tree_->addTopLevelItem(item);
    }
  }

  w_->process_tree_->expandAll();
}

void LaunchController::do_capture_overwrite(
    std::filesystem::file_time_type capture_time) {
  if (w_->current_instance_root_.empty() || w_->current_game_dir_.empty())
    return;

  bool case_insensitive =
      w_->knowledge_ &&
      w_->knowledge_->get(w_->current_game_id_, "case_sensitive", "true") ==
          "false";

  bool session_active =
      !w_->output_session_scratch_.empty() && !w_->output_mod_dir_.empty();
  auto capture_dir =
      session_active ? w_->output_session_scratch_ : w_->overwrite_dir_path();

  // When launched via overlay, all writes already went directly into the
  // capture dir (upperdir = session scratch for output-mod, Overwrite
  // otherwise).
  if (w_->overlay_launched_) {
    w_->overlay_launched_ = false;
    engine::Logger::instance().debug("Overlay launched: writes already in " +
                                     capture_dir.string());
  } else {
    engine::capture_overwrite(w_->current_game_dir_, capture_dir, capture_time,
                              case_insensitive);
  }

  // Fold CI-duplicate directories (Meshes/ + meshes/ split by the game's raw
  // case-insensitive writes) back together, for both capture paths. The
  // capture already normalizes internally; this covers the overlay upperdir
  // and the output-session scratch (so relayed mods are merged too).
  if (case_insensitive && engine::normalize_overwrite_casing(capture_dir) > 0) {
    engine::Logger::instance().debug(
        "Overwrite: merged case-insensitive directory duplicates in " +
        capture_dir.string());
  }

  if (session_active) {
    auto mods_subpath =
        w_->knowledge_
            ? w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "")
            : std::string();
    auto inc_id = w_->knowledge_->get(w_->current_game_id_,
                                      "deploy_include_mod_id", "false");
    auto overwrite_dir = w_->overwrite_dir_path();
    auto relayed = engine::relay_output_to_mod(
        w_->output_session_scratch_, w_->output_mod_dir_, overwrite_dir,
        mods_subpath, inc_id == "true",
        w_->output_mod_dir_.filename().string());
    engine::Logger::instance().debug(
        "Output-to-mod: relayed " + std::to_string(relayed) + " file(s) to " +
        w_->output_mod_dir_.string() +
        " (P2: the mod is the full write target, Overwrite untouched)");
    std::error_code ec;
    std::filesystem::remove_all(w_->output_session_scratch_, ec);
    w_->output_session_scratch_.clear();
    w_->output_mod_dir_.clear();
  }

  // Reload mod list so the mod / Overwrite contents become visible
  std::error_code ec;
  if (session_active || std::filesystem::exists(capture_dir, ec))
    w_->mod_list_->load_mods_from_game();
}

void LaunchController::add_shortcut_to_toolbar() {
  auto entry = w_->right_panel_->exec_controls()->current_entry();
  if (entry.path.isEmpty()) {
    QMessageBox::warning(w_, tr("Shortcut"), tr("No executable selected."));
    return;
  }
  if (w_->current_game_dir_.empty()) {
    QMessageBox::warning(w_, tr("Shortcut"), tr("Game directory not set."));
    return;
  }

  // Toolbar shortcuts reference the executable by game-relative path; the
  // full config (args, cwd, env, icon, output mod, title) is inherited from
  // the referenced Executables::Entry at click time (Issue #34).
  add_toolbar_shortcut_from_path(entry.path, entry.icon_path);
}

void LaunchController::add_toolbar_shortcut_from_path(
    const QString &rel_path, const QString &legacy_icon) {
  if (w_->toolbar_shortcut_paths_.contains(rel_path))
    return;

  // Resolve the referenced Executables::Entry for the icon/tooltip. During instance
  // restore this runs before the executables combo is populated, so the
  // lookup may be empty - then we fall back to the legacy icon / extraction.
  Executables::Entry entry;
  for (const auto &e :
       w_->right_panel_->exec_controls()->executable_entries()) {
    if (e.path.compare(rel_path, Qt::CaseInsensitive) == 0) {
      entry = e;
      break;
    }
  }

  QIcon icon;
  if (!entry.icon_path.isEmpty()) {
    QPixmap pix(entry.icon_path);
    if (!pix.isNull())
      icon = QIcon(pix);
  }
  if (icon.isNull() && !legacy_icon.isEmpty()) {
    QPixmap pix(legacy_icon);
    if (!pix.isNull())
      icon = QIcon(pix);
  }
  // A legacy per-shortcut icon (pre-#34 instance.toml) is recorded so
  // materialize_toolbar_shortcuts can fold it into the referenced entry.
  // User-added shortcuts pass the entry's own icon_path, which is already on
  // the entry and must NOT be re-recorded (the entry.icon_path check).
  if (!legacy_icon.isEmpty() && entry.icon_path.isEmpty())
    pending_toolbar_icons_.insert(rel_path, legacy_icon);

  if (icon.isNull()) {
    auto full = w_->current_game_dir_ / rel_path.toStdString();
    icon = ui::extractExeIcon(QString::fromStdString(full.string()),
                              w_->cache_thumbnails_dir_path());
  }

  // Tooltip: the entry's title when set, else game name + exe filename.
  QString tooltip;
  if (!entry.title.isEmpty()) {
    tooltip = entry.title;
  } else {
    auto info = QFileInfo(rel_path);
    auto game_name = QString::fromStdString(w_->current_game_name_.empty()
                                                ? w_->current_game_id_
                                                : w_->current_game_name_);
    game_name.replace('_', ' ');
    tooltip = game_name + " \u2014 " + info.fileName();
  }

  auto *btn = w_->toolbar_->add_exec_button(tooltip, icon);
  btn->setProperty("exec_path", rel_path);
  connect(btn, &QToolButton::clicked, this,
          [this, rel_path]() { launch_toolbar_shortcut(rel_path); });
  w_->toolbar_shortcut_paths_.append(rel_path);
  w_->mod_list_->save_order();
}

void LaunchController::launch_toolbar_shortcut(const QString &rel_path) {
  if (w_->current_game_dir_.empty()) {
    engine::Logger::instance().warn(
        "Toolbar shortcut: no game directory set, launch skipped");
    return;
  }

  // Resolve the referenced Executables::Entry (first-match wins, consistent with
  // output_mod_for_path). A deleted entry keeps the pin and falls back to a
  // path-only launch - the same behavior as before the reference schema.
  Executables::Entry entry;
  for (const auto &e :
       w_->right_panel_->exec_controls()->executable_entries()) {
    if (e.path.compare(rel_path, Qt::CaseInsensitive) == 0) {
      entry = e;
      break;
    }
  }
  if (entry.path.isEmpty())
    entry = Executables::Entry::fromLegacyPath(rel_path);

  // Same merged-view resolution as launch_game: canonical base + the entry's
  // deploy-relative path. Reachability is validated at launch (after deploy).
  std::error_code ce;
  auto canon_game =
      std::filesystem::weakly_canonical(w_->current_game_dir_, ce);
  auto exec_path =
      (ce || canon_game.empty() ? w_->current_game_dir_ : canon_game) /
      rel_path.toStdString();

  // The referenced entry's configured output mod wins (auto-created with the
  // game's metadata file when it doesn't exist yet); the full args/cwd/env
  // config rides along so the toolbar launch is identical to the list + Run.
  const auto output_mod_dir = ensure_output_mod_dir(entry.output_mod);
  launch_with_executable(QString::fromStdString(exec_path.string()),
                         output_mod_dir, entry.arguments, entry.start_in,
                         entry.environment);
}

void LaunchController::add_shortcut_to_desktop() {
  auto entry = w_->right_panel_->exec_controls()->current_entry();
  if (entry.path.isEmpty()) {
    QMessageBox::warning(w_, tr("Shortcut"), tr("No executable selected."));
    return;
  }
  if (w_->current_game_dir_.empty()) {
    QMessageBox::warning(w_, tr("Shortcut"), tr("Game directory not set."));
    return;
  }

  // Same merged-view resolution as the toolbar: canonical game dir spelling
  // + the entry's deploy-relative path. A .desktop file runs OUTSIDE the
  // launch namespace, so only physically present executables can be a
  // desktop target - mod-provided ones get an honest message instead.
  std::error_code ce;
  auto canon_game =
      std::filesystem::weakly_canonical(w_->current_game_dir_, ce);
  auto exec_path =
      (ce || canon_game.empty() ? w_->current_game_dir_ : canon_game) /
      entry.path.toStdString();
  if (!std::filesystem::exists(exec_path)) {
    QMessageBox::warning(
        w_, tr("Shortcut"),
        tr("Executable not found:\n%1\n\n"
           "Mod-provided executables can only be launched from within "
           "GameModManager.")
            .arg(QString::fromStdString(exec_path.string())));
    return;
  }

  // Find the user's desktop directory (XDG or fallback to ~/Desktop)
  auto desktop =
      QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
  if (desktop.isEmpty()) {
    desktop = QString::fromStdString(engine::safe_home_dir().string()) + "/Desktop";
  }
  if (desktop.isEmpty()) {
    QMessageBox::warning(w_, tr("Shortcut"),
                         tr("Could not determine desktop directory."));
    return;
  }

  // Effective working directory: the entry's start_in when set (resolved
  // against the game dir), else the game dir. A broken start_in downgrades to
  // the game dir - same policy as the engine launcher.
  auto work_dir = w_->current_game_dir_;
  if (!entry.start_in.isEmpty()) {
    auto wd = resolve_start_in(w_->current_game_dir_, entry.start_in);
    if (std::filesystem::is_directory(wd, ce))
      work_dir = wd;
  }

  // Effective Exec command: quoted exe path + the entry's args (Issue #34).
  auto exec_qstr =
      quote_for_desktop(QString::fromStdString(exec_path.string()));
  if (!entry.arguments.isEmpty())
    exec_qstr += " " + entry.arguments;

  // Per-entry filename (title or exe basename), never silently overwriting an
  // existing shortcut.
  auto display_name = Executables::exec_entry_display_name(entry);
  auto base_name = display_name;
  base_name.replace(" ", "_");
  base_name.remove(QRegularExpression("[^A-Za-z0-9_.-]"));
  if (base_name.isEmpty())
    base_name = "Game";
  auto desktop_file = desktop + "/" + base_name + ".desktop";
  int suffix = 2;
  while (QFile::exists(desktop_file)) {
    desktop_file =
        desktop + "/" + base_name + "_" + QString::number(suffix) + ".desktop";
    ++suffix;
  }

  // Environment variables cannot be expressed in a .desktop Exec= line: when
  // the entry sets any, generate a wrapper script that exports them and execs
  // the real command. The wrapper lives under the instance cache (survives
  // restarts; the shortcut is explicit, so a deleted instance breaking it is
  // acceptable and visible).
  auto exec_line = exec_qstr;
  QString wrapper_script;
  if (!entry.environment.isEmpty()) {
    wrapper_script =
        write_desktop_wrapper(entry, exec_path, work_dir, base_name);
    if (wrapper_script.isEmpty()) {
      QMessageBox::warning(
          w_, tr("Shortcut"),
          tr("Failed to create the environment wrapper script."));
      return;
    }
    exec_line = quote_for_desktop(wrapper_script);
  }
  // Escape % for the .desktop Exec line (field-code expansion).
  exec_line.replace("%", "%%");

  QFile f(desktop_file);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    QMessageBox::warning(
        w_, tr("Shortcut"),
        tr("Failed to create desktop file:\n%1").arg(desktop_file));
    return;
  }

  QTextStream out(&f);
  out << "[Desktop Entry]\n";
  out << "Type=Application\n";
  out << "Name=" << display_name << "\n";
  out << "Exec=" << exec_line << "\n";
  out << "Path=" << QString::fromStdString(work_dir.string()) << "\n";
  {
    auto icon_for_desktop = entry.icon_path.isEmpty()
                                ? QString::fromStdString(exec_path.string())
                                : entry.icon_path;
    out << "Icon=" << icon_for_desktop << "\n";
  }
  out << "Terminal=false\n";
  out << "Categories=Game;\n";
  out << "Comment=Launch " << display_name << " via GameModManager\n";
  f.close();

  // Make it executable
  QFile::setPermissions(desktop_file, QFile::ReadOwner | QFile::WriteOwner |
                                          QFile::ExeOwner | QFile::ReadGroup |
                                          QFile::ExeGroup | QFile::ReadOther |
                                          QFile::ExeOther);

  engine::Logger::instance().debug("Created desktop shortcut: " +
                                   desktop_file.toStdString());
  if (!wrapper_script.isEmpty())
    engine::Logger::instance().debug("Desktop wrapper script: " +
                                     wrapper_script.toStdString());
  QMessageBox::information(
      w_, tr("Shortcut"),
      tr("Desktop shortcut created:\n%1").arg(desktop_file));
}

QString LaunchController::write_desktop_wrapper(
    const Executables::Entry &entry, const std::filesystem::path &exec_path,
    const std::filesystem::path &work_dir, const QString &base_name) {
  auto cache_dir = w_->cache_dir_path();
  if (cache_dir.empty())
    return {};
  auto dir = cache_dir / "desktop-shortcuts";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec)
    return {};
  auto script = dir / (base_name.toStdString() + ".sh");
  QFile f(QString::fromStdString(script.string()));
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return {};

  auto display_name = Executables::exec_entry_display_name(entry);
  QTextStream out(&f);
  out << "#!/bin/sh\n";
  out << "# GameModManager desktop wrapper for " << display_name << "\n";
  for (const auto &var : entry.environment)
    out << "export \"" << var << "\"\n";
  out << "cd " << shell_quote(QString::fromStdString(work_dir.string()))
      << "\n";
  auto exec_line = shell_quote(QString::fromStdString(exec_path.string()));
  if (!entry.arguments.isEmpty())
    exec_line += " " + entry.arguments;
  out << "exec " << exec_line << "\n";
  f.close();

  QFile::setPermissions(QString::fromStdString(script.string()),
                        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                            QFile::ReadGroup | QFile::ExeGroup |
                            QFile::ReadOther | QFile::ExeOther);
  return QString::fromStdString(script.string());
}

void LaunchController::on_add_entry_requested() {
  auto icon_cache = w_->cache_thumbnails_dir_path();

  // Entries are never auto-pruned here: with merged-view (deploy-relative)
  // paths a "missing" file usually just means its mod is disabled, and it
  // must not be deleted on that basis. A "Clean entries" sweep (MO2-style)
  // is planned separately.
  auto existing = w_->right_panel_->exec_controls()->executable_entries();

  Executables::Dialog dlg(w_->current_game_dir_, output_mod_list(), existing,
                      icon_cache, w_);
  if (dlg.exec() != QDialog::Accepted)
    return;

  apply_exec_entries(dlg.entries());
}

QVector<QPair<QString, QString>> LaunchController::output_mod_list() const {
  // Collect mod list for the "Output to mod" dropdown
  QVector<QPair<QString, QString>> mod_list;
  if (w_->mod_model_) {
    for (const auto &m : w_->mod_model_->mods()) {
      if (!m.is_separator && !m.is_overwrite && !m.is_merged) {
        mod_list.append({m.id, m.name});
      }
    }
  }
  return mod_list;
}

void LaunchController::apply_exec_entries(const QVector<Executables::Entry> &entries) {
  // Replace the entire combo content, then re-apply the selection the user
  // had before opening the editor. Editing must not move the combo to the
  // first or last entry - the selection follows the user until app close.
  auto *bar = w_->right_panel_->exec_controls();
  auto prev_selection = bar->current_executable();
  bar->clear_executables();
  for (const auto &e : entries) {
    bar->add_entry(e);
  }
  if (!prev_selection.isEmpty())
    bar->select_executable(prev_selection);
  save_executables();
}

#ifndef Q_OS_WIN
bool LaunchController::validate_linux_executable(const QString &path) {
  QFileInfo fi(path);
  if (!fi.exists())
    return false;

  // Check extension-based patterns first (fast path)
  auto ext = fi.suffix().toLower();
  if (ext == "exe" || ext == "elf" || ext == "sh" || ext == "appimage" ||
      ext == "bin")
    return true;

  // For extensionless files, use `file --brief --mime-type`
  QProcess proc;
  proc.start("file", QStringList{"--brief", "--mime-type", path});
  if (!proc.waitForFinished(3000))
    return false;

  auto mime = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
  return mime == "application/x-executable" ||
         mime == "application/x-pie-executable" ||
         mime == "application/x-sharedlib" || mime == "text/x-shellscript" ||
         mime == "application/x-mach-binary" ||
         mime == "application/x-msdownload" ||
         mime == "application/x-msdos-program";
}
#endif

void LaunchController::create_game_lock_overlay() {
  w_->game_lock_overlay_ = new QWidget(w_);
  w_->game_lock_overlay_->setObjectName("gameLockOverlay");

  auto *layout = new QVBoxLayout(w_->game_lock_overlay_);
  layout->setAlignment(Qt::AlignCenter);
  layout->setContentsMargins(40, 40, 40, 40);

  layout->addStretch(2);

  w_->game_lock_label_ =
      new QLabel(tr("The game is running"), w_->game_lock_overlay_);
  w_->game_lock_label_->setAlignment(Qt::AlignCenter);
  layout->addWidget(w_->game_lock_label_);

  w_->pending_queue_label_ = new QLabel(w_->game_lock_overlay_);
  w_->pending_queue_label_->setAlignment(Qt::AlignCenter);
  w_->pending_queue_label_->setStyleSheet("color: #f0b000; font-weight: bold;");
  w_->pending_queue_label_->hide();
  layout->addWidget(w_->pending_queue_label_);

  auto *tree_row = new QHBoxLayout;
  tree_row->setAlignment(Qt::AlignCenter);

  w_->process_tree_checkbox_ = new QCheckBox(w_->game_lock_overlay_);
  w_->process_tree_checkbox_->setChecked(w_->show_process_tree_);
  QObject::connect(w_->process_tree_checkbox_, &QCheckBox::toggled, this,
                   [this](bool checked) {
                     w_->show_process_tree_ = checked;
                     if (w_->process_tree_) {
                       w_->process_tree_->setVisible(checked);
                       if (checked)
                         refresh_process_tree();
                     }
                   });
  tree_row->addWidget(w_->process_tree_checkbox_);

  auto *tree_label =
      new QLabel(tr("Show process tree"), w_->game_lock_overlay_);
  tree_label->setObjectName("processTreeLabel");
  tree_row->addWidget(tree_label);

  layout->addLayout(tree_row);

  w_->process_tree_ = new QTreeWidget(w_->game_lock_overlay_);
  w_->process_tree_->setHeaderLabels({tr("Name"), "PID", "S"});
  w_->process_tree_->setColumnWidth(0, 200);
  w_->process_tree_->setColumnWidth(1, 80);
  w_->process_tree_->setColumnWidth(2, 30);
  w_->process_tree_->setAlternatingRowColors(false);
  w_->process_tree_->setRootIsDecorated(true);
  w_->process_tree_->setAnimated(true);
  w_->process_tree_->header()->setStretchLastSection(false);
  w_->process_tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  w_->process_tree_->setVisible(w_->show_process_tree_);
  // Stretch 1 + the addStretch(1) spacers above/below: the tree fills a
  // proportional share of the overlay height and shrinks as the window does,
  // so the Copy/Unlock/Kill controls below it always stay in view.
  layout->addWidget(w_->process_tree_, 1);

  // Copy button row (bottom-right of tree)
  auto *copy_tree_row = new QHBoxLayout;
  copy_tree_row->setContentsMargins(0, 0, 0, 0);
  auto *copy_tree_btn = new QPushButton(tr("Copy"), w_->game_lock_overlay_);
  copy_tree_btn->setFixedSize(52, 22);
  copy_tree_btn->setToolTip(tr("Copy process tree structure to clipboard"));
  copy_tree_btn->setVisible(w_->show_process_tree_);
  QObject::connect(copy_tree_btn, &QPushButton::clicked, this,
                   &LaunchController::copy_process_tree);
  // Show/hide in sync with the tree toggle
  QObject::connect(w_->process_tree_checkbox_, &QCheckBox::toggled,
                   copy_tree_btn, &QPushButton::setVisible);
  copy_tree_row->addStretch();
  copy_tree_row->addWidget(copy_tree_btn);
  layout->addLayout(copy_tree_row);

  layout->addStretch(1);

  auto *btn_layout = new QHBoxLayout;
  btn_layout->setAlignment(Qt::AlignCenter);
  btn_layout->setSpacing(16);

  w_->unlock_button_ = new QPushButton(tr("Unlock"), w_->game_lock_overlay_);
  w_->unlock_button_->setObjectName("unlockBtn");
  QObject::connect(w_->unlock_button_, &QPushButton::clicked, this,
                   [this]() { hide_game_lock_overlay(); });
  btn_layout->addWidget(w_->unlock_button_);

  w_->kill_button_ = new QPushButton(tr("Kill"), w_->game_lock_overlay_);
  w_->kill_button_->setObjectName("killBtn");
  QObject::connect(w_->kill_button_, &QPushButton::clicked, this, [this]() {
    if (w_->running_process_pid_ <= 0) {
      w_->queue_->flush_pending_changes();
      hide_game_lock_overlay();
      return;
    }

    // ---- cgroup v2 kill (primary) ----
    if (!w_->cgroup_path_.empty()) {
      engine::cgroup_kill({w_->cgroup_path_});
      engine::Logger::instance().debug("Kill: cgroup.kill written for " +
                                       w_->cgroup_path_);
      reap_supervisor(static_cast<pid_t>(w_->running_process_pid_));
      engine::cgroup_remove({w_->cgroup_path_});
      w_->running_process_pid_ = -1;
      w_->cgroup_path_.clear();
      if (w_->process_watch_timer_)
        w_->process_watch_timer_->stop();
      w_->queue_->flush_pending_changes();
      hide_game_lock_overlay();
      return;
    }

    // ---- fallback: process group kill ----
    pid_t pgid = static_cast<pid_t>(w_->running_process_pid_);
    if (pgid <= 0) {
      w_->queue_->flush_pending_changes();
      hide_game_lock_overlay();
      return;
    }

    auto reap_or_schedule = [this](pid_t pid) {
      int status;
      pid_t ret = waitpid(pid, &status, WNOHANG);
      if (ret == pid) {
        engine::Logger::instance().debug("Kill: reaped child " +
                                         std::to_string(pid));
        return true;
      }
      if (ret == 0) {
        QTimer::singleShot(3000, this, [this, pid]() {
          int s;
          waitpid(pid, &s, WNOHANG);
        });
        return false;
      }
      return true;
    };

    int ret = kill(-pgid, SIGTERM);
    if (ret != 0) {
      int err = errno;
      if (err == ESRCH) {
        engine::Logger::instance().debug(
            "Kill: process group " + std::to_string(pgid) + " already empty");
        reap_or_schedule(pgid);
        w_->running_process_pid_ = -1;
        if (w_->process_watch_timer_)
          w_->process_watch_timer_->stop();
        w_->queue_->flush_pending_changes();
        hide_game_lock_overlay();
        return;
      }
      engine::Logger::instance().error(
          "Kill: kill(-" + std::to_string(pgid) + ", SIGTERM) failed: " +
          std::strerror(err) + " (" + std::to_string(err) + ")");
      ret = kill(-pgid, SIGKILL);
      if (ret != 0) {
        err = errno;
        engine::Logger::instance().error(
            "Kill: kill(-" + std::to_string(pgid) + ", SIGKILL) failed: " +
            std::strerror(err) + " (" + std::to_string(err) + ")");
        QMessageBox::warning(w_, tr("Kill Failed"),
                             tr("Failed to terminate process group %1: %2")
                                 .arg(static_cast<long long>(pgid))
                                 .arg(std::strerror(err)));
        return;
      }
    }
    engine::Logger::instance().debug("Kill: terminated process group " +
                                     std::to_string(pgid));
    reap_or_schedule(pgid);
    w_->running_process_pid_ = -1;
    if (w_->process_watch_timer_)
      w_->process_watch_timer_->stop();
    w_->queue_->flush_pending_changes();
    hide_game_lock_overlay();
  });
  btn_layout->addWidget(w_->kill_button_);

  layout->addLayout(btn_layout);

  w_->game_lock_overlay_->hide();
}

void LaunchController::show_game_lock_overlay(const QString &binary_name,
                                              int64_t pid) {
  w_->locked_pid_ = pid;
  w_->pending_changes_.clear();
  if (w_->pending_queue_label_)
    w_->pending_queue_label_->hide();
  if (pid > 0) {
    w_->game_lock_label_->setText(
        tr("The game is running: %1 (%2)").arg(binary_name).arg(pid));
  } else {
    w_->game_lock_label_->setText(tr("Launching %1 …").arg(binary_name));
  }

  w_->game_lock_overlay_->setGeometry(w_->rect());
  w_->game_lock_overlay_->raise();
  w_->game_lock_overlay_->show();
  // Grab keyboard focus so Space/Enter can't re-activate the still-focused
  // Run button while the game is starting. The overlay covers the mouse but
  // not the keyboard without this.
  w_->game_lock_overlay_->setFocus();
  w_->game_lock_overlay_->grabKeyboard();
  if (pid > 0 && w_->process_tree_checkbox_ &&
      w_->process_tree_checkbox_->isChecked())
    refresh_process_tree();
}

void LaunchController::hide_game_lock_overlay() {
  w_->locked_pid_ = -1;
  if (w_->game_lock_overlay_ && w_->game_lock_overlay_->isVisible())
    w_->game_lock_overlay_->releaseKeyboard();
  w_->game_lock_overlay_->hide();
}

InstanceOptionsParams LaunchController::instance_options_params() const {
  InstanceOptionsParams p;
  if (w_->current_instance_root_.empty())
    return p;

  engine::Instance inst =
      engine::Instance::from_root(w_->current_instance_root_);
  inst.read_toml();

  p.game_id = w_->current_game_id_;
  p.game_name = w_->current_game_name_.empty() ? w_->current_game_id_
                                               : w_->current_game_name_;
  p.game_dir = w_->current_game_dir_;
  p.instance_root = w_->current_instance_root_;
  p.current_runner = inst.info().proton_runner;

  uint32_t steam_appid = inst.info().steam_appid;
  if (steam_appid == 0 && w_->knowledge_) {
    auto id_str = w_->knowledge_->get(w_->current_game_id_, "steam_appid", "");
    if (!id_str.empty()) {
      try {
        steam_appid = std::stoul(id_str);
      } catch (...) {
      }
    }
  }
  p.steam_appid = steam_appid;

  // Snapshot the game knowledge (read-only after plugin registration) so the
  // panel's deploy management section uses the exact same effective strategy
  // and DeployConfig as the launch path (instance_utils: single source of
  // truth for direct-symlink deploys).
  engine::GameKnowledge knowledge =
      w_->knowledge_ ? *w_->knowledge_ : engine::GameKnowledge();
  p.deploy_strategy = engine::effective_deploy_strategy(
      w_->current_instance_root_, knowledge, w_->current_game_id_);
  p.deploy_config = engine::deploy_config_for(
      w_->current_instance_root_, w_->current_game_dir_, knowledge,
      w_->current_game_id_);
  p.valid = true;
  return p;
}

void LaunchController::show_instance_options() {
  InstanceOptionsParams p = instance_options_params();
  if (!p.valid) {
    QMessageBox::information(w_, tr("Instance Options"),
                             tr("No instance is currently loaded."));
    return;
  }

  // Game-less instance (Workspace-wk8): Deploy Management would operate on
  // an empty game dir. Prompt up front so the panel is built with a valid
  // DeployConfig; canceling skips the dialog entirely.
  if (p.game_dir.empty() && !w_->prompt_for_game_path())
    return;
  if (p.game_dir.empty())
    p = instance_options_params(); // re-read with the freshly picked dir

  ui::InstanceOptionsDialog dlg(w_->platform_, w_->plugin_loader_, p.game_id,
                                p.game_name, p.game_dir, p.steam_appid,
                                p.instance_root, p.current_runner,
                                p.deploy_strategy, p.deploy_config, w_);
  // The deploy management section must flush the deferred disable queue before
  // Force re-deploy / Remove, exactly like the launch path does (the deploy
  // reads on-disk sentinels, so queued toggles must be applied first).
  dlg.content()->set_flush_deferred_disable_queue(
      [this]() { flush_deferred_disable_queue(); });
  if (dlg.exec() == QDialog::Accepted) {
    auto runner = dlg.selected_runner();
    engine::Instance write = engine::Instance::from_root(p.instance_root);
    write.read_toml();
    write.write_key("proton_runner", runner);
    w_->current_instance_ = write;
  }
}

engine::ProtonToolRequest LaunchController::current_proton_request() const {
  engine::ProtonToolRequest request;
  if (w_->current_instance_root_.empty())
    return request;
  request.platform = w_->platform_;
  request.game_dir = w_->current_game_dir_;

  engine::Instance inst =
      engine::Instance::from_root(w_->current_instance_root_);
  if (inst.read_toml()) {
    request.runner_override = inst.info().proton_runner;
    request.steam_appid = inst.info().steam_appid;
  }
  if (request.steam_appid == 0 && w_->knowledge_) {
    auto id_str = w_->knowledge_->get(w_->current_game_id_, "steam_appid", "");
    if (!id_str.empty()) {
      try {
        request.steam_appid = std::stoul(id_str);
      } catch (...) {
      }
    }
  }
  return request;
}

void LaunchController::run_prefix_tool(const QStringList &args) {
  auto request = current_proton_request();
  if (request.platform == nullptr || request.steam_appid == 0) {
    QMessageBox::information(
        w_, tr("Proton Tools"),
        tr("No Steam game is loaded — a Proton prefix is required."));
    return;
  }

  std::vector<std::string> argv;
  for (const auto &a : args)
    argv.push_back(a.toStdString());

  int64_t pid = engine::run_proton_tool(request, argv);
  if (pid < 0) {
    QMessageBox::warning(
        w_, tr("Proton Tools"),
        tr("No protontricks / winetricks / Wine available to run this tool."));
  }
}

void LaunchController::run_exe_in_prefix() {
  auto request = current_proton_request();
  if (request.platform == nullptr || request.steam_appid == 0) {
    QMessageBox::information(
        w_, tr("Proton Tools"),
        tr("No Steam game is loaded — a Proton prefix is required."));
    return;
  }

  const QString file = QFileDialog::getOpenFileName(
      w_, tr("Run an .exe in this prefix"),
      QString::fromStdString(w_->current_game_dir_.string()),
      tr("Windows executables (*.exe);;All files (*)"));
  if (file.isEmpty())
    return;

  int64_t pid = engine::run_proton_exe(
      request, std::filesystem::path(file.toStdString()));
  if (pid < 0) {
    QMessageBox::warning(w_, tr("Proton Tools"),
                         tr("Failed to run:\n%1").arg(file));
  }
}

} // namespace ui