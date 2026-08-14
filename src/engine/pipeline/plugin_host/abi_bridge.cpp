// ABI bridge - implements the opaque-handle accessor functions declared in
// gmm_abi_v1.h so plugin stage-claim callbacks can actually inspect and
// mutate the Mod / Instance / ConflictIndex / Profile they are given.
//
// Handles are raw pointers to the real engine objects: a GmmModHandle is a
// engine::Mod*, a GmmInstanceHandle is a engine::Instance*, etc.  The engine
// owns them for the lifetime of the callback - plugins must not cache them.
// Every accessor is null-safe: plugin code can run against a context that
// lacks an instance / conflict index / profile.

#include "gmm_abi_v1.h"

#include "engine/index/conflict_index.h"
#include "engine/core/instance/instance.h"
#include "engine/mod/model/mod.h"
#include "engine/mod/model/profile.h"

#include <string>

namespace engine {

// Thread-local scratch for string returns. The ABI contract is that the
// engine owns returned strings for the duration of the callback only.
static thread_local std::string g_tls_str;

static GmmModState to_gmm_state(ModState state) {
    switch (state) {
        case ModState::Downloaded: return GMM_MOD_DOWNLOADED;
        case ModState::Extracted:  return GMM_MOD_EXTRACTED;
        case ModState::Installed:  return GMM_MOD_INSTALLED;
        case ModState::Staged:     return GMM_MOD_STAGED;
        case ModState::Deployed:   return GMM_MOD_DEPLOYED;
    }
    return GMM_MOD_DOWNLOADED;
}

static InstanceKind to_instance_kind(GmmInstanceKind kind) {
    switch (kind) {
        case GMM_INSTANCE_MODS:      return InstanceKind::Mods;
        case GMM_INSTANCE_PROFILES:  return InstanceKind::Profiles;
        case GMM_INSTANCE_DOWNLOADS: return InstanceKind::Downloads;
        case GMM_INSTANCE_CACHE:     return InstanceKind::Cache;
        case GMM_INSTANCE_LOGS:      return InstanceKind::Logs;
        case GMM_INSTANCE_CONFIG:    return InstanceKind::Config;
    }
    return InstanceKind::Mods;
}

// Handles are opaque by ABI design; the engine-side representation is a raw
// pointer to the real object.  reinterpret_cast is required because the ABI
// struct types (GmmMod, GmmInstance, ...) are unrelated incomplete types.
static Mod* as_mod(GmmModHandle h) { return reinterpret_cast<Mod*>(h); }
static Instance* as_instance(GmmInstanceHandle h) { return reinterpret_cast<Instance*>(h); }
static ConflictIndex* as_conflict(GmmConflictIndexHandle h) { return reinterpret_cast<ConflictIndex*>(h); }
static Profile* as_profile(GmmProfileHandle h) { return reinterpret_cast<Profile*>(h); }

}  // namespace engine

using namespace engine;

extern "C" {

/* -- Mod API -- */

const char* gmm_mod_id(GmmModHandle h) {
    auto* mod = as_mod(h);
    if (!mod) return "";
    return mod->id.c_str();
}

const char* gmm_mod_name(GmmModHandle h) {
    auto* mod = as_mod(h);
    if (!mod) return "";
    return mod->name.c_str();
}

const char* gmm_mod_version(GmmModHandle h) {
    auto* mod = as_mod(h);
    if (!mod) return "";
    return mod->version.c_str();
}

GmmModState gmm_mod_state(GmmModHandle h) {
    auto* mod = as_mod(h);
    if (!mod) return GMM_MOD_DOWNLOADED;
    return to_gmm_state(mod->state);
}

void gmm_mod_set_state(GmmModHandle h, GmmModState state) {
    auto* mod = as_mod(h);
    if (!mod) return;
    switch (state) {
        case GMM_MOD_DOWNLOADED: mod->state = ModState::Downloaded; break;
        case GMM_MOD_EXTRACTED:  mod->state = ModState::Extracted;  break;
        case GMM_MOD_INSTALLED:  mod->state = ModState::Installed;  break;
        case GMM_MOD_STAGED:     mod->state = ModState::Staged;     break;
        case GMM_MOD_DEPLOYED:   mod->state = ModState::Deployed;   break;
    }
}

GmmModFile gmm_mod_file_at(GmmModHandle h, size_t index) {
    auto* mod = as_mod(h);
    if (!mod || index >= mod->files.size())
        return {nullptr, 0};
    return {mod->files[index].relative_path.c_str(), mod->files[index].size};
}

size_t gmm_mod_file_count(GmmModHandle h) {
    auto* mod = as_mod(h);
    return mod ? mod->files.size() : 0;
}

/* -- Instance API -- */

const char* gmm_instance_game_id(GmmInstanceHandle h) {
    auto* inst = as_instance(h);
    if (!inst) return "";
    return inst->info().game_id.c_str();
}

const char* gmm_instance_root(GmmInstanceHandle h) {
    auto* inst = as_instance(h);
    if (!inst) return "";
    g_tls_str = inst->info().root.string();
    return g_tls_str.c_str();
}

const char* gmm_instance_path_for(GmmInstanceHandle h, GmmInstanceKind kind) {
    auto* inst = as_instance(h);
    if (!inst) return "";
    g_tls_str = inst->path_for(to_instance_kind(kind)).string();
    return g_tls_str.c_str();
}

/* -- ConflictIndex API -- */

void gmm_conflict_add_file(GmmConflictIndexHandle h,
                           const char* relative_path,
                           const char* mod_id,
                           uint32_t priority) {
    auto* index = as_conflict(h);
    if (!index || !relative_path || !mod_id) return;
    index->add_file(relative_path, mod_id, priority);
}

void gmm_conflict_remove_mod(GmmConflictIndexHandle h, const char* mod_id) {
    auto* index = as_conflict(h);
    if (!index || !mod_id) return;
    index->remove_mod(mod_id);
}

const char* gmm_conflict_winner(GmmConflictIndexHandle h, const char* relative_path) {
    auto* index = as_conflict(h);
    if (!index || !relative_path) return "";
    g_tls_str = index->winner(relative_path);
    return g_tls_str.c_str();
}

/* -- Profile API -- */

void gmm_profile_add_mod(GmmProfileHandle h, const char* mod_id, int enabled) {
    auto* profile = as_profile(h);
    if (!profile || !mod_id) return;
    profile->add_mod(mod_id, enabled != 0);
}

void gmm_profile_remove_mod(GmmProfileHandle h, const char* mod_id) {
    auto* profile = as_profile(h);
    if (!profile || !mod_id) return;
    profile->remove_mod(mod_id);
}

void gmm_profile_set_enabled(GmmProfileHandle h, const char* mod_id, int enabled) {
    auto* profile = as_profile(h);
    if (!profile || !mod_id) return;
    profile->set_enabled(mod_id, enabled != 0);
}

void gmm_profile_move_mod(GmmProfileHandle h, const char* mod_id, uint32_t new_position) {
    auto* profile = as_profile(h);
    if (!profile || !mod_id) return;
    profile->move_mod(mod_id, new_position);
}

uint32_t gmm_profile_priority_of(GmmProfileHandle h, const char* mod_id) {
    auto* profile = as_profile(h);
    if (!profile || !mod_id) return 0;
    return profile->priority_of(mod_id);
}

}  // extern "C"
