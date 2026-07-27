#include "ui/preview/anm2_parser.h"

#include <algorithm>
#include <filesystem>
#include <QFile>
#include <QPainter>
#include <QXmlStreamReader>

namespace fs = std::filesystem;

namespace ui::preview {

std::optional<Anm2Data> Anm2Parser::parse(const std::string& anm2_path) const {
    QFile file(QString::fromStdString(anm2_path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;

    QByteArray data = file.readAll();
    file.close();

    std::string base_dir = fs::path(anm2_path).parent_path().string();
    return parse_xml(data.toStdString(), base_dir);
}

std::optional<Anm2Data> Anm2Parser::parse_xml(const std::string& xml_content,
                                                const std::string& base_dir) const {
    QXmlStreamReader xml(QString::fromStdString(xml_content));
    Anm2Data result;

    // ── Parse spritesheets ──
    struct SpritesheetInfo {
        QString id;
        QString path;
        QImage image;
    };
    std::vector<SpritesheetInfo> spritesheets;

    // ── Parse info/fps ──
    int fps = 30;

    // ── First pass: collect spritesheets ──
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;

        if (xml.name() == u"Info") {
            auto fps_attr = xml.attributes().value("Fps");
            if (!fps_attr.isEmpty()) fps = fps_attr.toInt();
        }

        if (xml.name() == u"Spritesheet") {
            SpritesheetInfo ss;
            ss.id = xml.attributes().value("Id").toString();
            ss.path = xml.attributes().value("Path").toString();
            ss.path.replace("\\", "/");

            std::string resolved = resolve_spritesheet(base_dir, ss.path.toStdString());
            if (!resolved.empty()) {
                ss.image = QImage(QString::fromStdString(resolved));
            }
            spritesheets.push_back(ss);
        }
    }

    if (spritesheets.empty()) return std::nullopt;

    // Re-parse for the full animation data
    xml.clear();
    xml.addData(QString::fromStdString(xml_content));

    // ── Parse layers (Id -> SpritesheetId mapping) ──
    struct LayerInfo {
        int id = 0;
        int spritesheet_id = 0;
    };
    std::vector<LayerInfo> layers;

    // ── Parse all animations ──
    struct AnimData {
        struct RootFrame {
            bool visible = true;
            int x = 0;
            int y = 0;
        };
        struct LayerFrame {
            bool visible = true;
            int x = 0, y = 0;
            int xp = 0, yp = 0;
            int w = 0, h = 0;
            int xcrop = 0, ycrop = 0;
            int delay = 1;
            int layer_id = 0;
        };
        std::vector<RootFrame> root_frames;
        std::vector<std::vector<LayerFrame>> layer_animations; // per-layer frames
        std::vector<int> layer_order;
    };
    std::vector<AnimData> animations;

    // State for parsing
    int current_layer_id = 0;
    int current_spritesheet_id = 0;
    bool in_content = false;
    bool in_animations = false;
    int anim_index = -1;
    enum Section { NONE, LAYERS, LAYER_ANIMS, ROOT_ANIM } section = NONE;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == u"Content") {
                in_content = true;
            } else if (xml.name() == u"Layer" && in_content) {
                LayerInfo li;
                li.id = xml.attributes().value("Id").toInt();
                li.spritesheet_id = xml.attributes().value("SpritesheetId").toInt();
                layers.push_back(li);
            } else if (xml.name() == u"Animations") {
                in_animations = true;
            } else if (xml.name() == u"Animation" && in_animations) {
                anim_index++;
                animations.emplace_back();
            } else if (xml.name() == u"RootAnimation" && anim_index >= 0) {
                section = ROOT_ANIM;
            } else if (xml.name() == u"LayerAnimations" && anim_index >= 0) {
                section = LAYER_ANIMS;
            } else if (xml.name() == u"LayerAnimation" && anim_index >= 0 && section == LAYER_ANIMS) {
                current_layer_id = xml.attributes().value("LayerId").toInt();
            } else if (xml.name() == u"Frame") {
                auto visible = [](QXmlStreamAttributes a, const QString& name) {
                    auto v = a.value(name);
                    return v.isEmpty() || v.toString().toLower() != "false";
                };

                if (section == ROOT_ANIM && anim_index >= 0) {
                    AnimData::RootFrame rf;
                    rf.visible = visible(xml.attributes(), "Visible");
                    rf.x = xml.attributes().value("XPosition").toInt();
                    rf.y = xml.attributes().value("YPosition").toInt();
                    animations[anim_index].root_frames.push_back(rf);
                } else if (section == LAYER_ANIMS && anim_index >= 0) {
                    AnimData::LayerFrame lf;
                    lf.visible = visible(xml.attributes(), "Visible");
                    lf.x = xml.attributes().value("XPosition").toInt();
                    lf.y = xml.attributes().value("YPosition").toInt();
                    lf.xp = xml.attributes().value("XPivot").toInt();
                    lf.yp = xml.attributes().value("YPivot").toInt();
                    lf.w = xml.attributes().value("Width").toInt();
                    lf.h = xml.attributes().value("Height").toInt();
                    lf.xcrop = xml.attributes().value("XCrop").toInt();
                    lf.ycrop = xml.attributes().value("YCrop").toInt();
                    lf.delay = xml.attributes().value("Delay").toInt();
                    if (lf.delay < 1) lf.delay = 1;
                    lf.layer_id = current_layer_id;

                    auto& anim = animations[anim_index];
                    // Find or create entry for this layer
                    bool found = false;
                    for (auto& la : anim.layer_animations) {
                        if (!la.empty() && la.front().layer_id == current_layer_id) {
                            la.push_back(lf);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        anim.layer_animations.push_back({lf});
                        anim.layer_order.push_back(current_layer_id);
                    }
                }
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == u"Content") in_content = false;
            else if (xml.name() == u"RootAnimation") section = LAYER_ANIMS;
            else if (xml.name() == u"LayerAnimations") section = NONE;
            else if (xml.name() == u"Animations") in_animations = false;
        }
    }

