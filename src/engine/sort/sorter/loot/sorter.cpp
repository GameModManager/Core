#include "engine/sort/sorter/loot/sorter.h"

#include "engine/core/log/logger.h"
#include "engine/core/util/process_utils.h"
#include "engine/sort/sorter/loot/masterlists.h"
#include "platform/platform.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace engine {
namespace Sorter {
  namespace Loot {

    namespace {

      constexpr const char* kPluginPathsFile = "loot_plugin_paths.txt";
      constexpr const char* kSortedFile      = "loot_sorted.txt";
      constexpr const char* kReportFile      = "loot_report.json";

      // --- stdout protocol parsing (MO2 lootcli.h) ------------------------------
      //   [progress] N    stage marker, N = lootcli Progress enum
      //   [level] msg     log line, level in {trace,debug,info,warning,error}

      void parse_line(const std::string& line, ProgressFn& progress,
                      std::vector<std::string>& messages) {
        if (line.rfind("[progress] ", 0) == 0) {
          const std::string value = line.substr(11);
          char* end               = nullptr;
          const long stage        = std::strtol(value.c_str(), &end, 10);
          if (end && *end == '\0') {
            if (progress)
              progress(static_cast<int>(stage), "");
          }
          return;
        }
        if (line.rfind("[", 0) == 0) {
          const auto close = line.find(']');
          if (close != std::string::npos && close + 2 <= line.size() &&
              line[close + 1] == ' ') {
            messages.push_back(line);
            const std::string level   = line.substr(1, close - 1);
            const std::string message = line.substr(close + 2);
            if (level == "warning" || level == "error") {
              Logger::instance().warn("gmm_lootcli [" + level + "] " + message);
            } else {
              Logger::instance().debug("gmm_lootcli [" + level + "] " + message);
            }
            return;
          }
        }
        // Unmatched line - relay verbatim at trace level for diagnostics.
        messages.push_back(line);
      }

      // --- subprocess capture: engine::run_captured (process_utils.h) -----------
      // gmm_lootcli is a non-interactive CLI; stdin is /dev/null and stdout/stderr
      // are captured for protocol parsing below.

      fs::path make_scratch_dir(const fs::path& profile_dir) {
        std::error_code ec;
        fs::create_directories(profile_dir, ec);
        const fs::path dir = profile_dir / ".gmm_loot_tmp";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        return dir;
      }

      // --- loot_report.json per-plugin parsing (MO2 LootDialog::showReport parity)
      // The CLI writes MO2 lootcli's createPlugins shape; each entry feeds one
      // GamePlugin::loot_report (tooltip bullets + warning/information/dirty
      // emblems). Unknown keys and malformed entries are skipped; a missing or
      // unparsable report leaves every plugin without LOOT data (never an error:
      // the sort order itself is unaffected).
      std::string json_string(const nlohmann::json& v) {
        return v.is_string() ? v.get<std::string>() : std::string{};
      }

      long long json_count(const nlohmann::json& v) {
        if (v.is_number_unsigned())
          return static_cast<long long>(v.get<unsigned long long>());
        if (v.is_number_integer())
          return static_cast<long long>(v.get<long long>());
        return 0;
      }

      const nlohmann::json* find_key(const nlohmann::json& obj, const char* key) {
        if (!obj.is_object())
          return nullptr;
        const auto it = obj.find(key);
        return it != obj.end() ? &(*it) : nullptr;
      }

      // MO2 lootcli message types ("info"/"warn"/"error"/"unknown") map onto the
      // Warning:/Error: prefixes of PluginList::makeLootTooltip.
      std::string loot_message_level(const std::string& type) {
        if (type == "warn")
          return "warning";
        if (type == "error")
          return "error";
        return "info";
      }

      LootReport parse_plugin_report(const nlohmann::json& entry) {
        LootReport report;
        if (const nlohmann::json* v = find_key(entry, "incompatibilities")) {
          if (v->is_array()) {
            for (const auto& item : *v) {
              const std::string name =
                  json_string(item.value("name", nlohmann::json{}));
              if (name.empty())
                continue;
              const std::string display =
                  json_string(item.value("displayName", nlohmann::json{}));
              report.incompatibilities.emplace_back(name, display);
            }
          }
        }
        if (const nlohmann::json* v = find_key(entry, "missingMasters")) {
          if (v->is_array()) {
            for (const auto& item : *v) {
              const std::string name = json_string(item);
              if (!name.empty())
                report.missing_masters.push_back(name);
            }
          }
        }
        if (const nlohmann::json* v = find_key(entry, "messages")) {
          if (v->is_array()) {
            for (const auto& item : *v) {
              LootReport::Message message;
              message.level =
                  loot_message_level(json_string(item.value("type", nlohmann::json{})));
              message.text = json_string(item.value("text", nlohmann::json{}));
              if (message.text.empty())
                continue;
              report.messages.push_back(std::move(message));
            }
          }
        }
        if (const nlohmann::json* v = find_key(entry, "dirty")) {
          if (v->is_array()) {
            for (const auto& item : *v) {
              LootReport::DirtyEntry entry;
              entry.itm_records = json_count(item.value("itm", nlohmann::json{}));
              entry.deleted_references =
                  json_count(item.value("deletedReferences", nlohmann::json{}));
              entry.deleted_navmeshes =
                  json_count(item.value("deletedNavmesh", nlohmann::json{}));
              entry.cleaning_utility =
                  json_string(item.value("cleaningUtility", nlohmann::json{}));
              entry.info = json_string(item.value("info", nlohmann::json{}));
              report.dirty.push_back(std::move(entry));
            }
          }
        }
        if (const nlohmann::json* v = find_key(entry, "clean")) {
          if (v->is_array()) {
            for (const auto& item : *v) {
              LootReport::CleanEntry entry;
              entry.cleaning_utility =
                  json_string(item.value("cleaningUtility", nlohmann::json{}));
              report.clean.push_back(std::move(entry));
            }
          }
        }
        return report;
      }

