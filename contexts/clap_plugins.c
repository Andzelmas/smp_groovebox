#include "clap_plugins.h"
#include "../types.h"
#include "../util_funcs/intern_table.h"
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/math_funcs.h"
#include "../util_funcs/midi_buf.h"
#include "../util_funcs/path_funcs.h"
#include "../util_funcs/ring_buffer.h"
#include "../util_funcs/uniform_buffer.h"
#include "clap_scan.h"
#include "context_control.h"
#include <clap/clap.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include "clap_ext/clap_ext_preset_load.h"

// what is the size of the buffer to get the formated param values to
#define MAX_VALUE_LEN 64

// the default paths that clap plugins can be in, searched after CLAP_PATH
static const char *clap_paths[] = {"/usr/lib/clap", "~/.clap", NULL};

// how long the scanner may take over one .clap file before it is killed
#define CLAP_SCAN_TIMEOUT_MS 10000
// more than any real listing, so a runaway scanner is cut off
#define CLAP_SCAN_MAX_OUTPUT (1 << 20)
// the same two for indexing a plugin's presets - a big library takes a while
#define CLAP_SCAN_PRESETS_TIMEOUT_MS 60000
#define CLAP_SCAN_PRESETS_MAX_OUTPUT (32 << 20)

extern char **environ;

// how many clap plugins can there be in the plugin array
#define MAX_INSTANCES 5

// size of the single plugin display name buffer (CLAP_PLUG_PLUG.name). Internal
// to this module - the name leaves as a const char* so callers need no matching
// define.
#define CLAP_PLUGIN_NAME_MAX 128
// a graph port's name: the CLAP port's, and its channel
#define CLAP_PORT_NAME_MAX (CLAP_NAME_SIZE + 16)

// size of the input and output event lists. The input one holds the param
// changes and the note ports' events, the output one the plugin's events but
// note and MIDI ones (those go straight to the note ports). A param value is
// the largest event either holds
#define EVENT_LIST_ITEMS 1024
#define EVENT_LIST_SIZE                                                        \
    ((uint32_t)(EVENT_LIST_ITEMS * sizeof(clap_event_param_value_t)))

static thread_local bool is_audio_thread = false;
// set in clap_plug_init. Plugins can call the host from their own threads too,
// so "not audio" does not mean [main-thread]
static thread_local bool is_main_thread = false;

// requests plugins make through the [thread-safe] host functions. Set on any
// thread, handled on [main-thread] by clap_plug_requests_process
enum {
    CLAP_PLUG_REQ_CALLBACK = 1U << 0,
    CLAP_PLUG_REQ_RESTART = 1U << 1,
    CLAP_PLUG_REQ_PROCESS = 1U << 2,
    CLAP_PLUG_REQ_FLUSH = 1U << 3,
    // not a plugin request - set by the host when the ui side missed values
    // from the plugin or after a preset load, so they are read again from
    // the plugin
    CLAP_PLUG_REQ_SYNC_VALUES = 1U << 4,
};

// plugin list item - from this struct a clap plugin can be loaded
typedef struct _plugin_list_item {
    // full plugin path with the directory and all,
    // including the path from clap_paths too
    char path[MAX_PATH_STRING];
    // short name that is used in the clap descriptor
    char short_name[MAX_SHORT_NAME_LENGTH];
    // the plugin instance id in the clap plugin descriptor
    int plug_inst_id;
    uint64_t key;
    CLAP_PLUG_INFO *plug_data;
} PLUGIN_LIST_ITEM;

// struct that holds the PLUGIN_LIST_ITEMS
typedef struct _plugin_list {
    // maximum size of the plugin_list that is currently allocated -
    // before a realloc is needed
    unsigned int size_max;
    // current size of the plugin_list array
    unsigned int size_curr;
    PLUGIN_LIST_ITEM *plugin_list;
    // every descriptor id listed this session, so a key keeps meaning the same
    // plugin across rebuilds. The slot is its index in plugin_list
    INTERN_TABLE ids;
} PLUGIN_LIST;

// the graph ports of one clap audio port, one per channel
typedef struct _clap_plug_port_graph {
    uint32_t channel_count; // how many channels on one clap plugin port (the
                            // graph_ports array will be the same size)
    GRAPH_PORT **graph_ports;
} CLAP_PLUG_PORT_GRAPH;

// port struct that holds the graph port array and clap_audio_buffer_t array
typedef struct _clap_plug_port {
    uint32_t ports_count;
    CLAP_PLUG_PORT_GRAPH *graph_port_array;
    clap_audio_buffer_t *audio_ports;
    // what each channel buffer holds, 0 when not allocated
    uint32_t frames;
} CLAP_PLUG_PORT;

// note port struct holds the graph ports and info about the ports
typedef struct _clap_plug_note_port {
    uint32_t ports_count;
    GRAPH_PORT **graph_ports;
    clap_id *ids;
    uint32_t *supported_dialects;
    uint32_t *preferred_dialects;
    // [audio-thread] the graph ports' buffers this cycle
    MIDI_BUF **midi_bufs;
} CLAP_PLUG_NOTE_PORT;

// the single clap plugin struct
typedef struct _clap_plug_plug {
    int id; // plugin id on the clap_plug_info plugin array (slot index, reused)

    // monotonic per-module id, assigned at load, never reused. Identity for the
    // context/ui layers (see clap_plug_plugin_uid).
    uint32_t uid;
    // display name, owned by this struct, built once at load by
    // clap_plug_load_and_activate() and handed out by clap_plug_plugin_name()
    char name[CLAP_PLUGIN_NAME_MAX];
    char plugin_id[MAX_UNIQUE_ID_STRING]; // unique plugin id that is from the
                                          // clap_plugin_descriptor. Used rarely
                                          // (now to match if preset container
                                          // from preset-factory is can be used
                                          // with the plugin)
    void *dso_handle; // the dso handle to which plug_entry is dlsym() linked
    clap_plugin_entry_t *plug_entry; // the clap library file for this plugin
    const clap_plugin_t *plug_inst;  // the plugin instance
    unsigned int plug_inst_created;  // was a init function called in the
                                     // descriptor for this plugin
    unsigned int
        plug_inst_activated; // was the activate function called on this plugin
    // 0 - not processing, 1 - processing, 2 - sleeping (not processing, but
    // [audio-thread] keeps sending params and checking if input events or input
    // audio is not_quiet)
    unsigned int plug_inst_processing; // is the plugin instance processing,
                                       // touch only on [audio_thread]
    int plug_inst_id; // the plugin instance index in the array of the plugin
                      // factory
    char plug_path[MAX_PATH_STRING]; // the path for the clap file
    PRM_CONTAIN *plug_params;        // plugin parameter container for params.c
    clap_host_t clap_host_info; // need when creating the plugin instance, this
                                // struct has this CLAP_PLUG_PLUG in the
                                // host_data var as (void*)
    CLAP_PLUG_INFO *plug_data;  // CLAP_PLUG_INFO struct address for convenience
    // the plugin's node, its audio and note ports are on it
    GRAPH_NODE *node;
    // CLAP_PLUG_PORT holds the graph ports and the clap_audio_buffer_t
    // arrays, to send to plugin process function
    CLAP_PLUG_PORT input_ports;
    CLAP_PLUG_PORT output_ports;
    // input and output note ports
    CLAP_PLUG_NOTE_PORT input_note_ports;
    CLAP_PLUG_NOTE_PORT output_note_ports;
    // input output event streams. output_events.ctx is this plug: try_push
    // sends note and MIDI events to the output note ports, the rest (param
    // events) to out_list
    clap_input_events_t input_events;
    clap_output_events_t output_events;
    UB_EVENT *out_list;
    // the preset list from the plugin's preset-discovery factory, built on
    // first browse (see clap_plug_presets_get)
    TREE_INDEX *presets;
    bool presets_built;
    // CLAP_PLUG_REQ_* bits, set from any thread
    atomic_uint requests;
    // events the input event list had no room for, taken on [main-thread]
    atomic_uint in_events_dropped;
    // output events out_list had no room for, or with no such port or note,
    // taken on [main-thread]. A full MIDI_BUF counts its own drops
    atomic_uint out_events_dropped;
} CLAP_PLUG_PLUG;

// the main clap struct
typedef struct _clap_plug_info {
    // array with single clap plugins. there can be gaps (a plugin removed from
    // the middle); clap_plug_plugin_return() walks the occupied slots in order
    // for the UI.
    struct _clap_plug_plug plugins[MAX_INSTANCES];
    bool plugins_dirty; // did plugins array change?
    uint32_t
        next_plug_uid; // monotonic counter for CLAP_PLUG_PLUG.uid, never reset
    SAMPLE_T sample_rate;
    // for clap there can be min and max buffer sizes, for not changing buffer
    // sizes set as the same
    uint32_t min_buffer_size;
    uint32_t max_buffer_size;
    // the list of plugins available on the system
    PLUGIN_LIST clap_plugin_list;
    // placeholder for the host data that plugins send to the host, this has
    // clap_host_info.host_data = NULL, the clap_host_t has the CLAP_PLUG_PLUG*
    // plug in clap_host_info.host_data. This is just for convenience - to copy
    // to plug clap_host_info the necessary info.
    clap_host_t clap_host_info;
    // control_data struct that handles the sys messages between [audio-thread]
    // and [main-thread] (stop plugin, start plugin, etc.)
    CXCONTROL *control_data;
    // from here hold various host extension implementation structs
    clap_host_thread_check_t
        ext_thread_check; // struct that holds functions to check if the thread
                          // is main or audio
    clap_host_log_t ext_log; // struct that holds function for logging messages
                             // (severity is not sent)
    clap_host_audio_ports_t ext_audio_ports; // struct that holds functions for
                                             // the audio_ports extensions
    clap_host_note_ports_t ext_note_ports;   // struct that holds functions for
                                             // the note-ports extension
    clap_host_params_t
        ext_params; // struct that holds functions for the params extension
    clap_host_preset_load_t ext_preset_load; // struct that holds functions for
                                             // the preset-load extension
    GRAPH *graph;
    // paired with a plugin's uid as its node's owner
    uint64_t owner_tag;
} CLAP_PLUG_INFO;

// return the clap_plug_plug id on the plugins array that has the same
// plug_entry (initiated) if no plugin has this plug_entry return -1
static int
clap_plug_return_plug_id_with_same_plug_entry(CLAP_PLUG_INFO *plug_data,
                                              CLAP_PLUG_PLUG *plug) {
    int return_id = -1;
    if (!plug_data)
        return -1;
    if (!plug)
        return -1;
    if (!plug->plug_entry)
        return -1;

    for (unsigned int plug_num = 0; plug_num < MAX_INSTANCES; plug_num++) {
        CLAP_PLUG_PLUG *cur_plug = &(plug_data->plugins[plug_num]);
        if (!cur_plug)
            continue;
        if (cur_plug->id == plug->id)
            continue;
        if (cur_plug->plug_entry != plug->plug_entry)
            continue;
        return_id = cur_plug->id;
        break;
    }

    return return_id;
}

static int clap_plug_destroy_graph_ports(CLAP_PLUG_INFO *plug_data,
                                         CLAP_PLUG_PORT_GRAPH *ports_graph,
                                         uint32_t port_count) {
    if (!plug_data)
        return -1;
    if (!ports_graph)
        return -1;
    for (uint32_t i = 0; i < port_count; i++) {
        CLAP_PLUG_PORT_GRAPH *cur_port = &(ports_graph[i]);
        uint32_t channels = cur_port->channel_count;
        cur_port->channel_count = 0;
        if (!cur_port->graph_ports)
            continue;
        for (uint32_t chan = 0; chan < channels; chan++)
            graph_port_remove(plug_data->graph, cur_port->graph_ports[chan]);
        free(cur_port->graph_ports);
    }
    free(ports_graph);
    return 0;
}

static int clap_plug_destroy_audio_ports(clap_audio_buffer_t *ports_audio,
                                         uint32_t port_count) {
    if (!ports_audio)
        return -1;
    for (uint32_t i = 0; i < port_count; i++) {
        clap_audio_buffer_t *cur_audio_port = &(ports_audio[i]);
        uint32_t channels = cur_audio_port->channel_count;
        cur_audio_port->channel_count = 0;
        if (cur_audio_port->data32) {
            for (uint32_t chan = 0; chan < channels; chan++) {
                if (cur_audio_port->data32[chan])
                    free(cur_audio_port->data32[chan]);
            }
            free(cur_audio_port->data32);
        }
        if (cur_audio_port->data64) {
            for (uint32_t chan = 0; chan < channels; chan++) {
                if (cur_audio_port->data64[chan])
                    free(cur_audio_port->data64[chan]);
            }
            free(cur_audio_port->data64);
        }
    }
    free(ports_audio);
    return 0;
}

static int clap_plug_destroy_ports(CLAP_PLUG_INFO *plug_data,
                                   CLAP_PLUG_PORT *port) {
    if (!plug_data)
        return -1;
    if (!port)
        return -1;
    clap_plug_destroy_graph_ports(plug_data, port->graph_port_array,
                                  port->ports_count);
    port->graph_port_array = NULL;
    clap_plug_destroy_audio_ports(port->audio_ports, port->ports_count);
    port->audio_ports = NULL;
    port->ports_count = 0;
    port->frames = 0;
    return 0;
}

// [main-thread] every channel buffer of the ports to hold frames, zeroed, while
// the plugin is not activated. -1 on failure - frames stays 0
static int clap_plug_audio_buffers_fit(CLAP_PLUG_PORT *port, uint32_t frames) {
    if (port->frames == frames)
        return 0;
    port->frames = 0;
    for (uint32_t i = 0; port->audio_ports && i < port->ports_count; i++) {
        clap_audio_buffer_t *audio = &(port->audio_ports[i]);
        for (uint32_t chan = 0; chan < audio->channel_count; chan++) {
            if (audio->data32) {
                float *fresh = calloc(frames, sizeof(float));
                if (!fresh)
                    return -1;
                free(audio->data32[chan]);
                audio->data32[chan] = fresh;
            }
            if (audio->data64) {
                double *fresh = calloc(frames, sizeof(double));
                if (!fresh)
                    return -1;
                free(audio->data64[chan]);
                audio->data64[chan] = fresh;
            }
        }
    }
    port->frames = frames;
    return 0;
}

// the port's name, with _chan_num for an audio channel (chan_num >= 0)
static int clap_plug_port_name_create(int name_size, char *full_name,
                                      const char *port_name, int chan_num) {
    if (!port_name)
        return -1;
    if (name_size <= 0)
        return -1;
    if (!full_name)
        return -1;
    if (chan_num < 0) {
        snprintf(full_name, name_size, "%s", port_name);
        return 0;
    }
    snprintf(full_name, name_size, "%s_%d", port_name, chan_num);
    return 0;
}

