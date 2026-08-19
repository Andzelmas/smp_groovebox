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
// call data_update() and check if any contexts are dirty, if yes recreate their
// children
void nav_update(APP_INTRF *app_intrf);

// return the key of thetop context, that has no parent
uint64_t nav_cx_root_return(APP_INTRF *app_intrf);

// return the name of the key cx
int nav_cx_display_name_return(APP_INTRF *app_intrf, uint64_t key, char *return_name,
                               unsigned int name_len);

// return the cx interface flags, 
// see the intrfFlags enum in types.h
uint32_t nav_cx_flags_return(APP_INTRF *app_intrf, uint64_t key);
//----------------------------------------------------------------------------------------------------
