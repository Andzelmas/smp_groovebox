#include <errno.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
// my libraries
#include "app_data.h"
#include "backend/audio_backend.h"
#include "data_actions.h"
#include "engine/graph.h"
#include "engine/midi_clock.h"
#include "engine/transport.h"
#include "settings.h"
#include "song.h"
#include "window/host_window.h"
// string functions
#include "contexts/clap_plugins.h"
#include "contexts/plugins.h"
#include "types.h"
#include "util_funcs/string_funcs.h"
// math helper functions
#include "contexts/context_control.h"
#include "contexts/param_config.h"
#include "contexts/params.h"
#include "util_funcs/log_funcs.h"
#include "util_funcs/math_funcs.h"
#include "util_funcs/path_funcs.h"
#include "util_funcs/ring_buffer.h"
#include <threads.h>

// starting size of the data event queue, it grows when a cycle produces more
// (a CLAP text rescan changes every param of the plugin at once)
#define APP_DATA_EVENT_QUEUE_START 64

// param config owner kinds, a plugin's own id follows: "clap:<id>"
#define PARAM_CONFIG_KIND_CLAP "clap"
#define PARAM_CONFIG_KIND_LV2 "lv2"
#define PARAM_CONFIG_KIND_SYNTH_OSC "synth_osc"
#define PARAM_CONFIG_KIND_SAMPLER "sampler"
#define PARAM_CONFIG_KIND_TRK "trk"

typedef struct _app_info {
    // the smapler data
    SMP_INFO *smp_data;
    // the audio backend
    AUDIO_BACKEND *backend;
    // the routing of every port, run by trk_audio_process_rt
    GRAPH *graph;
    // the clock, advanced by trk_audio_process_rt before the graph
    TRANSPORT *transport;
    // MIDI clock out of the transport, a graph node
    MIDI_CLOCK *midi_clock;
    // transport_rolling as of the last app_data_update
    bool rolling_seen;
    // the Sync the backend was last told to lead for
    TransportSync sync_seen;
    // plugin data
    PLUG_INFO *plug_data;
    // CLAP plugin data
    CLAP_PLUG_INFO *clap_plug_data;
    // built in synth data
    SYNTH_DATA *synth_data;
    // control struct for sys messages between [audio-thread] and [main-thread]
    // (stop all processes and send messages for this context)
    CXCONTROL *control_data;
    unsigned int is_processing; // is the main jack function processing, should
                                // be touched only on [audio-thread]
    // what the graph and the contexts run at, follows the backend
    SAMPLE_T sample_rate;
    uint32_t buffer_size;
    // graph_generation as of the last "ports changed" check
    uint64_t graph_generation_seen;

    // data event queue (main thread only: produced in app_data_update, drained
    // by app_data_poll_event). Grows instead of dropping - a lost
    // DATA_EVENT_CHANGED would leave a view stale. Emptied once fully drained.
    DataEvent *events;
    size_t event_count; // events pushed since the queue was last empty
    size_t event_read;  // next event to read
    size_t event_cap;

    // wake fd: a top-level epoll composing clap_plug_data's own wake fd
    // (its plugins' posix-fd/timer fds) with a periodic rt->ui timerfd, for a
    // UI that wants to sleep instead of ticking. -1 until first asked for.
    int wake_epoll_fd;
    int rt_ui_timerfd;
    uint32_t rt_ui_period_ms; // last period asked for, 0 = disarmed
    // the plugin GUIs' host windows, connected by a context's first show
    HWIN_SYS *hwin;
    // hwin_sys_fd is in wake_epoll_fd
    bool hwin_fd_watched;
    // where the param config files go, "" = none written or read
    char param_config_dir[MAX_PATH_STRING];
    // the newest sample / plugin uid already given its param config
    uint32_t smp_uid_seen;
    uint32_t lv2_uid_seen;
    uint32_t clap_uid_seen;
    // saves and loads the whole song
    SONG *song;
} APP_INFO;

// push one event, growing the queue if it is full
static void app_data_push_event(APP_INFO *app_data, DataEventType type,
                                ContextId id) {
    if (!app_data)
        return;
    if (app_data->event_count == app_data->event_cap) {
        size_t new_cap = app_data->event_cap ? app_data->event_cap * 2
                                             : APP_DATA_EVENT_QUEUE_START;
        DataEvent *grown =
            realloc(app_data->events, new_cap * sizeof(DataEvent));
        // out of memory - this event is lost
        if (!grown)
            return;
        app_data->events = grown;
        app_data->event_cap = new_cap;
    }
    app_data->events[app_data->event_count].type = type;
    app_data->events[app_data->event_count].id = id;
    app_data->event_count++;
}

// clean memory, without pausing the [audio-thread]
static int clean_memory(APP_INFO *app_data) {
    if (!app_data)
        return -1;
    // clean the clap plug_data memory
    if (app_data->clap_plug_data)
        clap_plug_clean_memory(app_data->clap_plug_data);
    // clean the lv2 plug_data memory
    if (app_data->plug_data)
        plug_clean_memory(app_data->plug_data);
    // clean the sampler memory
    if (app_data->smp_data)
        smp_clean_memory(app_data->smp_data);
    // clean the synth memory
    if (app_data->synth_data)
        synth_clean_memory(app_data->synth_data);
    // the contexts destroyed their windows
    hwin_sys_clean(app_data->hwin);

    // stops the process callback, removes its endpoint nodes
    audio_backend_clean(app_data->backend);
    graph_free(app_data->graph);
    midi_clock_clean(app_data->midi_clock);
    transport_clean(app_data->transport);

    // clean the app_data
    song_clean(app_data->song);
    context_sub_clean(app_data->control_data);
    free(app_data->events);
    if (app_data->rt_ui_timerfd >= 0)
        close(app_data->rt_ui_timerfd);
    if (app_data->wake_epoll_fd >= 0)
        close(app_data->wake_epoll_fd);
    if (app_data)
        free(app_data);

    return 0;
}

// stop the whole audio process, usually to close the program
// this function is used in the CXCONTROL
static int app_stop_process(void *user_data) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data)
        return -1;
    app_data->is_processing = 0;
    return 0;
}

// start the whole audio process, usually after app_init
static int app_start_process(void *user_data) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data)
        return -1;
    app_data->is_processing = 1;
    return 0;
}

// ui function to write to the log,
// used in CXCONTROL for non-realtime messaging
static int app_sys_msg(void *user_data, const char *msg) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data)
        return -1;
    log_append_logfile("%s", msg);
    return 0;
}

// read ring buffers sent from ui to rt thread
static int app_read_rt_messages(APP_INFO *app_data) {
    if (!app_data)
        return -1;
    // first read the app_data messages
    context_sub_process_rt(app_data->control_data);
    // if the process is stopped dont process the contexts
    if (app_data->is_processing == 0)
        return 1;

    // read the CLAP plugins inner messages on the [audio-thread]
    if (clap_read_ui_to_rt_messages(app_data->clap_plug_data) != 0)
        return -1;
    // read the lv2 plugin messages on the [audio-thread]
    if (plug_read_ui_to_rt_messages(app_data->plug_data) != 0)
        return -1;
    // read the sampler messages on the [audio-thread]
    if (smp_read_ui_to_rt_messages(app_data->smp_data) != 0)
        return -1;
    // read the synth messages on the [audio-thread]
    if (synth_read_ui_to_rt_messages(app_data->synth_data) != 0)
        return -1;
    return 0;
}

// BackendTransportLeadFn, arg the TRANSPORT. [audio-thread] after the cycle
static void app_transport_lead_rt(void *arg, int64_t frame,
                                  POSITION_MUSIC *out) {
    transport_music_at_rt((TRANSPORT *)arg, frame, out);
}

// The callback function sent to the audio backend (at this time to jack)
// The audio processing happens here, only realtime functions are allowed here
static int trk_audio_process_rt(NFRAMES_T nframes, void *arg) {
    // get the app data
    APP_INFO *app_data = (APP_INFO *)arg;
    if (app_data == NULL) {
        return -1;
    }
    // process messages from ui to rt thread, like start processing a plugin,
    // stop_processing a plugin, update rt param values etc. if returns a 1
    // value, this means that is_processing is 0 and the function should not
    // process any contexts a value of -1 means that a fundamental error occured
    int read_err = app_read_rt_messages(app_data);
    if (read_err == 1)
        return 0;
    if (read_err == -1)
        return -1;

    // the position every node reads this cycle
    POSITION_SERVER server;
    bool has_server =
        audio_backend_transport_read_rt(app_data->backend, &server);
    transport_cycle_rt(app_data->transport, nframes,
                       has_server ? &server : NULL);
    // every node in edge order, the backend's endpoints included
    graph_process_rt(app_data->graph, nframes);
    return 0;
}

/* ----------------------------------------------------------------------------
 * DataObject adapter tables.
 *
 * app_data is the single place that knows how to map program state onto the
 * DataObject contract. The context modules (plugins.c, clap_plugins.c, ...)
 * stay unaware of DataObject: they only expose their own domain API and this
 * file adapts it.
 * ------------------------------------------------------------------------- */

/* Identity composition. A ContextId is 8 bits of namespace (which module /
 * whether it is a singleton) in the top byte, plus a local part below: a
 * fixed constant for singletons, a module uid for the rest. app_data is the
 * only place this is assembled; every layer above treats the ContextId as
 * opaque. Namespaces start at 1 so a real id always has a non-zero namespace. */
enum {
    DATA_NS_SINGLETON = 1,
    DATA_NS_SAMPLE = 2,
    DATA_NS_LV2_PLUG = 3,
    DATA_NS_CLAP_PLUG = 4,
    DATA_NS_SYNTH_OSC = 5,
    DATA_NS_PARAM = 6,
    DATA_NS_PARAM_CATEGORY = 7,
    // never a context: groups ports of other programs, so their group_key
    // cannot land on an owner's ContextId
    DATA_NS_JACK_CLIENT = 8,
};

// mask for everything below the namespace byte (56 bits). MAKE_ID keeps all
// of them rather than truncating local to uint32_t: every existing local is
// a uint32_t module uid, well inside 56 bits, so this is a no-op for those -
// but a hash-derived local (see fnv1a64 below) keeps its full width instead
// of losing 24 bits for nothing.
#define CTXID_LOCAL_MASK (((ContextId)1 << CTXID_NS_SHIFT) - 1)
#define MAKE_ID(ns, local)                                                      \
    (((ContextId)(ns) << CTXID_NS_SHIFT) | ((ContextId)(local) & CTXID_LOCAL_MASK))

// local part for the DATA_NS_SINGLETON namespace (one per singleton context)
enum {
    SID_ROOT = 1,
    SID_SAMPLER,
    SID_LV2_PLUGINS,
    SID_CLAP_PLUGINS,
    SID_SYNTH,
    SID_TRK,
};

/* DataListId composition mirrors ContextId. A namespace is one key space for
 * DataChoice.value, shared by every list in it. List kinds are static
 * constants, not counter-allocated: there is exactly one "LV2 catalogue" list
 * etc. for the life of the program. */
enum {
    DATA_LIST_NS_LV2_CATALOG = 1,
    DATA_LIST_NS_PORTS = 2,
    DATA_LIST_NS_PARAM_CHOICE = 3,
    DATA_LIST_NS_CLAP_CATALOG = 4,
    DATA_LIST_NS_BRANCH = 5, // the category rows (DATA_CHOICE_BRANCH) of every list
    DATA_LIST_NS_LV2_PRESET = 6,
    DATA_LIST_NS_CLAP_PRESET = 7,
    DATA_LIST_NS_GRAPH_PORTS = 8,
};
#define MAKE_LIST_ID(ns, local)                                                \
    (((DataListId)(ns) << CTXID_NS_SHIFT) | ((DataListId)(local) & CTXID_LOCAL_MASK))

#define LIST_LV2_CATALOG MAKE_LIST_ID(DATA_LIST_NS_LV2_CATALOG, 1)
#define LIST_CLAP_CATALOG MAKE_LIST_ID(DATA_LIST_NS_CLAP_CATALOG, 1)

// single static list id, reused by every enum param's SET_CHOICE arg - the
// param itself (via list_at's own user_data) chooses the container
#define LIST_PARAM_CHOICES MAKE_LIST_ID(DATA_LIST_NS_PARAM_CHOICE, 1)

// local part for the port namespaces: DATA_LIST_NS_PORTS (the backend's) and
// DATA_LIST_NS_GRAPH_PORTS. Plain static constants (not per-instance). The
// CONNECT actions narrow their *contents* dynamically via `partial`, not by
// minting a different DataListId per query.
enum {
    LID_PORTS_ANY = 1, // graph: every port
    LID_PORTS_EXPOSED, // backend: the app's own
    LID_PORTS_PEERS,
};
#define LIST_GRAPH_PORTS MAKE_LIST_ID(DATA_LIST_NS_GRAPH_PORTS, LID_PORTS_ANY)
#define LIST_GRAPH_PEERS MAKE_LIST_ID(DATA_LIST_NS_GRAPH_PORTS, LID_PORTS_PEERS)
#define LIST_EXTR_PORTS MAKE_LIST_ID(DATA_LIST_NS_PORTS, LID_PORTS_EXPOSED)
#define LIST_EXTR_PEERS MAKE_LIST_ID(DATA_LIST_NS_PORTS, LID_PORTS_PEERS)

// every loaded plugin's preset list - the plugin in user_data picks whose
#define LIST_LV2_PRESETS MAKE_LIST_ID(DATA_LIST_NS_LV2_PRESET, 1)
#define LIST_CLAP_PRESETS MAKE_LIST_ID(DATA_LIST_NS_CLAP_PRESET, 1)

