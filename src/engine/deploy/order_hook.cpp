#include "engine/deploy/order_hook.h"

#include <fstream>

namespace engine {

bool PlainTextOrderHook::write_order(
    const std::vector<std::string>& ordered_mod_ids,
    const std::filesystem::path& output_path) {
    std::ofstream out(output_path);
    if (!out) return false;

    for (const auto& mod_id : ordered_mod_ids) {
        out << mod_id << "\n";
    }
    return out.good();
}

}  // namespace engine
