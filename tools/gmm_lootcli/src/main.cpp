// gmm_lootcli — LOOT load-order sort subprocess (PLAN.md §7.1, Phase 5.5).
//
// Linux port of MO2's modorganizer-lootcli: a small standalone binary linking
// libloot that the engine spawns to sort a game's plugin load order. Plain
// C++20, no Qt. Networking is deliberately absent — the engine's masterlist
// manager places masterlist.yaml + prelude.yaml on disk and this tool only
// reads them, so the CLI stays hermetic and testable.
//
// stdout protocol (matches MO2's lootcli.h exactly, consumed by the engine):
//   [progress] N   stage marker, N = lootcli Progress enum (0-8)
//   [level] msg    log line, level in {trace,debug,info,warning,error}
// Errors also go to stderr as "Error: ..." with a non-zero exit code.
//
// libloot is GPL-3.0 (Copyright 2013-2026 Oliver Hamlet).

#include <loot/api.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Stage numbers mirror MO2's modorganizer-lootcli/include/lootcli/lootcli.h so
// the engine's stdout parser is source-compatible with both CLIs.
enum class Progress {
  None = 0,
  CheckingMasterlistExistence,
  UpdatingMasterlist,
  LoadingLists,
  ReadingPlugins,
  SortingPlugins,
  WritingLoadorder,
  ParsingLootMessages,
  Done,
};

void emit_progress(Progress stage) {
  std::cout << "[progress] " << static_cast<int>(stage) << "\n";
  std::cout.flush();
}

loot::LogLevel parse_log_level(const std::string& text) {
  if (text == "trace") return loot::LogLevel::trace;
  if (text == "debug") return loot::LogLevel::debug;
  if (text == "warning") return loot::LogLevel::warning;
  if (text == "error") return loot::LogLevel::error;
  return loot::LogLevel::info;
}

std::string log_level_name(loot::LogLevel level) {
  switch (level) {
    case loot::LogLevel::trace:
      return "trace";
    case loot::LogLevel::debug:
      return "debug";
    case loot::LogLevel::info:
      return "info";
    case loot::LogLevel::warning:
      return "warning";
    case loot::LogLevel::error:
      return "error";
  }
  return "info";
}

// Emits libloot's internal log output through the [level] stdout protocol.
class LogForwarder {
public:
  explicit LogForwarder(loot::LogLevel level) : level_(level) {
    loot::SetLoggingCallback(
        [this](loot::LogLevel level, std::string_view message) { log(level, message); });
    loot::SetLogLevel(level);
  }

  void log(loot::LogLevel level, std::string_view message) const {
    if (level < level_) {
      return;
    }
    std::cout << "[" << log_level_name(level) << "] " << message << "\n";
    std::cout.flush();
  }

private:
  loot::LogLevel level_;
};

struct Args {
  std::string game;                    // --game, e.g. "skyrimse"
  fs::path game_path;                  // --gamePath, dir containing the game executable
  fs::path local_path;                 // --localPath, profile dir holding loadorder.txt/plugins.txt
  fs::path plugin_paths_file;          // --pluginPathsFile, one absolute winning plugin path per line
  fs::path masterlist;                 // --masterlist, masterlist.yaml (engine-managed)
  fs::path prelude;                    // --prelude, prelude.yaml (engine-managed)
  fs::path plugin_list_output;         // --pluginListOutputPath, sorted list output
  fs::path report_output;              // --out, JSON report output
  std::optional<fs::path> plugin_list_path;  // --pluginListPath, current-order fallback
  loot::LogLevel log_level = loot::LogLevel::info;
  std::vector<fs::path> additional_data_paths;  // --additionalDataPaths, ';'-separated
};

[[noreturn]] void usage_error(const std::string& message) {
  throw std::runtime_error(message);
}

std::optional<std::string> take_value(int argc, char** argv, int& i,
                                      const std::string& key) {
  const std::string flag = "--" + key;
  if (i + 1 >= argc) {
    usage_error("argument missing " + key);
  }
  return std::string(argv[++i]);
}

std::optional<fs::path> take_path(int argc, char** argv, int& i,
                                  const std::string& key) {
  return fs::path(*take_value(argc, argv, i, key));
}