// one parameter, from any owner. user_data is the opaque handle from
// param_get_handle.
static ContextId cx_param_id(void *user_data) {
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return MAKE_ID(DATA_NS_PARAM, 0);
    return MAKE_ID(DATA_NS_PARAM, param_get_uid(container, val_id, 0));
}
static const char *cx_param_name(void *user_data) {
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return NULL;
    return param_get_name(container, val_id);
}
static const char *cx_param_value_as_string(void *user_data) {
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return NULL;
    return param_get_value_as_string(container, val_id,
                                     param_get_value(container, val_id, 0));
}
static bool cx_param_value_range(void *user_data, double *out_min,
                                 double *out_max, double *out_inc) {
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return false;
    if (out_min)
        *out_min = (double)param_get_min(container, val_id);
    if (out_max)
        *out_max = (double)param_get_max(container, val_id);
    if (out_inc)
        *out_inc = (double)param_get_increment(container, val_id);
    return true;
}
static bool cx_param_is_hidden(void *user_data) {
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return false;
    return param_is_hidden(container, val_id) != 0;
}
// DATA_CAP_ACTIONS: every param can SET_VALUE/ADJUST_VALUE; an enum param
// (PARAM_FLAG_ENUM) additionally gets SET_CHOICE. All three disabled if the
// param is readonly (PARAM_FLAG_READONLY).
static size_t cx_param_action_list(void *user_data, void *root_user_data,
                                   DataAction *out, size_t cap) {
    (void)root_user_data;
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return 0;
    if (!out || cap < 1)
        return 0;
    bool writable = !param_is_readonly(container, val_id);
    size_t n = 0;
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_SET_VALUE,
            .label = "Set",
            .tooltip = "Set this parameter to a specific value",
            .enabled = writable,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_ADJUST_VALUE,
            .label = "Adjust",
            .tooltip = "Increase or decrease this parameter by one increment",
            .enabled = writable,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (param_is_enum(container, val_id) && n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_SET_CHOICE,
            .label = "Choose",
            .tooltip = "Pick this parameter's value from a list",
            .enabled = writable,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_COMMAND,
            .label = "Default",
            .tooltip = "Set value to default",
            .enabled = writable,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    return n;
}
static size_t cx_param_action_args(void *user_data, DataActionType type,
                                   uint32_t id, DataArgSpec *out, size_t cap) {
    (void)user_data;
    (void)id;
    if (!out || cap < 1)
        return 0;
    if (type == DATA_ACTION_SET_VALUE) {
        out[0] = (DataArgSpec){.name = "value", .label = "Value",
                               .kind = DATA_ARG_NUMBER, .required = true,
                               .list = DATA_LIST_NONE};
        return 1;
    }
    if (type == DATA_ACTION_ADJUST_VALUE) {
        out[0] = (DataArgSpec){.name = "step", .label = "Step",
                               .kind = DATA_ARG_NUMBER, .required = true,
                               .list = DATA_LIST_NONE};
        return 1;
    }
    if (type == DATA_ACTION_SET_CHOICE) {
        out[0] = (DataArgSpec){.name = "choice", .label = "Choice",
                               .kind = DATA_ARG_CHOICE, .required = true,
                               .list = LIST_PARAM_CHOICES};
        return 1;
    }
    return 0;
}
// flat list: any branch but the top level is unknown, so empty
static size_t cx_param_list_count(void *user_data, DataListId list,
                                  const DataActionReq *partial,
                                  uint64_t branch) {
    (void)partial;
    if (list != LIST_PARAM_CHOICES || branch != 0)
        return 0;
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return 0;
    PARAM_T min = param_get_min(container, val_id);
    PARAM_T max = param_get_max(container, val_id);
    if (max < min)
        return 0;
    return (size_t)(max - min) + 1;
}
static bool cx_param_list_at(void *user_data, DataListId list,
                             const DataActionReq *partial, uint64_t branch,
                             size_t idx, DataChoice *out) {
    (void)partial;
    if (list != LIST_PARAM_CHOICES || branch != 0)
        return false;
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return false;
    PARAM_T min = param_get_min(container, val_id);
    PARAM_T max = param_get_max(container, val_id);
    PARAM_T val = min + (PARAM_T)idx;
    if (val > max)
        return false;
    const char *label = param_get_value_as_string(container, val_id, val);
    if (!label)
        return false;
    // the raw integer param value (not a position) - CLAP requires IS_ENUM
    // to imply IS_STEPPED, so every value in [min,max] is a whole number.
    // Namespaced, so an enum value of 0 is not a 0 choice value
    out->value = MAKE_ID(DATA_LIST_NS_PARAM_CHOICE, (uint32_t)(int)val);
    out->label = label;
    out->flags = 0;
    return true;
}
static DataActionResult cx_param_action_do(void *user_data,
                                           void *root_user_data,
                                           const DataActionReq *req,
                                           ContextId *out_new) {
    (void)root_user_data;
    (void)out_new; // none of these actions create anything
    if (!req)
        return DATA_ACTION_ERR_INVALID;
    PRM_CONTAIN *container;
    int val_id;
    if (!param_handle_resolve(user_data, &container, &val_id))
        return DATA_ACTION_ERR_INVALID;
    if (param_is_readonly(container, val_id))
        return DATA_ACTION_ERR_NOT_ALLOWED;
    if (req->type == DATA_ACTION_SET_VALUE) {
        if (param_set_value(container, val_id, (PARAM_T)req->set_value.value,
                            NULL, Operation_SetValue) != 0)
            return DATA_ACTION_ERR_DATA;
        return DATA_ACTION_OK;
    }
    if (req->type == DATA_ACTION_COMMAND) {
        if (param_set_value(container, val_id, 0.0, NULL, Operation_DefValue) !=
            0)
            return DATA_ACTION_ERR_DATA;
        return DATA_ACTION_OK;
    }
    if (req->type == DATA_ACTION_ADJUST_VALUE) {
        int step = req->adjust_value.step;
        if (step == 0)
            return DATA_ACTION_ERR_INVALID;
        unsigned char op = step > 0 ? Operation_Increase : Operation_Decrease;
        PARAM_T amount = (PARAM_T)(step > 0 ? step : -step);
        if (param_set_value(container, val_id, amount, NULL, op) != 0)
            return DATA_ACTION_ERR_DATA;
        return DATA_ACTION_OK;
    }
    if (req->type == DATA_ACTION_SET_CHOICE) {
        ContextId value = (ContextId)req->add_choice.choice_value;
        if (CTXID_NS(value) != DATA_LIST_NS_PARAM_CHOICE)
            return DATA_ACTION_ERR_INVALID; // not a choice from this list
        int enum_val = (int)(uint32_t)(value & CTXID_LOCAL_MASK);
        if (param_set_value(container, val_id, (PARAM_T)enum_val, NULL,
                            Operation_SetValue) != 0)
            return DATA_ACTION_ERR_DATA;
        return DATA_ACTION_OK;
    }
    return DATA_ACTION_ERR_INVALID;
}
// owner-agnostic: every owner's params share this one table
static const DataOps cx_param_ops = {
    .capabilities =
        DATA_CAP_NAME | DATA_CAP_VALUE | DATA_CAP_HIDDEN | DATA_CAP_ACTIONS,
    .id = cx_param_id,
    .name = cx_param_name,
    .value_as_string = cx_param_value_as_string,
    .value_range = cx_param_value_range,
    .is_hidden = cx_param_is_hidden,
    .action_list = cx_param_action_list,
    .action_args = cx_param_action_args,
    .list_count = cx_param_list_count,
    .list_at = cx_param_list_at,
    .action_do = cx_param_action_do,
};

// forward declaration of the category op functions, because of cyclic
// dependencies
static ContextId cx_param_cat_id(void *user_data);
static const char *cx_param_cat_name(void *user_data);
static size_t cx_param_cat_child_count(void *user_data);
static bool cx_param_cat_child_at(void *user_data, size_t idx, DataObject *out);

// A PARAMETER CATEGORY as a CX node. user_data is the opaque handle from
// param_category_handle. Owner-agnostic, like cx_param_ops.
static const DataOps cx_param_cat_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = cx_param_cat_id,
    .name = cx_param_cat_name,
    .child_count = cx_param_cat_child_count,
    .child_at = cx_param_cat_child_at,
};

static bool cx_param_cat_listed(PRM_CONTAIN *container, int cat_id,
                                uint32_t parent_cat_uid);

// a live param sits in the category, directly or in a listed sub-category. A
// hidden param counts - hiding one does not change the tree
static bool cx_param_cat_used(PRM_CONTAIN *container, uint32_t cat_uid) {
    unsigned int total = param_return_num_params(container, 0);
    // a freed slot reads category 0, never a category's uid
    for (unsigned int i = 0; i < total; i++)
        if (param_get_category_uid(container, (int)i) == cat_uid)
            return true;
    unsigned int ncat = param_return_num_categories(container);
    for (unsigned int i = 0; i < ncat; i++)
        if (cx_param_cat_listed(container, (int)i, cat_uid))
            return true;
    return false;
}

// is cat_id a non-empty sub-category of parent_cat_uid
static bool cx_param_cat_listed(PRM_CONTAIN *container, int cat_id,
                                uint32_t parent_cat_uid) {
    if (param_category_parent(container, cat_id) != parent_cat_uid)
        return false;
    return cx_param_cat_used(container, param_category_uid(container, cat_id));
}

static size_t cx_param_level_count(PRM_CONTAIN *container,
                                   uint32_t parent_cat_uid) {
    if (!container)
        return 0;
    size_t n = 0;
    unsigned int ncat = param_return_num_categories(container);
    for (unsigned int i = 0; i < ncat; i++)
        if (cx_param_cat_listed(container, (int)i, parent_cat_uid))
            n++;
    unsigned int total = param_return_num_params(container, 0);
    for (unsigned int i = 0; i < total; i++) {
        if (!param_get_name(container, (int)i))
            continue; // tombstoned slot
        if (param_get_category_uid(container, (int)i) == parent_cat_uid)
            n++;
    }
    return n;
}

// ONE level slice of a param container's category tree, shared by every owner
// whose params are CX children (they pass parent_cat_uid 0, the top level)
// and by each category node (which passes its own uid).
// A level is every listed sub-category of parent_cat_uid in cat_id (creation)
// order, followed by every live param sitting directly in it in val_id order
static bool cx_param_level_at(PRM_CONTAIN *container, uint32_t parent_cat_uid,
                              size_t idx, DataObject *out) {
    if (!container)
        return false;
    unsigned int ncat = param_return_num_categories(container);
    unsigned int ngroups = 0;
    for (unsigned int i = 0; i < ncat; i++)
        if (cx_param_cat_listed(container, (int)i, parent_cat_uid))
            ngroups++;

    // groups by cat_id, then params by val_id - both DECLARATION order, i.e.
    // the order the owner first mentioned them.
    if (idx < ngroups) {
        size_t want = idx;
        for (unsigned int i = 0; i < ncat; i++) {
            if (!cx_param_cat_listed(container, (int)i, parent_cat_uid))
                continue;
            if (want == 0) {
                void *handle = param_category_handle(container, (int)i);
                if (!handle)
                    return false;
                out->ops = &cx_param_cat_ops;
                out->user_data = handle;
                return true;
            }
            want--;
        }
        return false;
    }
    size_t want = idx - ngroups;
    unsigned int total = param_return_num_params(container, 0);
    for (unsigned int i = 0; i < total; i++) {
        if (!param_get_name(container, (int)i))
            continue;
        if (param_get_category_uid(container, (int)i) != parent_cat_uid)
            continue;
        if (want == 0) {
            void *handle = param_get_handle(container, (int)i);
            if (!handle)
                return false;
            out->ops = &cx_param_ops;
            out->user_data = handle;
            return true;
        }
        want--;
    }
    return false;
}

static ContextId cx_param_cat_id(void *user_data) {
    PRM_CONTAIN *container;
    int cat_id;
    if (!param_category_resolve(user_data, &container, &cat_id))
        return MAKE_ID(DATA_NS_PARAM_CATEGORY, 0);
    return MAKE_ID(DATA_NS_PARAM_CATEGORY,
                   param_category_uid(container, cat_id));
}
static const char *cx_param_cat_name(void *user_data) {
    PRM_CONTAIN *container;
    int cat_id;
    if (!param_category_resolve(user_data, &container, &cat_id))
        return NULL;
    return param_category_name(container, cat_id);
}
static size_t cx_param_cat_child_count(void *user_data) {
    PRM_CONTAIN *container;
    int cat_id;
    if (!param_category_resolve(user_data, &container, &cat_id))
        return 0;
    return cx_param_level_count(container,
                                param_category_uid(container, cat_id));
}
static bool cx_param_cat_child_at(void *user_data, size_t idx,
                                  DataObject *out) {
    PRM_CONTAIN *container;
    int cat_id;
    if (!param_category_resolve(user_data, &container, &cat_id))
        return false;
    return cx_param_level_at(container,
                             param_category_uid(container, cat_id), idx, out);
}

// the param config Reload, on every context with params
static DataAction app_param_config_reload_action(APP_INFO *app_data);
static DataActionResult app_param_config_reload(APP_INFO *app_data,
                                                PRM_CONTAIN *container,
                                                const char *kind,
                                                const char *id);

// the Trk container: constant name, the transport's params as children.
static size_t trk_child_count(void *user_data) {
    return cx_param_level_count(transport_param_container(user_data), 0);
}
static bool trk_child_at(void *user_data, size_t idx, DataObject *out) {
    return cx_param_level_at(transport_param_container(user_data), 0, idx,
                             out);
}
static const char *trk_name(void *user_data) {
    (void)user_data;
    return TRK_NAME;
}
static ContextId trk_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_TRK);
}
static size_t trk_action_list(void *user_data, void *root_user_data,
                              DataAction *out, size_t cap) {
    (void)user_data;
    if (!out || cap < 1)
        return 0;
    out[0] = app_param_config_reload_action(root_user_data);
    return 1;
}
static DataActionResult trk_action_do(void *user_data, void *root_user_data,
                                      const DataActionReq *req,
                                      ContextId *out_new) {
    (void)out_new;
    if (!req || req->type != DATA_ACTION_REFRESH)
        return DATA_ACTION_ERR_INVALID;
    return app_param_config_reload(root_user_data,
                                   transport_param_container(user_data),
                                   PARAM_CONFIG_KIND_TRK, "");
}

