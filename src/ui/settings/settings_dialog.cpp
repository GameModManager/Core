#include "ui/settings/settings_dialog.h"
#include "ui/settings/settings.h"
#include "ui/settings/source_pages.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/source/source_provider.h"
#include "engine/theme/style_manager.h"
#include "engine/theme/icon_manager.h"
#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/log/logger.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleFactory>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

namespace {

// Minimum window geometry for the settings dialog (H634 x W723).
constexpr int kMinDialogWidth = 723;
constexpr int kMinDialogHeight = 634;

} // namespace

SettingsDialog::SettingsDialog(engine::StyleManager* style_manager,
                               const QString& native_style_name,
                               const std::filesystem::path& instance_root,
                               engine::PluginLoader* plugin_loader,
                               QWidget* parent)
    : QDialog(parent), style_manager_(style_manager),
      native_style_name_(native_style_name), instance_root_(instance_root),
      plugin_loader_(plugin_loader) {
    setWindowTitle(tr("Settings"));
    // Min geometry H634 x W723: the Plugins tab's two columns need the width
    // (left plugin list ~half, right info pane ~half) at minimum window size.
    setMinimumSize(kMinDialogWidth, kMinDialogHeight);

    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);  // tab pages fill the whole window height

    tabs->addTab(build_general_tab(), tr("General"));
    tabs->addTab(build_theme_tab(), tr("Theme"));
    tabs->addTab(build_modlist_tab(), tr("Mod List"));
    tabs->addTab(build_paths_tab(), tr("Paths"));
    tabs->addTab(build_sources_tab(), tr("Sources"));
    tabs->addTab(build_plugins_tab(), tr("Plugins"));
    tabs->addTab(build_workarounds_tab(), tr("Workarounds"));
    tabs->addTab(build_fomod_tab(), tr("FOMOD"));
    tabs->addTab(build_diagnostics_tab(), tr("Diagnostics"));

    // P1.5: plugin-declared typed settings tabs (register_settings_tab)
    // appended after the fixed tabs, one per plugin.
    append_plugin_settings_tabs(tabs);

    auto* btn_box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(btn_box, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(btn_box);
}

// -- General ----------------------------------------------------------------

QWidget* SettingsDialog::build_general_tab() {
    auto& s = Settings::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    // Language ---------------------------------------------------------------
    auto* lang_group = new QGroupBox(tr("Language"), page);
    auto* lang_layout = new QVBoxLayout(lang_group);
    auto* lang_combo = new QComboBox(lang_group);
    QDir i18n_dir(":/i18n");
    for (const auto& info : i18n_dir.entryInfoList({"*.qm"}, QDir::Files | QDir::NoDotAndDotDot)) {
        const QString tag = info.completeBaseName();
        const QLocale loc(tag);
        const QString display = loc.language() != QLocale::C
            ? loc.nativeLanguageName() + " (" + tag + ")"
            : tag;
        lang_combo->addItem(display, tag);
    }
    int lang_idx = lang_combo->findData(s.language());
    lang_combo->setCurrentIndex(lang_idx >= 0 ? lang_idx : 0);
    auto* lang_hint = new QLabel(tr("Restart the application for the language change to take effect."), lang_group);
    lang_hint->setWordWrap(true);
    lang_layout->addWidget(lang_combo);
    lang_layout->addWidget(lang_hint);
    layout->addWidget(lang_group);

    connect(lang_combo, &QComboBox::currentIndexChanged, this, [&s, lang_combo](int index) {
        s.set_language(lang_combo->itemData(index).toString());
    });

    // General options ---------------------------------------------------------
    auto* gen_group = new QGroupBox(tr("General"), page);
    auto* gen_layout = new QVBoxLayout(gen_group);
    auto* update_box = new QCheckBox(tr("Check for updates on startup"), gen_group);
    update_box->setChecked(s.check_for_updates());
    auto* prerelease_box = new QCheckBox(tr("Use prerelease updates"), gen_group);
    prerelease_box->setChecked(s.use_prereleases());
    auto* smooth_box = new QCheckBox(tr("Smooth scrolling in lists"), gen_group);
    smooth_box->setChecked(s.smooth_scrolling());
    smooth_box->setToolTip(tr("Animates wheel scrolling in mod/executable lists."));
    auto* dl_notify_box = new QCheckBox(tr("Show download notifications"), gen_group);
    dl_notify_box->setChecked(s.show_download_notifications());
    gen_layout->addWidget(update_box);
    gen_layout->addWidget(prerelease_box);
    gen_layout->addWidget(smooth_box);
    gen_layout->addWidget(dl_notify_box);
    layout->addWidget(gen_group);

    connect(update_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_check_for_updates(on); });
    connect(prerelease_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_use_prereleases(on); });
    connect(smooth_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_smooth_scrolling(on); });
    connect(dl_notify_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_show_download_notifications(on); });

    // Profile defaults ---------------------------------------------------------
    auto* profile_group = new QGroupBox(tr("Profile Defaults"), page);
    auto* profile_layout = new QVBoxLayout(profile_group);
    auto* saves_box = new QCheckBox(tr("Store savegames per profile"), profile_group);
    saves_box->setChecked(s.local_saves());
    auto* inis_box = new QCheckBox(tr("Store INI files per profile"), profile_group);
    inis_box->setChecked(s.local_inis());
    auto* archinv_box = new QCheckBox(tr("Automatic archive invalidation"), profile_group);
    archinv_box->setChecked(s.archive_invalidation());
    profile_layout->addWidget(saves_box);
    profile_layout->addWidget(inis_box);
    profile_layout->addWidget(archinv_box);
    layout->addWidget(profile_group);

    connect(saves_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_local_saves(on); });
    connect(inis_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_local_inis(on); });
    connect(archinv_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_archive_invalidation(on); });

    // Geometry ------------------------------------------------------------------
    auto* geom_group = new QGroupBox(tr("Windows"), page);
    auto* geom_layout = new QVBoxLayout(geom_group);
    auto* center_box = new QCheckBox(tr("Center dialogs on screen"), geom_group);
    center_box->setChecked(s.center_dialogs());
    auto* reset_btn = new QPushButton(tr("Reset dialog sizes and positions"), geom_group);
    geom_layout->addWidget(center_box);
    geom_layout->addWidget(reset_btn);
    layout->addWidget(geom_group);

    connect(center_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_center_dialogs(on); });
    connect(reset_btn, &QPushButton::clicked, this, [&s]() {
        s.reset_dialog_geometry();
        QMessageBox::information(nullptr, QObject::tr("Settings"),
                                 QObject::tr("Dialog sizes and positions have been reset."));
    });

    layout->addStretch(1);
    return page;
}

// -- Theme --------------------------------------------------------------------

