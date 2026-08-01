#include "ui/settings/settings_dialog.h"
#include "ui/settings/settings.h"
#include "ui/settings/source_pages.h"
#include "engine/source/source_provider.h"
#include "engine/theme/style_manager.h"
#include "engine/log/logger.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <vector>

SettingsDialog::SettingsDialog(engine::StyleManager* style_manager,
                               const QString& native_style_name,
                               const std::filesystem::path& instance_root,
                               QWidget* parent)
    : QDialog(parent), style_manager_(style_manager),
      native_style_name_(native_style_name), instance_root_(instance_root) {
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

    auto* group = new QGroupBox(tr("Theme"), page);
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

    layout->addStretch(1);
    return page;
}

// -- Mod List ------------------------------------------------------------------

QWidget* SettingsDialog::build_modlist_tab() {
    auto& s = Settings::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* group = new QGroupBox(tr("Mod List"), page);
    auto* gl = new QVBoxLayout(group);

    auto* foreign_box = new QCheckBox(tr("Display foreign mods (DLC, Creation Club)"), group);
    foreign_box->setChecked(s.display_foreign());
    auto* save_filters_box = new QCheckBox(tr("Remember filter settings"), group);
    save_filters_box->setChecked(s.save_filters());
    auto* hover_box = new QCheckBox(tr("Auto-collapse separators on hover"), group);
    hover_box->setChecked(s.auto_collapse_on_hover());
    auto* per_profile_box = new QCheckBox(tr("Collapsible separators per profile"), group);
    per_profile_box->setChecked(s.collapsible_separators_per_profile());
    auto* highlight_from_box = new QCheckBox(tr("Highlight separator when collapsed from above"), group);
    highlight_from_box->setChecked(s.collapsible_separators_highlight_from());
    auto* highlight_to_box = new QCheckBox(tr("Highlight separator when collapsed from below"), group);
    highlight_to_box->setChecked(s.collapsible_separators_highlight_to());
    auto* sep_scrollbar_box = new QCheckBox(tr("Color the scrollbar at separators"), group);
    sep_scrollbar_box->setChecked(s.color_separator_scrollbar());
    auto* check_update_box = new QCheckBox(tr("Check for updates after install"), group);
    check_update_box->setChecked(s.check_update_after_install());

    gl->addWidget(foreign_box);
    gl->addWidget(save_filters_box);
    gl->addWidget(hover_box);
    gl->addWidget(per_profile_box);
    gl->addWidget(highlight_from_box);
    gl->addWidget(highlight_to_box);
    gl->addWidget(sep_scrollbar_box);
    gl->addWidget(check_update_box);
    layout->addWidget(group);

    // Conflict colors ------------------------------------------------------------
    auto* color_group = new QGroupBox(tr("Conflict Colors"), page);
    auto* color_form = new QFormLayout(color_group);
    auto* overwritten_edit = new QLineEdit(s.overwritten_files_color(), color_group);
    auto* contained_edit = new QLineEdit(s.contained_files_color(), color_group);
    color_form->addRow(tr("Overwritten files"), overwritten_edit);
    color_form->addRow(tr("Contained files"), contained_edit);
    layout->addWidget(color_group);

    connect(foreign_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_display_foreign(on); });
    connect(save_filters_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_save_filters(on); });
    connect(hover_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_auto_collapse_on_hover(on); });
    connect(per_profile_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_per_profile(on); });
    connect(highlight_from_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_highlight_from(on); });
    connect(highlight_to_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_collapsible_separators_highlight_to(on); });
    connect(sep_scrollbar_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_color_separator_scrollbar(on); });
    connect(check_update_box, &QCheckBox::toggled, this, [&s](bool on) { s.set_check_update_after_install(on); });
    connect(overwritten_edit, &QLineEdit::editingFinished, this, [&s, overwritten_edit]() {
        s.set_overwritten_files_color(overwritten_edit->text());
    });
    connect(contained_edit, &QLineEdit::editingFinished, this, [&s, contained_edit]() {
        s.set_contained_files_color(contained_edit->text());
    });

    layout->addStretch(1);
    return page;
}

// -- Paths ---------------------------------------------------------------------

QWidget* SettingsDialog::build_paths_tab() {
    auto& s = Settings::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* inst_group = new QGroupBox(tr("Instances Directory"), page);
    auto* inst_layout = new QVBoxLayout(inst_group);
    auto* dir_edit = new QLineEdit(s.instances_dir(), inst_group);
    dir_edit->setPlaceholderText(tr("Default (XDG data dir)"));
    auto* dir_row = new QHBoxLayout;
    dir_row->addWidget(dir_edit, 1);
    auto* browse_btn = new QPushButton(tr("Browse..."), inst_group);
    dir_row->addWidget(browse_btn);
    inst_layout->addLayout(dir_row);
    auto* inst_hint = new QLabel(
        tr("Where new instances are created. Leave empty for the default location."), inst_group);
    inst_hint->setWordWrap(true);
    inst_layout->addWidget(inst_hint);
    layout->addWidget(inst_group);

    connect(dir_edit, &QLineEdit::editingFinished, this, [&s, dir_edit]() {
        s.set_instances_dir(dir_edit->text().trimmed());
    });
    connect(browse_btn, &QPushButton::clicked, this, [dir_edit]() {
        const QString dir = QFileDialog::getExistingDirectory(
            dir_edit, QObject::tr("Choose instances directory"), dir_edit->text());
        if (!dir.isEmpty())
            dir_edit->setText(dir);
    });

    if (!instance_root_.empty()) {
        auto* cur_group = new QGroupBox(tr("Current Instance"), page);
        auto* cur_form = new QFormLayout(cur_group);
        auto add_dir = [&](const QString& label, const std::filesystem::path& p) {
            auto* lbl = new QLabel(QString::fromStdString(p.string()), cur_group);
            lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            lbl->setWordWrap(true);
            cur_form->addRow(label, lbl);
        };
        add_dir(tr("Base"), instance_root_);
        add_dir(tr("Mods"), instance_root_ / "mods");
        add_dir(tr("Downloads"), instance_root_ / "downloads");
        add_dir(tr("Cache"), instance_root_ / "cache");
        add_dir(tr("Profiles"), instance_root_ / "profiles");
        add_dir(tr("Overwrite"), instance_root_ / "overwrite");
        layout->addWidget(cur_group);
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
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* info = new QLabel(
        tr("Loaded modules and providers. Each provider that exposes settings "
           "does so on the Sources tab."), page);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto* group = new QGroupBox(tr("Loaded Plugins"), page);
    auto* gl = new QVBoxLayout(group);

    const auto providers = engine::SourceRegistry::instance().providers();
    if (providers.empty()) {
        gl->addWidget(new QLabel(tr("None loaded."), group));
    }
    for (auto* provider : providers) {
        auto* row = new QHBoxLayout;
        auto* name = new QLabel(QString::fromStdString(provider->display_name()), group);
        name->setWordWrap(true);
        auto* type = new QLabel(QString::fromStdString(provider->source_type()), group);
        type->setEnabled(false);
        row->addWidget(name, 1);
        row->addWidget(type);
        gl->addLayout(row);
    }
    layout->addWidget(group);

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