static const DataOps trk_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_ACTIONS,
    .id = trk_id,
    .child_count = trk_child_count,
    .child_at = trk_child_at,
    .name = trk_name,
    .action_list = trk_action_list,
    .action_do = trk_action_do,
};

// single loaded sample. user_data is the SMP_SMP* from smp_sample_return.
static size_t sample_child_count(void *user_data) {
    return cx_param_level_count(smp_sample_param_container(user_data), 0);
}
static bool sample_child_at(void *user_data, size_t idx, DataObject *out) {
    return cx_param_level_at(smp_sample_param_container(user_data), 0, idx, out);
}
static ContextId sample_id(void *user_data) {
    return MAKE_ID(DATA_NS_SAMPLE, smp_sample_uid(user_data));
}
// DATA_CAP_ACTIONS: a loaded sample can be removed and reload its param
// config. No args, no lists - action_args/list_count/list_at stay unset
// (NULL), which the data_object.h wrappers already treat as "0 args" / "empty
// list".
static size_t sample_action_list(void *user_data, void *root_user_data,
                                 DataAction *out, size_t cap) {
    (void)user_data;
    if (!out)
        return 0;
    size_t n = 0;
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_REMOVE,
            .label = "Remove",
            .tooltip = "Remove this sample",
            .enabled = true,
            .style = DATA_ACTION_STYLE_DANGEROUS,
        };
    if (n < cap)
        out[n++] = app_param_config_reload_action(root_user_data);
    return n;
}
static DataActionResult sample_action_do(void *user_data, void *root_user_data,
                                         const DataActionReq *req,
                                         ContextId *out_new) {
    (void)out_new; // no action creates anything
    if (!req || !user_data)
        return DATA_ACTION_ERR_INVALID;
    if (req->type == DATA_ACTION_REFRESH)
        return app_param_config_reload(root_user_data,
                                       smp_sample_param_container(user_data),
                                       PARAM_CONFIG_KIND_SAMPLER, "");
    if (req->type != DATA_ACTION_REMOVE)
        return DATA_ACTION_ERR_INVALID;
    if (smp_stop_and_remove_sample(user_data) != 0)
        return DATA_ACTION_ERR_DATA;
    return DATA_ACTION_OK;
}
// smp_sample_name already matches DataOps.name (const char *(*)(void *))
static const DataOps sample_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_ACTIONS,
    .id = sample_id,
    .child_count = sample_child_count,
    .child_at = sample_child_at,
    .name = smp_sample_name,
    .action_list = sample_action_list,
    .action_do = sample_action_do,
};

// container of the loaded samples. user_data is SMP_INFO*.
static size_t sampler_child_count(void *user_data) {
    size_t count = 0;
    while (smp_sample_return((SMP_INFO *)user_data, (unsigned int)count))
        count++;
    return count;
}
static bool sampler_child_at(void *user_data, size_t idx, DataObject *out) {
    void *smp = smp_sample_return((SMP_INFO *)user_data, (unsigned int)idx);
    if (!smp)
        return false;
    out->ops = &sample_ops;
    out->user_data = smp;
    return true;
}
static const char *sampler_name(void *user_data) {
    (void)user_data;
    return SAMPLER_NAME;
}
static ContextId sampler_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_SAMPLER);
}
// DATA_CAP_ACTIONS: the sampler list can add a sample from a file path. One
// PATH arg, no list - list_count/list_at stay unset.
static size_t sampler_action_list(void *user_data, void *root_user_data,
                                  DataAction *out, size_t cap) {
    (void)user_data;
    (void)root_user_data;
    if (!out || cap < 1)
        return 0;
    out[0] = (DataAction){
        .type = DATA_ACTION_ADD_FILE_PATH,
        .label = "Add",
        .tooltip = "Load a sample from a file",
        .enabled = true,
        .style = DATA_ACTION_STYLE_NORMAL,
    };
    return 1;
}
static size_t sampler_action_args(void *user_data, DataActionType type,
                                  uint32_t id, DataArgSpec *out, size_t cap) {
    (void)user_data;
    (void)id;
    if (type != DATA_ACTION_ADD_FILE_PATH)
        return 0;
    if (!out || cap < 1)
        return 0;
    out[0] = (DataArgSpec){
        .name = "path",
        .label = "File path",
        .kind = DATA_ARG_PATH,
        .required = true,
        .list = DATA_LIST_NONE,
    };
    return 1;
}
static DataActionResult sampler_action_do(void *user_data, void *root_user_data,
                                          const DataActionReq *req,
                                          ContextId *out_new) {
    (void)root_user_data;
    SMP_INFO *smp_data = (SMP_INFO *)user_data;
    if (!smp_data || !req || req->type != DATA_ACTION_ADD_FILE_PATH)
        return DATA_ACTION_ERR_INVALID;
    const char *path = req->add_file_path.path;
    if (!path || !path[0])
        return DATA_ACTION_ERR_INVALID;

    // smp_add returns the new sample's identity uid (always > 0) on success,
    // 0 on failure.
    uint32_t uid = smp_add(smp_data, path, -1);
    if (uid == 0)
        return DATA_ACTION_ERR_DATA;

    if (out_new)
        *out_new = MAKE_ID(DATA_NS_SAMPLE, uid);
    return DATA_ACTION_OK;
}
static const DataOps sampler_ops = {
    .capabilities =
        DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_ACTIONS | DATA_CAP_GROUP,
    .id = sampler_id,
    .group_key = sampler_id,
    .child_count = sampler_child_count,
    .child_at = sampler_child_at,
    .name = sampler_name,
    .action_list = sampler_action_list,
    .action_args = sampler_action_args,
    .action_do = sampler_action_do,
};

// a module's branch key from a list's branch argument: 0 is the top level, a
// category row's value loses its namespace. Returns true only for branches.
static bool list_branch_key(uint64_t branch, uint64_t *out) {
    *out = branch & CTXID_LOCAL_MASK;
    return branch == 0 ||
           (CTXID_NS(branch) == DATA_LIST_NS_BRANCH && *out != 0);
}

// a tree_index row as a choice: category rows in DATA_LIST_NS_BRANCH, the
// rest in leaf_ns
static void list_choice_from_tree_row(const TREE_ROW *row, unsigned int leaf_ns,
                                      DataChoice *out) {
    out->value =
        MAKE_ID(row->is_branch ? DATA_LIST_NS_BRANCH : leaf_ns, row->key);
    out->label = row->name;
    out->flags = row->is_branch ? DATA_CHOICE_BRANCH : 0;
}

// DataAction.id of a loaded plugin's COMMAND
enum {
    PLUGIN_ACTION_GUI = 0,
};

// a loaded plugin's (lv2 or clap) action menu: Remove, Preset - loading one
// picked from its preset list, Save preset, GUI - showing or hiding the
// plugin's own GUI, and Reload of its param config
static size_t plugin_action_list(APP_INFO *app_data, DataAction *out,
                                 size_t cap, bool save_available,
                                 bool gui_available, bool gui_shown) {
    if (!out)
        return 0;
    size_t n = 0;
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_REMOVE,
            .label = "Remove",
            .tooltip = "Remove this plugin",
            .enabled = true,
            .style = DATA_ACTION_STYLE_DANGEROUS,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_SET_CHOICE,
            .label = "Preset",
            .tooltip = "Load one of this plugin's presets",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_SAVE,
            .label = "Save preset",
            .tooltip = "Save this plugin's current state as a preset, "
                       "replacing one of the same name",
            .enabled = save_available,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_COMMAND,
            .id = PLUGIN_ACTION_GUI,
            .label = "GUI",
            .tooltip = "Show or hide this plugin's GUI",
            .enabled = gui_available,
            .active = gui_shown,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = app_param_config_reload_action(app_data);
    return n;
}

// the GUI COMMAND through a module's getter and toggle
static DataActionResult plugin_gui_command(void *plug, uint32_t id,
                                           bool (*available)(void *),
                                           int (*toggle)(void *)) {
    if (id != PLUGIN_ACTION_GUI)
        return DATA_ACTION_ERR_INVALID;
    if (!available(plug))
        return DATA_ACTION_ERR_NOT_ALLOWED;
    return toggle(plug) == 0 ? DATA_ACTION_OK : DATA_ACTION_ERR_DATA;
}

// the Preset action's one arg, picked from the plugin's preset list, and
// Save preset's name
static size_t plugin_preset_args(DataActionType type, DataListId list,
                                 DataArgSpec *out, size_t cap) {
    if (!out || cap < 1)
        return 0;
    if (type == DATA_ACTION_SAVE) {
        out[0] = (DataArgSpec){
            .name = "name",
            .label = "Preset name",
            .kind = DATA_ARG_STRING,
            .required = true,
        };
        return 1;
    }
    if (type != DATA_ACTION_SET_CHOICE)
        return 0;
    out[0] = (DataArgSpec){
        .name = "preset",
        .label = "Preset",
        .kind = DATA_ARG_CHOICE,
        .required = true,
        .list = list,
    };
    return 1;
}

// a module's preset load result: 0 loaded, -2 no preset has the key any more
static DataActionResult plugin_preset_loaded(int loaded) {
    if (loaded == -2)
        return DATA_ACTION_ERR_STALE; // the preset list changed since the pick
    return loaded == 0 ? DATA_ACTION_OK : DATA_ACTION_ERR_DATA;
}

// a module's preset save result: 0 saved, -2 an empty name
static DataActionResult plugin_preset_saved(int saved) {
    if (saved == -2)
        return DATA_ACTION_ERR_INVALID;
    return saved == 0 ? DATA_ACTION_OK : DATA_ACTION_ERR_DATA;
}

// single loaded lv2 plugin. user_data is the PLUG_PLUG* from plug_plugin_return.
static size_t lv2_plugin_child_count(void *user_data) {
    return cx_param_level_count(plug_plugin_param_container(user_data), 0);
}
static bool lv2_plugin_child_at(void *user_data, size_t idx, DataObject *out) {
    return cx_param_level_at(plug_plugin_param_container(user_data), 0, idx, out);
}
static ContextId lv2_plugin_id(void *user_data) {
    return MAKE_ID(DATA_NS_LV2_PLUG, plug_plugin_uid(user_data));
}
// DATA_CAP_ACTIONS: a loaded lv2 plugin can be removed, can load a preset
// picked from its preset list (banks as category rows), and show its UI.
static size_t lv2_plugin_action_list(void *user_data, void *root_user_data,
                                     DataAction *out, size_t cap) {
    return plugin_action_list(root_user_data, out, cap, true,
                              plug_plugin_gui_available(user_data),
                              plug_plugin_gui_shown(user_data));
}
static size_t lv2_plugin_action_args(void *user_data,
                                     DataActionType type, uint32_t id,
                                     DataArgSpec *out, size_t cap) {
    (void)user_data;
    (void)id;
    return plugin_preset_args(type, LIST_LV2_PRESETS, out, cap);
}
static size_t lv2_plugin_list_count(void *user_data, DataListId list,
                                    const DataActionReq *partial,
                                    uint64_t branch) {
    (void)partial;
    uint64_t key;
    if (list != LIST_LV2_PRESETS || !list_branch_key(branch, &key))
        return 0;
    return plug_plugin_presets_level_count(user_data, key);
}
static bool lv2_plugin_list_at(void *user_data, DataListId list,
                               const DataActionReq *partial, uint64_t branch,
                               size_t idx, DataChoice *out) {
    (void)partial;
    uint64_t key;
    TREE_ROW row;
    if (list != LIST_LV2_PRESETS || !list_branch_key(branch, &key) ||
        !plug_plugin_presets_level_at(user_data, key, idx, &row))
        return false;
    list_choice_from_tree_row(&row, DATA_LIST_NS_LV2_PRESET, out);
    return true;
}
static DataActionResult lv2_plugin_action_do(void *user_data,
                                             void *root_user_data,
                                             const DataActionReq *req,
                                             ContextId *out_new) {
    (void)out_new; // no action creates anything
    if (!req || !user_data)
        return DATA_ACTION_ERR_INVALID;
    if (req->type == DATA_ACTION_REMOVE)
        return plug_stop_and_remove_plug(user_data) != 0 ? DATA_ACTION_ERR_DATA
                                                         : DATA_ACTION_OK;
    if (req->type == DATA_ACTION_COMMAND)
        return plugin_gui_command(user_data, req->id, plug_plugin_gui_available,
                                  plug_plugin_gui_toggle);
    if (req->type == DATA_ACTION_REFRESH)
        return app_param_config_reload(
            root_user_data, plug_plugin_param_container(user_data),
            PARAM_CONFIG_KIND_LV2, plug_plugin_uri(user_data));
    if (req->type == DATA_ACTION_SAVE)
        return plugin_preset_saved(
            plug_plugin_preset_save(user_data, req->save.name));
    if (req->type != DATA_ACTION_SET_CHOICE)
        return DATA_ACTION_ERR_INVALID;

    ContextId value = (ContextId)req->add_choice.choice_value;
    // also refuses a category row's value
    if (CTXID_NS(value) != DATA_LIST_NS_LV2_PRESET)
        return DATA_ACTION_ERR_INVALID;
    return plugin_preset_loaded(
        plug_plugin_preset_load(user_data, value & CTXID_LOCAL_MASK));
}
// plug_plugin_name/_latency already match DataOps.name/latency
static const DataOps lv2_plugin_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_ACTIONS |
                    DATA_CAP_GROUP | DATA_CAP_LATENCY,
    .id = lv2_plugin_id,
    .group_key = lv2_plugin_id,
    .child_count = lv2_plugin_child_count,
    .child_at = lv2_plugin_child_at,
    .name = plug_plugin_name,
    .latency = plug_plugin_latency,
    .action_list = lv2_plugin_action_list,
    .action_args = lv2_plugin_action_args,
    .list_count = lv2_plugin_list_count,
    .list_at = lv2_plugin_list_at,
    .action_do = lv2_plugin_action_do,
};