    if (animations.empty()) return std::nullopt;

    result.fps = fps;

    // ── Composite frames ──
    // First pass: compute bounding box from the first animation
    int min_x = 1000000, min_y = 1000000;
    int max_x = -1000000, max_y = -1000000;

    // Build spritesheet lookup: id -> QImage
    std::unordered_map<int, QImage> ss_lookup;
    for (const auto& ss : spritesheets) {
        if (!ss.image.isNull()) {
            ss_lookup[ss.id.toInt()] = ss.image;
        }
    }

    // Build layer -> spritesheet mapping
    std::unordered_map<int, int> layer_to_ss;
    for (const auto& l : layers) {
        layer_to_ss[l.id] = l.spritesheet_id;
    }

    const auto& first_anim = animations[0];

    for (size_t frame_idx = 0; frame_idx < first_anim.root_frames.size() ||
         frame_idx < first_anim.layer_animations.size(); frame_idx++) {
        const auto& rf = frame_idx < first_anim.root_frames.size()
            ? first_anim.root_frames[frame_idx] : first_anim.root_frames.back();

        if (!rf.visible) continue;

        for (size_t li = 0; li < first_anim.layer_order.size(); li++) {
            int lid = first_anim.layer_order[li];
            const auto& la = first_anim.layer_animations[li];
            if (frame_idx >= la.size()) {
                // Use last frame if this layer has fewer frames
                if (!la.empty()) {
                    const auto& lf = la.back();
                    if (!lf.visible || lf.w == 0 || lf.h == 0) continue;

                    int lx = rf.x + lf.x - lf.xp;
                    int ly = rf.y + lf.y - lf.yp;
                    int rx = lx + lf.w;
                    int by = ly + lf.h;
                    if (lx < min_x) min_x = lx;
                    if (ly < min_y) min_y = ly;
                    if (rx > max_x) max_x = rx;
                    if (by > max_y) max_y = by;
                }
                continue;
            }
            const auto& lf = la[frame_idx];
            if (!lf.visible || lf.w == 0 || lf.h == 0) continue;

            int lx = rf.x + lf.x - lf.xp;
            int ly = rf.y + lf.y - lf.yp;
            int rx = lx + lf.w;
            int by = ly + lf.h;
            if (lx < min_x) min_x = lx;
            if (ly < min_y) min_y = ly;
            if (rx > max_x) max_x = rx;
            if (by > max_y) max_y = by;
        }
    }