QWidget* SettingsDialog::build_theme_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* theme_combo = new QComboBox(page);
    theme_combo->addItem(tr("Default (system)"), "default");
    for (const auto& key : QStyleFactory::keys())
        theme_combo->addItem(key, "qt:" + key);
    if (style_manager_) {
        const auto theme_names = style_manager_->theme_names();
        if (!theme_names.empty()) {
            theme_combo->insertSeparator(theme_combo->count());
            for (const auto& name : theme_names)
                theme_combo->addItem(QString::fromStdString(name), QString::fromStdString(name));
        }
    }

    auto& s = Settings::instance();
    const QString current_style = s.style();
    const QString current_theme = s.theme();
    int theme_idx = theme_combo->findData("qt:" + current_style);
    if (theme_idx < 0)
        theme_idx = theme_combo->findData(current_theme);
    theme_combo->setCurrentIndex(theme_idx >= 0 ? theme_idx : 0);

    // The old "Style" / "Icon pack" boxes were replaced by a plain two-row form;
    // the hints that used to sit below each combo now hover over the selector.
    theme_combo->setToolTip(
        tr("Editing a theme's .qss or tokens.json on disk live-reloads it. "
           "Qt styles (Fusion, Windows, ...) are the built-in Qt look - no custom theme files."));

    // -- Icon pack ---------------------------------------------------------
    auto* pack_combo = new QComboBox(page);
    pack_combo->addItem(tr("Default (theme then system)"), "default");
    pack_combo->addItem(tr("System (ignore theme and pack icons)"), "system");
    const auto pack_names = engine::IconManager::instance().pack_names();
    if (!pack_names.empty()) {
        pack_combo->insertSeparator(pack_combo->count());
        for (const auto& name : pack_names)
            pack_combo->addItem(QString::fromStdString(name),
                                QString::fromStdString(name));
    }
    const QString current_pack = s.icon_pack();
    int pack_idx = pack_combo->findData(current_pack);
    pack_combo->setCurrentIndex(pack_idx >= 0 ? pack_idx : 0);
    pack_combo->setToolTip(
        tr("Icons resolve as: theme/pack icons first, then the system icon "
           "theme, then the bundled Fugue base pack. \"System\" ignores theme "
           "and pack icons entirely. Menu icons apply immediately; toolbar and "
           "list icons refresh on the next launch."));

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->addRow(tr("Style:"), theme_combo);
    form->addRow(tr("Icons:"), pack_combo);
    layout->addLayout(form);

    connect(pack_combo, &QComboBox::currentIndexChanged, this,
            [&s, pack_combo](int index) {
                const QString data = pack_combo->itemData(index).toString();
                s.set_icon_pack(data);
                engine::IconManager::instance().set_mode(data.toStdString());
                engine::Logger::instance().info(
                    "Icon pack: " + data.toStdString());
            });

    connect(theme_combo, &QComboBox::currentIndexChanged, this, [this, &s, theme_combo](int index) {
        const QString data = theme_combo->itemData(index).toString();
        if (data.startsWith("qt:")) {
            // Built-in Qt style: no custom QSS, no GMM theme.
            const QString style = data.mid(3);
            s.set_style(style);
            s.clear_theme();
            if (QStyle* st = QStyleFactory::create(style))
                qApp->setStyle(st);
            qApp->setStyleSheet(QString());
            engine::Logger::instance().info("Applied Qt style: " + style.toStdString());
            return;
        }
        // GMM theme (or Default): restore the native platform style first so
        // the QSS renders on Breeze/etc., then apply the theme.
        s.set_theme(data);
        s.clear_style();
        if (!native_style_name_.isEmpty()) {
            if (QStyle* st = QStyleFactory::create(native_style_name_))
                qApp->setStyle(st);
        }
        if (style_manager_)
            style_manager_->apply_theme(data.toStdString());
    });

    // -- Colors: MO2 ColorTable parity -------------------------------------
    auto* colors_group = new QGroupBox(tr("Colors"), page);
    auto* colors_form = new QFormLayout(colors_group);
    colors_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    auto make_swatch = [this](QColor initial, std::function<void(const QColor&)> commit,
                              QWidget* parent) {
        auto* btn = new QPushButton(parent);
        btn->setFixedSize(56, 22);
        btn->setCursor(Qt::PointingHandCursor);
        auto apply_swatch = [btn](const QColor& c) {
            btn->setStyleSheet(
                QString("background-color: rgba(%1,%2,%3,%4);")
                    .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()));
        };
        apply_swatch(initial);
        QObject::connect(btn, &QPushButton::clicked, btn, [btn, commit, apply_swatch]() {
            QColor result = QColorDialog::getColor(
                btn->property("current").value<QColor>(), btn,
                tr("Choose color"), QColorDialog::ShowAlphaChannel);
            if (result.isValid()) {
                btn->setProperty("current", result);
                apply_swatch(result);
                commit(result);
            }
        });
        btn->setProperty("current", initial);
        return btn;
    };

    struct ColorRow { const char* label; QColor current; std::function<void(const QColor&)> commit; };
    const auto rows = {
        ColorRow{ QT_TR_NOOP("Is overwritten (loose files)"), s.modlist_overwritten_loose(),
                  [&s](const QColor& c) { s.set_modlist_overwritten_loose(c); } },
        ColorRow{ QT_TR_NOOP("Is overwriting (loose files)"), s.modlist_overwriting_loose(),
                  [&s](const QColor& c) { s.set_modlist_overwriting_loose(c); } },
        ColorRow{ QT_TR_NOOP("Is overwritten (archives)"), s.modlist_overwritten_archive(),
                  [&s](const QColor& c) { s.set_modlist_overwritten_archive(c); } },
        ColorRow{ QT_TR_NOOP("Is overwriting (archives)"), s.modlist_overwriting_archive(),
                  [&s](const QColor& c) { s.set_modlist_overwriting_archive(c); } },
        ColorRow{ QT_TR_NOOP("Mod contains selected file"), s.modlist_contains_file(),
                  [&s](const QColor& c) { s.set_modlist_contains_file(c); } },
        ColorRow{ QT_TR_NOOP("Plugin is contained in selected mod"), s.plugin_list_contained(),
                  [&s](const QColor& c) { s.set_plugin_list_contained(c); } },
        ColorRow{ QT_TR_NOOP("Plugin is master of selected plugin"), s.plugin_list_master(),
                  [&s](const QColor& c) { s.set_plugin_list_master(c); } },
    };
    for (const auto& row : rows) {
        colors_form->addRow(tr(row.label), make_swatch(row.current, row.commit, colors_group));
    }

    auto* reset_colors = new QPushButton(tr("Reset colors"), colors_group);
    reset_colors->setToolTip(tr("Restore the default MO2 colors."));
    connect(reset_colors, &QPushButton::clicked, this, [&s]() {
        s.set_modlist_overwritten_loose(QColor(0, 255, 0, 64));
        s.set_modlist_overwriting_loose(QColor(255, 0, 0, 64));
        s.set_modlist_overwritten_archive(QColor(0, 255, 255, 64));
        s.set_modlist_overwriting_archive(QColor(255, 0, 255, 64));
        s.set_modlist_contains_file(QColor(0, 0, 255, 64));
        s.set_plugin_list_contained(QColor(0, 0, 255, 64));
        s.set_plugin_list_master(QColor(255, 255, 0, 64));
    });
    colors_form->addRow(QString(), reset_colors);
    layout->addWidget(colors_group);

    // -- Design ----------------------------------------------------------
    auto* design_group = new QGroupBox(tr("Design"), page);
    auto* design_form = new QFormLayout(design_group);
    design_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto* compact_box = new QCheckBox(tr("Compact rows in the Downloads tab"), design_group);
    compact_box->setChecked(s.compact_downloads());
    design_form->addRow(compact_box);
    connect(compact_box, &QCheckBox::toggled, this,
            [&s](bool on) { s.set_compact_downloads(on); });
    auto* center_sep_box = new QCheckBox(tr("Center text on separators"), design_group);
    center_sep_box->setChecked(s.center_separator_text());
    design_form->addRow(center_sep_box);
    connect(center_sep_box, &QCheckBox::toggled, this,
            [&s](bool on) { s.set_center_separator_text(on); });
    layout->addWidget(design_group);

    layout->addStretch(1);
    return page;
}

