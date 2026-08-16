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

int main() {
    enableRawMode();
    log_clear_logfile();
    APP_INTRF *app_intrf = app_intrf_init();

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

        // get user inputs
        int input = getchar();
        unsigned int exit = 0;

        switch (input) {
        // Current UI GROUP navigation
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

    app_intrf_destroy(app_intrf);

    log_append_logfile("Cleaned everything, closing the app \n");
    disableRawMode();

    return 0;
}
