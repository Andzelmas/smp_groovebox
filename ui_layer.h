#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "ids.h"
#include "cx_events.h"
#include "data_actions.h"

// ContextId cannot be 0; kept as an alias for the shared sentinel
#define CONTEXT_ID_INVALID CONTEXT_ID_NULL

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

// what to do with a stored ContextId when the context it names is removed.
// the zero value (UI_STALE_REMOVE) is the default for every list and entry, so
// a view that does nothing still gets its tombstones cleaned.
typedef enum{
    UI_STALE_REMOVE = 0,     // drop the item / remove the whole entry
    UI_STALE_FIRST_SIBLING,  // re-point to the removed id's parent's first child
    UI_STALE_NEXT_SIBLING,   // ...to the child now at the removed id's index
    UI_STALE_PREV_SIBLING,   // ...to the child before the removed id's index
    UI_STALE_TO_SOURCE,      // re-point to the parent context itself
    UI_STALE_CLEAR,          // empty the list (for an entry: keep it, clear targets)
}UiStaleMode;

typedef struct {
    UiStaleMode mode;
} UiStalePolicy;

// result of ui_layer_state_reconcile
typedef enum{
    UI_RECONCILE_OK = 0,   // no change relevant to this view
    UI_RECONCILE_CHANGED,  // this view was touched - redraw it
    UI_RECONCILE_REBUILD,  // the cursor overflowed - re-derive the view from root
}UiReconcileResult;

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

// STALE-CONTEXT RECONCILIATION
// --------------------------------------------------
// set the policy applied to a target list's items when one becomes stale
bool ui_layer_target_list_set_stale_policy(UI_TARGET_LIST* targets, UiStalePolicy policy);
// set the policy applied to a (source,purpose) entry when its source becomes stale
bool ui_layer_state_entry_set_stale_policy(UI_STATE* state, ContextId source, UiPurpose purpose, UiStalePolicy policy);

// drain this state's cursor of context-layer events and apply the declared
// stale policies. call once per cycle per state, after ui_layer_update_cycle.
UiReconcileResult ui_layer_state_reconcile(UI_LAYER* ui_layer, UI_STATE* state);

// raw escape hatch: pop the next context-layer event for this state and let the
// caller react however it wants (via the UI_TARGET_LIST operations above). use
// this OR ui_layer_state_reconcile for a given state, not both - they share the
// state's cursor.
NavPollResult ui_layer_state_poll_event(UI_LAYER* ui_layer, UI_STATE* state, CxEvent* out);
// remove every occurrence of id from a target list. returns how many were removed.
size_t ui_layer_target_list_remove_id(UI_TARGET_LIST* targets, ContextId id);
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

// ACTIONS - thin pass-throughs to app_intrf's nav_cx_* (see data_actions.h
// for the full contract and struct docs, and app_intrf.h for the flow).
// fill *out with up to cap available actions for context.
size_t ui_layer_context_actions(UI_LAYER *ui_layer, ContextId context,
                                DataAction *out, size_t cap);
// fill *out with up to cap argument specs the given action needs.
size_t ui_layer_context_action_args(UI_LAYER *ui_layer, ContextId context,
                                    DataActionType type, DataArgSpec *out,
                                    size_t cap);
// how many options `list` currently has.
size_t ui_layer_context_list_count(UI_LAYER *ui_layer, ContextId context,
                                   DataListId list,
                                   const DataActionReq *partial);
// fill *out with option idx of `list`.
bool ui_layer_context_list_at(UI_LAYER *ui_layer, ContextId context,
                              DataListId list, const DataActionReq *partial,
                              size_t idx, DataChoice *out);
// execute an action on context. See nav_cx_action_do (app_intrf.h) for the
// synchronous-reconcile / out_new contract.
DataActionResult ui_layer_context_action_do(UI_LAYER *ui_layer,
                                            ContextId context,
                                            const DataActionReq *req,
                                            ContextId *out_new);
