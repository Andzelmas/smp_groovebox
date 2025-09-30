#pragma once

// Interface for building the data layer structure.
// This structure can be safely presented to the user
// IMPORTANT: the returned CX* structs should not be saved - each UI cycle,
// after the nav_update(), the various CX* that are possible to get should be
// get a new. This way UI is safe to traverse the returned CX*, knowing none of
// these will disappear while traversing - the nav_functions do not remove or
// add CX*. CX* are added or removed in the nav_update() call.

typedef struct _cx CX;
typedef struct _app_intrf APP_INTRF;

// init and return the app_intrf struct
APP_INTRF *app_intrf_init();

// destroy the whole app_intrf and clean the data layer too
void app_intrf_destroy(APP_INTRF *app_intrf);

// NAVIGATION functions that UI can use to explore the interface

// call data_update() and check if any contexts are dirty, if yes recreate their
// children
void nav_update(APP_INTRF *app_intrf);

// return the cx_root - the parent of all the other contexts
CX *nav_cx_root_return(APP_INTRF *app_intrf);

// return the cx_curr of the app_intrf - the context in which the user is
// currently in
CX *nav_cx_curr_return(APP_INTRF *app_intrf);

// returrn the currently selected context (this can be a context that user
// invoked last)
CX *nav_cx_selected_return(APP_INTRF *app_intrf);

// return the children in the parent->cx_children array
// count is how many there are
CX **nav_cx_children_return(APP_INTRF *app_intrf, CX *parent,
                            unsigned int *count);

// return the name of the cx for UI
int nav_cx_display_name_return(APP_INTRF *app_intrf, CX *cx, char *return_name,
                               unsigned int name_len);

// cx_select = next child of the cx_curr parent
void nav_cx_selected_next(APP_INTRF *app_intrf);

// cx_select = previous child of the cx_curr parent
void nav_cx_selected_prev(APP_INTRF *app_intrf);

// exit the cx_curr context, so cx_curr will become the previous
// cx_curr->cx_parent
//
int nav_cx_curr_exit(APP_INTRF *app_intrf);

// call the data function to invoke the user_data on the context (user
// interaction callback in essence) during invoke the context structure will not
// be changed, but it can be marked dirty, so the nav_update() function will
// know to change the cx structure also cx_curr becomes the cx_selected and
// cx_selected becomes the first cx_selected child.
int nav_cx_selected_invoke(APP_INTRF *app_intrf);
//----------------------------------------------------------------------------------------------------
