#include "params.h"
#include "../types.h"
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/ring_buffer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// max size for the ui<->rt parameter ring buffer messaging arrays
#define MAX_PARAM_RING_BUFFER_ARRAY_SIZE 1024

// message struct for the ui<->rt ring buffers - always "this param's value is
// now X", never an operation to replay. Whichever side (rt/ui) originates a
// change computes the final, already-clamped result itself val_id (position in
// the container) uid rides along too, as a safety check: val_id alone is only a
// position, and add/remove resync can reuse a freed slot for a DIFFERENT param.
typedef struct _params_ring_data_bit {
    int val_id;
    uint32_t uid;
    PARAM_T param_value;
} PARAM_RING_DATA_BIT;

// rt-side parameter - only what the audio thread actually touches. it only ever
// receives a final value to store (see param_msgs_process) or originates one
// itself (see param_set_value_rt)
typedef struct _params_param_rt {
    PARAM_T val;
    PARAM_T min_val;
    PARAM_T max_val;
    unsigned int val_changed;
    // owner-supplied stable identity, unique within this container - see
    // param_add_param's doc comment. Never a positional index.
    uint32_t uid;
    // owner-supplied secondary id, opaque to params.c
    uint32_t owner_id;
    // owner-supplied optional convenience pointer (e.g. a CLAP param cookie)
    //- borrowed, params.c never touches it. rt-only: nothing on the ui side
    // has ever needed a param's cookie.
    void *cookie;
} PRM_PARAM_RT;

// ui-side parameter - everything the ui thread needs to present and edit a
// parameter.
typedef struct _params_param_ui {
    PARAM_T val;
    PARAM_T min_val;
    PARAM_T max_val;
    PARAM_T def_val; // default value
    // how much to increase or decrease the parameter
    PARAM_T inc_am;
    // display name - can change during the lifetime of the program
    //(Operation_ChangeName), unlike uid which never does
    char name[MAX_SHORT_NAME_LENGTH];
    uint32_t uid;
    // owner-supplied secondary id, opaque to params.c
    uint32_t owner_id;
    // bitmask of enum paramFlags
    uint32_t flags;
    // which category this param sits in, 0 = none (see param_category_
    // intern). A uid, not an index - so it survives anything that
    // renumbers, and a re-categorise is a single field write.
    uint32_t category_uid;
    // set once by param_add_param - lets param_get_handle/param_handle_
    // resolve turn this struct's own address into an opaque (container,
    // val_id) handle without exposing PRM_PARAM_UI itself outside params.c
    PRM_CONTAIN *self_container;
    int self_val_id;
} PRM_PARAM_UI;

// one interned parameter category. Like PRM_PARAM_RT/UI each of these is its
// OWN malloc, for the same reason: the outer array is realloc'd on growth, and
// a handle handed out to the layer above must never move.
typedef struct _param_category {
    // set once at intern time - lets param_category_handle/param_category_
    // resolve turn this struct's own address into an opaque (container,
    // cat_id) handle, without exposing the struct itself outside params.c
    PRM_CONTAIN *self_container;
    int self_cat_id;
    uint32_t uid;
    // 0 = top level. The tree lives in THIS FIELD, not in the storage layout
    uint32_t parent_uid;
    // this category's own path segment only, never a whole path
    char name[MAX_CATEGORY_SEGMENT];
} PRM_PARAM_CATEGORY;

typedef struct _params_container {
    // the parameter arrays - each element is its OWN malloc'd PRM_PARAM_RT/UI.
    // That's deliberate: param_add_param grows these OUTER arrays with
    // realloc, which is only safe because they hold pointer VALUES. rt_params
    // should be touched only by the rt thread, and the ui_params only by the ui
    // thread.
    PRM_PARAM_RT **rt_params;
    PRM_PARAM_UI **ui_params;
    // how many parameters are there
    unsigned int num_of_params_ui;
    unsigned int num_of_params_rt;
    // ring buffers for parameter manipulation/communication
    RING_BUFFER *param_rt_to_ui;
    RING_BUFFER *param_ui_to_rt;
    PRM_CONT_USER_DATA user_data;
    // bumped whenever the CX tree shape changes: a param added or freed, or
    // one moved to another category (Operation_SetCategory). Never by a
    // value/name/flag change - see param_container_changed.
    // generation_polled is the value the last param_container_changed call
    // saw, so "changed since you last asked" needs no state in the caller.
    uint32_t generation;
    uint32_t generation_polled;
    // interned categories - own malloc each, outer array realloc'd (see
    // PRM_PARAM_CATEGORY). Never removed individually, so num_categories only
    // grows and every index below it is alive.
    PRM_PARAM_CATEGORY **categories;
    unsigned int num_categories;
    // scratch buffer param_get_value_as_string formats into and returns a
    // pointer to - valid only until the next param_get_value_as_string call
    // on this container (any val_id)
    char value_string_scratch[MAX_STRING_MSG_LENGTH];
} PRM_CONTAIN;

