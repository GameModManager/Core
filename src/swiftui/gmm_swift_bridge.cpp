#include "gmm_swift_bridge.h"

#include "engine/core/events/event_bus.h"
#include "engine/core/instance/instance.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/core/instance/toml_utils.h"
#include "engine/game/detect/mod_scanner.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/deploy/launch/launcher.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/profile/profile.h"
#include "engine/profile/profile_creation.h"
#include "engine/core/util/fs_utils.h"
#include "platform/macos/macos_platform.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct GmmSwiftOperation {
    std::atomic_bool cancelled{false};
    uint64_t generation = 0;
};

struct GmmSwiftSnapshot {
    std::string instance_id;
    std::string game_id;
    std::string profile_id;
    std::string game_dir;
    std::string instance_root;
    std::string mods_dir;
    std::string profiles_dir;
    uint32_t steam_appid = 0;
    struct Mod {
        std::string id;
        int order = 0;
        bool enabled = true;
    };
    std::vector<Mod> mods;
    std::vector<std::string> profiles;
    std::vector<std::string> executables;
};

struct GmmSwiftMutationResult {
    GmmSwiftResultCode code = GMM_SWIFT_RESULT_ERROR;
    std::string error;
    GmmSwiftSnapshotHandle snapshot = nullptr;
};

struct GmmSwiftLaunchResult {
    GmmSwiftResultCode code = GMM_SWIFT_RESULT_ERROR;
    std::string error;
    int64_t pid = -1;
    bool overlay_launched = false;
};

struct GmmSwiftEngine {
    engine::PluginLoader plugins;
    std::unique_ptr<engine::PlatformInterface> platform;
    std::vector<std::string> instances;
    std::mutex mutex;
    std::string error;
    std::vector<uint64_t> refresh_tokens;
    std::atomic_uint64_t latest_generation{0};
};

GmmSwiftSnapshotHandle make_snapshot(GmmSwiftEngine* engine, const char* instance_id,
                                     const char* profile_id, GmmSwiftOperationHandle operation);

