#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui_layer.h"
#include "ui_windowing.h"

// Reading and navigating contexts through ui_layer. Returns codes and data,
// never display text.
// Optional helper for ease of use: a UI can ignore it and call ui_layer directly.

typedef struct _ui_nav_context_info{
    const char *name; // borrowed
    size_t child_count;
    bool is_hidden; // hidden by the data layer
    const char *value; // borrowed, NULL when the context has no value
    uint32_t latency; // frames the context adds, 0 for none
} UI_NAV_CONTEXT_INFO;

// a context's children as a UI_WINDOWING_SOURCE: shown means visible
typedef struct _ui_nav_children{
    UI_LAYER *ui_layer;
    ContextId parent;
} UI_NAV_CHILDREN;

// false when context is not valid or has no name
bool ui_nav_context_info_get(UI_LAYER *ui_layer, ContextId context, UI_NAV_CONTEXT_INFO *info);

// a child is visible when ui_nav_context_info_get reads it and it is not
// hidden, so visible means "will be drawn". Reads only validity, name and
// hidden. out_child may be NULL
bool ui_nav_child_visible_at(UI_LAYER *ui_layer, ContextId parent, size_t idx,
                             ContextId *out_child);

// parent's children as a source. children is filled here and must outlive the
// returned source
UI_WINDOWING_SOURCE ui_nav_children_source(UI_LAYER *ui_layer, ContextId parent,
                                           UI_NAV_CHILDREN *children);

// A purpose is the (source, purpose) entry of a UI_STATE holding one context,
// e.g. the hovered child under its parent. Purpose values are the UI's own.

// CONTEXT_ID_INVALID when the entry is missing or empty
ContextId ui_nav_purpose_context(UI_STATE *state, ContextId source, UiPurpose purpose);

// make context the one saved for (source, purpose). false when the entry is
// missing or context is not valid
bool ui_nav_purpose_context_set(UI_LAYER *ui_layer, UI_STATE *state, ContextId source,
                                UiPurpose purpose, ContextId context);

// keep (parent, purpose) on a visible child of parent: the saved one while it
// is visible, else the nearest visible one. A missing entry is created with
// stale_policy for its context. Returns the child's index, parent's child count
// when there is no visible child
size_t ui_nav_purpose_context_ensure_visible(UI_LAYER *ui_layer, UI_STATE *state,
                                             ContextId parent, UiPurpose purpose,
                                             UiStalePolicy stale_policy);

// move (parent, purpose) steps visible children away from from_idx, as
// ui_windowing_step. Returns the child index it moved to, from_idx when it did
// not move
size_t ui_nav_purpose_step(UI_LAYER *ui_layer, UI_STATE *state, ContextId parent,
                           UiPurpose purpose, size_t from_idx, size_t steps, bool forward,
                           bool wrap);

// the current context is the one saved for (current_source, current_purpose).
// Enter makes its hovered_purpose child current when that child has children.
// Both return false when the current context did not change
bool ui_nav_context_enter(UI_LAYER *ui_layer, UI_STATE *state, ContextId current_source,
                          UiPurpose current_purpose, UiPurpose hovered_purpose);
// make the current context's parent current
bool ui_nav_context_exit(UI_LAYER *ui_layer, UI_STATE *state, ContextId current_source,
                         UiPurpose current_purpose);

// save the children of parent that slice holds under (source, purpose), so
// ui_layer_state_reconcile reports a change to any of them. Creates the entry,
// or resizes it when slice->capacity changed
bool ui_nav_shown_store(UI_LAYER *ui_layer, UI_STATE *state, ContextId source,
                        UiPurpose purpose, ContextId parent,
                        const UI_WINDOWING_SLICE *slice);