// clamps val into [min_val, max_val].
static PARAM_T param_clamp(PARAM_T val, PARAM_T min_val, PARAM_T max_val) {
    if (val < min_val)
        return min_val;
    if (val > max_val)
        return max_val;
    return val;
}

PRM_CONTAIN *
params_init_param_container(const PRM_CONT_USER_DATA *user_data_per_container) {
    PRM_CONTAIN *param_container = (PRM_CONTAIN *)malloc(sizeof(PRM_CONTAIN));
    if (!param_container)
        return NULL;
    param_container->param_rt_to_ui = NULL;
    param_container->param_ui_to_rt = NULL;
    param_container->num_of_params_rt = 0;
    param_container->num_of_params_ui = 0;
    param_container->rt_params = NULL;
    param_container->ui_params = NULL;
    param_container->generation = 0;
    param_container->generation_polled = 0;
    param_container->categories = NULL;
    param_container->num_categories = 0;
    param_container->user_data.user_data = NULL;
    param_container->user_data.build_value = NULL;
    param_container->user_data.val_to_string = NULL;
    if (user_data_per_container) {
        param_container->user_data.user_data =
            user_data_per_container->user_data;
        param_container->user_data.build_value =
            user_data_per_container->build_value;
        param_container->user_data.val_to_string =
            user_data_per_container->val_to_string;
    }

    param_container->param_rt_to_ui = ring_buffer_init(
        sizeof(PARAM_RING_DATA_BIT), MAX_PARAM_RING_BUFFER_ARRAY_SIZE);
    param_container->param_ui_to_rt = ring_buffer_init(
        sizeof(PARAM_RING_DATA_BIT), MAX_PARAM_RING_BUFFER_ARRAY_SIZE);
    if (!param_container->param_rt_to_ui || !param_container->param_ui_to_rt) {
        param_clean_param_container(param_container);
        return NULL;
    }

    return param_container;
}

// monotonic counter for every param's uid, across every container, never
// reset - so a uid identifies a param program-wide, not just within its own
// container. Only param_add_param touches this, and that is [main-thread] only
// (it mallocs).
static uint32_t next_param_uid = 0;