static int clap_plug_ports_rename(CLAP_PLUG_INFO *plug_data,
                                  CLAP_PLUG_PLUG *plug, CLAP_PLUG_PORT *ports,
                                  int input_ports) {
    if (!plug_data)
        return -1;
    if (!plug)
        return -1;
    if (!ports)
        return -1;
    if (!plug->plug_inst)
        return -1;
    // TODO in clap plugin source code warns to scan ports only if the plugin is
    // deactivated, but the rescan flag of rename allows to rescan the ports
    // right away?
    const clap_plugin_audio_ports_t *clap_plug_ports =
        plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_AUDIO_PORTS);
    if (!clap_plug_ports)
        return -1;

    uint32_t port_count = ports->ports_count;
    if (port_count != clap_plug_ports->count(plug->plug_inst, input_ports))
        return -1;

    for (uint32_t i = 0; i < port_count; i++) {
        CLAP_PLUG_PORT_GRAPH cur_graph_port = ports->graph_port_array[i];
        if (!cur_graph_port.graph_ports)
            continue;
        clap_audio_port_info_t port_info;
        if (!clap_plug_ports->get(plug->plug_inst, i, 0, &port_info))
            continue;
        for (uint32_t chan = 0; chan < cur_graph_port.channel_count; chan++) {
            char full_port_name[CLAP_PORT_NAME_MAX];
            if (clap_plug_port_name_create(CLAP_PORT_NAME_MAX, full_port_name,
                                           port_info.name, chan) != 0)
                continue;
            if (!cur_graph_port.graph_ports[chan])
                continue;
            graph_port_rename(plug_data->graph,
                              cur_graph_port.graph_ports[chan], full_port_name);
        }
    }
    return 0;
}

static int clap_plug_create_ports(CLAP_PLUG_INFO *plug_data, int id,
                                  CLAP_PLUG_PORT *port, int input_ports) {
    if (!plug_data)
        return -1;
    if (!port)
        return -1;
    if (id >= MAX_INSTANCES || id < 0)
        return -1;
    CLAP_PLUG_PLUG *plug = &(plug_data->plugins[id]);
    if (plug->plug_inst_created != 1)
        return -1;
    if (!plug->plug_inst)
        return -1;

    const clap_plugin_audio_ports_t *clap_plug_ports =
        plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_AUDIO_PORTS);
    // without the extension the plugin has no audio ports
    if (!clap_plug_ports)
        return 0;

    uint32_t clap_ports_count =
        clap_plug_ports->count(plug->plug_inst, input_ports);
    if (clap_ports_count <= 0)
        return 0;
    // create the graph port and clap audio buffer port arrays
    port->graph_port_array =
        malloc(sizeof(CLAP_PLUG_PORT_GRAPH) * clap_ports_count);
    if (!port->graph_port_array)
        return -1;
    port->audio_ports = malloc(sizeof(clap_audio_buffer_t) * clap_ports_count);
    if (!port->audio_ports) {
        clap_plug_destroy_graph_ports(plug_data, port->graph_port_array, 0);
        port->graph_port_array = NULL;
        return -1;
    }

    for (uint32_t i = 0; i < clap_ports_count; i++) {
        CLAP_PLUG_PORT_GRAPH *cur_graph_port = &(port->graph_port_array[i]);
        cur_graph_port->channel_count = 0;
        cur_graph_port->graph_ports = NULL;
        clap_audio_buffer_t *cur_clap_port = &(port->audio_ports[i]);
        cur_clap_port->channel_count = 0;
        cur_clap_port->constant_mask = 0;
        cur_clap_port->data32 = NULL;
        cur_clap_port->data64 = NULL;
        cur_clap_port->latency = 0;

        clap_audio_port_info_t port_info;
        if (!clap_plug_ports->get(plug->plug_inst, i, input_ports, &port_info))
            continue;
        uint32_t channels = port_info.channel_count;

        // a graph port per channel. One that fails stays NULL - silence in,
        // nothing out
        cur_graph_port->graph_ports = malloc(sizeof(GRAPH_PORT *) * channels);
        if (cur_graph_port->graph_ports) {
            for (uint32_t chan = 0; chan < channels; chan++) {
                cur_graph_port->graph_ports[chan] = NULL;
                char full_port_name[CLAP_PORT_NAME_MAX];
                if (clap_plug_port_name_create(CLAP_PORT_NAME_MAX,
                                               full_port_name, port_info.name,
                                               chan) != 0)
                    continue;
                unsigned int io_flow = PORT_FLOW_OUTPUT;
                if (input_ports == 1)
                    io_flow = PORT_FLOW_INPUT;
                cur_graph_port->graph_ports[chan] =
                    graph_port_create(plug_data->graph, plug->node,
                                      PORT_TYPE_AUDIO, io_flow, full_port_name);
            }
            cur_graph_port->channel_count = channels;
        }

        // the clap_audio_buffer's channel arrays, their buffers made by
        // clap_plug_audio_buffers_fit
        cur_clap_port->data64 = NULL;
        cur_clap_port->data32 = calloc(channels, sizeof(float *));
        if (cur_clap_port->data32) {
            if ((port_info.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) ==
                CLAP_AUDIO_PORT_SUPPORTS_64BITS)
                cur_clap_port->data64 = calloc(channels, sizeof(double *));
            cur_clap_port->constant_mask = 0;
            cur_clap_port->latency = 0;
            cur_clap_port->channel_count = channels;
        }
    }
    port->ports_count = clap_ports_count;
    return clap_plug_audio_buffers_fit(port, plug_data->max_buffer_size);
}

// rename the note ports
static int clap_plug_note_ports_rename(CLAP_PLUG_INFO *plug_data,
                                       CLAP_PLUG_PLUG *plug, bool input_ports) {
    if (!plug_data)
        return -1;
    if (!plug)
        return -1;
    if (!plug->plug_inst)
        return -1;
    // TODO in clap plugin source code warns to scan ports only if the plugin is
    // deactivated, but the rescan flag of rename allows to rescan the ports
    // right away?
    const clap_plugin_note_ports_t *clap_note_ports =
        plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_NOTE_PORTS);
    if (!clap_note_ports)
        return -1;

    CLAP_PLUG_NOTE_PORT cur_port = plug->output_note_ports;
    if (input_ports)
        cur_port = plug->input_note_ports;
    uint32_t port_count = cur_port.ports_count;
    if (port_count != clap_note_ports->count(plug->plug_inst, input_ports))
        return -1;

    for (uint32_t i = 0; i < port_count; i++) {
        GRAPH_PORT *cur_graph_port = cur_port.graph_ports[i];
        if (!cur_graph_port)
            continue;
        clap_note_port_info_t note_port_info;
        if (!clap_note_ports->get(plug->plug_inst, i, input_ports,
                                  &note_port_info))
            continue;
        char full_port_name[CLAP_PORT_NAME_MAX];
        if (clap_plug_port_name_create(CLAP_PORT_NAME_MAX, full_port_name,
                                       note_port_info.name, -1) != 0)
            continue;
        graph_port_rename(plug_data->graph, cur_graph_port, full_port_name);
    }
    return 0;
}

// clear note_port memory
static int clap_plug_note_ports_destroy(CLAP_PLUG_INFO *plug_data,
                                        CLAP_PLUG_NOTE_PORT *note_port) {
    if (!plug_data)
        return -1;
    if (!note_port)
        return -1;

    for (uint32_t i = 0; note_port->graph_ports && i < note_port->ports_count;
         i++)
        graph_port_remove(plug_data->graph, note_port->graph_ports[i]);
    free(note_port->graph_ports);
    note_port->graph_ports = NULL;
    free(note_port->ids);
    note_port->ids = NULL;
    free(note_port->preferred_dialects);
    note_port->preferred_dialects = NULL;
    free(note_port->supported_dialects);
    note_port->supported_dialects = NULL;
    free(note_port->midi_bufs);
    note_port->midi_bufs = NULL;
    note_port->ports_count = 0;
    return 0;
}

// create note_ports
static int clap_plug_note_ports_create(CLAP_PLUG_INFO *plug_data, int id,
                                       bool input_ports) {
    if (!plug_data)
        return -1;
    if (id >= MAX_INSTANCES || id < 0)
        return -1;
    CLAP_PLUG_PLUG *plug = &(plug_data->plugins[id]);
    if (plug->plug_inst_created != 1)
        return -1;
    if (!plug->plug_inst)
        return -1;

    const clap_plugin_note_ports_t *clap_plug_note_ports =
        plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_NOTE_PORTS);
    // without the extension the plugin has no note ports
    if (!clap_plug_note_ports)
        return 0;

    uint32_t clap_ports_count =
        clap_plug_note_ports->count(plug->plug_inst, input_ports);
    if (clap_ports_count <= 0)
        return 0;
    // create the graph port pointer and other arrays per note port
    CLAP_PLUG_NOTE_PORT *note_port = &(plug->output_note_ports);
    if (input_ports)
        note_port = &(plug->input_note_ports);

    note_port->graph_ports = calloc(clap_ports_count, sizeof(GRAPH_PORT *));
    note_port->ports_count = clap_ports_count;
    note_port->ids = calloc(clap_ports_count, sizeof(clap_id));
    note_port->supported_dialects = calloc(clap_ports_count, sizeof(uint32_t));
    note_port->preferred_dialects = calloc(clap_ports_count, sizeof(uint32_t));
    note_port->midi_bufs = calloc(clap_ports_count, sizeof(MIDI_BUF *));
    if (!note_port->graph_ports || !note_port->ids ||
        !note_port->supported_dialects || !note_port->preferred_dialects ||
        !note_port->midi_bufs) {
        clap_plug_note_ports_destroy(plug_data, note_port);
        return -1;
    }

    for (uint32_t i = 0; i < clap_ports_count; i++) {
        clap_note_port_info_t note_port_info;
        if (!clap_plug_note_ports->get(plug->plug_inst, i, input_ports,
                                       &note_port_info))
            continue;
        char full_port_name[CLAP_PORT_NAME_MAX];
        if (clap_plug_port_name_create(CLAP_PORT_NAME_MAX, full_port_name,
                                       note_port_info.name, -1) != 0)
            continue;
        unsigned int io_flow = PORT_FLOW_OUTPUT;
        if (input_ports == 1)
            io_flow = PORT_FLOW_INPUT;
        // one that fails stays NULL - its buffer is NULL, nothing in or out
        note_port->graph_ports[i] =
            graph_port_create(plug_data->graph, plug->node, PORT_TYPE_MIDI,
                              io_flow, full_port_name);
        note_port->ids[i] = note_port_info.id;
        note_port->preferred_dialects[i] = note_port_info.preferred_dialect;
        note_port->supported_dialects[i] = note_port_info.supported_dialects;
    }

    return 0;
}

// clear all the parameters on the id plugin, the plugin should not be
// processing
static int clap_plug_params_destroy(CLAP_PLUG_INFO *plug_data, int id) {
    if (!plug_data)
        return -1;
    if (id >= MAX_INSTANCES || id < 0)
        return -1;
    CLAP_PLUG_PLUG *plug = &(plug_data->plugins[id]);
    if (plug->plug_params) {
        param_clean_param_container(plug->plug_params);
        plug->plug_params = NULL;
    }

    return 0;
}

// function that uses the value_to_text function on the param extension. use on
// [main-thread] this function is on param_container and will be used when the
// function param_get_value_as_string is called in params.c
static unsigned int clap_plug_params_value_to_text(const void *user_data,
                                                   int val_id, PARAM_T value,
                                                   char *ret_string,
                                                   uint32_t string_len) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)user_data;
    if (!plug)
        return 0;
    if (!plug->plug_inst)
        return 0;
    const clap_plugin_params_t *clap_params =
        plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
    if (!clap_params)
        return 0;

    const char *param_name = param_get_name(plug->plug_params, val_id);
    if (!param_name || param_name[0] == '\0')
        return 0;
    // this param's real clap_id is stored as owner_id
    uint32_t clap_param_id = param_get_owner_id(plug->plug_params, val_id, 0);
    // TODO have to create a long string first, because Juice wrapper and some
    // CLAP plugins do not respect the string_len given to the value_to_text
    // function
    char long_string[MAX_STRING_MSG_LENGTH];
    unsigned int convert_err = clap_params->value_to_text(
        plug->plug_inst, clap_param_id, (double)value, long_string,
        MAX_STRING_MSG_LENGTH);
    if (convert_err == 1) {
        snprintf(ret_string, string_len, "%s", long_string);
    }
    return convert_err;
}

// clap_param_info_t.module is a "/"-separated tree path (its own doc: e.g.
// "Oscillators/Wavetable 1"). params.c only ever interns
// one segment at a time and has no notion of a path. Returns the leaf
// category's uid, 0 for an empty/absent module.
static uint32_t clap_plug_intern_module(PRM_CONTAIN *params,
                                        const char *module) {
    if (!params || !module)
        return 0;
    uint32_t parent_uid = 0;
    const char *seg = module;
    while (*seg) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (len > 0) {
            char name[MAX_CATEGORY_SEGMENT];
            if (len >= MAX_CATEGORY_SEGMENT)
                len = MAX_CATEGORY_SEGMENT - 1;
            memcpy(name, seg, len);
            name[len] = '\0';
            uint32_t uid = param_category_intern(params, parent_uid, name);
            if (uid == 0)
                return parent_uid; // out of memory - keep what resolved
            parent_uid = uid;
        }
        if (!slash)
            break;
        seg = slash + 1; // an empty segment just advances, so "a//b" == "a/b"
    }
    return parent_uid;
}

// translate the CLAP_PARAM_IS_HIDDEN/READONLY/ENUM bits of a clap_param_
// info_t.flags into this container's own paramFlags
static uint32_t clap_plug_translate_param_flags(uint32_t clap_flags) {
    uint32_t flags = 0;
    if ((clap_flags & CLAP_PARAM_IS_HIDDEN) == CLAP_PARAM_IS_HIDDEN)
        flags |= PARAM_FLAG_HIDDEN;
    if ((clap_flags & CLAP_PARAM_IS_READONLY) == CLAP_PARAM_IS_READONLY)
        flags |= PARAM_FLAG_READONLY;
    if ((clap_flags & CLAP_PARAM_IS_ENUM) == CLAP_PARAM_IS_ENUM)
        flags |= PARAM_FLAG_ENUM;
    return flags;
}