namespace {
const char* copy_string(const std::string& value) {
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    if (!result) return nullptr;
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

void set_error(GmmSwiftEngine* engine, std::string message) {
    std::lock_guard lock(engine->mutex);
    engine->error = std::move(message);
}

bool cancelled(GmmSwiftOperationHandle operation) {
    return operation && operation->cancelled.load();
}

bool stale(GmmSwiftEngine* engine, GmmSwiftOperationHandle operation) {
    return operation && operation->generation < engine->latest_generation.load();
}

bool begin(GmmSwiftEngine* engine, GmmSwiftOperationHandle operation) {
    if (!operation) return true;
    auto latest = engine->latest_generation.load();
    while (operation->generation > latest &&
           !engine->latest_generation.compare_exchange_weak(latest, operation->generation)) {}
    return !cancelled(operation) && !stale(engine, operation);
}

GmmSwiftMutationResultHandle result(GmmSwiftResultCode code, std::string error = {},
                                    GmmSwiftSnapshotHandle snapshot = nullptr) {
    auto* value = new GmmSwiftMutationResult;
    value->code = code;
    value->error = std::move(error);
    value->snapshot = snapshot;
    return value;
}

std::filesystem::path profiles_dir(const engine::Instance& instance) {
    return instance.path_for(engine::InstanceKind::Profiles);
}

std::vector<std::string> known_mods(GmmSwiftEngine* engine, const engine::Instance& instance) {
    const auto mods = engine::ModScanner::scan_dir(engine->plugins.knowledge(),
                                                   instance.info().game_id,
                                                   instance.path_for(engine::InstanceKind::Mods));
    std::vector<std::string> ids;
    for (const auto& mod : mods) ids.push_back(mod.folder_name);
    return ids;
}

// Saved executables from instance.toml's `executables` array (inline tables
// with a "path" key, or legacy plain strings). Falls back to the game
// plugin's known-executables CSV on first launch — same seeding order as the
// Qt ExecControlsBar.
std::vector<std::string> instance_executables(const engine::Instance& instance,
                                              const engine::GameKnowledge& knowledge) {
    std::vector<std::string> paths;
    if (auto tbl = engine::parse_instance_toml(instance.toml_path())) {
        if (auto arr = (*tbl)["executables"].as_array()) {
            for (const auto& elem : *arr) {
                if (auto entry = elem.as_table()) {
                    if (auto value = entry->get("path")->value<std::string>())
                        paths.push_back(*value);
                } else if (auto value = elem.value<std::string>()) {
                    paths.push_back(*value);
                }
            }
        }
    }
    if (!paths.empty()) return paths;
    const auto csv = knowledge.get(instance.info().game_id, "executables", "");
    std::istringstream stream(csv);
    std::string token;
    while (std::getline(stream, token, ',')) {
        const auto first = token.find_first_not_of(" \t");
        const auto last = token.find_last_not_of(" \t");
        if (first != std::string::npos && last != std::string::npos)
            paths.push_back(token.substr(first, last - first + 1));
    }
    return paths;
}

GmmSwiftMutationResultHandle mutate(GmmSwiftEngine* engine, const char* instance_id,
                                     const char* profile_id, GmmSwiftOperationHandle operation,
                                     const std::function<bool(engine::Instance&, const std::string&, std::string&)>& action) {
    if (!engine) return result(GMM_SWIFT_RESULT_ERROR, "engine is required");
    if (!begin(engine, operation))
        return result(cancelled(operation) ? GMM_SWIFT_RESULT_CANCELLED : GMM_SWIFT_RESULT_STALE);
    try {
        const auto root = engine::resolve_instance_path(instance_id ? instance_id : "");
        if (root.empty()) return result(GMM_SWIFT_RESULT_ERROR, "instance was not found");
        auto instance = engine::Instance::from_root(root);
        if (!instance.read_toml() || instance.info().game_id.empty())
            return result(GMM_SWIFT_RESULT_ERROR, "instance.toml is invalid");
        std::string error;
        if (!action(instance, profile_id ? profile_id : "", error))
            return result(GMM_SWIFT_RESULT_ERROR, std::move(error));
        if (cancelled(operation) || stale(engine, operation))
            return result(cancelled(operation) ? GMM_SWIFT_RESULT_CANCELLED : GMM_SWIFT_RESULT_STALE);
        return result(GMM_SWIFT_RESULT_OK, {}, make_snapshot(engine, instance_id, profile_id, nullptr));
    } catch (const std::exception& error) {
        return result(GMM_SWIFT_RESULT_ERROR, error.what());
    }
}
}  // namespace

extern "C" GmmSwiftEngineHandle gmm_swift_engine_create(
    const char* instances_dir, const char* plugins_dir) {
    try {
        auto engine = std::make_unique<GmmSwiftEngine>();
        engine->platform = std::make_unique<engine::MacOSPlatform>();
        if (instances_dir && *instances_dir)
            engine::set_instances_dir_override(instances_dir);
        if (plugins_dir && *plugins_dir && fs::is_directory(plugins_dir) &&
            !engine->plugins.load_directory(plugins_dir)) {
            set_error(engine.get(), "one or more plugins failed to load");
        }
        engine->instances = engine::scan_instances();
        std::sort(engine->instances.begin(), engine->instances.end());
        return engine.release();
    } catch (const std::exception& error) {
        return nullptr;
    }
}

extern "C" void gmm_swift_engine_destroy(GmmSwiftEngineHandle engine) {
    if (!engine) return;
    for (const auto token : engine->refresh_tokens)
        engine::EventBus::instance().unsubscribe(token);
    delete engine;
}

extern "C" size_t gmm_swift_instance_count(GmmSwiftEngineHandle engine) {
    return engine ? engine->instances.size() : 0;
}

extern "C" const char* gmm_swift_instance_id(GmmSwiftEngineHandle engine,
                                              size_t index) {
    if (!engine || index >= engine->instances.size()) return nullptr;
    return engine->instances[index].c_str();
}

extern "C" GmmSwiftSnapshotHandle gmm_swift_snapshot_create(
    GmmSwiftEngineHandle engine, const char* instance_id,
    GmmSwiftOperationHandle operation) {
    return gmm_swift_snapshot_create_for_profile(engine, instance_id, nullptr, operation);
}

GmmSwiftSnapshotHandle make_snapshot(GmmSwiftEngine* engine, const char* instance_id,
                                     const char* requested_profile,
                                     GmmSwiftOperationHandle operation) {
    if (!engine || !instance_id || !*instance_id) return nullptr;
    try {
        if (!begin(engine, operation)) return nullptr;
        const auto root = engine::resolve_instance_path(instance_id);
        if (root.empty()) {
            set_error(engine, "instance was not found: " + std::string(instance_id));
            return nullptr;
        }
        auto instance = engine::Instance::from_root(root);
        if (!instance.read_toml() || instance.info().game_id.empty()) {
            set_error(engine, "instance.toml is invalid: " + root.string());
            return nullptr;
        }
        auto snapshot = std::make_unique<GmmSwiftSnapshot>();
        snapshot->instance_id = root.filename().string();
        snapshot->game_id = instance.info().game_id;
        snapshot->game_dir = instance.info().game_dir.string();
        snapshot->instance_root = root.string();
        snapshot->mods_dir = instance.path_for(engine::InstanceKind::Mods).string();
        snapshot->profiles_dir = instance.path_for(engine::InstanceKind::Profiles).string();
        snapshot->steam_appid = instance.info().steam_appid;
        snapshot->executables = instance_executables(instance, engine->plugins.knowledge());

        std::vector<std::string> profiles;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(
                 instance.path_for(engine::InstanceKind::Profiles), ec)) {
            if (entry.is_directory(ec)) profiles.push_back(entry.path().filename().string());
        }
        std::sort(profiles.begin(), profiles.end());
        snapshot->profiles = profiles;
        if (!profiles.empty()) {
            snapshot->profile_id = requested_profile && *requested_profile ? requested_profile : profiles.front();
            if (std::find(profiles.begin(), profiles.end(), snapshot->profile_id) == profiles.end()) {
                set_error(engine, "profile was not found: " + snapshot->profile_id);
                return nullptr;
            }
            engine::profile::Profile profile(
                instance.path_for(engine::InstanceKind::Profiles) / snapshot->profile_id);
            const auto known = known_mods(engine, instance);
            profile.refresh_mod_status(known, {}, false);
            for (const auto& mod : profile.mods())
                snapshot->mods.push_back({mod.mod_id, mod.priority, mod.enabled});
        }
        if (cancelled(operation)) return nullptr;
        return snapshot.release();
    } catch (const std::exception& error) {
        set_error(engine, error.what());
        return nullptr;
    }
}

