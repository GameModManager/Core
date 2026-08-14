#pragma once

#include "engine/mod/overwrite/overwrite_utils.h"

#include <QDialog>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

class QComboBox;
class QTreeWidget;
class QTreeWidgetItem;

namespace ui {

// MO2 SyncOverwriteDialog port. Shows every file in the Overwrite folder as a
// tree; each file row has a combo letting the user pick the destination mod
// (winner first, then alternatives) or "<don't sync>". Files the game itself
// provides additionally get a game-origin destination (GMM extension) which
// lands in a mod folder named after the game.
class SyncOverwriteDialog : public QDialog {
    Q_OBJECT
public:
    struct Context {
        std::filesystem::path overwrite_dir;
        std::filesystem::path mods_dir;
        // (folder_name, priority) for ENABLED managed mods only - Overwrite /
        // MERGED / game-native are excluded by the caller.
        std::vector<std::pair<std::string, int>> mod_infos;
        std::string mods_subpath;
        bool conflict_reversed = false;
        bool include_mod_id = false;
        std::filesystem::path game_dir;
        std::string game_folder;  // mod folder used for game-origin files
        std::string game_label;   // display label for game-origin files
        std::string metadata_file;  // e.g. "meta.ini"
    };

    explicit SyncOverwriteDialog(const Context& ctx, QWidget* parent = nullptr);

    // Destinations chosen for the files that should move. Files left at
    // "<don't sync>" are absent.
    std::vector<engine::OverwriteSyncTarget> targets() const;

protected:
    void accept() override;

private:
    struct FileLeaf {
        std::string name;
        size_t index;  // into files_
    };
    struct DirNode {
        std::map<std::string, DirNode> children;
        std::vector<FileLeaf> files;
    };
    struct Row {
        QComboBox* combo;
        size_t index;
    };

    void build_tree();
    void add_items(QTreeWidgetItem* parent_item, const DirNode& node);
    void add_file_row(QTreeWidgetItem* item, size_t file_index);

    Context ctx_;
    std::vector<engine::OverwriteSyncFile> files_;
    std::vector<Row> rows_;
    std::vector<engine::OverwriteSyncTarget> targets_;
    DirNode root_;

    QTreeWidget* tree_ = nullptr;
};

}  // namespace ui
