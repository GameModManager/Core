#include "ui/modpack/modpack_import_dialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <variant>

#include "engine/collection/nexus/adapter.h"
#include "engine/gmmpack/unpacker.h"
#include "engine/pack/source_detector.h"

namespace ui {

namespace fs = std::filesystem;

namespace {

  // ---------------------------------------------------------------------------
  // Collection::Manifest -> Gmmpack conversion (Nexus URL import path).
  //
  // Field-for-field mapping: pack identity, info, tools, rules, load order,
  // choice groups, and per-mod source entries. Nexus collections carry no
  // patches, executables, INI tweaks, or tree data, so those stay empty and
  // the wizard shows its standard empty-state notes for those steps.
  // ---------------------------------------------------------------------------

  namespace Coll = engine::Collection;
  namespace Gmm  = engine::gmmpack;

  std::string resolution_name(Coll::SourceResolution resolution) {
    switch (resolution) {
    case Coll::SourceResolution::Api:
      return "api";
    case Coll::SourceResolution::Browser:
      return "browser";
    case Coll::SourceResolution::ClientSubscription:
      return "client-subscription";
    }
    return "browser";
  }

  std::string update_policy_name(Coll::UpdatePolicy policy) {
    return policy == Coll::UpdatePolicy::Latest ? "latest" : "exact";
  }

  Gmm::ModCategory convert_category(Coll::ModCategory category) {
    switch (category) {
    case Coll::ModCategory::Required:
      return Gmm::ModCategory::Required;
    case Coll::ModCategory::Recommended:
      return Gmm::ModCategory::Recommended;
    case Coll::ModCategory::Optional:
      return Gmm::ModCategory::Optional;
    }
    return Gmm::ModCategory::Optional;
  }

  std::optional<std::string> non_empty(const std::string &value) {
    if (value.empty())
      return std::nullopt;
    return value;
  }

