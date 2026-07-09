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
    GROUP_ROOT = 1
};

// used for callback functions
// the CX* in this struct must be set to NULL at the beginning of each loop
// the CX* can be removed or their addresses changed between the loops
struct ui_data{
    int user_int01;
    int user_int02;
    int user_int03;
    int user_int04;
    CX* cx_user;
    APP_INTRF* app_intrf;
};

// clean the user_data struct
static void ui_data_clean(struct ui_data* my_data){
    my_data->user_int01 = 0;
    my_data->user_int02 = 0;
    my_data->user_int03 = 0;
    my_data->user_int04 = 0;
    my_data->cx_user = NULL;
    my_data->app_intrf = NULL;
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
    int min_cx = my_data->user_int03;
    int max_cx = my_data->user_int04;
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
    if (selected_cx_main == cx_matched) {
        printf("\033[0;30;47m");
    }
    printf("     |");

    // show the name of the context
    if (nav_cx_display_name_return(my_data->app_intrf, cx_matched, display_name,
                                   MAX_PARAM_NAME_LENGTH) == 1) {
        printf("--> %s", display_name);
    }
    // reset highlighting
    if (selected_cx_main == cx_matched)
        printf("\033[0m");
    printf("\n");

    my_data->user_int01 += 1;
}

int main() {
    struct ui_data* user_data= calloc(1, sizeof(struct ui_data));
    if(!user_data)exit(1);

    enableRawMode();
    log_clear_logfile();
    APP_INTRF *app_intrf = app_intrf_init();
    // set the group filters for the main group
    // dont show buttons that should be on top (delete, refresh and similar)
    nav_group_filter_set(app_intrf, GROUP_MAIN, (INTRF_FLAG_INTERACT | INTRF_FLAG_ON_TOP), false);

    // set the gorup filters
    nav_group_filter_set(app_intrf, GROUP_ROOT, INTRF_FLAG_ON_TOP, true);

    // if app_intrf failed to initialize analyze the error write it and exit
    if (!app_intrf) {
        log_append_logfile("Could not start the app_intrf\n");
        exit(1);
    }

    while (1) {
        // erase the terminal
        printf("\033[2J\033[H");
        // update the interface, of course should be in a loop
        nav_update(app_intrf);
        // clean the user_data struct
        ui_data_clean(user_data);
        user_data->app_intrf = app_intrf;

        // this is the MAIN UI GROUP 
        CX *cx_curr_main = nav_cx_curr_return(app_intrf, GROUP_MAIN);
        CX *selected_cx_main = nav_cx_selected_return(app_intrf, GROUP_MAIN);
        if (cx_curr_main){
            char display_name[MAX_PARAM_NAME_LENGTH];
            if (nav_cx_display_name_return(app_intrf, cx_curr_main, display_name,
                                           MAX_PARAM_NAME_LENGTH) == 1) {
                printf("---> %s\n", display_name);
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

            user_data->user_int03 = min_cx;
            user_data->user_int04 = max_cx;
            user_data->user_int01 = 0;

            nav_cx_children_match_callback(app_intrf, cx_curr_main, GROUP_MAIN, (void*)user_data, ui_cx_match_print_names);
        }
        //----------------------------------------------------------------------------------------------------

        // get user inputs
        char input = getchar();
        unsigned int exit = 0;
        switch (input) {
        // Current UI GROUP navigation
        // TODO currently only MAIN and ROOT GROUPS navigated, implement GROUP switching
        // TODO would be better to use json conf to set the keybindings
        case 'j':
            nav_cx_selected_next(app_intrf, GROUP_MAIN);
            break;
        case 'k':
            nav_cx_selected_prev(app_intrf, GROUP_MAIN);
            break;
        case 'l':
            if (nav_cx_enter(app_intrf, selected_cx_main, GROUP_MAIN) == -1)
                exit = 1;
            break;
        case 'h':
            if (nav_cx_curr_exit(app_intrf, GROUP_MAIN) == -1)
                exit = 1;
            break;
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