    if (min_x == 1000000) return std::nullopt;

    int cw = std::max(max_x - min_x, 1);
    int ch = std::max(max_y - min_y, 1);
    int ox = min_x;
    int oy = min_y;

    // Second pass: composite each frame
    for (size_t frame_idx = 0; frame_idx < first_anim.root_frames.size() ||
         frame_idx < first_anim.layer_animations.size(); frame_idx++) {
        const auto& rf = frame_idx < first_anim.root_frames.size()
            ? first_anim.root_frames[frame_idx] : first_anim.root_frames.back();

        if (!rf.visible) continue;

        Anm2Frame frame;

        // Compute max delay from any layer
        int max_delay = 1;

        for (size_t li = 0; li < first_anim.layer_order.size(); li++) {
            int lid = first_anim.layer_order[li];
            const auto& la = first_anim.layer_animations[li];

            const AnimData::LayerFrame* lf_ptr = nullptr;
            if (frame_idx < la.size()) {
                lf_ptr = &la[frame_idx];
            } else if (!la.empty()) {
                lf_ptr = &la.back();
            }

            if (!lf_ptr || !lf_ptr->visible || lf_ptr->w == 0 || lf_ptr->h == 0) continue;

            max_delay = std::max(max_delay, lf_ptr->delay);

            int lx = rf.x + lf_ptr->x - lf_ptr->xp;
            int ly = rf.y + lf_ptr->y - lf_ptr->yp;

            // Get the spritesheet image
            auto ss_it = layer_to_ss.find(lid);
            if (ss_it == layer_to_ss.end()) continue;
            auto img_it = ss_lookup.find(ss_it->second);
            if (img_it == ss_lookup.end() || img_it->second.isNull()) continue;

            // Crop the sprite
            QImage cropped = img_it->second.copy(
                lf_ptr->xcrop, lf_ptr->ycrop, lf_ptr->w, lf_ptr->h);

            Anm2Frame::LayerItem item;
            item.position = QPointF(lx - ox, ly - oy);
            item.sprite = cropped;
            frame.items.push_back(item);
        }

        frame.delay_ms = std::max(max_delay * 1000 / fps, 16);
        result.frames.push_back(frame);
    }

    if (result.frames.empty()) return std::nullopt;

    result.canvas_width = cw;
    result.canvas_height = ch;
    return result;
}

std::string Anm2Parser::resolve_spritesheet(const std::string& anm2_dir,
                                             const std::string& ss_path) const {
    // Walk up from anm2_dir looking for a gfx/ directory (Isaac mod structure)
    std::string resource_root = anm2_dir;
    fs::path mod_dir(anm2_dir);
    for (auto p = mod_dir; p != p.parent_path(); p = p.parent_path()) {
        if (fs::is_directory(p / "gfx")) {
            resource_root = p.string();
            break;
        }
    }

    // Try relative to gfx/ root, then relative to anm2 dir
    std::vector<fs::path> candidates = {
        fs::path(resource_root) / "gfx" / ss_path,
        fs::path(anm2_dir) / ss_path
    };

    for (const auto& c : candidates) {
        if (fs::exists(c)) return c.string();
    }

    // Case-insensitive fallback (Linux)
    std::string basename_lower = fs::path(ss_path).filename().string();
    std::transform(basename_lower.begin(), basename_lower.end(),
                   basename_lower.begin(), ::tolower);

    for (const auto& c : candidates) {
        std::string lower = c.string();
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (fs::exists(lower)) return lower;
    }

    // Deep search for the file (case-insensitive)
    for (auto it = fs::recursive_directory_iterator(resource_root);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        std::string fname = it->path().filename().string();
        std::transform(fname.begin(), fname.end(), fname.begin(), ::tolower);
        if (fname == basename_lower) return it->path().string();
    }

    return {};
}

}  // namespace ui::preview
