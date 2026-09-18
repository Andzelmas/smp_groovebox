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

// message struct for the ui<->rt ring buffers - always "this param's value
// is now X", never an operation to replay. Whichever side (rt/ui)
// originates a change computes the final, already-clamped result itself
// (it has its own min_val/max_val) and sends just that - the receiving
// side stores it verbatim, no re-computation. val_id (position in the
// container) - never an owner_id, this never crosses into any module's own
// external id space (e.g. CLAP's clap_id).
// uid rides along too, as a safety check: val_id alone is only a position,
// and add/remove resync can reuse a freed slot for a DIFFERENT
// param. 
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
// parameter. No val_changed here - the ui side has no polled protocol consuming
// it once something wants to know "did this param's value change" from the ui
// side, that belongs on the push-based CX DATA_EVENT_CHANGED mechanism, not a
// stored flag here.
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
    // TODO instead of is hidden implement flag system for parameters
    // is the parameter hidden
    uint16_t is_hidden;
} PRM_PARAM_UI;

typedef struct _params_container {
    // the parameter arrays - each element is its OWN malloc'd PRM_PARAM_RT/UI.
    // That's deliberate: param_add_param grows these OUTER arrays with
    // realloc, which is only safe because they hold pointer VALUES. If the
    // params themselves lived in one realloc'd block, an address already
    // handed out could be silently invalidated the next time a parameter
    // gets added. rt_params should be touched only by the rt thread, and
    // the ui_params only by the ui thread.
    PRM_PARAM_RT **rt_params;
    PRM_PARAM_UI **ui_params;
    // how many parameters are there
    unsigned int num_of_params_ui;
    unsigned int num_of_params_rt;
    // ring buffers for parameter manipulation/communication
    RING_BUFFER *param_rt_to_ui;
    RING_BUFFER *param_ui_to_rt;
    PRM_CONT_USER_DATA user_data;
    // scratch buffer param_get_value_as_string formats into and returns a
    // pointer to - valid only until the next param_get_value_as_string call
    // on this container (any val_id)
    char value_string_scratch[MAX_STRING_MSG_LENGTH];
} PRM_CONTAIN;

// clamps val into [min_val, max_val] - shared by both sides, since both can
// originate a value change and each clamps against its own min/max before
// sending the final value across (see the "send final value" protocol on
// param_set_value_rt/param_set_value).
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

int param_add_param(PRM_CONTAIN *param_container, const char *name, PARAM_T val,
                    PARAM_T min, PARAM_T max, PARAM_T inc, uint32_t uid,
                    uint32_t owner_id, void *cookie) {
    if (!param_container)
        return -1;
    if (!name)
        return -1;
    // uid must stay unique within this container - fail loudly here rather
    // than silently corrupting the CX layer later (see param_get_uid's doc)
    if (param_find_uid(param_container, uid) != -1)
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
    // to make the parameter hidden send a param_set_value with
    // Operation_ToggleHidden and set_to 0 or 1
    new_ui_param->is_hidden = 0;

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
    }

    // pass 2: add anything not already alive via param_add_param (reuses a
    // freed slot if pass 1 left one). A survivor is left untouched -
    // value/name refresh is the owner's own job.
    for (unsigned int i = 0; i < new_count; i++) {
        if (param_find_uid(param_container, new_params[i].uid) != -1)
            continue;
        param_add_param(param_container, new_params[i].name, new_params[i].val,
                        new_params[i].min, new_params[i].max, new_params[i].inc,
                        new_params[i].uid, new_params[i].owner_id,
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
        // the value crossing the ring buffer is already final and clamped by
        // whichever side sent it (see param_set_value_rt/param_set_value) -
        // store it verbatim, nothing to recompute here. NULL/uid checks
        // reject a message targeting a freed or freed-and-reused slot.
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
    case Operation_ToggleHidden:
        if (set_to <= 0)
            cur_param->is_hidden = 0;
        if (set_to >= 1)
            cur_param->is_hidden = 1;
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
    // needed to resolve a uid it doesn't already have a val_id for. Skips
    // NULL (freed) slots.
    for (unsigned int i = 0; i < param_container->num_of_params_ui; i++) {
        if (!param_container->ui_params[i])
            continue;
        if (param_container->ui_params[i]->uid == uid)
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

const char *param_get_value_as_string(PRM_CONTAIN *param_container,
                                      int val_id, PARAM_T value) {
    if (!param_container)
        return NULL;
    // if there is no user provided function to convert the parameter value to
    // string, there is nothing to display
    if (!param_container->user_data.user_data ||
        !param_container->user_data.val_to_string)
        return NULL;

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

unsigned int param_is_hidden(PRM_CONTAIN *param_container, int val_id) {
    if (!param_container)
        return 0;
    if (val_id < 0 || (unsigned int)val_id >= param_container->num_of_params_ui)
        return 0;
    if (!param_container->ui_params[val_id])
        return 0;
    return param_container->ui_params[val_id]->is_hidden;
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

    free(param_container);
}
