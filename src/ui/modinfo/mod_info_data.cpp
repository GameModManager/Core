#include "ui/modinfo/mod_info_data.h"

#include <QColor>

#include <algorithm>

namespace ui {

std::vector<ModInfoData::ConflictFile> ModInfoData::conflict_files() const {
    std::vector<ConflictFile> out;
    out.reserve(conflicts.size());

    const auto strip_prefix = [&](const QString& rel) {
        if (!data_subpath.isEmpty() &&
            rel.startsWith(data_subpath + QLatin1Char('/')))
            return rel.mid(data_subpath.length() + 1);
        return rel;
    };

    for (const auto& [path, owners] : conflicts) {
        for (const auto& [owner, priority] : owners) {
            if (owner != id) continue;

            // Winner = owner with the extreme priority (matches ConflictEngine:
            // max priority normally, min when reversed).
            int best = owners.front().second;
            for (const auto& o : owners) {
                best = conflict_reversed
                           ? std::min(best, o.second)
                           : std::max(best, o.second);
            }
            // The engine's max/min_element returns the FIRST extreme; pick the
            // first owner whose priority equals the extreme to match it.
            bool won = false;
            for (const auto& o : owners) {
                if (o.second == best) {
                    won = (o.first == id);
                    break;
                }
            }

            out.push_back({path, strip_prefix(path), won});
            break;
        }
    }
    return out;
}

QColor ModInfoData::color_value() const {
    if (color.isEmpty()) return {};
    QColor c(color);
    return c.isValid() ? c : QColor();
}

}  // namespace ui
