#include "ui/settings/settings_dialog.h"
#include "ui/settings/settings.h"
#include "ui/settings/source_pages.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/source/source_provider.h"
#include "engine/theme/style_manager.h"
#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/log/logger.h"

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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <functional>
#include <vector>

SettingsDialog::SettingsDialog(engine::StyleManager* style_manager,
                               const QString& native_style_name,
                               const std::filesystem::path& instance_root,
                               engine::PluginLoader* plugin_loader,
                               QWidget* parent)
    : QDialog(parent), style_manager_(style_manager),
      native_style_name_(native_style_name), instance_root_(instance_root),
      plugin_loader_(plugin_loader) {
    setWindowTitle(tr("Settings"));
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);

    tabs->addTab(build_general_tab(), tr("General"));
    tabs->addTab(build_theme_tab(), tr("Theme"));
    tabs->addTab(build_modlist_tab(), tr("Mod List"));
    tabs->addTab(build_paths_tab(), tr("Paths"));
    tabs->addTab(build_sources_tab(), tr("Sources"));
    tabs->addTab(build_plugins_tab(), tr("Plugins"));
    tabs->addTab(build_workarounds_tab(), tr("Workarounds"));
    tabs->addTab(build_diagnostics_tab(), tr("Diagnostics"));

    layout->addWidget(tabs);

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

    auto* theme_hint = new QLabel(
        tr("Editing a theme's .qss or tokens.json on disk live-reloads it. "
           "Qt styles (Fusion, Windows, ...) are the built-in Qt look - no custom theme files."), page);
    theme_hint->setWordWrap(true);

    auto* group = new QGroupBox(tr("Style"), page);
    auto* gl = new QVBoxLayout(group);
    gl->addWidget(theme_combo);
    gl->addWidget(theme_hint);
    layout->addWidget(group);

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

    connect(asc_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_asc(on); });
    connect(dsc_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_dsc(on); });
    connect(highlight_to_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_highlight_to(on); });
    connect(highlight_from_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_highlight_from(on); });
    connect(icons_conflicts, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_icons_conflicts(on); });
    connect(icons_flags, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_icons_flags(on); });
    connect(icons_content, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_icons_content(on); });
    connect(icons_version, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_icons_version(on); });
    connect(per_profile_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_per_profile(on); });

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

        auto make_dir_row = [&](const QString& label, const fs::path& p) {
            auto* lbl = new QLabel(QString::fromStdString(p.string()), page);
            lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            lbl->setWordWrap(true);
            base_form->addRow(label, lbl);
            return lbl;
        };
        auto* mods_label = make_dir_row(tr("Mods"), instance_root_ / "mods");
        auto* downloads_label = make_dir_row(tr("Downloads"), instance_root_ / "downloads");
        auto* cache_label = make_dir_row(tr("Cache"), instance_root_ / "cache");
        auto* profiles_label = make_dir_row(tr("Profiles"), instance_root_ / "profiles");
        auto* overwrite_label = make_dir_row(tr("Overwrite"), instance_root_ / "overwrite");

        auto refresh_derived = [this, mods_label, downloads_label, cache_label, profiles_label, overwrite_label]() {
            mods_label->setText(QString::fromStdString((instance_root_ / "mods").string()));
            downloads_label->setText(QString::fromStdString((instance_root_ / "downloads").string()));
            cache_label->setText(QString::fromStdString((instance_root_ / "cache").string()));
            profiles_label->setText(QString::fromStdString((instance_root_ / "profiles").string()));
            overwrite_label->setText(QString::fromStdString((instance_root_ / "overwrite").string()));
        };

        auto commit_base = [this, &s, base_edit, refresh_derived]() {
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
            refresh_derived();
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
    }
    for (auto* provider : providers) {
        auto* group = new QGroupBox(
            QString::fromStdString(provider->display_name()), page);
        auto* gl = new QVBoxLayout(group);
        if (QWidget* settings_page =
                ui::build_source_settings_page(provider, group)) {
            gl->addWidget(settings_page);
        } else {
            auto* lbl = new QLabel(
                tr("This source has no configurable settings."), group);
            lbl->setWordWrap(true);
            gl->addWidget(lbl);
        }
        layout->addWidget(group);
    }

    layout->addStretch(1);
    return page;
}

// -- Plugins -------------------------------------------------------------------