// -- Mod List ------------------------------------------------------------------

QWidget* SettingsDialog::build_modlist_tab() {
    auto& s = Settings::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    // Flat options, no group box (MO2 layout)
    auto* foreign_box = new QCheckBox(tr("Display foreign mods (DLC, Creation Club)"), page);
    foreign_box->setChecked(s.display_foreign());
    auto* save_filters_box = new QCheckBox(tr("Remember filter settings"), page);
    save_filters_box->setChecked(s.save_filters());
    auto* hover_box = new QCheckBox(tr("Auto-collapse separators on hover"), page);
    hover_box->setChecked(s.auto_collapse_on_hover());
    auto* sep_scrollbar_box = new QCheckBox(tr("Color the scrollbar at separators"), page);
    sep_scrollbar_box->setChecked(s.color_separator_scrollbar());
    auto* check_update_box = new QCheckBox(tr("Check for updates after install"), page);
    check_update_box->setChecked(s.check_update_after_install());

    layout->addWidget(foreign_box);
    layout->addWidget(save_filters_box);
    layout->addWidget(hover_box);
    layout->addWidget(sep_scrollbar_box);
    layout->addWidget(check_update_box);

    connect(foreign_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_display_foreign(on); });
    connect(save_filters_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_save_filters(on); });
    connect(hover_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_auto_collapse_on_hover(on); });
    connect(sep_scrollbar_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_color_separator_scrollbar(on); });
    connect(check_update_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_check_update_after_install(on); });

    // Collapsible Separators box (mirrors MO2 settingsdialog.ui collapsibleSeparatorsWidget)
    auto* sep_group = new QGroupBox(tr("Collapsible Separators"), page);
    auto* sep_grid = new QGridLayout(sep_group);

    auto* sort_label = new QLabel(tr("Enable when sorting by"), sep_group);
    auto* asc_box = new QCheckBox(tr("ascending priority"), sep_group);
    asc_box->setChecked(s.collapsible_separators_asc());
    auto* dsc_box = new QCheckBox(tr("descending priority"), sep_group);
    dsc_box->setChecked(s.collapsible_separators_dsc());

    auto* conflicts_label = new QLabel(tr("Show conflicts and plugins"), sep_group);
    auto* highlight_to_box = new QCheckBox(tr("on separators"), sep_group);
    highlight_to_box->setChecked(s.collapsible_separators_highlight_to());
    auto* highlight_from_box = new QCheckBox(tr("from separators"), sep_group);
    highlight_from_box->setChecked(s.collapsible_separators_highlight_from());

    auto* icons_label = new QLabel(tr("Show icons on separators"), sep_group);
    auto* icons_conflicts = new QCheckBox(tr("conflicts"), sep_group);
    icons_conflicts->setChecked(s.collapsible_separators_icons_conflicts());
    auto* icons_flags = new QCheckBox(tr("flags"), sep_group);
    icons_flags->setChecked(s.collapsible_separators_icons_flags());
    auto* icons_content = new QCheckBox(tr("content"), sep_group);
    icons_content->setChecked(s.collapsible_separators_icons_content());
    auto* icons_version = new QCheckBox(tr("version"), sep_group);
    icons_version->setChecked(s.collapsible_separators_icons_version());

    sep_grid->addWidget(sort_label, 0, 0);
    sep_grid->addWidget(asc_box, 0, 1);
    sep_grid->addWidget(dsc_box, 0, 2);
    sep_grid->addWidget(conflicts_label, 1, 0);
    sep_grid->addWidget(highlight_to_box, 1, 1);
    sep_grid->addWidget(highlight_from_box, 1, 2);
    sep_grid->addWidget(icons_label, 2, 0);
    auto* icons_row = new QHBoxLayout;
    icons_row->addWidget(icons_conflicts);
    icons_row->addWidget(icons_flags);
    icons_row->addWidget(icons_content);
    icons_row->addWidget(icons_version);
    icons_row->addStretch(1);
    sep_grid->addLayout(icons_row, 2, 1, 1, 2);
    layout->addWidget(sep_group);

    auto* per_profile_box = new QCheckBox(tr("Collapsible separators per profile"), page);
    per_profile_box->setChecked(s.collapsible_separators_per_profile());
    layout->addWidget(per_profile_box);

    // Per-instance nesting toggle (Settings > Mod List, below the per-profile
    // box). The key is the instance-root folder name, same key namespace as
    // modlist_hidden_columns, so each instance remembers its own value.
    const QString inst_key = instance_root_.empty()
        ? QString()
        : QString::fromStdString(instance_root_.filename().string());
    auto* nested_box = new QCheckBox(tr("Nested mod list ⚠️ (Experimental)"), page);
    nested_box->setChecked(!inst_key.isEmpty() && s.modlist_nested(inst_key));
    if (inst_key.isEmpty()) {
        // No instance loaded (standalone tests): keep the checkbox inert.
        nested_box->setEnabled(false);
    }
    layout->addWidget(nested_box);

    connect(asc_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_asc(on); });
    connect(dsc_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_dsc(on); });
    connect(highlight_to_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_highlight_to(on); });
    connect(highlight_from_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_highlight_from(on); });
    connect(icons_conflicts, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_icons_conflicts(on); });
    connect(icons_flags, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_icons_flags(on); });
    connect(icons_content, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_icons_content(on); });
    connect(icons_version, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_icons_version(on); });
    connect(per_profile_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_per_profile(on); });
    connect(nested_box, &QCheckBox::toggled, this, [&s, inst_key](bool on) {
        if (!inst_key.isEmpty()) s.set_modlist_nested(inst_key, on);
    });

    layout->addStretch(1);
    return page;
}