Args parse_args(int argc, char** argv) {
  Args args;
  bool have_game = false;
  bool have_game_path = false;
  bool have_local_path = false;
  bool have_plugin_paths_file = false;
  bool have_masterlist = false;
  bool have_prelude = false;
  bool have_output = false;
  bool have_report = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.size() < 3 || arg.rfind("--", 0) != 0) {
      usage_error("unexpected argument: " + arg);
    }
    const std::string key = arg.substr(2);

    if (key == "game") {
      args.game = *take_value(argc, argv, i, key);
      have_game = true;
    } else if (key == "gamePath") {
      args.game_path = *take_path(argc, argv, i, key);
      have_game_path = true;
    } else if (key == "localPath") {
      args.local_path = *take_path(argc, argv, i, key);
      have_local_path = true;
    } else if (key == "pluginPathsFile") {
      args.plugin_paths_file = *take_path(argc, argv, i, key);
      have_plugin_paths_file = true;
    } else if (key == "masterlist") {
      args.masterlist = *take_path(argc, argv, i, key);
      have_masterlist = true;
    } else if (key == "prelude") {
      args.prelude = *take_path(argc, argv, i, key);
      have_prelude = true;
    } else if (key == "pluginListOutputPath") {
      args.plugin_list_output = *take_path(argc, argv, i, key);
      have_output = true;
    } else if (key == "out") {
      args.report_output = *take_path(argc, argv, i, key);
      have_report = true;
    } else if (key == "pluginListPath") {
      args.plugin_list_path = *take_path(argc, argv, i, key);
    } else if (key == "logLevel") {
      args.log_level = parse_log_level(*take_value(argc, argv, i, key));
    } else if (key == "language") {
      (void)take_value(argc, argv, i, key);  // accepted for MO2 parity, unused
    } else if (key == "additionalDataPaths") {
      const std::string value = *take_value(argc, argv, i, key);
      std::size_t start = 0;
      while (start < value.size()) {
        const auto sep = value.find(';', start);
        if (sep == std::string::npos) {
          args.additional_data_paths.emplace_back(value.substr(start));
          break;
        }
        args.additional_data_paths.emplace_back(value.substr(start, sep - start));
        start = sep + 1;
      }
    } else {
      usage_error("unknown argument: --" + key);
    }
  }

  if (!have_game || !have_game_path || !have_local_path || !have_plugin_paths_file ||
      !have_masterlist || !have_prelude || !have_output || !have_report) {
    usage_error("required arguments: --game --gamePath --localPath --pluginPathsFile "
                "--masterlist --prelude --pluginListOutputPath --out");
  }
  return args;
}

std::optional<loot::GameType> game_type_for(const std::string& name) {
  static const std::unordered_map<std::string, loot::GameType> kGameMap = {
      {"morrowind", loot::GameType::tes3},
      {"oblivion", loot::GameType::tes4},
      {"oblivionremastered", loot::GameType::oblivionRemastered},
      {"skyrim", loot::GameType::tes5},
      {"skyrimse", loot::GameType::tes5se},
      {"skyrimvr", loot::GameType::tes5vr},
      {"fallout3", loot::GameType::fo3},
      {"falloutnv", loot::GameType::fonv},
      {"fallout4", loot::GameType::fo4},
      {"fallout4vr", loot::GameType::fo4vr},
      {"starfield", loot::GameType::starfield},
      {"openmw", loot::GameType::openmw},
  };
  const auto it = kGameMap.find(name);
  if (it == kGameMap.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<fs::path> read_plugin_paths(const fs::path& file) {
  std::ifstream in(file);
  if (!in.is_open()) {
    throw std::runtime_error("could not open plugin paths file " + file.string());
  }
  std::vector<fs::path> paths;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    paths.emplace_back(line);
  }
  return paths;
}

std::vector<std::string> read_load_order(const fs::path& file) {
  std::vector<std::string> order;
  std::ifstream in(file);
  if (!in.is_open()) {
    return order;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    order.push_back(line);
  }
  return order;
}

std::unordered_set<std::string> loaded_filenames(loot::GameInterface* game) {
  std::unordered_set<std::string> names;
  for (const auto& plugin : game->GetLoadedPlugins()) {
    if (plugin != nullptr) {
      names.insert(plugin->GetName());
    }
  }
  return names;
}

void write_sorted_list(const fs::path& output, const std::vector<std::string>& sorted) {
  std::ofstream out(output);
  if (!out.is_open()) {
    throw std::runtime_error("failed to open " + output.string() + " to rewrite it");
  }
  out << "# This file was automatically generated by GameModManager's gmm_lootcli.\n";
  for (const auto& plugin : sorted) {
    out << plugin << "\n";
  }
  if (!out) {
    throw std::runtime_error("failed to write " + output.string());
  }
}

// Minimal but real JSON report: the sorted order plus per-plugin flags. The
// engine drives its UI from the sorted list; this file is for display/debugging.
void write_report(const fs::path& output, const std::string& game_name,
                  const std::vector<std::string>& sorted) {
  std::ofstream out(output);
  if (!out.is_open()) {
    throw std::runtime_error("failed to open report output " + output.string());
  }
  out << "{\n  \"game\": " << '"' << game_name << "\",\n  \"masterlistLoaded\": true,\n"
      << "  \"sortedPlugins\": [";
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << '"' << sorted[i] << '"';
  }
  out << "]\n}\n";
  if (!out) {
    throw std::runtime_error("failed to write report " + output.string());
  }
}

