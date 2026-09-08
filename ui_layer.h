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

// results of adding an item to a target_list
typedef enum{
    UI_TARGET_LIST_ADD_SUCCESS = 1,
    UI_TARGET_LIST_ADD_FULL = 2,
    UI_TARGET_LIST_ADD_ERROR = 3
}UiTargetListAddResult;

// init the ui_layer struct
UI_LAYER* ui_layer_init();
// initiate the ui_state - create the ui_state_entry entries
UI_STATE* ui_layer_state_init(UI_LAYER* ui_layer);
// clean the ui_state
void ui_layer_state_clear(UI_STATE* state);
// destroy the ui_layer and the given ui_states
void ui_layer_destroy(UI_LAYER *ui_layer, UI_STATE **states, size_t states_count);

// create a new UI_STATE_ENTRY on the UI_STATE state
// UI_STATE_ENTRY* is a hash table, where the user can save ContextId arrays in the UI_TARGET_LIST->items
// Example: source ContextId 101, with UiPurpose UI_PURPOSE_SELECTED.
// later the user can _target_list_add to that entry and have 101 (UI_PURPOSE_SELECTED) -> {102, 104, 107} 
// and use that to display selected ContexIds in the 101 ContextId.

// if the (source, purpose) already exists returns true but does nothing
// if the (source, purpose) does not exist, create entry and initiate the UI_TARGET_LIST with the capacity
bool ui_layer_state_entry_set(UI_STATE* state, ContextId source, UiPurpose purpose, size_t capacity);
// remove a UI_STATE_ENTRY
bool ui_layer_state_entry_remove(UI_STATE* state, ContextId source, UiPurpose purpose);

// -------------------------------------------------- 
// UI_TARGET_LIST operations:
// user uses these functions to add, remove, retrieve etc. the UI_TARGE_LIST->items
// In other words use the UI_TARGET_LIST->items as save slots for ContextIds

// get the target list from the state with key (context,purpose).
// must call ui_layer_nav_target_list_end after modifying the target list
UI_TARGET_LIST* ui_layer_nav_target_list_begin(UI_STATE* state, ContextId context, UiPurpose purpose); 
// insert the context_insert into the UI_TARGET_LIST
UiTargetListAddResult ui_layer_nav_target_list_add(UI_TARGET_LIST* targets, ContextId context_insert);
// remove idx from the target_list
bool ui_layer_nav_target_list_remove(UI_TARGET_LIST* targets, size_t idx);
// return how many items the target_list can hold
size_t ui_layer_nav_target_list_capacity(UI_TARGET_LIST* targets);
// return how many items the target_list currently holds
size_t ui_layer_nav_target_list_count(UI_TARGET_LIST* targets);
// clear the target list - target_list->count becomes 0
bool ui_layer_nav_target_list_clear(UI_TARGET_LIST* targets);
// get the ContextId from the targets list in the idx index
ContextId ui_layer_nav_target_list_get(UI_TARGET_LIST* targets, size_t idx);
// double the target_list size
bool ui_layer_nav_target_list_resize(UI_TARGET_LIST* targets, size_t new_capacity);
// lower the borrow_count of the state, indicating that it is safe to change the hash table
void ui_layer_nav_target_list_end(UI_STATE* state);
// -------------------------------------------------- 

// user should call this each cycle
void ui_layer_update_cycle(UI_LAYER* ui_layer);

// return the root context ContextId
ContextId ui_layer_state_root_return(UI_LAYER* ui_layer);

// check if the context is valid and still linked to an existing cx on the context layer
bool ui_layer_context_valid(UI_LAYER *ui_layer, ContextId context);

// return the address of the context name
const char *ui_layer_context_name_return(UI_LAYER *ui_layer, ContextId context);

// how many children the context has
size_t ui_layer_context_children_count(UI_LAYER *ui_layer, ContextId context);

// get the ContextId of the index child in the parent children array
ContextId ui_layer_context_child_at(UI_LAYER *ui_layer, ContextId parent, size_t index);

// return the context parent
ContextId ui_layer_context_parent_return(UI_LAYER *ui_layer, ContextId context);