int param_add_param(PRM_CONTAIN *param_container, const char *name, PARAM_T val,
                    PARAM_T min, PARAM_T max, PARAM_T inc, uint32_t uid,
                    uint32_t owner_id, uint32_t flags, uint32_t category_uid,
                    void *cookie) {
    if (!param_container)
        return -1;
    if (!name)
        return -1;
    if (uid == 0) {
        // "genuinely new, mint one" - the owner matches survivors by its own
        // key and passes their existing uid back (see params_container_
        // resync), it never invents a number itself
        uid = ++next_param_uid;
    }
    // a caller-supplied uid must stay unique within this container - fail
    // loudly here rather than silently corrupting the CX layer later (see
    // param_get_uid's doc). A minted one is unique by construction.
    else if (param_find_uid(param_container, uid) != -1)
        return -1;

    PRM_PARAM_RT *new_rt_param = malloc(sizeof(PRM_PARAM_RT));
    if (!new_rt_param)
        return -1;
    PRM_PARAM_UI *new_ui_param = malloc(sizeof(PRM_PARAM_UI));
    if (!new_ui_param) {
        free(new_rt_param);
        return -1;
    }

    // reuse a freed slot before growing; ui_params is NULL at the same
    // index, so checking rt_params alone is enough
    int reuse_idx = -1;
    for (unsigned int i = 0; i < param_container->num_of_params_rt; i++) {
        if (!param_container->rt_params[i]) {
            reuse_idx = (int)i;
            break;
        }
    }

    unsigned int idx;
    if (reuse_idx != -1) {
        idx = (unsigned int)reuse_idx;
    } else {
        unsigned int new_count = param_container->num_of_params_rt + 1;
        // growing this outer pointer array is always safe to realloc - it
        // only ever moves pointer VALUES, never the structs they point at
        PRM_PARAM_RT **new_rt_array = realloc(
            param_container->rt_params, new_count * sizeof(PRM_PARAM_RT *));
        if (!new_rt_array) {
            free(new_rt_param);
            free(new_ui_param);
            return -1;
        }
        param_container->rt_params = new_rt_array;
        PRM_PARAM_UI **new_ui_array = realloc(
            param_container->ui_params, new_count * sizeof(PRM_PARAM_UI *));
        if (!new_ui_array) {
            free(new_rt_param);
            free(new_ui_param);
            return -1;
        }
        param_container->ui_params = new_ui_array;

        idx = param_container->num_of_params_rt;
        param_container->num_of_params_rt = new_count;
        param_container->num_of_params_ui = new_count;
    }

    param_container->rt_params[idx] = new_rt_param;
    param_container->ui_params[idx] = new_ui_param;

    new_rt_param->val = val;
    new_rt_param->min_val = min;
    new_rt_param->max_val = max;
    new_rt_param->val_changed = 0;
    new_rt_param->uid = uid;
    new_rt_param->owner_id = owner_id;
    new_rt_param->cookie = cookie;

    new_ui_param->val = val;
    new_ui_param->min_val = min;
    new_ui_param->max_val = max;
    new_ui_param->def_val = val;
    new_ui_param->inc_am = inc;
    snprintf(new_ui_param->name, MAX_SHORT_NAME_LENGTH, "%s", name);
    new_ui_param->uid = uid;
    new_ui_param->owner_id = owner_id;
    new_ui_param->flags = flags;
    new_ui_param->category_uid = category_uid;
    new_ui_param->self_container = param_container;
    new_ui_param->self_val_id = (int)idx;

    param_container->generation++;

    return (int)idx;
}

void params_container_resync(PRM_CONTAIN *param_container,
                             const PARAM_RESYNC_ITEM *new_params,
                             unsigned int new_count) {
    if (!param_container)
        return;
    if (!new_params && new_count > 0)
        return;

    // pass 1: free and NULL any alive slot whose uid is no longer in
    // new_params - never shift survivors 
    for (unsigned int val_id = 0; val_id < param_container->num_of_params_rt;
        val_id++) {
        if (!param_container->rt_params[val_id])
            continue;
        uint32_t cur_uid = param_container->rt_params[val_id]->uid;
        unsigned int still_present = 0;
        for (unsigned int i = 0; i < new_count; i++) {
            if (new_params[i].uid == cur_uid) {
                still_present = 1;
                break;
            }
        }
        if (still_present)
            continue;
        free(param_container->rt_params[val_id]);
        param_container->rt_params[val_id] = NULL;
        free(param_container->ui_params[val_id]);
        param_container->ui_params[val_id] = NULL;
        param_container->generation++;
    }

    // pass 2: add anything not already alive via param_add_param (reuses a
    // freed slot if pass 1 left one). A survivor is left untouched -
    // value/name refresh is the owner's own job. An item carrying uid 0
    // ("new, mint it") never matches here, since no live param can hold 0.
    for (unsigned int i = 0; i < new_count; i++) {
        if (param_find_uid(param_container, new_params[i].uid) != -1)
            continue;
        param_add_param(param_container, new_params[i].name, new_params[i].val,
                        new_params[i].min, new_params[i].max, new_params[i].inc,
                        new_params[i].uid, new_params[i].owner_id,
                        new_params[i].flags, new_params[i].category_uid,
                        new_params[i].cookie);
    }
}