// -- Paths ---------------------------------------------------------------------

QWidget* SettingsDialog::build_paths_tab() {
    namespace fs = std::filesystem;
    auto& s = Settings::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    // Instances Directory — bare row, no group box (MO2 layout)
    auto* inst_label = new QLabel(tr("Instances Directory"), page);
    auto* dir_edit = new QLineEdit(s.instances_dir(), page);
    dir_edit->setPlaceholderText(QString::fromStdString(engine::default_instances_dir().string()));
    auto* inst_browse = new QPushButton(tr("Browse..."), page);
    auto* inst_row = new QHBoxLayout;
    inst_row->addWidget(inst_label);
    inst_row->addWidget(dir_edit, 1);
    inst_row->addWidget(inst_browse);
    layout->addLayout(inst_row);
    auto* inst_hint = new QLabel(
        tr("Where new instances are created. Leave empty for the default location."), page);
    inst_hint->setWordWrap(true);
    layout->addWidget(inst_hint);

    connect(dir_edit, &QLineEdit::editingFinished, this, [&s, dir_edit]() {
        s.set_instances_dir(dir_edit->text().trimmed());
    });
    connect(inst_browse, &QPushButton::clicked, this, [dir_edit]() {
        const QString dir = QFileDialog::getExistingDirectory(
            dir_edit, QObject::tr("Choose instances directory"), dir_edit->text());
        if (!dir.isEmpty())
            dir_edit->setText(dir);
    });

    auto* hline = new QFrame(page);
    hline->setFrameShape(QFrame::HLine);
    hline->setFrameShadow(QFrame::Sunken);
    layout->addWidget(hline);

    if (!instance_root_.empty()) {
        auto* base_form = new QFormLayout;
        base_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        // Base Directory — editable; committing a different path relocates the instance
        auto* base_edit = new QLineEdit(QString::fromStdString(instance_root_.string()), page);
        auto* base_browse = new QPushButton(tr("Browse..."), page);
        auto* base_row = new QHBoxLayout;
        base_row->addWidget(base_edit, 1);
        base_row->addWidget(base_browse);
        base_form->addRow(tr("Base Directory"), base_row);

        // Per-folder overrides. Empty field = default location under the
        // Base Directory. Overrides persist to instance.toml and are honored
        // by the instance wherever it reads/writes each folder.
        struct FolderRow {
            engine::InstanceKind kind;
            QString label;
            QString placeholder;
        };
        const FolderRow folders[] = {
            {engine::InstanceKind::Mods, tr("Mods"), "$BASE_DIRECTORY/mods"},
            {engine::InstanceKind::Downloads, tr("Downloads"), "$BASE_DIRECTORY/downloads"},
            {engine::InstanceKind::Cache, tr("Cache"), "$BASE_DIRECTORY/cache"},
            {engine::InstanceKind::Profiles, tr("Profiles"), "$BASE_DIRECTORY/profiles"},
            {engine::InstanceKind::Overwrite, tr("Overwrite"), "$BASE_DIRECTORY/overwrite"},
        };

        auto load_overrides = [&]() {
            auto inst = engine::Instance::from_root(instance_root_);
            inst.read_toml();
            return inst;
        };

        for (const auto& f : folders) {
            auto* edit = new QLineEdit(page);
            edit->setPlaceholderText(f.placeholder);
            {
                auto inst = load_overrides();
                auto ov = inst.path_override(f.kind);
                if (!ov.empty())
                    edit->setText(QString::fromStdString(ov.string()));
            }
            auto* browse = new QPushButton(tr("Browse..."), page);
            auto* row = new QHBoxLayout;
            row->addWidget(edit, 1);
            row->addWidget(browse);
            base_form->addRow(f.label, row);

            auto commit = [this, kind = f.kind, edit]() {
                const QString text = edit->text().trimmed();
                auto inst = engine::Instance::from_root(instance_root_);
                inst.read_toml();
                inst.set_path_override(
                    kind, text.isEmpty() ? std::filesystem::path{}
                                         : std::filesystem::path(text.toStdString()));
                inst.write_toml();
            };
            connect(edit, &QLineEdit::editingFinished, this, commit);
            connect(browse, &QPushButton::clicked, this, [edit, commit]() {
                const QString dir = QFileDialog::getExistingDirectory(
                    edit, QObject::tr("Choose folder"), edit->text());
                if (!dir.isEmpty()) {
                    edit->setText(dir);
                    commit();
                }
            });
        }
        auto* folders_hint = new QLabel(
            tr("Each folder defaults to a subdirectory of the Base Directory. "
               "Leave a field empty to keep the default location."), page);
        folders_hint->setWordWrap(true);
        base_form->addRow(QString(), folders_hint);

        auto commit_base = [this, &s, base_edit]() {
            QString new_base = base_edit->text().trimmed();
            if (new_base.isEmpty()) return;
            while (new_base.size() > 1 && new_base.endsWith('/'))
                new_base.chop(1);
            const fs::path target = new_base.toStdString();
            const fs::path current = instance_root_;
            if (target == current) return;

            const auto resp = QMessageBox::question(
                this, tr("Relocate Instance"),
                tr("Change the Base Directory to:\n%1\n\nThe instance folder will be "
                   "moved there. This may take a while for large instances, and a "
                   "restart is recommended afterwards.").arg(new_base),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (resp != QMessageBox::Yes) {
                base_edit->setText(QString::fromStdString(current.string()));
                return;
            }
            std::error_code ec;
            if (fs::exists(target)) {
                QMessageBox::warning(this, tr("Relocate Instance"),
                    tr("The target directory already exists:\n%1").arg(new_base));
                base_edit->setText(QString::fromStdString(current.string()));
                return;
            }
            fs::rename(current, target, ec);
            if (ec) {
                QMessageBox::warning(this, tr("Relocate Instance"),
                    tr("Failed to move the instance: %1")
                        .arg(QString::fromStdString(ec.message())));
                base_edit->setText(QString::fromStdString(current.string()));
                return;
            }
            instance_root_ = target;
            base_edit->setText(QString::fromStdString(instance_root_.string()));
            engine::write_last_instance(instance_root_.filename().string());
        };
        connect(base_edit, &QLineEdit::editingFinished, this, commit_base);
        connect(base_browse, &QPushButton::clicked, this, [base_edit, commit_base]() {
            const QString dir = QFileDialog::getExistingDirectory(
                base_edit, QObject::tr("Choose base directory"), base_edit->text());
            if (!dir.isEmpty()) {
                base_edit->setText(dir);
                commit_base();
            }
        });

        // Managed Game — writes game_dir back into instance.toml
        auto* game_edit = new QLineEdit(page);
        auto* game_browse = new QPushButton(tr("Browse..."), page);
        auto* game_row = new QHBoxLayout;
        game_row->addWidget(game_edit, 1);
        game_row->addWidget(game_browse);
        base_form->addRow(tr("Managed Game"), game_row);
        {
            auto inst = engine::Instance::installed(instance_root_.filename().string(),
                                                    instance_root_.parent_path());
            if (inst.read_toml() && !inst.info().game_dir.empty())
                game_edit->setText(QString::fromStdString(inst.info().game_dir.string()));
        }
        auto commit_game = [this, game_edit]() {
            const QString new_dir = game_edit->text().trimmed();
            if (new_dir.isEmpty()) return;
            auto inst = engine::Instance::installed(instance_root_.filename().string(),
                                                    instance_root_.parent_path());
            if (!inst.read_toml()) return;
            inst.info().game_dir = new_dir.toStdString();
            inst.write_toml();
        };
        connect(game_edit, &QLineEdit::editingFinished, this, commit_game);
        connect(game_browse, &QPushButton::clicked, this, [game_edit, commit_game]() {
            const QString dir = QFileDialog::getExistingDirectory(
                game_edit, QObject::tr("Choose game directory"), game_edit->text());
            if (!dir.isEmpty()) {
                game_edit->setText(dir);
                commit_game();
            }
        });

        layout->addLayout(base_form);
    }

    layout->addStretch(1);
    return page;
}

// -- Sources -------------------------------------------------------------------

QWidget* SettingsDialog::build_sources_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    const auto providers = engine::SourceRegistry::instance().providers();
    if (providers.empty()) {
        layout->addWidget(new QLabel(tr("No download sources are available."), page));
    } else {
        // One sub-tab per source provider (Nexus Mods, Steam Workshop, ...).
        auto* tabs = new QTabWidget(page);
        for (auto* provider : providers) {
            QWidget* content = ui::build_source_settings_page(provider, tabs);
            if (content == nullptr) {
                auto* lbl = new QLabel(
                    tr("This source has no configurable settings."), tabs);
                lbl->setWordWrap(true);
                content = lbl;
            }
            tabs->addTab(content, QString::fromStdString(provider->display_name()));
            const std::string vendor_key =
                engine::vendor_icon_key(provider->source_type());
            if (!vendor_key.empty())
                tabs->setTabIcon(tabs->count() - 1,
                                 engine::IconManager::instance().resolve_icon(
                                     QString::fromStdString(vendor_key)));
        }
        layout->addWidget(tabs, 1);
    }

    layout->addStretch(1);
    return page;
}

