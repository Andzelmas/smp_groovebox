#include "data_actions.h"
#include "ui_layer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define SELECTED_DIST 10 // further away contexts from cx_selected will not be displayed
#define ACTION_LIST_COUNT 5 // maximum possible actions for a context when returning DataAction

// convenient struct to hold info about a retrieved ContextId
typedef struct _context_intrf_info{
    // BORROWED STRING ADDRESS
    const char *name;
    size_t child_count;
} CONTEXT_INTRF_INFO;

// convenient struct to hold info about a ContextId actions
typedef struct _context_actions_info{
    DataAction action_list[ACTION_LIST_COUNT];
    size_t action_count;
    char action_char_init[ACTION_LIST_COUNT];
    ContextId actions_context;
} CONTEXT_ACTIONS_INFO;

struct termios orig_termios;

// reserved letters for navigation
static char reserved_letters[] = {'J','K','j','k','h','l','q', '\0'};

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

enum UiPurpose{
    UI_PURPOSE_HOVERED = 1,
    UI_PURPOSE_SELECTED = 2,
    UI_PURPOSE_CURRENT = 3
};

// navigation modes
// standard - where standard hjkl navigation works
// action - where the user started selecting an action for a context
enum Ui_Modes{
    UI_MODE_STANDARD = 1,
    UI_MODE_ACTION = 2
};

static bool helper_context_info_get( UI_LAYER *ui_layer, ContextId context, CONTEXT_INTRF_INFO *info)
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

    info->child_count =
        ui_layer_context_children_count(ui_layer, context);

    return true;
}

