#pragma once

#include <stdint.h>

typedef struct _ui_navigation_entry UI_NAVIGATION_ENTRY;

typedef struct _ui_navigation UI_NAVIGATION;

typedef struct _ui_state UI_STATE;

typedef struct _ui_layer UI_LAYER;

// initiate the ui_layer
// ui_states_num - how many ui_states will be used during 
// the life time of the program
UI_LAYER* ui_layer_init(unsigned int ui_states_num);

void ui_layer_destroy(UI_LAYER* ui_layer);

// call each program loop to update the underlying data
// also to check if all UI items are still valid and
// get recommendations how to replace them if they are not
void ui_layer_update_cycle(UI_LAYER* ui_layer);

// create a new ui_state on the ui_layer
// returns the index in the ui_states array
// or a -1 if did not create a state
int ui_layer_state_create(UI_LAYER* ui_layer);


