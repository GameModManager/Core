#include "ui/fomod/fomod_wizard_dialog.h"

#include "ui/fomod/fomod_image_viewer.h"
#include "ui/settings/settings.h"
#include "engine/fomod/file_installer.h"
#include "engine/fomod/fomod_utils.h"
#include "engine/log/logger.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QCursor>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace ui {

namespace {

// FOMOD Plus HoverEventFilter: shows the hovered plugin's description/image.
class HoverFilter final : public QObject {
public:
    explicit HoverFilter(std::shared_ptr<engine::PluginViewModel> plugin, QObject* parent = nullptr)
        : QObject(parent)
        , plugin_(std::move(plugin))
    {
    }

    void set_on_hover(const std::function<void(engine::PluginRef)>& cb) { on_hover_ = cb; }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (event->type() == QEvent::HoverEnter) {
            if (on_hover_) {
                on_hover_(plugin_);
            }
            return true;
        }
        return QObject::eventFilter(obj, event);
    }

private:
    std::shared_ptr<engine::PluginViewModel> plugin_;
    std::function<void(engine::PluginRef)> on_hover_;
};

// FOMOD Plus ContextMenuEventFilter (Nexus search dropped; copy actions kept).
class ContextMenuFilter final : public QObject {
public:
    ContextMenuFilter(std::shared_ptr<engine::PluginViewModel> plugin,
        std::shared_ptr<engine::GroupViewModel> group, std::shared_ptr<engine::StepViewModel> step,
        QObject* parent = nullptr)
        : QObject(parent)
        , plugin_(std::move(plugin))
        , group_(std::move(group))
        , step_(std::move(step))
    {
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (event->type() == QEvent::ContextMenu) {
            auto* widget = qobject_cast<QWidget*>(obj);
            if (!widget) {
                return QObject::eventFilter(obj, event);
            }
            QMenu menu(widget);
            const auto pluginName = QString::fromStdString(plugin_->getName());
            const auto description = QString::fromStdString(plugin_->getDescription());
            const auto groupName = QString::fromStdString(group_->getName());
            const auto stepName = QString::fromStdString(step_->getName());
            menu.addAction(tr("Copy Option Name"), [pluginName] { QApplication::clipboard()->setText(pluginName); });
            menu.addAction(tr("Copy Description"), [description] { QApplication::clipboard()->setText(description); });
            menu.addAction(tr("Copy Group Name"), [groupName] { QApplication::clipboard()->setText(groupName); });
            menu.addAction(tr("Copy Step Name"), [stepName] { QApplication::clipboard()->setText(stepName); });
            menu.exec(QCursor::pos());
            return true;
        }
        return QObject::eventFilter(obj, event);
    }

private:
    std::shared_ptr<engine::PluginViewModel> plugin_;
    std::shared_ptr<engine::GroupViewModel> group_;
    std::shared_ptr<engine::StepViewModel> step_;
};

}  // namespace

// Scales a QImage to the label while keeping aspect ratio; a click opens the
// full-size image viewer (FOMOD Plus ScaleLabel port, reduced to static
// images - animated previews are not supported).
class ScaledImageLabel final : public QLabel {
public:
    explicit ScaledImageLabel(QWidget* parent = nullptr)
        : QLabel(parent)
    {
        setAlignment(Qt::AlignCenter);
        setMinimumHeight(80);
    }

    void set_image(const QImage& image)
    {
        image_ = image;
        rescale();
    }

    void clear_image()
    {
        image_ = QImage();
        clear();
    }

    [[nodiscard]] bool has_image() const { return !image_.isNull(); }

    std::function<void()> on_click;

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        rescale();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (on_click) {
            on_click();
        }
        QLabel::mousePressEvent(event);
    }