extern "C" GmmSwiftSnapshotHandle gmm_swift_snapshot_create_for_profile(
    GmmSwiftEngineHandle engine, const char* instance_id, const char* profile_id,
    GmmSwiftOperationHandle operation) {
    return make_snapshot(engine, instance_id, profile_id, operation);
}

extern "C" void gmm_swift_snapshot_destroy(GmmSwiftSnapshotHandle snapshot) {
    delete snapshot;
}
extern "C" const char* gmm_swift_snapshot_instance_id(GmmSwiftSnapshotHandle s) { return s ? s->instance_id.c_str() : nullptr; }
extern "C" const char* gmm_swift_snapshot_game_id(GmmSwiftSnapshotHandle s) { return s ? s->game_id.c_str() : nullptr; }
extern "C" const char* gmm_swift_snapshot_profile_id(GmmSwiftSnapshotHandle s) { return s ? s->profile_id.c_str() : nullptr; }
extern "C" size_t gmm_swift_snapshot_mod_count(GmmSwiftSnapshotHandle s) { return s ? s->mods.size() : 0; }
extern "C" GmmSwiftMod gmm_swift_snapshot_mod_at(GmmSwiftSnapshotHandle s, size_t i) {
    if (!s || i >= s->mods.size()) return {nullptr, nullptr, 0, 0};
    const auto& mod = s->mods[i];
    return {mod.id.c_str(), mod.id.c_str(), mod.order, mod.enabled ? 1 : 0};
}

