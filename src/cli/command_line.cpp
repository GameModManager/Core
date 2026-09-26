#include "cli/command_line.h"

#include <QCoreApplication>
#include <cstdio>

namespace cli {

CommandLine::CommandLine(int /*argc*/, char ** /*argv*/) {
  parser_.setApplicationDescription("GameModManager - Cross-platform game mod manager");
  parser_.addVersionOption();

  QCommandLineOption helpOpt("help", "Show this help message");
  QCommandLineOption helpShort("h", "Show this help message");
  parser_.addOption(helpOpt);
  parser_.addOption(helpShort);

  QCommandLineOption instanceOpt("instance", "Load specific instance by name", "name");
  parser_.addOption(instanceOpt);

  QCommandLineOption launchOpt("launch", "Launch game directly (headless mode)");
  parser_.addOption(launchOpt);

  QCommandLineOption exeOpt("exe", "Executable path relative to game dir", "path");
  parser_.addOption(exeOpt);

  QCommandLineOption argsOpt(
      "args", "Extra game arguments (shell-quoted, e.g. --args '-a \"b c\"')", "args");
  parser_.addOption(argsOpt);

  QCommandLineOption envOpt("env", "Environment override KEY=VALUE (repeatable)",
                            "KEY=VALUE");
  parser_.addOption(envOpt);

  QCommandLineOption cwdOpt("cwd", "Working directory (game-relative or absolute)",
                            "path");
  parser_.addOption(cwdOpt);

  QCommandLineOption nxmOpt("handle-nxm", "Handle an nxm:// download link", "url");
  parser_.addOption(nxmOpt);

  QCommandLineOption gmmOpt("handle-gmm", "Handle a gmm:// download link", "url");
  parser_.addOption(gmmOpt);

  QCommandLineOption modlOpt("handle-modl", "Handle a modl:// download link", "url");
  parser_.addOption(modlOpt);
}

bool CommandLine::parse() {
  parser_.process(*QCoreApplication::instance());

  args_.show_help   = parser_.isSet("help") || parser_.isSet("h");
  args_.headless    = parser_.isSet("launch");
  args_.handle_nxm  = parser_.isSet("handle-nxm");
  args_.handle_gmm  = parser_.isSet("handle-gmm");
  args_.handle_modl = parser_.isSet("handle-modl");

  if (parser_.isSet("instance"))
    args_.instance_name = parser_.value("instance");

  if (parser_.isSet("exe"))
    args_.exe_path = parser_.value("exe");

  args_.has_launch_args = parser_.isSet("args");
  args_.has_launch_env  = parser_.isSet("env");
  args_.has_launch_cwd  = parser_.isSet("cwd");
  if (args_.has_launch_args)
    args_.launch_args = parser_.value("args");
  if (args_.has_launch_env)
    args_.launch_env = parser_.values("env");
  if (args_.has_launch_cwd)
    args_.launch_cwd = parser_.value("cwd");

  if (parser_.isSet("handle-nxm"))
    args_.nxm_url = parser_.value("handle-nxm");

  if (parser_.isSet("handle-gmm"))
    args_.gmm_url = parser_.value("handle-gmm");

  if (parser_.isSet("handle-modl"))
    args_.modl_url = parser_.value("handle-modl");

  if (args_.show_help) {
    // ANSI color codes
    constexpr const char *R = "\033[31m";        // red - headers
    constexpr const char *G = "\033[32m";        // green - short flags
    constexpr const char *O = "\033[38;5;208m";  // orange - long flags
    constexpr const char *B = "\033[34m";        // blue - variables
    constexpr const char *D = "\033[0m";         // default reset

    // Header
    fprintf(stdout, "%sUsage:%s\n", R, D);
    fprintf(stdout, "  gamemodmanager [options]\n\n");

    // Examples
    fprintf(stdout, "%sExamples:%s\n", R, D);
    fprintf(stdout, "  gamemodmanager                         # Start GUI with "
                    "last-used instance\n");
    fprintf(stdout,
            "  gamemodmanager %s--instance%s %s<path>%s   Start GUI with a "
            "specific instance\n",
            O, D, B, D);
    fprintf(stdout,
            "  gamemodmanager %s--handle-nxm%s %s<url>%s      # Handle an "
            "nxm:// download link\n",
            O, D, B, D);
    fprintf(stdout,
            "  gamemodmanager %s--handle-gmm%s %s<url>%s      # Handle a "
            "gmm:// download link\n",
            O, D, B, D);
    fprintf(stdout,
            "  gamemodmanager %s--handle-modl%s %s<url>%s     # Handle a "
            "modl:// download link\n",
            O, D, B, D);
    fprintf(stdout,
            "  gamemodmanager %s--launch%s %s--instance%s %s<path>%s "
            "%s--exe%s %s<path>%s\n",
            O, D, O, D, B, D, O, D, B, D);
    fprintf(stdout, "                                # Launch game headless\n");
    fprintf(stdout,
            "  gamemodmanager %s--launch%s %s--instance%s %s<path>%s "
            "%s--exe%s %s<path>%s\n",
            O, D, O, D, B, D, O, D, B, D);
    fprintf(
        stdout,
        "    %s--args%s %s<args>%s %s--env%s %s<KEY=VALUE>%s %s--cwd%s %s<path>%s\n", O,
        D, B, D, O, D, B, D, O, D, B, D);
    fprintf(stdout, "        # Headless launch with args/env/cwd overrides\n");
    fprintf(stdout,
            "  (Explicit %s--args%s/%s--env%s/%s--cwd%s win over the matching "
            "instance.toml executables entry for that field;\n",
            O, D, O, D, O, D);
    fprintf(stdout, "   without flags the entry is used.)\n\n");

    // Options
    fprintf(stdout, "%sOptions:%s\n", R, D);
    fprintf(stdout, "  %s-h%s, %s--help%s          Show this help message\n", G, D, O,
            D);
    fprintf(stdout, "  %s-v%s, %s--version%s       Show version information\n", G, D, O,
            D);
    fprintf(stdout, "  %s--instance%s %s<path>%s   Instance path or name\n", O, D, B,
            D);
    fprintf(stdout, "  %s--launch%s            Launch game directly (headless)\n", O,
            D);
    fprintf(stdout,
            "  %s--exe%s %s<path>%s      Executable path relative to game "
            "dir\n",
            O, D, B, D);
    fprintf(stdout, "  %s--args%s %s<args>%s     Extra game arguments (shell-quoted)\n",
            O, D, B, D);
    fprintf(stdout, "  %s--env%s %s<KEY=VALUE>%s  Environment override (repeatable)\n",
            O, D, B, D);
    fprintf(stdout,
            "  %s--cwd%s %s<path>%s      Working directory (game-rel. or "
            "absolute)\n",
            O, D, B, D);
    fprintf(stdout,
            "  %s--handle-nxm%s %s<url>%s  Handle an nxm:// download "
            "link\n",
            O, D, B, D);
    fprintf(stdout,
            "  %s--handle-gmm%s %s<url>%s  Handle a gmm:// download "
            "link\n",
            O, D, B, D);
    fprintf(stdout,
            "  %s--handle-modl%s %s<url>%s  Handle a modl:// download "
            "link\n",
            O, D, B, D);

    should_exit_ = true;
    exit_code_   = 0;
    return false;
  }

  return true;
}

const ParsedArgs &CommandLine::args() const {
  return args_;
}

bool CommandLine::should_exit() const {
  return should_exit_;
}

int CommandLine::exit_code() const {
  return exit_code_;
}

}  // namespace cli