int run(const Args& args) {
  const auto game_type = game_type_for(args.game);
  if (!game_type) {
    std::cerr << "Error: no LOOT support for game \"" << args.game << "\"\n";
    return 2;
  }

  LogForwarder log(args.log_level);

  emit_progress(Progress::CheckingMasterlistExistence);
  if (!fs::exists(args.masterlist) || !fs::exists(args.prelude)) {
    std::cerr << "Error: masterlist (" << args.masterlist << ") or prelude ("
              << args.prelude << ") not found\n";
    return 3;
  }

  emit_progress(Progress::UpdatingMasterlist);
  log.log(loot::LogLevel::info,
          "masterlist updates are managed by the GameModManager engine; gmm_lootcli "
          "sorts using the on-disk files");

  // game_path = game install dir; local_path = the MO2-format profile dir, so
  // libloot reads the current load order and active state from the profile's
  // loadorder.txt / plugins.txt (exactly what MO2 does with the same contract).
  auto game = loot::CreateGameHandle(*game_type, args.game_path, args.local_path);
  game->SetAdditionalDataPaths(args.additional_data_paths);

  emit_progress(Progress::LoadingLists);
  game->GetDatabase().LoadMasterlistWithPrelude(args.masterlist, args.prelude);

  emit_progress(Progress::ReadingPlugins);
  const auto plugin_paths = read_plugin_paths(args.plugin_paths_file);
  if (plugin_paths.empty()) {
    std::cerr << "Error: no plugins listed in " << args.plugin_paths_file.string()
              << "\n";
    return 4;
  }
  game->LoadPlugins(plugin_paths, false);
  game->LoadCurrentLoadOrderState();
  std::vector<std::string> current_order = game->GetLoadOrder();
  if (current_order.empty() && args.plugin_list_path) {
    current_order = read_load_order(*args.plugin_list_path);
  }

  // Restrict the sort input to plugins that were actually loaded (a loadorder.txt
  // entry with no file on disk is a missing plugin, and libloot requires every
  // given plugin to be loaded). Any loaded plugin not yet listed is appended so
  // newly-installed plugins still get sorted into the result.
  const auto loaded = loaded_filenames(game.get());
  std::vector<std::string> sort_input;
  std::unordered_set<std::string> in_input;
  for (const auto& name : current_order) {
    if (loaded.count(name) != 0 && in_input.insert(name).second) {
      sort_input.push_back(name);
    }
  }
  for (const auto& name : loaded) {
    if (in_input.insert(name).second) {
      sort_input.push_back(name);
    }
  }

  if (sort_input.empty()) {
    std::cerr << "Error: no plugins to sort\n";
    return 4;
  }

  emit_progress(Progress::SortingPlugins);
  const std::vector<std::string> sorted = game->SortPlugins(sort_input);

  emit_progress(Progress::WritingLoadorder);
  write_sorted_list(args.plugin_list_output, sorted);

  emit_progress(Progress::ParsingLootMessages);
  write_report(args.report_output, args.game, sorted);

  emit_progress(Progress::Done);
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    const Args args = parse_args(argc, argv);
    return run(args);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
