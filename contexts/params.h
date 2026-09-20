#pragma once
#include "../structs.h"
#include "../types.h"
#include <stdbool.h>
#include <stdint.h>
// operations param_set_value can apply to a parameter - the four value ops
// (Decrease/Increase/SetValue/DefValue) clamp and may propagate to the rt
// side; the three property ops (SetIncr/ChangeName/SetFlags) only ever
// mutate ui-only fields (see param_set_value's own doc comment).
enum paramOperType {
    Operation_Nothing = 0x00,
    Operation_Decrease = 0x01,
    Operation_Increase = 0x02,
    Operation_SetValue = 0x03,
    // set the value of the parameter to the default value
    Operation_DefValue = 0x04,
    // set the increment of the parameter to this value
    Operation_SetIncr = 0x05,
    // set the default value to this value
    Operation_SetDefValue = 0x06,
    // change the name of the parameter
    Operation_ChangeName = 0x07,
    // replace the whole flags bitmask (see enum paramFlags) with set_to
    Operation_SetFlags = 0x08
};

// per-parameter property bits, translated by the owner from its own flag
// source. Own numbering, independent of any owner's own flag values - more
// bits get added here as something actually needs them.
enum paramFlags {
    PARAM_FLAG_HIDDEN   = 1 << 0,
    PARAM_FLAG_READONLY = 1 << 1,
    PARAM_FLAG_ENUM     = 1 << 2,
};

// struct that holds the parameters
typedef struct _params_container PRM_CONTAIN;

// user data and functions supplied by the parameter's owner, per container.
// user_data is passed back to all callbacks unchanged.
// build_value: turns a param's raw stored value into the value param_get_
// value should return - curve mapping, interpolation, anything the owner
// wants. May be NULL, in which case param_get_value returns the raw value
// unchanged. rt_params tells the callback which side is asking, for owners
// whose transform differs by side (e.g. rt-only smoothing)
// val_to_string: writes a display string for a param's value - the owner
// decides the whole presentation (plain number, dB, True/False, a choice
// label, anything). May be NULL, in which case param_get_value_as_string
// writes nothing and returns 0. Use only on [main-thread] 
typedef struct _params_cont_user_data {
    void *user_data;
    PARAM_T (*build_value)(const void *user_data, int val_id, PARAM_T raw_val,
                           unsigned int rt_params);
    unsigned int (*val_to_string)(const void *user_data, int val_id,
                                  PARAM_T value, char *ret_string,
                                  uint32_t string_len);
} PRM_CONT_USER_DATA;

// initializes an empty parameter container (0 params) with the given owner
// callbacks - add parameters to it with param_add_param.
PRM_CONTAIN *
params_init_param_container(const PRM_CONT_USER_DATA *user_data_per_container);

// appends one parameter to the container. Pass uid 0 for a genuinely new
// parameter and params.c mints one: unique program-wide (not merely within this
// container) Pass a non-zero uid only to re-add a parameter that already has
// one - in practice only params_container_resync does that, for a survivor.
// Fails (returns -1) if a caller-supplied uid already exists in this container
// (checked via param_find_uid) or on allocation failure. name is
// copied immediately, not borrowed past this call. owner_id is a second,
// separate identifier, opaque to params.c - for an owner with its own external
// id space for this param (e.g. CLAP's clap_id)
// cookie is optional convenience storage for the owner (e.g. a CLAP param's
// cookie) - may be NULL, and is only ever readable from the rt side (see
// param_cookie_return_rt). flags is this param's initial paramFlags bitmask
// (0 if the owner has no flag source). Reuses a freed slot if one exists,
// else grows the container. Returns the new val_id (same index on both the
// rt and ui side) on success.
int param_add_param(PRM_CONTAIN *param_container, const char *name, PARAM_T val,
                    PARAM_T min, PARAM_T max, PARAM_T inc, uint32_t uid,
                    uint32_t owner_id, uint32_t flags, void *cookie);

// one parameter as the owner currently sees it, for params_container_resync.
// name is copied, doesn't need to outlive the call. uid: for a survivor,
// pass its EXISTING uid (via param_find_uid/param_get_uid) - resync matches
// by uid only, so a wrong value here drops the survivor instead of matching
// it. For a genuinely new param pass 0 and params.c mints one (see param_
// add_param) - the owner does the matching, since only it knows what "the
// same param" means for its own id space. flags only takes effect for a
// genuinely new param - a survivor's flags are left untouched by resync (see
// params_container_ resync's own doc comment), reconciling those is the owner's
// own job.
typedef struct _params_resync_item {
    char name[MAX_SHORT_NAME_LENGTH];
    PARAM_T val;
    PARAM_T min;
    PARAM_T max;
    PARAM_T inc;
    uint32_t uid;
    uint32_t owner_id;
    uint32_t flags;
    void *cookie;
} PARAM_RESYNC_ITEM;

// reconciles the container against new_params[], the owner's current full
// param list, matched by uid, not position:
//   - existing val_id whose uid is missing from new_params -> removed
//     (freed, slot set to NULL, never shifted
//   - new_params[] entry with no existing match -> added via param_add_
//     param.
//   - new_params[] entry that already exists -> left untouched (a
//     survivor); value/name refresh is the owner's own job.
// Only safe to call while nothing is concurrently reading/writing this
// container from the rt side.
void params_container_resync(PRM_CONTAIN *param_container,
                             const PARAM_RESYNC_ITEM *new_params,
                             unsigned int new_count);

// process ring_buffers - apply value messages that crossed from the other
// side. Each message is already the final, clamped value (see
// PARAM_RING_DATA_BIT) - this just stores it, no operation is replayed.
void param_msgs_process(PRM_CONTAIN *param_container, unsigned int rt_params);