// walks CLAP's current param list and fills out[] with one PARAM_RESYNC_
// ITEM per param (up to max_out). uid is reused via an existing owner_id
// match if present, else freshly minted. Returns entries written.
static uint32_t
clap_plug_discover_params(CLAP_PLUG_PLUG *plug,
                          const clap_plugin_params_t *clap_params,
                          PARAM_RESYNC_ITEM *out, uint32_t max_out) {
    uint32_t param_count = clap_params->count(plug->plug_inst);
    uint32_t written = 0;
    for (uint32_t clap_idx = 0; clap_idx < param_count && written < max_out;
         clap_idx++) {
        clap_param_info_t param_info;
        if (!clap_params->get_info(plug->plug_inst, clap_idx, &param_info))
            continue;

        PARAM_T param_min = param_info.min_value;
        PARAM_T param_max = param_info.max_value;
        // calculate the increment
        PARAM_T param_range = param_max - param_min;
        if (param_range < 0)
            param_range *= -1;
        PARAM_T param_inc = param_range / 100.0;

        if ((param_info.flags & CLAP_PARAM_IS_STEPPED) ==
            CLAP_PARAM_IS_STEPPED) {
            param_inc = 1.0;
        }
        // TODO not sure what to do with periodic parameters
        if ((param_info.flags & CLAP_PARAM_IS_PERIODIC) ==
            CLAP_PARAM_IS_PERIODIC) {
        }
        // TODO not sure what to do with bypass parameter
        if ((param_info.flags & CLAP_PARAM_IS_BYPASS) == CLAP_PARAM_IS_BYPASS) {
        }
        if ((param_info.flags & CLAP_PARAM_IS_READONLY) ==
            CLAP_PARAM_IS_READONLY) {
            param_inc = 0;
        }
        uint32_t param_flags =
            clap_plug_translate_param_flags(param_info.flags);

        int existing_val_id =
            plug->plug_params
                ? param_find_owner_id(plug->plug_params, param_info.id)
                : -1;
        // a survivor keeps the uid it already has; a genuinely new param goes
        // in as 0 and params.c mints one.
        uint32_t uid =
            (existing_val_id != -1)
                ? param_get_uid(plug->plug_params, existing_val_id, 0)
                : 0;

        snprintf(out[written].name, MAX_SHORT_NAME_LENGTH, "%s",
                 param_info.name);
        out[written].val = param_info.default_value;
        out[written].min = param_min;
        out[written].max = param_max;
        out[written].inc = param_inc;
        out[written].uid = uid;
        out[written].owner_id = param_info.id;
        out[written].flags = param_flags;
        // the chain is interned, so every param under "Oscillators/..."
        // resolves to the same "Oscillators" entity
        out[written].category_uid =
            clap_plug_intern_module(plug->plug_params, param_info.module);
        out[written].cookie = param_info.cookie;
        written++;
    }
    return written;
}

// paramFlags bits clap_plug_discover_params can translate - a reconciliation
// merge only ever touches these, so a future non-CLAP-sourced flag bit is
// never silently clobbered.
#define CLAP_PLUG_KNOWN_PARAM_FLAGS                                            \
    (PARAM_FLAG_HIDDEN | PARAM_FLAG_READONLY | PARAM_FLAG_ENUM)

// reconcile one survivor param's category against CLAP's current report for
// it. CLAP lists module among the fields a RESCAN_INFO may change, so a
// plugin can move a param between modules at any time. No-op if val_id is -1
// (not found/alive); param_set_value only bumps the generation when the
// category actually differs.
static void clap_plug_reconcile_param_category(PRM_CONTAIN *plug_params,
                                               int val_id,
                                               uint32_t category_uid) {
    if (val_id == -1)
        return;
    param_set_value(plug_params, val_id, (PARAM_T)category_uid, NULL,
                    Operation_SetCategory);
}

// reconcile one survivor param's flags against CLAP's current report for it
// - masked merge (only CLAP_PLUG_KNOWN_PARAM_FLAGS), only if it actually
// differs. No-op if val_id is -1 (not found/alive).
static void clap_plug_reconcile_param_flags(PRM_CONTAIN *plug_params,
                                            int val_id,
                                            uint32_t discovered_flags) {
    if (val_id == -1)
        return;
    uint32_t cur_flags = param_get_flags(plug_params, val_id);
    uint32_t new_flags = (cur_flags & ~CLAP_PLUG_KNOWN_PARAM_FLAGS) |
                         (discovered_flags & CLAP_PLUG_KNOWN_PARAM_FLAGS);
    if (new_flags != cur_flags)
        param_set_value(plug_params, val_id, (PARAM_T)new_flags, NULL,
                        Operation_SetFlags);
}

// after a discovery/resync pass, what only RESCAN_ALL may change on a survivor:
// range, increment, default and cookie (resync leaves survivors untouched, new
// adds got them from param_add_param; name, flags and category are the INFO
// pass's). The plugin is not activated, so [main-thread] owns the rt side -
// the messages are applied at once, a big param set never fills the queue
static void clap_plug_reconcile_survivors(PRM_CONTAIN *plug_params,
                                          const PARAM_RESYNC_ITEM *items,
                                          uint32_t item_count) {
    for (uint32_t i = 0; i < item_count; i++) {
        int val_id = param_find_uid(plug_params, items[i].uid);
        if (val_id == -1)
            continue;
        // the plugin has its own value, read again after this
        param_set_range(plug_params, val_id, items[i].min, items[i].max, true);
        param_set_value(plug_params, val_id, items[i].inc, NULL,
                        Operation_SetIncr);
        param_set_value(plug_params, val_id, items[i].val, NULL,
                        Operation_SetDefValue);
        param_set_cookie(plug_params, val_id, items[i].cookie);
        param_msgs_process(plug_params, 1);
    }
}

// the param set (re)built from the plugin, while it is not activated: at load
// every param is new, on a RESCAN_ALL survivors keep their uid. -1 on
// allocation failure
static int clap_plug_params_rescan_all(CLAP_PLUG_PLUG *plug) {
    const clap_plugin_params_t *clap_params =
        plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
    // without the extension the plugin has no params
    if (!clap_params)
        return 0;
    uint32_t param_count = clap_params->count(plug->plug_inst);
    // param_count==0 still must run resync (as an empty new_params[])
    // so pass 1 removes every param this plugin no longer has
    PARAM_RESYNC_ITEM *items =
        param_count > 0 ? malloc(param_count * sizeof(PARAM_RESYNC_ITEM))
                        : NULL;
    if (param_count > 0 && !items)
        return -1;
    uint32_t item_count =
        param_count > 0
            ? clap_plug_discover_params(plug, clap_params, items, param_count)
            : 0;
    params_container_resync(plug->plug_params, items, item_count);
    clap_plug_reconcile_survivors(plug->plug_params, items, item_count);
    free(items);
    return 0;
}

// the id plugin's param container, the plugin not activated yet. Every plugin
// gets one, empty without params - a RESCAN_ALL may add some later
static int clap_plug_params_create(CLAP_PLUG_INFO *plug_data, int id) {
    if (!plug_data)
        return -1;
    if (id >= MAX_INSTANCES || id < 0)
        return -1;
    CLAP_PLUG_PLUG *plug = &(plug_data->plugins[id]);
    if (plug->plug_inst_created != 1)
        return -1;
    if (!plug->plug_inst)
        return -1;

    PRM_CONT_USER_DATA container_user_data;
    container_user_data.user_data = (void *)plug;
    container_user_data.build_value = NULL;
    container_user_data.val_to_string = clap_plug_params_value_to_text;
    plug->plug_params = params_init_param_container(&container_user_data);
    if (!plug->plug_params)
        return -1;
    return clap_plug_params_rescan_all(plug);
}

// [main-thread] read every param value from the plugin into the param
// container
static void clap_plug_params_sync_values(CLAP_PLUG_PLUG *plug) {
    if (!plug->plug_params)
        return;
    const clap_plugin_params_t *clap_params =
        plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
    if (!clap_params)
        return;
    uint32_t param_count =
        (uint32_t)param_return_num_params(plug->plug_params, 0);
    for (uint32_t param_num = 0; param_num < param_count; param_num++) {
        // this param's real clap_id is stored as owner_id
        const char *param_name =
            param_get_name(plug->plug_params, (int)param_num);
        if (!param_name || param_name[0] == '\0')
            continue;
        uint32_t clap_param_id =
            param_get_owner_id(plug->plug_params, (int)param_num, 0);
        double cur_value = 0;
        if (!clap_params->get_value(plug->plug_inst, clap_param_id,
                                    &cur_value))
            continue;
        // the plugin already has this value - do not send it back
        param_set_value(plug->plug_params, param_num, (PARAM_T)cur_value,
                        NULL, Operation_SyncValue);
    }
}


static void clap_plug_ext_params_rescan(const clap_host_t *host,
                                        clap_param_rescan_flags flags) {
    if (!is_main_thread)
        return;
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return;
    if (!plug->plug_params)
        return;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    if (!plug_data)
        return;
    // ALL first, the passes below then read the rebuilt set. It invalidates
    // everything the host knows, so values, texts and info are read again too
    if ((flags & CLAP_PARAM_RESCAN_ALL) == CLAP_PARAM_RESCAN_ALL &&
        plug->plug_inst_activated == 0) {
        clap_plug_params_rescan_all(plug);
        flags |= CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT |
                 CLAP_PARAM_RESCAN_INFO;
    }
    if ((flags & CLAP_PARAM_RESCAN_VALUES) == CLAP_PARAM_RESCAN_VALUES)
        clap_plug_params_sync_values(plug);
    if ((flags & CLAP_PARAM_RESCAN_TEXT) == CLAP_PARAM_RESCAN_TEXT) {
        // the value texts are read from the plugin when drawn, so only tell the
        // ui side that every param may now read differently
        param_mark_changed(plug->plug_params, -1);
    }
    if ((flags & CLAP_PARAM_RESCAN_INFO) == CLAP_PARAM_RESCAN_INFO) {
        // go through the params and change the names, flags, categories
        const clap_plugin_params_t *clap_params =
            plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
        if (!clap_params)
            return;
        // get_info only takes CLAP's OWN live param_index - this
        // container's val_id only equals that index up until the FIRST
        // CLAP_PARAM_RESCAN_ALL this plugin instance ever goes through
        uint32_t clap_count = clap_params->count(plug->plug_inst);
        for (uint32_t clap_idx = 0; clap_idx < clap_count; clap_idx++) {
            clap_param_info_t param_info;
            if (!clap_params->get_info(plug->plug_inst, clap_idx, &param_info))
                continue;
            int val_id = param_find_owner_id(plug->plug_params, param_info.id);
            if (val_id == -1)
                continue;
            param_set_value(plug->plug_params, val_id, 0.0, param_info.name,
                            Operation_ChangeName);
            clap_plug_reconcile_param_flags(
                plug->plug_params, val_id,
                clap_plug_translate_param_flags(param_info.flags));
            clap_plug_reconcile_param_category(
                plug->plug_params, val_id,
                clap_plug_intern_module(plug->plug_params, param_info.module));
        }
    }
}

// clear param of automation and modulation, since it is not implemented yet,
// does nothing
static void clap_plug_ext_params_clear(const clap_host_t *host,
                                       clap_id param_id,
                                       clap_param_clear_flags flags) {
    (void)host;
    (void)param_id;
    (void)flags;
    return;
}

// [thread-safe, !audio-thread]
static void clap_plug_ext_params_request_flush(const clap_host_t *host) {
    if (is_audio_thread)
        return;
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return;
    atomic_fetch_or(&plug->requests, CLAP_PLUG_REQ_FLUSH);
}

// host extension function for audio_ports - return true if a rescan with the
// flag is supported by this host
static bool
clap_plug_ext_audio_ports_is_rescan_flag_supported(const clap_host_t *host,
                                                   uint32_t flag) {
    // this function is only usable on the [main-thread]
    if (!is_main_thread)
        return false;
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return false;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    if (!plug_data)
        return false;
    if ((flag & CLAP_AUDIO_PORTS_RESCAN_NAMES) != 0)
        return true;
    if ((flag & CLAP_AUDIO_PORTS_RESCAN_FLAGS) != 0)
        return true;
    if ((flag & CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT) != 0)
        return true;
    if ((flag & CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE) != 0)
        return true;
    if ((flag & CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR) != 0)
        return true;
    if ((flag & CLAP_AUDIO_PORTS_RESCAN_LIST) != 0)
        return true;
    return false;
}

// host extension function for audio_ports - rescan ports and get what is
// changed (in essence create the ports again)
static void clap_plug_ext_audio_ports_rescan(const clap_host_t *host,
                                             uint32_t flags) {
    // this function is only usable on the [main-thread]
    if (!is_main_thread)
        return;
    if (flags == 0)
        return;
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    if (!plug_data)
        return;
    if (!plug->plug_inst)
        return;
    // If the names of the ports changed, but nothing else did, rename the
    // ports, this can be done with an activated plugin
    if ((flags ^ CLAP_AUDIO_PORTS_RESCAN_NAMES) == 0) {
        clap_plug_ports_rename(plug_data, plug, &(plug->input_ports), 1);
        clap_plug_ports_rename(plug_data, plug, &(plug->output_ports), 0);
        return;
    }
    // other flags only usable on an !active plugin instance
    if (plug->plug_inst_activated)
        return;
    // for other flags destroy and create the ports again
    clap_plug_destroy_ports(plug_data, &(plug->output_ports));
    clap_plug_destroy_ports(plug_data, &(plug->input_ports));

    clap_plug_create_ports(plug_data, plug->id, &(plug->output_ports), 0);
    clap_plug_create_ports(plug_data, plug->id, &(plug->input_ports), 1);
}

// for note-ports extension return what note dialects are supported by this host
static uint32_t
clap_plug_ext_note_ports_supported_dialects(const clap_host_t *host) {
    (void)host;
    if (!is_main_thread)
        return 0;
    uint32_t supported = 0;
    supported = (supported | CLAP_NOTE_DIALECT_CLAP);
    supported = (supported | CLAP_NOTE_DIALECT_MIDI);
    // right now MIDI2 and MPE are not supported
    return supported;
}

