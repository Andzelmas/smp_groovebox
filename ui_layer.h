#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

// contextid cannot be 0
#define CONTEXT_ID_INVALID 0

typedef uint64_t ContextId;
typedef uint32_t UiPurpose;

typedef struct _ui_target_list UI_TARGET_LIST;
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
// UI_NAVIGATION_ENTRY* is a hash table, where the user can save ContextId arrays in the UI_TARGET_LIST->items
// Example: source ContextId 101, with UiPurpose UI_PURPOSE_SELECTED.
// later the user can _target_list_add to that entry and have 101 (UI_PURPOSE_SELECTED) -> {102, 104, 107} 
// and use that to display selected ContexIds in the 101 ContextId.

// if the (source, purpose) already exists returns true but does nothing
// if the (source, purpose) does not exist, create entry and initiate the UI_TARGET_LIST with the capacity
// dynamic true will resize the UI_TARGET_LIST when adding, otherwise _target_list_add will wrap around
bool ui_layer_nav_set(UI_STATE* state, ContextId source, UiPurpose purpose, size_t capacity, bool dynamic);
// remove a UI_NAVIGATION_ENTRY
bool ui_layer_nav_remove(UI_STATE* state, ContextId source, UiPurpose purpose);

// -------------------------------------------------- 
// UI_TARGET_LIST operations:
// user uses these functions to add, remove, retrieve etc. the UI_TARGE_LIST->items
// In other words use the UI_TARGET_LIST->items as save slots for ContextIds

// get the target list from the state with key (context,purpose).
// must call ui_layer_nav_target_list_end after modifying the target list
UI_TARGET_LIST* ui_layer_nav_target_list_begin(UI_STATE* state, ContextId context, UiPurpose purpose); 
// insert the context_insert into the UI_TARGET_LIST
bool ui_layer_nav_target_list_add(UI_TARGET_LIST* targets, ContextId context_insert);
// get the ContextId from the targets list in the idx index
ContextId ui_layer_nav_target_list_get(UI_TARGET_LIST* targets, size_t idx);
// find the context_find and return the index in the targets array or -1 on failure
int ui_layer_nav_target_list_find(UI_TARGET_LIST* targets, ContextId context_find);
// insert context_insert into the index idx, return false is failed
bool ui_layer_nav_target_list_insert(UI_TARGET_LIST* targets, ContextId context_insert, size_t idx);
// remove the idx index from targets array, shrink the array if necessary and dynamic, lower count
void ui_layer_nav_target_list_remove(UI_TARGET_LIST* targets, size_t idx);
// lower the borrow_count of the state->navigation, indicating that it is safe to change the hash table
void ui_layer_nav_target_list_end(UI_STATE* state);
// -------------------------------------------------- 

// user should call this each cycle
// TODO should return context messages if contexts are deleted
void ui_layer_update_cycle(UI_LAYER* ui_layer);

// return the current ContextId of a state
ContextId ui_layer_state_current_return(UI_STATE* state);

// return name of a contextid
const char* ui_layer_contextid_name_return(UI_LAYER* ui_layer, ContextId context);
// return the first child of the parent
// returns CONTEX_ID_INVALID on error or if parent has no children
ContextId ui_layer_contextid_children_get_first(UI_LAYER* ui_layer, ContextId parent);