// rt-side value setter. Always a direct "set to this value" if the clamped
// result actually changed, sends that final value across to the ui side.
// Returns -1 on error.
int param_set_value_rt(PRM_CONTAIN *param_container, int val_id,
                       PARAM_T set_to);

// ui-side value/property setter. param_op is a paramOperType (above) to
// apply - unlike the rt side, every operation is meaningful here, since the
// ui struct holds inc_am/def_val/name/is_hidden. The four value operations
//(Increase/Decrease/SetValue/DefValue) clamp and, if the result changed,
// propagate the final value to the rt side; Returns -1 on error.
int param_set_value(PRM_CONTAIN *param_container, int val_id, PARAM_T set_to,
                    const char *set_string_to, unsigned char param_op);

// return the cookie stored for this param (see param_add_param) - rt-side
// only, NULL if none/on error.
void *param_cookie_return_rt(PRM_CONTAIN *param_container, int val_id);

// return this param's owner-supplied uid (see param_add_param) for whichever
// side rt_params selects, 0 on error.
uint32_t param_get_uid(PRM_CONTAIN *param_container, int val_id,
                       unsigned int rt_params);

// find the val_id whose uid matches on the ui side, -1 if not found. ui-only
//- nothing on the rt side has ever needed to resolve a uid it doesn't
// already have a val_id for.
int param_find_uid(PRM_CONTAIN *param_container, uint32_t uid);

// return this param's owner_id (see param_add_param) for whichever side
// rt_params selects, 0 on error. Opaque to params.c - meaningless without
// knowing what the owner put there (e.g. CLAP's clap_id). 
uint32_t param_get_owner_id(PRM_CONTAIN *param_container, int val_id,
                            unsigned int rt_params);

// return the parameter increment amount (by how much the parameter value
// increases or decreases per Operation_Increase/Decrease) - ui-only, the rt
// struct has no inc_am.
PARAM_T param_get_increment(PRM_CONTAIN *param_container, int val_id);

// return the parameter's min/max range - ui-only, mirrors param_get_
// increment. -1 on error (a real min/max of -1 is indistinguishable from
// that; callers needing to tell them apart should check val_id validity
// themselves first).
PARAM_T param_get_min(PRM_CONTAIN *param_container, int val_id);
PARAM_T param_get_max(PRM_CONTAIN *param_container, int val_id);

// get the parameter value for whichever side rt_params selects, run through
// the owner's build_value callback if one is registered (see
// PRM_CONT_USER_DATA), otherwise the raw stored value. On the rt side this
// also clears that side's "changed since last read" tracking (see
// param_get_if_changed_rt) - the ui side has no such tracking to clear.
PARAM_T param_get_value(PRM_CONTAIN *param_container, int val_id,
                        unsigned int rt_params);

// return a display string for `value` on this param, via the owner's
// val_to_string callback. value is caller-supplied, not read from the
// param's current state - pass param_get_value(container, val_id, 0) for
// "what does this param currently show", or any other value to preview a
// hypothetical one. Returns NULL if no callback is registered or on error.
// The string is owned by the container and valid only until the next
// param_get_value_as_string call on it (any val_id) - callers that need to
// keep it must copy on [main-thread] - there is no rt variant.
const char *param_get_value_as_string(PRM_CONTAIN *param_container,
                                      int val_id, PARAM_T value);

// check if this param's value changed since param_get_value(..., 1) last
// read it - rt-only, this tracking only exists on the rt side. Used to
// decide whether an external protocol (a CLAP/LV2 event, a JACK transport
// update) still needs to be told about this param.
int param_get_if_changed_rt(PRM_CONTAIN *param_container, int val_id);
// check if any parameter's rt-side value has changed - see
// param_get_if_changed_rt.
int param_get_if_any_changed_rt(PRM_CONTAIN *param_container);

// return the parameter's whole flags bitmask (see enum paramFlags), 0 on
// error - ui-only.
uint32_t param_get_flags(PRM_CONTAIN *param_container, int val_id);

// get if parameter is hidden or not (PARAM_FLAG_HIDDEN) - ui-only, the rt
// struct has no flags.
unsigned int param_is_hidden(PRM_CONTAIN *param_container, int val_id);

// get if the parameter can't be changed (PARAM_FLAG_READONLY) - ui-only,
// mirrors param_is_hidden.
unsigned int param_is_readonly(PRM_CONTAIN *param_container, int val_id);

// get if the parameter represents an enumerated value (PARAM_FLAG_ENUM) -
// ui-only, mirrors param_is_hidden.
unsigned int param_is_enum(PRM_CONTAIN *param_container, int val_id);

// get the parameter's current display name - ui-only, the rt struct has no
// name. The string is owned by the param and stays valid while it exists
// (renamed by Operation_ChangeName, never freed out from under a caller);
// callers that need to keep it past that must copy,  Returns NULL on error. Use
// only on [main-thread].
const char *param_get_name(PRM_CONTAIN *param_container, int val_id);

// opaque per-param handle, stable for exactly the param's own lifetime (same
// underlying storage as the param itself. NULL on error. ui-only.
void *param_get_handle(PRM_CONTAIN *param_container, int val_id);

// resolve a handle from param_get_handle back to its container/val_id.
// Returns false (leaving the out-params untouched) if handle is NULL.
bool param_handle_resolve(void *handle, PRM_CONTAIN **out_container,
                          int *out_val_id);

// return how many parameters are on the container, for whichever side
// rt_params selects (both sides always hold the same count - param_add_param
// grows them together
unsigned int param_return_num_params(PRM_CONTAIN *param_container,
                                     unsigned int rt_params);

// cleans the parameter container
void param_clean_param_container(PRM_CONTAIN *param_container);