// for note-ports extension rescan the note ports and get what is changed
// (create the ports again)
static void clap_plug_ext_note_ports_rescan(const clap_host_t *host,
                                            uint32_t flags) {
    if (!is_main_thread)
        return;
    if (flags == 0)
        return;
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    if (!plug_data)
        return;
    if (!plug->plug_inst)
        return;
    // if only the names of the ports changed (and nothing else) rename the
    // ports - can be done with active plugin
    if ((flags ^ CLAP_NOTE_PORTS_RESCAN_NAMES) == 0) {
        clap_plug_note_ports_rename(plug_data, plug, 0);
        clap_plug_note_ports_rename(plug_data, plug, 1);
        return;
    }
    // other flags need deactivated plugin instance
    if (plug->plug_inst_activated)
        return;
    // destroy and create the note ports again
    if ((flags & CLAP_NOTE_PORTS_RESCAN_ALL) == CLAP_NOTE_PORTS_RESCAN_ALL) {
        clap_plug_note_ports_destroy(plug_data, &(plug->input_note_ports));
        clap_plug_note_ports_destroy(plug_data, &(plug->output_note_ports));
        clap_plug_note_ports_create(plug_data, plug->id, 0);
        clap_plug_note_ports_create(plug_data, plug->id, 1);
    }
}

static uint32_t
clap_plug_ext_events_size(const struct clap_input_events *list) {
    if (!list)
        return 0;
    UB_EVENT *ub_ev = (UB_EVENT *)list->ctx;
    if (!ub_ev)
        return 0;
    return ub_size(ub_ev);
}

static const clap_event_header_t *
clap_plug_ext_events_get(const struct clap_input_events *list, uint32_t index) {
    if (!list)
        return NULL;
    UB_EVENT *ub_ev = (UB_EVENT *)list->ctx;
    if (!ub_ev)
        return NULL;
    clap_event_header_t *header =
        (clap_event_header_t *)ub_item_get(ub_ev, index);
    return header;
}

// a CLAP velocity (0..1) as a MIDI one, at least min
static uint8_t clap_velocity_to_midi(double velocity, uint8_t min) {
    long vel = lround(fmin(fmax(velocity, 0.0), 1.0) * 127.0);
    return (uint8_t)(vel < min ? min : vel);
}

// push a message to output note port port, -1 = every port
static bool clap_output_midi_push(CLAP_PLUG_PLUG *plug, int32_t port,
                                  uint32_t frame, const uint8_t *data,
                                  uint32_t size) {
    CLAP_PLUG_NOTE_PORT *note_ports = &(plug->output_note_ports);
    if (!note_ports->midi_bufs || port >= (int32_t)note_ports->ports_count) {
        atomic_fetch_add(&plug->out_events_dropped, 1U);
        return false;
    }
    if (port >= 0)
        return midi_buf_push(note_ports->midi_bufs[port], frame, data, size);
    bool pushed = true;
    for (uint32_t i = 0; i < note_ports->ports_count; i++)
        pushed &= midi_buf_push(note_ports->midi_bufs[i], frame, data, size);
    return pushed;
}

// NOTE_ON, NOTE_OFF or NOTE_CHOKE as MIDI. Off and choke take wildcards: every
// port, all 16 channels, all notes - CC 123 All Notes Off, for a choke CC 120
// All Sound Off
static bool clap_output_note_push(CLAP_PLUG_PLUG *plug,
                                  const clap_event_note_t *note) {
    uint32_t frame = note->header.time;
    bool on = note->header.type == CLAP_EVENT_NOTE_ON;
    bool choke = note->header.type == CLAP_EVENT_NOTE_CHOKE;
    int16_t min = on ? 0 : -1;
    if (note->port_index < min || note->channel < min || note->channel > 15 ||
        note->key < min || note->key > 127) {
        atomic_fetch_add(&plug->out_events_dropped, 1U);
        return false;
    }
    if (on) {
        uint8_t msg[3] = {(uint8_t)(MIDI_NOTE_ON | note->channel),
                          (uint8_t)note->key,
                          clap_velocity_to_midi(note->velocity, 1)};
        return clap_output_midi_push(plug, note->port_index, frame, msg, 3);
    }
    uint8_t first = note->channel < 0 ? 0 : (uint8_t)note->channel;
    uint8_t last = note->channel < 0 ? 15 : (uint8_t)note->channel;
    bool pushed = true;
    for (uint8_t ch = first; ch <= last; ch++) {
        uint8_t msg[3] = {(uint8_t)(MIDI_NOTE_OFF | ch), (uint8_t)note->key,
                          choke ? MIDI_RELEASE_VELOCITY_DEFAULT
                                : clap_velocity_to_midi(note->velocity, 0)};
        if (note->key < 0) {
            msg[0] = (uint8_t)(MIDI_CC | ch);
            msg[1] = choke ? 120 : 123;
            msg[2] = 0;
        }
        pushed &= clap_output_midi_push(plug, note->port_index, frame, msg, 3);
    }
    return pushed;
}

// a note or MIDI event to its output note port's MIDI_BUF, false if it is not
// one. *pushed false when it was dropped
static bool clap_output_events_note_add(CLAP_PLUG_PLUG *plug,
                                        const clap_event_header_t *event,
                                        bool *pushed) {
    *pushed = true;
    if (event->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return false;
    switch (event->type) {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE:
    case CLAP_EVENT_MIDI:
    case CLAP_EVENT_MIDI_SYSEX:
        break;
    case CLAP_EVENT_NOTE_END:
    case CLAP_EVENT_NOTE_EXPRESSION:
    case CLAP_EVENT_MIDI2:
        // no use for them yet
        return true;
    default:
        return false;
    }
    // a params flush on [main-thread] has no cycle to send them in
    if (!is_audio_thread)
        return true;
    uint32_t min_size = sizeof(clap_event_note_t);
    if (event->type == CLAP_EVENT_MIDI)
        min_size = sizeof(clap_event_midi_t);
    if (event->type == CLAP_EVENT_MIDI_SYSEX)
        min_size = sizeof(clap_event_midi_sysex_t);
    if (event->size < min_size) {
        atomic_fetch_add(&plug->out_events_dropped, 1U);
        *pushed = false;
        return true;
    }
    if (event->type == CLAP_EVENT_MIDI) {
        const clap_event_midi_t *midi = (const clap_event_midi_t *)event;
        // 0 for sysex or a data byte, the MIDI_BUF drops those
        uint32_t size = midi_expected_size(midi->data[0]);
        *pushed = clap_output_midi_push(plug, midi->port_index, event->time,
                                        midi->data, size);
    } else if (event->type == CLAP_EVENT_MIDI_SYSEX) {
        // the buffer is valid only during try_push, the MIDI_BUF copies it
        const clap_event_midi_sysex_t *sysex =
            (const clap_event_midi_sysex_t *)event;
        *pushed = clap_output_midi_push(plug, sysex->port_index, event->time,
                                        sysex->buffer, sysex->size);
    } else {
        *pushed = clap_output_note_push(plug, (const clap_event_note_t *)event);
    }
    return true;
}

static bool clap_plug_ext_events_try_push(const struct clap_output_events *list,
                                          const clap_event_header_t *event) {
    if (!list || !event)
        return false;
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)list->ctx;
    if (!plug || !plug->out_list)
        return false;
    bool pushed = true;
    if (clap_output_events_note_add(plug, event, &pushed))
        return pushed;
    if (ub_push(plug->out_list, event, event->size) == 0)
        return true;
    atomic_fetch_add(&plug->out_events_dropped, 1U);
    // the ui side would miss the value
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID &&
        event->type == CLAP_EVENT_PARAM_VALUE)
        atomic_fetch_or(&plug->requests, CLAP_PLUG_REQ_SYNC_VALUES);
    return false;
}

// a from_location() call failed, [main-thread]
static void clap_ext_preset_load_on_error(const clap_host_t *host,
                                          uint32_t location_kind,
                                          const char *location,
                                          const char *load_key,
                                          int32_t os_error, const char *msg) {
    (void)host;
    (void)location_kind;
    (void)os_error;
    log_append_logfile("preset %s %s did not load: %s\n",
                       location ? location : "(in plugin)",
                       load_key ? load_key : "", msg ? msg : "");
}

static void clap_ext_preset_load_on_load(const clap_host_t *host,
                                         uint32_t location_kind,
                                         const char *location,
                                         const char *load_key) {
    (void)host;
    (void)location_kind;
    (void)location;
    (void)load_key;
    return;
}

// deactivate the plugin if it is activated. The flag goes first - a plugin
// may rescan(CLAP_PARAM_RESCAN_ALL) from inside deactivate()
static void clap_plug_deactivate(CLAP_PLUG_PLUG *plug) {
    if (plug->plug_inst_activated != 1)
        return;
    plug->plug_inst_activated = 0;
    plug->plug_inst->deactivate(plug->plug_inst);
}

// clean the single plugin struct
// before calling this the plug_inst_processing should be == 0
static int clap_plug_plug_clean(CLAP_PLUG_INFO *plug_data, int plug_id) {
    if (!plug_data)
        return -1;
    if (plug_id < 0)
        return -1;
    if (plug_id >= MAX_INSTANCES)
        return -1;
    CLAP_PLUG_PLUG *plug = &(plug_data->plugins[plug_id]);

    if (plug->plug_inst) {
        clap_plug_deactivate(plug);
        if (plug->plug_inst_created == 1) {
            plug->plug_inst->destroy(plug->plug_inst);
            plug->plug_inst_created = 0;
        }
        plug->plug_inst = NULL;
    }
    // requests and drops left by the destroyed instance must not reach the
    // next plugin loaded into this slot
    atomic_store(&plug->requests, 0U);
    atomic_store(&plug->in_events_dropped, 0U);
    atomic_store(&plug->out_events_dropped, 0U);
    if (plug->plug_entry) {
        if (clap_plug_return_plug_id_with_same_plug_entry(plug_data, plug) ==
            -1) {
            plug->plug_entry->deinit();
            dlclose(plug->dso_handle);
        }
        plug->plug_entry = NULL;
        plug->dso_handle = NULL;
    }
    plug->plug_path[0] = '\0';

    // clean the audio ports
    clap_plug_destroy_ports(plug_data, &(plug->output_ports));
    clap_plug_destroy_ports(plug_data, &(plug->input_ports));
    // clean the note ports
    clap_plug_note_ports_destroy(plug_data, &(plug->input_note_ports));
    clap_plug_note_ports_destroy(plug_data, &(plug->output_note_ports));
    graph_node_remove(plug_data->graph, plug->node);
    plug->node = NULL;
    // clean the event structs
    clap_input_events_t *in_events = &(plug->input_events);
    ub_clean((UB_EVENT *)in_events->ctx);
    in_events->ctx = NULL;
    ub_clean(plug->out_list);
    plug->out_list = NULL;
    plug->output_events.ctx = NULL;
    // clean the parameters
    clap_plug_params_destroy(plug_data, plug->id);
    // remove presets
    tree_index_free(plug->presets);
    plug->presets = NULL;
    plug->presets_built = false;

    plug->plug_inst_id = -1;
    plug_data->plugins_dirty = true;

    return 0;
}

int clap_plug_plug_stop_and_clean(void *plug) {
    CLAP_PLUG_PLUG *cur_plug = (CLAP_PLUG_PLUG *)plug;
    if (!cur_plug)
        return -1;
    if (!cur_plug->plug_data)
        return -1;
    // stop this plugin process if its processing before cleaning.
    context_sub_wait_for_stop(cur_plug->plug_data->control_data,
                              (void *)cur_plug);
    return clap_plug_plug_clean(cur_plug->plug_data, cur_plug->id);
}

// return if this is audio_thread or not
static bool clap_plug_return_is_audio_thread() { return is_audio_thread; }

// extension functions for thread-check.h to return is this audio or main thread
// for the clap_host_thread_t extension
static bool clap_plug_ext_is_audio_thread(const clap_host_t *host) {
    (void)host;
    return clap_plug_return_is_audio_thread();
}

static bool clap_plug_ext_is_main_thread(const clap_host_t *host) {
    (void)host;
    return is_main_thread;
}

static int clap_sys_msg(void *user_data, const char *msg) {
    (void)user_data;
    // if(!plug_data)return -1;
    log_append_logfile("%s", msg);
    return 0;
}

// extension function for log.h to send messages in [thread-safe] manner
static void clap_plug_ext_log(const clap_host_t *host,
                              clap_log_severity severity, const char *msg) {
    (void)severity;
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    if (!plug_data)
        return;
    // severity is not sent, right now dont see the need to send severity - this
    // should be obvious from the message
    context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                         clap_plug_return_is_audio_thread(), msg);
}

static const void *clap_plug_get_extension(const clap_host_t *host,
                                           const char *ex_id) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return NULL;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    if (!plug_data)
        return NULL;
    if (strcmp(ex_id, CLAP_EXT_THREAD_CHECK) == 0) {
        return &(plug_data->ext_thread_check);
    }
    if (strcmp(ex_id, CLAP_EXT_LOG) == 0) {
        return &(plug_data->ext_log);
    }
    if (strcmp(ex_id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &(plug_data->ext_audio_ports);
    }
    if (strcmp(ex_id, CLAP_EXT_NOTE_PORTS) == 0) {
        return &(plug_data->ext_note_ports);
    }
    if (strcmp(ex_id, CLAP_EXT_PARAMS) == 0) {
        return &(plug_data->ext_params);
    }
    if (strcmp(ex_id, CLAP_EXT_PRESET_LOAD) == 0 ||
        strcmp(ex_id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0) {
        return &(plug_data->ext_preset_load);
    }
    // if there is no extension implemented that the plugin needs send the name
    // of the extension to the ui
    context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                         clap_plug_return_is_audio_thread(),
                         "%s asked for ext %s\n", plug->plug_path, ex_id);

    return NULL;
}

static int clap_plug_activate_start_processing(void *user_data) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)user_data;
    if (!plug)
        return -1;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    if (!plug_data)
        return -1;
    // since there was a request to start processing the plugin, it should be
    // stopped, but just in case, send a request to stop it
    context_sub_wait_for_stop(plug_data->control_data, (void *)plug);

    if (plug->plug_inst_activated == 0) {
        if (!plug->plug_inst)
            return -1;
        // the channel buffers hold what activate promises
        if (clap_plug_audio_buffers_fit(&(plug->input_ports),
                                        plug_data->max_buffer_size) != 0 ||
            clap_plug_audio_buffers_fit(&(plug->output_ports),
                                        plug_data->max_buffer_size) != 0)
            return -1;
        if (!plug->plug_inst->activate(plug->plug_inst, plug_data->sample_rate,
                                       plug_data->min_buffer_size,
                                       plug_data->max_buffer_size)) {
            return -1;
        }
    }
    plug->plug_inst_activated = 1;
    // its first cycle already runs a plan with its ports
    graph_plan_update(plug_data->graph);
    // send message to the audio thread that the plugin can be started to
    // process and wait for it to start
    context_sub_wait_for_start(plug_data->control_data, (void *)plug);

    return 0;
}