private:
    void rescale()
    {
        if (image_.isNull()) {
            setPixmap(QPixmap());
            return;
        }
        setPixmap(QPixmap::fromImage(image_).scaled(
            size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    QImage image_;
};

FomodWizardDialog::FomodWizardDialog(const std::shared_ptr<engine::FomodViewModel>& view_model,
    const std::filesystem::path& content_root, QString suggested_name, QString previous_choices,
    bool always_restore_choices, bool show_images, QWidget* parent)
    : QDialog(parent)
    , view_model_(view_model)
    , content_root_(content_root)
    , suggested_name_(std::move(suggested_name))
    , previous_choices_(std::move(previous_choices))
    , always_restore_choices_(always_restore_choices)
    , show_images_(show_images)
{
    setup_ui();

    step_stack_ = new QStackedWidget(this);

    // Legacy FOMODs with no install steps get a plain "install all" page
    // (FOMOD Plus FomodInstallerWindow constructor).
    if (view_model_->getSteps().empty()) {
        auto* legacy_widget = new QWidget(this);
        auto* layout = new QVBoxLayout(legacy_widget);
        layout->addWidget(new QLabel(tr("This mod will install all files automatically."), legacy_widget));
        step_stack_->addWidget(legacy_widget);
    } else {
        update_step_stack();
        style_previously_selected_options();
        style_previously_deselected_options();
    }

    auto* container = new QVBoxLayout(this);
    auto* top_row = create_top_row();
    center_row_ = create_center_row();
    auto* bottom_row = create_bottom_row();
    container->addWidget(top_row);
    container->addWidget(center_row_, 1);
    container->addWidget(bottom_row);
    setLayout(container);

    update_buttons();
    restore_geometry_and_state();

    if (!view_model_->getSteps().empty()) {
        populate_plugin_map();
        if (always_restore_choices_) {
            select_previously_selected_options();
        }
    } else {
        description_box_->setText(tr("This mod will install all files automatically."));
    }
}

void FomodWizardDialog::done(int result)
{
    save_geometry_and_state();
    QDialog::done(result);
}

QString FomodWizardDialog::mod_name() const
{
    return name_input_ ? name_input_->currentText().trimmed() : suggested_name_;
}

std::string FomodWizardDialog::choices_json() const
{
    return engine::generate_fomod_json(*view_model_);
}

void FomodWizardDialog::setup_ui()
{
    setWindowTitle(suggested_name_);
    setWindowFlags(Qt::Window);  // OS-controlled resizing, including snapping
    setMinimumSize(900, 600);
}

void FomodWizardDialog::populate_plugin_map()
{
    const auto checkboxes = findChildren<QCheckBox*>();
    const auto radio_buttons = findChildren<QRadioButton*>();

    for (const auto& step : view_model_->getSteps()) {
        for (const auto& group : step->getGroups()) {
            for (const auto& plugin : group->getPlugins()) {
                const auto name = object_name_for(plugin, group);
                for (auto* checkbox : checkboxes) {
                    if (checkbox->objectName() == name) {
                        plugin_map_.insert(name, PluginData{plugin, checkbox});
                    }
                }
                for (auto* radio : radio_buttons) {
                    if (radio->objectName() == name) {
                        plugin_map_.insert(name, PluginData{plugin, radio});
                    }
                }
            }
        }
    }
}

void FomodWizardDialog::on_next_clicked()
{
    // Legacy FOMODs with no steps always install.
    if (view_model_->getSteps().empty()) {
        on_install_clicked();
        return;
    }

    if (!view_model_->isLastVisibleStep()) {
        view_model_->stepForward();
        step_stack_->setCurrentIndex(view_model_->getCurrentStepIndex());
        update_buttons();
        update_display_for_active_plugin();
    } else {
        on_install_clicked();
    }
}

void FomodWizardDialog::update_checkbox_states()
{
    for (const auto& plugin_data : plugin_map_) {
        if (plugin_data.plugin->isSelected() != plugin_data.ui_element->isChecked()) {
            plugin_data.ui_element->setChecked(plugin_data.plugin->isSelected());
        }
        if (plugin_data.plugin->isEnabled() != plugin_data.ui_element->isEnabled()) {
            plugin_data.ui_element->setEnabled(plugin_data.plugin->isEnabled());
        }
        update_could_be_usable_indicator(plugin_data.ui_element, plugin_data.plugin);
    }
}

void FomodWizardDialog::on_plugin_toggled(bool selected, engine::GroupRef group, engine::PluginRef plugin)
{
    if (view_model_->togglePlugin(group, plugin, selected)) {
        update_checkbox_states();
    }
    if (next_install_button_ != nullptr) {
        update_buttons();
    }
}

void FomodWizardDialog::on_plugin_manually_unchecked(engine::PluginRef plugin)
{
    engine::FomodViewModel::markManuallySet(plugin);
}

void FomodWizardDialog::on_plugin_hovered(engine::PluginRef plugin)
{
    view_model_->setActivePlugin(plugin);
    update_display_for_active_plugin();
}

void FomodWizardDialog::on_back_clicked()
{
    view_model_->stepBack();
    step_stack_->setCurrentIndex(view_model_->getCurrentStepIndex());
    update_buttons();
    update_display_for_active_plugin();
}

void FomodWizardDialog::on_manual_clicked()
{
    manual_ = true;
    reject();  // save_geometry_and_state runs in done()
}

void FomodWizardDialog::on_cancel_clicked()
{
    reject();  // save_geometry_and_state runs in done()
}

void FomodWizardDialog::on_install_clicked()
{
    // Preflight the archive for sources the FOMOD references but the package
    // does not contain. Warn (FOMOD Plus "Files missing from archive" dialog);
    // the user may still force the install, in which case the engine skips the
    // missing files.
    const auto files = engine::collect_files_to_install(*view_model_);
    std::vector<QString> missing;
    for (const auto& file : files) {
        // Windows-native FOMOD sources (backslash separators, any casing) are
        // resolved case-insensitively, matching the engine installer exactly.
        if (engine::resolve_path(content_root_, file.source).empty()) {
            missing.emplace_back(QString::fromStdString(file.source));
        }
    }
    if (!missing.empty()) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Files Missing from Archive"));
        box.setIcon(QMessageBox::Warning);
        box.setText(tr("The FOMOD references files that are missing from this archive. "
                       "Install without them?"));
        QStringList missing_names;
        for (const auto& path : missing) {
            missing_names << path;
        }
        box.setDetailedText(missing_names.join('\n'));
        auto* install_anyway = box.addButton(tr("Install Anyway"), QMessageBox::AcceptRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(install_anyway);
        box.exec();
        if (box.clickedButton() != install_anyway) {
            return;
        }
        ignore_missing_ = true;
    }

    accept();
}

void FomodWizardDialog::update_buttons()
{
    // Legacy FOMODs with no steps always show Install.
    if (view_model_->getSteps().empty()) {
        back_button_->setEnabled(false);
        next_install_button_->setText(tr("Install"));
        return;
    }

    back_button_->setEnabled(!view_model_->isFirstVisibleStep());
    next_install_button_->setText(view_model_->isLastVisibleStep() ? tr("Install") : tr("Next"));
}

void FomodWizardDialog::update_step_stack()
{
    if (!step_stack_) {
        return;
    }
    for (const auto& step : view_model_->getSteps()) {
        step_stack_->addWidget(create_step_widget(step));
    }
    step_stack_->setCurrentIndex(view_model_->getCurrentStepIndex());
}

void FomodWizardDialog::update_display_for_active_plugin()
{
    // Skip if no steps (legacy FOMOD).
    if (view_model_->getSteps().empty()) {
        return;
    }

    auto plugin = view_model_->getActivePlugin();
    if (!plugin) {
        const auto active_step = view_model_->getActiveStep();
        if (!active_step || active_step->getGroups().empty() || active_step->getGroups().front()->getPlugins().empty()) {
            description_box_->setText(tr("Select a plugin to see its description."));
            image_label_->clear_image();
            return;
        }
        // Fall back to the first plugin in the active step.
        plugin = active_step->getGroups().front()->getPlugins().front();
        view_model_->setActivePlugin(plugin);
    }

    description_box_->setText(QString::fromStdString(plugin->getDescription()));

    const auto image = view_model_->getDisplayImage();
    if (image.empty()) {
        image_label_->clear_image();
        return;
    }
    image_label_->set_image(QImage(QString::fromStdString((content_root_ / image).string())));
}

void FomodWizardDialog::show_image_viewer()
{
    if (!image_label_->has_image()) {
        return;
    }
    const auto image = view_model_->getDisplayImage();
    if (image.empty()) {
        return;
    }
    const auto image_path = content_root_ / image;
    auto* viewer = new FomodImageViewer(QImage(QString::fromStdString(image_path.string())), this);
    viewer->showMaximized();
}

QComboBox* FomodWizardDialog::create_name_combo()
{
    name_input_ = new QComboBox(this);
    name_input_->setEditable(true);
    name_input_->addItem(suggested_name_);
    name_input_->completer()->setCaseSensitivity(Qt::CaseSensitive);
    return name_input_;
}

QSplitter* FomodWizardDialog::create_center_row()
{
    left_pane_ = create_left_pane();
    auto* center_row = new QSplitter(Qt::Horizontal, this);
    center_row->addWidget(left_pane_);
    center_row->addWidget(create_right_pane());
    center_row->setSizes({width() / 2, width() / 2});
    return center_row;
}

QWidget* FomodWizardDialog::create_top_row()
{
    auto* top_row = new QWidget(this);

    auto* main_h_layout = new QHBoxLayout(top_row);

    // Left metadata column: label names next to their values.
    auto* metadata_layout = new QHBoxLayout();
    auto* labels_column = new QVBoxLayout();
    auto* values_column = new QVBoxLayout();

    labels_column->addWidget(new QLabel(tr("Name:"), top_row));
    labels_column->addWidget(new QLabel(tr("Author:"), top_row));
    labels_column->addWidget(new QLabel(tr("Version:"), top_row));
    labels_column->addWidget(new QLabel(tr("Website:"), top_row));

    const auto info = view_model_->getInfoViewModel();
    values_column->addWidget(new QLabel("", top_row));  // aligned with the name combo
    values_column->addWidget(new QLabel(QString::fromStdString(info->getAuthor()), top_row));
    values_column->addWidget(new QLabel(QString::fromStdString(info->getVersion()), top_row));

    auto* website = new QLabel("", top_row);
    const auto website_url = QString::fromStdString(info->getWebsite());
    if (!website_url.isEmpty() && QUrl(website_url).isValid()) {
        website->setTextFormat(Qt::RichText);
        website->setOpenExternalLinks(true);
        website->setText(QString("<a href=\"%1\">%2</a>").arg(website_url, tr("Link")));
    } else {
        website->setText(website_url);
    }
    values_column->addWidget(website);

    metadata_layout->addLayout(labels_column);
    metadata_layout->addLayout(values_column, 1);

    main_h_layout->addLayout(metadata_layout, 1);
    main_h_layout->addWidget(create_name_combo(), 4);
    top_row->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    return top_row;
}

QWidget* FomodWizardDialog::create_bottom_row()
{
    auto* bottom_row = new QWidget(this);
    auto* layout = new QHBoxLayout(bottom_row);

    manual_button_ = new QPushButton(tr("Manual"), bottom_row);
    select_previous_button_ = new QPushButton(tr("Restore Previous Choices"), bottom_row);
    reset_choices_button_ = new QPushButton(tr("Reset Choices"), bottom_row);

    layout->addWidget(manual_button_);
    layout->addWidget(select_previous_button_);
    layout->addWidget(reset_choices_button_);

    layout->addStretch();

    back_button_ = new QPushButton(tr("Back"), bottom_row);
    next_install_button_ = new QPushButton(tr("Next"), bottom_row);
    cancel_button_ = new QPushButton(tr("Cancel"), bottom_row);

    next_install_button_->setDefault(true);
    next_install_button_->setAutoDefault(true);

    connect(manual_button_, &QPushButton::clicked, this, &FomodWizardDialog::on_manual_clicked);
    connect(next_install_button_, &QPushButton::clicked, this, &FomodWizardDialog::on_next_clicked);
    connect(back_button_, &QPushButton::clicked, this, &FomodWizardDialog::on_back_clicked);
    connect(cancel_button_, &QPushButton::clicked, this, &FomodWizardDialog::on_cancel_clicked);
    connect(select_previous_button_, &QPushButton::clicked, this,
        &FomodWizardDialog::select_previously_selected_options);
    connect(reset_choices_button_, &QPushButton::clicked, this, &FomodWizardDialog::on_reset_choices_clicked);

    layout->addWidget(back_button_);
    layout->addWidget(next_install_button_);
    layout->addWidget(cancel_button_);

    return bottom_row;
}

QSplitter* FomodWizardDialog::create_left_pane()
{
    auto* left_pane = new QSplitter(Qt::Vertical, this);

    auto* scroll_area = new QScrollArea(left_pane);
    scroll_area->setWidgetResizable(true);

    description_box_ = new QLabel("", left_pane);
    description_box_->setTextFormat(Qt::RichText);
    description_box_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    description_box_->setOpenExternalLinks(true);
    description_box_->setWordWrap(true);
    description_box_->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    scroll_area->setWidget(description_box_);
    left_pane->addWidget(scroll_area);

    image_label_ = new ScaledImageLabel(left_pane);
    image_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    image_label_->on_click = [this] { show_image_viewer(); };
    if (!show_images_) {
        image_label_->hide();
    }
    left_pane->addWidget(image_label_);
    left_pane->setSizes({height() / 2, height() / 2});

    update_display_for_active_plugin();

    return left_pane;
}

QWidget* FomodWizardDialog::create_right_pane()
{
    auto* right_pane = new QWidget(this);
    auto* layout = new QVBoxLayout(right_pane);
    layout->addWidget(step_stack_);
    return right_pane;
}

QWidget* FomodWizardDialog::create_step_widget(const std::shared_ptr<engine::StepViewModel>& step)
{
    auto* step_box = new QGroupBox(QString::fromStdString(step->getName()), this);
    auto* step_box_layout = new QVBoxLayout(step_box);

    auto* scroll_area = new QScrollArea(step_box);
    scroll_area->setWidgetResizable(true);

    auto* content = new QWidget(scroll_area);
    auto* content_layout = new QVBoxLayout(content);
    for (const auto& group : step->getGroups()) {
        content_layout->addWidget(render_group(group, step));
    }

    scroll_area->setWidget(content);
    step_box_layout->addWidget(scroll_area);
    return step_box;
}

QWidget* FomodWizardDialog::render_group(engine::GroupRef group,
    const std::shared_ptr<engine::StepViewModel>& step)
{
    auto* group_box = new QGroupBox(QString::fromStdString(group->getName()), this);
    auto* group_box_layout = new QVBoxLayout(group_box);

    switch (group->getType()) {
    case engine::GroupTypeEnum::SelectAtLeastOne:
    case engine::GroupTypeEnum::SelectAny:
    case engine::GroupTypeEnum::SelectAll:
        render_checkbox_group(group_box, group_box_layout, group, step);
        break;
    case engine::GroupTypeEnum::SelectExactlyOne:
    case engine::GroupTypeEnum::SelectAtMostOne:
        render_select_exactly_one(group_box, group_box_layout, group, step);
        break;
    default:
        break;
    }

    return group_box;
}

QString FomodWizardDialog::object_name_for(engine::PluginRef plugin, engine::GroupRef group)
{
    return QString("[%1:%2] %3-%4")
        .arg(group->getStepIndex())
        .arg(group->getOwnIndex())
        .arg(QString::fromStdString(group->getName()))
        .arg(QString::fromStdString(plugin->getName()));
}

QRadioButton* FomodWizardDialog::create_plugin_radio(engine::PluginRef plugin, engine::GroupRef group,
    const std::shared_ptr<engine::StepViewModel>& step, QWidget* parent)
{
    auto* radio = new QRadioButton(QString::fromStdString(plugin->getName()), parent);
    radio->setObjectName(object_name_for(plugin, group));
    radio->setAttribute(Qt::WA_Hover);
    radio->installEventFilter(new ContextMenuFilter(plugin, group, step, this));

    auto* hover_filter = new HoverFilter(plugin, this);
    hover_filter->set_on_hover([this](engine::PluginRef p) { on_plugin_hovered(p); });
    radio->installEventFilter(hover_filter);

    connect(radio, &QRadioButton::toggled, this,
        [this, group, plugin](bool checked) { on_plugin_toggled(checked, group, plugin); });

    radio->setEnabled(plugin->isEnabled());
    radio->setChecked(plugin->isSelected());
    update_could_be_usable_indicator(radio, plugin);
    return radio;
}

QCheckBox* FomodWizardDialog::create_plugin_checkbox(engine::PluginRef plugin, engine::GroupRef group,
    const std::shared_ptr<engine::StepViewModel>& step, QWidget* parent)
{
    auto* checkbox = new QCheckBox(QString::fromStdString(plugin->getName()), parent);
    checkbox->setObjectName(object_name_for(plugin, group));
    checkbox->setAttribute(Qt::WA_Hover);
    checkbox->installEventFilter(new ContextMenuFilter(plugin, group, step, this));

    auto* hover_filter = new HoverFilter(plugin, this);
    hover_filter->set_on_hover([this](engine::PluginRef p) { on_plugin_hovered(p); });
    checkbox->installEventFilter(hover_filter);

    checkbox->setEnabled(plugin->isEnabled());
    checkbox->setChecked(plugin->isSelected());

    // A user unchecking an automatically-selected option is a manual decision
    // the JSON serializer records (reconciled at serialization time).
    connect(checkbox, &QCheckBox::clicked, this, [this, plugin](bool checked) {
        if (!checked) {
            on_plugin_manually_unchecked(plugin);
        }
    });
    connect(checkbox, &QCheckBox::toggled, this,
        [this, group, plugin](bool checked) { on_plugin_toggled(checked, group, plugin); });

    update_could_be_usable_indicator(checkbox, plugin);
    return checkbox;
}

void FomodWizardDialog::render_select_exactly_one(QWidget* parent, QLayout* parent_layout,
    engine::GroupRef group, const std::shared_ptr<engine::StepViewModel>& step)
{
    // Parity with the legacy installer: a group of one renders as a checkbox.
    if (group->getPlugins().size() == 1) {
        render_checkbox_group(parent, parent_layout, group, step);
    } else {
        render_radio_group(parent, parent_layout, group, step);
    }
}

void FomodWizardDialog::render_checkbox_group(QWidget* parent, QLayout* parent_layout,
    engine::GroupRef group, const std::shared_ptr<engine::StepViewModel>& step)
{
    for (const auto& plugin : group->getPlugins()) {
        parent_layout->addWidget(create_plugin_checkbox(plugin, group, step, parent));
    }
}

QButtonGroup* FomodWizardDialog::render_radio_group(QWidget* parent, QLayout* parent_layout,
    engine::GroupRef group, const std::shared_ptr<engine::StepViewModel>& step)
{
    auto* button_group = new QButtonGroup(parent);
    button_group->setExclusive(true);

    for (const auto& plugin : group->getPlugins()) {
        auto* radio = create_plugin_radio(plugin, group, step, parent);
        button_group->addButton(radio);
        parent_layout->addWidget(radio);
    }
    return button_group;
}

void FomodWizardDialog::update_could_be_usable_indicator(QAbstractButton* button, engine::PluginRef plugin)
{
    static const QString tooltip = tr("This option may not be fully compatible with your current setup.");

    if (plugin->getCurrentPluginType() == engine::PluginTypeEnum::CouldBeUsable) {
        button->setIcon(button->style()->standardIcon(QStyle::SP_MessageBoxWarning));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(tooltip);
    } else {
        if (!button->icon().isNull()) {
            button->setIcon(QIcon());
        }
        if (button->toolTip() == tooltip) {
            button->setToolTip(QString());
        }
    }
}

void FomodWizardDialog::apply_fn_from_json(const std::string& plugin_selector,
    const std::function<void(QAbstractButton*)>& fn)
{
    if (previous_choices_.isEmpty()) {
        return;
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(previous_choices_.toStdString());
    } catch (...) {
        return;
    }
    if (!json.contains("steps") || !json["steps"].is_array()) {
        return;
    }

    std::vector<std::string> plugins;
    const auto& json_steps = json["steps"];
    for (int step_index = 0; step_index < static_cast<int>(json_steps.size()); ++step_index) {
        const auto& step = json_steps[step_index];
        if (!step.contains("groups") || !step["groups"].is_array()) {
            continue;
        }
        for (int group_index = 0; group_index < static_cast<int>(step["groups"].size()); ++group_index) {
            const auto& group = step["groups"][group_index];
            if (!group.contains(plugin_selector)) {
                continue;
            }
            for (const auto& plugin : group[plugin_selector]) {
                plugins.emplace_back("[" + std::to_string(step_index) + ":" + std::to_string(group_index) + "] "
                    + group["name"].get<std::string>() + "-" + plugin.get<std::string>());
            }
        }
    }

    for (auto* checkbox : findChildren<QCheckBox*>()) {
        for (const auto& name : plugins) {
            if (checkbox->objectName().toStdString() == name) {
                fn(checkbox);
            }
        }
    }
    for (auto* radio : findChildren<QRadioButton*>()) {
        for (const auto& name : plugins) {
            if (radio->objectName().toStdString() == name) {
                fn(radio);
            }
        }
    }
}

void FomodWizardDialog::style_previously_selected_options()
{
    // FOMOD Plus Colors::Green, applied per-widget (ColorApplication::BACKGROUND
    // opacity 0.4) so it never touches the global QSS theme.
    const auto stylesheet = QString(
        "QCheckBox { background-color: rgba(152, 151, 26, 0.4); } "
        "QRadioButton { background-color: rgba(152, 151, 26, 0.4); }");
    const auto tooltip = tr("You previously selected this plugin when installing this mod.");
    apply_fn_from_json("plugins", [stylesheet, tooltip](QAbstractButton* button) {
        button->setStyleSheet(stylesheet);
        button->setToolTip(tooltip);
    });
}

void FomodWizardDialog::style_previously_deselected_options()
{
    const auto stylesheet = QString(
        "QCheckBox { border: 1px dashed rgb(152, 151, 26); } "
        "QRadioButton { border: 1px dashed rgb(152, 151, 26); }");
    const auto tooltip = tr("You previously unchecked this plugin when installing this mod.");
    apply_fn_from_json("deselected", [stylesheet, tooltip](QAbstractButton* button) {
        button->setStyleSheet(stylesheet);
        button->setToolTip(tooltip);
    });
}

void FomodWizardDialog::select_previously_selected_options()
{
    if (previous_choices_.isEmpty()) {
        return;
    }
    try {
        view_model_->selectFromJson(nlohmann::json::parse(previous_choices_.toStdString()));
    } catch (const std::exception& e) {
        engine::Logger::instance().warn(
            std::string("FomodWizardDialog: failed to restore previous choices: ") + e.what());
        return;
    }
    update_checkbox_states();
}

void FomodWizardDialog::on_reset_choices_clicked()
{
    try {
        view_model_->resetToDefaults();
    } catch (const std::exception& e) {
        engine::Logger::instance().warn(std::string("FomodWizardDialog: failed to reset choices: ") + e.what());
        return;
    }

    step_stack_->setCurrentIndex(view_model_->getCurrentStepIndex());
    update_checkbox_states();
    update_buttons();
    update_display_for_active_plugin();
}

void FomodWizardDialog::save_geometry_and_state()
{
    auto& s = Settings::instance();
    s.set_fomod_window_geometry(saveGeometry());
    if (center_row_) {
        s.set_fomod_center_split(center_row_->saveState());
    }
    if (left_pane_) {
        s.set_fomod_left_split(left_pane_->saveState());
    }
}

void FomodWizardDialog::restore_geometry_and_state()
{
    auto& s = Settings::instance();
    restoreGeometry(s.fomod_window_geometry());
    center_row_->restoreState(s.fomod_center_split());
    left_pane_->restoreState(s.fomod_left_split());
}

namespace {

engine::FomodDecision ask_fomod_impl(const std::shared_ptr<engine::FomodViewModel>& view_model,
    const std::filesystem::path& content_root, const std::string& suggested_name,
    const std::string& previous_choices, bool always_restore_choices, bool show_images, QWidget* parent)
{
    engine::FomodDecision decision;
    FomodWizardDialog dialog(view_model, content_root, QString::fromStdString(suggested_name),
        QString::fromStdString(previous_choices), always_restore_choices, show_images, parent);
    dialog.exec();
    if (dialog.manual_install()) {
        decision.manual = true;
        decision.mod_name = dialog.mod_name().toStdString();
        return decision;
    }
    if (dialog.result() != QDialog::Accepted) {
        return decision;  // canceled
    }
    decision.accept = true;
    decision.choices_json = dialog.choices_json();
    decision.mod_name = dialog.mod_name().toStdString();
    decision.ignore_missing = dialog.ignore_missing();
    return decision;
}

}  // namespace

engine::FomodDecision ask_fomod(const std::shared_ptr<engine::FomodViewModel>& view_model,
    const std::filesystem::path& content_root, const std::string& suggested_name,
    const std::string& previous_choices, bool always_restore_choices, bool show_images, QWidget* parent)
{
    if (QThread::currentThread() == qApp->thread()) {
        return ask_fomod_impl(view_model, content_root, suggested_name, previous_choices,
            always_restore_choices, show_images, parent);
    }
    // Marshal onto the main thread and block until the modal dialog is done.
    // Same pattern as QtKeychainKeyring's run_on_main / ui::ask_overwrite.
    engine::FomodDecision result;
    QMetaObject::invokeMethod(qApp,
        [&] {
            result = ask_fomod_impl(view_model, content_root, suggested_name, previous_choices,
                always_restore_choices, show_images, parent);
        },
        Qt::BlockingQueuedConnection);
    return result;
}

}  // namespace ui
