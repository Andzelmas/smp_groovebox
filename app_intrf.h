#pragma once

//Interface for building the data layer structure.
//This structure can be safely presented to the user
//IMPORTANT: the returned CX* structs should not be saved - each UI cycle, after the nav_update(), the various CX* that are possible to get should be get a new.
//This way UI is safe to traverse the returned CX*, knowing none of these will disappear while traversing - the nav_functions do not remove or add CX*.
//CX* are added or removed in the nav_update() call.


typedef struct _cx CX;
typedef struct _app_intrf APP_INTRF;
//init and return the app_intrf struct
APP_INTRF* app_intrf_init();
//destroy the whole app_intrf and clean the data layer too
void app_intrf_destroy(APP_INTRF* app_intrf);

//NAVIGATION functions that UI can use to explore the interface
//call data_update() and check if any contexts are dirty, if yes recreate their children
void nav_update(APP_INTRF* app_intrf);
//return the cx_curr of the app_intrf - the context in which the user is currently in
CX* nav_cx_curr_return(APP_INTRF* app_intrf);
//returrn the currently selected context (this can be a context that user invoked last)
CX* nav_cx_selected_return(APP_INTRF* app_intrf);
//return the children in the parent->cx_children array
//count is how many there are
CX** nav_cx_children_return(APP_INTRF* app_intrf, CX* parent, unsigned int* count);
//return the cx top array, count is how many there are
CX** nav_cx_top_children_return(APP_INTRF* app_intrf, unsigned int* count);
//return the name of the cx for UI
int nav_cx_display_name_return(APP_INTRF* app_intrf, CX* cx, char* return_name, unsigned int name_len);
//----------------------------------------------------------------------------------------------------