// -- Plugin settings tabs (P1.5) -------------------------------------------------
// One native Settings-dialog tab per plugin that declared a typed settings
// tab (register_settings_tab). Each setting renders as the widget matching
// its type and edits persist through the same per-plugin key:value store as
// the plain register_settings rows (plugins/settings/<basename>/<key>).

void SettingsDialog::append_plugin_settings_tabs(QTabWidget* tabs) {
    if (!plugin_loader_) return;

    auto& s = Settings::instance();
    for (const auto& p : plugin_loader_->plugins()) {
        const auto& tab = p.settings_tab;
        if (tab.title.empty() || tab.settings.empty()) continue;

        const QString basename =
            QString::fromStdString(std::filesystem::path(p.path).filename().string());
        auto* page = new QWidget(tabs);
        auto* page_layout = new QVBoxLayout(page);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setLabelAlignment(Qt::AlignLeft);

        auto parse_bool = [](const QString& v) {
            const QString t = v.trimmed().toLower();
            return t == "1" || t == "true" || t == "yes" || t == "on";
        };

        for (const auto& st : tab.settings) {
            const QString key = QString::fromStdString(st.key);
            const QString def = QString::fromStdString(st.default_value);
            const QString value = s.plugin_setting(basename, key, def);

            if (st.type == "bool") {
                auto* box = new QCheckBox(page);
                box->setChecked(parse_bool(value));
                connect(box, &QCheckBox::toggled, this,
                        [&s, basename, key](bool on) {
                            s.set_plugin_setting(basename, key, on ? "1" : "0");
                        });
                form->addRow(key, box);
            } else if (st.type == "int") {
                auto* spin = new QSpinBox(page);
                // Optional "min:max" range; anything unparsable keeps the
                // widget's default 0..INT_MAX (a bad min/max falls back too).
                if (!st.int_range.empty()) {
                    const auto colon = st.int_range.find(':');
                    if (colon != std::string::npos) {
                        const int lo = std::atoi(st.int_range.substr(0, colon).c_str());
                        const int hi = std::atoi(st.int_range.substr(colon + 1).c_str());
                        if (lo < hi) spin->setRange(lo, hi);
                    }
                }
                int int_value = spin->minimum();
                bool parsed = false;
                const int from_value = value.toInt(&parsed);
                if (parsed) {
                    int_value = from_value;
                } else {
                    parsed = false;
                    const int from_def = def.toInt(&parsed);
                    if (parsed) int_value = from_def;
                }
                spin->setValue(int_value);
                connect(spin, &QSpinBox::valueChanged, this,
                        [&s, basename, key](int v) {
                            s.set_plugin_setting(basename, key, QString::number(v));
                        });
                form->addRow(key, spin);
            } else if (st.type == "choice") {
                auto* combo = new QComboBox(page);
                int select = 0;
                for (size_t i = 0; i < st.choices.size(); ++i) {
                    const QString opt = QString::fromStdString(st.choices[i]);
                    combo->addItem(opt);
                    if (opt == value) select = static_cast<int>(i);
                }
                combo->setCurrentIndex(select);
                connect(combo, &QComboBox::currentTextChanged, this,
                        [&s, basename, key](const QString& text) {
                            s.set_plugin_setting(basename, key, text);
                        });
                form->addRow(key, combo);
            } else {
                // "string" and any unknown type: plaintext line edit.
                if (st.type != "string")
                    engine::Logger::instance().warn("Plugin '" + p.game_id +
                        "' settings tab: unknown setting type '" + st.type +
                        "' for key '" + st.key + "' - rendered as text");                auto* edit = new QLineEdit(value, page);
                connect(edit, &QLineEdit::textChanged, this,
                        [&s, basename, key](const QString& text) {
                            s.set_plugin_setting(basename, key, text);
                        });
                form->addRow(key, edit);
            }
        }

        page_layout->addLayout(form);
        page_layout->addStretch(1);
        tabs->addTab(page, QString::fromStdString(tab.title));
    }
}

// -- Plugins -------------------------------------------------------------------