extern "C" size_t gmm_swift_snapshot_profile_count(GmmSwiftSnapshotHandle s) {
    return s ? s->profiles.size() : 0;
}
extern "C" const char* gmm_swift_snapshot_profile_at(GmmSwiftSnapshotHandle s, size_t i) {
    return s && i < s->profiles.size() ? s->profiles[i].c_str() : nullptr;
}

extern "C" const char* gmm_swift_snapshot_game_dir(GmmSwiftSnapshotHandle s) {
    return s && !s->game_dir.empty() ? s->game_dir.c_str() : nullptr;
}
extern "C" const char* gmm_swift_snapshot_instance_root(GmmSwiftSnapshotHandle s) {
    return s && !s->instance_root.empty() ? s->instance_root.c_str() : nullptr;
}
extern "C" const char* gmm_swift_snapshot_mods_dir(GmmSwiftSnapshotHandle s) {
    return s && !s->mods_dir.empty() ? s->mods_dir.c_str() : nullptr;
}
extern "C" const char* gmm_swift_snapshot_profiles_dir(GmmSwiftSnapshotHandle s) {
    return s && !s->profiles_dir.empty() ? s->profiles_dir.c_str() : nullptr;
}
extern "C" uint32_t gmm_swift_snapshot_steam_appid(GmmSwiftSnapshotHandle s) {
    return s ? s->steam_appid : 0;
}
extern "C" size_t gmm_swift_snapshot_executable_count(GmmSwiftSnapshotHandle s) {
    return s ? s->executables.size() : 0;
}
extern "C" const char* gmm_swift_snapshot_executable_at(GmmSwiftSnapshotHandle s, size_t i) {
    return s && i < s->executables.size() ? s->executables[i].c_str() : nullptr;
}

extern "C" GmmSwiftOperationHandle gmm_swift_operation_create(void) { return new GmmSwiftOperation; }
extern "C" GmmSwiftOperationHandle gmm_swift_operation_create_for_generation(uint64_t generation) {
    auto* operation = new GmmSwiftOperation;
    operation->generation = generation;
    return operation;
}
extern "C" void gmm_swift_operation_cancel(GmmSwiftOperationHandle o) { if (o) o->cancelled = true; }
extern "C" int gmm_swift_operation_is_cancelled(GmmSwiftOperationHandle o) { return cancelled(o) ? 1 : 0; }
extern "C" void gmm_swift_operation_destroy(GmmSwiftOperationHandle o) { delete o; }

extern "C" GmmSwiftResultCode gmm_swift_result_code(GmmSwiftMutationResultHandle r) {
    return r ? r->code : GMM_SWIFT_RESULT_ERROR;
}
extern "C" GmmSwiftSnapshotHandle gmm_swift_result_snapshot(GmmSwiftMutationResultHandle r) {
    return r ? r->snapshot : nullptr;
}
extern "C" const char* gmm_swift_result_error(GmmSwiftMutationResultHandle r) {
    return r && !r->error.empty() ? r->error.c_str() : nullptr;
}
extern "C" void gmm_swift_result_destroy(GmmSwiftMutationResultHandle r) {
    if (!r) return;
    gmm_swift_snapshot_destroy(r->snapshot);
    delete r;
}

extern "C" GmmSwiftMutationResultHandle gmm_swift_set_mod_enabled(
    GmmSwiftEngineHandle e, const char* instance_id, const char* profile_id,
    const char* mod_id, int enabled, GmmSwiftOperationHandle operation) {
    return mutate(e, instance_id, profile_id, operation,
                  [=](engine::Instance& instance, const std::string& profile_id, std::string& error) {
        if (!mod_id || !*mod_id || profile_id.empty()) { error = "mod and profile are required"; return false; }
        engine::profile::Profile profile(profiles_dir(instance) / profile_id);
        profile.refresh_mod_status(known_mods(e, instance), {}, false);
        if (profile.priority_of(mod_id) < 0) { error = "mod was not found"; return false; }
        profile.set_mod_enabled(mod_id, enabled != 0);
        profile.write_modlist_now();
        return true;
    });
}

