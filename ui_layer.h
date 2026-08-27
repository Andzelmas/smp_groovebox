#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

typedef uint64_t ContextId;
typedef uint32_t UiPurpose;

// the UI_STATE is owned by user not ui_layer
// user can create many UI_STATEs and use them as different views
typedef struct _ui_state UI_STATE;
// ui_layer holds the context data
// this is owned by the ui_layer
typedef struct _ui_layer UI_LAYER;

// init the ui_layer struct
UI_LAYER* ui_layer_init();
// initiate the ui_state - create the ui_navigation_entry entries
// init the selection array etc.
UI_STATE* ui_layer_state_init(UI_LAYER* ui_layer);
// clean the ui_state
void ui_layer_state_clear(UI_STATE* state);
// destroy the ui_layer and the given ui_states
void ui_layer_destroy(UI_LAYER *ui_layer, UI_STATE **states,
                      size_t states_count);
// create a new UI_NAVIGATION_ENTRY on the UI_NAVIGATION on UI_STATE state
// UiPurpose is a user enum that the ui_layer knows nothing about
bool ui_layer_nav_set(UI_STATE* state, ContextId source, ContextId target, UiPurpose purpose);
// get the target on the UI_NAVIGATION on the state, given the context and purpose
bool ui_layer_nav_try_get(const UI_STATE* state, ContextId context, UiPurpose purpose, ContextId* target);
// remove a UI_NAVIGATION_ENTRY
// before removing returns the target
bool ui_layer_nav_remove(UI_STATE* state, ContextId source, UiPurpose purpose, ContextId* target);
// user should call this each cycle
// TODO should return context messages if contexts are deleted
void ui_layer_update_cycle(UI_LAYER* ui_layer);

// return the current ContextId of a state
ContextId ui_layer_state_current_return(UI_STATE* state);

// return name of a contextid
const char* ui_layer_contextid_name_return(UI_LAYER* ui_layer, ContextId context);
