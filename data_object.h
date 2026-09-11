#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "ids.h"
#include "data_actions.h"

// The contract shared by the data layer (app_data.c) and the context layer
// (app_intrf.c). The data layer describes each piece of program state as a
// DataObject: an opaque handle plus a static table of operations. The context
// layer never switches on a type tag, it only calls through the ops table.
//
// This header depends only on ids.h (also dependency-free), so the context/ui
// layers can be reused with a different data layer.

typedef struct DataObject DataObject;
typedef struct DataOps DataOps;

// What a DataObject is able to do. Kept as an explicit bitmask (rather than
// only relying on which function pointers are non NULL) so the context/ui
// layers can ask "is this capable of X" cheaply, and so "capable but currently
// empty" stays distinguishable from "not capable".
typedef enum {
    DATA_CAP_NAME = 1 << 0,
    DATA_CAP_CHILDREN = 1 << 1,
    // action_list/action_args/list_count/list_at/action_do - see
    // data_actions.h. Per-action availability is DataAction.enabled, so
    // there is no separate DATA_CAP_RENAME or similar per-action cap.
    DATA_CAP_ACTIONS = 1 << 2,
} DataCapabilities;

struct DataOps {
    // bitmask of DataCapabilities
    unsigned int capabilities;

    // MANDATORY (not capability-gated). Return this object's identity: stable
    // while the object lives, program-unique, and never reused for a different
    // object. Must not return CONTEXT_ID_NULL.
    ContextId (*id)(void *user_data);

    // DATA_CAP_CHILDREN
    // how many children this object currently has
    size_t (*child_count)(void *user_data);
    // fill *out with the child at idx. return false (and leave *out zeroed) if
    // idx is out of range.
    bool (*child_at)(void *user_data, size_t idx, DataObject *out);

    // DATA_CAP_NAME
    // return a display name for this object. The string is owned by the data
    // layer and stays valid while the object exists; callers that need to keep
    // it past that must copy. Navigation (main thread) use only. May return
    // NULL on error.
    const char *(*name)(void *user_data);

    // DATA_CAP_ACTIONS - see data_actions.h for the flow and struct docs.
    // fill *out with up to cap available actions for this object.
    size_t (*action_list)(void *user_data, DataAction *out, size_t cap);
    // fill *out with up to cap argument specs the given action needs.
    size_t (*action_args)(void *user_data, DataActionType type,
                          DataArgSpec *out, size_t cap);
    // how many options `list` currently has (list is a DataArgSpec.list value
    // returned by action_args - the caller forwards it unchanged). `partial`
    // is the request as filled in so far (earlier args may already carry a
    // value), so a list's content can depend on an earlier arg - e.g. a
    // CONNECT action's target list is filtered by the chosen source. May
    // return 0 to mean "unknown, page with list_at until it returns false"
    // instead of "empty".
    size_t (*list_count)(void *user_data, DataListId list,
                         const DataActionReq *partial);
    // fill *out with option idx of `list`. return false (and leave *out
    // zeroed) if idx is out of range.
    bool (*list_at)(void *user_data, DataListId list,
                    const DataActionReq *partial, size_t idx,
                    DataChoice *out);
    // execute the action. On success that creates a new object, *out_new is
    // set to its ContextId; otherwise *out_new is left untouched.
    DataActionResult (*action_do)(void *user_data, const DataActionReq *req,
                                  ContextId *out_new);
};

struct DataObject {
    // operation table for this object, like a vtable. One shared immutable
    // DataOps instance per data kind, so comparing ops by pointer identifies
    // the kind.
    const DataOps *ops;
    // opaque handle owned by the data layer. The context layer only borrows it
    // and must never free or modify it.
    void *user_data;
};

static inline bool data_obj_valid(const DataObject *obj) {
    return obj && obj->ops;
}

static inline bool data_obj_has(const DataObject *obj, DataCapabilities cap) {
    return data_obj_valid(obj) && (obj->ops->capabilities & (unsigned int)cap);
}

static inline size_t data_child_count(const DataObject *obj) {
    if (!data_obj_has(obj, DATA_CAP_CHILDREN) || !obj->ops->child_count)
        return 0;
    return obj->ops->child_count(obj->user_data);
}

static inline bool data_child_at(const DataObject *obj, size_t idx,
                                 DataObject *out) {
    if (!out)
        return false;
    out->ops = NULL;
    out->user_data = NULL;
    if (!data_obj_has(obj, DATA_CAP_CHILDREN) || !obj->ops->child_at)
        return false;
    return obj->ops->child_at(obj->user_data, idx, out);
}

static inline const char *data_name(const DataObject *obj) {
    if (!data_obj_has(obj, DATA_CAP_NAME) || !obj->ops->name)
        return NULL;
    return obj->ops->name(obj->user_data);
}

static inline ContextId data_id(const DataObject *obj) {
    if (!data_obj_valid(obj) || !obj->ops->id)
        return CONTEXT_ID_NULL;
    return obj->ops->id(obj->user_data);
}

static inline size_t data_action_list(const DataObject *obj, DataAction *out,
                                      size_t cap) {
    if (!data_obj_has(obj, DATA_CAP_ACTIONS) || !obj->ops->action_list)
        return 0;
    return obj->ops->action_list(obj->user_data, out, cap);
}

static inline size_t data_action_args(const DataObject *obj,
                                      DataActionType type, DataArgSpec *out,
                                      size_t cap) {
    if (!data_obj_has(obj, DATA_CAP_ACTIONS) || !obj->ops->action_args)
        return 0;
    return obj->ops->action_args(obj->user_data, type, out, cap);
}

static inline size_t data_list_count(const DataObject *obj, DataListId list,
                                     const DataActionReq *partial) {
    if (!data_obj_has(obj, DATA_CAP_ACTIONS) || !obj->ops->list_count)
        return 0;
    return obj->ops->list_count(obj->user_data, list, partial);
}

static inline bool data_list_at(const DataObject *obj, DataListId list,
                                const DataActionReq *partial, size_t idx,
                                DataChoice *out) {
    if (!out)
        return false;
    out->value = 0;
    out->label = NULL;
    out->flags = 0;
    if (!data_obj_has(obj, DATA_CAP_ACTIONS) || !obj->ops->list_at)
        return false;
    return obj->ops->list_at(obj->user_data, list, partial, idx, out);
}

static inline DataActionResult data_action_do(const DataObject *obj,
                                               const DataActionReq *req,
                                               ContextId *out_new) {
    if (out_new)
        *out_new = CONTEXT_ID_NULL;
    if (!data_obj_has(obj, DATA_CAP_ACTIONS) || !obj->ops->action_do || !req)
        return DATA_ACTION_ERR_INVALID;
    return obj->ops->action_do(obj->user_data, req, out_new);
}
