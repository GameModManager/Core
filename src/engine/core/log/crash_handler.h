#pragma once

#include <string>

namespace engine {

class CrashHandler {
public:
    static void install(const std::string& dump_dir);
    static void uninstall();

private:
    static void signal_handler(int sig);
    static void write_dump(int sig);
    static std::string dump_dir_;
};

}  // namespace engine