void param_msgs_process(PRM_CONTAIN *param_container, unsigned int rt_params) {
    if (!param_container)
        return;

    RING_BUFFER *ring_buffer = NULL;
    if (!rt_params)
        ring_buffer = param_container->param_rt_to_ui;
    if (rt_params)
        ring_buffer = param_container->param_ui_to_rt;
    if (!ring_buffer)
        return;

    unsigned int cur_items = ring_buffer_return_items(ring_buffer);
    for (unsigned int i = 0; i < cur_items; i++) {
        PARAM_RING_DATA_BIT cur_bit;
        int read_buffer =
            ring_buffer_read(ring_buffer, &cur_bit, sizeof(cur_bit));
        if (read_buffer <= 0)
            continue;
        //  NULL/uid checks reject a message targeting a freed or
        //  freed-and-reused slot.
        if (rt_params) {
            if ((unsigned int)cur_bit.val_id >=
                param_container->num_of_params_rt)
                continue;
            PRM_PARAM_RT *cur_param =
                param_container->rt_params[cur_bit.val_id];
            if (!cur_param || cur_param->uid != cur_bit.uid)
                continue;
            cur_param->val = cur_bit.param_value;
            cur_param->val_changed = 1;
        } else {
            if ((unsigned int)cur_bit.val_id >=
                param_container->num_of_params_ui)
                continue;
            PRM_PARAM_UI *cur_param = param_container->ui_params[cur_bit.val_id];
            if (!cur_param || cur_param->uid != cur_bit.uid)
                continue;
            cur_param->val = cur_bit.param_value;
        }
    }
}

int param_set_value_rt(PRM_CONTAIN *param_container, int val_id,
                       PARAM_T set_to) {
    if (!param_container)
        return -1;
    if (isnan(set_to))
        return -1;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_rt)
        return -1;
    PRM_PARAM_RT *cur_param = param_container->rt_params[val_id];
    if (!cur_param)
        return -1;

    PARAM_T prev_val = cur_param->val;
    PARAM_T new_val =
        param_clamp(set_to, cur_param->min_val, cur_param->max_val);
    cur_param->val = new_val;
    // only send the change to the other side if the value actually changed
    if (new_val != prev_val) {
        cur_param->val_changed = 1;
        PARAM_RING_DATA_BIT send_bit;
        send_bit.val_id = val_id;
        send_bit.uid = cur_param->uid;
        send_bit.param_value = new_val;
        ring_buffer_write(param_container->param_rt_to_ui, &send_bit,
                          sizeof(send_bit));
    }
    return 0;
}

int param_set_value(PRM_CONTAIN *param_container, int val_id, PARAM_T set_to,
                    const char *set_string_to, unsigned char param_op) {
    if (!param_container)
        return -1;
    if (isnan(set_to))
        return -1;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return -1;
    PRM_PARAM_UI *cur_param = param_container->ui_params[val_id];
    if (!cur_param)
        return -1;

    // property-only operations - mutate ui-only fields, nothing to clamp or
    // propagate to the rt side, which has no matching fields
    switch (param_op) {
    case Operation_SetIncr:
        cur_param->inc_am = set_to;
        return 0;
    case Operation_SetDefValue:
        cur_param->def_val = set_to;
        return 0;
    case Operation_ChangeName:
        if (set_string_to)
            snprintf(cur_param->name, MAX_SHORT_NAME_LENGTH, "%s",
                     set_string_to);
        return 0;
    case Operation_SetFlags:
        cur_param->flags = (uint32_t)set_to;
        return 0;
    case Operation_SetCategory:
        // structural: the param hangs under a different parent afterwards, so
        // the generation has to move even though the param SET is unchanged.
        // Only when it actually differs - a rescan re-reporting the same
        // category must not trigger a resync.
        if (cur_param->category_uid != (uint32_t)set_to) {
            cur_param->category_uid = (uint32_t)set_to;
            param_container->generation++;
        }
        return 0;
    default:
        break;
    }

    // value operations - compute the new value, clamp it, and if it
    // actually changed send that final value to the rt side
    PARAM_T prev_val = cur_param->val;
    PARAM_T new_val = prev_val;
    switch (param_op) {
    case Operation_Decrease:
        new_val = prev_val - set_to * cur_param->inc_am;
        break;
    case Operation_Increase:
        new_val = prev_val + set_to * cur_param->inc_am;
        break;
    case Operation_SetValue:
        new_val = set_to;
        break;
    case Operation_DefValue:
        new_val = cur_param->def_val;
        break;
    default:
        return -1;
    }
    new_val = param_clamp(new_val, cur_param->min_val, cur_param->max_val);
    cur_param->val = new_val;
    if (new_val != prev_val) {
        PARAM_RING_DATA_BIT send_bit;
        send_bit.val_id = val_id;
        send_bit.uid = cur_param->uid;
        send_bit.param_value = new_val;
        ring_buffer_write(param_container->param_ui_to_rt, &send_bit,
                          sizeof(send_bit));
    }
    return 0;
}

