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

typedef struct {
    const char* id;
    const char* display_name;
    int32_t order;
    int enabled;
} GmmSwiftMod;

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
void gmm_swift_snapshot_destroy(GmmSwiftSnapshotHandle snapshot);
const char* gmm_swift_snapshot_instance_id(GmmSwiftSnapshotHandle snapshot);
const char* gmm_swift_snapshot_game_id(GmmSwiftSnapshotHandle snapshot);
const char* gmm_swift_snapshot_profile_id(GmmSwiftSnapshotHandle snapshot);
size_t gmm_swift_snapshot_mod_count(GmmSwiftSnapshotHandle snapshot);
GmmSwiftMod gmm_swift_snapshot_mod_at(GmmSwiftSnapshotHandle snapshot, size_t index);

GmmSwiftOperationHandle gmm_swift_operation_create(void);
void gmm_swift_operation_cancel(GmmSwiftOperationHandle operation);
int gmm_swift_operation_is_cancelled(GmmSwiftOperationHandle operation);
void gmm_swift_operation_destroy(GmmSwiftOperationHandle operation);

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