static int clap_plug_restart(void *user_data) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)user_data;
    if (!plug)
        return -1;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    if (!plug_data)
        return -1;
    // send a message to rt thread to stop the plugin process and block while
    // its stopping
    context_sub_wait_for_stop(plug_data->control_data, (void *)plug);

    clap_plug_deactivate(plug);
    // now when the plugin was deactivated simply call the activate and process
    // function, that will reactivate the plugin and send a message to
    // [audio-thread] to start processing it
    return clap_plug_activate_start_processing((void *)plug);
}

int clap_plug_audio_config_set(CLAP_PLUG_INFO *plug_data, SAMPLE_T sample_rate,
                               uint32_t min_buffer_size,
                               uint32_t max_buffer_size) {
    if (!plug_data)
        return -1;
    if (sample_rate == plug_data->sample_rate &&
        min_buffer_size == plug_data->min_buffer_size &&
        max_buffer_size == plug_data->max_buffer_size)
        return 0;
    // [audio-thread] reads the sizes for a processing plugin, so every plugin
    // stops before they change
    bool was_activated[MAX_INSTANCES] = {false};
    for (int i = 0; i < MAX_INSTANCES; i++) {
        CLAP_PLUG_PLUG *plug = &(plug_data->plugins[i]);
        if (!plug->plug_inst)
            continue;
        context_sub_wait_for_stop(plug_data->control_data, (void *)plug);
        was_activated[i] = plug->plug_inst_activated == 1;
        clap_plug_deactivate(plug);
    }
    plug_data->sample_rate = sample_rate;
    plug_data->min_buffer_size = min_buffer_size;
    plug_data->max_buffer_size = max_buffer_size;
    // the others take the values when they activate
    int result = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        CLAP_PLUG_PLUG *plug = &(plug_data->plugins[i]);
        if (!was_activated[i] ||
            clap_plug_activate_start_processing((void *)plug) == 0)
            continue;
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "%s did not activate again\n", plug->name);
        result = -1;
    }
    return result;
}

// [thread-safe] clap_host_t requests - only mark them, clap_plug_requests_process
// does the work on [main-thread]
static void clap_plug_request_restart(const clap_host_t *host) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return;
    atomic_fetch_or(&plug->requests, CLAP_PLUG_REQ_RESTART);
}

static void clap_plug_request_process(const clap_host_t *host) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return;
    atomic_fetch_or(&plug->requests, CLAP_PLUG_REQ_PROCESS);
}

static void clap_plug_request_callback(const clap_host_t *host) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)host->host_data;
    if (!plug)
        return;
    atomic_fetch_or(&plug->requests, CLAP_PLUG_REQ_CALLBACK);
}

// a full list counts the event as dropped
static void clap_input_events_push(CLAP_PLUG_PLUG *plug, UB_EVENT *ub_in,
                                   const void *event, uint32_t size) {
    if (ub_push(ub_in, event, size) != 0)
        atomic_fetch_add(&plug->in_events_dropped, 1U);
}

// push the changed rt params to ub_in as param value events, return 1 if any
// were pushed. Use on the thread that owns the rt params - [audio-thread], or
// [main-thread] for a plugin that is not activated
static int clap_input_events_params_add(CLAP_PLUG_PLUG *plug,
                                        UB_EVENT *ub_in) {
    int not_quiet = 0;
    // TODO right now smp_groovebox does not handle automation or modulation so
    // simply put the parameter changes into the event buffer first on the 0
    // offset frame put parameter changes into the event queue
    uint32_t param_count = param_return_num_params(plug->plug_params, 1);
    for (uint32_t param_idx = 0; param_idx < param_count; param_idx++) {
        if (param_get_if_changed_rt(plug->plug_params, (int)param_idx) != 1)
            continue;
        if (not_quiet == 0)
            not_quiet = 1;
        clap_event_header_t head;
        head.flags = CLAP_EVENT_IS_LIVE;
        head.size = sizeof(clap_event_param_value_t);
        head.space_id = CLAP_CORE_EVENT_SPACE_ID;
        head.time = 0;
        head.type = CLAP_EVENT_PARAM_VALUE;

        clap_event_param_value_t param_val;
        param_val.channel = -1;
        param_val.cookie =
            param_cookie_return_rt(plug->plug_params, (int)param_idx);
        param_val.header = head;
        param_val.key = -1;
        param_val.note_id = -1;
        param_val.param_id =
            param_get_owner_id(plug->plug_params, (int)param_idx, 1);
        param_val.port_index = -1;
        param_val.value =
            (double)param_get_value(plug->plug_params, (int)param_idx, 1);
        clap_input_events_push(plug, ub_in, &param_val,
                               (uint32_t)sizeof(clap_event_param_value_t));
    }
    return not_quiet;
}

// apply the param value events the plugin pushed to ub_out. Use on the thread
// that owns the rt params - [audio-thread], or [main-thread] for a plugin that
// is not activated. Returns 1 if ub_out has any events, 0 if none
static int clap_output_events_params_apply(CLAP_PLUG_PLUG *plug,
                                           UB_EVENT *ub_out) {
    uint32_t events_count = ub_size(ub_out);
    if (!plug->plug_params)
        return events_count > 0 ? 1 : 0;
    for (uint32_t i = 0; i < events_count; i++) {
        const clap_event_header_t *head =
            (const clap_event_header_t *)ub_item_get(ub_out, i);
        if (!head || head->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        // TODO gesture begin/end are not used yet (automation recording)
        if (head->type != CLAP_EVENT_PARAM_VALUE ||
            head->size < sizeof(clap_event_param_value_t))
            continue;
        const clap_event_param_value_t *param_ev =
            (const clap_event_param_value_t *)head;
        int val_id = param_find_owner_id(plug->plug_params, param_ev->param_id);
        if (val_id == -1)
            continue;
        // the ui side missed this value (full queue) - read all the values
        // from the plugin again on [main-thread]
        if (param_set_value_rt(plug->plug_params, val_id,
                               (PARAM_T)param_ev->value) != 0)
            atomic_fetch_or(&plug->requests, CLAP_PLUG_REQ_SYNC_VALUES);
    }
    return events_count > 0 ? 1 : 0;
}

// [main-thread] params.flush() for a plugin that is not activated, since it
// gets no process() calls. [audio-thread] skips a plugin that is not
// processing, so its rt params and event lists are free to use here.
// forced == false flushes only if the host changed any params
static void clap_plug_params_flush_inactive(CLAP_PLUG_PLUG *plug,
                                            bool forced) {
    if (!plug->plug_params)
        return;
    UB_EVENT *ub_in = (UB_EVENT *)plug->input_events.ctx;
    UB_EVENT *ub_out = plug->out_list;
    if (!ub_in || !ub_out)
        return;
    // take the param changes the [audio-thread] would have picked up
    param_msgs_process(plug->plug_params, 1);
    if (!forced && param_get_if_any_changed_rt(plug->plug_params) != 1)
        return;
    const clap_plugin_params_t *clap_params =
        plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
    if (!clap_params || !clap_params->flush)
        return;
    ub_list_reset(ub_in);
    clap_input_events_params_add(plug, ub_in);
    ub_list_reset(ub_out);
    clap_params->flush(plug->plug_inst, &(plug->input_events),
                       &(plug->output_events));
    clap_output_events_params_apply(plug, ub_out);
}

// handle the plugin requests on [main-thread]. Deferred to here even when
// requested on [main-thread], so the host never acts while still inside the
// plugin call that made the request
static void clap_plug_requests_process(CLAP_PLUG_PLUG *plug) {
    unsigned int reqs = atomic_exchange(&plug->requests, 0U);
    if (reqs == 0)
        return;
    if ((reqs & CLAP_PLUG_REQ_CALLBACK) != 0)
        plug->plug_inst->on_main_thread(plug->plug_inst);
    // restart ends by starting the process, which covers process and flush
    if ((reqs & CLAP_PLUG_REQ_RESTART) != 0)
        clap_plug_restart((void *)plug);
    else if ((reqs & CLAP_PLUG_REQ_PROCESS) != 0)
        clap_plug_activate_start_processing((void *)plug);
    // process() flushes the params, a no-op if already processing
    else if ((reqs & CLAP_PLUG_REQ_FLUSH) != 0 &&
             plug->plug_inst_activated == 1)
        context_sub_wait_for_start(plug->plug_data->control_data, (void *)plug);
    // not activated (also when restart or process failed to activate) - no
    // process() calls, so flush() here
    if ((reqs & CLAP_PLUG_REQ_FLUSH) != 0 && plug->plug_inst_activated == 0)
        clap_plug_params_flush_inactive(plug, true);
    // after the queued (older) values were applied by the caller
    if ((reqs & CLAP_PLUG_REQ_SYNC_VALUES) != 0)
        clap_plug_params_sync_values(plug);
}

static int clap_plug_start_process(void *user_data) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)user_data;
    if (!plug)
        return -1;

    // if plugin is already processing do nothing
    if (plug->plug_inst_processing == 1) {
        return 0;
    }

    if (!plug->plug_inst) {
        return -1;
    }
    if (!(plug->plug_inst->start_processing(plug->plug_inst))) {
        return -1;
    }

    plug->plug_inst_processing = 1;
    return 0;
}

// [audio-thread] point midi_bufs at the graph ports' buffers this cycle
static void clap_note_ports_bufs_rt(CLAP_PLUG_NOTE_PORT *note_ports) {
    for (uint32_t i = 0; note_ports->midi_bufs && i < note_ports->ports_count;
         i++)
        note_ports->midi_bufs[i] =
            graph_port_midi_rt(note_ports->graph_ports[i]);
}

static int clap_plug_stop_process(void *user_data) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)user_data;
    if (!plug)
        return -1;

    // if plugin is sleeping stop it completely, since when it is sleeping
    // [audio-thread] can still access some parts of the CLAP_PLUG_PLUG struct
    if (plug->plug_inst_processing == 2) {
        plug->plug_inst_processing = 0;
        return 0;
    }
    // if plugin is already stopped and not processing do nothing
    if (plug->plug_inst_processing != 1) {
        return 0;
    }
    if (!plug->plug_inst) {
        return -1;
    }
    plug->plug_inst->stop_processing(plug->plug_inst);

    plug->plug_inst_processing = 0;
    return 0;
}

int clap_read_ui_to_rt_messages(CLAP_PLUG_INFO *plug_data) {
    // this is a local thread var its false on [main-thread] and true on
    // [audio-thread]
    is_audio_thread = true;

    if (!plug_data)
        return -1;
    // process the sys messages (stop, start plugin and similar)
    context_sub_process_rt(plug_data->control_data);

    // read the param messages for plugins on the [audio-thread]
    for (unsigned int i = 0; i < MAX_INSTANCES; i++) {
        CLAP_PLUG_PLUG *cur_plug = &(plug_data->plugins[i]);
        if (cur_plug->plug_inst_processing == 0)
            continue;
        if (!cur_plug->plug_inst)
            continue;
        param_msgs_process(cur_plug->plug_params, 1);
    }
    return 0;
}

int clap_read_rt_to_ui_messages(CLAP_PLUG_INFO *plug_data) {
    if (!plug_data)
        return -1;
    // process the sys messages on [main-thread] (messages from [audio-thread])
    context_sub_process_ui(plug_data->control_data);
    // read the param messages and handle the plugin requests on the
    // [main-thread]
    for (unsigned int i = 0; i < MAX_INSTANCES; i++) {
        CLAP_PLUG_PLUG *cur_plug = &(plug_data->plugins[i]);
        if (!cur_plug->plug_inst)
            continue;
        param_msgs_process(cur_plug->plug_params, 0);
        unsigned int in_dropped =
            atomic_exchange(&cur_plug->in_events_dropped, 0U);
        if (in_dropped > 0)
            log_append_logfile("%s: %u input events dropped\n", cur_plug->name,
                               in_dropped);
        unsigned int out_dropped =
            atomic_exchange(&cur_plug->out_events_dropped, 0U);
        if (out_dropped > 0)
            log_append_logfile("%s: %u output events dropped\n",
                               cur_plug->name, out_dropped);
        clap_plug_requests_process(cur_plug);
        // host side flush - send the param changes to a plugin that is not
        // activated, since it gets no process() calls
        if (cur_plug->plug_inst_activated == 0)
            clap_plug_params_flush_inactive(cur_plug, false);
    }
    return 0;
}