extern "C" GmmSwiftMutationResultHandle gmm_swift_move_mod(
    GmmSwiftEngineHandle e, const char* instance_id, const char* profile_id,
    const char* mod_id, int32_t new_priority, GmmSwiftOperationHandle operation) {
    return mutate(e, instance_id, profile_id, operation,
                  [=](engine::Instance& instance, const std::string& profile_id, std::string& error) {
        if (!mod_id || !*mod_id || profile_id.empty()) { error = "mod and profile are required"; return false; }
        engine::profile::Profile profile(profiles_dir(instance) / profile_id);
        profile.refresh_mod_status(known_mods(e, instance), {}, false);
        if (profile.priority_of(mod_id) < 0) { error = "mod was not found"; return false; }
        // set_mod_priority clamps and is a no-op when the priority is
        // unchanged; both are fine — the refreshed snapshot reports the truth.
        profile.set_mod_priority(mod_id, new_priority);
        profile.write_modlist_now();
        return true;
    });
}

namespace {
// Shared plumbing for profile lifecycle mutations: same begin/cancel/stale
// handling as mutate(), but no modlist pre-check and the returned snapshot
// targets view_profile so the UI keeps its current selection.
GmmSwiftMutationResultHandle profile_mutate(
    GmmSwiftEngine* engine, const char* instance_id, const char* view_profile,
    GmmSwiftOperationHandle operation,
    const std::function<bool(engine::Instance&, std::string&)>& action) {
    if (!engine) return result(GMM_SWIFT_RESULT_ERROR, "engine is required");
    if (!begin(engine, operation))
        return result(cancelled(operation) ? GMM_SWIFT_RESULT_CANCELLED : GMM_SWIFT_RESULT_STALE);
    try {
        const auto root = engine::resolve_instance_path(instance_id ? instance_id : "");
        if (root.empty()) return result(GMM_SWIFT_RESULT_ERROR, "instance was not found");
        auto instance = engine::Instance::from_root(root);
        if (!instance.read_toml() || instance.info().game_id.empty())
            return result(GMM_SWIFT_RESULT_ERROR, "instance.toml is invalid");
        std::string error;
        if (!action(instance, error)) return result(GMM_SWIFT_RESULT_ERROR, std::move(error));
        if (cancelled(operation) || stale(engine, operation))
            return result(cancelled(operation) ? GMM_SWIFT_RESULT_CANCELLED : GMM_SWIFT_RESULT_STALE);
        return result(GMM_SWIFT_RESULT_OK, {},
                      make_snapshot(engine, instance_id, view_profile, nullptr));
    } catch (const std::exception& error) {
        return result(GMM_SWIFT_RESULT_ERROR, error.what());
    }
}
}  // namespace

extern "C" GmmSwiftMutationResultHandle gmm_swift_create_profile(
    GmmSwiftEngineHandle e, const char* instance_id, const char* name,
    const char* view_profile, GmmSwiftOperationHandle operation) {
    return profile_mutate(e, instance_id, view_profile, operation,
                          [=](engine::Instance& instance, std::string& error) {
            if (!name || !*name) { error = "profile name is required"; return false; }
            const auto created =
                engine::profile::create_fresh_profile(profiles_dir(instance), name);
            if (!created.success) { error = created.error; return false; }
            return true;
        });
}

extern "C" GmmSwiftMutationResultHandle gmm_swift_rename_profile(
    GmmSwiftEngineHandle e, const char* instance_id, const char* old_name,
    const char* new_name, const char* view_profile, GmmSwiftOperationHandle operation) {
    return profile_mutate(e, instance_id, view_profile, operation,
                          [=](engine::Instance& instance, std::string& error) {
            if (!old_name || !*old_name || !new_name || !*new_name) {
                error = "old and new profile names are required";
                return false;
            }
            if (!engine::profile::rename_profile(profiles_dir(instance), old_name,
                                                 new_name, &error)) {
                return false;
            }
            return true;
        });
}

