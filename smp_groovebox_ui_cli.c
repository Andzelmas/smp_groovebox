#include "app_intrf.h"
#include "types.h"
#include "util_funcs/log_funcs.h"
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

int main() {
    enableRawMode();
    log_clear_logfile();
    APP_INTRF *app_intrf = app_intrf_init();
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

        // Get the context interface
        //----------------------------------------------------------------------------------------------------
        CX *cx_curr = nav_cx_curr_return(app_intrf);
        CX *selected_cx = nav_cx_selected_return(app_intrf);
        if (cx_curr && selected_cx) {
            char display_name[MAX_PARAM_NAME_LENGTH];
            if (nav_cx_display_name_return(app_intrf, cx_curr, display_name,
                                           MAX_PARAM_NAME_LENGTH) == 1) {
                printf("---> %s\n", display_name);
            }
            // reset highlighting
            printf("\033[0m");

            unsigned int count = 0;
            CX **cx_children =
                nav_cx_children_return(app_intrf, cx_curr, &count);
            // first find the selected cx iteration
            int selected_iter = -1;
            for (unsigned int i = 0; i < count; i++) {
                CX *cx_child = cx_children[i];
                if (cx_child == selected_cx) {
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
                if (selected_cx == cx_child) {
                    printf("\033[0;30;47m");
                }
                printf("     |");

                // show the name of the context
                if (nav_cx_display_name_return(app_intrf, cx_child,
                                               display_name,
                                               MAX_PARAM_NAME_LENGTH) == 1) {
                    printf("--> %s", display_name);
                }
                // reset highlighting
                if (selected_cx == cx_child)
                    printf("\033[0m");
                printf("\n");
            }
        }
        //----------------------------------------------------------------------------------------------------

        // get user inputs
        char input = getchar();
        unsigned int exit = 0;
        switch (input) {
        // TODO would be better to use json conf to set the keybindings
        case 'j':
            nav_cx_selected_next(app_intrf);
            break;
        case 'k':
            nav_cx_selected_prev(app_intrf);
            break;
        case 'l':
            if (nav_cx_selected_invoke(app_intrf) == -1)
                exit = 1;
            break;
        case 'h':
            if (nav_cx_curr_exit(app_intrf) == -1)
                exit = 1;
            break;
        }

        if (exit == 1)
            break;
    }

    app_intrf_destroy(app_intrf);

    log_append_logfile("Cleaned everything, closing the app \n");
    disableRawMode();
    return 0;
}