  // Numeric provider ids stay numeric, slugs stay strings.
  std::variant<int64_t, std::string> convert_mod_id(const std::string &id) {
    bool numeric = !id.empty();
    for (char c : id) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        numeric = false;
        break;
      }
    }
    if (numeric) {
      try {
        return static_cast<int64_t>(std::stoll(id));
      } catch (const std::exception &) {
      }
    }
    return id;
  }

  Gmm::ModSource convert_source(const Coll::ModSource &source) {
    return std::visit(
        [](const auto &src) -> Gmm::ModSource {
          using T = std::decay_t<decltype(src)>;
          if constexpr (std::is_same_v<T, Coll::SourceNexus>) {
            Gmm::ModSourceNexus out;
            out.resolution  = resolution_name(src.resolution);
            out.game_domain = src.game_domain;
            out.mod_id      = src.mod_id;
            if (src.file_id > 0)
              out.file_id = src.file_id;
            out.version   = non_empty(src.version);
            out.file_name = non_empty(src.file_name);
            if (src.file_size > 0)
              out.file_size = src.file_size;
            out.sha256        = non_empty(src.sha256);
            out.update_policy = update_policy_name(src.update_policy);
            return out;
          } else if constexpr (std::is_same_v<T, Coll::SourceDirect>) {
            Gmm::ModSourceDirect out;
            out.resolution    = resolution_name(src.resolution);
            out.url           = src.url;
            out.version       = non_empty(src.version);
            out.file_name     = non_empty(src.file_name);
            out.sha256        = non_empty(src.sha256);
            out.update_policy = update_policy_name(src.update_policy);
            return out;
          } else if constexpr (std::is_same_v<T, Coll::SourceLoversLab>) {
            Gmm::ModSourceLoversLab out;
            out.resolution    = resolution_name(src.resolution);
            out.mod_id        = convert_mod_id(src.mod_id);
            out.section_slug  = src.section_slug;
            out.version       = non_empty(src.version);
            out.file_name     = non_empty(src.file_name);
            out.sha256        = non_empty(src.sha256);
            out.update_policy = update_policy_name(src.update_policy);
            return out;
          } else if constexpr (std::is_same_v<T, Coll::SourceModPub>) {
            Gmm::ModSourceModPub out;
            out.resolution    = resolution_name(src.resolution);
            out.mod_id        = convert_mod_id(src.mod_id);
            out.version       = non_empty(src.version);
            out.file_name     = non_empty(src.file_name);
            out.sha256        = non_empty(src.sha256);
            out.update_policy = update_policy_name(src.update_policy);
            return out;
          } else {
            Gmm::ModSourceSteamWorkshop out;
            out.app_id           = src.app_id;
            out.workshop_item_id = src.workshop_item_id;
            out.version          = non_empty(src.version);
            return out;
          }
        },
        source);
  }

  std::string rule_name(Coll::RuleType type) {
    switch (type) {
    case Coll::RuleType::Before:
      return "before";
    case Coll::RuleType::After:
      return "after";
    case Coll::RuleType::Requires:
      return "requires";
    case Coll::RuleType::Conflicts:
      return "conflicts";
    }
    return "requires";
  }

  Gmm::Gmmpack manifest_to_gmmpack(const Coll::Manifest &manifest) {
    Gmm::Gmmpack pack;
    pack.manifest.gmmpack_schema   = "1.0.0";
    pack.manifest.id               = manifest.id;
    pack.manifest.revision         = static_cast<int>(manifest.revision);
    pack.manifest.info.name        = manifest.info.name;
    pack.manifest.info.author      = manifest.info.author;
    pack.manifest.info.description = manifest.info.description;
    pack.manifest.info.gmm_game_id = manifest.info.game_id;
    pack.manifest.info.homepage    = manifest.info.homepage;
    pack.manifest.info.created_at  = manifest.info.created_at;
    pack.manifest.info.updated_at  = manifest.info.updated_at;
    for (const auto &tool : manifest.tools) {
      Gmm::ManifestTool entry;
      entry.id       = tool.id;
      entry.name     = tool.name;
      entry.homepage = tool.homepage;
      pack.manifest.tools.push_back(std::move(entry));
    }
    for (const auto &rule : manifest.rules) {
      Gmm::ManifestRule entry;
      entry.type = rule_name(rule.type);
      entry.from = rule.from;
      entry.to   = rule.to;
      entry.note = rule.note;
      pack.manifest.rules.push_back(std::move(entry));
    }
    pack.manifest.load_order.plugin_hint = manifest.load_order.plugin_hint;
    for (const auto &group : manifest.choice_groups) {
      Gmm::ChoiceGroup entry;
      entry.id   = group.id;
      entry.name = group.name;
      entry.mode =
          group.mode == Coll::ChoiceMode::ExactlyOne ? "exactly-one" : "at-most-one";
      entry.member_mod_ids = group.member_mod_ids;
      pack.manifest.choice_groups.push_back(std::move(entry));
    }
    for (const auto &mod : manifest.mods) {
      Gmm::ModEntry entry;
      entry.id       = mod.id;
      entry.name     = mod.name;
      entry.phase    = mod.phase;
      entry.category = convert_category(mod.category);
      entry.source   = convert_source(mod.source);
      if (!mod.installer_choices.type.empty()) {
        Gmm::InstallerChoices choices;
        choices.type            = mod.installer_choices.type;
        choices.selections      = mod.installer_choices.selections;
        entry.installer_choices = std::move(choices);
      }
      pack.mods.push_back(std::move(entry));
    }
    return pack;
  }

}  // namespace

