#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
// my libraries
#include "host_window.h"
#include "../types.h"
#include "../util_funcs/log_funcs.h"

#define HWIN_MAX 64
// our resizes the server has not reported yet
#define HWIN_REQS_MAX 4

typedef struct {
    uint32_t w;
    uint32_t h;
} HWIN_SIZE;

typedef struct _hwin {
    HWIN_SYS *sys;
    // None = a free slot
    Window xwin;
    uint32_t flags;
    HWIN_FUNCS funcs;
    void *user_data;
    // the size the server last reported, or the created one
    HWIN_SIZE size;
    // our resizes not reported yet, oldest first, and the newest one's
    // request serial - an event before it is stale
    HWIN_SIZE reqs[HWIN_REQS_MAX];
    uint32_t reqs_count;
    unsigned long req_serial;
    // HWIN_FIT_CHILD: the child followed, None while none
    Window child;
} HWIN;

typedef struct _hwin_sys {
    Display *dpy;
    XErrorHandler old_error_handler;
    Atom wm_protocols;
    Atom wm_delete_window;
    Atom net_wm_name;
    Atom utf8_string;
    // one past the last used slot
    uint32_t wins_count;
    HWIN wins[HWIN_MAX];
} HWIN_SYS;

// every connection's in the process, the plugins' too
static int hwin_x_error(Display *dpy, XErrorEvent *ev) {
    char text[128];
    XGetErrorText(dpy, ev->error_code, text, sizeof(text));
    log_append_logfile("X error: %s, request %u.%u, resource 0x%lx\n", text,
                       ev->request_code, ev->minor_code, ev->resourceid);
    return 0;
}

HWIN_SYS *hwin_sys_init(void) {
    HWIN_SYS *sys = calloc(1, sizeof(HWIN_SYS));
    if (!sys)
        return NULL;
    sys->old_error_handler = XSetErrorHandler(hwin_x_error);
    return sys;
}

bool hwin_sys_open(HWIN_SYS *sys) {
    if (!sys)
        return false;
    if (sys->dpy)
        return true;
    sys->dpy = XOpenDisplay(NULL);
    if (!sys->dpy)
        return false;
    char *names[] = {"WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME",
                     "UTF8_STRING"};
    Atom atoms[4] = {None};
    XInternAtoms(sys->dpy, names, 4, False, atoms);
    sys->wm_protocols = atoms[0];
    sys->wm_delete_window = atoms[1];
    sys->net_wm_name = atoms[2];
    sys->utf8_string = atoms[3];
    return true;
}

int hwin_sys_fd(HWIN_SYS *sys) {
    if (!sys || !sys->dpy)
        return -1;
    return ConnectionNumber(sys->dpy);
}

const char *hwin_sys_api(HWIN_SYS *sys) { return sys ? "x11" : NULL; }

static bool hwin_live(const HWIN *win) { return win && win->xwin != None; }

static HWIN *hwin_find(HWIN_SYS *sys, Window xwin) {
    if (xwin == None)
        return NULL;
    for (uint32_t i = 0; i < sys->wins_count; i++) {
        if (sys->wins[i].xwin == xwin)
            return &(sys->wins[i]);
    }
    return NULL;
}

static HWIN_SIZE hwin_size_make(uint32_t w, uint32_t h) {
    return (HWIN_SIZE){.w = w > 0 ? w : 1, .h = h > 0 ? h : 1};
}

static bool hwin_size_eq(HWIN_SIZE a, HWIN_SIZE b) {
    return a.w == b.w && a.h == b.h;
}

// the size the window ends at once our resizes are through
static HWIN_SIZE hwin_size_target(const HWIN *win) {
    return win->reqs_count > 0 ? win->reqs[win->reqs_count - 1] : win->size;
}

static void hwin_size_hints(HWIN *win, HWIN_SIZE size) {
    XSizeHints hints = {0};
    if ((win->flags & HWIN_RESIZABLE) == 0) {
        hints.flags = PMinSize | PMaxSize;
        hints.min_width = hints.max_width = (int)size.w;
        hints.min_height = hints.max_height = (int)size.h;
    }
    XSetWMNormalHints(win->sys->dpy, win->xwin, &hints);
}

