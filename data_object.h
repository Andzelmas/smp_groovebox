#pragma once
#include <stdbool.h>
#include <stddef.h>

// The contract shared by the data layer (app_data.c) and the context layer
// (app_intrf.c). The data layer describes each piece of program state as a
// DataObject: an opaque handle plus a static table of operations. The context
// layer never switches on a type tag, it only calls through the ops table.
//
// This header has no dependency on the rest of the program on purpose, so the
// context/ui layers can be reused with a different data layer.

typedef struct DataObject DataObject;
typedef struct DataOps DataOps;

// What a DataObject is able to do. Kept as an explicit bitmask (rather than
// only relying on which function pointers are non NULL) so the context/ui
// layers can ask "is this capable of X" cheaply, and so "capable but currently
// empty" stays distinguishable from "not capable".
typedef enum {
    DATA_CAP_NAME = 1 << 0,
    DATA_CAP_CHILDREN = 1 << 1,
    // reserved, not backed by ops yet:
    DATA_CAP_ACTIONS = 1 << 2,
    DATA_CAP_RENAME = 1 << 3,
} DataCapabilities;

struct DataOps {
    // bitmask of DataCapabilities
    unsigned int capabilities;

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