      std::map<std::string, LootReport>
      parse_loot_reports(const fs::path& report_file) {
        std::map<std::string, LootReport> reports;
        std::ifstream in(report_file);
        if (!in.is_open())
          return reports;
        try {
          const nlohmann::json root     = nlohmann::json::parse(in);
          const nlohmann::json* plugins = find_key(root, "plugins");
          if (plugins == nullptr || !plugins->is_array())
            return reports;
          for (const auto& entry : *plugins) {
            const std::string name = json_string(entry.value("name", nlohmann::json{}));
            if (name.empty())
              continue;
            LootReport report = parse_plugin_report(entry);
            if (!report.empty())
              reports.emplace(name, std::move(report));
          }
        } catch (const std::exception& e) {
          Logger::instance().warn(std::string{"LOOT report parse failed ("} +
                                  report_file.string() + "): " + e.what());
        }
        return reports;
      }

    }  // namespace

    Result run_sort(const Request& request, ProgressFn progress) {
      Result result;

      if (request.cli_path.empty() || !fs::is_regular_file(request.cli_path)) {
        result.error = "LOOT sorting is unavailable: gmm_lootcli was not built "
                       "(GMM_WITH_LOOT=OFF or the build did not produce the binary)";
        return result;
      }
      if (request.loot_game_id.empty()) {
        result.error = "This game has no LOOT support";
        return result;
      }
      if (request.plugins.empty()) {
        result.error = "No plugins to sort";
        return result;
      }
      if (request.profile_dir.empty() || request.game_dir.empty()) {
        result.error = "Game or profile directory not set";
        return result;
      }

      // 1. Masterlists - engine-owned so the CLI never touches the network.
      MasterlistManager manager(request.platform);
      MasterlistManager::Masterlists masterlists;
      if (request.update_masterlists) {
        masterlists =
            manager.ensure(request.game_id, request.masterlist_repo, &result.error);
      } else {
        masterlists.masterlist = manager.dir_for(request.game_id) / "masterlist.yaml";
        masterlists.prelude    = manager.dir_for(request.game_id) / "prelude.yaml";
      }
      if (masterlists.masterlist.empty() || masterlists.prelude.empty()) {
        if (result.error.empty())
          result.error = "masterlist unavailable";
        return result;
      }

      // 2. Scratch files: winning plugin paths in, sorted list + report out.
      const fs::path scratch     = make_scratch_dir(request.profile_dir);
      const fs::path paths_file  = scratch / kPluginPathsFile;
      const fs::path sorted_file = scratch / kSortedFile;
      const fs::path report_file = scratch / kReportFile;

      {
        std::ofstream out(paths_file);
        if (!out.is_open()) {
          result.error = "could not create " + paths_file.string();
          return result;
        }
        for (const auto& plugin : request.plugins) {
          if (plugin.full_path.empty())
            continue;
          out << plugin.full_path.string() << "\n";
        }
      }

      // 3. Spawn the CLI and stream its protocol.
      std::vector<std::string> args = {
          request.cli_path.string(),
          "--game",
          request.loot_game_id,
          "--gamePath",
          request.game_dir.string(),
          "--localPath",
          request.profile_dir.string(),
          "--pluginPathsFile",
          paths_file.string(),
          "--masterlist",
          masterlists.masterlist.string(),
          "--prelude",
          masterlists.prelude.string(),
          "--pluginListOutputPath",
          sorted_file.string(),
          "--out",
          report_file.string(),
          "--logLevel",
          "info",
      };

      Logger::instance().info("Running LOOT sort: " + args[0] + " --game " +
                              request.loot_game_id + " (" +
                              std::to_string(request.plugins.size()) + " plugins)");

      const CapturedProcess proc = run_captured(args);
      if (!proc.ok) {
        result.error = "failed to start " + request.cli_path.string();
        return result;
      }

      {
        std::istringstream in(proc.out);
        std::string line;
        while (std::getline(in, line)) {
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          parse_line(line, progress, result.messages);
        }
      }

      if (proc.exit_code != 0) {
        std::string reason;
        if (proc.err.rfind("Error: ", 0) == 0) {
          reason = proc.err.substr(7);
        } else if (!proc.err.empty()) {
          reason = proc.err;
        } else {
          reason = "gmm_lootcli exited with code " + std::to_string(proc.exit_code);
        }
        if (!reason.empty() && reason.back() == '\n')
          reason.pop_back();
        result.error = "LOOT sort failed: " + reason;
        Logger::instance().warn("gmm_lootcli exit " + std::to_string(proc.exit_code) +
                                ": " + reason);
        return result;
      }

      // 4. Read the sorted output (first line = first-loaded; skip the header).
      {
        std::ifstream in(sorted_file);
        if (!in.is_open()) {
          result.error = "LOOT sort finished but its output file is missing";
          return result;
        }
        std::string line;
        while (std::getline(in, line)) {
          if (line.empty() || line[0] == '#')
            continue;
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          result.sorted_names.push_back(line);
        }
      }
      if (result.sorted_names.empty()) {
        result.error = "LOOT sort produced no output";
        return result;
      }

      result.ok          = true;
      result.report_path = report_file;
      // Per-plugin findings for the tooltip bullets (MO2 LootDialog parity).
      // Never fails the sort: a missing/unparsable report just yields no data.
      result.reports = parse_loot_reports(report_file);
      if (progress)
        progress(8, "");  // Done
      return result;
    }

  }  // namespace Loot
}  // namespace Sorter

}  // namespace engine