// what only a top-level has, for the window manager
static void hwin_toplevel_setup(HWIN *win, const char *title) {
    Display *dpy = win->sys->dpy;
    XSetWMProtocols(dpy, win->xwin, &(win->sys->wm_delete_window), 1);
    XStoreName(dpy, win->xwin, title);
    XChangeProperty(dpy, win->xwin, win->sys->net_wm_name,
                    win->sys->utf8_string, 8, PropModeReplace,
                    (const unsigned char *)title, (int)strlen(title));
    XWMHints wm_hints = {.flags = InputHint | StateHint,
                         .input = True,
                         .initial_state = NormalState};
    XSetWMHints(dpy, win->xwin, &wm_hints);
    XClassHint class_hint = {.res_name = APP_NAME, .res_class = APP_NAME};
    XSetClassHint(dpy, win->xwin, &class_hint);
    hwin_size_hints(win, win->size);
}

HWIN *hwin_create(HWIN_SYS *sys, const char *title, uint32_t w, uint32_t h,
                  uint32_t flags, HWIN_FUNCS funcs, void *user_data) {
    if (!sys || !sys->dpy)
        return NULL;
    uint32_t slot = 0;
    while (slot < sys->wins_count && sys->wins[slot].xwin != None)
        slot++;
    if (slot == HWIN_MAX) {
        log_append_logfile("No room for another host window\n");
        return NULL;
    }
    Display *dpy = sys->dpy;
    int screen = DefaultScreen(dpy);
    HWIN_SIZE size = hwin_size_make(w, h);
    XSetWindowAttributes attrs = {
        .background_pixel = BlackPixel(dpy, screen),
        .event_mask = StructureNotifyMask | ((flags & HWIN_FIT_CHILD)
                                                 ? SubstructureNotifyMask
                                                 : 0)};
    Window xwin = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, size.w,
                                size.h, 0, CopyFromParent, InputOutput,
                                CopyFromParent, CWBackPixel | CWEventMask,
                                &attrs);
    HWIN *win = &(sys->wins[slot]);
    *win = (HWIN){.sys = sys,
                  .xwin = xwin,
                  .flags = flags,
                  .funcs = funcs,
                  .user_data = user_data,
                  .size = size,
                  .child = None};
    if (slot == sys->wins_count)
        sys->wins_count++;
    hwin_toplevel_setup(win, title ? title : APP_NAME);
    // a plugin's own connection parents into it next
    XSync(dpy, False);
    return win;
}

uintptr_t hwin_native(const HWIN *win) {
    return hwin_live(win) ? (uintptr_t)win->xwin : 0;
}

void hwin_resize(HWIN *win, uint32_t w, uint32_t h) {
    if (!hwin_live(win))
        return;
    HWIN_SIZE size = hwin_size_make(w, h);
    if (hwin_size_eq(size, hwin_size_target(win)))
        return;
    if (win->reqs_count == HWIN_REQS_MAX) {
        memmove(win->reqs, win->reqs + 1,
                (HWIN_REQS_MAX - 1) * sizeof(HWIN_SIZE));
        win->reqs_count--;
    }
    win->reqs[win->reqs_count++] = size;
    Display *dpy = win->sys->dpy;
    // before the resize, so the window manager allows it
    if ((win->flags & HWIN_RESIZABLE) == 0)
        hwin_size_hints(win, size);
    win->req_serial = NextRequest(dpy);
    XResizeWindow(dpy, win->xwin, size.w, size.h);
    XFlush(dpy);
}

void hwin_set_resizable(HWIN *win, bool resizable) {
    if (!hwin_live(win))
        return;
    if (resizable)
        win->flags |= HWIN_RESIZABLE;
    else
        win->flags &= ~(uint32_t)HWIN_RESIZABLE;
    hwin_size_hints(win, hwin_size_target(win));
    XFlush(win->sys->dpy);
}

void hwin_show(HWIN *win) {
    if (!hwin_live(win))
        return;
    XMapWindow(win->sys->dpy, win->xwin);
    XFlush(win->sys->dpy);
}

