#include "app_intrf.h"
#include "types.h"
#include "util_funcs/log_funcs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#define SELECTED_DIST 10 // further away contexts from cx_selected will not be displayed

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
    GROUP_BUTTONS = 1,
    GROUP_ROOT = 2
};

// how many temporary CX* can hold the cx_user_array in the ui_data struct
#define CX_USER_ARRAY_MAX 10
// used for callback functions
// the CX* in this struct must be set to NULL at the beginning of each loop
struct ui_data{
    int user_int01;
    int user_int02;
    int user_int03;
    int user_int04;
    CX* cx_user;
    CX* cx_user_array[CX_USER_ARRAY_MAX];
    APP_INTRF* app_intrf;
};

// clean the user_data struct
static void ui_data_clean(struct ui_data* my_data, APP_INTRF* app_intrf){
    my_data->user_int01 = 0;
    my_data->user_int02 = 0;
    my_data->user_int03 = 0;
    my_data->user_int04 = 0;
    my_data->cx_user = NULL;
    for(unsigned int i = 0; i < CX_USER_ARRAY_MAX; i ++){
        my_data->cx_user_array[i] = NULL;
    }
    my_data->app_intrf = app_intrf;
}

// find which iteration is the given cx in the matched contexts
// also counts the matched contexts
static void ui_cx_match_iter(CX* cx_matched, void* user_data){
    if(!user_data)return;
    struct ui_data* my_data = (struct ui_data*)user_data;
    if(!my_data->cx_user)return;
    if(cx_matched == my_data->cx_user){
        my_data->user_int02 = my_data->user_int01;
    }
    // matched contexts so far
    my_data->user_int01 += 1;
}

// print the names of the matched contexts
// if the context is selected highlight it
// if the context is out of bounds of display contexts print ...
static void ui_cx_match_print_names(CX* cx_matched, void* user_data){
    if(!user_data)return;
    struct ui_data* my_data = (struct ui_data*)user_data;

    if (!my_data->cx_user)return;
    if(!my_data->app_intrf)return;

    int iter = my_data->user_int01;
    int selected_iter = my_data->user_int02;
    int count = my_data->user_int03;
    int highlight = my_data->user_int04;

    int min_cx = selected_iter - SELECTED_DIST;
    int p_max = 0;
    if (min_cx < 0)
        p_max = abs(min_cx);

    int max_cx = selected_iter + SELECTED_DIST;
    int p_min = 0;
    if (max_cx > (count - 1))
        p_min = (max_cx - (count - 1));

    min_cx -= p_min;
    if (min_cx < 0)
        min_cx = 0;
    max_cx += p_max;
    if (max_cx > (count - 1))
        max_cx = count - 1;

    CX* selected_cx_main = my_data->cx_user;

    char display_name[MAX_PARAM_NAME_LENGTH];

    if (selected_iter == -1)
        return;

    // if a cx is too far away from the selected_cx dont show it
    if (iter < min_cx) {
        if ((min_cx - iter) == 1)
            printf("     ...\n");
        my_data->user_int01 += 1;
        return;
    }
    if (iter > max_cx) {
        if ((iter - max_cx) == 1)
            printf("     ...\n");
        my_data->user_int01 += 1;
        return;
    }

    // highlight the selected context
    if (selected_cx_main == cx_matched && highlight == 1) {
        printf("\033[0;30;47m");
    }
    printf("     |");

    // show the name of the context
    if (nav_cx_display_name_return(my_data->app_intrf, cx_matched, display_name,
                                   MAX_PARAM_NAME_LENGTH) == 1) {
        printf("--> %s", display_name);
    }
    // reset highlighting
    if (selected_cx_main == cx_matched && highlight == 1)
        printf("\033[0m");
    printf("\n");

    my_data->user_int01 += 1;
}

