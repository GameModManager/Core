#ifndef GMM_SWIFT_BRIDGE_H
#define GMM_SWIFT_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GmmSwiftEngine* GmmSwiftEngineHandle;
typedef struct GmmSwiftSnapshot* GmmSwiftSnapshotHandle;
typedef struct GmmSwiftOperation* GmmSwiftOperationHandle;
typedef struct GmmSwiftMutationResult* GmmSwiftMutationResultHandle;

typedef struct {
    const char* id;
    const char* display_name;
    int32_t order;
    int enabled;
} GmmSwiftMod;

typedef enum {
    GMM_SWIFT_RESULT_OK = 0,
    GMM_SWIFT_RESULT_CANCELLED = 1,
    GMM_SWIFT_RESULT_STALE = 2,
    GMM_SWIFT_RESULT_ERROR = 3,
} GmmSwiftResultCode;

typedef void (*GmmSwiftRefreshFn)(const char* event_id, const char* payload,
                                  void* user_data);

GmmSwiftEngineHandle gmm_swift_engine_create(const char* instances_dir,
                                             const char* plugins_dir);
void gmm_swift_engine_destroy(GmmSwiftEngineHandle engine);

size_t gmm_swift_instance_count(GmmSwiftEngineHandle engine);
const char* gmm_swift_instance_id(GmmSwiftEngineHandle engine, size_t index);

GmmSwiftSnapshotHandle gmm_swift_snapshot_create(GmmSwiftEngineHandle engine,
                                                 const char* instance_id,
                                                 GmmSwiftOperationHandle operation);
GmmSwiftSnapshotHandle gmm_swift_snapshot_create_for_profile(
    GmmSwiftEngineHandle engine, const char* instance_id, const char* profile_id,
    GmmSwiftOperationHandle operation);
void gmm_swift_snapshot_destroy(GmmSwiftSnapshotHandle snapshot);
const char* gmm_swift_snapshot_instance_id(GmmSwiftSnapshotHandle snapshot);
const char* gmm_swift_snapshot_game_id(GmmSwiftSnapshotHandle snapshot);
const char* gmm_swift_snapshot_profile_id(GmmSwiftSnapshotHandle snapshot);
size_t gmm_swift_snapshot_mod_count(GmmSwiftSnapshotHandle snapshot);
GmmSwiftMod gmm_swift_snapshot_mod_at(GmmSwiftSnapshotHandle snapshot, size_t index);
size_t gmm_swift_snapshot_profile_count(GmmSwiftSnapshotHandle snapshot);
const char* gmm_swift_snapshot_profile_at(GmmSwiftSnapshotHandle snapshot, size_t index);

GmmSwiftOperationHandle gmm_swift_operation_create(void);
GmmSwiftOperationHandle gmm_swift_operation_create_for_generation(uint64_t generation);
void gmm_swift_operation_cancel(GmmSwiftOperationHandle operation);
int gmm_swift_operation_is_cancelled(GmmSwiftOperationHandle operation);
void gmm_swift_operation_destroy(GmmSwiftOperationHandle operation);

GmmSwiftResultCode gmm_swift_result_code(GmmSwiftMutationResultHandle result);
GmmSwiftSnapshotHandle gmm_swift_result_snapshot(GmmSwiftMutationResultHandle result);
const char* gmm_swift_result_error(GmmSwiftMutationResultHandle result);
void gmm_swift_result_destroy(GmmSwiftMutationResultHandle result);

GmmSwiftMutationResultHandle gmm_swift_set_mod_enabled(
    GmmSwiftEngineHandle engine, const char* instance_id, const char* profile_id,
    const char* mod_id, int enabled, GmmSwiftOperationHandle operation);

GmmSwiftMutationResultHandle gmm_swift_move_mod(
    GmmSwiftEngineHandle engine, const char* instance_id, const char* profile_id,
    const char* mod_id, int32_t new_priority, GmmSwiftOperationHandle operation);

// Profile lifecycle mutations (MO2 parity: the caller refuses rename/delete
// of the viewed profile in the UI; delete additionally takes is_active as
// defense in depth). view_profile names the profile the browser is showing —
// the returned snapshot re-reads it so the UI keeps its selection.
GmmSwiftMutationResultHandle gmm_swift_create_profile(
    GmmSwiftEngineHandle engine, const char* instance_id, const char* name,
    const char* view_profile, GmmSwiftOperationHandle operation);

GmmSwiftMutationResultHandle gmm_swift_rename_profile(
    GmmSwiftEngineHandle engine, const char* instance_id, const char* old_name,
    const char* new_name, const char* view_profile,
    GmmSwiftOperationHandle operation);

GmmSwiftMutationResultHandle gmm_swift_delete_profile(
    GmmSwiftEngineHandle engine, const char* instance_id, const char* name,
    int is_active, const char* view_profile, GmmSwiftOperationHandle operation);

const char* gmm_swift_last_error(GmmSwiftEngineHandle engine);
void gmm_swift_free_string(const char* value);

uint64_t gmm_swift_subscribe_refresh(GmmSwiftEngineHandle engine,
                                     GmmSwiftRefreshFn callback,
                                     void* user_data);
void gmm_swift_unsubscribe_refresh(GmmSwiftEngineHandle engine, uint64_t token);

#ifdef __cplusplus
}
#endif

#endif