extern "C" GmmSwiftMutationResultHandle gmm_swift_delete_profile(
    GmmSwiftEngineHandle e, const char* instance_id, const char* name, int is_active,
    const char* view_profile, GmmSwiftOperationHandle operation) {
    return profile_mutate(e, instance_id, view_profile, operation,
                          [=](engine::Instance& instance, std::string& error) {
            if (!name || !*name) { error = "profile name is required"; return false; }
            engine::profile::Profile profile(profiles_dir(instance) / name);
            switch (profile.remove(is_active != 0)) {
            case engine::profile::ProfileRemoveResult::Removed:
                return true;
            case engine::profile::ProfileRemoveResult::NotFound:
                error = "profile directory does not exist";
                return false;
            case engine::profile::ProfileRemoveResult::ActiveProfile:
                error = "the active profile cannot be deleted";
                return false;
            case engine::profile::ProfileRemoveResult::PermissionDenied:
                error = "could not delete the profile directory";
                return false;
            case engine::profile::ProfileRemoveResult::PartialFailure:
                error = "some profile contents were deleted, but the directory still exists";
                return false;
            }
            error = "could not delete the profile directory";
            return false;
        });
}

namespace {
GmmSwiftLaunchResultHandle launch_result(GmmSwiftResultCode code, std::string error = {},
                                         int64_t pid = -1, bool overlay = false) {
    auto* value = new GmmSwiftLaunchResult;
    value->code = code;
    value->error = std::move(error);
    value->pid = pid;
    value->overlay_launched = overlay;
    return value;
}
}  // namespace

extern "C" GmmSwiftLaunchResultHandle gmm_swift_launch(GmmSwiftEngineHandle engine,
                                                       const char* instance_id,
                                                       const char* executable,
                                                       GmmSwiftOperationHandle operation) {
    if (!engine) return launch_result(GMM_SWIFT_RESULT_ERROR, "engine is required");
    if (!begin(engine, operation))
        return launch_result(cancelled(operation) ? GMM_SWIFT_RESULT_CANCELLED : GMM_SWIFT_RESULT_STALE);
    try {
        if (!executable || !*executable)
            return launch_result(GMM_SWIFT_RESULT_ERROR, "executable is required");
        const auto root = engine::resolve_instance_path(instance_id ? instance_id : "");
        if (root.empty()) return launch_result(GMM_SWIFT_RESULT_ERROR, "instance was not found");
        auto instance = engine::Instance::from_root(root);
        if (!instance.read_toml() || instance.info().game_id.empty())
            return launch_result(GMM_SWIFT_RESULT_ERROR, "instance.toml is invalid");
        const auto info = instance.info();
        if (info.game_dir.empty() || !fs::is_directory(info.game_dir))
            return launch_result(GMM_SWIFT_RESULT_ERROR, "game directory was not found");

        // Canonical path parity with Qt/cli: full LaunchPrepRequest snapshot,
        // engine-side deploy + params assembly, then launch_game. No deploy
        // logic lives here.
        engine::LaunchPrepRequest req;
        req.instance_root = root;
        req.game_dir = info.game_dir;
        req.executable = executable;
        req.knowledge = engine->plugins.knowledge();
        req.game_id = info.game_id;
        req.steam_appid = info.steam_appid;
        const std::string extension = fs::path(executable).extension().string();
        req.is_windows_exe = extension == ".exe" || extension == ".EXE";
        req.local_saves_enabled = false;  // settings arrive with the Stage 6 slice
        req.platform = engine->platform.get();

        auto params = engine::prepare_launch_params(req);
        params.platform = engine->platform.get();
        // MO2-equivalent plugin order written right before launch (no-op for
        // games without plugin support).
        engine::PluginDatabase::write_plugins_txt_for_launch(
            info.game_dir, root, info.game_id, info.steam_appid,
            engine->plugins.knowledge(), engine->platform.get());

        const auto result = engine::launch_game(params);
        if (result.pid <= 0)
            return launch_result(GMM_SWIFT_RESULT_ERROR, "failed to launch the game");
        return launch_result(GMM_SWIFT_RESULT_OK, {}, result.pid, result.overlay_launched);
    } catch (const std::exception& error) {
        return launch_result(GMM_SWIFT_RESULT_ERROR, error.what());
    }
}

