#pragma once
#include <stdint.h>
#include "types.h"
#include <stdbool.h>

// Interface for building the data layer structure.
// This structure can be safely presented to the user
// IMPORTANT: the returned CX* structs should not be saved - each UI cycle,
// after the nav_update(), the various CX* that are possible to get should be
// get a new. This way UI is safe to traverse the returned CX*, knowing none of
// these will disappear while traversing - the nav_functions do not remove or
// add CX*. CX* are added or removed in the nav_update() call.

// single context struct, that has info like name, user data etc.
typedef struct _cx CX;

// struct the holds the whole app_intrf layer, with the main root_cx context
typedef struct _app_intrf APP_INTRF;

// init and return the app_intrf struct
APP_INTRF *app_intrf_init();

// destroy the whole app_intrf and clean the data layer too
void app_intrf_destroy(APP_INTRF *app_intrf);

// NAVIGATION functions that UI can use to explore the interface
// IMPORTANT - returned CX* cant be saved - they should be gotten
// each cycle, after the nav_update() call.

// call data_update() and check if any contexts are dirty, if yes recreate their
// children
void nav_update(APP_INTRF *app_intrf);

// set the group cx filters
// the nav_ functions that navigate cx_curr, cx_selected and return children will use these flags
void nav_group_filter_set(APP_INTRF *app_intrf, unsigned int gr_idx, enum intrfFlags group_flags_include, enum intrfFlags group_flags_exclude);

// return the top context, that has no parent
CX *nav_cx_root_return(APP_INTRF *app_intrf);

// return the cx_curr of the cx_group - the context in which the user is
// currently in
CX *nav_cx_curr_return(APP_INTRF *app_intrf, unsigned int gr_idx);

// exit the cx_curr context of the cx_group, 
// after this cx_curr will be cx_curr->cx_parent 
int nav_cx_curr_exit(APP_INTRF *app_intrf, unsigned int gr_idx);

// change the cx_curr of the gr_idx, without calling the data_invoke function
int nav_cx_curr_change(APP_INTRF* app_intrf, CX* cx_self, unsigned int gr_idx);

// invoke the app_data on the cx_self (push a button from user perspective)
// during invoke the context structure will not be changed, but it can be marked
// dirty, so the nav_update() function will change the context structure
// USE ONLY if there is a need to invoke a CX without entering it
int nav_cx_invoke(APP_INTRF *app_intrf, CX *cx_self);

// enter cx_self if it has children and is a container
// the cx_curr will become cx_self on the gr_idx cx_group
// this function calls nav_cx_invoke, so no need to do that separatly
int nav_cx_enter(APP_INTRF *app_intrf, CX *cx_self, unsigned int gr_idx);

// return the currently selected context for a cx_group
CX *nav_cx_selected_return(APP_INTRF *app_intrf, CX* cx_curr, unsigned int gr_idx);

// go through children and run the match_func on each match
// gr_idx is the group index where the filters for the context matching are stored
void nav_cx_children_match_callback(APP_INTRF *app_intrf, CX *parent,
                                    unsigned int gr_idx, void *user_data,
                                    void(match_func)(CX *cx_matched,
                                                     void *user_data));

//go through contexts with _ON_TOP flag, starting from cx_curr
// traverse the context tree moving up towards the root cx
void nav_cx_on_top_match_callback(APP_INTRF *app_intrf, CX *cx_curr,
                                  unsigned int gr_idx, void *user_data,
                                  void(match_func)(CX *cx_matched,
                                                   void *user_data));

// return the name of the cx
int nav_cx_display_name_return(APP_INTRF *app_intrf, CX *cx, char *return_name,
                               unsigned int name_len);

// cx_selected = next child of the cx_curr in gr_idx group 
void nav_cx_selected_next(APP_INTRF *app_intrf, CX* cx_curr, unsigned int gr_idx);

// cx_selected = previous child of the cx_curr in gr_idx group 
void nav_cx_selected_prev(APP_INTRF *app_intrf, CX* cx_curr, unsigned int gr_idx);

// return the cx interface flags, 
// see the intrfFlags enum in types.h
uint32_t nav_cx_flags_return(APP_INTRF *app_intrf, CX *cx_self);
//----------------------------------------------------------------------------------------------------
