#pragma once

#include <QColor>
#include <QDir>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace engine {
class ModMeta;
namespace Source::Nexus { struct ModInfoResult; }
namespace Source::LoversLab { struct ModInfoResult; }
using ModInfoResult = Source::Nexus::ModInfoResult;
using LoversLabModInfoResult = Source::LoversLab::ModInfoResult;
}

namespace ui {

// MO2's ModInfoTabIDs - tab order must match ModInfoDialog's tab construction.
enum class ModInfoTabId {
    TextFiles = 0,
    ConfigFiles,
    Images,
    Esps,
    Conflicts,
    Categories,
    Source,
    Notes,
    Filetree,
    Count,
};

// Everything a Mod Info tab needs about the currently selected mod. Built once
// per mod by MainWindow; tabs read it and persist changes through the lambdas.
struct ModInfoData {
    // --- identity / state ---
    QString id;                 // folder name
    QString name;               // display name
    QString version;
    QString color;              // separator color (hex ARGB) when set
    bool enabled = true;
    bool is_separator = false;
    bool is_overwrite = false;
    bool is_game_native = false;
    bool is_merged = false;
    int priority = 0;

    // --- source ---
    QString source_type;        // "nexus" / "workshop" / ...
    QString source_id;
    QString nexus_domain;       // from game knowledge, e.g. "skyrimspecialedition"
    QStringList supported_sources;  // game's download_sources knowledge (display names)
    QString metadata_file;          // from GameKnowledge metadata_file hook, e.g. "metadata.xml" or "meta.ini"
    // Source page URL persisted in the mod's per-source meta section
    // (LoversLab: the page the download came from). Used by "Visit on ...".
    QString source_page_url;
    // Installation (folder birth time) and Changed (folder last-write time).
    // 0 = unavailable (separators, Overwrite/MERGED pseudo-rows). Used for
    // out-of-date detection against dateModified scraped from LoversLab
    // (compare normalized to date granularity so date-without-time beats a
    // mid-day install).
    qint64 installation_ts = 0;

    // --- paths ---
    QDir mod_dir;               // <instance>/mods/<id>
    QString instance_root;
    QString data_subpath;       // mods_subpath hook, e.g. "Data" (Skyrim)

    // --- conflict data (subset of the engine PathRegistry touching this mod) ---
    // path (relative to mod dir) -> [(owner_folder, priority), ...] in the
    // registry's insertion order. `conflict_reversed` inverts priority (Isaac).
    using Owners = std::vector<std::pair<QString, int>>;
    using RegistryEntry = std::pair<QString, Owners>;
    std::vector<RegistryEntry> conflicts;
    bool conflict_reversed = false;
    int conflict_wins = 0;
    int conflict_losses = 0;

    // --- persistence (engine ModMeta handle) ---
    std::function<engine::ModMeta()> load_meta;
    std::function<bool(const engine::ModMeta&)> save_meta;

    // --- actions (wired to MainWindow) ---
    std::function<void()> open_explorer;           // reveal in file manager
    std::function<void(const QString&)> open_file; // open with the default app
    std::function<void(const QString&)> open_url;  // open an http(s) URL
    std::function<bool(const QString&, bool)> hide_file;  // (abs path, hide)
    std::function<void()> refresh_conflicts;       // recompute + reshow dialog
    std::function<bool()> delete_mod;              // remove the mod from the list
    std::function<void(const QColor&)> set_mod_color;  // list separator color

    // Live Nexus mod-info fetch for the Nexus tab's Refresh button (domain and
    // mod id are captured by the caller). available=false on any failure.
    std::function<engine::ModInfoResult()> fetch_nexus_info;

    // Live LoversLab mod-info fetch for the LoversLab tab's Refresh button.
    // Captures the file id or page URL (LoversLab has no domain like Nexus's
    // skyrimspecialedition; the file id IS the identifier). Same
    // available=false failure semantics.
    std::function<engine::LoversLabModInfoResult()> fetch_loverslab_info;

    // The mod's Data directory (mod_dir + data_subpath), if a game ever keeps
    // mods under one. Note: file-walking tabs scan data.mod_dir directly - the
    // mod folder root IS the game-data root (MO2 layout), so data_dir() is
    // currently unused.
    [[nodiscard]] QDir data_dir() const {
        if (data_subpath.isEmpty()) return mod_dir;
        return QDir(mod_dir.filePath(data_subpath));
    }

    // Files (relative to mod_dir) this mod owns and whether its copy wins.
    struct ConflictFile {
        QString rel_path;   // relative to mod dir (for hide/unhide)
        QString display;    // data_subpath stripped, for display
        bool won = false;
    };
    [[nodiscard]] std::vector<ConflictFile> conflict_files() const;

    // Current separator color or invalid QColor.
    [[nodiscard]] QColor color_value() const;
};

}  // namespace ui