void *param_cookie_return_rt(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return NULL;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_rt)
        return NULL;
    if (!param_container->rt_params[val_id])
        return NULL;
    return param_container->rt_params[val_id]->cookie;
}

uint32_t param_get_uid(PRM_CONTAIN *param_container, int val_id,
                       unsigned int rt_params) {
    if (!param_container)
        return 0;
    if (rt_params) {
        if (val_id < 0 ||
            (unsigned int)val_id >= param_container->num_of_params_rt)
            return 0;
        if (!param_container->rt_params[val_id])
            return 0;
        return param_container->rt_params[val_id]->uid;
    }
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return 0;
    if (!param_container->ui_params[val_id])
        return 0;
    return param_container->ui_params[val_id]->uid;
}

uint32_t param_get_owner_id(PRM_CONTAIN *param_container, int val_id,
                            unsigned int rt_params) {
    if (!param_container)
        return 0;
    if (rt_params) {
        if (val_id < 0 ||
            (unsigned int)val_id >= param_container->num_of_params_rt)
            return 0;
        if (!param_container->rt_params[val_id])
            return 0;
        return param_container->rt_params[val_id]->owner_id;
    }
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return 0;
    if (!param_container->ui_params[val_id])
        return 0;
    return param_container->ui_params[val_id]->owner_id;
}

int param_find_uid(PRM_CONTAIN *param_container, uint32_t uid) {
    if (!param_container)
        return -1;
    // uid is identical on both sides once param_add_param sets it - checking
    // the ui side alone is sufficient, nothing on the rt side has ever
    // needed to resolve a uid it doesn't already have a val_id for. 
    for (unsigned int i = 0; i < param_container->num_of_params_ui; i++) {
        if (!param_container->ui_params[i])
            continue;
        if (param_container->ui_params[i]->uid == uid)
            return (int)i;
    }
    return -1;
}

int param_find_owner_id(PRM_CONTAIN *param_container, uint32_t owner_id) {
    if (!param_container || owner_id == 0)
        return -1;
    for (unsigned int i = 0; i < param_container->num_of_params_ui; i++) {
        if (!param_container->ui_params[i])
            continue;
        if (param_container->ui_params[i]->owner_id == owner_id)
            return (int)i;
    }
    return -1;
}

PARAM_T param_get_increment(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return -1;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return -1;
    if (!param_container->ui_params[val_id])
        return -1;
    return param_container->ui_params[val_id]->inc_am;
}

PARAM_T param_get_min(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return -1;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return -1;
    if (!param_container->ui_params[val_id])
        return -1;
    return param_container->ui_params[val_id]->min_val;
}

PARAM_T param_get_max(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return -1;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return -1;
    if (!param_container->ui_params[val_id])
        return -1;
    return param_container->ui_params[val_id]->max_val;
}

PARAM_T param_get_value(PRM_CONTAIN *param_container, int val_id,
                        unsigned int rt_params) {
    if (!param_container)
        return -1;
    if (rt_params) {
        if (val_id < 0 ||
            (unsigned int)val_id >= param_container->num_of_params_rt)
            return -1;
        PRM_PARAM_RT *cur_param = param_container->rt_params[val_id];
        if (!cur_param)
            return -1;
        // when returning the value we mark this param as no longer changed -
        // only the rt side tracks this (see PRM_PARAM_RT's doc comment)
        cur_param->val_changed = 0;
        PARAM_T raw_val = cur_param->val;
        if (param_container->user_data.build_value)
            return param_container->user_data.build_value(
                param_container->user_data.user_data, val_id, raw_val,
                rt_params);
        return raw_val;
    }
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return -1;
    if (!param_container->ui_params[val_id])
        return -1;
    PARAM_T raw_val = param_container->ui_params[val_id]->val;
    if (param_container->user_data.build_value)
        return param_container->user_data.build_value(
            param_container->user_data.user_data, val_id, raw_val, rt_params);
    return raw_val;
}

// smallest decimal count d where inc*10^d is (within an epsilon) a whole
// number, capped at 6 - inc=1 -> 0 decimals, inc=0.001 -> 3.
static int param_decimals_for_inc(PARAM_T inc) {
    if (inc <= 0)
        return 2;
    PARAM_T abs_inc = fabs(inc);
    for (int d = 0; d <= 6; d++) {
        PARAM_T scaled = abs_inc * pow(10, d);
        if (fabs(scaled - round(scaled)) < 1e-6)
            return d;
    }
    return 6;
}