QWidget* SettingsDialog::build_plugins_tab() {
    struct Entry {
        QString name;
        bool is_plugin = false;
        // native plugin fields
        QString author, version, description, game, category, abi_version;
        QString enabled_basename;
        bool enabled = true;
        // provider fields
        QString provider_type;
        engine::SourceProvider* provider = nullptr;
        // plugin-declared options (register_settings): key -> effective value
        std::vector<std::pair<QString, QString>> options;
        // P1.5: non-empty when the plugin declared a typed settings tab; the
        // keys it declares then stop rendering as raw key:value rows.
        QString settings_tab_title;
    };

    // Heap-owned state so the info-pane lambda outlives this function.
    struct InfoState {
        std::vector<Entry> entries;
        QWidget* content = nullptr;  // current info-pane body, rebuilt on selection
    };
    auto state = std::make_shared<InfoState>();

    if (plugin_loader_) {
        for (const auto& p : plugin_loader_->plugins()) {
            Entry e;
            e.name = QString::fromStdString(p.game_display_name);
            e.is_plugin = true;
            e.author = QString::fromStdString(p.author);
            e.version = QString::fromStdString(p.version);
            e.description = QString::fromStdString(p.description);
            e.game = QString::fromStdString(p.game_id);
            e.category = QString::fromStdString(p.category);
            e.abi_version = p.abi_version > 0
                ? QString::number(p.abi_version) : QString();
            e.enabled_basename =
                QString::fromStdString(std::filesystem::path(p.path).filename().string());
            e.enabled = Settings::instance().plugin_enabled(e.enabled_basename);
            // P1.5: keys declared by a typed settings tab render on that tab
            // instead of raw key:value rows here.
            if (!p.settings_tab.title.empty())
                e.settings_tab_title = QString::fromStdString(p.settings_tab.title);
            const auto tab_key = [&p](const std::string& key) {
                for (const auto& st : p.settings_tab.settings)
                    if (st.key == key) return true;
                return false;
            };
            for (const auto& [key, def] : p.settings) {
                if (tab_key(key)) continue;
                const QString k = QString::fromStdString(key);
                e.options.emplace_back(
                    k, Settings::instance().plugin_setting(e.enabled_basename, k,
                                                           QString::fromStdString(def)));
            }
            state->entries.push_back(std::move(e));
        }
    }
    for (auto* provider : engine::SourceRegistry::instance().providers()) {
        Entry e;
        e.name = QString::fromStdString(provider->display_name());
        e.is_plugin = false;
        e.provider_type = QString::fromStdString(provider->source_type());
        e.provider = provider;
        state->entries.push_back(std::move(e));
    }

    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* splitter = new QSplitter(Qt::Horizontal, page);
    layout->addWidget(splitter, 1);  // columns stretch to the window bottom

    // -- Left: category-grouped plugin list + bottom filter bar --------------
    auto* left = new QWidget(splitter);
    left->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    left->setMinimumWidth(kMinDialogWidth / 2);  // half the min window width
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    auto* list = new QTreeWidget(left);
    list->setHeaderHidden(true);
    list->setRootIsDecorated(true);
    auto* filter = new QLineEdit(left);
    filter->setObjectName("pluginFilter");
    filter->setPlaceholderText(tr("Filter..."));
    filter->setClearButtonEnabled(true);
    left_layout->addWidget(list, 1);
    left_layout->addWidget(filter);

    // -- Right: info pane ----------------------------------------------------
    auto* info_pane = new QWidget(splitter);
    info_pane->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* info_layout = new QVBoxLayout(info_pane);

    // Foldable category headers, MO2 plugin types in display order.
    // Indices 0-7 = plugin categories (matching declared strings), 8 = sources,
    // 9 = uncategorized fallback.
    const std::vector<const char*> group_ids = {
        "Game Support", "Installer", "Tool", "Diagnostics",
        "Preview", "File Mapper", "Mod Page", "Settings Page",
        "Sources", "Uncategorized",
    };
    constexpr int kSourcesGroup = 8;
    constexpr int kUncategorizedGroup = 9;

    auto group_for = [&group_ids](const Entry& e) -> int {
        if (!e.is_plugin) return kSourcesGroup;
        for (int i = 0; i < kSourcesGroup; ++i) {
            if (QString::compare(e.category, QString::fromUtf8(group_ids[i]),
                                 Qt::CaseInsensitive) == 0)
                return i;
        }
        return kUncategorizedGroup;
    };

    // Build the group headers in fixed order, then the leaf items under them.
    std::vector<bool> group_used(group_ids.size(), false);
    for (const auto& e : state->entries) group_used[group_for(e)] = true;

    std::vector<QTreeWidgetItem*> group_items(group_ids.size(), nullptr);
    for (size_t g = 0; g < group_ids.size(); ++g) {
        if (!group_used[g]) continue;
        auto* group = new QTreeWidgetItem(list);
        group->setText(0, tr(group_ids[g]));
        QFont gf = group->font(0);
        gf.setBold(true);
        group->setFont(0, gf);
        group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
        group->setToolTip(0, tr("Click the arrow to collapse/expand this category"));
        group_items[g] = group;
    }
    for (size_t i = 0; i < state->entries.size(); ++i) {
        const Entry& e = state->entries[i];
        auto* item = new QTreeWidgetItem(group_items[group_for(e)]);
        item->setText(0, e.name);
        item->setData(0, Qt::UserRole, static_cast<int>(i));
        item->setToolTip(0, e.is_plugin
            ? tr("Plugin: %1").arg(e.name)
            : tr("Source provider: %1").arg(e.name));
    }

    std::function<void(int)> rebuild_info = [this, info_pane, info_layout, state](int index) {
        if (index < 0 || static_cast<size_t>(index) >= state->entries.size()) return;
        const Entry& e = state->entries[index];

        // Delete the whole previous body (removes it from info_layout too).
        delete state->content;
        state->content = new QWidget(info_pane);
        info_layout->addWidget(state->content);
        auto* content_layout = new QVBoxLayout(state->content);

        auto* title = new QLabel(e.name, state->content);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 2);
        title->setFont(title_font);
        content_layout->addWidget(title);
        content_layout->addWidget(new QLabel(
            e.is_plugin ? tr("Plugin") : tr("Source provider (%1)").arg(e.provider_type),
            state->content));

        if (e.is_plugin) {
            auto* form = new QFormLayout;
            form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
            auto add_row = [&](const QString& label, const QString& value) {
                auto* lbl = new QLabel(value.isEmpty() ? tr("(not set)") : value, state->content);
                lbl->setWordWrap(true);
                lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
                form->addRow(label, lbl);
            };
            add_row(tr("Author"), e.author);
            add_row(tr("Version"), e.version);
            add_row(tr("Description"), e.description);
            add_row(tr("Game"), e.game);
            add_row(tr("ABI version"), e.abi_version);
            content_layout->addLayout(form);
        }

        auto* enabled_box = new QCheckBox(tr("Enabled"), state->content);
        if (e.is_plugin) {
            enabled_box->setChecked(e.enabled);
            const QString basename = e.enabled_basename;
            connect(enabled_box, &QCheckBox::toggled, this,
                    [basename](bool on) { Settings::instance().set_plugin_enabled(basename, on); });
            enabled_box->setToolTip(tr("Disabled plugins are not loaded on the next start."));
        } else {
            enabled_box->setChecked(true);
            enabled_box->setEnabled(false);
            enabled_box->setToolTip(tr("Source providers are always enabled."));
        }
        content_layout->addWidget(enabled_box);

        // P1.5: when the plugin moved its settings onto a typed tab, say so —
        // even if a few undeclared keys still render as rows below.
        if (e.is_plugin && !e.settings_tab_title.isEmpty()) {
            auto* tab_hint = new QLabel(
                tr("Settings live on the \"%1\" tab.").arg(e.settings_tab_title),
                state->content);
            tab_hint->setWordWrap(true);
            content_layout->addWidget(tab_hint);
        }
        if (e.is_plugin && e.options.empty() && e.settings_tab_title.isEmpty()) {
            content_layout->addWidget(new QLabel(tr("This plugin exposes no settings."),
                                                 state->content));
        }
        if (e.is_plugin && e.options.empty()) {
            content_layout->addStretch(1);
        } else if (e.is_plugin) {
            // Key | Value table (scrolls internally) as the last item in the
            // info list; it is the only stretch-1 item so it reaches the
            // window bottom. Bool-looking values ("1"/"0"/"true"/"false"/
            // "yes"/"no"/"on"/"off") get a checkbox in the Value column;
            // everything else is a plaintext cell.
            auto* table = new QTableWidget(0, 2, state->content);
            table->setHorizontalHeaderLabels({tr("Key"), tr("Value")});
            table->verticalHeader()->setVisible(false);
            table->horizontalHeader()->setStretchLastSection(true);
            table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
            table->setSelectionBehavior(QAbstractItemView::SelectItems);
            table->setEditTriggers(QAbstractItemView::DoubleClicked |
                                   QAbstractItemView::EditKeyPressed);
            table->setAlternatingRowColors(true);

            auto is_bool = [](const QString& v) {
                const QString t = v.trimmed().toLower();
                return t == "1" || t == "0" || t == "true" || t == "false" ||
                       t == "yes" || t == "no" || t == "on" || t == "off";
            };
            auto to_bool = [](const QString& v) {
                const QString t = v.trimmed().toLower();
                return t == "1" || t == "true" || t == "yes" || t == "on";
            };

            const QString basename = e.enabled_basename;
            for (const auto& [key, value] : e.options) {
                const int row = table->rowCount();
                table->insertRow(row);
                auto* key_item = new QTableWidgetItem(key);
                key_item->setFlags(key_item->flags() & ~Qt::ItemIsEditable);
                table->setItem(row, 0, key_item);

                if (is_bool(value)) {
                    auto* val_item = new QTableWidgetItem;
                    val_item->setCheckState(to_bool(value) ? Qt::Checked : Qt::Unchecked);
                    val_item->setFlags(val_item->flags() & ~Qt::ItemIsEditable);
                    table->setItem(row, 1, val_item);
                } else {
                    // QTableWidgetItem's default flags include
                    // ItemIsUserCheckable; clear it so the persistence
                    // handler can tell checkboxes from plaintext cells.
                    auto* val_item = new QTableWidgetItem(value);
                    val_item->setFlags(val_item->flags() & ~Qt::ItemIsUserCheckable);
                    table->setItem(row, 1, val_item);
                }
            }

            // Connect after populating so programmatic setItem/setCheckState
            // above do not fire the persistence write.
            connect(table, &QTableWidget::itemChanged, this,
                    [basename, table](QTableWidgetItem* item) {
                        if (!item || item->column() != 1) return;
                        auto* key_item = table->item(item->row(), 0);
                        if (!key_item) return;
                        const QString value =
                            (item->flags() & Qt::ItemIsUserCheckable)
                            ? (item->checkState() == Qt::Checked ? "1" : "0")
                            : item->text();
                        Settings::instance().set_plugin_setting(basename,
                                                                key_item->text(), value);
                    });

            content_layout->addWidget(table, 1);
        } else {
            // Source providers: their settings live on the Sources tab only.
            auto* hint = new QLabel(tr("Source provider settings live on the Sources tab."),
                                    state->content);
            hint->setWordWrap(true);
            content_layout->addWidget(hint);
            content_layout->addStretch(1);
        }
    };

    connect(list, &QTreeWidget::currentItemChanged, this,
            [rebuild_info](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (!current) return;
                const QVariant v = current->data(0, Qt::UserRole);
                if (!v.isValid()) return;  // group header
                rebuild_info(v.toInt());
            });
    connect(filter, &QLineEdit::textChanged, this, [list](const QString& text) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto* group = list->topLevelItem(i);
            int visible = 0;
            for (int j = 0; j < group->childCount(); ++j) {
                auto* child = group->child(j);
                const bool match = child->text(0).contains(text, Qt::CaseInsensitive);
                child->setHidden(!match);
                if (match) ++visible;
            }
            group->setHidden(visible == 0);
        }
    });

    splitter->addWidget(left);
    splitter->addWidget(info_pane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 360});

    list->expandAll();
    bool has_entries = false;
    for (int i = 0; i < list->topLevelItemCount() && !has_entries; ++i) {
        auto* group = list->topLevelItem(i);
        if (group->childCount() > 0) {
            list->setCurrentItem(group->child(0));
            has_entries = true;
        }
    }
    if (!has_entries) {
        auto* none = new QLabel(tr("No plugins or providers loaded."), info_pane);
        none->setWordWrap(true);
        info_layout->addWidget(none);
        info_layout->addStretch(1);
    }

    return page;
}

