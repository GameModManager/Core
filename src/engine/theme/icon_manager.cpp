#include "engine/theme/icon_manager.h"
#include "engine/theme/theme_manager.h"

#include <QApplication>
#include <QFileInfo>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace engine {

std::string vendor_icon_key(const std::string& source) {
    std::string low;
    low.reserve(source.size());
    for (char c : source) {
        if (c == ' ')
            continue;
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (low == "nexus" || low == "nexusmods") return "nexusmods";
    if (low == "loverslab") return "loverslab";
    if (low == "steam" || low == "steamworkshop") return "steam";
    if (low == "moddb") return "moddb";
    return "";
}

IconManager& IconManager::instance() {
    static IconManager mgr;
    return mgr;
}

void IconManager::discover_packs(const std::filesystem::path& app_dir) {
    // Resources live beside the binary's parent (dev: Core/build -> Core/resources;
    // portable/installed: <app>/../resources) - same layout as theme_search_dirs.
    app_dir_ = app_dir;
    resources_dir_ = app_dir.parent_path() / "resources";
    packs_dir_ = resources_dir_ / "icons" / "packs";
    packs_.clear();

    std::error_code ec;
    if (!std::filesystem::exists(packs_dir_, ec) ||
        !std::filesystem::is_directory(packs_dir_, ec)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(packs_dir_, ec)) {
        if (entry.is_directory()) {
            packs_.push_back(entry.path().filename().string());
        }
    }
    std::sort(packs_.begin(), packs_.end());
}

std::vector<std::string> IconManager::pack_names() const {
    return packs_;
}

void IconManager::set_mode(const std::string& mode) {
    mode_ = mode;
}

void IconManager::set_current_theme(const std::string& name) {
    current_theme_ = name;
}

QIcon IconManager::load_from_dir(const std::filesystem::path& dir,
                                 const QString& key) const {
    if (dir.empty()) return {};

    std::error_code ec;
    std::filesystem::path base = dir / key.toStdString();
    for (const char* ext : {".png", ".svg", ".jpg", ".jpeg", ".gif", ".ico"}) {
        std::filesystem::path candidate = base;
        candidate += ext;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return QIcon(QString::fromStdString(candidate.string()));
        }
    }
    return {};
}

QIcon IconManager::resolve_icon(const QString& key, QStyle::StandardPixmap sp) const {
    // 1. Override source (theme icons dir, or the selected pack). "system"
    //    mode skips this tier entirely.
    if (mode_ != "system") {
        if (mode_ == "default") {
            if (!current_theme_.empty() && current_theme_ != "default" && !app_dir_.empty()) {
                for (const auto& dir : theme_search_dirs(app_dir_)) {
                    QIcon icon = load_from_dir(dir / current_theme_ / "icons", key);
                    if (!icon.isNull()) return icon;
                }
            }
        } else {
            QIcon icon = load_from_dir(packs_dir_ / mode_, key);
            if (!icon.isNull()) return icon;
        }
    }

    // 2/3. System tier vs bundled app defaults. In "system" mode the desktop
    //      icon theme wins over the app's own bundled defaults; otherwise the
    //      bundled defaults (the app's theme surface) win over the desktop.
    QIcon bundled;
    if (!resources_dir_.empty()) {
        bundled = load_from_dir(resources_dir_ / "icons", key);
        // Branded source icons live in resources/icons/vendor/<key>.* so the
        // root icons dir stays free of dead leftovers.
        if (bundled.isNull())
            bundled = load_from_dir(resources_dir_ / "icons" / "vendor", key);
    }
    QIcon theme_icon = QIcon::fromTheme(key);
    if (mode_ == "system") {
        if (!theme_icon.isNull()) return theme_icon;
        if (!bundled.isNull()) return bundled;
    } else {
        if (!bundled.isNull()) return bundled;
        if (!theme_icon.isNull()) return theme_icon;
    }

    // 4. Fugue base pack (always-on safety net).
    if (!packs_dir_.empty()) {
        QIcon icon = load_from_dir(packs_dir_ / "fugue", key);
        if (!icon.isNull()) return icon;
    }

    // 5. Caller-provided standard-icon fallback.
    if (sp != QStyle::SP_CustomBase) {
        QIcon icon = QApplication::style()->standardIcon(sp);
        if (!icon.isNull()) return icon;
    }

    return {};
}

}  // namespace engine