static void ui_cx_match_print_names_horizontal(CX* cx_matched, void* user_data){
    if(!cx_matched)return;
    if(!user_data)return;

    struct ui_data* my_data = (struct ui_data*)user_data;

    char display_name[MAX_PARAM_NAME_LENGTH];
    CX* selected_cx_main = my_data->cx_user;
    int highlight = my_data->user_int04;
    int numbers = my_data->user_int01;
    int iter = my_data->user_int02;
    int put_into_cx_array = my_data->user_int03;

    // highlight the selected context
    if(selected_cx_main){
        if (selected_cx_main == cx_matched && highlight == 1) {
            printf("\033[0;30;47m");
        }
    }

    if(put_into_cx_array == 1){
        my_data->cx_user_array[iter] = cx_matched;
    }

    if(nav_cx_display_name_return(my_data->app_intrf, cx_matched, display_name, MAX_PARAM_NAME_LENGTH) == 1){
        if (numbers == 0) {
            printf("| %s |", display_name);
        }
        else{
            printf("| %d_%s |", iter, display_name);
            my_data->user_int02 += 1;
        }
    }
    // reset highlighting
    if (selected_cx_main) {
        if (selected_cx_main == cx_matched && highlight == 1)
            printf("\033[0m");
    }
}

int main() {
    struct ui_data* user_data= calloc(1, sizeof(struct ui_data));
    if(!user_data)exit(1);

    enableRawMode();
    log_clear_logfile();
    APP_INTRF *app_intrf = app_intrf_init();
    // set the group filters for the main group
    // dont show buttons (delete, refresh and similar)
    nav_group_filter_set(app_intrf, GROUP_MAIN, 0, (INTRF_FLAG_INTERACT | INTRF_FLAG_ON_TOP));
    // set the root group filters - show on top contexts, but not buttons
    nav_group_filter_set(app_intrf, GROUP_ROOT, INTRF_FLAG_ON_TOP, INTRF_FLAG_INTERACT);
    //group for the buttons - show only buttons
    nav_group_filter_set(app_intrf, GROUP_BUTTONS, (INTRF_FLAG_ON_TOP), 0);

    // if app_intrf failed to initialize analyze the error write it and exit
    if (!app_intrf) {
        log_append_logfile("Could not start the app_intrf\n");
        exit(1);
    }

    //which group is chosen right now
    unsigned int group_curr = GROUP_MAIN;
    //the last possible group
    unsigned int group_max = GROUP_BUTTONS;
    while (1) {
        // erase the terminal
        printf("\033[2J\033[H");
        // update the interface, of course should be in a loop
        nav_update(app_intrf);
        // clean the user_data struct
        ui_data_clean(user_data, app_intrf);

        // this is the MAIN UI GROUP 
        CX *cx_curr_main = nav_cx_curr_return(app_intrf, GROUP_MAIN);
        CX *selected_cx_main = nav_cx_selected_return(app_intrf, cx_curr_main, GROUP_MAIN);
        if (cx_curr_main){
            char display_name[MAX_PARAM_NAME_LENGTH];
            if (nav_cx_display_name_return(app_intrf, cx_curr_main, display_name,
                                           MAX_PARAM_NAME_LENGTH) == 1) {
                if(group_curr == GROUP_MAIN){
                    printf("---> %s\n", display_name);
                }
                else{
                    printf("%s\n", display_name);
                }
            }
            // reset highlighting
            printf("\033[0m");
        }
        if (cx_curr_main && selected_cx_main) {
            // first find the selected cx iteration
            user_data->cx_user = selected_cx_main;
            int selected_iter = -1;
            nav_cx_children_match_callback(app_intrf, cx_curr_main, GROUP_MAIN, (void*)user_data, ui_cx_match_iter);
            selected_iter = user_data->user_int02;
            // calc the min and max cx to show
            int count = user_data->user_int01;
            user_data->user_int03 = count;
            user_data->user_int04 = 0;
            // do wee need to highlight the selected item in the main group
            if(group_curr == GROUP_MAIN)
                user_data->user_int04 = 1;
            user_data->user_int01 = 0;

            nav_cx_children_match_callback(app_intrf, cx_curr_main, GROUP_MAIN, (void*)user_data, ui_cx_match_print_names);
        }
        //----------------------------------------------------------------------------------------------------

        // this is the BUTTON UI GROUP
        printf("--------------------------------------------------\n");
        if(group_curr == GROUP_BUTTONS)
            printf("---> ");
        ui_data_clean(user_data, app_intrf);
        // sync the cx_curr of the buttons group to the main group
        nav_cx_curr_change(app_intrf, cx_curr_main, GROUP_BUTTONS);
        CX* cx_curr_buttons = nav_cx_curr_return(app_intrf, GROUP_BUTTONS);
        CX* cx_selected_buttons = nav_cx_selected_return(app_intrf, cx_curr_buttons, GROUP_BUTTONS);
        user_data->cx_user = cx_selected_buttons;
        // higlight the selected item or no in the buttons group
        if(group_curr == GROUP_BUTTONS)
            user_data->user_int04 = 1;
        nav_cx_on_top_match_callback(app_intrf, cx_curr_buttons, GROUP_BUTTONS, (void*)user_data, ui_cx_match_print_names_horizontal);
        printf("\n--------------------------------------------------\n");
        //----------------------------------------------------------------------------------------------------

        // this is the ROOT UI GROUP
        printf("--------------------------------------------------\n");
        ui_data_clean(user_data, app_intrf);
        // dont highlight anything
        user_data->user_int04 = 0;
        // put a number in front of the context name
        user_data->user_int01 = 1;
        // put the matched contexts into the cx_user_array for navigation
        user_data->user_int03 = 1;
        CX* cx_curr_root = nav_cx_root_return(app_intrf);
        nav_cx_children_match_callback(app_intrf, cx_curr_root, GROUP_ROOT, (void*)user_data, ui_cx_match_print_names_horizontal);
        printf("\n--------------------------------------------------\n");
        //----------------------------------------------------------------------------------------------------

        // get user inputs
        int input = getchar();
        unsigned int exit = 0;

        // set vars for what is possible for the current group
        unsigned int cx_curr_exit = 1;
        CX* cx_selected = selected_cx_main;
        CX* cx_curr = cx_curr_main;
        switch (group_curr){
            case GROUP_BUTTONS:
                cx_curr = cx_curr_buttons;
                cx_selected = cx_selected_buttons;
                // in the buttons group cannot exit to upper layer then the cx_curr in the main group
                // not really necessary precaution, since in the button group it should be impossible to enter the contexts
                if(cx_curr_buttons == cx_curr_main)
                    cx_curr_exit = 0;
        }

        switch (input) {
        // Current UI GROUP navigation
        // TODO would be better to use json conf to set the keybindings
        case 'J':
            group_curr += 1;
            if (group_curr > group_max)
                group_curr = GROUP_MAIN;
            break;
        case 'K':
            if (group_curr > GROUP_MAIN)
                group_curr -= 1;
            break;
        case 'j':
            nav_cx_selected_next(app_intrf, cx_curr, group_curr);
            break;
        case 'k':
            nav_cx_selected_prev(app_intrf, cx_curr, group_curr);
            break;
        case 'l':
            if (nav_cx_enter(app_intrf, cx_selected, group_curr) == -1)
                exit = 1;
            break;
        case 'h':
            if (cx_curr_exit == 1) {
                if (nav_cx_curr_exit(app_intrf, group_curr) == -1)
                    exit = 1;
            }
            break;
        default:
            // if a number 0..9 is entered, enter the cx in the GROUP_ROOT
            // but dont change the cx_curr in GROUP_ROOT - change it in the
            // GROUP_MAIN
            if (input >= '0' && input <= '9') {
                int digit = input - '0';
                nav_cx_enter(app_intrf, user_data->cx_user_array[digit],
                             GROUP_MAIN);
            }
        }

        if (exit == 1)
            break;
    }

    app_intrf_destroy(app_intrf);

    log_append_logfile("Cleaned everything, closing the app \n");
    disableRawMode();

    free(user_data);

    return 0;
}
