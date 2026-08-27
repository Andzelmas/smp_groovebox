#pragma once
#include <stdint.h>
#include "types.h"
#include <stdbool.h>

// Interface for building the data layer structure.
// This structure can be safely presented to the user
// CX structs are found using unique ids (for the lifetime of the program)
// and a hash map and links the ids to the cx*

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

// return the key of the top context, that has no parent
uint64_t nav_cx_root_return(APP_INTRF *app_intrf);

// return the name of the key cx
const char* nav_cx_display_name_return(APP_INTRF *app_intrf, uint64_t key);

// run a ChildFn function on the parent_id children
// in other words iterate children and use function
void nav_cx_children(APP_INTRF *app_intrf, uint64_t parent_id,
                     bool (ChildFn)(uint64_t child_id, void *user_data),
                     void *user_data); 
