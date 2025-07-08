#pragma once
typedef struct _cx CX;
typedef struct _app_intrf APP_INTRF;
//init and return the app_intrf struct
APP_INTRF* app_intrf_init();
//destroy the whole app_intrf and clean the data layer too
void app_intrf_destroy(APP_INTRF* app_intrf);

//NAVIGATION functions that UI can use to explore the interface
//return the cx_root of the app_intrf - the very top context of the interface
CX* nav_cx_root_return(APP_INTRF* app_intrf);
//return the child in the parent->cx_children array at the child_idx index
//return NULL if the child does not exist
CX* nav_cx_child_return(APP_INTRF* app_intrf, CX* parent, unsigned int child_idx);
//return the name of the cx for UI
int nav_cx_display_name_return(APP_INTRF* app_intrf, CX* cx, char* return_name, unsigned int name_len);
//----------------------------------------------------------------------------------------------------