// container of the loaded lv2 plugins. user_data is PLUG_INFO*.
static size_t lv2_plugins_child_count(void *user_data) {
    size_t count = 0;
    while (plug_plugin_return((PLUG_INFO *)user_data, (unsigned int)count))
        count++;
    return count;
}
static bool lv2_plugins_child_at(void *user_data, size_t idx, DataObject *out) {
    void *plug = plug_plugin_return((PLUG_INFO *)user_data, (unsigned int)idx);
    if (!plug)
        return false;
    out->ops = &lv2_plugin_ops;
    out->user_data = plug;
    return true;
}
static const char *lv2_plugins_name(void *user_data) {
    (void)user_data;
    return PLUGINS_LV2_NAME;
}
static ContextId lv2_plugins_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_LV2_PLUGINS);
}
// DATA_CAP_ACTIONS: the lv2 list can add a plugin chosen from the catalogue
// (one CHOICE arg, list = the catalogue), and rescan that catalogue.
static size_t lv2_plugins_action_list(void *user_data, void *root_user_data,
                                      DataAction *out, size_t cap) {
    (void)user_data;
    (void)root_user_data;
    if (!out)
        return 0;
    size_t n = 0;
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_ADD_CHOICE,
            .label = "Add",
            .tooltip = "Load an LV2 plugin from the catalogue",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_REFRESH,
            .label = "Scan",
            .tooltip = "Rescan the installed LV2 plugins",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    return n;
}
static size_t lv2_plugins_action_args(void *user_data,
                                      DataActionType type, uint32_t id,
                                      DataArgSpec *out, size_t cap) {
    (void)user_data;
    (void)id;
    if (type != DATA_ACTION_ADD_CHOICE)
        return 0;
    if (!out || cap < 1)
        return 0;
    out[0] = (DataArgSpec){
        .name = "plugin",
        .label = "Plugin",
        .kind = DATA_ARG_CHOICE,
        .required = true,
        .list = LIST_LV2_CATALOG,
    };
    return 1;
}
static size_t lv2_plugins_list_count(void *user_data, DataListId list,
                                     const DataActionReq *partial,
                                     uint64_t branch) {
    (void)partial;
    if (list != LIST_LV2_CATALOG || branch != 0)
        return 0;
    return (size_t)plug_plugin_list_count((PLUG_INFO *)user_data);
}
static bool lv2_plugins_list_at(void *user_data, DataListId list,
                                const DataActionReq *partial, uint64_t branch,
                                size_t idx, DataChoice *out) {
    (void)partial;
    if (list != LIST_LV2_CATALOG || branch != 0)
        return false;
    void *item =
        plug_plugin_list_item_get((PLUG_INFO *)user_data, (unsigned int)idx);
    if (!item)
        return false;
    out->value =
        MAKE_ID(DATA_LIST_NS_LV2_CATALOG, plug_plugin_list_item_key(item));
    out->label = plug_plugin_list_item_name(item);
    out->flags = 0;
    return true;
}
static DataActionResult lv2_plugins_action_do(void *user_data,
                                              void *root_user_data,
                                              const DataActionReq *req,
                                              ContextId *out_new) {
    (void)root_user_data;
    PLUG_INFO *plug_data = (PLUG_INFO *)user_data;
    if (!plug_data || !req)
        return DATA_ACTION_ERR_INVALID;
    if (req->type == DATA_ACTION_REFRESH)
        return plug_plugin_list_init(plug_data) < 0 ? DATA_ACTION_ERR_DATA
                                                    : DATA_ACTION_OK;
    if (req->type != DATA_ACTION_ADD_CHOICE)
        return DATA_ACTION_ERR_INVALID;

    ContextId value = (ContextId)req->add_choice.choice_value;
    if (CTXID_NS(value) != DATA_LIST_NS_LV2_CATALOG)
        return DATA_ACTION_ERR_INVALID; // not a choice from this list
    void *found =
        plug_plugin_list_item_by_key(plug_data, value & CTXID_LOCAL_MASK);
    if (!found)
        return DATA_ACTION_ERR_STALE; // catalogue changed since the pick

    uint32_t uid = plug_load_and_activate(found);
    if (uid == 0)
        return DATA_ACTION_ERR_DATA;

    if (out_new)
        *out_new = MAKE_ID(DATA_NS_LV2_PLUG, uid);
    return DATA_ACTION_OK;
}
static const DataOps lv2_plugins_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_ACTIONS,
    .id = lv2_plugins_id,
    .child_count = lv2_plugins_child_count,
    .child_at = lv2_plugins_child_at,
    .name = lv2_plugins_name,
    .action_list = lv2_plugins_action_list,
    .action_args = lv2_plugins_action_args,
    .list_count = lv2_plugins_list_count,
    .list_at = lv2_plugins_list_at,
    .action_do = lv2_plugins_action_do,
};

// single loaded clap plugin. user_data is the plugin handle from
// clap_plug_plugin_return.
static size_t clap_plugin_child_count(void *user_data) {
    return cx_param_level_count(clap_plug_plugin_param_container(user_data), 0);
}
static bool clap_plugin_child_at(void *user_data, size_t idx, DataObject *out) {
    return cx_param_level_at(clap_plug_plugin_param_container(user_data), 0,
                             idx, out);
}
static ContextId clap_plugin_id(void *user_data) {
    return MAKE_ID(DATA_NS_CLAP_PLUG, clap_plug_plugin_uid(user_data));
}
// DATA_CAP_ACTIONS: a loaded clap plugin can be removed, can load a preset
// picked from its preset list (locations and their sub directories as
// category rows), and show its GUI.
static size_t clap_plugin_action_list(void *user_data, void *root_user_data,
                                      DataAction *out, size_t cap) {
    return plugin_action_list(root_user_data, out, cap,
                              clap_plug_preset_can_save(user_data),
                              clap_plug_plugin_gui_available(user_data),
                              clap_plug_plugin_gui_shown(user_data));
}
static size_t clap_plugin_action_args(void *user_data,
                                      DataActionType type, uint32_t id,
                                      DataArgSpec *out, size_t cap) {
    (void)user_data;
    (void)id;
    return plugin_preset_args(type, LIST_CLAP_PRESETS, out, cap);
}
static size_t clap_plugin_list_count(void *user_data, DataListId list,
                                     const DataActionReq *partial,
                                     uint64_t branch) {
    (void)partial;
    uint64_t key;
    if (list != LIST_CLAP_PRESETS || !list_branch_key(branch, &key))
        return 0;
    return clap_plug_presets_level_count(user_data, key);
}
static bool clap_plugin_list_at(void *user_data, DataListId list,
                                const DataActionReq *partial, uint64_t branch,
                                size_t idx, DataChoice *out) {
    (void)partial;
    uint64_t key;
    TREE_ROW row;
    if (list != LIST_CLAP_PRESETS || !list_branch_key(branch, &key) ||
        !clap_plug_presets_level_at(user_data, key, idx, &row))
        return false;
    list_choice_from_tree_row(&row, DATA_LIST_NS_CLAP_PRESET, out);
    return true;
}
static DataActionResult clap_plugin_action_do(void *user_data,
                                              void *root_user_data,
                                              const DataActionReq *req,
                                              ContextId *out_new) {
    (void)out_new; // no action creates anything
    if (!req || !user_data)
        return DATA_ACTION_ERR_INVALID;
    if (req->type == DATA_ACTION_REMOVE)
        return clap_plug_plug_stop_and_clean(user_data) != 0
                   ? DATA_ACTION_ERR_DATA
                   : DATA_ACTION_OK;
    if (req->type == DATA_ACTION_COMMAND)
        return plugin_gui_command(user_data, req->id,
                                  clap_plug_plugin_gui_available,
                                  clap_plug_plugin_gui_toggle);
    if (req->type == DATA_ACTION_REFRESH)
        return app_param_config_reload(
            root_user_data, clap_plug_plugin_param_container(user_data),
            PARAM_CONFIG_KIND_CLAP, clap_plug_plugin_id(user_data));
    if (req->type == DATA_ACTION_SAVE) {
        if (!clap_plug_preset_can_save(user_data))
            return DATA_ACTION_ERR_NOT_ALLOWED;
        return plugin_preset_saved(
            clap_plug_preset_save(user_data, req->save.name));
    }
    if (req->type != DATA_ACTION_SET_CHOICE)
        return DATA_ACTION_ERR_INVALID;

    ContextId value = (ContextId)req->add_choice.choice_value;
    // also refuses a category row's value
    if (CTXID_NS(value) != DATA_LIST_NS_CLAP_PRESET)
        return DATA_ACTION_ERR_INVALID;
    return plugin_preset_loaded(
        clap_plug_preset_load(user_data, value & CTXID_LOCAL_MASK));
}
// clap_plug_plugin_name/_latency already match DataOps.name/latency
static const DataOps clap_plugin_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_ACTIONS |
                    DATA_CAP_GROUP | DATA_CAP_LATENCY,
    .id = clap_plugin_id,
    .group_key = clap_plugin_id,
    .child_count = clap_plugin_child_count,
    .child_at = clap_plugin_child_at,
    .name = clap_plug_plugin_name,
    .latency = clap_plug_plugin_latency,
    .action_list = clap_plugin_action_list,
    .action_args = clap_plugin_action_args,
    .list_count = clap_plugin_list_count,
    .list_at = clap_plugin_list_at,
    .action_do = clap_plugin_action_do,
};

// container of the loaded clap plugins. user_data is CLAP_PLUG_INFO*.
static size_t clap_plugins_child_count(void *user_data) {
    size_t count = 0;
    while (clap_plug_plugin_return((CLAP_PLUG_INFO *)user_data,
                                   (unsigned int)count))
        count++;
    return count;
}
static bool clap_plugins_child_at(void *user_data, size_t idx, DataObject *out) {
    void *plug =
        clap_plug_plugin_return((CLAP_PLUG_INFO *)user_data, (unsigned int)idx);
    if (!plug)
        return false;
    out->ops = &clap_plugin_ops;
    out->user_data = plug;
    return true;
}
static const char *clap_plugins_name(void *user_data) {
    (void)user_data;
    return PLUGINS_CLAP_NAME;
}
static ContextId clap_plugins_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_CLAP_PLUGINS);
}
// DATA_CAP_ACTIONS: the clap list can add a plugin chosen from the catalogue
// (one CHOICE arg, list = the catalogue), and rescan that catalogue.
static size_t clap_plugins_action_list(void *user_data, void *root_user_data,
                                       DataAction *out, size_t cap) {
    (void)user_data;
    (void)root_user_data;
    if (!out)
        return 0;
    size_t n = 0;
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_ADD_CHOICE,
            .label = "Add",
            .tooltip = "Load a CLAP plugin from the catalogue",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_REFRESH,
            .label = "Scan",
            .tooltip = "Rescan the installed CLAP plugins",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    return n;
}
static size_t clap_plugins_action_args(void *user_data,
                                       DataActionType type, uint32_t id,
                                       DataArgSpec *out, size_t cap) {
    (void)user_data;
    (void)id;
    if (type != DATA_ACTION_ADD_CHOICE)
        return 0;
    if (!out || cap < 1)
        return 0;
    out[0] = (DataArgSpec){
        .name = "plugin",
        .label = "Plugin",
        .kind = DATA_ARG_CHOICE,
        .required = true,
        .list = LIST_CLAP_CATALOG,
    };
    return 1;
}
static size_t clap_plugins_list_count(void *user_data, DataListId list,
                                      const DataActionReq *partial,
                                      uint64_t branch) {
    (void)partial;
    if (list != LIST_CLAP_CATALOG || branch != 0)
        return 0;
    return (size_t)clap_plug_plugin_list_count((CLAP_PLUG_INFO *)user_data);
}
static bool clap_plugins_list_at(void *user_data, DataListId list,
                                 const DataActionReq *partial, uint64_t branch,
                                 size_t idx, DataChoice *out) {
    (void)partial;
    if (list != LIST_CLAP_CATALOG || branch != 0)
        return false;
    void *item = clap_plug_plugin_list_item_get((CLAP_PLUG_INFO *)user_data,
                                                (unsigned int)idx);
    if (!item)
        return false;
    out->value =
        MAKE_ID(DATA_LIST_NS_CLAP_CATALOG, clap_plug_plugin_list_item_key(item));
    out->label = clap_plug_plugin_list_item_name(item);
    out->flags = 0;
    return true;
}
static DataActionResult clap_plugins_action_do(void *user_data,
                                               void *root_user_data,
                                               const DataActionReq *req,
                                               ContextId *out_new) {
    (void)root_user_data;
    CLAP_PLUG_INFO *plug_data = (CLAP_PLUG_INFO *)user_data;
    if (!plug_data || !req)
        return DATA_ACTION_ERR_INVALID;
    if (req->type == DATA_ACTION_REFRESH)
        return clap_plug_plugin_list_init(plug_data) < 0 ? DATA_ACTION_ERR_DATA
                                                         : DATA_ACTION_OK;
    if (req->type != DATA_ACTION_ADD_CHOICE)
        return DATA_ACTION_ERR_INVALID;

    ContextId value = (ContextId)req->add_choice.choice_value;
    if (CTXID_NS(value) != DATA_LIST_NS_CLAP_CATALOG)
        return DATA_ACTION_ERR_INVALID; // not a choice from this list
    void *found =
        clap_plug_plugin_list_item_by_key(plug_data, value & CTXID_LOCAL_MASK);
    if (!found)
        return DATA_ACTION_ERR_STALE; // catalogue changed since the pick

    uint32_t uid = clap_plug_load_and_activate(found);
    if (uid == 0)
        return DATA_ACTION_ERR_DATA;

    if (out_new)
        *out_new = MAKE_ID(DATA_NS_CLAP_PLUG, uid);
    return DATA_ACTION_OK;
}
static const DataOps clap_plugins_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_ACTIONS,
    .id = clap_plugins_id,
    .child_count = clap_plugins_child_count,
    .child_at = clap_plugins_child_at,
    .name = clap_plugins_name,
    .action_list = clap_plugins_action_list,
    .action_args = clap_plugins_action_args,
    .list_count = clap_plugins_list_count,
    .list_at = clap_plugins_list_at,
    .action_do = clap_plugins_action_do,
};

