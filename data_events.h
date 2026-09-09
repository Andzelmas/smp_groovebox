#pragma once
#include "ids.h"

// Notifications from the data layer to the context layer. The context layer
// drains these once per nav_update and reconciles its CX tree against them - it
// never walks the whole tree polling for changes.
//
// Depends only on ids.h, like data_object.h - part of the portable contract.

typedef enum {
    // the set of children under `id` changed (one added, removed, reordered).
    // the context layer re-syncs that node's child list by identity: it adds a
    // CX for each data child it does not have, frees the subtree of each CX
    // whose data child is gone, and leaves the rest untouched.
    DATA_EVENT_CHILDREN_CHANGED,
    // `id` itself changed in a non-structural way (its name or value). the
    // context/ui layers refresh presentation only - no tree surgery.
    DATA_EVENT_CHANGED,
} DataEventType;

typedef struct {
    DataEventType type;
    ContextId id;
} DataEvent;
