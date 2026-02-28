#include "app_intrf.h"
#include "types.h"
#include "util_funcs/log_funcs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#define SELECTED_DIST                                                          \
    10 // how many items from the selected context this context should be,
       // further away contexts will not be displayed

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

int main() {
    enableRawMode();
    log_clear_logfile();
    APP_INTRF *app_intrf = app_intrf_init();
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

        // this is the MAIN UI GROUP 
        CX *cx_curr_main = nav_cx_curr_return(app_intrf, GROUP_MAIN);
        CX *selected_cx_main = nav_cx_selected_return(app_intrf, GROUP_MAIN);
        if (cx_curr_main) {
            char display_name[MAX_PARAM_NAME_LENGTH];
            if (nav_cx_display_name_return(app_intrf, cx_curr_main, display_name,
                                           MAX_PARAM_NAME_LENGTH) == 1) {
                printf("---> %s\n", display_name);
            }
            // reset highlighting
            printf("\033[0m");

            unsigned int count = 0;
            CX **cx_children =
                nav_cx_children_return(app_intrf, cx_curr_main, &count);
            if (selected_cx_main){
                // first find the selected cx iteration
                int selected_iter = -1;
                for (unsigned int i = 0; i < count; i++) {
                    CX *cx_child = cx_children[i];
                    if (cx_child == selected_cx_main) {
                        selected_iter = i;
                        break;
                    }
                }
                // calc the min and max cx to show
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

                for (unsigned int i = 0; i < count; i++) {
                    if (selected_iter == -1)
                        break;
                    CX *cx_child = cx_children[i];
                    // if a cx is too far away from the selected_cx dont show it
                    if (i < min_cx) {
                        if ((min_cx - i) == 1)
                            printf("     ...\n");
                        continue;
                    }
                    if (i > max_cx) {
                        if ((i - max_cx) == 1)
                            printf("     ...\n");
                        break;
                    }
                    // highlight the selected context
                    if (selected_cx_main == cx_child) {
                        printf("\033[0;30;47m");
                    }
                    printf("     |");

                    // show the name of the context
                    if (nav_cx_display_name_return(
                            app_intrf, cx_child, display_name,
                            MAX_PARAM_NAME_LENGTH) == 1) {
                        printf("--> %s", display_name);
                    }
                    // reset highlighting
                    if (selected_cx_main == cx_child)
                        printf("\033[0m");
                    printf("\n");
                }
            }
        }
        //----------------------------------------------------------------------------------------------------

        // this is the ROOT UI GROUP display
        // only cx with INTRF_FLAG_ON_TOP will be shown
        // TODO flags compare should be done in the nav_ function
        CX *cx_curr_root = nav_cx_curr_return(app_intrf, GROUP_ROOT);
        unsigned int root_children_count = 0;
        if(cx_curr_root){
            printf("-----------------------------------------\n");
            CX **root_children = nav_cx_children_return(app_intrf, cx_curr_root, &root_children_count);
            if (root_children && root_children_count > 0) {
                for (unsigned int child_idx = 0; child_idx < root_children_count; child_idx++) {
                    CX *cx_this = root_children[child_idx];
                    uint32_t flags = nav_cx_flags_return(app_intrf, cx_this);
                    if ((flags & INTRF_FLAG_ON_TOP) != INTRF_FLAG_ON_TOP)
                        continue;
                    char name[MAX_PARAM_NAME_LENGTH];
                    if (nav_cx_display_name_return(app_intrf, cx_this, name,
                                                   MAX_PARAM_NAME_LENGTH) ==
                        1) {
                        printf("%d - %s | ", child_idx, name);
                    }
                }
            }

            printf ("\n");
        }

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

        // navigate the GROUP ROOT
        // user can enter numbers that represent the root CX,
        // the entered CX will become the cx_curr in the GROUP_MAIN GROUP
        if (input >= '0' && input <= '9' && root_children_count > 0) {
            unsigned int num = input - '0';
            CX *root_child = NULL;
            if(nav_cx_selected_choose(app_intrf, num, GROUP_ROOT) == 1)
                root_child = nav_cx_selected_return(app_intrf, GROUP_ROOT);
            if (root_child){
                uint32_t flags = nav_cx_flags_return(app_intrf, root_child);
                if ((flags & INTRF_FLAG_ON_TOP) == INTRF_FLAG_ON_TOP) {
                    if (nav_cx_enter(app_intrf, root_child, GROUP_MAIN) == -1)
                        exit = 1;
                }
            }
        }

        if (exit == 1)
            break;
    }

    app_intrf_destroy(app_intrf);

    log_append_logfile("Cleaned everything, closing the app \n");
    disableRawMode();
    return 0;
}