// single synth oscillator. user_data is the handle from synth_osc_return - it
// represents "oscillator number N" on the synth, not a SYNTH_OSC* pointer.
static size_t osc_child_count(void *user_data) {
    return cx_param_level_count(synth_osc_param_container(user_data), 0);
}
static bool osc_child_at(void *user_data, size_t idx, DataObject *out) {
    return cx_param_level_at(synth_osc_param_container(user_data), 0, idx, out);
}
static ContextId osc_id(void *user_data) {
    return MAKE_ID(DATA_NS_SYNTH_OSC, synth_osc_uid(user_data));
}
static size_t osc_action_list(void *user_data, void *root_user_data,
                              DataAction *out, size_t cap) {
    (void)user_data;
    if (!out || cap < 1)
        return 0;
    out[0] = app_param_config_reload_action(root_user_data);
    return 1;
}
static DataActionResult osc_action_do(void *user_data, void *root_user_data,
                                      const DataActionReq *req,
                                      ContextId *out_new) {
    (void)out_new;
    if (!req || req->type != DATA_ACTION_REFRESH)
        return DATA_ACTION_ERR_INVALID;
    return app_param_config_reload(root_user_data,
                                   synth_osc_param_container(user_data),
                                   PARAM_CONFIG_KIND_SYNTH_OSC, "");
}
// synth_osc_name already matches DataOps.name (const char *(*)(void *))
static const DataOps osc_ops = {
    .capabilities =
        DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_GROUP | DATA_CAP_ACTIONS,
    .id = osc_id,
    .group_key = osc_id,
    .child_count = osc_child_count,
    .child_at = osc_child_at,
    .name = synth_osc_name,
    .action_list = osc_action_list,
    .action_do = osc_action_do,
};

// container of the synth oscillators. user_data is SYNTH_DATA*. Oscillators are
// fixed at init (never added/removed), so there are no gaps to walk.
static size_t synth_child_count(void *user_data) {
    return synth_return_osc_num((SYNTH_DATA *)user_data);
}
static bool synth_child_at(void *user_data, size_t idx, DataObject *out) {
    void *osc = synth_osc_return((SYNTH_DATA *)user_data, (unsigned int)idx);
    if (!osc)
        return false;
    out->ops = &osc_ops;
    out->user_data = osc;
    return true;
}
static const char *synth_name(void *user_data) {
    (void)user_data;
    return SYNTH_NAME;
}
static ContextId synth_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_SYNTH);
}
static const DataOps synth_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = synth_id,
    .child_count = synth_child_count,
    .child_at = synth_child_at,
    .name = synth_name,
};

// root object. user_data is APP_INFO*.
static size_t root_child_count(void *user_data) {
    (void)user_data;
    return 5;
}
static bool root_child_at(void *user_data, size_t idx, DataObject *out) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data)
        return false;
    switch (idx) {
    case 0:
        out->ops = &sampler_ops;
        out->user_data = app_data->smp_data;
        return true;
    case 1:
        out->ops = &lv2_plugins_ops;
        out->user_data = app_data->plug_data;
        return true;
    case 2:
        out->ops = &clap_plugins_ops;
        out->user_data = app_data->clap_plug_data;
        return true;
    case 3:
        out->ops = &synth_ops;
        out->user_data = app_data->synth_data;
        return true;
    case 4:
        out->ops = &trk_ops;
        out->user_data = app_data->transport;
        return true;
    default:
        return false;
    }
}
static const char *root_name(void *user_data) {
    (void)user_data;
    return ROOT_NAME;
}
static ContextId root_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_ROOT);
}

// DATA_CAP_ACTIONS: the root context hosts two CONNECT actions, told apart by
// DataAction.id: Connect links the graph's ports, Extr_Connect the exposed
// ports to other programs' ports. Ports are never materialised into the CX
// tree - these read the graph and the backend's port cache, each keyed by its
// own identities. Play and Stop are the transport's COMMANDs.
enum {
    ROOT_ACTION_CONNECT = 0,
    ROOT_ACTION_EXTR_CONNECT,
    ROOT_ACTION_PLAY,
    ROOT_ACTION_STOP,
    ROOT_ACTION_NEW_SONG,
};

// the name the tree shows for a port owner, NULL for an unknown one. Owner
// handles are only reachable by index, so this is a scan
static const char *owner_label(APP_INFO *app_data, uint64_t owner_tag,
                               uint64_t owner_uid) {
    switch ((unsigned)owner_tag) {
    case DATA_NS_LV2_PLUG:
        for (unsigned int i = 0;; i++) {
            void *plug = plug_plugin_return(app_data->plug_data, i);
            if (!plug)
                break;
            if (plug_plugin_uid(plug) == (uint32_t)owner_uid)
                return plug_plugin_name(plug);
        }
        break;
    case DATA_NS_CLAP_PLUG:
        for (unsigned int i = 0;; i++) {
            void *plug = clap_plug_plugin_return(app_data->clap_plug_data, i);
            if (!plug)
                break;
            if (clap_plug_plugin_uid(plug) == (uint32_t)owner_uid)
                return clap_plug_plugin_name(plug);
        }
        break;
    case DATA_NS_SYNTH_OSC:
        for (unsigned int i = 0;; i++) {
            void *osc = synth_osc_return(app_data->synth_data, i);
            if (!osc)
                break;
            if (synth_osc_uid(osc) == (uint32_t)owner_uid)
                return synth_osc_name(osc);
        }
        break;
    case DATA_NS_SINGLETON:
        // the same names the tree shows for these contexts
        if (owner_uid == SID_SAMPLER)
            return sampler_name(NULL);
        if (owner_uid == SID_ROOT)
            return root_name(NULL);
        if (owner_uid == SID_TRK)
            return trk_name(NULL);
        break;
    }
    return NULL;
}

// Connect: the graph key a peers list hangs off, 0 when no source is chosen
static uint64_t root_graph_source(const DataActionReq *partial) {
    if (!partial || partial->type != DATA_ACTION_CONNECT)
        return 0;
    ContextId value = (ContextId)partial->connect.source;
    if (CTXID_NS(value) != DATA_LIST_NS_GRAPH_PORTS)
        return 0;
    return value & CTXID_LOCAL_MASK;
}

// linked_to is the key a row's DATA_CHOICE_LINKED is measured against, 0 for a
// list with no "linked" concept. Owners add nodes with (namespace, uid), so
// MAKE_ID(tag, uid) is the owner's own id - its group_key op is its id op
static void root_graph_choice_fill(APP_INFO *app_data,
                                   const GraphPortInfo *info,
                                   uint64_t linked_to, DataChoice *out) {
    out->value = MAKE_ID(DATA_LIST_NS_GRAPH_PORTS, info->key);
    out->label = info->name;
    out->group_key = MAKE_ID(info->owner_tag, info->owner_uid);
    out->group_label = owner_label(app_data, info->owner_tag, info->owner_uid);
    out->flags = (linked_to && graph_port_keys_connected(app_data->graph,
                                                         linked_to, info->key))
                     ? DATA_CHOICE_LINKED
                     : 0;
}

// Extr_Connect: the peers a source can be linked to, opposite flow, matching
// type
static BackendPortList root_extr_peer_list(const BackendPortInfo *source) {
    bool want_output = source->flow != PORT_FLOW_OUTPUT;
    if (source->type == PORT_TYPE_MIDI)
        return want_output ? BACKEND_PORT_LIST_OUT_MIDI
                           : BACKEND_PORT_LIST_IN_MIDI;
    return want_output ? BACKEND_PORT_LIST_OUT_AUDIO
                       : BACKEND_PORT_LIST_IN_AUDIO;
}

// the source a peers list hangs off, read from the half-filled request. false
// when none has been chosen yet, or the chosen one is gone
static bool root_extr_source(AUDIO_BACKEND *backend,
                             const DataActionReq *partial,
                             BackendPortInfo *out) {
    if (!partial || partial->type != DATA_ACTION_CONNECT)
        return false;
    ContextId value = (ContextId)partial->connect.source;
    if (CTXID_NS(value) != DATA_LIST_NS_PORTS)
        return false;
    return audio_backend_port_by_key(backend, value & CTXID_LOCAL_MASK, out);
}

// the exposed ports group under their owner, as graph ports do; anything else
// has only its client to go on
static uint64_t root_extr_group_key(const BackendPortInfo *info) {
    if (info->owner_tag == 0)
        return MAKE_ID(DATA_NS_JACK_CLIENT, str_hash_fnv1a64(info->client));
    return MAKE_ID(info->owner_tag, info->owner_uid);
}

static const char *root_extr_group_label(APP_INFO *app_data,
                                         const BackendPortInfo *info) {
    const char *label = owner_label(app_data, info->owner_tag, info->owner_uid);
    return label ? label : info->client;
}

// linked_to as in root_graph_choice_fill. The label skips "client:" - the
// group has it
static void root_extr_choice_fill(APP_INFO *app_data,
                                  const BackendPortInfo *info,
                                  uint64_t linked_to, DataChoice *out) {
    size_t client_len = strlen(info->client);
    out->value = MAKE_ID(DATA_LIST_NS_PORTS, info->key);
    out->label = info->name[client_len] == ':' ? info->name + client_len + 1
                                                : info->name;
    out->group_key = root_extr_group_key(info);
    out->group_label = root_extr_group_label(app_data, info);
    out->flags = (linked_to && audio_backend_port_keys_connected(
                                   app_data->backend, linked_to, info->key))
                     ? DATA_CHOICE_LINKED
                     : 0;
}

static size_t root_action_list(void *user_data, void *root_user_data,
                               DataAction *out, size_t cap) {
    (void)root_user_data;
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data || !out)
        return 0;
    size_t n = 0;
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_CONNECT,
            .id = ROOT_ACTION_CONNECT,
            .label = "Connect",
            .tooltip = "Connect or disconnect the app's own ports",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_CONNECT,
            .id = ROOT_ACTION_EXTR_CONNECT,
            .label = "Extr_Connect",
            .tooltip = "Connect or disconnect the exposed ports to other "
                       "programs' ports",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_COMMAND,
            .id = ROOT_ACTION_PLAY,
            .label = "Play",
            .tooltip = "Start the transport",
            .enabled = !transport_rolling(app_data->transport),
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_COMMAND,
            .id = ROOT_ACTION_STOP,
            .label = "Stop",
            .tooltip = "Stop the transport, when stopped go back to the start",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_SAVE,
            .label = "Save song",
            .tooltip = "Save the song to a path, an empty one saves the "
                       "current song",
            .enabled = true,
            .style = DATA_ACTION_STYLE_NORMAL,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_LOAD,
            .label = "Load song",
            .tooltip = "Replace the song with a saved one",
            .enabled = true,
            .style = DATA_ACTION_STYLE_DANGEROUS,
        };
    if (n < cap)
        out[n++] = (DataAction){
            .type = DATA_ACTION_COMMAND,
            .id = ROOT_ACTION_NEW_SONG,
            .label = "New song",
            .tooltip = "Replace the song with the template, or an empty one",
            .enabled = true,
            .style = DATA_ACTION_STYLE_DANGEROUS,
        };
    return n;
}

static size_t root_action_args(void *user_data, DataActionType type,
                               uint32_t id, DataArgSpec *out, size_t cap) {
    (void)user_data;
    if ((type == DATA_ACTION_SAVE || type == DATA_ACTION_LOAD) && out &&
        cap >= 1) {
        out[0] = (DataArgSpec){
            .name = "path",
            .label = "Song path",
            .kind = DATA_ARG_PATH,
            .required = type == DATA_ACTION_LOAD,
            .list = DATA_LIST_NONE,
        };
        return 1;
    }
    if (type != DATA_ACTION_CONNECT || !out || cap < 2)
        return 0;
    bool graph = id == ROOT_ACTION_CONNECT;
    if (!graph && id != ROOT_ACTION_EXTR_CONNECT)
        return 0;
    out[0] = (DataArgSpec){
        .name = "source",
        .label = "Port",
        .kind = DATA_ARG_CHOICE,
        .required = true,
        .list = graph ? LIST_GRAPH_PORTS : LIST_EXTR_PORTS,
    };
    out[1] = (DataArgSpec){
        .name = "targets",
        .label = "Connect to",
        .kind = DATA_ARG_MULTI_CHOICE,
        .required = true,
        .list = graph ? LIST_GRAPH_PEERS : LIST_EXTR_PEERS,
    };
    return 2;
}