// -- Workarounds ----------------------------------------------------------------

QWidget* SettingsDialog::build_workarounds_tab() {
    auto& s = Settings::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* net_group = new QGroupBox(tr("Network"), page);
    auto* net_form = new QFormLayout(net_group);
    auto* offline_box = new QCheckBox(tr("Offline mode - do not access the internet"), net_group);
    offline_box->setChecked(s.offline_mode());
    auto* proxy_box = new QCheckBox(tr("Use a proxy server"), net_group);
    proxy_box->setChecked(s.use_proxy());
    auto* proxy_host = new QLineEdit(s.proxy_host(), net_group);
    proxy_host->setEnabled(s.use_proxy());
    auto* proxy_port = new QSpinBox(net_group);
    proxy_port->setRange(1, 65535);
    proxy_port->setValue(s.proxy_port());
    proxy_port->setEnabled(s.use_proxy());
    auto* browser_box = new QCheckBox(tr("Use a custom browser for web links"), net_group);
    browser_box->setChecked(s.use_custom_browser());
    auto* browser_cmd = new QLineEdit(s.custom_browser_command(), net_group);
    browser_cmd->setEnabled(s.use_custom_browser());
    browser_cmd->setPlaceholderText(tr("e.g. firefox %1"));

    net_form->addRow(QString(), offline_box);
    net_form->addRow(QString(), proxy_box);
    net_form->addRow(tr("Proxy host"), proxy_host);
    net_form->addRow(tr("Proxy port"), proxy_port);
    net_form->addRow(QString(), browser_box);
    net_form->addRow(tr("Browser command"), browser_cmd);
    layout->addWidget(net_group);

    connect(offline_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_offline_mode(on); });
    connect(proxy_box, &QCheckBox::toggled, this, [&s, proxy_host, proxy_port](bool on) {
        s.set_use_proxy(on);
        proxy_host->setEnabled(on);
        proxy_port->setEnabled(on);
    });
    connect(proxy_host, &QLineEdit::editingFinished, this, [&s, proxy_host]() {
        s.set_proxy_host(proxy_host->text().trimmed());
    });
    connect(proxy_port, &QSpinBox::valueChanged, this, [&s](int v) { s.set_proxy_port(v); });
    connect(browser_box, &QCheckBox::toggled, this, [&s, browser_cmd](bool on) {
        s.set_use_custom_browser(on);
        browser_cmd->setEnabled(on);
    });
    connect(browser_cmd, &QLineEdit::editingFinished, this, [&s, browser_cmd]() {
        s.set_custom_browser_command(browser_cmd->text().trimmed());
    });

    auto* misc_group = new QGroupBox(tr("Miscellaneous"), page);
    auto* misc_form = new QFormLayout(misc_group);
    auto* skip_suffixes = new QLineEdit(s.skip_file_suffixes().join(", "), misc_group);
    auto* skip_dirs = new QLineEdit(s.skip_directories().join(", "), misc_group);
    auto* exec_blacklist = new QLineEdit(s.executables_blacklist(), misc_group);
    auto* delay_spin = new QSpinBox(misc_group);
    delay_spin->setRange(0, 30000);
    delay_spin->setSuffix(tr(" ms"));
    delay_spin->setValue(s.overlay_capture_delay_ms());
    delay_spin->setToolTip(tr("Wait before capturing the Overwrite folder after "
                              "the game exits."));

    auto* core_box = new QCheckBox(tr("Force-enable game core files"), misc_group);
    core_box->setChecked(s.force_enable_core_files());
    auto* archive_box = new QCheckBox(tr("Experimental archive parsing"), misc_group);
    archive_box->setChecked(s.experimental_archive_parsing());

    misc_form->addRow(tr("Skip file suffixes"), skip_suffixes);
    misc_form->addRow(tr("Skip directories"), skip_dirs);
    misc_form->addRow(tr("Executable blacklist"), exec_blacklist);
    misc_form->addRow(tr("Overwrite capture delay"), delay_spin);
    misc_form->addRow(QString(), core_box);
    misc_form->addRow(QString(), archive_box);
    layout->addWidget(misc_group);

    connect(skip_suffixes, &QLineEdit::editingFinished, this, [&s, skip_suffixes]() {
        s.set_skip_file_suffixes(skip_suffixes->text()
                                     .split(',', Qt::SkipEmptyParts));
    });
    connect(skip_dirs, &QLineEdit::editingFinished, this, [&s, skip_dirs]() {
        s.set_skip_directories(skip_dirs->text().split(',', Qt::SkipEmptyParts));
    });
    connect(exec_blacklist, &QLineEdit::editingFinished, this, [&s, exec_blacklist]() {
        s.set_executables_blacklist(exec_blacklist->text().trimmed());
    });
    connect(delay_spin, &QSpinBox::valueChanged, this, [&s](int v) { s.set_overlay_capture_delay_ms(v); });
    connect(core_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_force_enable_core_files(on); });
    connect(archive_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_experimental_archive_parsing(on); });

    layout->addStretch(1);
    return page;
}

