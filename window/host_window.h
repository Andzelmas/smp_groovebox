#pragma once
#include <stdbool.h>
#include <stdint.h>

// Host windows for plugin GUIs, the window system picked at compile time
// (x11_window.c, or null_window.c without one). Every function is
// [main-thread].

typedef struct _hwin_sys HWIN_SYS;
typedef struct _hwin HWIN;

// called from hwin_sys_process, either may be NULL. A callback may destroy
// its own window
typedef struct _hwin_funcs {
    // the user or the window manager asks to close it, the window stays
    // until hwin_destroy
    void (*on_close)(void *user_data);
    // a size the user or the window manager made, never one hwin_resize set
    void (*on_resize)(void *user_data, uint32_t w, uint32_t h);
} HWIN_FUNCS;

enum hwinFlags {
    // else the window manager is asked to keep the size set
    HWIN_RESIZABLE = 1 << 0,
    // follows the size of its first child window
    HWIN_FIT_CHILD = 1 << 1,
};

// connects nothing yet. Installs a process wide error handler that logs and
// never exits. NULL on failure
HWIN_SYS *hwin_sys_init(void);
// connects once, true while connected. Without a window system false, a
// later call tries again
bool hwin_sys_open(HWIN_SYS *sys);
// readable -> hwin_sys_process. -1 while not connected
int hwin_sys_fd(HWIN_SYS *sys);
// the window api a plugin is asked about ("x11"), NULL without one. Known
// before hwin_sys_open
const char *hwin_sys_api(HWIN_SYS *sys);
// handles every pending event, events already read included
void hwin_sys_process(HWIN_SYS *sys);
// its windows destroyed already
void hwin_sys_clean(HWIN_SYS *sys);

// a top-level window, not shown, known to the server when this returns. NULL
// while not connected or on failure
HWIN *hwin_create(HWIN_SYS *sys, const char *title, uint32_t w, uint32_t h,
                  uint32_t flags, HWIN_FUNCS funcs, void *user_data);
// the native window a plugin embeds into (X11: the Window)
uintptr_t hwin_native(const HWIN *win);
void hwin_resize(HWIN *win, uint32_t w, uint32_t h);
void hwin_set_resizable(HWIN *win, bool resizable);
void hwin_show(HWIN *win);
void hwin_hide(HWIN *win);
// win is invalid after
void hwin_destroy(HWIN *win);
