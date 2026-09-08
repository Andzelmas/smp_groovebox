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

// return how many children a context has
size_t nav_cx_children_count(APP_INTRF *app_intrf, uint64_t context);

// return a cx in the index of the parent array
uint64_t nav_cx_child_at(APP_INTRF *app_intrf, uint64_t parent, size_t index);

// return the parent of the context
uint64_t nav_cx_parent_return(APP_INTRF *app_intrf, uint64_t context);

// return the address of the string of the context name
const char *nav_cx_name_return(APP_INTRF *app_intrf, uint64_t context);

// check if the context is valid or not anymore
bool nav_cx_is_valid(APP_INTRF *app_intrf, uint64_t context);