static size_t root_list_count(void *user_data, DataListId list,
                              const DataActionReq *partial, uint64_t branch) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data || branch != 0)
        return 0;

    if (list == LIST_GRAPH_PORTS)
        return graph_port_count(app_data->graph, GRAPH_PORT_LIST_ALL);
    if (list == LIST_GRAPH_PEERS)
        return graph_peer_count(app_data->graph, root_graph_source(partial));

    if (list == LIST_EXTR_PORTS)
        return audio_backend_port_count(app_data->backend,
                                        BACKEND_PORT_LIST_EXPOSED);
    if (list == LIST_EXTR_PEERS) {
        BackendPortInfo source;
        if (!root_extr_source(app_data->backend, partial, &source))
            return 0;
        return audio_backend_port_count(app_data->backend,
                                        root_extr_peer_list(&source));
    }
    return 0;
}

static bool root_list_at(void *user_data, DataListId list,
                         const DataActionReq *partial, uint64_t branch,
                         size_t idx, DataChoice *out) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data || branch != 0)
        return false;

    GraphPortInfo graph_info;
    if (list == LIST_GRAPH_PORTS) {
        if (!graph_port_at(app_data->graph, GRAPH_PORT_LIST_ALL, idx,
                           &graph_info))
            return false;
        root_graph_choice_fill(app_data, &graph_info, 0, out);
        return true;
    }
    if (list == LIST_GRAPH_PEERS) {
        // empty until a source is chosen
        uint64_t source = root_graph_source(partial);
        if (!graph_peer_at(app_data->graph, source, idx, &graph_info))
            return false;
        root_graph_choice_fill(app_data, &graph_info, source, out);
        return true;
    }

    BackendPortInfo info;
    if (list == LIST_EXTR_PORTS) {
        if (!audio_backend_port_at(app_data->backend, BACKEND_PORT_LIST_EXPOSED,
                                   idx, &info))
            return false;
        root_extr_choice_fill(app_data, &info, 0, out);
        return true;
    }
    if (list == LIST_EXTR_PEERS) {
        // empty until a source is chosen - its flow and type pick the list
        BackendPortInfo source;
        if (!root_extr_source(app_data->backend, partial, &source))
            return false;
        if (!audio_backend_port_at(app_data->backend,
                                   root_extr_peer_list(&source), idx, &info))
            return false;
        root_extr_choice_fill(app_data, &info, source.key, out);
        return true;
    }
    return false;
}

// every target is checked before any is linked, so a request that is wrong or
// out of date changes nothing
static DataActionResult root_graph_connect_do(APP_INFO *app_data,
                                              const DataActionReq *req) {
    GRAPH *graph = app_data->graph;
    ContextId source_val = (ContextId)req->connect.source;
    if (CTXID_NS(source_val) != DATA_LIST_NS_GRAPH_PORTS)
        return DATA_ACTION_ERR_INVALID;
    GraphPortInfo source;
    if (!graph_port_by_key(graph, source_val & CTXID_LOCAL_MASK, &source))
        return DATA_ACTION_ERR_STALE;

    size_t connects = 0;
    for (size_t i = 0; i < req->connect.target_count; i++) {
        ContextId target_val = (ContextId)req->connect.targets[i];
        if (CTXID_NS(target_val) != DATA_LIST_NS_GRAPH_PORTS)
            return DATA_ACTION_ERR_INVALID;
        GraphPortInfo target;
        if (!graph_port_by_key(graph, target_val & CTXID_LOCAL_MASK, &target))
            return DATA_ACTION_ERR_STALE;
        // what the peers list leaves out: flow, type, loops, a full MIDI input
        if (!graph_port_keys_connectable(graph, source.key, target.key))
            return DATA_ACTION_ERR_INVALID;
        if (!graph_port_keys_connected(graph, source.key, target.key))
            connects++;
    }
    // each check above counted one new source into a MIDI input. Disconnects
    // in the same request are not counted
    if (source.type == PORT_TYPE_MIDI && source.flow == PORT_FLOW_INPUT &&
        graph_port_connection_count(graph, source.key) + connects >
            MIDI_BUF_MERGE_MAX)
        return DATA_ACTION_ERR_INVALID;

    bool any_failed = false;
    for (size_t i = 0; i < req->connect.target_count; i++) {
        uint64_t target_key = req->connect.targets[i] & CTXID_LOCAL_MASK;
        int rc = graph_port_keys_connected(graph, source.key, target_key)
                     ? graph_disconnect_keys(graph, source.key, target_key)
                     : graph_connect_keys(graph, source.key, target_key);
        if (rc != 0)
            any_failed = true;
    }
    return any_failed ? DATA_ACTION_ERR_DATA : DATA_ACTION_OK;
}

static DataActionResult root_extr_connect_do(APP_INFO *app_data,
                                             const DataActionReq *req) {
    AUDIO_BACKEND *backend = app_data->backend;
    ContextId source_val = (ContextId)req->connect.source;
    if (CTXID_NS(source_val) != DATA_LIST_NS_PORTS)
        return DATA_ACTION_ERR_INVALID;
    BackendPortInfo source;
    if (!audio_backend_port_by_key(backend, source_val & CTXID_LOCAL_MASK,
                                   &source))
        return DATA_ACTION_ERR_STALE;
    // the source list offers only the exposed ports
    if (source.owner_tag == 0)
        return DATA_ACTION_ERR_INVALID;

    for (size_t i = 0; i < req->connect.target_count; i++) {
        ContextId target_val = (ContextId)req->connect.targets[i];
        if (CTXID_NS(target_val) != DATA_LIST_NS_PORTS)
            return DATA_ACTION_ERR_INVALID;
        BackendPortInfo target;
        if (!audio_backend_port_by_key(backend, target_val & CTXID_LOCAL_MASK,
                                       &target))
            return DATA_ACTION_ERR_STALE;
        // the peers list only offers other programs' ports of opposite flow
        // and matching type
        if (target.flow == source.flow || target.type != source.type ||
            target.owner_tag != 0)
            return DATA_ACTION_ERR_INVALID;
    }

    bool any_failed = false;
    for (size_t i = 0; i < req->connect.target_count; i++) {
        uint64_t target_key = req->connect.targets[i] & CTXID_LOCAL_MASK;
        bool linked =
            audio_backend_port_keys_connected(backend, source.key, target_key);
        int rc =
            linked
                ? audio_backend_disconnect_keys(backend, source.key, target_key)
                : audio_backend_connect_keys(backend, source.key, target_key);
        if (rc != 0)
            any_failed = true;
    }
    return any_failed ? DATA_ACTION_ERR_DATA : DATA_ACTION_OK;
}

static DataActionResult root_song_result(SONG_RESULT result) {
    if (result == SONG_INVALID)
        return DATA_ACTION_ERR_INVALID;
    return result == SONG_OK ? DATA_ACTION_OK : DATA_ACTION_ERR_DATA;
}

// a load drops and makes links after a backend port sync whose news
// app_data_update did not see
static void root_song_replaced(APP_INFO *app_data) {
    app_data_push_event(app_data, DATA_EVENT_CHANGED,
                        MAKE_ID(DATA_NS_SINGLETON, SID_ROOT));
}

static DataActionResult root_action_do(void *user_data, void *root_user_data,
                                       const DataActionReq *req,
                                       ContextId *out_new) {
    (void)root_user_data;
    APP_INFO *app_data = (APP_INFO *)user_data;
    (void)out_new; // none of these creates a context
    if (!app_data || !req)
        return DATA_ACTION_ERR_INVALID;
    if (req->type == DATA_ACTION_SAVE)
        return root_song_result(song_save(app_data->song, req->save.name));
    if (req->type == DATA_ACTION_LOAD) {
        SONG_RESULT result =
            song_load(app_data->song, req->add_file_path.path);
        if (result == SONG_OK)
            root_song_replaced(app_data);
        return root_song_result(result);
    }
    if (req->type == DATA_ACTION_COMMAND && req->id == ROOT_ACTION_NEW_SONG) {
        song_new(app_data->song);
        root_song_replaced(app_data);
        return DATA_ACTION_OK;
    }
    if (req->type == DATA_ACTION_COMMAND) {
        int rc = -1;
        if (req->id == ROOT_ACTION_PLAY)
            rc = transport_play(app_data->transport);
        else if (req->id == ROOT_ACTION_STOP)
            rc = transport_stop(app_data->transport);
        else
            return DATA_ACTION_ERR_INVALID;
        return rc == 0 ? DATA_ACTION_OK : DATA_ACTION_ERR_DATA;
    }
    if (req->type != DATA_ACTION_CONNECT)
        return DATA_ACTION_ERR_INVALID;
    if (!req->connect.targets || req->connect.target_count == 0)
        return DATA_ACTION_ERR_INVALID;
    if (req->id == ROOT_ACTION_CONNECT)
        return root_graph_connect_do(app_data, req);
    if (req->id == ROOT_ACTION_EXTR_CONNECT)
        return root_extr_connect_do(app_data, req);
    return DATA_ACTION_ERR_INVALID;
}

static const DataOps root_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN | DATA_CAP_ACTIONS,
    .id = root_id,
    .child_count = root_child_count,
    .child_at = root_child_at,
    .action_list = root_action_list,
    .action_args = root_action_args,
    .list_count = root_list_count,
    .list_at = root_list_at,
    .action_do = root_action_do,
    .name = root_name,
};

// owner's param config file in dir, named by path_safe_name - the owner
// inside the file tells apart two that come out the same. false when it does
// not fit
static bool app_param_config_path(const char *dir, const char *owner,
                                  char *out, size_t out_len) {
    char name[MAX_PATH_STRING];
    if (path_safe_name(owner, name, sizeof(name)) != 0)
        return false;
    int len = snprintf(out, out_len, "%s/%s.json", dir, name);
    return len >= 0 && (size_t)len < out_len;
}

// a param config file lists only the params with a key
static bool app_param_container_keyed(PRM_CONTAIN *container) {
    unsigned int count = param_return_num_params(container, 0);
    for (unsigned int val_id = 0; val_id < count; val_id++) {
        const char *key = param_get_key(container, (int)val_id);
        if (key && key[0] != '\0')
            return true;
    }
    return false;
}

// the owner, "kind:id" or kind alone for an id of "", and its file in dir.
// false (logged) when the path does not fit
static bool app_param_config_owner_path(const char *dir, const char *kind,
                                        const char *id,
                                        char owner[MAX_PATH_STRING],
                                        char path[MAX_PATH_STRING]) {
    if (id[0] != '\0')
        snprintf(owner, MAX_PATH_STRING, "%s:%s", kind, id);
    else
        snprintf(owner, MAX_PATH_STRING, "%s", kind);
    // a cut owner does not fit the path either
    if (app_param_config_path(dir, owner, path, MAX_PATH_STRING))
        return true;
    log_append_logfile("param_config: the file path for %s is too long, not "
                       "read or written\n",
                       owner);
    return false;
}

// a missing file for container, once it has a param with a key. false when it
// could not be written
static bool app_param_config_create(const char *dir, PRM_CONTAIN *container,
                                    const char *owner, const char *path) {
    if (!app_param_container_keyed(container))
        return true;
    if (path_make_dirs(dir) != 0) {
        log_append_logfile("param_config: %s: %s, %s not written\n", dir,
                           strerror(errno), path);
        return false;
    }
    return param_config_write(container, owner, path) != PARAM_CONFIG_FAILED;
}

// a new container reads its param config file. A missing file is written once
// the container has a param with a key - a CLAP plugin may get its params
// after load. A NULL id (an error) reads and writes nothing
static void app_param_config_sync(APP_INFO *app_data, PRM_CONTAIN *container,
                                  const char *kind, const char *id,
                                  bool is_new) {
    if (!container || !id || app_data->param_config_dir[0] == '\0')
        return;
    char owner[MAX_PATH_STRING];
    char path[MAX_PATH_STRING];
    if (!app_param_config_owner_path(app_data->param_config_dir, kind, id,
                                     owner, path))
        return;
    if (is_new) {
        if (param_config_read(container, owner, path) != PARAM_CONFIG_MISSING)
            return;
    } else if (access(path, F_OK) == 0)
        return;
    app_param_config_create(app_data->param_config_dir, container, owner, path);
}

static bool app_param_config_on(APP_INFO *app_data) {
    return app_data && app_data->param_config_dir[0] != '\0';
}

static DataAction app_param_config_reload_action(APP_INFO *app_data) {
    return (DataAction){
        .type = DATA_ACTION_REFRESH,
        .label = "Reload param config",
        .tooltip = "Read the param config file again, into everything that "
                   "shares it",
        .enabled = app_param_config_on(app_data),
        .style = DATA_ACTION_STYLE_NORMAL,
    };
}

// one owner's file, read again into each of its containers
typedef struct {
    const char *kind;
    const char *id;
    char owner[MAX_PATH_STRING];
    char path[MAX_PATH_STRING];
    bool missing;
    bool failed;
} APP_PARAM_RELOAD;

// container reads the file again if it is the reload's owner's. A missing
// file clears its user values
static void app_param_config_reread(APP_PARAM_RELOAD *reload,
                                    PRM_CONTAIN *container, const char *kind,
                                    const char *id) {
    if (!container || !id || strcmp(kind, reload->kind) != 0 ||
        strcmp(id, reload->id) != 0)
        return;
    PARAM_CONFIG_RESULT result =
        param_config_read(container, reload->owner, reload->path);
    if (result == PARAM_CONFIG_MISSING) {
        reload->missing = true;
        if (param_user_table_set(container, NULL, 0) != 0)
            reload->failed = true;
    } else if (result == PARAM_CONFIG_FAILED)
        reload->failed = true;
}

