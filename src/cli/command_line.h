#pragma once

#include <QCommandLineParser>
#include <QString>

namespace cli {

struct ParsedArgs {
  bool show_help = false;
  bool show_version = false;
  bool headless = false;
  bool handle_nxm = false;
  bool handle_gmm = false;
  QString instance_name;
  QString exe_path;
  QString nxm_url;
  QString gmm_url;
};

class CommandLine {
public:
  CommandLine(int argc, char **argv);
  bool parse(); // returns false when --help/--version printed
  const ParsedArgs &args() const;
  bool should_exit() const;
  int exit_code() const;

private:
  QCommandLineParser parser_;
  ParsedArgs args_;
  bool should_exit_ = false;
  int exit_code_ = 0;
};

} // namespace cli
