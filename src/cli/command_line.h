#pragma once

#include <QCommandLineParser>
#include <QString>
#include <QStringList>

namespace cli {

struct ParsedArgs {
  bool show_help    = false;
  bool show_version = false;
  bool headless     = false;
  bool handle_nxm   = false;
  bool handle_gmm   = false;
  bool handle_modl  = false;
  QString instance_name;
  QString exe_path;
  QString nxm_url;
  QString gmm_url;
  QString modl_url;
  // Headless launch overrides (--args/--env/--cwd). Each has_* flag records
  // whether the flag was passed: an explicit flag wins over the matching
  // instance.toml executables entry for that field (see --help).
  QString launch_args;
  QStringList launch_env;
  QString launch_cwd;
  bool has_launch_args = false;
  bool has_launch_env  = false;
  bool has_launch_cwd  = false;
};

class CommandLine {
public:
  CommandLine(int argc, char **argv);
  bool parse();  // returns false when --help/--version printed
  const ParsedArgs &args() const;
  bool should_exit() const;
  int exit_code() const;

private:
  QCommandLineParser parser_;
  ParsedArgs args_;
  bool should_exit_ = false;
  int exit_code_    = 0;
};

}  // namespace cli