CLAP_PLUG_INFO *clap_plug_init(uint32_t min_buffer_size,
                               uint32_t max_buffer_size, SAMPLE_T samplerate,
                               clap_plug_status_t *plug_error, GRAPH *graph,
                               uint64_t owner_tag) {
    if (!graph)
        return NULL;
    // clap_plug_init is called on [main-thread]
    is_main_thread = true;
    CLAP_PLUG_INFO *plug_data =
        (CLAP_PLUG_INFO *)malloc(sizeof(CLAP_PLUG_INFO));
    if (!plug_data) {
        *plug_error = clap_plug_failed_malloc;
        return NULL;
    }
    memset(plug_data, '\0', sizeof(*plug_data));
    plug_data->graph = graph;
    plug_data->owner_tag = owner_tag;
    plug_data->clap_plugin_list.plugin_list = NULL;
    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = clap_plug_start_process;
    rt_funcs_struct.subcx_stop_process = clap_plug_stop_process;
    ui_funcs_struct.send_msg = clap_sys_msg;
    plug_data->control_data =
        context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if (!plug_data->control_data) {
        free(plug_data);
        *plug_error = clap_plug_failed_malloc;
        return NULL;
    }
    plug_data->min_buffer_size = min_buffer_size;
    plug_data->max_buffer_size = max_buffer_size;
    plug_data->sample_rate = samplerate;

    clap_host_t clap_info_host;
    clap_version_t clap_v;
    clap_v.major = CLAP_VERSION_MAJOR;
    clap_v.minor = CLAP_VERSION_MINOR;
    clap_v.revision = CLAP_VERSION_REVISION;
    clap_info_host.clap_version = clap_v;
    clap_info_host.host_data = NULL;
    clap_info_host.name = "smp_groovebox";
    clap_info_host.vendor = "bru";
    clap_info_host.url = "https://brumakes.com";
    clap_info_host.version = "0.2";
    clap_info_host.get_extension = clap_plug_get_extension;
    clap_info_host.request_restart = clap_plug_request_restart;
    clap_info_host.request_process = clap_plug_request_process;
    clap_info_host.request_callback = clap_plug_request_callback;

    plug_data->clap_host_info = clap_info_host;

    // initiate the host clap extension structs
    // thread check extension
    plug_data->ext_thread_check.is_audio_thread = clap_plug_ext_is_audio_thread;
    plug_data->ext_thread_check.is_main_thread = clap_plug_ext_is_main_thread;
    // log extension
    plug_data->ext_log.log = clap_plug_ext_log;
    // audio-ports extension
    plug_data->ext_audio_ports.is_rescan_flag_supported =
        clap_plug_ext_audio_ports_is_rescan_flag_supported;
    plug_data->ext_audio_ports.rescan = clap_plug_ext_audio_ports_rescan;
    // note-ports extension
    plug_data->ext_note_ports.supported_dialects =
        clap_plug_ext_note_ports_supported_dialects;
    plug_data->ext_note_ports.rescan = clap_plug_ext_note_ports_rescan;
    // param extension
    plug_data->ext_params.clear = clap_plug_ext_params_clear;
    plug_data->ext_params.request_flush = clap_plug_ext_params_request_flush;
    plug_data->ext_params.rescan = clap_plug_ext_params_rescan;
    // preset-load extension
    plug_data->ext_preset_load.loaded = clap_ext_preset_load_on_load;
    plug_data->ext_preset_load.on_error = clap_ext_preset_load_on_error;

    // init the plugins array
    plug_data->plugins_dirty = false;
    plug_data->next_plug_uid = 0;
    for (int i = 0; i < (MAX_INSTANCES); i++) {
        CLAP_PLUG_PLUG *plug = &(plug_data->plugins[i]);
        plug->clap_host_info = clap_info_host;
        plug->id = i;
        plug->plug_data = plug_data;
        plug->plug_entry = NULL;
        plug->dso_handle = NULL;
        plug->plug_inst = NULL;
        plug->plug_inst_activated = 0;
        plug->plug_inst_created = 0;
        plug->plug_inst_id = -1;
        plug->plug_inst_processing = 0;
        plug->plug_params = NULL;
        plug->presets = NULL;
        plug->presets_built = false;
        atomic_init(&plug->requests, 0U);
        atomic_init(&plug->in_events_dropped, 0U);
        atomic_init(&plug->out_events_dropped, 0U);
        plug->out_list = NULL;
    }

    return plug_data;
}

// opens plug->plug_path and takes its clap entry. dlopen hands back the
// library already loaded for that file, however the path reaches it - its entry
// is then shared with the plugins using it and not initialized again. One
// library reference is held for all of them, released by the last one
static void clap_plug_entry_open(CLAP_PLUG_INFO *plug_data,
                                 CLAP_PLUG_PLUG *plug) {
    if (!plug_data)
        return;
    if (!plug)
        return;
    void *handle = dlopen(plug->plug_path, RTLD_LOCAL | RTLD_LAZY);
    if (!handle)
        return;
    clap_plugin_entry_t *plug_entry =
        (clap_plugin_entry_t *)dlsym(handle, "clap_entry");
    if (!plug_entry) {
        dlclose(handle);
        return;
    }

    for (unsigned int plug_num = 0; plug_num < MAX_INSTANCES; plug_num++) {
        const CLAP_PLUG_PLUG *cur_plug = &plug_data->plugins[plug_num];
        if (cur_plug->id == plug->id || cur_plug->plug_entry != plug_entry)
            continue;
        dlclose(handle);
        plug->plug_entry = cur_plug->plug_entry;
        plug->dso_handle = cur_plug->dso_handle;
        return;
    }

    if (!plug_entry->init(plug->plug_path)) {
        dlclose(handle);
        return;
    }
    plug->plug_entry = plug_entry;
    plug->dso_handle = handle;
}

// adds one row. A descriptor id already listed keeps its first row
static void clap_plug_list_add(CLAP_PLUG_INFO *plug_data, const char *file_path,
                               int plug_inst_id, const char *plugin_id,
                               const char *name) {
    PLUGIN_LIST *plugin_list = &(plug_data->clap_plugin_list);
    uint64_t key = intern_add(&plugin_list->ids, plugin_id);
    if (!key || intern_slot(&plugin_list->ids, key) != INTERN_SLOT_NONE)
        return;
    if (plugin_list->size_curr == plugin_list->size_max) {
        unsigned int new_max_size = plugin_list->size_max * 2;
        PLUGIN_LIST_ITEM *temp_array = realloc(
            plugin_list->plugin_list, new_max_size * sizeof(PLUGIN_LIST_ITEM));
        if (!temp_array)
            return;
        plugin_list->size_max = new_max_size;
        plugin_list->plugin_list = temp_array;
    }
    PLUGIN_LIST_ITEM *cur_item =
        &(plugin_list->plugin_list[plugin_list->size_curr]);
    snprintf(cur_item->path, MAX_PATH_STRING, "%s", file_path);
    snprintf(cur_item->short_name, MAX_SHORT_NAME_LENGTH, "%s", name);
    cur_item->plug_inst_id = plug_inst_id;
    cur_item->key = key;
    cur_item->plug_data = plug_data;
    intern_set_slot(&plugin_list->ids, key, plugin_list->size_curr);
    plugin_list->size_curr += 1;
}

// files and directories one list scan has visited, so one reached again
// through a symlink or an overlapping search path is scanned once
typedef struct _clap_scan_seen {
    dev_t dev;
    ino_t ino;
} CLAP_SCAN_SEEN;

typedef struct _clap_scan {
    char scanner[MAX_PATH_STRING]; // path of the CLAP_SCAN_EXE program
    CLAP_SCAN_SEEN *seen;
    size_t count;
    size_t max;
} CLAP_SCAN;

// false when st's file was visited already
static bool clap_scan_first_visit(CLAP_SCAN *scan, const struct stat *st) {
    for (size_t i = 0; i < scan->count; i++) {
        if (scan->seen[i].dev == st->st_dev && scan->seen[i].ino == st->st_ino)
            return false;
    }
    if (scan->count == scan->max) {
        size_t new_max = scan->max ? scan->max * 2 : 32;
        CLAP_SCAN_SEEN *grown =
            realloc(scan->seen, sizeof(CLAP_SCAN_SEEN) * new_max);
        if (!grown)
            return false;
        scan->seen = grown;
        scan->max = new_max;
    }
    scan->seen[scan->count++] =
        (CLAP_SCAN_SEEN){.dev = st->st_dev, .ino = st->st_ino};
    return true;
}

// the scanner sits next to this program's own binary
static bool clap_scan_find_scanner(char *out, size_t out_len) {
    char exe[MAX_PATH_STRING];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe));
    if (len <= 0 || (size_t)len >= sizeof(exe))
        return false;
    exe[len] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash)
        return false;
    *slash = '\0';
    int path_len = snprintf(out, out_len, "%s/%s", exe, CLAP_SCAN_EXE);
    if (path_len < 0 || (size_t)path_len >= out_len)
        return false;
    return access(out, X_OK) == 0;
}

static int clap_scan_ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)((now.tv_sec - start->tv_sec) * 1000 +
                 (now.tv_nsec - start->tv_nsec) / 1000000);
}

// runs the scanner with argv (argv[0] is its path) and returns everything it
// printed (NULL when nothing), its length in *out_len. A scanner that hangs is
// killed after timeout_ms, keeping what it printed so far. Output past
// max_output is cut off
static char *clap_scan_run(char *const argv[], int timeout_ms,
                           size_t max_output, size_t *out_len) {
    *out_len = 0;
    int fds[2];
    if (pipe(fds) != 0)
        return NULL;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    // a clean signal state, whatever this process blocks or handles
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t no_signals;
    sigset_t all_signals;
    sigemptyset(&no_signals);
    sigfillset(&all_signals);
    posix_spawnattr_setsigmask(&attr, &no_signals);
    posix_spawnattr_setsigdefault(&attr, &all_signals);
    posix_spawnattr_setflags(&attr,
                             POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid;
    int spawn_err =
        posix_spawn(&pid, argv[0], &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (spawn_err != 0) {
        close(fds[0]);
        return NULL;
    }

    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    bool ended = false;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int left_ms = timeout_ms - clap_scan_ms_since(&start);
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        int ready = left_ms > 0 ? poll(&pfd, 1, left_ms) : 0;
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        if (len == cap) {
            size_t new_cap = cap ? cap * 2 : 4096;
            char *grown =
                new_cap <= max_output ? realloc(buf, new_cap) : NULL;
            if (!grown)
                break;
            buf = grown;
            cap = new_cap;
        }
        ssize_t got = read(fds[0], buf + len, cap - len);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            ended = true;
            break;
        }
        len += (size_t)got;
    }
    close(fds[0]);
    if (!ended)
        kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
    *out_len = len;
    return buf;
}

// the next '\0' terminated field at *pos, NULL when the output ends first
static const char *clap_scan_field(const char *buf, size_t len, size_t *pos) {
    if (!buf || *pos >= len)
        return NULL;
    const char *field = buf + *pos;
    const char *end = memchr(field, '\0', len - *pos);
    if (!end)
        return NULL;
    *pos = (size_t)(end - buf) + 1;
    return field;
}

// walks the scanner's output (see clap_scan.h), adding the rows when add is
// set. false when the output stops before the end mark or is malformed - the
// rows are then not to be trusted
static bool clap_scan_parse(CLAP_PLUG_INFO *plug_data, const char *file_path,
                            const char *buf, size_t len, bool add) {
    size_t pos = 0;
    for (;;) {
        const char *index = clap_scan_field(buf, len, &pos);
        if (!index)
            return false;
        if (index[0] == '\0')
            return true;
        const char *plugin_id = clap_scan_field(buf, len, &pos);
        const char *name = clap_scan_field(buf, len, &pos);
        if (!plugin_id || !name)
            return false;
        char *index_end = NULL;
        unsigned long plug_inst_id = strtoul(index, &index_end, 10);
        if (*index_end != '\0' || plug_inst_id > INT32_MAX)
            return false;
        if (add)
            clap_plug_list_add(plug_data, file_path, (int)plug_inst_id,
                               plugin_id, name);
    }
}

// lists one .clap file through the scanner, which loads it in a process of its
// own: nothing the plugin does while being listed can leak into or crash this
// one
static void clap_plug_scan_file(CLAP_PLUG_INFO *plug_data,
                                const CLAP_SCAN *scan, const char *file_path) {
    size_t len = 0;
    char *const argv[] = {(char *)scan->scanner, (char *)file_path, NULL};
    char *out = clap_scan_run(argv, CLAP_SCAN_TIMEOUT_MS, CLAP_SCAN_MAX_OUTPUT,
                              &len);
    if (clap_scan_parse(plug_data, file_path, out, len, false))
        clap_scan_parse(plug_data, file_path, out, len, true);
    else
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Could not list the plugins in %s\n", file_path);
    free(out);
}

// every .clap file under dir_path, however deep
static void clap_plug_scan_dir(CLAP_PLUG_INFO *plug_data, CLAP_SCAN *scan,
                               const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d)
        return;
    size_t dir_len = strlen(dir_path);
    const char *sep = (dir_len > 0 && dir_path[dir_len - 1] == '/') ? "" : "/";
    struct dirent *dir = NULL;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
            continue;
        char entry_path[MAX_PATH_STRING];
        int path_len = snprintf(entry_path, MAX_PATH_STRING, "%s%s%s",
                                dir_path, sep, dir->d_name);
        if (path_len < 0 || path_len >= MAX_PATH_STRING)
            continue;
        // follows symlinks, so a linked file or directory counts as its target
        struct stat st;
        if (stat(entry_path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (clap_scan_first_visit(scan, &st))
                clap_plug_scan_dir(plug_data, scan, entry_path);
            continue;
        }
        if (S_ISREG(st.st_mode) &&
            path_extension_matches(dir->d_name, "clap") == 1 &&
            clap_scan_first_visit(scan, &st))
            clap_plug_scan_file(plug_data, scan, entry_path);
    }
    closedir(d);
}

static void clap_plug_scan_root(CLAP_PLUG_INFO *plug_data, CLAP_SCAN *scan,
                                const char *path) {
    char dir_path[MAX_PATH_STRING];
    if (path_expand_home(path, dir_path, sizeof(dir_path)) != 0)
        return;
    struct stat st;
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    if (clap_scan_first_visit(scan, &st))
        clap_plug_scan_dir(plug_data, scan, dir_path);
}

// empty the preset list, the keys stay
static void clap_plug_presets_clear(CLAP_PLUG_PLUG *plug) {
    tree_index_reset(plug->presets);
    plug->presets_built = false;
}

// the identity of the preset at location / load_key into *buf, grown as
// needed. false when it cannot grow
static bool clap_scan_preset_identity(char **buf, size_t *cap,
                                      const char *location,
                                      const char *load_key) {
    size_t need = clap_ext_preset_identity(NULL, 0, location, load_key) + 1;
    if (need > *cap) {
        char *grown = realloc(*buf, need);
        if (!grown)
            return false;
        *buf = grown;
        *cap = need;
    }
    clap_ext_preset_identity(*buf, *cap, location, load_key);
    return true;
}

// adds the scanner's preset records (see clap_scan.h) to tree, in the order
// they came. false when the output stops before the end mark or is malformed
// - the presets read until then are kept
static bool clap_scan_presets_parse(TREE_INDEX *tree, const char *buf,
                                    size_t len) {
    size_t pos = 0;
    char *identity = NULL;
    size_t identity_cap = 0;
    bool complete = false;
    for (;;) {
        const char *field = clap_scan_field(buf, len, &pos);
        if (!field || field[0] == '\0') {
            complete = field != NULL;
            break;
        }
        char *depth_end = NULL;
        unsigned long depth = strtoul(field, &depth_end, 10);
        if (*depth_end != '\0')
            break;
        // the category path, one branch per segment
        uint64_t branch = 0;
        bool placed = true;
        for (unsigned long d = 0; d < depth && field; d++) {
            field = clap_scan_field(buf, len, &pos);
            if (field && placed) {
                branch = tree_index_branch(tree, branch, field);
                placed = branch != 0;
            }
        }
        const char *name = field ? clap_scan_field(buf, len, &pos) : NULL;
        const char *location = name ? clap_scan_field(buf, len, &pos) : NULL;
        const char *load_key =
            location ? clap_scan_field(buf, len, &pos) : NULL;
        if (!load_key)
            break;
        if (placed && clap_scan_preset_identity(&identity, &identity_cap,
                                                location, load_key))
            tree_index_leaf(tree, branch, name, identity);
    }
    free(identity);
    return complete;
}

