#include "engine/sort/loot/loot_sorter.h"

#include "engine/log/logger.h"
#include "engine/sort/loot/masterlists.h"
#include "platform/platform_interface.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace engine {

namespace {

constexpr const char* kPluginPathsFile = "loot_plugin_paths.txt";
constexpr const char* kSortedFile = "loot_sorted.txt";
constexpr const char* kReportFile = "loot_report.json";

// --- stdout protocol parsing (MO2 lootcli.h) ------------------------------
//   [progress] N    stage marker, N = lootcli Progress enum
//   [level] msg     log line, level in {trace,debug,info,warning,error}

void parse_line(const std::string& line, LootProgressFn& progress,
                std::vector<std::string>& messages) {
    if (line.rfind("[progress] ", 0) == 0) {
        const std::string value = line.substr(11);
        char* end = nullptr;
        const long stage = std::strtol(value.c_str(), &end, 10);
        if (end && *end == '\0') {
            if (progress) progress(static_cast<int>(stage), "");
        }
        return;
    }
    if (line.rfind("[", 0) == 0) {
        const auto close = line.find(']');
        if (close != std::string::npos && close + 2 <= line.size() &&
            line[close + 1] == ' ') {
            messages.push_back(line);
            const std::string level = line.substr(1, close - 1);
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

// --- subprocess capture (POSIX only; gmm_lootcli builds on Linux) ---------

struct CapturedProcess {
    bool ok = false;
    int exit_code = -1;
    std::string out;
    std::string err;
};

CapturedProcess run_captured(const std::vector<std::string>& args) {
    CapturedProcess result;
#ifndef _WIN32
    int out_fds[2] = {-1, -1};
    int err_fds[2] = {-1, -1};
    if (pipe2(out_fds, O_CLOEXEC) != 0) return result;
    if (pipe2(err_fds, O_CLOEXEC) != 0) {
        close(out_fds[0]);
        close(out_fds[1]);
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(out_fds[0]);
        close(out_fds[1]);
        close(err_fds[0]);
        close(err_fds[1]);
        return result;
    }
    if (pid == 0) {
        close(out_fds[0]);
        close(err_fds[0]);
        dup2(out_fds[1], STDOUT_FILENO);
        dup2(err_fds[1], STDERR_FILENO);
        close(out_fds[1]);
        close(err_fds[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.data()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(out_fds[1]);
    close(err_fds[1]);

    char buf[8192];
    bool out_open = true;
    bool err_open = true;
    while (out_open || err_open) {
        struct pollfd fds[2];
        nfds_t n = 0;
        if (out_open) {
            fds[n].fd = out_fds[0];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            ++n;
        }
        if (err_open) {
            fds[n].fd = err_fds[0];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            ++n;
        }
        const int rc = poll(fds, n, 1000);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;  // still running - a full sort can take a while
        for (nfds_t i = 0; i < n; ++i) {
            const bool is_out = fds[i].fd == out_fds[0];
            bool* open = is_out ? &out_open : &err_open;
            if ((fds[i].revents & (POLLIN | POLLHUP)) == 0) continue;
            const ssize_t got = read(fds[i].fd, buf, sizeof(buf));
            if (got > 0) {
                std::string* sink = is_out ? &result.out : &result.err;
                sink->append(buf, static_cast<size_t>(got));
            } else if (got == 0) {
                *open = false;
            } else if (errno != EINTR) {
                *open = false;
            }
        }
    }
    close(out_fds[0]);
    close(err_fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.ok = true;
    result.exit_code =
        WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
#endif
    return result;
}

fs::path make_scratch_dir(const fs::path& profile_dir) {
    std::error_code ec;
    fs::create_directories(profile_dir, ec);
    const fs::path dir = profile_dir / ".gmm_loot_tmp";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

}  // namespace

LootResult run_loot_sort(const LootRequest& request, LootProgressFn progress) {
    LootResult result;

    if (request.cli_path.empty() || !fs::is_regular_file(request.cli_path)) {
        result.error =
            "LOOT sorting is unavailable: gmm_lootcli was not built "
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
        masterlists = manager.ensure(request.game_id, request.masterlist_repo,
                                     &result.error);
    } else {
        masterlists.masterlist = manager.dir_for(request.game_id) / "masterlist.yaml";
        masterlists.prelude = manager.dir_for(request.game_id) / "prelude.yaml";
    }
    if (masterlists.masterlist.empty() || masterlists.prelude.empty()) {
        if (result.error.empty()) result.error = "masterlist unavailable";
        return result;
    }

    // 2. Scratch files: winning plugin paths in, sorted list + report out.
    const fs::path scratch = make_scratch_dir(request.profile_dir);
    const fs::path paths_file = scratch / kPluginPathsFile;
    const fs::path sorted_file = scratch / kSortedFile;
    const fs::path report_file = scratch / kReportFile;

    {
        std::ofstream out(paths_file);
        if (!out.is_open()) {
            result.error = "could not create " + paths_file.string();
            return result;
        }
        for (const auto& plugin : request.plugins) {
            if (plugin.full_path.empty()) continue;
            out << plugin.full_path.string() << "\n";
        }
    }

    // 3. Spawn the CLI and stream its protocol.
    std::vector<std::string> args = {
        request.cli_path.string(),
        "--game",        request.loot_game_id,
        "--gamePath",    request.game_dir.string(),
        "--localPath",   request.profile_dir.string(),
        "--pluginPathsFile", paths_file.string(),
        "--masterlist",  masterlists.masterlist.string(),
        "--prelude",     masterlists.prelude.string(),
        "--pluginListOutputPath", sorted_file.string(),
        "--out",         report_file.string(),
        "--logLevel",    "info",
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
            if (!line.empty() && line.back() == '\r') line.pop_back();
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
            reason = "gmm_lootcli exited with code " +
                     std::to_string(proc.exit_code);
        }
        if (!reason.empty() && reason.back() == '\n') reason.pop_back();
        result.error = "LOOT sort failed: " + reason;
        Logger::instance().warn("gmm_lootcli exit " +
                                std::to_string(proc.exit_code) + ": " + reason);
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
            if (line.empty() || line[0] == '#') continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            result.sorted_names.push_back(line);
        }
    }
    if (result.sorted_names.empty()) {
        result.error = "LOOT sort produced no output";
        return result;
    }

    result.ok = true;
    result.report_path = report_file;
    if (progress) progress(8, "");  // Done
    return result;
}

}  // namespace engine
