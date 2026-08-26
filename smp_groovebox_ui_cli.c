#include "ui_layer.h"
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

int main() {
    enableRawMode();
    log_clear_logfile();
    UI_LAYER* ui_layer = ui_layer_init(2);

    // if app_intrf failed to initialize analyze the error write it and exit
    if (!ui_layer) {
        log_append_logfile("Could not start the ui_layer\n");
        exit(1);
    }

    //which group is chosen right now
    unsigned int group_curr = GROUP_MAIN;
    while (1) {
        // erase the terminal
        //printf("\033[2J\033[H");
        // update the interface, of course should be in a loop
        ui_layer_update_cycle(ui_layer);

        // get user inputs
        int input = getchar();
        unsigned int exit = 0;

        switch (input) {
        // Current UI GROUP navigation
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

    ui_layer_destroy(ui_layer);

    log_append_logfile("Cleaned everything, closing the app \n");
    disableRawMode();

    return 0;
}