// fills the preset list through the scanner, which runs the plugin's
// preset-discovery factory in a process of its own: no instance of the plugin
// lives there and nothing the plugin does while indexing can reach this one
static void clap_plug_presets_scan(CLAP_PLUG_PLUG *plug) {
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    char scanner[MAX_PATH_STRING];
    if (!clap_scan_find_scanner(scanner, sizeof(scanner)))
        return;
    char *const argv[] = {scanner, (char *)CLAP_SCAN_PRESETS_ARG,
                          plug->plug_path, plug->plugin_id, NULL};
    size_t len = 0;
    char *out = clap_scan_run(argv, CLAP_SCAN_PRESETS_TIMEOUT_MS,
                              CLAP_SCAN_PRESETS_MAX_OUTPUT, &len);
    if (!clap_scan_presets_parse(plug->presets, out, len))
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Not every preset of %s could be listed\n",
                             plug->name);
    free(out);
}

// the preset list, read the first time it is asked for. NULL (no presets)
// when the index cannot be allocated - the next browse tries again
static TREE_INDEX *clap_plug_presets_get(CLAP_PLUG_PLUG *plug) {
    if (!plug || !plug->plug_inst || !plug->plug_data)
        return NULL;
    if (!plug->presets) {
        plug->presets = tree_index_new();
        if (!plug->presets)
            return NULL;
    }
    if (!plug->presets_built) {
        // also when there are none, so they are not looked for on every browse
        plug->presets_built = true;
        tree_index_reset(plug->presets);
        clap_plug_presets_scan(plug);
        tree_index_finish(plug->presets);
    }
    return plug->presets;
}

size_t clap_plug_presets_level_count(void *plug, uint64_t branch) {
    return tree_index_level_count(clap_plug_presets_get(plug), branch);
}

bool clap_plug_presets_level_at(void *plug, uint64_t branch, size_t idx,
                                TREE_ROW *out) {
    return tree_index_level_at(clap_plug_presets_get(plug), branch, idx, out);
}

int clap_plug_preset_load(void *plug, uint64_t key) {
    CLAP_PLUG_PLUG *cur_plug = (CLAP_PLUG_PLUG *)plug;
    if (!cur_plug || !cur_plug->plug_inst)
        return -1;
    const char *identity = tree_index_leaf_identity(cur_plug->presets, key);
    if (!identity)
        return -2;
    if (!clap_ext_preset_load(cur_plug->plug_inst, identity))
        return -1;
    // not every plugin reports the new values (rescan or events) - u-he does
    // not after the first load. Harmless when the plugin loads async (Surge),
    // it rescans later itself
    atomic_fetch_or(&cur_plug->requests, CLAP_PLUG_REQ_SYNC_VALUES);
    return 0;
}

int clap_plug_plugin_list_init(CLAP_PLUG_INFO *plug_data) {
    if (!plug_data)
        return -1;
    // a rescan is also how new preset files of a loaded plugin show up
    for (int i = 0; i < MAX_INSTANCES; i++)
        clap_plug_presets_clear(&plug_data->plugins[i]);
    // checked first, so a list that cannot be rebuilt is kept
    CLAP_SCAN scan = {0};
    if (!clap_scan_find_scanner(scan.scanner, sizeof(scan.scanner))) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Could not find %s next to this program, clap "
                             "plugins can not be listed\n",
                             CLAP_SCAN_EXE);
        return -1;
    }
    PLUGIN_LIST *plugin_list = &(plug_data->clap_plugin_list);
    if (plugin_list->plugin_list)
        free(plugin_list->plugin_list);
    plugin_list->size_curr = 0;
    plugin_list->size_max = 0;
    intern_detach_all(&plugin_list->ids);
    plugin_list->plugin_list =
        calloc(PTR_ARRAY_COUNT, sizeof(PLUGIN_LIST_ITEM));
    if (!plugin_list->plugin_list)
        return -1;
    plugin_list->size_max = PTR_ARRAY_COUNT;

    // CLAP_PATH is ':' separated like PATH. Searched first, so a directory the
    // user named wins a plugin id also installed in a default path
    const char *env_paths = getenv("CLAP_PATH");
    char *env_copy = env_paths ? strdup(env_paths) : NULL;
    if (env_copy) {
        char *save = NULL;
        for (char *path = strtok_r(env_copy, ":", &save); path;
             path = strtok_r(NULL, ":", &save))
            clap_plug_scan_root(plug_data, &scan, path);
        free(env_copy);
    }
    for (unsigned int i = 0; clap_paths[i]; i++)
        clap_plug_scan_root(plug_data, &scan, clap_paths[i]);
    free(scan.seen);

    return 1;
}

unsigned int clap_plug_plugin_list_count(CLAP_PLUG_INFO *plug_data) {
    if (!plug_data)
        return 0;
    return plug_data->clap_plugin_list.size_curr;
}

void *clap_plug_plugin_list_item_get(CLAP_PLUG_INFO *plug_data,
                                     unsigned int idx) {
    if (!plug_data)
        return NULL;
    PLUGIN_LIST cur_plugin_list = plug_data->clap_plugin_list;
    if (idx >= cur_plugin_list.size_curr)
        return NULL;
    PLUGIN_LIST_ITEM *cur_plugin_list_item =
        &(cur_plugin_list.plugin_list[idx]);
    return (void *)cur_plugin_list_item;
}

const char *clap_plug_plugin_list_item_name(void *plugin_item) {
    PLUGIN_LIST_ITEM *plugin_list_item = (PLUGIN_LIST_ITEM *)plugin_item;
    if (!plugin_list_item)
        return NULL;
    return plugin_list_item->short_name;
}

uint64_t clap_plug_plugin_list_item_key(void *plugin_item) {
    PLUGIN_LIST_ITEM *plugin_list_item = (PLUGIN_LIST_ITEM *)plugin_item;
    if (!plugin_list_item)
        return 0;
    return plugin_list_item->key;
}

void *clap_plug_plugin_list_item_by_key(CLAP_PLUG_INFO *plug_data,
                                        uint64_t key) {
    if (!plug_data)
        return NULL;
    PLUGIN_LIST *list = &plug_data->clap_plugin_list;
    size_t slot = intern_slot(&list->ids, key);
    if (slot >= list->size_curr)
        return NULL;
    return &list->plugin_list[slot];
}

// build the display name into plug->name. Called once when the plugin is
// loaded; clap_plug_plugin_name() just returns the stored string after.
static void clap_plug_set_display_name(CLAP_PLUG_PLUG *plug) {
    if (!plug || !plug->plug_inst || !plug->plug_inst->desc)
        return;
    snprintf(plug->name, sizeof(plug->name), "%s", plug->plug_inst->desc->name);
}

uint32_t clap_plug_load_and_activate(void *plugin_item) {
    PLUGIN_LIST_ITEM *plugin_list_item = (PLUGIN_LIST_ITEM *)plugin_item;
    if (!plugin_list_item)
        return 0;
    CLAP_PLUG_INFO *plug_data = plugin_list_item->plug_data;
    if (!plug_data)
        return 0;

    // find an empty slot in the plugins array and create the
    // plugin there
    int id = -1;
    for (int i = 0; i < (MAX_INSTANCES); i++) {
        CLAP_PLUG_PLUG cur_plug = plug_data->plugins[i];
        // if there is an plugin entry point the slot is not empty
        if (cur_plug.plug_entry)
            continue;
        id = cur_plug.id;
        break;
    }

    // if id is not in range an error occured or there is no space for the
    // plugin
    if (id < 0 || id >= MAX_INSTANCES)
        return 0;

    CLAP_PLUG_PLUG *plug = &(plug_data->plugins[id]);
    // if id is in the possible range, clean the slot just in case its occupied
    clap_plug_plug_stop_and_clean((void *)plug);
    // assign the identity uid once, when the slot is claimed. Ports are
    // registered under it as their owner - and plugin code can ask for ports
    // from as early as init - so it has to exist before anything else runs.
    // A slot keeps its last uid until this reassigns it
    plug->uid = ++plug_data->next_plug_uid;
    plug->node = graph_node_add(plug_data->graph, plug_data->owner_tag,
                                plug->uid, clap_plug_plugin_process_rt, plug);
    if (!plug->node)
        return 0;

    snprintf(plug->plug_path, MAX_PATH_STRING, "%s", plugin_list_item->path);
    clap_plug_entry_open(plug_data, plug);
    if (!plug->plug_entry) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Could not create entry point for %s plugin\n",
                             plug->plug_path);
        clap_plug_plug_stop_and_clean((void *)plug);
        return 0;
    }

    plug->plug_inst_id = plugin_list_item->plug_inst_id;

    const clap_plugin_factory_t *plug_fac =
        plug->plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t *plug_desc =
        plug_fac->get_plugin_descriptor(plug_fac, plug->plug_inst_id);
    if (!plug_desc) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Could not get plugin %s descriptor\n",
                             plug->plug_path);
        clap_plug_plug_stop_and_clean((void *)plug);
        return 0;
    }
    if (plug_desc->name) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Got clap_plugin %s descriptor\n",
                             plug_desc->name);
    }
    if (plug_desc->description) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "%s info: %s\n", plug_desc->name,
                             plug_desc->description);
    }
    snprintf(plug->plugin_id, MAX_UNIQUE_ID_STRING, "%s", plug_desc->id);

    // add the clap_host_t struct to the plug
    clap_host_t clap_info_host;
    clap_info_host.clap_version = plug_data->clap_host_info.clap_version;
    clap_info_host.host_data = (void *)plug;
    clap_info_host.name = plug_data->clap_host_info.name;
    clap_info_host.vendor = plug_data->clap_host_info.vendor;
    clap_info_host.url = plug_data->clap_host_info.url;
    clap_info_host.version = plug_data->clap_host_info.version;
    clap_info_host.get_extension = plug_data->clap_host_info.get_extension;
    clap_info_host.request_restart = plug_data->clap_host_info.request_restart;
    clap_info_host.request_process = plug_data->clap_host_info.request_process;
    clap_info_host.request_callback =
        plug_data->clap_host_info.request_callback;
    plug->clap_host_info = clap_info_host;

    // create plugin instance
    const clap_plugin_t *plug_inst = plug_fac->create_plugin(
        plug_fac, &(plug->clap_host_info), plug_desc->id);
    if (!plug_inst) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Failed to create %s plugin\n", plug->plug_path);
        clap_plug_plug_stop_and_clean((void *)plug);
        return 0;
    }

    plug->plug_inst = plug_inst;
    plug->plug_inst_created = 1;
    bool inst_err = plug->plug_inst->init(plug->plug_inst);
    if (!inst_err) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Failed to init %s plugin\n", plug->plug_path);
        clap_plug_plug_stop_and_clean((void *)plug);
        return 0;
    }

    // Create the ports
    int port_out_err =
        clap_plug_create_ports(plug_data, plug->id, &(plug->output_ports), 0);
    int port_in_err =
        clap_plug_create_ports(plug_data, plug->id, &(plug->input_ports), 1);
    if (port_out_err == -1 || port_in_err == -1) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Failed to create %s plugin audio ports\n",
                             plug->plug_path);
        clap_plug_plug_stop_and_clean((void *)plug);
        return 0;
    }

    // Initiate the event lists
    clap_input_events_t *in_events = &(plug->input_events);
    in_events->ctx = (void *)ub_init(EVENT_LIST_SIZE, EVENT_LIST_ITEMS);
    in_events->get = clap_plug_ext_events_get;
    in_events->size = clap_plug_ext_events_size;
    plug->out_list = ub_init(EVENT_LIST_SIZE, EVENT_LIST_ITEMS);
    plug->output_events.ctx = (void *)plug;
    plug->output_events.try_push = clap_plug_ext_events_try_push;
    if (!in_events->ctx || !plug->out_list) {
        context_sub_send_msg(
            plug_data->control_data, (void *)plug_data, is_audio_thread,
            "Failed to create %s plugin clap event structs\n", plug->plug_path);
        clap_plug_plug_stop_and_clean((void *)plug);
        return 0;
    }

    // Initiate the note ports on the graph
    int note_port_out_err = clap_plug_note_ports_create(plug_data, plug->id, 0);
    int note_port_in_err = clap_plug_note_ports_create(plug_data, plug->id, 1);
    if (note_port_in_err == -1 || note_port_out_err == -1) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             clap_plug_return_is_audio_thread(),
                             "Failed to create %s plugin note ports\n",
                             plug->plug_path);
        clap_plug_plug_stop_and_clean((void *)plug);
        return 0;
    }
    // create the parameter cotainer
    if (clap_plug_params_create(plug_data, plug->id) != 0) {
        context_sub_send_msg(
            plug_data->control_data, (void *)plug_data,
            clap_plug_return_is_audio_thread(),
            "Failed to create %s plugin parameters container\n",
            plug->plug_path);
        clap_plug_plug_stop_and_clean((void *)plug);
        return 0;
    }

    // build the display name once, now that plug->plug_inst and plug->id are
    // set
    clap_plug_set_display_name(plug);

    // start processing the plugin
    clap_plug_activate_start_processing((void *)plug);

    plug_data->plugins_dirty = true;

    return plug->uid;
}

void *clap_plug_plugin_return(CLAP_PLUG_INFO *plug_data, unsigned int idx) {
    if (!plug_data)
        return NULL;

    // the plugins array can have gaps, walk the occupied slots in order and
    // return the idx-th one; NULL once idx is past the last occupied slot
    unsigned int found = 0;
    for (unsigned int i = 0; i < MAX_INSTANCES; i++) {
        CLAP_PLUG_PLUG *cur_plug = &(plug_data->plugins[i]);
        if (!cur_plug->plug_inst)
            continue;
        if (found == idx)
            return (void *)cur_plug;
        found++;
    }
    return NULL;
}

const char *clap_plug_plugin_name(void *plug) {
    CLAP_PLUG_PLUG *cur_plug = (CLAP_PLUG_PLUG *)plug;
    if (!cur_plug)
        return NULL;
    if (!cur_plug->plug_entry)
        return NULL;
    CLAP_PLUG_INFO *plug_data = cur_plug->plug_data;
    if (!plug_data)
        return NULL;

    return cur_plug->name;
}

uint32_t clap_plug_plugin_uid(void *plug) {
    CLAP_PLUG_PLUG *cur_plug = (CLAP_PLUG_PLUG *)plug;
    if (!cur_plug)
        return 0;
    return cur_plug->uid;
}

// return this plugin instance's own param container, NULL on error/none yet
PRM_CONTAIN *clap_plug_plugin_param_container(void *plug) {
    CLAP_PLUG_PLUG *cur_plug = (CLAP_PLUG_PLUG *)plug;
    if (!cur_plug)
        return NULL;
    return cur_plug->plug_params;
}