QWidget* SettingsDialog::build_plugins_tab() {
    struct Entry {
        QString name;
        bool is_plugin = false;
        // native plugin fields
        QString author, version, description, game, steam_appid, abi_version, path;
        bool enabled = true;
        // provider fields
        QString provider_type;
        engine::SourceProvider* provider = nullptr;
    };

    std::vector<Entry> entries;

    if (plugin_loader_) {
        for (const auto& p : plugin_loader_->plugins()) {
            Entry e;
            e.name = QString::fromStdString(p.game_display_name);
            e.is_plugin = true;
            e.author = QString::fromStdString(p.author);
            e.version = QString::fromStdString(p.version);
            e.description = QString::fromStdString(p.description);
            e.game = QString::fromStdString(p.game_id);
            e.steam_appid = p.steam_appid > 0
                ? QString::number(p.steam_appid) : QString();
            e.abi_version = p.abi_version > 0
                ? QString::number(p.abi_version) : QString();
            e.path = QString::fromStdString(p.path);
            e.enabled = Settings::instance().plugin_enabled(
                QString::fromStdString(std::filesystem::path(p.path).filename().string()));
            entries.push_back(std::move(e));
        }
    }
    for (auto* provider : engine::SourceRegistry::instance().providers()) {
        Entry e;
        e.name = QString::fromStdString(provider->display_name());
        e.is_plugin = false;
        e.provider_type = QString::fromStdString(provider->source_type());
        e.provider = provider;
        entries.push_back(std::move(e));
    }

    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* splitter = new QSplitter(Qt::Horizontal, page);
    layout->addWidget(splitter);

    // -- Left: plugin list + bottom filter bar -----------------------------
    auto* left = new QWidget(splitter);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    auto* list = new QListWidget(left);
    auto* filter = new QLineEdit(left);
    filter->setPlaceholderText(tr("Filter..."));
    filter->setClearButtonEnabled(true);
    left_layout->addWidget(list, 1);
    left_layout->addWidget(filter);

    // -- Right: info pane ---------------------------------------------------
    auto* info_pane = new QWidget(splitter);
    auto* info_layout = new QVBoxLayout(info_pane);

    for (const auto& e : entries) {
        auto* item = new QListWidgetItem(e.name, list);
        item->setData(Qt::UserRole, static_cast<int>(&e - &entries[0]));
        item->setToolTip(e.is_plugin
            ? tr("Plugin: %1").arg(e.name)
            : tr("Source provider: %1").arg(e.name));
    }

    auto rebuild_info = [this, info_pane, info_layout, &entries](int index) {
        if (index < 0 || static_cast<size_t>(index) >= entries.size()) return;
        const Entry& e = entries[index];

        // Clear previous content
        while (auto* item = info_layout->takeAt(0)) {
            if (item->widget()) delete item->widget();
            delete item;
        }

        auto* title = new QLabel(e.name, info_pane);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 2);
        title->setFont(title_font);
        info_layout->addWidget(title);
        info_layout->addWidget(new QLabel(
            e.is_plugin ? tr("Plugin") : tr("Source provider (%1)").arg(e.provider_type),
            info_pane));

        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        if (e.is_plugin) {
            auto add_row = [&](const QString& label, const QString& value) {
                auto* lbl = new QLabel(value.isEmpty() ? tr("(not set)") : value, info_pane);
                lbl->setWordWrap(true);
                lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
                form->addRow(label, lbl);
            };
            add_row(tr("Author"), e.author);
            add_row(tr("Version"), e.version);
            add_row(tr("Description"), e.description);
            add_row(tr("Game"), e.game);
            add_row(tr("Steam App ID"), e.steam_appid);
            add_row(tr("ABI version"), e.abi_version);
            add_row(tr("Path"), e.path);
        }
        info_layout->addLayout(form);

        auto* enabled_box = new QCheckBox(
            e.is_plugin ? tr("Enabled") : tr("Enabled"), info_pane);
        if (e.is_plugin) {
            enabled_box->setChecked(e.enabled);
            const QString basename =
                QString::fromStdString(std::filesystem::path(e.path.toStdString()).filename().string());
            connect(enabled_box, &QCheckBox::toggled, this,
                    [basename](bool on) { Settings::instance().set_plugin_enabled(basename, on); });
            enabled_box->setToolTip(tr("Disabled plugins are not loaded on the next start."));
        } else {
            enabled_box->setChecked(true);
            enabled_box->setEnabled(false);
            enabled_box->setToolTip(tr("Source providers are always enabled."));
        }
        info_layout->addWidget(enabled_box);

        auto* settings_group = new QGroupBox(tr("Settings"), info_pane);
        auto* gl = new QVBoxLayout(settings_group);
        if (e.is_plugin) {
            gl->addWidget(new QLabel(tr("This plugin exposes no settings."), settings_group));
        } else if (QWidget* settings_page =
                       ui::build_source_settings_page(e.provider, settings_group)) {
            gl->addWidget(settings_page);
        } else {
            gl->addWidget(new QLabel(tr("This provider has no configurable settings."), settings_group));
        }
        info_layout->addWidget(settings_group);
        info_layout->addStretch(1);
    };

    connect(list, &QListWidget::currentRowChanged, this, rebuild_info);
    connect(filter, &QLineEdit::textChanged, this, [list](const QString& text) {
        for (int i = 0; i < list->count(); ++i) {
            auto* item = list->item(i);
            item->setHidden(!item->text().contains(text, Qt::CaseInsensitive));
        }
    });

    splitter->addWidget(left);
    splitter->addWidget(info_pane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 360});

    if (!entries.empty()) {
        list->setCurrentRow(0);
    } else {
        while (auto* item = info_layout->takeAt(0)) {
            if (item->widget()) delete item->widget();
            delete item;
        }
        auto* none = new QLabel(tr("No plugins or providers loaded."), info_pane);
        none->setWordWrap(true);
        info_layout->addWidget(none);
        info_layout->addStretch(1);
    }

    layout->addStretch(1);
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
