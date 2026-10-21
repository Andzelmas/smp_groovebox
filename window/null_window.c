#include <stdlib.h>
// my libraries
#include "host_window.h"

// no window system: nothing connects, no window is made

typedef struct _hwin_sys {
    int unused;
} HWIN_SYS;

HWIN_SYS *hwin_sys_init(void) { return calloc(1, sizeof(HWIN_SYS)); }

bool hwin_sys_open(HWIN_SYS *sys) {
    (void)sys;
    return false;
}

int hwin_sys_fd(HWIN_SYS *sys) {
    (void)sys;
    return -1;
}

const char *hwin_sys_api(HWIN_SYS *sys) {
    (void)sys;
    return NULL;
}

void hwin_sys_process(HWIN_SYS *sys) { (void)sys; }

void hwin_sys_clean(HWIN_SYS *sys) { free(sys); }

HWIN *hwin_create(HWIN_SYS *sys, const char *title, uint32_t w, uint32_t h,
                  uint32_t flags, HWIN_FUNCS funcs, void *user_data) {
    (void)sys;
    (void)title;
    (void)w;
    (void)h;
    (void)flags;
    (void)funcs;
    (void)user_data;
    return NULL;
}

uintptr_t hwin_native(const HWIN *win) {
    (void)win;
    return 0;
}

void hwin_resize(HWIN *win, uint32_t w, uint32_t h) {
    (void)win;
    (void)w;
    (void)h;
}

void hwin_set_resizable(HWIN *win, bool resizable) {
    (void)win;
    (void)resizable;
}

void hwin_show(HWIN *win) { (void)win; }

void hwin_hide(HWIN *win) { (void)win; }

void hwin_destroy(HWIN *win) { (void)win; }