bool clap_plug_plugins_is_dirty(CLAP_PLUG_INFO *plug_data) {
    if (!plug_data)
        return false;
    bool is_dirty = plug_data->plugins_dirty;
    plug_data->plugins_dirty = false;
    return is_dirty;
}

// return -1 on error, return 0 if successful but the output was quiet and
// return 1 if successful and the output not quiet
static int clap_prepare_input_ports(CLAP_PLUG_INFO *plug_data,
                                    CLAP_PLUG_PORT *input_ports,
                                    unsigned int nframes) {
    if (!plug_data)
        return -1;
    if (!input_ports)
        return -1;
    // no ports is quiet, not an error
    if (input_ports->ports_count == 0)
        return 0;
    if (!input_ports->graph_port_array)
        return -1;
    if (!input_ports->audio_ports)
        return -1;
    int not_quiet = 0;
    for (uint32_t port = 0; port < input_ports->ports_count; port++) {
        CLAP_PLUG_PORT_GRAPH cur_port_graph =
            input_ports->graph_port_array[port];
        clap_audio_buffer_t cur_clap_port = input_ports->audio_ports[port];
        // TODO nothing is done with the latency property
        uint32_t channels = cur_port_graph.channel_count;
        for (uint32_t chan = 0; chan < channels; chan++) {
            const SAMPLE_T *graph_buffer = NULL;
            if (cur_port_graph.graph_ports)
                graph_buffer =
                    graph_port_audio_rt(cur_port_graph.graph_ports[chan]);
            for (unsigned int frame = 0; frame < nframes; frame++) {
                SAMPLE_T cur_frame = 0.0;
                if (graph_buffer)
                    cur_frame = graph_buffer[frame];
                if (not_quiet == 0 && cur_frame != 0)
                    not_quiet = 1;
#if SAMPLE_T_AS_DOUBLE == 1
                if (cur_clap_port.data64)
                    cur_clap_port.data64[chan][frame] = cur_frame;
#else
                if (cur_clap_port.data32)
                    cur_clap_port.data32[chan][frame] = cur_frame;
#endif
            }
        }
    }

    return not_quiet;
}

// process the output audio ports, by copying them to the graph output ports
// return -1 on error, return 0 if successful but the output was quiet and
// return 1 if successful and the output not quiet
static int clap_prepare_output_ports(CLAP_PLUG_INFO *plug_data,
                                     CLAP_PLUG_PORT *output_ports,
                                     unsigned int nframes) {
    if (!plug_data)
        return -1;
    if (!output_ports)
        return -1;
    // no ports is quiet, not an error
    if (output_ports->ports_count == 0)
        return 0;
    if (!output_ports->graph_port_array)
        return -1;
    if (!output_ports->audio_ports)
        return -1;
    int not_quiet = 0;
    for (uint32_t port = 0; port < output_ports->ports_count; port++) {
        CLAP_PLUG_PORT_GRAPH cur_port_graph =
            output_ports->graph_port_array[port];
        clap_audio_buffer_t clap_port = output_ports->audio_ports[port];
        uint32_t channels = cur_port_graph.channel_count;
        // TODO nothing is done with the latency property
        for (uint32_t chan = 0; chan < channels; chan++) {
            SAMPLE_T *clap_buffer = NULL;
#if SAMPLE_T_AS_DOUBLE == 1
            if (clap_port.data64)
                clap_buffer = clap_port.data64[chan];
#else
            if (clap_port.data32)
                clap_buffer = clap_port.data32[chan];
#endif
            SAMPLE_T *graph_buffer = NULL;
            if (cur_port_graph.graph_ports)
                graph_buffer =
                    graph_port_audio_rt(cur_port_graph.graph_ports[chan]);
            if (!graph_buffer)
                continue;
            memset(graph_buffer, '\0', sizeof(SAMPLE_T) * nframes);
            if (!clap_buffer)
                continue;
            // if the buffer is constant leave the graph buffer filled with
            // zeroes
            if ((clap_port.constant_mask & (1 << chan)) != 0)
                continue;
            for (unsigned int frame = 0; frame < nframes; frame++) {
                graph_buffer[frame] = clap_buffer[frame];
                if (clap_buffer[frame] != 0 && not_quiet == 0)
                    not_quiet = 1;
            }
        }
    }
    return not_quiet;
}

// apply the param events, the note events are in the note ports already (see
// clap_plug_ext_events_try_push).
// return -1 on error, return 0 if successful but the output was quiet and
// return 1 if successful and the output not quiet
static int clap_output_events_read(CLAP_PLUG_INFO *plug_data,
                                   CLAP_PLUG_PLUG *plug) {
    if (!plug_data)
        return -1;
    if (!plug)
        return -1;
    if (!plug->out_list)
        return -1;
    int not_quiet = clap_output_events_params_apply(plug, plug->out_list);
    if(not_quiet == 1)
        return 1;
    CLAP_PLUG_NOTE_PORT *note_ports = &(plug->output_note_ports);
    for (uint32_t i = 0; note_ports->midi_bufs && i < note_ports->ports_count;
         i++) {
        if (midi_buf_count(note_ports->midi_bufs[i]) > 0)
            return 1;
    }
    return not_quiet;
}

// the dialect an input note port gets: the preferred one if it is CLAP or
// MIDI, else CLAP, else MIDI. 0 when it supports neither
static uint32_t clap_note_port_dialect(const CLAP_PLUG_NOTE_PORT *note_ports,
                                       uint32_t port) {
    uint32_t preferred = note_ports->preferred_dialects[port];
    if (preferred == CLAP_NOTE_DIALECT_CLAP ||
        preferred == CLAP_NOTE_DIALECT_MIDI)
        return preferred;
    uint32_t supported = note_ports->supported_dialects[port];
    if (supported & CLAP_NOTE_DIALECT_CLAP)
        return CLAP_NOTE_DIALECT_CLAP;
    if (supported & CLAP_NOTE_DIALECT_MIDI)
        return CLAP_NOTE_DIALECT_MIDI;
    return 0;
}

// push an input note port event in the port's dialect. CLAP: notes as note
// events, the rest raw if the port supports MIDI too. MIDI: every message raw.
// false when the dialect has no place for it
static bool clap_input_events_note_add(CLAP_PLUG_PLUG *plug, UB_EVENT *ub_in,
                                       const MIDI_EVENT *ev, uint32_t port) {
    const CLAP_PLUG_NOTE_PORT *note_ports = &(plug->input_note_ports);
    uint32_t dialect = clap_note_port_dialect(note_ports, port);
    clap_event_header_t head = {.time = ev->frame,
                                .space_id = CLAP_CORE_EVENT_SPACE_ID,
                                .flags = CLAP_EVENT_IS_LIVE};
    if (dialect == CLAP_NOTE_DIALECT_CLAP &&
        (midi_is_note_on(ev->data) || midi_is_note_off(ev->data))) {
        head.type = midi_is_note_on(ev->data) ? CLAP_EVENT_NOTE_ON
                                              : CLAP_EVENT_NOTE_OFF;
        head.size = sizeof(clap_event_note_t);
        clap_event_note_t note = {
            .header = head,
            .note_id = -1,
            .port_index = (int16_t)port,
            .channel = (int16_t)midi_channel(ev->data),
            .key = (int16_t)ev->data[1],
            .velocity = (double)fit_range(127, 0, 1, 0, (SAMPLE_T)ev->data[2])};
        clap_input_events_push(plug, ub_in, &note, head.size);
        return true;
    }
    if (dialect != CLAP_NOTE_DIALECT_MIDI &&
        !(note_ports->supported_dialects[port] & CLAP_NOTE_DIALECT_MIDI))
        return false;
    if (midi_type(ev->data) == MIDI_SYSEX) {
        head.type = CLAP_EVENT_MIDI_SYSEX;
        head.size = sizeof(clap_event_midi_sysex_t);
        // points into the port's MIDI_BUF, cleared only next cycle
        clap_event_midi_sysex_t sysex = {.header = head,
                                         .port_index = (uint16_t)port,
                                         .buffer = ev->data,
                                         .size = ev->size};
        clap_input_events_push(plug, ub_in, &sysex, head.size);
        return true;
    }
    head.type = CLAP_EVENT_MIDI;
    head.size = sizeof(clap_event_midi_t);
    // a shorter message stays zero padded
    clap_event_midi_t midi = {.header = head, .port_index = (uint16_t)port};
    memcpy(midi.data, ev->data, ev->size);
    clap_input_events_push(plug, ub_in, &midi, head.size);
    return true;
}

// return -1 on error, return 0 if successful but the output was quiet and
// return 1 if successful and the output not quiet
static int clap_input_events_prepare(CLAP_PLUG_INFO *plug_data,
                                     CLAP_PLUG_PLUG *plug) {
    if (!plug_data)
        return -1;
    if (!plug)
        return -1;
    clap_input_events_t *in_events = &(plug->input_events);
    if (!in_events->ctx)
        return -1;
    UB_EVENT *ub_in = (UB_EVENT *)in_events->ctx;
    // reset the event array
    ub_list_reset(ub_in);
    // the params go first, at frame 0
    int not_quiet = clap_input_events_params_add(plug, ub_in);
    // TODO how to add transport events so everything is sorted by the frames
    // offset?

    CLAP_PLUG_NOTE_PORT *note_ports = &(plug->input_note_ports);
    if (note_ports->ports_count == 0)
        return not_quiet;
    clap_note_ports_bufs_rt(note_ports);
    // past what the merge walks
    for (uint32_t port = MIDI_BUF_MERGE_MAX; port < note_ports->ports_count;
         port++)
        atomic_fetch_add(&plug->in_events_dropped,
                         midi_buf_count(note_ports->midi_bufs[port]));
    // every port's events in time order
    MIDI_BUF_MERGE_ITER it = midi_buf_merge_iter(
        (const MIDI_BUF *const *)note_ports->midi_bufs, note_ports->ports_count);
    MIDI_EVENT ev;
    uint32_t port = 0;
    while (midi_buf_merge_next(&it, &ev, &port)) {
        if (clap_input_events_note_add(plug, ub_in, &ev, port))
            not_quiet = 1;
    }
    return not_quiet;
}

bool clap_plug_plugin_process_rt(void *plug_ptr, NFRAMES_T nframes) {
    CLAP_PLUG_PLUG *plug = (CLAP_PLUG_PLUG *)plug_ptr;
    if (!plug)
        return false;
    // do nothing if the plugin is completely stopped
    if (plug->plug_inst_processing == 0)
        return false;
    CLAP_PLUG_INFO *plug_data = plug->plug_data;
    // not what it was activated for - a buffer size change, until its restart
    if (nframes < plug_data->min_buffer_size ||
        nframes > plug_data->max_buffer_size)
        return false;

    clap_process_t _process = {0};
    _process.steady_time = -1;
    _process.frames_count = nframes;
    _process.transport = NULL;

    // copy the graph input buffers to clap input audio buffers
    int in_audio_not_quiet =
        clap_prepare_input_ports(plug_data, &(plug->input_ports), nframes);
    _process.audio_inputs = NULL;
    _process.audio_inputs_count = 0;
    if (in_audio_not_quiet != -1) {
        _process.audio_inputs_count = plug->input_ports.ports_count;
        _process.audio_inputs = plug->input_ports.audio_ports;
    }

    _process.audio_outputs = plug->output_ports.audio_ports;
    _process.audio_outputs_count = plug->output_ports.ports_count;

    // write messages from midi, parameters and similar to the input events
    int in_event_not_quiet = clap_input_events_prepare(plug_data, plug);
    _process.in_events = &(plug->input_events);

    // reset the output event array, the note outs the plugin pushes to were
    // cleared by the plan
    ub_list_reset(plug->out_list);
    clap_note_ports_bufs_rt(&(plug->output_note_ports));
    _process.out_events = &(plug->output_events);

    // the plugin is sleeping, check if it needs to wake up
    if (plug->plug_inst_processing == 2) {
        // if audio or event input was not quiet from the graph ports start
        // the plugin and continue the process
        if ((in_audio_not_quiet != 0 || in_event_not_quiet != 0) &&
            plug->plug_inst->start_processing(plug->plug_inst))
            plug->plug_inst_processing = 1;
        if (plug->plug_inst_processing == 2)
            return false;
    }

    clap_process_status clap_status =
        plug->plug_inst->process(plug->plug_inst, &_process);

    if (clap_status == CLAP_PROCESS_ERROR) {
        // process failed, false - the plan empties the outputs, the output
        // events are left unread
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             is_audio_thread,
                             "ERROR Processing discard buffer\n");
        return false;
    }
    if (clap_status == CLAP_PROCESS_CONTINUE) {
        clap_prepare_output_ports(plug_data, &(plug->output_ports), nframes);
        clap_output_events_read(plug_data, plug);
    }
    if (clap_status == CLAP_PROCESS_CONTINUE_IF_NOT_QUIET) {
        int not_quiet_audio = clap_prepare_output_ports(
            plug_data, &(plug->output_ports), nframes);
        int not_quiet_events = clap_output_events_read(plug_data, plug);
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             is_audio_thread, "Process if NOT_QUIET\n");
        // process successful but outputs are quiet send this plugin to
        // sleep
        if (not_quiet_audio == 0 && not_quiet_events == 0) {
            plug->plug_inst->stop_processing(plug->plug_inst);
            plug->plug_inst_processing = 2;
        }
    }
    if (clap_status == CLAP_PROCESS_TAIL) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             is_audio_thread, "Process if TAIL\n");
        clap_prepare_output_ports(plug_data, &(plug->output_ports), nframes);
        clap_output_events_read(plug_data, plug);
        // TODO implement tail extension
    }
    if (clap_status == CLAP_PROCESS_SLEEP) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             is_audio_thread, "SLEEP for now\n");
        clap_prepare_output_ports(plug_data, &(plug->output_ports), nframes);
        clap_output_events_read(plug_data, plug);
        // no need to process further so plugin goes to sleep
        plug->plug_inst->stop_processing(plug->plug_inst);
        plug->plug_inst_processing = 2;
    }
    return true;
}

void clap_plug_clean_memory(CLAP_PLUG_INFO *plug_data) {
    if (!plug_data)
        return;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        clap_plug_plug_clean(plug_data, i);
    }

    context_sub_clean(plug_data->control_data);

    if (plug_data->clap_plugin_list.plugin_list)
        free(plug_data->clap_plugin_list.plugin_list);
    intern_clean(&plug_data->clap_plugin_list.ids);

    free(plug_data);
}