const char *param_get_value_as_string(PRM_CONTAIN *param_container,
                                      int val_id, PARAM_T value) {
    if (!param_container)
        return NULL;
    // no owner-supplied formatter - format the raw value as a plain number,
    // decimal count derived from this param's own increment
    if (!param_container->user_data.user_data ||
        !param_container->user_data.val_to_string) {
        if (val_id < 0 ||
            (unsigned int)val_id >= param_container->num_of_params_ui)
            return NULL;
        if (!param_container->ui_params[val_id])
            return NULL;
        int decimals =
            param_decimals_for_inc(param_container->ui_params[val_id]->inc_am);
        snprintf(param_container->value_string_scratch,
                sizeof(param_container->value_string_scratch), "%.*f",
                decimals, value);
        return param_container->value_string_scratch;
    }

    unsigned int wrote = param_container->user_data.val_to_string(
        param_container->user_data.user_data, val_id, value,
        param_container->value_string_scratch,
        sizeof(param_container->value_string_scratch));
    if (!wrote)
        return NULL;
    return param_container->value_string_scratch;
}

int param_get_if_changed_rt(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return -1;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_rt)
        return -1;
    if (!param_container->rt_params[val_id])
        return -1;
    return param_container->rt_params[val_id]->val_changed;
}

int param_get_if_any_changed_rt(PRM_CONTAIN *param_container) {
    if (!param_container)
        return -1;
    unsigned int num_params = param_container->num_of_params_rt;
    if (num_params == 0)
        return -1;
    for (unsigned int i = 0; i < num_params; i++) {
        if (!param_container->rt_params[i])
            continue;
        if (param_container->rt_params[i]->val_changed == 1)
            return 1;
    }
    return 0;
}

uint32_t param_get_flags(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return 0;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return 0;
    if (!param_container->ui_params[val_id])
        return 0;
    return param_container->ui_params[val_id]->flags;
}

unsigned int param_is_hidden(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return 0;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return 0;
    if (!param_container->ui_params[val_id])
        return 0;
    return (param_container->ui_params[val_id]->flags & PARAM_FLAG_HIDDEN) != 0;
}

unsigned int param_is_readonly(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return 0;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return 0;
    if (!param_container->ui_params[val_id])
        return 0;
    return (param_container->ui_params[val_id]->flags & PARAM_FLAG_READONLY) != 0;
}

unsigned int param_is_enum(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return 0;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return 0;
    if (!param_container->ui_params[val_id])
        return 0;
    return (param_container->ui_params[val_id]->flags & PARAM_FLAG_ENUM) != 0;
}

const char *param_get_name(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return NULL;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return NULL;
    if (!param_container->ui_params[val_id])
        return NULL;
    return param_container->ui_params[val_id]->name;
}


void *param_get_handle(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return NULL;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return NULL;
    if (!param_container->ui_params[val_id])
        return NULL;
    return (void *)param_container->ui_params[val_id];
}

bool param_handle_resolve(void *handle, PRM_CONTAIN **out_container,
                          int *out_val_id) {
    if (!handle)
        return false;
    PRM_PARAM_UI *ui = (PRM_PARAM_UI *)handle;
    if (out_container)
        *out_container = ui->self_container;
    if (out_val_id)
        *out_val_id = ui->self_val_id;
    return true;
}

// monotonic counter for every interned category's uid, across every
// container, never reset - so a category uid is unique program-wide the same
// way a param uid is. 
static uint32_t next_category_uid = 0;

uint32_t param_category_intern(PRM_CONTAIN *param_container,
                               uint32_t parent_uid, const char *name) {
    if (!param_container || !name || name[0] == '\0')
        return 0;
    for (unsigned int i = 0; i < param_container->num_categories; i++) {
        PRM_PARAM_CATEGORY *cat = param_container->categories[i];
        if (cat && cat->parent_uid == parent_uid &&
            strcmp(cat->name, name) == 0)
            return cat->uid;
    }

    unsigned int idx = param_container->num_categories;
    // growing this outer pointer array is safe to realloc for the same reason
    // rt_params/ui_params are - it only moves pointer VALUES
    PRM_PARAM_CATEGORY **grown =
        realloc(param_container->categories,
                (idx + 1) * sizeof(PRM_PARAM_CATEGORY *));
    if (!grown)
        return 0;
    param_container->categories = grown;

    PRM_PARAM_CATEGORY *cat = malloc(sizeof(PRM_PARAM_CATEGORY));
    if (!cat)
        return 0;
    snprintf(cat->name, MAX_CATEGORY_SEGMENT, "%s", name);
    cat->parent_uid = parent_uid;
    cat->uid = ++next_category_uid;
    cat->self_container = param_container;
    cat->self_cat_id = (int)idx;

    param_container->categories[idx] = cat;
    param_container->num_categories = idx + 1;
    // no generation bump here: a category nothing points at yet is not
    // enumerated, so nothing is visible until a param is added into it or
    // moved into it - and both of those bump it themselves
    return cat->uid;
}

