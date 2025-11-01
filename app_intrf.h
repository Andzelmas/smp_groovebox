#pragma once

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

// return the cx_curr of the cx_group - the context in which the user is
// currently in
CX *nav_cx_curr_return(APP_INTRF *app_intrf, unsigned int gr_idx);

// return the currently selected context for a cx_group
CX *nav_cx_selected_return(APP_INTRF *app_intrf, unsigned int gr_idx);

// return a single parent child
CX *nav_cx_child_return(APP_INTRF *app_intrf, CX *parent, unsigned int child_idx);

// return the children in the parent->cx_children array
// count is how many children there are
CX **nav_cx_children_return(APP_INTRF *app_intrf, CX *parent,
                            unsigned int *count);

// return the name of the cx
int nav_cx_display_name_return(APP_INTRF *app_intrf, CX *cx, char *return_name,
                               unsigned int name_len);

// cx_selected = next child of the cx_curr in cx_group
void nav_cx_selected_next(APP_INTRF *app_intrf, unsigned int gr_idx);

// cx_selected = previous child of the cx_curr in cx_group
void nav_cx_selected_prev(APP_INTRF *app_intrf, unsigned int gr_idx);

// exit the cx_curr context of the cx_group, 
// after this cx_curr will be cx_curr->cx_parent 
int nav_cx_curr_exit(APP_INTRF *app_intrf, unsigned int gr_idx);

// enter cx_self if it has children and is a container
// the cx_curr will become cx_self on the gr_idx cx_group
int nav_cx_enter(APP_INTRF *app_intrf, CX *cx_self, unsigned int gr_idx);

// invoke the app_data on the cx_self (push a button from user perspective)
// during invoke the context structure will not be changed, but it can be marked
// dirty, so the nav_update() function will change the context structure
int nav_cx_invoke(APP_INTRF *app_intrf, CX *cx_self);
//----------------------------------------------------------------------------------------------------