extern "C" GmmSwiftResultCode gmm_swift_launch_code(GmmSwiftLaunchResultHandle r) {
    return r ? r->code : GMM_SWIFT_RESULT_ERROR;
}
extern "C" int64_t gmm_swift_launch_pid(GmmSwiftLaunchResultHandle r) { return r ? r->pid : -1; }
extern "C" int gmm_swift_launch_overlay(GmmSwiftLaunchResultHandle r) {
    return r && r->overlay_launched ? 1 : 0;
}
extern "C" const char* gmm_swift_launch_error(GmmSwiftLaunchResultHandle r) {
    return r && !r->error.empty() ? r->error.c_str() : nullptr;
}
extern "C" void gmm_swift_launch_destroy(GmmSwiftLaunchResultHandle r) { delete r; }

extern "C" int gmm_swift_process_alive(int64_t pid) {
    if (pid <= 0) return 0;
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM ? 1 : 0;
}

extern "C" GmmSwiftMutationResultHandle gmm_swift_create_separator(
    GmmSwiftEngineHandle e, const char* instance_id, const char* name,
    GmmSwiftOperationHandle operation) {
    return profile_mutate(e, instance_id, nullptr, operation,
                          [=](engine::Instance& instance, std::string& error) {
            if (!name || !*name) { error = "separator name is required"; return false; }
            const auto sanitized = engine::sanitize_directory_name(name);
            if (sanitized.empty()) { error = "separator name is invalid"; return false; }
            const auto suffix = e->plugins.knowledge().get(
                instance.info().game_id, "separator_suffix", "_separator");
            const auto dir = instance.path_for(engine::InstanceKind::Mods) /
                             (sanitized + suffix);
            std::error_code ec;
            if (fs::exists(dir)) {
                error = "a separator named \"" + sanitized + "\" already exists";
                return false;
            }
            fs::create_directories(dir, ec);
            if (ec) { error = "failed to create the separator directory"; return false; }
            // No metadata file needed: the display name derives from the
            // folder name minus the separator suffix (Qt parity).
            return true;
        });
}

extern "C" GmmSwiftMutationResultHandle gmm_swift_apply_modlist(
    GmmSwiftEngineHandle e, const char* instance_id, const char* profile_id,
    const char* const* folders, size_t count, GmmSwiftOperationHandle operation) {
    return mutate(e, instance_id, profile_id, operation,
                  [=](engine::Instance& instance, const std::string& profile, std::string& error) {
            if (!folders || count == 0 || profile.empty()) {
                error = "profile and modlist entries are required";
                return false;
            }
            engine::profile::Profile prof(profiles_dir(instance) / profile);
            const auto known = known_mods(e, instance);
            prof.refresh_mod_status(known, {}, false);
            size_t applied = 0;
            for (size_t i = 0; i < count; ++i) {
                if (!folders[i] || !*folders[i]) continue;
                if (prof.priority_of(folders[i]) < 0) continue;  // unknown: skipped
                prof.set_mod_priority(folders[i], static_cast<int>(applied));
                ++applied;
            }
            if (applied == 0) { error = "no listed mods were found"; return false; }
            prof.write_modlist_now();
            return true;
        });
}

extern "C" const char* gmm_swift_last_error(GmmSwiftEngineHandle engine) {
    if (!engine) return nullptr;
    std::lock_guard lock(engine->mutex);
    return copy_string(engine->error);
}
extern "C" void gmm_swift_free_string(const char* value) { std::free(const_cast<char*>(value)); }

extern "C" uint64_t gmm_swift_subscribe_refresh(GmmSwiftEngineHandle engine,
                                                 GmmSwiftRefreshFn callback,
                                                 void* user_data) {
    if (!engine || !callback) return 0;
    const auto token = engine::EventBus::instance().subscribe(
        engine::events::kProfileChanged,
        [callback, user_data](const std::string& id, const std::string& payload) {
            const auto copied_id = id;
            const auto copied_payload = payload;
            callback(copied_id.c_str(), copied_payload.c_str(), user_data);
        });
    if (token) engine->refresh_tokens.push_back(token);
    return token;
}
extern "C" void gmm_swift_unsubscribe_refresh(GmmSwiftEngineHandle engine, uint64_t token) {
    if (engine && token) engine::EventBus::instance().unsubscribe(token);
}
