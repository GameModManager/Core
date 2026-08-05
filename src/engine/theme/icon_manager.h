#pragma once

#include <QIcon>
#include <QStyle>
#include <QString>

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// Central icon resolution. Every logical icon key ("list-add",
// "conflict-overwrite", "gmm-logo", ...) resolves through one chain, so a
// theme or icon pack can override any icon and every caller sees the same
// result. The chain is:
//
//   1. override source (skipped when mode == "system"):
//        "default" -> the active theme's icons dir (themes/<theme>/icons)
//        "<pack>"  -> resources/icons/packs/<pack>/
//   2. bundled app asset      resources/icons/<key>.png|.svg
//   3. desktop icon theme     QIcon::fromTheme(key)
//   4. fugue base pack        resources/icons/packs/fugue/<key>.png|.svg
//   5. caller standardIcon fallback (QStyle::SP_CustomBase = none)
//
// Mode comes from Settings::icon_pack() ("default" | "system" | pack name);
// main.cpp syncs it once at startup and again when the settings dialog or a
// theme change updates it.
class IconManager {
public:
    static IconManager& instance();

    // Scan <appDir>/../resources/icons/packs/ for bundled packs. Idempotent.
    void discover_packs(const std::filesystem::path& app_dir);

    // Bundled pack names, sorted. "fugue" is always included (the base pack).
    std::vector<std::string> pack_names() const;

    // Resolution mode: "default", "system", or a pack name from pack_names().
    void set_mode(const std::string& mode);
    std::string mode() const { return mode_; }

    // Active QSS theme name (""/"default" = no theme-icons override).
    void set_current_theme(const std::string& name);

    // Resolve a logical icon key through the chain above. `sp` is the
    // per-callsite last-resort fallback (pass QStyle::SP_CustomBase to skip).
    QIcon resolve_icon(const QString& key,
                       QStyle::StandardPixmap sp = QStyle::SP_CustomBase) const;

private:
    IconManager() = default;

    // Look for <dir>/<key>.png|.svg|... and load it, or a null icon.
    QIcon load_from_dir(const std::filesystem::path& dir, const QString& key) const;

    std::filesystem::path app_dir_;
    std::filesystem::path resources_dir_;
    std::filesystem::path packs_dir_;
    std::string mode_ = "default";
    std::string current_theme_;
    std::vector<std::string> packs_;
};

}  // namespace engine