// -- FOMOD -------------------------------------------------------------------

QWidget* SettingsDialog::build_fomod_tab() {
    auto& s = Settings::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* group = new QGroupBox(tr("Wizard"), page);
    auto* form = new QFormLayout(group);

    auto* restore_box = new QCheckBox(tr("Restore previous choices"), group);
    restore_box->setChecked(s.always_restore_fomod_choices());
    restore_box->setToolTip(tr("Automatically re-select the options chosen the last "
                               "time this FOMOD was installed."));

    auto* images_box = new QCheckBox(tr("Show FOMOD images"), group);
    images_box->setChecked(s.show_fomod_images());

    auto* hint = new QLabel(tr("Per-module choices are stored in the module's "
                               "meta.ini [fomod] section."), group);
    hint->setWordWrap(true);
    hint->setEnabled(false);

    form->addRow(QString(), restore_box);
    form->addRow(QString(), images_box);
    form->addRow(QString(), hint);
    layout->addWidget(group);

    connect(restore_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_always_restore_fomod_choices(on); });
    connect(images_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_show_fomod_images(on); });

    layout->addStretch(1);
    return page;
}

// -- Diagnostics ----------------------------------------------------------------

QWidget* SettingsDialog::build_diagnostics_tab() {
    auto& s = Settings::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* group = new QGroupBox(tr("Diagnostics"), page);
    auto* form = new QFormLayout(group);

    auto* level_combo = new QComboBox(group);
    level_combo->addItem(tr("Debug"), "debug");
    level_combo->addItem(tr("Info"), "info");
    level_combo->addItem(tr("Warning"), "warn");
    level_combo->addItem(tr("Error"), "error");
    int level_idx = level_combo->findData(s.log_level());
    level_combo->setCurrentIndex(level_idx >= 0 ? level_idx : 1);

    auto* dumps_spin = new QSpinBox(group);
    dumps_spin->setRange(0, 500);
    dumps_spin->setValue(s.max_core_dumps());

    auto* type_combo = new QComboBox(group);
    type_combo->addItem(tr("Text backtrace"), "text");
    type_combo->addItem(tr("Full core dump"), "full");
    int type_idx = type_combo->findData(s.core_dump_type());
    type_combo->setCurrentIndex(type_idx >= 0 ? type_idx : 0);

    auto* level_hint = new QLabel(tr("Applies to new sessions; the GMM_DEBUG "
                                     "environment variable still forces Debug."), group);
    level_hint->setWordWrap(true);
    level_hint->setEnabled(false);

    form->addRow(tr("Log level"), level_combo);
    form->addRow(tr("Maximum crash dumps kept"), dumps_spin);
    form->addRow(tr("Crash dump type"), type_combo);
    form->addRow(QString(), level_hint);
    layout->addWidget(group);

    connect(level_combo, &QComboBox::currentIndexChanged, this, [&s, level_combo](int index) {
        s.set_log_level(level_combo->itemData(index).toString());
    });
    connect(dumps_spin, &QSpinBox::valueChanged, this, [&s](int v) { s.set_max_core_dumps(v); });
    connect(type_combo, &QComboBox::currentIndexChanged, this, [&s, type_combo](int index) {
        s.set_core_dump_type(type_combo->itemData(index).toString());
    });

    layout->addStretch(1);
    return page;
}
