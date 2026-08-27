#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

typedef uint64_t ContextId;
typedef uint32_t UiPurpose;

typedef struct _ui_navigation_entry UI_NAVIGATION_ENTRY;
typedef struct _ui_navigation UI_NAVIGATION;
// the UI_STATE is owned by user not ui_layer
// user can create many UI_STATEs and use them as different views
typedef struct _ui_state UI_STATE;
// ui_layer holds the context data
// this is owned by the ui_layer
typedef struct _ui_layer UI_LAYER;

// init the ui_layer struct
UI_LAYER* ui_layer_init();
// destroy the ui_layer and the given ui_states
void ui_layer_destroy(UI_LAYER* ui_layer, UI_STATE* states, size_t states_count);
// create a new UI_NAVIGATION_ENTRY on the UI_NAVIGATION on UI_STATE state
// UiPurpose is a user enum that the ui_layer knows nothing about
bool ui_layer_nav_set(UI_STATE* state, ContextId source, ContextId target, UiPurpose purpose);
// get the target on the UI_NAVIGATION on the state, given the context and purpose
bool ui_layer_nav_try_get(const UI_STATE* state, ContextId context, UiPurpose purpose, ContextId* target);
// remove a UI_NAVIGATION_ENTRY
bool ui_layer_nav_remove(UI_STATE* state, ContextId source, UiPurpose purpose);
// remove all UI_NAVIGATION_ENTRY entries on the state
void ui_layer_nav_clear(UI_STATE* state);