ModpackImportDialog::ModpackImportDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Import Modpack"));
  setMinimumSize(520, 360);
  setSizeGripEnabled(true);
  setAcceptDrops(true);

  auto *layout = new QVBoxLayout(this);

  auto *file_card   = new QGroupBox(tr("From file"), this);
  auto *file_layout = new QVBoxLayout(file_card);
  pick_button_      = new QPushButton(tr("Pick a .gmmpack archive..."), file_card);
  file_layout->addWidget(pick_button_);
  picked_file_label_ = new QLabel(tr("No file selected"), file_card);
  picked_file_label_->setWordWrap(true);
  file_layout->addWidget(picked_file_label_);
  auto *drop_hint =
      new QLabel(tr("Tip: you can also drag a .gmmpack file onto this dialog - "
                    "or straight onto the main window."),
                 file_card);
  drop_hint->setWordWrap(true);
  drop_hint->setEnabled(false);
  file_layout->addWidget(drop_hint);
  connect(pick_button_, &QPushButton::clicked, this,
          &ModpackImportDialog::on_pick_file);
  layout->addWidget(file_card);

  auto *or_label = new QLabel(tr("- or -"), this);
  or_label->setAlignment(Qt::AlignCenter);
  layout->addWidget(or_label);

  auto *url_card   = new QGroupBox(tr("From collection URL"), this);
  auto *url_layout = new QVBoxLayout(url_card);
  url_edit_        = new QLineEdit(url_card);
  url_edit_->setPlaceholderText(tr("Paste a collection URL..."));
  url_edit_->setClearButtonEnabled(true);
  url_layout->addWidget(url_edit_);
  connect(url_edit_, &QLineEdit::textChanged, this,
          &ModpackImportDialog::on_url_edited);
  layout->addWidget(url_card);

  auto *box      = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  import_button_ = box->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
  import_button_->setDefault(true);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(import_button_, &QPushButton::clicked, this, &ModpackImportDialog::on_import);
  layout->addWidget(box);
}

void ModpackImportDialog::on_pick_file() {
  const QString path =
      QFileDialog::getOpenFileName(this, tr("Import Modpack"), QString(),
                                   tr("Modpacks (*.gmmpack *.zip);;All files (*)"));
  if (path.isEmpty())
    return;
  set_picked_file(path);
}

void ModpackImportDialog::set_picked_file(const QString &path) {
  if (path.isEmpty())
    return;
  picked_file_ = path;
  picked_file_label_->setText(path);
  if (!url_edit_->text().trimmed().isEmpty())
    url_edit_->clear();
}

void ModpackImportDialog::on_url_edited(const QString &text) {
  if (!text.trimmed().isEmpty() && !picked_file_.isEmpty()) {
    picked_file_.clear();
    picked_file_label_->setText(tr("No file selected"));
  }
}

fs::path ModpackImportDialog::resolve_schema_dir() {
  const fs::path app_dir = QCoreApplication::applicationDirPath().toStdString();
  // Installed layout first, then dev runs (walk up to Workspace/input/).
  std::vector<fs::path> candidates = {
      app_dir / "schemas",
      app_dir / ".." / "share" / "gamemodmanager" / "schemas",
  };
  std::error_code ec;
  for (fs::path dir = app_dir; !dir.empty(); dir = dir.parent_path()) {
    candidates.push_back(dir / "input");
    if (dir == dir.root_path())
      break;
  }
  for (const auto &dir : candidates) {
    ec.clear();
    if (fs::exists(dir / "manifest.schema.json", ec))
      return dir;
  }
  return {};
}