unsigned int param_return_num_categories(PRM_CONTAIN *param_container) {
    if (!param_container)
        return 0;
    return param_container->num_categories;
}

// shared bounds check - categories are never freed individually, so a valid
// index always yields a live entity
static PRM_PARAM_CATEGORY *param_category_at(PRM_CONTAIN *param_container,
                                             int cat_id) {
    if (!param_container)
        return NULL;
    if (cat_id < 0 || (unsigned int)cat_id >= param_container->num_categories)
        return NULL;
    return param_container->categories[cat_id];
}

uint32_t param_category_uid(PRM_CONTAIN *param_container, int cat_id) {
    PRM_PARAM_CATEGORY *cat = param_category_at(param_container, cat_id);
    return cat ? cat->uid : 0;
}

uint32_t param_category_parent(PRM_CONTAIN *param_container, int cat_id) {
    PRM_PARAM_CATEGORY *cat = param_category_at(param_container, cat_id);
    return cat ? cat->parent_uid : 0;
}

const char *param_category_name(PRM_CONTAIN *param_container, int cat_id) {
    PRM_PARAM_CATEGORY *cat = param_category_at(param_container, cat_id);
    return cat ? cat->name : NULL;
}

int param_category_find_uid(PRM_CONTAIN *param_container, uint32_t uid) {
    if (!param_container || uid == 0)
        return -1;
    for (unsigned int i = 0; i < param_container->num_categories; i++) {
        PRM_PARAM_CATEGORY *cat = param_container->categories[i];
        if (cat && cat->uid == uid)
            return (int)i;
    }
    return -1;
}

void *param_category_handle(PRM_CONTAIN *param_container, int cat_id) {
    return (void *)param_category_at(param_container, cat_id);
}

bool param_category_resolve(void *handle, PRM_CONTAIN **out_container,
                            int *out_cat_id) {
    if (!handle)
        return false;
    PRM_PARAM_CATEGORY *cat = (PRM_PARAM_CATEGORY *)handle;
    if (out_container)
        *out_container = cat->self_container;
    if (out_cat_id)
        *out_cat_id = cat->self_cat_id;
    return true;
}

uint32_t param_get_category_uid(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return 0;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return 0;
    if (!param_container->ui_params[val_id])
        return 0;
    return param_container->ui_params[val_id]->category_uid;
}

bool param_container_changed(PRM_CONTAIN *param_container) {
    if (!param_container)
        return false;
    if (param_container->generation == param_container->generation_polled)
        return false;
    param_container->generation_polled = param_container->generation;
    return true;
}

unsigned int param_return_num_params(PRM_CONTAIN *param_container,
                                     unsigned int rt_params) {
    if (!param_container)
        return 0;
    if (rt_params)
        return param_container->num_of_params_rt;
    return param_container->num_of_params_ui;
}

void param_clean_param_container(PRM_CONTAIN *param_container) {
    if (!param_container)
        return;
    ring_buffer_clean(param_container->param_rt_to_ui);
    ring_buffer_clean(param_container->param_ui_to_rt);
    if (param_container->rt_params) {
        for (unsigned int i = 0; i < param_container->num_of_params_rt; i++)
            if (param_container->rt_params[i])
                free(param_container->rt_params[i]);
        free(param_container->rt_params);
    }
    if (param_container->ui_params) {
        for (unsigned int i = 0; i < param_container->num_of_params_ui; i++)
            if (param_container->ui_params[i])
                free(param_container->ui_params[i]);
        free(param_container->ui_params);
    }
    if (param_container->categories) {
        for (unsigned int i = 0; i < param_container->num_categories; i++)
            if (param_container->categories[i])
                free(param_container->categories[i]);
        free(param_container->categories);
    }

    free(param_container);
}
