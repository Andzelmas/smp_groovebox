#pragma once
#include "ids.h"
#include <stddef.h>
#include <stdint.h>

// Notifications from the context layer (app_intrf) to the ui layer.
//
// Unlike the data->context DataEvent stream (one consumer, fully drained every
// nav_update), this is a sequenced log with one caller-owned NavCursor per ui
// view, so several independent views can each read every change at their own
// pace. Depends only on ids.h - part of the portable contract.

typedef enum {
    // a context appeared (id, under parent, at index).
    CX_ADDED,
    // a context and its whole subtree are gone. id is already dead by the time
    // this is read; parent/index are its position from just before removal, so
    // a view can re-anchor a cursor to a surviving sibling.
    CX_REMOVED,
    // a context changed in a non-structural way (its name or value). refresh
    // presentation; nothing structural.
    CX_CHANGED,
} CxEventType;

typedef struct {
    CxEventType type;
    ContextId id;
    ContextId parent; // CONTEXT_ID_NULL for the root, or when unknown
    size_t index;     // id's index among its parent's children (pre-removal)
} CxEvent;

// A ui view's read position in the context layer's event log. Zero it and pass
// it to nav_cursor_init() before first use.
typedef struct {
    uint64_t next_seq;
} NavCursor;

typedef enum {
    NAV_POLL_EVENT,    // *out was filled and the cursor advanced
    NAV_POLL_EMPTY,    // caught up, nothing to read
    NAV_POLL_OVERFLOW, // the cursor fell behind the log and was reset to the
                       // head; the caller should rebuild the view from scratch
} NavPollResult;