void hwin_hide(HWIN *win) {
    if (!hwin_live(win))
        return;
    XWithdrawWindow(win->sys->dpy, win->xwin, DefaultScreen(win->sys->dpy));
    XFlush(win->sys->dpy);
}

void hwin_destroy(HWIN *win) {
    if (!hwin_live(win))
        return;
    HWIN_SYS *sys = win->sys;
    XDestroyWindow(sys->dpy, win->xwin);
    // an error lands inside this call
    XSync(sys->dpy, False);
    win->xwin = None;
    while (sys->wins_count > 0 && sys->wins[sys->wins_count - 1].xwin == None)
        sys->wins_count--;
}

static void hwin_configured(HWIN *win, const XConfigureEvent *ev) {
    // our newest resize overrides it
    if (ev->serial < win->req_serial)
        return;
    HWIN_SIZE size = {.w = (uint32_t)ev->width, .h = (uint32_t)ev->height};
    // a move, or the window manager holding its size
    if (hwin_size_eq(size, win->size))
        return;
    win->size = size;
    for (uint32_t i = 0; i < win->reqs_count; i++) {
        if (!hwin_size_eq(size, win->reqs[i]))
            continue;
        // ours, the older ones passed on the way
        win->reqs_count -= i + 1;
        memmove(win->reqs, win->reqs + i + 1,
                win->reqs_count * sizeof(HWIN_SIZE));
        return;
    }
    win->reqs_count = 0;
    if (win->funcs.on_resize)
        win->funcs.on_resize(win->user_data, size.w, size.h);
}

static void hwin_fit(HWIN *win, int w, int h, int border) {
    hwin_resize(win, (uint32_t)(w + 2 * border), (uint32_t)(h + 2 * border));
}

static void hwin_child_reparented(HWIN *win, Window child) {
    Window root;
    int x, y;
    unsigned int w, h, border, depth;
    if (!XGetGeometry(win->sys->dpy, child, &root, &x, &y, &w, &h, &border,
                      &depth))
        return;
    win->child = child;
    hwin_fit(win, (int)w, (int)h, (int)border);
}

// returns after a callback, which may have destroyed win
static void hwin_event(HWIN *win, const XEvent *ev) {
    HWIN_SYS *sys = win->sys;
    switch (ev->type) {
    case ClientMessage:
        if (ev->xclient.message_type == sys->wm_protocols &&
            ev->xclient.format == 32 &&
            (Atom)ev->xclient.data.l[0] == sys->wm_delete_window &&
            win->funcs.on_close)
            win->funcs.on_close(win->user_data);
        break;
    case ConfigureNotify:
        if (ev->xconfigure.window == win->xwin)
            hwin_configured(win, &(ev->xconfigure));
        else if (ev->xconfigure.window == win->child)
            hwin_fit(win, ev->xconfigure.width, ev->xconfigure.height,
                     ev->xconfigure.border_width);
        break;
    // the child events come only with HWIN_FIT_CHILD
    case CreateNotify:
        if (win->child == None) {
            win->child = ev->xcreatewindow.window;
            hwin_fit(win, ev->xcreatewindow.width, ev->xcreatewindow.height,
                     ev->xcreatewindow.border_width);
        }
        break;
    case ReparentNotify:
        if (ev->xreparent.window == win->child &&
            ev->xreparent.parent != win->xwin)
            win->child = None;
        else if (win->child == None && ev->xreparent.parent == win->xwin)
            hwin_child_reparented(win, ev->xreparent.window);
        break;
    case DestroyNotify:
        if (ev->xdestroywindow.window == win->child)
            win->child = None;
        break;
    }
}

void hwin_sys_process(HWIN_SYS *sys) {
    if (!sys || !sys->dpy)
        return;
    // XPending flushes too
    while (XPending(sys->dpy) > 0) {
        XEvent ev;
        XNextEvent(sys->dpy, &ev);
        HWIN *win = hwin_find(sys, ev.xany.window);
        if (win)
            hwin_event(win, &ev);
    }
}

void hwin_sys_clean(HWIN_SYS *sys) {
    if (!sys)
        return;
    if (sys->dpy)
        XCloseDisplay(sys->dpy);
    XSetErrorHandler(sys->old_error_handler);
    free(sys);
}
