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
// hidden, so visible means "will be drawn". out_child may be NULL
bool ui_nav_child_visible_at(UI_LAYER *ui_layer, ContextId parent, size_t idx,
                             ContextId *out_child);

// parent's children as a source. children is filled here and must outlive the
// returned source
UI_WINDOWING_SOURCE ui_nav_children_source(UI_LAYER *ui_layer, ContextId parent,
                                           UI_NAV_CHILDREN *children);
