#pragma once
#include <stdint.h>
#include "types.h"
#include "ids.h"
#include "cx_events.h"
#include "data_actions.h"
#include <stdbool.h>

// Interface for building the data layer structure.
// This structure can be safely presented to the user
// CX structs are found by their ContextId (minted by the data layer, stable
// for the object's lifetime) through a hash map.

// single context struct, that has info like name, user data etc.
typedef struct _cx CX;

// struct that holds the whole app_intrf layer, with the main root_cx context
typedef struct _app_intrf APP_INTRF;

// init and return the app_intrf struct
APP_INTRF *app_intrf_init();

// destroy the whole app_intrf and clean the data layer too
void app_intrf_destroy(APP_INTRF *app_intrf);

// NAVIGATION functions that UI can use to explore the interface
// call data_update() to update the data underneath and check if
// all the contexts still represent valid data
void nav_update(APP_INTRF *app_intrf);

// return the ContextId of the top context, that has no parent
ContextId nav_cx_root_return(APP_INTRF *app_intrf);

// return how many children a context has
size_t nav_cx_children_count(APP_INTRF *app_intrf, ContextId context);

// return a cx in the index of the parent array
ContextId nav_cx_child_at(APP_INTRF *app_intrf, ContextId parent, size_t index);

// return the parent of the context
ContextId nav_cx_parent_return(APP_INTRF *app_intrf, ContextId context);

// return the address of the string of the context name
const char *nav_cx_name_return(APP_INTRF *app_intrf, ContextId context);

// check if the context is valid or not anymore
bool nav_cx_is_valid(APP_INTRF *app_intrf, ContextId context);

// EVENTS - the context layer's change log. A ui view uses one NavCursor to
// react to structural changes (contexts added / removed / changed) instead of
// re-scanning the whole tree.

// attach cursor at the current head of the log: it will see only future events.
void nav_cursor_init(APP_INTRF *app_intrf, NavCursor *cursor);

// pop the next event after cursor's position, advancing the cursor.
// NAV_POLL_OVERFLOW means the cursor fell behind and was reset - rebuild.
NavPollResult nav_poll_event(APP_INTRF *app_intrf, NavCursor *cursor,
                             CxEvent *out);

// ACTIONS - see data_actions.h for the full contract and struct docs. Three
// step flow, every buffer caller-owned (stack array or a single struct),
// nothing here allocates:
//   nav_cx_actions      -> menu of what can be done to `context`
//   nav_cx_action_args  -> what inputs the chosen action needs
//   nav_cx_list_count/  -> for a CHOICE/MULTI_CHOICE arg, its options. `list`
//     nav_cx_list_at       is the DataListId from that arg's DataArgSpec,
//                          forwarded unchanged. `partial` is the request
//                          built so far, so a later arg's list can depend on
//                          an earlier arg's value (e.g. a CONNECT action's
//                          target list is filtered by the chosen source).
//   nav_cx_action_do    -> execute

// fill *out with up to cap available actions for context.
size_t nav_cx_actions(APP_INTRF *app_intrf, ContextId context,
                      DataAction *out, size_t cap);

// fill *out with up to cap argument specs the given action needs.
size_t nav_cx_action_args(APP_INTRF *app_intrf, ContextId context,
                          DataActionType type, DataArgSpec *out, size_t cap);

// how many options `list` currently has. May return 0 to mean "unknown, page
// with nav_cx_list_at until it returns false" instead of "empty".
size_t nav_cx_list_count(APP_INTRF *app_intrf, ContextId context,
                         DataListId list, const DataActionReq *partial);

// fill *out with option idx of `list`. returns false (and leaves *out
// zeroed) if idx is out of range.
bool nav_cx_list_at(APP_INTRF *app_intrf, ContextId context, DataListId list,
                    const DataActionReq *partial, size_t idx,
                    DataChoice *out);

// execute an action on context. Synchronously reconciles the CX tree and its
// change log before returning (the Phase D invariant), so the tree and
// *out_new below are already consistent with whatever the action just did.
// On success that creates a new context, *out_new is set to its ContextId;
// otherwise (including on failure) *out_new is left CONTEXT_ID_NULL.
DataActionResult nav_cx_action_do(APP_INTRF *app_intrf, ContextId context,
                                  const DataActionReq *req,
                                  ContextId *out_new);
