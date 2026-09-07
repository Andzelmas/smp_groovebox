#include "ui_layer.h"
#include "util_funcs/log_funcs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#define SELECTED_DIST 10 // further away contexts from cx_selected will not be displayed

// convenient struct to hold info about a retrieved ContextId
typedef struct {
    // BORROWED STRING ADDRESS
    const char *name;
    uint32_t flags;
    size_t child_count;
} InterfaceContextInfo;

struct termios orig_termios;

static void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}
static void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

enum ui_groups {
    GROUP_MAIN = 0,
    GROUP_ROOT = 1
};

enum UiPurpose{
    UI_PURPOSE_HOVERED = 1,
    UI_PURPOSE_SELECTED = 2,
    UI_PURPOSE_CURRENT = 3
};

static bool helper_context_info_get( UI_LAYER *ui_layer, ContextId context, InterfaceContextInfo *info)
{
    if (!ui_layer)
        return false;

    if (!info)
        return false;

    if (!ui_layer_context_valid(ui_layer, context))
        return false;

    const char* cx_name =
        ui_layer_context_name_return(ui_layer, context);
    if (!cx_name)
        return false;
    info->name = cx_name;

    info->flags =
        ui_layer_context_flags_return(ui_layer, context);

    info->child_count =
        ui_layer_context_children_count(ui_layer, context);

    return true;
}

// Add add_id, if the array is full reset the array and start from beginning
static bool helper_target_list_add_reset_on_full(UI_TARGET_LIST* target_list, ContextId add_id){
    if (!target_list)
        return false;
    if (add_id == CONTEXT_ID_INVALID)
        return false;

    UiTargetListAddResult add_result =
        ui_layer_nav_target_list_add(target_list, add_id);
    if (add_result == UI_TARGET_LIST_ADD_ERROR)
        return false;
    if (add_result == UI_TARGET_LIST_ADD_FULL) {
        if(!ui_layer_nav_target_list_clear(target_list))
            return false;

        add_result = ui_layer_nav_target_list_add(target_list, add_id);
        if(add_result != UI_TARGET_LIST_ADD_SUCCESS)
            return false;
    }

    return true;
}
// Helper function to add a add_id, if the array is full, double its size
static bool helper_target_list_add_resize(UI_TARGET_LIST* target_list, ContextId add_id){
    if (!target_list)
        return false;
    if (add_id == CONTEXT_ID_INVALID)
        return false;

    UiTargetListAddResult add_result =
        ui_layer_nav_target_list_add(target_list, add_id);
    if (add_result == UI_TARGET_LIST_ADD_ERROR)
        return false;
    if (add_result == UI_TARGET_LIST_ADD_FULL) {
        size_t capacity = ui_layer_nav_target_list_capacity(target_list);
        if (capacity > SIZE_MAX / 2)
            return false;
        size_t new_capacity = capacity * 2;
        if(!ui_layer_nav_target_list_resize(target_list, new_capacity))
            return false;

        add_result = ui_layer_nav_target_list_add(target_list, add_id);
        if(add_result != UI_TARGET_LIST_ADD_SUCCESS)
            return false;
    }

    return true;
}

static void helper_program_destroy(UI_LAYER* ui_layer, UI_STATE** states, size_t states_capacity){
    ui_layer_destroy(ui_layer, states, states_capacity);

    log_append_logfile("Cleaned everything, closing the app \n");
    disableRawMode();
}

int main() {
    enableRawMode();
    log_clear_logfile();
    UI_LAYER* ui_layer = ui_layer_init();

    // if ui_layer failed to initialize analyze the error write it and exit
    if (!ui_layer) {
        log_append_logfile("Could not start the ui_layer\n");
        exit(1);
    }

    // create the various states
    // this state will always stay on the root context
    UI_STATE* state_root = ui_layer_state_init(ui_layer);
    ContextId id_root = ui_layer_state_root_return(ui_layer);

    // main ui state, for general program navigation
    UI_STATE* state_main = ui_layer_state_init(ui_layer);
    // current ContextId of the state_main;
    ContextId state_main_current = id_root;
    // this array will contain all of the states
    size_t states_count = 2;
    UI_STATE* states_all[2] = {state_main, state_root};
    // init the hovered purpose on the state_main_current ContextId
    if (!ui_layer_state_entry_set(state_main, state_main_current , UI_PURPOSE_HOVERED, 1)) {
        helper_program_destroy(ui_layer, states_all, states_count);
    }
    // initialize the state_main hovered ContextId to the first child of the root context
    // TODO these will need to be abstracted into a helper function
    InterfaceContextInfo state_main_current_info;
    if(helper_context_info_get(ui_layer, state_main_current, &state_main_current_info)){
        if(state_main_current_info.child_count > 0){
            UI_TARGET_LIST* selected_target = ui_layer_nav_target_list_begin(state_main, state_main_current, UI_PURPOSE_HOVERED);
            if (selected_target) {
                helper_target_list_add_reset_on_full(
                    selected_target,
                    ui_layer_context_child_at(ui_layer, state_main_current, 0));
                ui_layer_nav_target_list_end(state_main);
            }
        }
    }


    // currently focused state
    UI_STATE* state_current = state_main;

    while (1) {
        // erase the terminal
        //printf("\033[2J\033[H");
        // update the interface, of course should be in a loop
        ui_layer_update_cycle(ui_layer);

        // show the state_root info
        InterfaceContextInfo cx_root_info;
        if(helper_context_info_get(ui_layer, id_root, &cx_root_info)){
            printf(" --- %s --- \n", cx_root_info.name);

            for(size_t i = 0; i < cx_root_info.child_count; i++){
                ContextId root_child =
                    ui_layer_context_child_at(ui_layer, id_root, i);
                if (root_child != CONTEXT_ID_INVALID) {
                    InterfaceContextInfo cx_root_child_info;
                    if(helper_context_info_get(ui_layer, root_child, &cx_root_child_info)){
                        printf("| %lu_%s |", i, cx_root_child_info.name);
                    }
                }
                if(i == cx_root_info.child_count - 1)
                    printf("\n--------------------\n");
            }
        }

        // show the state_main info
        if(helper_context_info_get(ui_layer, state_main_current, &state_main_current_info)){
            // get the selected ContextId
            UI_TARGET_LIST* selected_target = ui_layer_nav_target_list_begin(state_main, state_main_current, UI_PURPOSE_HOVERED);
            ContextId state_main_id_hovered = CONTEXT_ID_INVALID;
            if(selected_target){
                state_main_id_hovered = ui_layer_nav_target_list_get(selected_target, 0);
                ui_layer_nav_target_list_end(state_main);
            }
            for(size_t i = 0; i < state_main_current_info.child_count; i++){
                InterfaceContextInfo state_main_current_child_info;
                ContextId cur_child = ui_layer_context_child_at(ui_layer, state_main_current, i);
                if(helper_context_info_get(ui_layer, cur_child, &state_main_current_child_info)){
                    if(cur_child == state_main_id_hovered){
                        printf("| >%s |", state_main_current_child_info.name);
                    }
                    else{
                        printf("| %s |", state_main_current_child_info.name);
                    }
                }
            }
        }
        
        // get user inputs
        int input = getchar();
        unsigned int exit = 0;

        // TODO navigation should be in abstracted functions
        switch (input) {
        case 'J':
            break;
        case 'K':
            break;
        case 'j':
            break;
        case 'k':
            break;
        case 'l':
            break;
        case 'h':
            break;
        case 'q':
            exit = 1;
            break;
        }

        if (exit == 1)
            break;
    }

    helper_program_destroy(ui_layer, states_all, states_count);

    return 0;
}