void ModpackImportDialog::on_import() {
  const QString url_text = url_edit_->text().trimmed();
  if (url_text.isEmpty() && picked_file_.isEmpty()) {
    QMessageBox::information(this, tr("Import Modpack"),
                             tr("Pick a .gmmpack file or paste a "
                                "collection URL to continue."));
    return;
  }

  if (!url_text.isEmpty()) {
    const QUrl url(url_text);
    if (!url.isValid() ||
        (url.scheme() != "http" && url.scheme() != "https" && url.scheme() != "nxm")) {
      QMessageBox::warning(this, tr("Import Modpack"),
                           tr("That doesn't look like a collection URL.\n"
                              "Expected an https:// or nxm:// link, got: %1")
                               .arg(url_text));
      return;
    }
  } else {
    const QFileInfo info(picked_file_);
    if (!info.exists()) {
      QMessageBox::warning(this, tr("Import Modpack"),
                           tr("The selected file no longer exists:\n%1\n"
                              "Pick the file again.")
                               .arg(picked_file_));
      return;
    }
    if (!info.isFile() || !info.isReadable()) {
      QMessageBox::warning(this, tr("Import Modpack"),
                           tr("Cannot read the selected file:\n%1").arg(picked_file_));
      return;
    }
  }

  const std::string ref =
      url_text.isEmpty() ? picked_file_.toStdString() : url_text.toStdString();

  const engine::Pack::Detection detection = engine::Pack::detect_pack_source(ref);
  if (!detection.known()) {
    QMessageBox::warning(this, tr("Import Modpack"),
                         tr("Unrecognized modpack source: %1")
                             .arg(QString::fromStdString(detection.reason)));
    return;
  }

  if (detection.format == engine::Pack::PackFormat::NexusCollection) {
    engine::Collection::Nexus::Adapter adapter;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const engine::Collection::FetchOutcome outcome = adapter.fetch(ref);
    QApplication::restoreOverrideCursor();
    if (std::holds_alternative<engine::Collection::FetchError>(outcome)) {
      const auto &error = std::get<engine::Collection::FetchError>(outcome);
      QMessageBox::warning(this, tr("Import Modpack"),
                           tr("Could not fetch the collection:\n%1")
                               .arg(QString::fromStdString(error.message)));
      return;
    }
    pack_ = manifest_to_gmmpack(
        std::get<engine::Collection::FetchResult>(outcome).manifest);
    accept();
    return;
  }

  // .gmmpack path: unpack + validate.
  const fs::path schema_dir = resolve_schema_dir();
  if (schema_dir.empty()) {
    QMessageBox::warning(this, tr("Import Modpack"),
                         tr("Could not locate the gmmpack schemas "
                            "(manifest.schema.json)."));
    return;
  }
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const engine::gmmpack::UnpackResult result =
      engine::gmmpack::unpack_gmmpack(fs::path(ref), schema_dir);
  QApplication::restoreOverrideCursor();
  if (!result.ok) {
    QString errors;
    for (const auto &d : result.diagnostics) {
      if (d.severity == engine::gmmpack::Diagnostic::Severity::Error) {
        errors += QString::fromStdString(d.path + ": " + d.message + "\n");
      }
    }
    if (errors.isEmpty())
      errors = tr("Unknown unpack error.");
    QMessageBox::warning(this, tr("Import Modpack"),
                         tr("Invalid modpack:\n%1").arg(errors));
    return;
  }
  pack_ = result.pack;
  accept();
}

void ModpackImportDialog::dragEnterEvent(QDragEnterEvent *event) {
  if (!event->mimeData()->hasUrls())
    return;
  for (const QUrl &url : event->mimeData()->urls()) {
    const QString suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
    if (suffix == "gmmpack" || suffix == "zip") {
      event->acceptProposedAction();
      return;
    }
  }
}

void ModpackImportDialog::dropEvent(QDropEvent *event) {
  for (const QUrl &url : event->mimeData()->urls()) {
    const QString local  = url.toLocalFile();
    const QString suffix = QFileInfo(local).suffix().toLower();
    if (!local.isEmpty() && (suffix == "gmmpack" || suffix == "zip")) {
      if (!QFileInfo::exists(local)) {
        QMessageBox::warning(this, tr("Import Modpack"),
                             tr("The dropped file no longer exists:\n%1").arg(local));
        return;
      }
      set_picked_file(local);
      event->acceptProposedAction();
      return;
    }
  }
}

}  // namespace ui