// Add add_id, if the array is full reset the array and start from beginning
static bool helper_target_list_add_reset_on_full(UI_LAYER* ui_layer, UI_TARGET_LIST* target_list, ContextId add_id){
    if (!target_list)
        return false;
    if(!ui_layer)
        return false;
    if (!ui_layer_context_valid(ui_layer, add_id))
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
static bool helper_target_list_add_resize(UI_LAYER* ui_layer, UI_TARGET_LIST* target_list, ContextId add_id){
    if(!ui_layer)
        return false;
    if (!target_list)
        return false;
    if (!ui_layer_context_valid(ui_layer, add_id))
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

// select previous child of parent (or next if next true).
// cur_idx - address of the current index in the parent children array
static void helper_nav_context_scroll(UI_LAYER* ui_layer, UI_STATE* state, ContextId parent, UiPurpose purpose, size_t* cur_idx, bool next){
    if(!ui_layer)
        return;
    if(!state)
        return;
    if(!cur_idx)
        return;

    UI_TARGET_LIST* target_list = ui_layer_nav_target_list_begin(state, parent, purpose);
    if(!target_list)
        return;

    int old_idx = (int)*cur_idx;
    int new_idx = 0;
    if (!next)
        new_idx = old_idx - 1;
    else
        new_idx = old_idx + 1;

    CONTEXT_INTRF_INFO cx_info;
    if(helper_context_info_get(ui_layer, parent, &cx_info)){
        if(new_idx < 0){
            new_idx = (int)cx_info.child_count - 1;
        }
        if(new_idx >= (int)cx_info.child_count){
            new_idx = 0;
        }

        ContextId new_context = ui_layer_context_child_at(ui_layer, parent, (size_t)new_idx);

        if(ui_layer_context_valid(ui_layer, new_context)){
            *cur_idx = (size_t)new_idx;
            helper_target_list_add_reset_on_full(ui_layer, target_list, new_context);
        }
    }

    ui_layer_nav_target_list_end(state);
}

// find the parent + purpose on the state and return the target ContextId index
// in the parent child array if the parent + purpose does not exist create it
// (capacity 1) and add the first child as the target ContextId
static size_t
helper_nav_context_single_purpose_set(UI_LAYER *ui_layer, UI_STATE *state,
                                      ContextId parent, UiPurpose purpose,
                                      UiStalePolicy stale_policy) {
    if(!ui_layer)
        return 0;
    if(!ui_layer_context_valid(ui_layer, parent))
        return 0;
    CONTEXT_INTRF_INFO state_main_current_info;
    if(helper_context_info_get(ui_layer, parent, &state_main_current_info)){
        if(state_main_current_info.child_count > 0){
            UI_TARGET_LIST* selected_target = ui_layer_nav_target_list_begin(state, parent, purpose);
            if (selected_target) {

                size_t return_idx = 0;
                ContextId cur_purpose = ui_layer_nav_target_list_get(selected_target, 0);
                for(size_t i = 0; i < state_main_current_info.child_count; i ++){
                    ContextId cur_child = ui_layer_context_child_at(ui_layer, parent, i);
                    if(cur_child == cur_purpose){
                       return_idx = i;
                       break;
                    }
                }
                ui_layer_nav_target_list_end(state);
                return return_idx;
            }
            else{
                ui_layer_state_entry_set(state, parent, purpose, 1);
                UI_TARGET_LIST* targets = ui_layer_nav_target_list_begin(state, parent, purpose);
                ui_layer_target_list_set_stale_policy(targets, stale_policy);

                if(targets){
                    ContextId new_purpose = ui_layer_context_child_at(ui_layer, parent, 0);
                    if(ui_layer_context_valid(ui_layer, new_purpose)){
                        helper_target_list_add_reset_on_full(ui_layer, targets, new_purpose);
                    }
                    ui_layer_nav_target_list_end(state);
                    return 0;
                }
            }
        }
    }

    return 0;
}

// return the (single) ContextId saved for parent+purpose on state, or
// CONTEXT_ID_INVALID if there is none. Factors the begin/get/end idiom used
// in several places below.
static ContextId helper_purpose_get(UI_LAYER* ui_layer, UI_STATE* state, ContextId parent, UiPurpose purpose){
    if(!ui_layer)
        return CONTEXT_ID_INVALID;
    if(!state)
        return CONTEXT_ID_INVALID;

    UI_TARGET_LIST* target_list = ui_layer_nav_target_list_begin(state, parent, purpose);
    if(!target_list)
        return CONTEXT_ID_INVALID;

    ContextId id = ui_layer_nav_target_list_get(target_list, 0);
    ui_layer_nav_target_list_end(state);
    return id;
}

// change the parent to the ContextId saved in the purpose on the *parent
static void helper_nav_context_enter(UI_LAYER* ui_layer, UI_STATE* state, ContextId* parent, UiPurpose purpose){
    if(!ui_layer)
        return;
    if(!state)
        return;
    if(!parent)
        return;
    if(!ui_layer_context_valid(ui_layer, *parent))
        return;

    ContextId cx_curr = helper_purpose_get(ui_layer, state, *parent, purpose);
    if(ui_layer_context_valid(ui_layer, cx_curr)){
        *parent = cx_curr;
    }
}

// change the parent to the parents parent
static void helper_nav_context_exit(UI_LAYER* ui_layer, UI_STATE* state, ContextId* parent, UiPurpose purpose){
    if(!ui_layer)
        return;
    if(!state)
        return;
    if(!parent)
        return;
    if(!ui_layer_context_valid(ui_layer, *parent))
        return;

    ContextId cx_parent = ui_layer_context_parent_return(ui_layer, *parent);
    if(ui_layer_context_valid(ui_layer, cx_parent)){
        *parent = cx_parent;
    }

}

// add a CONTEXT_ACTIONS_INFO to the context_actions_insert_id in the context_actions array
// also set the action_char_init to a char with which the user can initiate the action
static void helper_actions_list_add(UI_LAYER *ui_layer, UI_STATE *state,
                                      ContextId context,
                                      CONTEXT_ACTIONS_INFO *context_actions, size_t context_actions_cap,
                                      size_t context_actions_insert_id) {
    if(!ui_layer || !state)
        return;
    if(!ui_layer_context_valid(ui_layer, context))
        return;
    CONTEXT_ACTIONS_INFO *curr_action_info =
        &context_actions[context_actions_insert_id];
    curr_action_info->action_count = ui_layer_context_actions(
        ui_layer, context, curr_action_info->action_list, ACTION_LIST_COUNT);
    if(curr_action_info->action_count > 0)
        curr_action_info->actions_context = context;

    // find letters for action initialization
    for(size_t i = 0; i < curr_action_info->action_count; i++){
        DataAction* cur_action = &curr_action_info->action_list[i];
        curr_action_info->action_char_init[i] = '\0';
        size_t secondary_idx = 0;
        // find the letter_init
        for(size_t j = 0; j < strlen(cur_action->label); j++){
            char cur_char = cur_action->label[j];
            size_t k = 0;
            char target_char = reserved_letters[k];
            bool found = false;
            while(target_char != '\0'){
                if(cur_char == target_char){
                    found = true;
                    break;
                }
                target_char = reserved_letters[k];
                k++;
            }
            if(!found){
                curr_action_info->action_char_init[i] = cur_char;
                break;
            }
        }
    }
}

// return how many actions are activated with the init_char
// if out and out_cap is given, return the DataActions in question
static size_t helper_actions_char_return(CONTEXT_ACTIONS_INFO* actions_infos, size_t actions_cap, DataAction* out, size_t out_cap, char init_char){
    if(!actions_infos || actions_cap == 0)
        return 0;
    size_t actions_amount = 0;
    size_t insert_idx = 0;
    for(size_t i = 0; i < actions_cap; i++){
        CONTEXT_ACTIONS_INFO cur_info = actions_infos[i];
        for(size_t j = 0; j < cur_info.action_count; j++){
            if(cur_info.action_char_init[j] == init_char){
                if(insert_idx < out_cap && out){
                    out[insert_idx] = cur_info.action_list[j];
                    insert_idx++;
                }
                actions_amount++;
            }
        }
    }
    return actions_amount;
}

static void helper_string_print_underline(const char* string, char char_underline){
    if(!string)return;

    for(size_t i = 0; i < strlen(string); i++){
        char cur_char = string[i];
        if(cur_char == char_underline){
            printf("\033[4m%c\033[24m", cur_char);
        }
        else{
            printf("%c", cur_char);
        }
    }
}

static void helper_program_destroy(UI_LAYER* ui_layer, UI_STATE** states, size_t states_capacity){
    ui_layer_destroy(ui_layer, states, states_capacity);

    printf("\nCleaned everything, closing the up\n");
    disableRawMode();
}

int main() {
    enableRawMode();
    UI_LAYER* ui_layer = ui_layer_init();

    // if ui_layer failed to initialize analyze the error write it and exit
    if (!ui_layer) {
        printf("\nCould not start the ui_layer\n");
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
    // which idx in the state_main_current children array is the UI_PURPOSE_HOVERED
    size_t state_main_hovered_idx = 0;

    // this array will contain all of the states
    size_t states_count = 2;
    UI_STATE* states_all[2] = {state_main, state_root};

    // currently focused state
    UI_STATE* state_current = state_main;
    ContextId* state_current_context_current = &state_main_current;
    size_t *state_current_hovered_idx = &state_main_hovered_idx;

    // navigation mode
    size_t ui_nav_mode = UI_MODE_STANDARD;

    while (1) {
        // erase the terminal
        printf("\033[2J\033[H");
        // update the interface, of course should be in a loop
        ui_layer_update_cycle(ui_layer);

        // let each view react to contexts that appeared / were removed
        for (size_t si = 0; si < states_count; si++) {
            UiReconcileResult rr =
                ui_layer_state_reconcile(ui_layer, states_all[si]);
            if (rr == UI_RECONCILE_REBUILD && states_all[si] == state_main) {
                // the view's cursor fell behind - fall back to the root
                state_main_current = id_root;
                state_main_hovered_idx = 0;
            }
        }

        // show the state_main info
        // first update state_main_current and state_main_hovered_idx if user inputs changed these
        state_main_hovered_idx = helper_nav_context_single_purpose_set(
            ui_layer, state_main, state_main_current, UI_PURPOSE_HOVERED,
            (UiStalePolicy){.mode = UI_STALE_PREV_SIBLING});

        CONTEXT_INTRF_INFO state_main_current_info;
        if(helper_context_info_get(ui_layer, state_main_current, &state_main_current_info)){
            printf("----| %s |----\n\n", state_main_current_info.name);
            // get the selected ContextId
            ContextId state_main_id_hovered = helper_purpose_get(
                ui_layer, state_main, state_main_current, UI_PURPOSE_HOVERED);
            for(size_t i = 0; i < state_main_current_info.child_count; i++){
                CONTEXT_INTRF_INFO state_main_current_child_info;
                ContextId cur_child = ui_layer_context_child_at(ui_layer, state_main_current, i);
                if(helper_context_info_get(ui_layer, cur_child, &state_main_current_child_info)){
                    if(cur_child == state_main_id_hovered){
                        state_main_hovered_idx = i;
                        printf(">%s\n", state_main_current_child_info.name);
                    }
                    else{
                        printf("%s\n", state_main_current_child_info.name);
                    }
                }
            }
        }

        // show possible actions for the current state contexts
        printf("\n-------------------------------------------------------------"
               "---------------------------------------\n");
        // generate letters that user can use to initiate the actions
        // and print the action names
        CONTEXT_ACTIONS_INFO cx_action_infos[2] = {0};
        helper_actions_list_add(ui_layer, state_current,
                                *state_current_context_current, cx_action_infos,
                                2, 0);

        ContextId state_current_context_hovered = helper_purpose_get(
            ui_layer, state_current, *state_current_context_current,
            UI_PURPOSE_HOVERED);
        helper_actions_list_add(ui_layer, state_current,
                                state_current_context_hovered, cx_action_infos,
                                2, 1);
        for (size_t i = 0; i < 2; i++) {
            CONTEXT_ACTIONS_INFO cur_cx_action_info = cx_action_infos[i];
            CONTEXT_INTRF_INFO action_context_info;
            if (helper_context_info_get(ui_layer,
                                        cur_cx_action_info.actions_context,
                                        &action_context_info)) {
                printf("%s: ", action_context_info.name);
            }
            for (size_t j = 0; j < cur_cx_action_info.action_count; j++) {
                DataAction cur_action = cur_cx_action_info.action_list[j];
                helper_string_print_underline(cur_action.label, cur_cx_action_info.action_char_init[j]);
            }
        }
        printf("\n-------------------------------------------------------------"
               "---------------------------------------\n");

        // show the state_root info
        printf("\n");
        CONTEXT_INTRF_INFO cx_root_info;
        if(helper_context_info_get(ui_layer, id_root, &cx_root_info)){
            for(size_t i = 0; i < cx_root_info.child_count; i++){
                ContextId root_child =
                    ui_layer_context_child_at(ui_layer, id_root, i);
                if (ui_layer_context_valid(ui_layer, root_child)) {
                    CONTEXT_INTRF_INFO cx_root_child_info;
                    if(helper_context_info_get(ui_layer, root_child, &cx_root_child_info)){
                        printf("| %lu_%s |", i, cx_root_child_info.name);
                    }
                }
                if(i == cx_root_info.child_count - 1)
                    printf("\n");
            }
        }

        // get user inputs
        int input = getchar();
        // if user pressed ESC return to standard mode from any other mode
        if(input == '\e')
            ui_nav_mode = UI_MODE_STANDARD;
        // if the user pressed a action init key, go to action initialize mode
        if (helper_actions_char_return(cx_action_infos, 2, NULL, 0, input) > 0)
            ui_nav_mode = UI_MODE_ACTION;

        unsigned int exit = 0;

        if (ui_nav_mode == UI_MODE_STANDARD) {
            switch (input) {
            case 'J':
                break;
            case 'K':
                break;
            case 'j':
                helper_nav_context_scroll(
                    ui_layer, state_current, *state_current_context_current,
                    UI_PURPOSE_HOVERED, state_current_hovered_idx, true);
                break;
            case 'k':
                helper_nav_context_scroll(
                    ui_layer, state_current, *state_current_context_current,
                    UI_PURPOSE_HOVERED, state_current_hovered_idx, false);
                break;
            case 'l':
                helper_nav_context_enter(ui_layer, state_current,
                                         state_current_context_current,
                                         UI_PURPOSE_HOVERED);
                break;
            case 'h':
                helper_nav_context_exit(ui_layer, state_current,
                                        state_current_context_current,
                                        UI_PURPOSE_HOVERED);
                break;
            case 'q':
                exit = 1;
                break;
            }

            if (exit == 1)
                break;
        }
    }

    helper_program_destroy(ui_layer, states_all, states_count);

    return 0;
}