// every container of container's owner reads the file again, a missing file
// is written from container
static DataActionResult app_param_config_reload(APP_INFO *app_data,
                                                PRM_CONTAIN *container,
                                                const char *kind,
                                                const char *id) {
    if (!app_param_config_on(app_data))
        return DATA_ACTION_ERR_NOT_ALLOWED;
    if (!container || !id)
        return DATA_ACTION_ERR_INVALID;
    APP_PARAM_RELOAD reload = {.kind = kind, .id = id};
    if (!app_param_config_owner_path(app_data->param_config_dir, kind, id,
                                     reload.owner, reload.path))
        return DATA_ACTION_ERR_DATA;

    app_param_config_reread(&reload,
                            transport_param_container(app_data->transport),
                            PARAM_CONFIG_KIND_TRK, "");
    size_t osc_count = synth_return_osc_num(app_data->synth_data);
    for (unsigned int i = 0; i < osc_count; i++)
        app_param_config_reread(
            &reload,
            synth_osc_param_container(synth_osc_return(app_data->synth_data, i)),
            PARAM_CONFIG_KIND_SYNTH_OSC, "");
    for (unsigned int i = 0;; i++) {
        void *smp = smp_sample_return(app_data->smp_data, i);
        if (!smp)
            break;
        app_param_config_reread(&reload, smp_sample_param_container(smp),
                                PARAM_CONFIG_KIND_SAMPLER, "");
    }
    for (unsigned int i = 0;; i++) {
        void *plug = plug_plugin_return(app_data->plug_data, i);
        if (!plug)
            break;
        app_param_config_reread(&reload, plug_plugin_param_container(plug),
                                PARAM_CONFIG_KIND_LV2, plug_plugin_uri(plug));
    }
    for (unsigned int i = 0;; i++) {
        void *plug = clap_plug_plugin_return(app_data->clap_plug_data, i);
        if (!plug)
            break;
        app_param_config_reread(&reload,
                                clap_plug_plugin_param_container(plug),
                                PARAM_CONFIG_KIND_CLAP,
                                clap_plug_plugin_id(plug));
    }

    if (reload.missing &&
        !app_param_config_create(app_data->param_config_dir, container,
                                 reload.owner, reload.path))
        reload.failed = true;
    return reload.failed ? DATA_ACTION_ERR_DATA : DATA_ACTION_OK;
}

DataObject app_init(void) {
    const DataObject invalid = {0};
    APP_INFO *app_data = (APP_INFO *)malloc(sizeof(APP_INFO));
    if (!app_data)
        return invalid;

    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = app_start_process;
    rt_funcs_struct.subcx_stop_process = app_stop_process;
    ui_funcs_struct.send_msg = app_sys_msg;
    app_data->control_data = context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if (!app_data->control_data) {
        free(app_data);
        return invalid;
    }
    // init the members to NULLS
    app_data->smp_data = NULL;
    app_data->backend = NULL;
    app_data->graph = NULL;
    app_data->transport = NULL;
    app_data->midi_clock = NULL;
    app_data->rolling_seen = false;
    app_data->sync_seen = TRANSPORT_SYNC_OFF;
    app_data->plug_data = NULL;
    app_data->clap_plug_data = NULL;
    app_data->synth_data = NULL;
    app_data->is_processing = 0;
    app_data->events = NULL;
    app_data->event_count = 0;
    app_data->event_read = 0;
    app_data->event_cap = 0;
    app_data->graph_generation_seen = 0;
    app_data->wake_epoll_fd = -1;
    app_data->rt_ui_timerfd = -1;
    app_data->rt_ui_period_ms = 0;
    app_data->hwin = NULL;
    app_data->hwin_fd_watched = false;
    app_data->param_config_dir[0] = '\0';
    app_data->smp_uid_seen = 0;
    app_data->lv2_uid_seen = 0;
    app_data->clap_uid_seen = 0;
    app_data->song = NULL;

    const SETTINGS_KEY params_keys[] = {
        {.key = "param_config_dir",
         .type = SETTINGS_PATH,
         .out = app_data->param_config_dir,
         .out_len = sizeof(app_data->param_config_dir)},
    };
    settings_read(SETTINGS_SECTION_PARAMS, params_keys,
                  sizeof(params_keys) / sizeof(params_keys[0]));
    // the presets the host saves for clap plugins, "" for none
    char clap_preset_dir[MAX_PATH_STRING] = "";
    char data_dir[MAX_PATH_STRING];
    if (path_user_data_dir(data_dir, sizeof(data_dir)) == 0) {
        int len = snprintf(clap_preset_dir, sizeof(clap_preset_dir),
                           "%s/" SETTINGS_DIR_NAME "/clap_presets", data_dir);
        if (len < 0 || (size_t)len >= sizeof(clap_preset_dir))
            clap_preset_dir[0] = '\0';
    }
    const SETTINGS_KEY plugins_keys[] = {
        {.key = "clap_preset_dir",
         .type = SETTINGS_PATH,
         .out = clap_preset_dir,
         .out_len = sizeof(clap_preset_dir)},
    };
    settings_read(SETTINGS_SECTION_PLUGINS, plugins_keys,
                  sizeof(plugins_keys) / sizeof(plugins_keys[0]));
    // where a relative song path goes, the song a start loads. "" for none
    char song_dir[MAX_PATH_STRING] = "";
    char song_template[MAX_PATH_STRING] = "";
    const SETTINGS_KEY songs_keys[] = {
        {.key = "song_dir",
         .type = SETTINGS_PATH,
         .out = song_dir,
         .out_len = sizeof(song_dir)},
        {.key = "template",
         .type = SETTINGS_PATH,
         .out = song_template,
         .out_len = sizeof(song_template)},
    };
    settings_read(SETTINGS_SECTION_SONGS, songs_keys,
                  sizeof(songs_keys) / sizeof(songs_keys[0]));

    /*init the audio backend for the whole program*/
    /*--------------------------------------------------*/
    app_data->backend =
        audio_backend_init(app_data, APP_NAME, trk_audio_process_rt);
    if (!app_data->backend) {
        clean_memory(app_data);
        return invalid;
    }

    uint32_t buffer_size = audio_backend_buffer_size(app_data->backend);
    SAMPLE_T samplerate = audio_backend_sample_rate(app_data->backend);
    app_data->buffer_size = buffer_size;
    app_data->sample_rate = samplerate;
    app_data->graph = graph_new(buffer_size);
    // the exposed ports (master_in/out, midi_in/out, midi_clock_out) group
    // under the root
    if (!app_data->graph ||
        audio_backend_endpoints_add(app_data->backend, app_data->graph,
                                    DATA_NS_SINGLETON, SID_ROOT) != 0) {
        clean_memory(app_data);
        return invalid;
    }
    app_data->transport = transport_init(samplerate);
    if (!app_data->transport) {
        clean_memory(app_data);
        return invalid;
    }
    // now activate the backend, it will launch the rt thread
    // (trk_audio_process_rt function) but app_data->is_processing == 0, so the
    // graph will not run, only app_data sys messages (to start the
    // processes for example)
    if (audio_backend_activate(app_data->backend) != 0) {
        clean_memory(app_data);
        return invalid;
    }
    /*initiate the sampler it will be empty initialy*/
    /*-----------------------------------------------*/
    smp_status_t smp_status_err = 0;
    app_data->smp_data = smp_init(samplerate, &smp_status_err, app_data->graph,
                                  DATA_NS_SINGLETON, SID_SAMPLER);
    if (!app_data->smp_data) {
        // clean app_data
        clean_memory(app_data);
        return invalid;
    }
    /*--------------------------------------------------*/
    // before any plugin loads - it installs the window system's error handler
    app_data->hwin = hwin_sys_init();
    if (!app_data->hwin) {
        clean_memory(app_data);
        return invalid;
    }
    // Init the plugin data object, it will not run any plugins yet
    plug_status_t plug_errors = 0;
    app_data->plug_data =
        plug_init(buffer_size, samplerate, &plug_errors, app_data->graph,
                  app_data->transport, app_data->hwin, DATA_NS_LV2_PLUG);
    if (!app_data->plug_data) {
        clean_memory(app_data);
        return invalid;
    }
    // build the catalogue of installed lv2 plugins for DATA_ACTION_ADD_CHOICE.
    plug_plugin_list_init(app_data->plug_data);

    clap_plug_status_t clap_plug_errors = 0;
    app_data->clap_plug_data =
        clap_plug_init(buffer_size, buffer_size, samplerate, &clap_plug_errors,
                       app_data->graph, app_data->transport, app_data->hwin,
                       DATA_NS_CLAP_PLUG, clap_preset_dir);
    if (!(app_data->clap_plug_data)) {
        clean_memory(app_data);
        return invalid;
    }
    // same as above, for the clap catalogue
    clap_plug_plugin_list_init(app_data->clap_plug_data);

    // initiate the Synth data
    app_data->synth_data = synth_init(samplerate, 1, app_data->graph,
                                      app_data->transport, DATA_NS_SYNTH_OSC);
    if (!app_data->synth_data) {
        clean_memory(app_data);
        return invalid;
    }
    // the transport's and the oscillators' containers are new only here
    app_param_config_sync(app_data,
                          transport_param_container(app_data->transport),
                          PARAM_CONFIG_KIND_TRK, "", true);
    size_t osc_count = synth_return_osc_num(app_data->synth_data);
    for (unsigned int i = 0; i < osc_count; i++)
        app_param_config_sync(
            app_data,
            synth_osc_param_container(synth_osc_return(app_data->synth_data, i)),
            PARAM_CONFIG_KIND_SYNTH_OSC, "", true);
    // the transport's MIDI clock, wired by the song
    app_data->midi_clock = midi_clock_init(
        app_data->graph, app_data->transport, DATA_NS_SINGLETON, SID_TRK);
    if (!app_data->midi_clock) {
        clean_memory(app_data);
        return invalid;
    }
    const SONG_MODULES song_modules = {
        .graph = app_data->graph,
        .backend = app_data->backend,
        .transport = app_data->transport,
        .synth = app_data->synth_data,
        .smp = app_data->smp_data,
        .lv2 = app_data->plug_data,
        .clap = app_data->clap_plug_data,
        .root = {DATA_NS_SINGLETON, SID_ROOT},
        .sampler = {DATA_NS_SINGLETON, SID_SAMPLER},
        .midi_clock = {DATA_NS_SINGLETON, SID_TRK},
        .synth_osc_tag = DATA_NS_SYNTH_OSC,
        .lv2_tag = DATA_NS_LV2_PLUG,
        .clap_tag = DATA_NS_CLAP_PLUG,
    };
    app_data->song = song_init(&song_modules, song_dir, song_template);
    if (!app_data->song) {
        clean_memory(app_data);
        return invalid;
    }
    // the first cycle already runs a plan with every node
    if (graph_plan_update(app_data->graph) != 0) {
        clean_memory(app_data);
        return invalid;
    }
    // now unpause the jack function again
    context_sub_wait_for_start(app_data->control_data, (void *)app_data);
    // a template's plugins and samples wait for the audio thread to start them
    song_new(app_data->song);

    DataObject root = {.ops = &root_ops,
                       .user_data = (void *)app_data,
                       .root_user_data = (void *)app_data};
    return root;
}

bool app_data_poll_event(void *root_user_data, DataEvent *out) {
    APP_INFO *app_data = (APP_INFO *)root_user_data;
    if (!app_data || !out)
        return false;
    if (app_data->event_read == app_data->event_count) {
        app_data->event_read = 0;
        app_data->event_count = 0;
        return false;
    }
    *out = app_data->events[app_data->event_read++];
    return true;
}

// push a DATA_EVENT_CHANGED for every param of the container whose value, name,
// flags or increment changed since the last call
static void app_data_push_param_changes(APP_INFO *app_data,
                                        PRM_CONTAIN *container) {
    if (!container)
        return;
    int val_ids[64];
    size_t taken = 0;
    while ((taken = param_changed_take(container, val_ids, 64)) > 0) {
        // the same id cx_param_id gives this param
        for (size_t i = 0; i < taken; i++)
            app_data_push_event(
                app_data, DATA_EVENT_CHANGED,
                MAKE_ID(DATA_NS_PARAM,
                        param_get_uid(container, val_ids[i], 0)));
    }
}

// GRAPH_DROPS_FN
static void app_log_midi_drops(void *arg, const GraphPortInfo *port,
                               uint32_t dropped) {
    const char *owner =
        owner_label((APP_INFO *)arg, port->owner_tag, port->owner_uid);
    log_append_logfile("%s %s: %u MIDI events dropped\n", owner ? owner : "?",
                       port->name, dropped);
}

static void app_log_delay_clamp(void *arg, const GraphPortInfo *from,
                                const GraphPortInfo *to, uint32_t needed,
                                uint32_t max) {
    APP_INFO *app_data = (APP_INFO *)arg;
    const char *from_owner =
        owner_label(app_data, from->owner_tag, from->owner_uid);
    const char *to_owner = owner_label(app_data, to->owner_tag, to->owner_uid);
    log_append_logfile("%s %s -> %s %s: needs a %u frame delay, clamped at "
                       "%u\n",
                       from_owner ? from_owner : "?", from->name,
                       to_owner ? to_owner : "?", to->name, needed, max);
}

// follow the backend's sample rate and buffer size. The plan and the plugins
// guard the sizes they were set up for, so the order is free; the graph goes
// first so the plugins' restarts already publish its new buffers
static void app_audio_config_update(APP_INFO *app_data) {
    SAMPLE_T sample_rate = audio_backend_sample_rate(app_data->backend);
    uint32_t buffer_size = audio_backend_buffer_size(app_data->backend);
    if (sample_rate == app_data->sample_rate &&
        buffer_size == app_data->buffer_size)
        return;
    log_append_logfile("Audio config: %.0f Hz, %u frames\n", (double)sample_rate,
                       buffer_size);
    // not retried - a failure would repeat every update
    app_data->sample_rate = sample_rate;
    app_data->buffer_size = buffer_size;
    if (graph_max_buffer_size_set(app_data->graph, buffer_size) != 0)
        log_append_logfile("Graph buffers stay at the old size\n");
    smp_sample_rate_set(app_data->smp_data, sample_rate);
    if (transport_sample_rate_set(app_data->transport, sample_rate) != 0)
        log_append_logfile("Transport stays at the old sample rate\n");
    if (synth_sample_rate_set(app_data->synth_data, sample_rate) != 0)
        log_append_logfile("Synth stays at the old sample rate\n");
    plug_audio_config_set(app_data->plug_data, sample_rate, buffer_size);
    clap_plug_audio_config_set(app_data->clap_plug_data, sample_rate,
                               buffer_size, buffer_size);
}

