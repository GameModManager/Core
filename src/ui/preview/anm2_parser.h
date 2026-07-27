#pragma once

#include <optional>
#include <string>
#include <vector>

#include <QImage>
#include <QPointF>

namespace ui::preview {

struct Anm2Frame {
    struct LayerItem {
        QPointF position;   // composited position on canvas
        QImage sprite;      // cropped sprite from spritesheet
    };
    std::vector<LayerItem> items;
    int delay_ms = 33;     // frame duration in milliseconds
};

struct Anm2Data {
    std::vector<Anm2Frame> frames;
    int canvas_width = 0;
    int canvas_height = 0;
    int fps = 30;
};

// Parse an Isaac .anm2 animation file.
//
// .anm2 is an XML format that references spritesheets and defines
// per-layer, per-frame sprite regions with position/size/crop data.
// The parser resolves spritesheets relative to the mod's gfx/ directory,
// crops individual sprites, and composites each animation frame into
// a list of positioned QImage layers.
class Anm2Parser {
public:
    // Parse a .anm2 file, resolving spritesheets relative to anm2_dir.
    // Returns nullopt on parse failure or missing spritesheets.
    std::optional<Anm2Data> parse(const std::string& anm2_path) const;

    // Parse from a pre-read XML string (for testing or when file is in memory).
    std::optional<Anm2Data> parse_xml(const std::string& xml_content,
                                       const std::string& base_dir) const;

private:
    // Resolve a spritesheet path referenced in the .anm2 XML.
    // Searches from the mod's gfx/ root downward.
    std::string resolve_spritesheet(const std::string& anm2_dir,
                                    const std::string& ss_path) const;
};

}  // namespace ui::preview