// the clock's requests to the server's transport
static void app_transport_requests(APP_INFO *app_data) {
    TRANSPORT_REQ req;
    while (transport_request_take(app_data->transport, &req)) {
        switch (req.kind) {
        case TRANSPORT_REQ_START:
            audio_backend_transport_start(app_data->backend);
            break;
        case TRANSPORT_REQ_STOP:
            audio_backend_transport_stop(app_data->backend);
            break;
        case TRANSPORT_REQ_LOCATE:
            audio_backend_transport_locate(app_data->backend, req.frame);
            break;
        }
    }
}

// lead the server's musical position while Sync is Lead. Sync turns Follow
// when that fails or another program takes the lead
static void app_transport_sync_update(APP_INFO *app_data) {
    TransportSync sync = transport_sync(app_data->transport);
    if (sync == TRANSPORT_SYNC_LEAD &&
        app_data->sync_seen == TRANSPORT_SYNC_LEAD &&
        audio_backend_transport_lead_lost(app_data->backend)) {
        log_append_logfile("Another program leads the server's transport\n");
        transport_sync_set(app_data->transport, TRANSPORT_SYNC_FOLLOW);
        sync = TRANSPORT_SYNC_FOLLOW;
    }
    if (sync == app_data->sync_seen)
        return;
    app_data->sync_seen = sync;
    bool lead = sync == TRANSPORT_SYNC_LEAD;
    if (audio_backend_transport_lead(app_data->backend,
                                     lead ? app_transport_lead_rt : NULL,
                                     lead ? app_data->transport : NULL) == 0)
        return;
    log_append_logfile("Could not lead the server's transport\n");
    if (!lead)
        return;
    transport_sync_set(app_data->transport, TRANSPORT_SYNC_FOLLOW);
    app_data->sync_seen = TRANSPORT_SYNC_FOLLOW;
}

// the window connection's fd joins the wake epoll once both exist
static void app_wake_hwin_watch(APP_INFO *app_data) {
    if (app_data->hwin_fd_watched || app_data->wake_epoll_fd < 0)
        return;
    int fd = hwin_sys_fd(app_data->hwin);
    if (fd < 0)
        return;
    struct epoll_event ev = {.events = EPOLLIN};
    ev.data.fd = fd;
    app_data->hwin_fd_watched =
        epoll_ctl(app_data->wake_epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

int app_data_wake_fd(void *root_user_data, uint32_t period_ms) {
    if (!root_user_data)
        return -1;
    APP_INFO *app_data = (APP_INFO *)root_user_data;
    if (app_data->wake_epoll_fd < 0) {
        app_data->wake_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (app_data->wake_epoll_fd < 0)
            return -1;
        int clap_fd = clap_plug_wake_fd(app_data->clap_plug_data);
        if (clap_fd >= 0) {
            struct epoll_event ev = {.events = EPOLLIN};
            ev.data.fd = clap_fd;
            epoll_ctl(app_data->wake_epoll_fd, EPOLL_CTL_ADD, clap_fd, &ev);
        }
        app_data->rt_ui_timerfd =
            timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (app_data->rt_ui_timerfd >= 0) {
            struct epoll_event ev = {.events = EPOLLIN};
            ev.data.fd = app_data->rt_ui_timerfd;
            epoll_ctl(app_data->wake_epoll_fd, EPOLL_CTL_ADD,
                     app_data->rt_ui_timerfd, &ev);
        }
        app_wake_hwin_watch(app_data);
    }
    if (app_data->rt_ui_timerfd >= 0 &&
        period_ms != app_data->rt_ui_period_ms) {
        struct itimerspec spec = {
            .it_value = {.tv_sec = period_ms / 1000,
                        .tv_nsec = (long)(period_ms % 1000) * 1000000L},
            .it_interval = {.tv_sec = period_ms / 1000,
                           .tv_nsec = (long)(period_ms % 1000) * 1000000L}};
        timerfd_settime(app_data->rt_ui_timerfd, 0, &spec, NULL);
        app_data->rt_ui_period_ms = period_ms;
    }
    return app_data->wake_epoll_fd;
}

// a params container's structural change -> CHILDREN_CHANGED on its owner.
// is_new: the container appeared since the last update
static void app_param_container_update(APP_INFO *app_data,
                                       PRM_CONTAIN *container, ContextId owner,
                                       const char *kind, const char *id,
                                       bool is_new) {
    // before the poll, so what the file changes is in this update's event
    if (is_new)
        app_param_config_sync(app_data, container, kind, id, true);
    if (!param_container_changed(container))
        return;
    app_data_push_event(app_data, DATA_EVENT_CHILDREN_CHANGED, owner);
    if (!is_new)
        app_param_config_sync(app_data, container, kind, id, false);
}

// every params container. Sample and plugin uids only grow, so one above the
// newest seen is new; the synth's and Trk's were new in app_init
static void app_param_containers_update(APP_INFO *app_data) {
    app_param_container_update(app_data,
                               transport_param_container(app_data->transport),
                               trk_id(NULL), PARAM_CONFIG_KIND_TRK, "", false);
    size_t osc_count = synth_return_osc_num(app_data->synth_data);
    for (unsigned int i = 0; i < osc_count; i++) {
        void *osc = synth_osc_return(app_data->synth_data, i);
        app_param_container_update(app_data, synth_osc_param_container(osc),
                                   osc_id(osc), PARAM_CONFIG_KIND_SYNTH_OSC, "",
                                   false);
    }
    uint32_t newest = app_data->smp_uid_seen;
    for (unsigned int i = 0;; i++) {
        void *smp = smp_sample_return(app_data->smp_data, i);
        if (!smp)
            break;
        uint32_t uid = smp_sample_uid(smp);
        app_param_container_update(app_data, smp_sample_param_container(smp),
                                   sample_id(smp), PARAM_CONFIG_KIND_SAMPLER,
                                   "", uid > app_data->smp_uid_seen);
        if (uid > newest)
            newest = uid;
    }
    app_data->smp_uid_seen = newest;
    newest = app_data->lv2_uid_seen;
    for (unsigned int i = 0;; i++) {
        void *plug = plug_plugin_return(app_data->plug_data, i);
        if (!plug)
            break;
        uint32_t uid = plug_plugin_uid(plug);
        app_param_container_update(app_data, plug_plugin_param_container(plug),
                                   lv2_plugin_id(plug), PARAM_CONFIG_KIND_LV2,
                                   plug_plugin_uri(plug),
                                   uid > app_data->lv2_uid_seen);
        if (uid > newest)
            newest = uid;
    }
    app_data->lv2_uid_seen = newest;
    newest = app_data->clap_uid_seen;
    for (unsigned int i = 0;; i++) {
        void *plug = clap_plug_plugin_return(app_data->clap_plug_data, i);
        if (!plug)
            break;
        uint32_t uid = clap_plug_plugin_uid(plug);
        app_param_container_update(
            app_data, clap_plug_plugin_param_container(plug),
            clap_plugin_id(plug), PARAM_CONFIG_KIND_CLAP,
            clap_plug_plugin_id(plug), uid > app_data->clap_uid_seen);
        if (uid > newest)
            newest = uid;
    }
    app_data->clap_uid_seen = newest;
}

void app_data_update(void *root_user_data) {
    if (!root_user_data)
        return;
    APP_INFO *app_data = (APP_INFO *)root_user_data;
    // drain the periodic rt->ui timerfd - level-triggered, so leaving it
    // unread would keep the wake epoll fd readable forever. One read empties
    // the whole count and resets it to 0.
    if (app_data->rt_ui_timerfd >= 0) {
        uint64_t expirations;
        read(app_data->rt_ui_timerfd, &expirations, sizeof(expirations));
    }
    // read app_data messages from [audio-thread] on the [main-thread]
    context_sub_process_ui(app_data->control_data);
    transport_update(app_data->transport);
    app_transport_requests(app_data);
    app_transport_sync_update(app_data);
    // before the contexts' reads, so a GUI window's close is handled in this
    // cycle
    hwin_sys_process(app_data->hwin);
    // read messages from rt thread on [main-thread] for CLAP plugins
    clap_read_rt_to_ui_messages(app_data->clap_plug_data);
    // read messages from the rt thread on the [main-thread] for lv2 plugins
    plug_read_rt_to_ui_messages(app_data->plug_data);
    // read messages from the rt thread on the [main-thread] for sampler
    smp_read_rt_to_ui_messages(app_data->smp_data);
    // read messages from the rt thread on the [main-thread] for the synth
    // context
    synth_read_rt_to_ui_messages(app_data->synth_data);
    // a GUI shown since the last update, or by the reads above, connects
    app_wake_hwin_watch(app_data);
    app_audio_config_update(app_data);
    graph_compensation_set(app_data->graph, transport_pdc(app_data->transport));
    // publish what the contexts changed in the graph, free what the audio
    // thread no longer runs
    graph_plan_update(app_data->graph);
    graph_delay_clamps_take(app_data->graph, app_log_delay_clamp, app_data);
    if (audio_backend_latency_update(app_data->backend))
        log_append_logfile("Exposed port latency capped - a loop through "
                           "another program?\n");
    graph_midi_drops_take(app_data->graph, app_log_midi_drops, app_data);

    // now that all rt->ui messages are drained, turn each module's "my contents
    // is dirty" flag into a CHILDREN_CHANGED event.
    // (the synth and trk has no such flag - their oscillators are fixed.)
    if (smp_samples_is_dirty(app_data->smp_data))
        app_data_push_event(app_data, DATA_EVENT_CHILDREN_CHANGED,
                            MAKE_ID(DATA_NS_SINGLETON, SID_SAMPLER));
    if (plug_plugins_is_dirty(app_data->plug_data))
        app_data_push_event(app_data, DATA_EVENT_CHILDREN_CHANGED,
                            MAKE_ID(DATA_NS_SINGLETON, SID_LV2_PLUGINS));
    if (clap_plug_plugins_is_dirty(app_data->clap_plug_data))
        app_data_push_event(app_data, DATA_EVENT_CHILDREN_CHANGED,
                            MAKE_ID(DATA_NS_SINGLETON, SID_CLAP_PLUGINS));

    // same, one level down: each params container's own param set
    app_param_containers_update(app_data);

    // ports are not in the CX tree, so a backend or graph port / edge change is
    // a data changed event on root, not a structure change
    BackendPortSync port_sync = audio_backend_ports_sync(app_data->backend);
    uint64_t graph_gen = graph_generation(app_data->graph);
    bool graph_changed = graph_gen != app_data->graph_generation_seen;
    app_data->graph_generation_seen = graph_gen;
    // root's Play is enabled only while stopped
    bool rolling = transport_rolling(app_data->transport);
    bool rolling_changed = rolling != app_data->rolling_seen;
    app_data->rolling_seen = rolling;
    if (port_sync.ports || port_sync.connections || graph_changed ||
        rolling_changed)
        app_data_push_event(app_data, DATA_EVENT_CHANGED,
                            MAKE_ID(DATA_NS_SINGLETON, SID_ROOT));

    // param value/name/flag and plugin latency / GUI shown changes, after the
    // structure events above so a just added param or plugin already has its
    // context
    app_data_push_param_changes(app_data,
                                transport_param_container(app_data->transport));
    for (unsigned int i = 0;; i++) {
        void *smp = smp_sample_return(app_data->smp_data, i);
        if (!smp)
            break;
        app_data_push_param_changes(app_data, smp_sample_param_container(smp));
    }
    for (unsigned int i = 0;; i++) {
        void *plug = plug_plugin_return(app_data->plug_data, i);
        if (!plug)
            break;
        app_data_push_param_changes(app_data,
                                    plug_plugin_param_container(plug));
        // both flags cleared
        bool latency_changed = plug_plugin_latency_changed(plug);
        bool gui_changed = plug_plugin_gui_changed(plug);
        if (latency_changed || gui_changed)
            app_data_push_event(app_data, DATA_EVENT_CHANGED,
                                lv2_plugin_id(plug));
    }
    for (unsigned int i = 0;; i++) {
        void *plug = clap_plug_plugin_return(app_data->clap_plug_data, i);
        if (!plug)
            break;
        app_data_push_param_changes(app_data,
                                    clap_plug_plugin_param_container(plug));
        bool latency_changed = clap_plug_plugin_latency_changed(plug);
        bool gui_changed = clap_plug_plugin_gui_changed(plug);
        if (latency_changed || gui_changed)
            app_data_push_event(app_data, DATA_EVENT_CHANGED,
                                clap_plugin_id(plug));
    }
    size_t osc_count = synth_return_osc_num(app_data->synth_data);
    for (unsigned int i = 0; i < osc_count; i++)
        app_data_push_param_changes(
            app_data,
            synth_osc_param_container(synth_osc_return(app_data->synth_data, i)));
}

void app_stop_and_clean(void *root_user_data) {
    if (!root_user_data)
        return;
    APP_INFO *app_data = (APP_INFO *)root_user_data;
    // the clock's slave hears Stop while the graph still runs - the cycle
    // that pushes it ends before the stop's cycle starts
    midi_clock_halt(app_data->midi_clock);
    context_sub_wait_for_stop(app_data->control_data, root_user_data);

    clean_memory(app_data);
}
