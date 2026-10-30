#include <dlfcn.h>
#include <lilv-0/lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/data-access/data-access.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/patch/patch.h>
#include <lv2/port-groups/port-groups.h>
#include <lv2/port-props/port-props.h>
#include <lv2/presets/presets.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/ui/ui.h>
#include <lv2/worker/worker.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
// my libraries
#include "../types.h"
#include "context_control.h"
#include "plugins.h"
#include <threads.h>
// my math funcs
#include "../util_funcs/math_funcs.h"
// symap for map to id and back
#include "../util_funcs/jalv/symap.h"
// for workers feature
#include "../util_funcs/jalv/worker.h"
#include "../util_funcs/jalv/zix/ring.h"
#include "../util_funcs/jalv/zix/sem.h"
#include "../util_funcs/intern_table.h"
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/midi_buf.h"
#include "../util_funcs/path_funcs.h"
#include "../util_funcs/string_funcs.h"
// a simple macro to get the max of the two values
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
// maximum instances of plugins
#define MAX_INSTANCES 5
// size of the single plugin display name buffer (PLUG_PLUG.name). Internal to
// this module - the name leaves as a const char* so callers need no matching
// define.
#define PLUG_PLUGIN_NAME_MAX 128
// saved presets go here, a bundle each - every LV2 host looks there
#define PLUG_PRESET_DIR "~/.lv2"
// buffer cycles for ui, for example to get the buffer_size we midi_buf_size *
// N_BUFFER_CYCLES
#define N_BUFFER_CYCLES 4
// default block_length, should be set to the client buffer size
#define DEFAULT_BLOCK_LENGTH 512
// default sample_rate should be set by the audio client
#define DEFAULT_SAMPLE_RATE 48000
// default atom sequence size, also given to plugins as bufsz:sequenceSize.
// Takes a full MIDI_BUF - an event there is 8 bytes or more, in a sequence up
// to 3 times that - plus the frame 0 atoms of the control port
#define DEFAULT_MIDI_BUF_SIZE (MIDI_PORT_BUF_SIZE * 3U + 4096U)
// atom message body size
#ifndef MSG_BUFFER_SIZE
#define MSG_BUFFER_SIZE 1024
#endif
// an embedded UI's host window until the UI sizes it
#define PLUG_GUI_START_W 400
#define PLUG_GUI_START_H 300

static thread_local bool is_audio_thread =
    false; // can be used to check what thread this is, useful for thread safe
           // functions

// plugin list item - from this struct a lv2 plugin can be loaded
typedef struct _plugin_list_item {
    char plugin_path[MAX_PATH_STRING]; // the plugin URI
    char plugin_short_name[MAX_SHORT_NAME_LENGTH];
    uint64_t key;
    PLUG_INFO *plug_data;
} PLUGIN_LIST_ITEM;

// the plugin list containing the plugin list items
typedef struct _plugin_list {
    unsigned int plugin_list_count;
    PLUGIN_LIST_ITEM *plugin_list;
    // every URI listed this session, so a key keeps meaning the same plugin
    // across rebuilds. The slot is its index in plugin_list
    INTERN_TABLE uris;
} PLUGIN_LIST;

// lilv nodes for more convinient coding, when for ex we need to tell port class
// by providing a livnode
typedef struct _plug_nodes {
    LilvNode *atom_AtomPort;
    LilvNode *atom_Chunk;
    LilvNode *atom_Float;
    LilvNode *atom_Path;
    LilvNode *atom_Sequence;
    LilvNode *lv2_AudioPort;
    LilvNode *lv2_CVPort;
    LilvNode *lv2_ControlPort;
    LilvNode *lv2_InputPort;
    LilvNode *lv2_OutputPort;
    LilvNode *lv2_connectionOptional;
    LilvNode *lv2_control;
    LilvNode *lv2_default;
    LilvNode *lv2_enumeration;
    LilvNode *lv2_extensionData;
    LilvNode *lv2_integer;
    LilvNode *lv2_maximum;
    LilvNode *lv2_minimum;
    LilvNode *lv2_name;
    LilvNode *lv2_optionalFeature;
    LilvNode *lv2_requiredFeature;
    LilvNode *lv2_sampleRate;
    LilvNode *lv2_symbol;
    LilvNode *lv2_toggled;
    LilvNode *midi_MidiEvent;
    LilvNode *pg_group;
    LilvNode *pprops_logarithmic;
    LilvNode *pprops_notOnGUI;
    LilvNode *pprops_rangeSteps;
    LilvNode *pset_Preset;
    LilvNode *pset_bank;
    LilvNode *rdfs_comment;
    LilvNode *rdfs_label;
    LilvNode *rdfs_range;
    LilvNode *rsz_minimumSize;
    LilvNode *ui_fixedSize;
    LilvNode *ui_noUserResize;
    LilvNode *ui_showInterface;
    LilvNode *ui_X11UI;
    LilvNode *work_interface;
    LilvNode *work_schedule;
    LilvNode *end;
} PLUG_NODES;

typedef struct _plug_urids {
    LV2_URID atom_Float;
    LV2_URID atom_Int;
    LV2_URID atom_Object;
    LV2_URID atom_Path;
    LV2_URID atom_String;
    LV2_URID atom_eventTransfer;
    LV2_URID bufsz_maxBlockLength;
    LV2_URID bufsz_minBlockLength;
    LV2_URID bufsz_sequenceSize;
    LV2_URID log_Error;
    LV2_URID log_Trace;
    LV2_URID log_Warning;
    LV2_URID midi_MidiEvent;
    LV2_URID param_sampleRate;
    LV2_URID patch_Get;
    LV2_URID patch_Put;
    LV2_URID patch_Set;
    LV2_URID patch_body;
    LV2_URID patch_property;
    LV2_URID patch_value;
    LV2_URID time_Position;
    LV2_URID time_bar;
    LV2_URID time_barBeat;
    LV2_URID time_beatUnit;
    LV2_URID time_beatsPerBar;
    LV2_URID time_beatsPerMinute;
    LV2_URID time_frame;
    LV2_URID time_speed;
    LV2_URID ui_scaleFactor;
    LV2_URID ui_updateRate;
} PLUG_URIDS;

// struct to hold the event data
typedef struct _plug_evbuf_impl {
    uint32_t capacity;
    uint32_t atom_Chunk;
    uint32_t atom_Sequence;
    uint32_t pad; // So buf has correct atom alignment
    LV2_Atom_Sequence buf;
} PLUG_EVBUF;
// iterator for the PLUG_EVBUF
typedef struct _plug_evbuf_iterator_impl {
    PLUG_EVBUF *evbuf;
    uint32_t offset;
} PLUG_EVBUF_ITERATOR;

typedef struct _plug_port {
    const LilvPort *lilv_port; ///< LV2 port
    enum PortType type;        ///< Data type
    enum FlowType flow;        ///< Data flow direction
    GRAPH_PORT *graph_port;    ///< For audio/CV/midi:MidiEvent ports
    PLUG_EVBUF *evbuf;         ///< For MIDI ports, otherwise NULL
    void *widget;              ///< Control widget, if applicable
    size_t buf_size;           ///< Custom buffer size, or 0
    uint32_t index;            ///< Port index
    int param_index; // index on the plug_plug->controls arrays if this is a
                     // CONTROL type port (will be the same index as on
                     // plug_params array)
    float control;   ///< For control ports, otherwise 0.0f
    // for convenience we can save the current port urid of its type
    LV2_URID port_type_urid;
    // events (MIDI, property sets) the input sequence had no room for, taken
    // on [main-thread]
    atomic_uint seq_dropped;
    // an output control's value after each run, written by [audio-thread]
    _Atomic(float) control_rt;
    // [main-thread] the value the shown UI last got or wrote
    float gui_sent;
} PLUG_PORT;

typedef struct _plug_features {
    LV2_Log_Log log;
    LV2_Feature log_feature;
    LV2_Feature map_feature;
    LV2_Feature unmap_feature;
    LV2_State_Make_Path make_path;
    LV2_Feature make_path_feature;
    LV2_Worker_Schedule sched;
    LV2_Feature sched_feature;
    LV2_Worker_Schedule ssched;
    LV2_Feature state_sched_feature;
    LV2_Feature safe_restore_feature;
    LV2_Options_Option options[5];
    LV2_Feature options_feature;
    LV2_Extension_Data_Feature ext_data;
} PLUG_FEATURES;

typedef enum {
    PORT,
    PROPERTY // property parameter not a port (set through atom msg)
} ControlType;
typedef struct {
    float value;
    char *label;
} ScalePoint;
typedef struct _plug_control {
    ControlType type;
    LilvNode *node; // port or property
    LilvNode *symbol;
    LilvNode *label;     // short name of the control
    LV2_URID property;   // iff type == PROPERTY
    uint32_t index;      // number of port if type == PORT
    LilvNode *group;     // port/property group or NULL if none
    size_t n_points;     // number of scale points
    ScalePoint *points;  // scale points
    LV2_URID value_type; // type of parameter value
    LilvNode *min;
    LilvNode *max;
    LilvNode *def;  // default value
    bool is_toggle; // 0 or 1
    bool is_integer;
    bool is_enumeration; // point values only
    bool is_logarithmic;
    bool is_writable;
    bool is_readable;
    bool is_sample_rate; // lv2:sampleRate - min/max are multiples of the rate
} PLUG_CONTROL;

// one property control's param, see PLUG_PLUG.property_params
typedef struct _plug_property_param {
    LV2_URID urid;
    int val_id;
} PLUG_PROPERTY_PARAM;

// how a plugin's UI is shown: embedded in a host window, or in a window of its
// own through ui:showInterface
typedef enum {
    PLUG_GUI_NONE = 0,
    PLUG_GUI_EMBED,
    PLUG_GUI_SHOW,
} PlugGuiMode;

// the features a UI is instantiated with, set at each show and valid until
// its cleanup()
#define PLUG_GUI_OWN_FEATURES 7
typedef struct _plug_gui_features {
    LV2UI_Resize resize;
    LV2UI_Port_Map port_map;
    LV2UI_Touch touch;
    LV2_Feature own[PLUG_GUI_OWN_FEATURES];
    // map, unmap, log, options and own, NULL terminated
    const LV2_Feature *list[PLUG_GUI_OWN_FEATURES + 5];
} PLUG_GUI_FEATURES;

// a record in a UI atom ring, the atom follows it
typedef struct _plug_gui_atom {
    uint32_t port;
    uint32_t size; // the atom's, its header included
} PLUG_GUI_ATOM;

typedef struct _plug_plug {
    // is the plugin processing, if is_processing == 0, it will be skipped in
    // the process function, touch only on [audio-thread]
    unsigned int is_processing;
    // the plugin data that is used by the instance
    const LilvPlugin *plug;
    // the activated plugin instance
    LilvInstance *plug_instance;
    // if the plugin instance is activated (if it is when cleaning needs to be
    // deactivated
    unsigned int plug_instance_activated;
    // the instance id to link it to cx (slot index, reused)
    int id;
    // monotonic per-module id, assigned at load, never reused. Identity for the
    // context/ui layers (see plug_plugin_uid).
    uint32_t uid;
    // display name, owned by this struct, built once at load by
    // plug_load_and_activate() and handed out by plug_plugin_name()
    char name[PLUG_PLUGIN_NAME_MAX];
    // name's number among the loaded ones of the same name, 1 = not shown,
    // and where the plain name starts in name
    uint32_t name_num;
    size_t name_at;
    // the plugin's node, its audio/MIDI ports are on it
    GRAPH_NODE *node;
    // the latency output control port's index, -1 when none
    int latency_port;
    // its value after each run(), written by [audio-thread] (by [main-thread]
    // only while the plugin is stopped). 0 until the first run
    atomic_uint latency_rt;
    // [main-thread] frames, as the node has it
    uint32_t latency;
    // [main-thread] latency changed, see plug_plugin_latency_changed
    bool latency_changed;
    // [main-thread] the plugin's UIs and the one picked at load, gui_ui NULL
    // when gui_mode is NONE
    LilvUIs *uis;
    const LilvUI *gui_ui;
    PlugGuiMode gui_mode;
    bool gui_shown;
    // [main-thread] gui_shown changed, see plug_plugin_gui_changed
    bool gui_changed;
    // [main-thread] the shown UI, gui_lib our reference from show to hide
    void *gui_lib;
    const LV2UI_Descriptor *gui_desc;
    LV2UI_Handle gui_inst;
    const LV2UI_Idle_Interface *gui_idle;
    // a UI of its own window only
    const LV2UI_Show_Interface *gui_show_if;
    // the UI's own resize, NULL when the user may not resize it
    const LV2UI_Resize *gui_user_resize;
    // an embedded UI's host window, NULL otherwise
    HWIN *gui_win;
    // the size the UI last asked for, 0 before it did
    uint32_t gui_w;
    uint32_t gui_h;
    PLUG_GUI_FEATURES gui_features;
    // [main-thread] the UI's atoms, made at the first show and kept until
    // removal. [audio-thread] reads ui_to_rt and writes rt_to_ui, gui_atom
    // holds one atom read from rt_to_ui
    ZixRing *gui_ui_to_rt;
    ZixRing *gui_rt_to_ui;
    LV2_Atom *gui_atom;
    // gui_shown for [audio-thread], stored after the rings
    atomic_bool gui_shown_rt;
    // atoms gui_rt_to_ui had no room for, taken on [main-thread]
    atomic_uint gui_dropped;
    // array of available ports for the plugin
    PLUG_PORT *ports;
    // how many ports do we have on plugin
    uint32_t num_ports;
    // index of the controling port
    int control_in;
    // uri to id and id to uri
    LV2_URID_Map map;
    LV2_URID_Unmap unmap;
    // features and feature list
    PLUG_FEATURES features;
    const LV2_Feature **feature_list;
    // for workers feature
    JalvWorker *worker;
    JalvWorker *state_worker;
    ZixSem work_lock;
    // state, preset safe restore flag
    bool safe_restore;
    // urids of the system, for convenience
    PLUG_URIDS *urids;
    // the current preset
    LilvState *preset;
    // the preset list, built on first browse (see plug_presets_get). A
    // preset's identity is its URI
    TREE_INDEX *presets;
    bool presets_built;
    // port symbol -> index in ports, for restoring a preset's port values
    INTERN_TABLE port_symbols;
    // [main-thread] the value plug_get_port_value last handed out
    float state_port_value;
    // property URID -> param, sorted by URID. Built at load, read by the
    // [audio-thread] for the plugin's patch:Set and patch:Put messages and by
    // [main-thread] for its UI's patch:Set
    PLUG_PROPERTY_PARAM *property_params;
    uint32_t num_property_params;
    // [main-thread] asks for a patch:Get, after a preset loading for example.
    // Taken by [audio-thread]
    atomic_bool request_update;
    // [audio-thread] the transport seq the plugin last got time:Position for
    // (0 = never) and the frame its own count expects next. [main-thread]
    // zeroes pos_seq only before the plugin is in the graph or while stopped
    uint64_t pos_seq;
    int64_t pos_frame_next;
    // atom forge for types and general convenience
    LV2_Atom_Forge forge;
    PLUG_CONTROL **controls;   // parameters (properties and port controls)
    unsigned int num_controls; // number of PLUG_CONTROL
    PRM_CONTAIN *plug_params;  // plugin parameter container for params.c
    PLUG_INFO *plug_data;      // plug_data struct for convenience
} PLUG_PLUG;

typedef struct _plug_info {
    // uri map
    Symap *symap;
    // symap lock
    ZixSem symap_lock;
    // urids
    PLUG_URIDS urids;
    // lilv world object
    LilvWorld *lv_world;
    // array of plugins. there can be gaps (a plugin removed from the middle);
    // plug_plugin_return() walks the occupied slots in order for the UI.
    struct _plug_plug plugins[MAX_INSTANCES];
    // did the plugins array change?
    bool plugins_dirty;
    // monotonic counter for PLUG_PLUG.uid, never reset
    uint32_t next_plug_uid;
    // sample_rate
    float sample_rate;
    // block_length
    uint32_t block_length;
    // midi buffer size
    size_t midi_buf_size;
    // buffer size for communication with the plugin
    uint32_t buffer_size;
    // node for convenience
    PLUG_NODES nodes;
    GRAPH *graph;
    // the clock time:Position comes from, read on [audio-thread]
    TRANSPORT *transport;
    // paired with a plugin's uid as its node's owner
    uint64_t owner_tag;
    // the plugin GUIs' host windows
    HWIN_SYS *hwin;
    // the available plugins list struct
    PLUGIN_LIST plugin_list;
    // this is control for [audio-thread] and [main-thread] sys communication
    // (wait for a plugin pause etc.)
    CXCONTROL *control_data;
    // rt tick var, that goes from 0 to RT_CYCLES, rt thread will send info to
    // ui thread only when rt tick var is 0 should only be touched on
    // [audio-thread]
    int rt_tick;
} PLUG_INFO;

static int plug_sys_send_msg(void *user_data, const char *msg) {
    PLUG_INFO *plug_data = (PLUG_INFO *)user_data;
    if (!plug_data)
        return -1;
    log_append_logfile("%s", msg);
    return 0;
}

// empty the preset list, the keys stay
static void plug_presets_clear(PLUG_PLUG *plug) {
    tree_index_reset(plug->presets);
    plug->presets_built = false;
}

// the instance's extension data access and its workers, started
static void plug_workers_start(PLUG_INFO *plug_data, PLUG_PLUG *plug) {
    if (lilv_plugin_has_extension_data(plug->plug,
                                       plug_data->nodes.work_interface)) {
        plug->worker = jalv_worker_new(&(plug->work_lock), true);
        plug->features.sched.handle = plug->worker;
        if (plug->safe_restore) {
            plug->state_worker = jalv_worker_new(&(plug->work_lock), false);
            plug->features.ssched.handle = plug->state_worker;
        }
    }
    plug->features.ext_data.data_access =
        lilv_instance_get_descriptor(plug->plug_instance)->extension_data;
    const LV2_Worker_Interface *worker_iface =
        (const LV2_Worker_Interface *)lilv_instance_get_extension_data(
            plug->plug_instance, LV2_WORKER__interface);
    jalv_worker_start(plug->worker, worker_iface,
                      plug->plug_instance->lv2_handle);
    jalv_worker_start(plug->state_worker, worker_iface,
                      plug->plug_instance->lv2_handle);
}

// the features restore() gets, NULL terminated. A threadSafeRestore plugin
// must get a work:schedule there - the state worker's, which runs the work at
// once. Never the RT worker's
#define PLUG_RESTORE_FEATURES_MAX 7
static void plug_restore_features(PLUG_PLUG *plug, const LV2_Feature **out) {
    size_t n = 0;
    out[n++] = &(plug->features.map_feature);
    out[n++] = &(plug->features.unmap_feature);
    out[n++] = &(plug->features.log_feature);
    out[n++] = &(plug->features.options_feature);
    if (plug->state_worker) {
        out[n++] = &(plug->features.state_sched_feature);
        out[n++] = &(plug->features.safe_restore_feature);
    }
    out[n] = NULL;
}

// before the instance goes - a worker thread may be inside its work()
static void plug_workers_free(PLUG_PLUG *plug) {
    jalv_worker_free(plug->worker);
    plug->worker = NULL;
    plug->features.sched.handle = NULL;
    jalv_worker_free(plug->state_worker);
    plug->state_worker = NULL;
    plug->features.ssched.handle = NULL;
}

static void plug_gui_destroy(PLUG_PLUG *plug, bool shown);
static void plug_gui_rings_free(PLUG_PLUG *plug);
static int plug_gui_hide(PLUG_PLUG *plug);
static void plug_gui_ports_update(PLUG_PLUG *plug, bool all);
static void plug_gui_atoms_out(PLUG_PLUG *plug);

static int plug_remove_plug(PLUG_INFO *plug_data, int id) {
    if (!plug_data)
        return -1;
    if (id >= MAX_INSTANCES || id < 0)
        return -1;

    PLUG_PLUG *cur_plug = &(plug_data->plugins[id]);
    // the UI before the instance it may access
    plug_gui_destroy(cur_plug, cur_plug->gui_shown);
    plug_gui_rings_free(cur_plug);
    // remove preset
    if (cur_plug->preset) {
        const LilvNode *old_preset = lilv_state_get_uri(cur_plug->preset);
        if (old_preset) {
            lilv_world_unload_resource(plug_data->lv_world, old_preset);
        }
        lilv_state_free(cur_plug->preset);
        cur_plug->preset = NULL;
    }
    plug_presets_clear(cur_plug);
    tree_index_free(cur_plug->presets);
    cur_plug->presets = NULL;
    intern_clean(&cur_plug->port_symbols);
    free(cur_plug->property_params);
    cur_plug->property_params = NULL;
    cur_plug->num_property_params = 0;
    plug_workers_free(cur_plug);
    // free instance
    if (cur_plug->plug_instance) {
        LilvInstance *cur_instance = cur_plug->plug_instance;
        if (cur_plug->plug_instance_activated == 1) {
            lilv_instance_deactivate(cur_instance);
            cur_plug->plug_instance_activated = 0;
        }
        lilv_instance_free(cur_instance);
        cur_plug->plug_instance = NULL;
    }

    // clean the ports, the graph ones go with the node
    graph_node_remove(plug_data->graph, cur_plug->node);
    cur_plug->node = NULL;
    if (cur_plug->ports) {
        for (uint32_t i = 0; i < cur_plug->num_ports; i++)
            free(cur_plug->ports[i].evbuf);
    }
    if (cur_plug->ports)
        free(cur_plug->ports);
    cur_plug->ports = NULL;
    cur_plug->num_ports = 0;

    zix_sem_destroy(&(cur_plug->work_lock));

    // free the features_list
    if (cur_plug->feature_list)
        free(cur_plug->feature_list);
    cur_plug->feature_list = NULL;

    // clean the controls
    if (cur_plug->plug_params)
        param_clean_param_container(cur_plug->plug_params);
    cur_plug->plug_params = NULL;
    if (cur_plug->controls) {
        for (unsigned int i = 0; i < cur_plug->num_controls; i++) {
            PLUG_CONTROL *const control = cur_plug->controls[i];
            lilv_node_free(control->node);
            lilv_node_free(control->symbol);
            lilv_node_free(control->label);
            lilv_node_free(control->group);
            lilv_node_free(control->min);
            lilv_node_free(control->max);
            lilv_node_free(control->def);
            if (control->points) {
                for (unsigned int pt_iter = 0; pt_iter < control->n_points;
                     pt_iter++) {
                    ScalePoint pt = control->points[pt_iter];
                    if (pt.label)
                        free(pt.label);
                }
                free(control->points);
            }
            free(control);
        }
        free(cur_plug->controls);
    }
    cur_plug->controls = NULL;
    cur_plug->num_controls = 0;
    cur_plug->control_in = -1;
    cur_plug->latency_port = -1;
    atomic_store(&cur_plug->latency_rt, 0U);
    cur_plug->latency = 0;
    cur_plug->latency_changed = false;
    if (cur_plug->uis)
        lilv_uis_free(cur_plug->uis);
    cur_plug->uis = NULL;
    cur_plug->gui_ui = NULL;
    cur_plug->gui_mode = PLUG_GUI_NONE;
    cur_plug->gui_shown = false;
    cur_plug->gui_changed = false;
    atomic_store(&cur_plug->gui_shown_rt, false);
    atomic_store(&cur_plug->gui_dropped, 0U);
    atomic_store(&cur_plug->request_update, false);
    cur_plug->safe_restore = false;
    cur_plug->plug = NULL;
    cur_plug->plug_data = NULL;
    plug_data->plugins_dirty = true;

    return 0;
}

// internal functions to manipulate evbuf buffer for midi and other events
static PLUG_EVBUF *plug_evbuf_new(uint32_t capacity, uint32_t atom_Chunk,
                                  uint32_t atom_Sequence) {
    const size_t buffer_size =
        sizeof(PLUG_EVBUF) + sizeof(LV2_Atom_Sequence) + capacity;
    PLUG_EVBUF *evbuf = (PLUG_EVBUF *)malloc(buffer_size);
    if (evbuf) {
        memset(evbuf, 0, sizeof(*evbuf));
        evbuf->capacity = capacity;
        evbuf->atom_Chunk = atom_Chunk;
        evbuf->atom_Sequence = atom_Sequence;
    }
    return evbuf;
}

static void plug_evbuf_reset(PLUG_EVBUF *evbuf, unsigned int is_input) {
    if (is_input == 1) {
        evbuf->buf.atom.size = sizeof(LV2_Atom_Sequence_Body);
        evbuf->buf.atom.type = evbuf->atom_Sequence;
    } else {
        evbuf->buf.atom.size = evbuf->capacity;
        evbuf->buf.atom.type = evbuf->atom_Chunk;
    }
}

static void *plug_evbuf_get_buffer(PLUG_EVBUF *evbuf) { return &(evbuf->buf); }

static PLUG_EVBUF_ITERATOR plug_evbuf_begin(PLUG_EVBUF *evbuf) {
    PLUG_EVBUF_ITERATOR iter = {evbuf, 0};
    return iter;
}

static uint32_t plug_evbuf_get_size(PLUG_EVBUF *evbuf) {
    assert(evbuf->buf.atom.type != evbuf->atom_Sequence ||
           evbuf->buf.atom.size >= sizeof(LV2_Atom_Sequence_Body));
    return evbuf->buf.atom.type == evbuf->atom_Sequence
               ? evbuf->buf.atom.size - sizeof(LV2_Atom_Sequence_Body)
               : 0;
}

static bool plug_evbuf_is_valid(PLUG_EVBUF_ITERATOR iter) {
    return iter.offset < plug_evbuf_get_size(iter.evbuf);
}

static PLUG_EVBUF_ITERATOR plug_evbuf_end(PLUG_EVBUF *evbuf) {
    const uint32_t size = plug_evbuf_get_size(evbuf);
    const PLUG_EVBUF_ITERATOR iter = {evbuf, lv2_atom_pad_size(size)};
    return iter;
}

static PLUG_EVBUF_ITERATOR plug_evbuf_next(const PLUG_EVBUF_ITERATOR iter) {
    if (!plug_evbuf_is_valid(iter)) {
        return iter;
    }

    LV2_Atom_Sequence *aseq = &iter.evbuf->buf;
    LV2_Atom_Event *aev =
        (LV2_Atom_Event *)((char *)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, aseq) +
                           iter.offset);
    const uint32_t offset =
        iter.offset +
        lv2_atom_pad_size(sizeof(LV2_Atom_Event) + aev->body.size);
    PLUG_EVBUF_ITERATOR next = {iter.evbuf, offset};
    return next;
}

static bool plug_evbuf_get(PLUG_EVBUF_ITERATOR iter, uint32_t *frames,
                           uint32_t *subframes, uint32_t *type, uint32_t *size,
                           void **data) {
    *frames = *subframes = *type = *size = 0;
    *data = NULL;

    if (!plug_evbuf_is_valid(iter)) {
        return false;
    }

    LV2_Atom_Sequence *aseq = &iter.evbuf->buf;
    LV2_Atom_Event *aev =
        (LV2_Atom_Event *)((char *)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, aseq) +
                           iter.offset);
    *frames = aev->time.frames;
    *subframes = 0;
    *type = aev->body.type;
    *size = aev->body.size;
    *data = LV2_ATOM_BODY(&aev->body);

    return true;
}

// the padded event an atom of atom_size bytes, its header included, takes in a
// sequence
static uint32_t plug_evbuf_event_size(uint32_t atom_size) {
    return lv2_atom_pad_size(sizeof(LV2_Atom_Event) - sizeof(LV2_Atom) +
                             atom_size);
}

// an event for an atom of atom_size bytes, its header included, at the
// iterator, the atom left for the caller to fill. NULL when there is no room
static LV2_Atom *plug_evbuf_append(PLUG_EVBUF_ITERATOR *iter, uint32_t frames,
                                   uint32_t atom_size) {
    LV2_Atom_Sequence *aseq = &iter->evbuf->buf;
    const uint32_t size = plug_evbuf_event_size(atom_size);
    if (iter->evbuf->capacity - sizeof(LV2_Atom) - aseq->atom.size < size)
        return NULL;

    LV2_Atom_Event *aev =
        (LV2_Atom_Event *)((char *)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, aseq) +
                           iter->offset);
    aev->time.frames = frames;
    aseq->atom.size += size;
    iter->offset += size;
    return &aev->body;
}

// does an atom of atom_size bytes, its header included, fit the empty input
// sequence. The capacity never changes, any thread may ask
static bool plug_evbuf_fits(const PLUG_EVBUF *evbuf, uint32_t atom_size) {
    return atom_size < evbuf->capacity &&
           plug_evbuf_event_size(atom_size) <=
               evbuf->capacity - sizeof(LV2_Atom) -
                   sizeof(LV2_Atom_Sequence_Body);
}

static int plug_evbuf_write(PLUG_EVBUF_ITERATOR *iter, uint32_t frames,
                            uint32_t subframes, uint32_t type, uint32_t size,
                            const void *data) {
    (void)subframes;
    LV2_Atom *atom = plug_evbuf_append(iter, frames, sizeof(LV2_Atom) + size);
    if (!atom)
        return -1;
    atom->type = type;
    atom->size = size;
    memcpy(LV2_ATOM_BODY(atom), data, size);
    return 0;
}

// internal function to initialize the nodes
static void plug_init_nodes(LilvWorld *const world, PLUG_NODES *const nodes) {
    nodes->atom_AtomPort = lilv_new_uri(world, LV2_ATOM__AtomPort);
    nodes->atom_Chunk = lilv_new_uri(world, LV2_ATOM__Chunk);
    nodes->atom_Float = lilv_new_uri(world, LV2_ATOM__Float);
    nodes->atom_Path = lilv_new_uri(world, LV2_ATOM__Path);
    nodes->atom_Sequence = lilv_new_uri(world, LV2_ATOM__Sequence);
    nodes->lv2_AudioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
    nodes->lv2_CVPort = lilv_new_uri(world, LV2_CORE__CVPort);
    nodes->lv2_ControlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
    nodes->lv2_InputPort = lilv_new_uri(world, LV2_CORE__InputPort);
    nodes->lv2_OutputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
    nodes->lv2_connectionOptional =
        lilv_new_uri(world, LV2_CORE__connectionOptional);
    nodes->lv2_control = lilv_new_uri(world, LV2_CORE__control);
    nodes->lv2_default = lilv_new_uri(world, LV2_CORE__default);
    nodes->lv2_enumeration = lilv_new_uri(world, LV2_CORE__enumeration);
    nodes->lv2_extensionData = lilv_new_uri(world, LV2_CORE__extensionData);
    nodes->lv2_integer = lilv_new_uri(world, LV2_CORE__integer);
    nodes->lv2_maximum = lilv_new_uri(world, LV2_CORE__maximum);
    nodes->lv2_minimum = lilv_new_uri(world, LV2_CORE__minimum);
    nodes->lv2_name = lilv_new_uri(world, LV2_CORE__name);
    nodes->lv2_optionalFeature = lilv_new_uri(world, LV2_CORE__optionalFeature);
    nodes->lv2_requiredFeature = lilv_new_uri(world, LV2_CORE__requiredFeature);
    nodes->lv2_sampleRate = lilv_new_uri(world, LV2_CORE__sampleRate);
    nodes->lv2_symbol = lilv_new_uri(world, LV2_CORE__symbol);
    nodes->lv2_toggled = lilv_new_uri(world, LV2_CORE__toggled);
    nodes->midi_MidiEvent = lilv_new_uri(world, LV2_MIDI__MidiEvent);
    nodes->pg_group = lilv_new_uri(world, LV2_PORT_GROUPS__group);
    nodes->pprops_logarithmic =
        lilv_new_uri(world, LV2_PORT_PROPS__logarithmic);
    nodes->pprops_notOnGUI = lilv_new_uri(world, LV2_PORT_PROPS__notOnGUI);
    nodes->pprops_rangeSteps = lilv_new_uri(world, LV2_PORT_PROPS__rangeSteps);
    nodes->pset_Preset = lilv_new_uri(world, LV2_PRESETS__Preset);
    nodes->pset_bank = lilv_new_uri(world, LV2_PRESETS__bank);
    nodes->rdfs_comment = lilv_new_uri(world, LILV_NS_RDFS "comment");
    nodes->rdfs_label = lilv_new_uri(world, LILV_NS_RDFS "label");
    nodes->rdfs_range = lilv_new_uri(world, LILV_NS_RDFS "range");
    nodes->rsz_minimumSize = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
    nodes->ui_fixedSize = lilv_new_uri(world, LV2_UI__fixedSize);
    nodes->ui_noUserResize = lilv_new_uri(world, LV2_UI__noUserResize);
    nodes->ui_showInterface = lilv_new_uri(world, LV2_UI__showInterface);
    nodes->ui_X11UI = lilv_new_uri(world, LV2_UI__X11UI);
    nodes->work_interface = lilv_new_uri(world, LV2_WORKER__interface);
    nodes->work_schedule = lilv_new_uri(world, LV2_WORKER__schedule);
    nodes->end = NULL;
}
// internal function to initialize urids
static void plug_init_urids(Symap *const symap, PLUG_URIDS *const urids) {
    urids->atom_Float = symap_map(symap, LV2_ATOM__Float);
    urids->atom_Int = symap_map(symap, LV2_ATOM__Int);
    urids->atom_Object = symap_map(symap, LV2_ATOM__Object);
    urids->atom_Path = symap_map(symap, LV2_ATOM__Path);
    urids->atom_String = symap_map(symap, LV2_ATOM__String);
    urids->atom_eventTransfer = symap_map(symap, LV2_ATOM__eventTransfer);
    urids->bufsz_maxBlockLength =
        symap_map(symap, LV2_BUF_SIZE__maxBlockLength);
    urids->bufsz_minBlockLength =
        symap_map(symap, LV2_BUF_SIZE__minBlockLength);
    urids->bufsz_sequenceSize = symap_map(symap, LV2_BUF_SIZE__sequenceSize);
    urids->log_Error = symap_map(symap, LV2_LOG__Error);
    urids->log_Trace = symap_map(symap, LV2_LOG__Trace);
    urids->log_Warning = symap_map(symap, LV2_LOG__Warning);
    urids->midi_MidiEvent = symap_map(symap, LV2_MIDI__MidiEvent);
    urids->param_sampleRate = symap_map(symap, LV2_PARAMETERS__sampleRate);
    urids->patch_Get = symap_map(symap, LV2_PATCH__Get);
    urids->patch_Put = symap_map(symap, LV2_PATCH__Put);
    urids->patch_Set = symap_map(symap, LV2_PATCH__Set);
    urids->patch_body = symap_map(symap, LV2_PATCH__body);
    urids->patch_property = symap_map(symap, LV2_PATCH__property);
    urids->patch_value = symap_map(symap, LV2_PATCH__value);
    urids->time_Position = symap_map(symap, LV2_TIME__Position);
    urids->time_bar = symap_map(symap, LV2_TIME__bar);
    urids->time_barBeat = symap_map(symap, LV2_TIME__barBeat);
    urids->time_beatUnit = symap_map(symap, LV2_TIME__beatUnit);
    urids->time_beatsPerBar = symap_map(symap, LV2_TIME__beatsPerBar);
    urids->time_beatsPerMinute = symap_map(symap, LV2_TIME__beatsPerMinute);
    urids->time_frame = symap_map(symap, LV2_TIME__frame);
    urids->time_speed = symap_map(symap, LV2_TIME__speed);
    urids->ui_scaleFactor = symap_map(symap, LV2_UI__scaleFactor);
    urids->ui_updateRate = symap_map(symap, LV2_UI__updateRate);
}
// internal function to map uri to id, this will be used in a feature sent to
// the plugin
static LV2_URID map_uri(LV2_URID_Map_Handle handle, const char *uri) {
    PLUG_INFO *plug_data = (PLUG_INFO *)handle;
    zix_sem_wait(&(plug_data->symap_lock));
    const LV2_URID id = symap_map(plug_data->symap, uri);
    zix_sem_post(&(plug_data->symap_lock));
    return id;
}
// internal function to map id to uri, this will be used in a feature sent to
// the plugin
static const char *unmap_uri(LV2_URID_Unmap_Handle handle, LV2_URID urid) {
    PLUG_INFO *plug_data = (PLUG_INFO *)handle;
    zix_sem_wait(&(plug_data->symap_lock));
    const char *uri = symap_unmap(plug_data->symap, urid);
    zix_sem_post(&(plug_data->symap_lock));
    return uri;
}

// internal function for printf for the log feature
static int plug_vprintf(LV2_Log_Handle handle, LV2_URID type, const char *fmt,
                        va_list ap) {
    (void)type;
    PLUG_INFO *plug_data = (PLUG_INFO *)handle;
    int ret = -1;
    char send_msg[MAX_STRING_MSG_LENGTH];
    ret = vsnprintf(send_msg, MAX_STRING_MSG_LENGTH, fmt, ap);
    context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                         is_audio_thread, "%s", send_msg);
    return ret;
}
static int plug_printf(LV2_Log_Handle handle, LV2_URID type, const char *fmt,
                       ...) {
    int ret = -1;
    va_list args;
    va_start(args, fmt);
    ret = plug_vprintf(handle, type, fmt, args);
    va_end(args);
    return ret;
}

static int plug_plug_start_process(void *user_data) {
    PLUG_PLUG *plug = (PLUG_PLUG *)user_data;
    if (!plug->plug_instance)
        return -1;

    plug->is_processing = 1;
    return 0;
}
static int plug_plug_stop_process(void *user_data) {
    PLUG_PLUG *plug = (PLUG_PLUG *)user_data;
    if (!plug)
        return -1;
    // TODO before stopping would be nice to fadeout audio
    plug->is_processing = 0;
    return 0;
}

PLUG_INFO *plug_init(uint32_t block_length, SAMPLE_T samplerate,
                     plug_status_t *plug_errors, GRAPH *graph,
                     TRANSPORT *transport, HWIN_SYS *hwin,
                     uint64_t owner_tag) {
    PLUG_INFO *plug_data = (PLUG_INFO *)malloc(sizeof(PLUG_INFO));
    if (!plug_data) {
        *plug_errors = plug_failed_malloc;
        return NULL;
    }
    memset(plug_data, '\0', sizeof(*plug_data));
    // init the control_data struct for [audio-thread] to [main-thread]
    // messaging
    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = plug_plug_start_process;
    rt_funcs_struct.subcx_stop_process = plug_plug_stop_process;
    ui_funcs_struct.send_msg = plug_sys_send_msg;
    plug_data->control_data =
        context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if (!plug_data->control_data) {
        free(plug_data);
        *plug_errors = plug_failed_malloc;
        return NULL;
    }

    plug_data->rt_tick = 0;
    plug_data->lv_world = lilv_world_new();
    plug_data->sample_rate = (float)samplerate;
    plug_data->block_length = (uint32_t)block_length;
    plug_data->midi_buf_size = (size_t)DEFAULT_MIDI_BUF_SIZE;
    plug_data->buffer_size = plug_data->midi_buf_size * N_BUFFER_CYCLES;
    plug_data->plugin_list.plugin_list_count = 0;
    plug_data->plugin_list.plugin_list = NULL;

    plug_data->symap = symap_new();
    zix_sem_init(&(plug_data->symap_lock), 1);
    plug_init_urids(plug_data->symap, &(plug_data->urids));
    plug_init_nodes(plug_data->lv_world, &(plug_data->nodes));

    plug_data->graph = graph;
    plug_data->transport = transport;
    plug_data->hwin = hwin;
    plug_data->owner_tag = owner_tag;

    // init the plugin instances to shell
    plug_data->plugins_dirty = false;
    plug_data->next_plug_uid = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        PLUG_PLUG *plug = &(plug_data->plugins[i]);
        plug->is_processing = 0;
        plug->plug = NULL;
        plug->plug_instance = NULL;
        plug->plug_instance_activated = 0;
        plug->num_ports = 0;
        plug->feature_list = NULL;
        plug->worker = NULL;
        plug->state_worker = NULL;
        plug->preset = NULL;
        plug->controls = NULL;
        plug->num_controls = 0;
        atomic_init(&plug->request_update, false);
        plug->control_in = -1;
        plug->latency_port = -1;
        atomic_init(&plug->latency_rt, 0U);
        plug->latency = 0;
        plug->latency_changed = false;
        plug->uis = NULL;
        plug->gui_ui = NULL;
        plug->gui_mode = PLUG_GUI_NONE;
        plug->gui_shown = false;
        plug->gui_changed = false;
        plug->gui_ui_to_rt = NULL;
        plug->gui_rt_to_ui = NULL;
        plug->gui_atom = NULL;
        atomic_init(&plug->gui_shown_rt, false);
        atomic_init(&plug->gui_dropped, 0U);
        plug->plug_params = NULL;
        plug->safe_restore = false;
        plug->id = i;
        plug->plug_data = NULL;

        // initialize the plug urids, they are the same as plug_data, used for
        // convenience
        plug->urids = &(plug_data->urids);
        // init the features
        // log feature
        plug->features.log.handle = plug_data;
        plug->features.log.printf = plug_printf;
        plug->features.log.vprintf = plug_vprintf;
        plug->features.log_feature.URI = LV2_LOG__log;
        plug->features.log_feature.data = &(plug->features.log);
        // state:threadSafeRestore
        plug->features.safe_restore_feature.URI = LV2_STATE__threadSafeRestore;
        plug->features.safe_restore_feature.data = NULL;
        // map to id feature
        plug->map.handle = plug_data;
        plug->map.map = map_uri;
        plug->features.map_feature.URI = LV2_URID__map;
        plug->features.map_feature.data = &(plug->map);
        // map id to uri feature
        plug->unmap.handle = plug_data;
        plug->unmap.unmap = unmap_uri;
        plug->features.unmap_feature.URI = LV2_URID__unmap;
        plug->features.unmap_feature.data = &(plug->unmap);
        // worker features
        plug->features.sched.schedule_work = jalv_worker_schedule;
        plug->features.sched_feature.URI = LV2_WORKER__schedule;
        plug->features.sched_feature.data = &(plug->features.sched);
        plug->features.ssched.schedule_work = jalv_worker_schedule;
        plug->features.state_sched_feature.URI = LV2_WORKER__schedule;
        plug->features.state_sched_feature.data = &(plug->features.ssched);
        // init options features
        const LV2_Options_Option options[(sizeof(plug->features.options)) /
                                         (sizeof(plug->features.options[0]))] =
            {{LV2_OPTIONS_INSTANCE, 0, plug_data->urids.param_sampleRate,
              sizeof(float), plug_data->urids.atom_Float,
              &(plug_data->sample_rate)},

             {LV2_OPTIONS_INSTANCE, 0, plug_data->urids.bufsz_minBlockLength,
              sizeof(int32_t), plug_data->urids.atom_Int,
              &(plug_data->block_length)},

             {LV2_OPTIONS_INSTANCE, 0, plug_data->urids.bufsz_maxBlockLength,
              sizeof(int32_t), plug_data->urids.atom_Int,
              &(plug_data->block_length)},

             {LV2_OPTIONS_INSTANCE, 0, plug_data->urids.bufsz_sequenceSize,
              sizeof(int32_t), plug_data->urids.atom_Int,
              &(plug_data->midi_buf_size)},

             {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL}};
        memcpy(plug->features.options, options, sizeof(plug->features.options));
        plug->features.options_feature.URI = LV2_OPTIONS__options;
        plug->features.options_feature.data = (void *)plug->features.options;

        // init atom forge
        lv2_atom_forge_init(&(plug->forge), &(plug->map));
    }

    return plug_data;
}

int plug_plugin_list_init(PLUG_INFO *plug_data) {
    if (!plug_data)
        return -1;
    PLUGIN_LIST *return_plugin_list = &(plug_data->plugin_list);
    if (return_plugin_list->plugin_list)
        free(return_plugin_list->plugin_list);
    return_plugin_list->plugin_list = NULL;
    return_plugin_list->plugin_list_count = 0;
    intern_detach_all(&return_plugin_list->uris);

    // picks up newly installed bundles, the plugins already known keep their
    // LilvPlugin, so loaded instances are not disturbed
    lilv_world_load_all(plug_data->lv_world);
    // a new bundle can hold presets for a loaded plugin
    for (int i = 0; i < MAX_INSTANCES; i++)
        plug_presets_clear(&plug_data->plugins[i]);
    const LilvPlugins *plugins =
        lilv_world_get_all_plugins(plug_data->lv_world);
    if (!plugins)
        return -1;
    LilvIter *plug_iter = lilv_plugins_begin(plugins);
    unsigned int plugins_list_size = lilv_plugins_size(plugins);
    if (plugins_list_size <= 0)
        return -1;
    return_plugin_list->plugin_list =
        (PLUGIN_LIST_ITEM *)calloc(plugins_list_size, sizeof(PLUGIN_LIST_ITEM));
    if (!return_plugin_list->plugin_list)
        return -1;

    unsigned int count = 0;
    for (; !lilv_plugins_is_end(plugins, plug_iter);
         plug_iter = lilv_plugins_next(plugins, plug_iter)) {
        const LilvPlugin *cur_plug = lilv_plugins_get(plugins, plug_iter);
        const char *path_string =
            lilv_node_as_string(lilv_plugin_get_uri(cur_plug));
        uint64_t key = intern_add(&return_plugin_list->uris, path_string);
        if (!key)
            continue;
        PLUGIN_LIST_ITEM *list_item = &return_plugin_list->plugin_list[count];
        LilvNode *cur_name = lilv_plugin_get_name(cur_plug);
        snprintf(list_item->plugin_path, MAX_PATH_STRING, "%s", path_string);
        snprintf(list_item->plugin_short_name, MAX_SHORT_NAME_LENGTH, "%s",
                 cur_name ? lilv_node_as_string(cur_name) : path_string);
        lilv_node_free(cur_name);
        list_item->key = key;
        list_item->plug_data = plug_data;
        intern_set_slot(&return_plugin_list->uris, key, count);
        count += 1;
    }
    return_plugin_list->plugin_list_count = count;
    return 1;
}

unsigned int plug_plugin_list_count(PLUG_INFO *plug_data) {
    if (!plug_data)
        return 0;
    return plug_data->plugin_list.plugin_list_count;
}

void *plug_plugin_list_item_get(PLUG_INFO *plug_data, unsigned int idx) {
    if (!plug_data)
        return NULL;
    PLUGIN_LIST cur_plugin_list = plug_data->plugin_list;
    if (idx >= cur_plugin_list.plugin_list_count)
        return NULL;
    PLUGIN_LIST_ITEM *cur_plug_list_item = &(cur_plugin_list.plugin_list[idx]);
    return (void *)cur_plug_list_item;
}

const char *plug_plugin_list_item_name(void *plug_list_item) {
    PLUGIN_LIST_ITEM *cur_plug_list_item = (PLUGIN_LIST_ITEM *)plug_list_item;
    if (!cur_plug_list_item)
        return NULL;
    return cur_plug_list_item->plugin_short_name;
}

uint64_t plug_plugin_list_item_key(void *plug_list_item) {
    PLUGIN_LIST_ITEM *cur_plug_list_item = (PLUGIN_LIST_ITEM *)plug_list_item;
    if (!cur_plug_list_item)
        return 0;
    return cur_plug_list_item->key;
}

void *plug_plugin_list_item_by_key(PLUG_INFO *plug_data, uint64_t key) {
    if (!plug_data)
        return NULL;
    PLUGIN_LIST *list = &plug_data->plugin_list;
    size_t slot = intern_slot(&list->uris, key);
    if (slot >= list->plugin_list_count)
        return NULL;
    return &list->plugin_list[slot];
}

// name for a preset without an rdfs:label: the last part of its URI
static const char *plug_preset_uri_tail(const char *uri) {
    const char *tail = uri;
    for (const char *c = uri; *c; c++)
        if ((*c == '/' || *c == '#') && c[1])
            tail = c + 1;
    return tail;
}

// add one preset to the preset list. Its own file is only read when the
// manifest gives it no rdfs:label, so a pset:bank that is only in that file is
// only seen then
static void plug_presets_add(PLUG_PLUG *plug, const LilvNode *preset) {
    LilvWorld *world = plug->plug_data->lv_world;
    PLUG_NODES *nodes = &plug->plug_data->nodes;
    const char *uri = lilv_node_as_uri(preset);
    if (!uri)
        return;
    LilvNode *label = lilv_world_get(world, preset, nodes->rdfs_label, NULL);
    bool loaded = false;
    if (!label) {
        loaded = lilv_world_load_resource(world, preset) > 0;
        label = lilv_world_get(world, preset, nodes->rdfs_label, NULL);
    }
    uint64_t bank_key = 0;
    LilvNode *bank = lilv_world_get(world, preset, nodes->pset_bank, NULL);
    if (bank) {
        LilvNode *bank_label =
            lilv_world_get(world, bank, nodes->rdfs_label, NULL);
        bank_key = tree_index_branch(
            plug->presets, 0,
            lilv_node_as_string(bank_label ? bank_label : bank));
        lilv_node_free(bank_label);
        lilv_node_free(bank);
    }
    // some plugins declare an empty label
    const char *name = label ? lilv_node_as_string(label) : NULL;
    if (!name || name[0] == '\0')
        name = plug_preset_uri_tail(uri);
    tree_index_leaf(plug->presets, bank_key, name, uri);
    lilv_node_free(label);
    // the names are copied, the file is not needed until the preset is loaded
    if (loaded)
        lilv_world_unload_resource(world, preset);
}

static void plug_presets_build(PLUG_PLUG *plug) {
    plug_presets_clear(plug);
    // also when there are none, so they are not looked for on every browse
    plug->presets_built = true;
    LilvNodes *presets =
        lilv_plugin_get_related(plug->plug, plug->plug_data->nodes.pset_Preset);
    if (presets) {
        LILV_FOREACH(nodes, i, presets) {
            plug_presets_add(plug, lilv_nodes_get(presets, i));
        }
        lilv_nodes_free(presets);
    }
    tree_index_finish(plug->presets);
}

// the preset list, read the first time it is asked for. NULL (no presets)
// when the index cannot be allocated - the next browse tries again
static TREE_INDEX *plug_presets_get(PLUG_PLUG *plug) {
    if (!plug || !plug->plug || !plug->plug_data)
        return NULL;
    if (!plug->presets) {
        plug->presets = tree_index_new();
        if (!plug->presets)
            return NULL;
    }
    if (!plug->presets_built)
        plug_presets_build(plug);
    return plug->presets;
}

size_t plug_plugin_presets_level_count(void *plug, uint64_t branch) {
    return tree_index_level_count(plug_presets_get(plug), branch);
}

bool plug_plugin_presets_level_at(void *plug, uint64_t branch, size_t idx,
                                  TREE_ROW *out) {
    return tree_index_level_at(plug_presets_get(plug), branch, idx, out);
}

static PLUG_PORT *plug_find_port_by_name(PLUG_PLUG *plug, const char *name) {
    if (!plug || !name)
        return NULL;
    size_t slot = intern_slot(&plug->port_symbols,
                              intern_find(&plug->port_symbols, name));
    if (slot >= plug->num_ports)
        return NULL;
    return &plug->ports[slot];
}

// a numeric LV2 value as a float, false for any other type. rt safe
static bool plug_value_as_float(const PLUG_PLUG *plug, const void *value,
                                uint32_t size, uint32_t type, float *out) {
    const LV2_Atom_Forge *forge = &plug->forge;
    if (type == forge->Float && size >= sizeof(float))
        *out = *(const float *)value;
    else if (type == forge->Double && size >= sizeof(double))
        *out = (float)*(const double *)value;
    else if (type == forge->Int && size >= sizeof(int32_t))
        *out = (float)*(const int32_t *)value;
    else if (type == forge->Long && size >= sizeof(int64_t))
        *out = (float)*(const int64_t *)value;
    else if (type == forge->Bool && size >= sizeof(int32_t))
        *out = *(const int32_t *)value ? 1.0f : 0.0f;
    else
        return false;
    return true;
}

static void plug_set_value_direct(const char *port_symbol, void *data,
                                  const void *value, uint32_t size,
                                  uint32_t type) {
    PLUG_PLUG *plug = (PLUG_PLUG *)data;
    if (!plug)
        return;
    PLUG_PORT *port = plug_find_port_by_name(plug, port_symbol);
    float fvalue;
    if (port && plug_value_as_float(plug, value, size, type, &fvalue))
        port->control = fvalue;
}

// LilvGetPortValueFunc - a control input's value, for a state. Its param's,
// [audio-thread] writes port->control
static const void *plug_get_port_value(const char *port_symbol,
                                       void *user_data, uint32_t *size,
                                       uint32_t *type) {
    PLUG_PLUG *plug = (PLUG_PLUG *)user_data;
    PLUG_PORT *port = plug_find_port_by_name(plug, port_symbol);
    if (!port || port->type != PORT_TYPE_CONTROL ||
        port->flow != PORT_FLOW_INPUT) {
        *size = 0;
        *type = 0;
        return NULL;
    }
    // without a param only [main-thread] writes it, while the plugin is stopped
    plug->state_port_value =
        port->param_index < 0 || !plug->plug_params
            ? port->control
            : (float)param_get_value(plug->plug_params, port->param_index, 0);
    *size = sizeof(float);
    *type = plug->forge.Float;
    // lilv copies it before the next call
    return &(plug->state_port_value);
}

int plug_plugin_preset_load(void *plug, uint64_t key) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug || !cur_plug->plug_instance || !cur_plug->plug_data)
        return -1;
    const char *preset_uri = tree_index_leaf_identity(cur_plug->presets, key);
    if (!preset_uri)
        return -2;
    PLUG_INFO *plug_data = cur_plug->plug_data;
    LilvWorld *world = plug_data->lv_world;

    // read the preset before pausing the plugin, so the pause only covers the
    // restore
    LilvNode *uri = lilv_new_uri(world, preset_uri);
    if (!uri)
        return -1;
    LilvState *state = NULL;
    if (lilv_world_load_resource(world, uri) >= 0)
        state = lilv_state_new_from_world(world, &(cur_plug->map), uri);
    if (!state) {
        lilv_world_unload_resource(world, uri);
        lilv_node_free(uri);
        return -1;
    }
    lilv_node_free(uri);

    context_sub_wait_for_stop(plug_data->control_data, (void *)cur_plug);
    // TODO a plugin with safe_restore would not need the pause
    const LV2_Feature *features[PLUG_RESTORE_FEATURES_MAX];
    plug_restore_features(cur_plug, features);
    lilv_state_restore(state, cur_plug->plug_instance, plug_set_value_direct,
                       cur_plug, 0, features);
    // the preset set the control ports directly - sync the params to them,
    // as values the plugin already has, so they are not sent back
    for (uint32_t i = 0; i < cur_plug->num_ports; i++) {
        PLUG_PORT *const cur_port = &(cur_plug->ports[i]);
        if (cur_port->type != PORT_TYPE_CONTROL || cur_port->param_index < 0)
            continue;
        param_set_value(cur_plug->plug_params, cur_port->param_index,
                        (PARAM_T)cur_port->control, NULL, Operation_SyncValue);
    }
    atomic_store(&cur_plug->request_update, true);
    context_sub_wait_for_start(plug_data->control_data, (void *)cur_plug);

    // the current preset's data stays loaded in the world while it is current
    if (cur_plug->preset) {
        const LilvNode *old_uri = lilv_state_get_uri(cur_plug->preset);
        if (old_uri && !lilv_node_equals(old_uri, lilv_state_get_uri(state)))
            lilv_world_unload_resource(world, old_uri);
        lilv_state_free(cur_plug->preset);
    }
    cur_plug->preset = state;
    return 0;
}

// a saved preset's bundle in PLUG_PRESET_DIR, "<plugin>_<name>.preset.lv2/" -
// a bundle URI ends in '/' - and its file in there. false when they do not fit
static bool plug_preset_paths(const PLUG_PLUG *plug, const char *name,
                              char bundle[MAX_PATH_STRING],
                              char file[MAX_PATH_STRING]) {
    const char *plug_name = plug->name + plug->name_at;
    if (plug_name[0] == '\0')
        plug_name = plug_preset_uri_tail(
            lilv_node_as_uri(lilv_plugin_get_uri(plug->plug)));
    char dir[MAX_PATH_STRING];
    char plug_safe[MAX_PATH_STRING];
    char name_safe[MAX_PATH_STRING];
    if (path_expand_home(PLUG_PRESET_DIR, dir, sizeof(dir)) != 0 ||
        path_safe_name(plug_name, plug_safe, sizeof(plug_safe)) != 0 ||
        path_safe_name(name, name_safe, sizeof(name_safe)) != 0)
        return false;
    int len = snprintf(bundle, MAX_PATH_STRING, "%s/%s_%s.preset.lv2/", dir,
                       plug_safe, name_safe);
    if (len < 0 || len >= MAX_PATH_STRING)
        return false;
    len = snprintf(file, MAX_PATH_STRING, "%s.ttl", name_safe);
    return len >= 0 && len < MAX_PATH_STRING;
}

int plug_plugin_preset_save(void *plug, const char *name) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug || !cur_plug->plug_instance || !cur_plug->plug_data)
        return -1;
    if (!name || name[0] == '\0')
        return -2;
    char bundle[MAX_PATH_STRING];
    char file[MAX_PATH_STRING];
    if (!plug_preset_paths(cur_plug, name, bundle, file))
        return -1;
    PLUG_INFO *plug_data = cur_plug->plug_data;
    LilvWorld *world = plug_data->lv_world;

    // the plugin's save() may run next to its run(), no pause. Files it saves
    // go into the bundle
    LilvState *state = lilv_state_new_from_instance(
        cur_plug->plug, cur_plug->plug_instance, &(cur_plug->map), NULL,
        bundle, bundle, bundle, plug_get_port_value, cur_plug,
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, NULL);
    if (!state)
        return -1;
    lilv_state_set_label(state, name);
    int err = lilv_state_save(world, &(cur_plug->map), &(cur_plug->unmap),
                              state, NULL, bundle, file);
    lilv_state_free(state);
    if (err != 0) {
        context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                             is_audio_thread, "Could not save preset %s%s\n",
                             bundle, file);
        return -1;
    }

    // unloaded first, a loaded bundle is not read again
    LilvNode *bundle_uri = lilv_new_file_uri(world, NULL, bundle);
    if (!bundle_uri)
        return -1;
    lilv_world_unload_bundle(world, bundle_uri);
    lilv_world_load_bundle(world, bundle_uri);
    lilv_node_free(bundle_uri);
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (plug_data->plugins[i].plug == cur_plug->plug)
            plug_presets_clear(&plug_data->plugins[i]);
    return 0;
}

int plug_read_ui_to_rt_messages(PLUG_INFO *plug_data) {
    // set the is_audio_thread to true so its false on [main-thread] and true on
    // [audio-thread]
    is_audio_thread = true;
    if (!plug_data) {
        return -1;
    }
    // update the rt_tick
    plug_data->rt_tick += 1;
    if (plug_data->rt_tick > RT_CYCLES)
        plug_data->rt_tick = 0;

    // process the control_data sys messages for [audio_thread] (stop plugin and
    // similar)
    context_sub_process_rt(plug_data->control_data);
    // read the param messages for plugins
    for (unsigned int i = 0; i < MAX_INSTANCES; i++) {
        PLUG_PLUG *cur_plug = &(plug_data->plugins[i]);
        if (cur_plug->is_processing == 0)
            continue;
        if (!cur_plug->plug_instance)
            continue;
        param_msgs_process(cur_plug->plug_params, 1);
    }
    return 0;
}
int plug_read_rt_to_ui_messages(PLUG_INFO *plug_data) {
    if (!plug_data)
        return -1;
    // process the control_data sys messages for [main_thread] (like send msg)
    context_sub_process_ui(plug_data->control_data);
    // read the param rt_to_ui messages and set the parameter values
    for (unsigned int i = 0; i < MAX_INSTANCES; i++) {
        PLUG_PLUG *cur_plug = &(plug_data->plugins[i]);
        if (!cur_plug->plug_instance)
            continue;
        param_msgs_process(cur_plug->plug_params, 0);
        plug_gui_ports_update(cur_plug, false);
        plug_gui_atoms_out(cur_plug);
        // non-zero: the UI was closed
        if (cur_plug->gui_idle &&
            cur_plug->gui_idle->idle(cur_plug->gui_inst) != 0)
            plug_gui_hide(cur_plug);
        uint32_t latency = atomic_load(&cur_plug->latency_rt);
        if (latency != cur_plug->latency) {
            cur_plug->latency = latency;
            cur_plug->latency_changed = true;
            graph_node_latency_set(plug_data->graph, cur_plug->node, latency);
            log_append_logfile("%s: latency %u frames\n", cur_plug->name,
                               latency);
        }
        uint32_t gui_dropped = atomic_exchange(&cur_plug->gui_dropped, 0U);
        if (gui_dropped > 0)
            log_append_logfile("%s: %u atoms to the GUI dropped\n",
                               cur_plug->name, gui_dropped);
        for (uint32_t j = 0; cur_plug->ports && j < cur_plug->num_ports; j++) {
            PLUG_PORT *port = &(cur_plug->ports[j]);
            uint32_t dropped = atomic_exchange(&port->seq_dropped, 0U);
            if (dropped > 0)
                log_append_logfile(
                    "%s %s: %u events dropped\n", cur_plug->name,
                    lilv_node_as_string(
                        lilv_port_get_symbol(cur_plug->plug, port->lilv_port)),
                    dropped);
        }
    }
    return 0;
}

// build the display name into plug->name, numbered when a loaded plugin has
// the same name. Called once when the plugin is loaded; plug_plugin_name()
// just returns the stored string after.
static void plug_set_display_name(PLUG_PLUG *plug) {
    if (!plug || !plug->plug || !plug->plug_data)
        return;
    LilvNode *name_node = lilv_plugin_get_name(plug->plug);
    const char *name = name_node ? lilv_node_as_string(name_node) : "";
    uint32_t used[MAX_INSTANCES];
    size_t used_count = 0;
    for (unsigned int i = 0; i < MAX_INSTANCES; i++) {
        const PLUG_PLUG *other = &plug->plug_data->plugins[i];
        if (other != plug && other->plug_instance &&
            strcmp(other->name + other->name_at, name) == 0)
            used[used_count++] = other->name_num;
    }
    plug->name_num = str_numbered_name(plug->name, sizeof(plug->name), name,
                                       used, used_count, &plug->name_at);
    lilv_node_free(name_node);
}

// classifies an LV2 control's value - used only to decide the size of this
// control's param increment (see the controls loop below, val_t).
enum appReturnType {
    Uchar_type = 0x01,
    Int_type = 0x02,
    Float_type = 0x03,
    // returned value should be displayed as db, so
    // converted to log scale
    DB_Return_Type = 0x04,
    // control is an enumeration - forces increment to 1 below
    String_Return_Type = 0x05,
    // float that should be presented to the user as a special curve
    Curve_Float_Return_Type = 0x06
};

// a control's param increment for its range [min, max], 0 when read-only
static PARAM_T plug_control_increment(const PLUG_CONTROL *ctrl, PARAM_T min,
                                      PARAM_T max) {
    unsigned char val_t = Float_type;
    if (ctrl->is_integer || ctrl->is_toggle) {
        val_t = Int_type;
    }
    if (ctrl->is_enumeration) {
        val_t = String_Return_Type;
    }

    PARAM_T cur_inc = 0;
    if (ctrl->is_writable == 1) {
        // decide how big the increment of the parameter will be
        PARAM_T total_range = max - min;
        if (total_range < 0)
            total_range *= -1;
        cur_inc = 1;
        if (val_t == Float_type) {
            cur_inc = total_range * 0.01;
        }
        if (val_t == Int_type) {
            if (total_range <= 10)
                cur_inc = 1;
            else if (total_range <= 100)
                cur_inc = 5;
            else if (total_range <= 1000)
                cur_inc = 10;
            else
                cur_inc = (unsigned int)(total_range * 0.05);
        }
        if (ctrl->is_toggle)
            cur_inc = 1;
        if (ctrl->is_enumeration)
            cur_inc = 1;
    }
    return cur_inc;
}

// create a control from property (not a port control)
static PLUG_CONTROL *new_property_control(LilvWorld *const world,
                                          const LilvNode *property,
                                          const PLUG_NODES *const nodes,
                                          LV2_URID_Map *const map,
                                          LV2_Atom_Forge *forge) {
    PLUG_CONTROL *id = (PLUG_CONTROL *)calloc(1, sizeof(PLUG_CONTROL));
    if (!id)
        return NULL;
    id->node = NULL;
    id->is_enumeration = 0;
    id->is_integer = 0;
    id->is_logarithmic = 0;
    id->is_readable = 0;
    id->is_toggle = 0;
    id->is_writable = 0;
    id->points = NULL;
    id->type = PROPERTY;
    id->node = lilv_node_duplicate(property);
    id->symbol = lilv_world_get_symbol(world, property);
    id->property = map->map(map->handle, lilv_node_as_uri(property));

    id->label = lilv_world_get(world, property, nodes->rdfs_label, NULL);
    id->min = lilv_world_get(world, property, nodes->lv2_minimum, NULL);
    id->max = lilv_world_get(world, property, nodes->lv2_maximum, NULL);
    id->def = lilv_world_get(world, property, nodes->lv2_default, NULL);

    const char *const types[] = {
        LV2_ATOM__Int,  LV2_ATOM__Long,   LV2_ATOM__Float, LV2_ATOM__Double,
        LV2_ATOM__Bool, LV2_ATOM__String, LV2_ATOM__Path,  NULL};
    for (const char *const *t = types; *t; ++t) {
        LilvNode *range = lilv_new_uri(world, *t);
        const bool result =
            lilv_world_ask(world, property, nodes->rdfs_range, range);
        lilv_node_free(range);
        if (result) {
            id->value_type = map->map(map->handle, *t);
            break;
        }
    }

    id->is_toggle = (id->value_type == forge->Bool);
    id->is_integer =
        (id->value_type == forge->Int) || (id->value_type == forge->Long);

    return id;
}

static void plug_create_properties(PLUG_INFO *plug_data, PLUG_PLUG *plug,
                                   bool writable) {
    const LilvPlugin *plugin = plug->plug;
    LilvWorld *world = plug_data->lv_world;
    LilvNode *patch_writable = lilv_new_uri(world, LV2_PATCH__writable);
    LilvNode *patch_readable = lilv_new_uri(world, LV2_PATCH__readable);

    LilvNodes *properties =
        lilv_world_find_nodes(world, lilv_plugin_get_uri(plugin),
                              writable ? patch_writable : patch_readable, NULL);
    LILV_FOREACH(nodes, p, properties) {
        const LilvNode *property = lilv_nodes_get(properties, p);
        PLUG_CONTROL *record = NULL;

        // dont create a control if its already created as writable, simply add
        // readable property to true too
        if (!writable && lilv_world_ask(world, lilv_plugin_get_uri(plug->plug),
                                        patch_writable, property)) {
            for (size_t i = 0; i < plug->num_controls; i++) {
                if (lilv_node_equals(plug->controls[i]->node, property)) {
                    record = plug->controls[i];
                    record->is_readable = true;
                    break;
                }
            }

            if (record) {
                continue;
            }
        }

        record = new_property_control(world, property, &plug_data->nodes,
                                      &plug->map, &plug->forge);
        if (writable) {
            record->is_writable = true;
        } else {
            record->is_readable = true;
        }

        if (record->value_type) {
            // add control to the controls array
            plug->controls = (PLUG_CONTROL **)realloc(
                plug->controls,
                sizeof(PLUG_CONTROL *) * (plug->num_controls + 1));
            plug->controls[plug->num_controls] = record;
            plug->num_controls += 1;
        } else {
            free(record);
        }
    }

    lilv_nodes_free(properties);
    lilv_node_free(patch_readable);
    lilv_node_free(patch_writable);
}

static int plug_property_param_cmp(const void *a, const void *b) {
    LV2_URID ua = ((const PLUG_PROPERTY_PARAM *)a)->urid;
    LV2_URID ub = ((const PLUG_PROPERTY_PARAM *)b)->urid;
    return (ua > ub) - (ua < ub);
}

// build property_params from the property controls, whose index is their
// val_id. 0 on success
static int plug_property_params_build(PLUG_PLUG *plug) {
    uint32_t count = 0;
    for (unsigned int i = 0; i < plug->num_controls; i++)
        if (plug->controls[i] && plug->controls[i]->type == PROPERTY)
            count++;
    if (count == 0)
        return 0;
    plug->property_params = malloc(count * sizeof(PLUG_PROPERTY_PARAM));
    if (!plug->property_params)
        return -1;
    for (unsigned int i = 0; i < plug->num_controls; i++) {
        PLUG_CONTROL *control = plug->controls[i];
        if (!control || control->type != PROPERTY)
            continue;
        plug->property_params[plug->num_property_params++] =
            (PLUG_PROPERTY_PARAM){.urid = control->property, .val_id = (int)i};
    }
    qsort(plug->property_params, count, sizeof(PLUG_PROPERTY_PARAM),
          plug_property_param_cmp);
    return 0;
}

// a property's param, NULL when it has none. property_params is not changed
// after load, any thread may ask
static const PLUG_PROPERTY_PARAM *
plug_property_param_find(const PLUG_PLUG *plug, LV2_URID property) {
    if (plug->num_property_params == 0)
        return NULL;
    PLUG_PROPERTY_PARAM want = {.urid = property};
    return bsearch(&want, plug->property_params, plug->num_property_params,
                   sizeof(PLUG_PROPERTY_PARAM), plug_property_param_cmp);
}

// [audio-thread] the plugin says one of its properties has a new value
static void plug_property_changed_rt(PLUG_PLUG *plug, LV2_URID property,
                                     const LV2_Atom *value) {
    const PLUG_PROPERTY_PARAM *found = plug_property_param_find(plug, property);
    float fvalue;
    if (!found || !plug_value_as_float(plug, LV2_ATOM_BODY_CONST(value),
                                       value->size, value->type, &fvalue))
        return;
    // a value a full queue refuses is sent by param_rt_resend
    param_set_value_rt(plug->plug_params, found->val_id, (PARAM_T)fvalue);
}

// [audio-thread] an object from the plugin's output. patch:Set carries one
// property's new value, patch:Put several - it is how a plugin answers
// patch:Get
static void plug_patch_message_rt(PLUG_PLUG *plug,
                                  const LV2_Atom_Object_Body *body,
                                  uint32_t size) {
    const PLUG_URIDS *urids = plug->urids;
    if (body->otype == urids->patch_Set) {
        const LV2_Atom *property = NULL;
        const LV2_Atom *value = NULL;
        lv2_atom_object_body_get(size, body, urids->patch_property, &property,
                                 urids->patch_value, &value, 0);
        if (property && value && property->type == plug->forge.URID)
            plug_property_changed_rt(
                plug, ((const LV2_Atom_URID *)property)->body, value);
        return;
    }
    if (body->otype == urids->patch_Put) {
        const LV2_Atom *put_body = NULL;
        lv2_atom_object_body_get(size, body, urids->patch_body, &put_body, 0);
        if (!put_body || put_body->type != plug->forge.Object)
            return;
        LV2_ATOM_OBJECT_FOREACH((const LV2_Atom_Object *)put_body, prop) {
            plug_property_changed_rt(plug, prop->key, &prop->value);
        }
    }
}

static int plug_ports_create(PLUG_INFO *plug_data, PLUG_PLUG *plug);

// the features a UI is given, ui:parent only to an embedded one
static const char *const plug_ui_features[] = {
    LV2_URID__map,          LV2_URID__unmap,     LV2_UI__resize,
    LV2_INSTANCE_ACCESS_URI, LV2_DATA_ACCESS_URI, LV2_OPTIONS__options,
    LV2_UI__idleInterface,  LV2_UI__portMap,     LV2_LOG__log,
    LV2_UI__touch,          NULL};

// does the host give every feature the UI requires. Logs the first one
// missing
static bool plug_ui_features_given(PLUG_INFO *plug_data, const char *name,
                                   const LilvUI *ui, bool embed) {
    const LilvNode *ui_uri = lilv_ui_get_uri(ui);
    LilvNodes *required =
        lilv_world_find_nodes(plug_data->lv_world, ui_uri,
                              plug_data->nodes.lv2_requiredFeature, NULL);
    bool given = true;
    LILV_FOREACH(nodes, i, required) {
        const char *feature = lilv_node_as_string(lilv_nodes_get(required, i));
        bool found = embed && strcmp(feature, LV2_UI__parent) == 0;
        for (size_t f = 0; !found && plug_ui_features[f]; f++)
            found = strcmp(feature, plug_ui_features[f]) == 0;
        if (found)
            continue;
        log_append_logfile("%s: UI %s skipped, it requires %s\n", name,
                           lilv_node_as_string(ui_uri), feature);
        given = false;
        break;
    }
    lilv_nodes_free(required);
    return given;
}

// an X11 UI embedded in a host window, else a UI showing its own window
static void plug_gui_ui_pick(PLUG_INFO *plug_data, PLUG_PLUG *plug, bool x11) {
    LilvWorld *world = plug_data->lv_world;
    PLUG_NODES *nodes = &plug_data->nodes;
    plug->uis = lilv_plugin_get_uis(plug->plug);
    const LilvUI *show_ui = NULL;
    LILV_FOREACH(uis, i, plug->uis) {
        const LilvUI *ui = lilv_uis_get(plug->uis, i);
        const LilvNode *ui_uri = lilv_ui_get_uri(ui);
        // its features and extension data are often in a file of its own
        lilv_world_load_resource(world, ui_uri);
        // a UI missing a feature when embedded misses it when shown too
        if (x11 && lilv_ui_is_a(ui, nodes->ui_X11UI)) {
            if (!plug_ui_features_given(plug_data, plug->name, ui, true))
                continue;
            plug->gui_ui = ui;
            plug->gui_mode = PLUG_GUI_EMBED;
            break;
        }
        if (!show_ui &&
            lilv_world_ask(world, ui_uri, nodes->lv2_extensionData,
                           nodes->ui_showInterface) &&
            plug_ui_features_given(plug_data, plug->name, ui, false))
            show_ui = ui;
    }
    if (plug->gui_mode == PLUG_GUI_NONE && show_ui) {
        plug->gui_ui = show_ui;
        plug->gui_mode = PLUG_GUI_SHOW;
    }
}

// [main-thread] at load. Either mode needs a window api, embedding x11
static void plug_gui_pick(PLUG_INFO *plug_data, PLUG_PLUG *plug) {
    const char *api = hwin_sys_api(plug_data->hwin);
    if (api)
        plug_gui_ui_pick(plug_data, plug, strcmp(api, "x11") == 0);
    if (plug->gui_mode == PLUG_GUI_NONE) {
        log_append_logfile("%s: GUI none\n", plug->name);
        return;
    }
    log_append_logfile(
        "%s: GUI %s, %s\n", plug->name,
        plug->gui_mode == PLUG_GUI_EMBED ? "embedded" : "floating",
        lilv_node_as_string(lilv_ui_get_uri(plug->gui_ui)));
}

uint32_t plug_load_and_activate(void *plugin_item) {
    PLUGIN_LIST_ITEM *plugin_list_item = (PLUGIN_LIST_ITEM *)plugin_item;
    if (!plugin_list_item)
        return 0;
    PLUG_INFO *plug_data = plugin_list_item->plug_data;
    if (!plug_data) {
        return 0;
    }
    if (!plug_data->lv_world) {
        return 0;
    }

    const LilvPlugin *plugins = lilv_world_get_all_plugins(plug_data->lv_world);
    LilvNode *name_uri =
        lilv_new_uri(plug_data->lv_world, plugin_list_item->plugin_path);
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(plugins, name_uri);
    lilv_node_free(name_uri);
    if (!plugin) {
        return 0;
    }
    int plug_id = -1;
    // find next empty plugin and fill it in
    for (int i = 0; i < MAX_INSTANCES; i++) {
        PLUG_PLUG *cur_plug = &(plug_data->plugins[i]);
        if (cur_plug->plug_instance)
            continue;
        plug_id = cur_plug->id;
        break;
    }

    if (plug_id == -1 || plug_id >= MAX_INSTANCES)
        return 0;

    PLUG_PLUG *plug = &(plug_data->plugins[plug_id]);
    // assign the identity uid once, when the slot is claimed. Ports are
    // registered under it as their owner, so it has to exist before any of
    // them. A slot keeps its last uid until this reassigns it
    plug->uid = ++plug_data->next_plug_uid;
    plug->pos_seq = 0;

    plug->plug = plugin;

    zix_sem_init(&(plug->work_lock), 1);
    // check safe restore
    LilvNode *state_threadSafeRestore =
        lilv_new_uri(plug_data->lv_world, LV2_STATE__threadSafeRestore);
    if (lilv_plugin_has_feature(plug->plug, state_threadSafeRestore)) {
        plug->safe_restore = true;
    }
    lilv_node_free(state_threadSafeRestore);
    // features that have no data. static - feature_list keeps pointing at
    // them, a re-instantiate passes it again
    static const LV2_Feature static_features[] = {
        {LV2_STATE__loadDefaultState, NULL},
        {LV2_BUF_SIZE__powerOf2BlockLength, NULL},
        {LV2_BUF_SIZE__fixedBlockLength, NULL},
        {LV2_BUF_SIZE__boundedBlockLength, NULL}};
    // build the features list to pass to plugins
    // one work:schedule, the RT worker's - the state worker's and
    // threadSafeRestore are for restore() only (plug_restore_features)
    const LV2_Feature *const features[] = {
        &(plug->features.map_feature),
        &(plug->features.unmap_feature),
        &(plug->features.sched_feature),
        &(plug->features.log_feature),
        &(plug->features.options_feature),
        &static_features[0],
        &static_features[1],
        &static_features[2],
        &static_features[3],
        NULL};

    // the failures below remove the plugin directly - it was never started,
    // so [audio-thread] never saw it
    plug->feature_list = (const LV2_Feature **)calloc(1, sizeof(features));
    if (!plug->feature_list) {
        plug_remove_plug(plug_data, plug->id);
        return 0;
    }
    memcpy(plug->feature_list, features, sizeof(features));
    // instantiate and activate the plugin
    plug->plug_instance = lilv_plugin_instantiate(
        plugin, plug_data->sample_rate, plug->feature_list);
    if (!plug->plug_instance) {
        plug_remove_plug(plug_data, plug->id);
        return 0;
    }

    plug_workers_start(plug_data, plug);

    // the plugin's node and its ports
    if (plug_ports_create(plug_data, plug) != 0) {
        plug_remove_plug(plug_data, plug->id);
        return 0;
    }

    //--------------------------------------------------
    // create controls if they are properties and not ports
    plug_create_properties(plug_data, plug, true);
    plug_create_properties(plug_data, plug, false);
    //--------------------------------------------------

    // go through all created controls and create a param for each of them.
    // params are created in the same order as plug->controls, which never
    // reorders/resyncs after load - val_id is kept in lockstep with plug->
    // controls' own indices, since plug_run_rt later indexes params by the
    // same ctrl_iter it uses for plug->controls - a gap here would desync
    // every param after it. uid is passed as 0 so params.c mints it.
    // owner_id is ct_iter + 1, the index into plug->controls (offset because
    // 0 means "no owner id"). NEVER persist that index - it is in-memory
    // only; the key is what a saved value is matched by: the port symbol,
    // or a property's URI (lilv_world_get_symbol derives a symbol from the
    // URI, two properties may get the same one).
    if (plug->controls) {
        PRM_CONTAIN *plug_params = params_init_param_container(NULL);
        // the lockstep above is an invariant, so check it rather than trust
        // it: every add must land on val_id == ct_iter. A single failed add
        // (-1) would shift every param after it and silently pair each one
        // with the wrong control on the [audio-thread].
        int lockstep_ok = 1;

        for (unsigned int ct_iter = 0; ct_iter < plug->num_controls;
             ct_iter++) {
            PLUG_CONTROL *cur_ctrl = plug->controls[ct_iter];
            if (!cur_ctrl) {
                if (param_add_param(plug_params, "", "", 0, 0, 0, 0, 0,
                                    ct_iter + 1, 0, 0,
                                    NULL) != (int)ct_iter) {
                    lockstep_ok = 0;
                    break;
                }
                continue;
            }

            const char *param_key =
                cur_ctrl->type == PROPERTY
                    ? lilv_node_as_uri(cur_ctrl->node)
                    : lilv_node_as_string(cur_ctrl->symbol);
            const char *param_name = lilv_node_as_string(cur_ctrl->label);
            if (!param_name)
                param_name = lilv_node_as_string(cur_ctrl->symbol);
            if (!param_name)
                param_name = "";
            PARAM_T param_val = 0;
            if (lilv_node_is_float(cur_ctrl->def) == 1 ||
                lilv_node_is_int(cur_ctrl->def) == 1) {
                param_val = lilv_node_as_float(cur_ctrl->def);
            }
            PARAM_T param_min = lilv_node_as_float(cur_ctrl->min);
            PARAM_T param_max = lilv_node_as_float(cur_ctrl->max);
            PARAM_T cur_inc =
                plug_control_increment(cur_ctrl, param_min, param_max);
            uint32_t p_flags = 0;
            if (cur_ctrl->is_writable == 0)
                p_flags |= PARAM_FLAG_READONLY;
            if (cur_ctrl->is_enumeration)
                p_flags |= PARAM_FLAG_ENUM;
            if (param_add_param(plug_params, param_name, param_key, param_val,
                                param_min, param_max, cur_inc, 0, ct_iter + 1,
                                p_flags, 0, NULL) != (int)ct_iter) {
                lockstep_ok = 0;
                break;
            }
        }
        if (!lockstep_ok) {
            param_clean_param_container(plug_params);
            plug_remove_plug(plug_data, plug->id);
            return 0;
        }
        // TODO val_to_string callback reading them straight from plug->controls
        // can be added here whenever something actually calls
        // param_get_value_as_string for this container (nothing does yet -
        // params are not wired into any UI at this stage)

        plug->plug_params = plug_params;
        if (plug_property_params_build(plug) != 0) {
            plug_remove_plug(plug_data, plug->id);
            return 0;
        }
    }

    // activate the plugin instance
    lilv_instance_activate(plug->plug_instance);
    plug->plug_instance_activated = 1;
    plug->plug_data = plug_data;
    // build the display name once, now that plug->plug and plug->id are set
    plug_set_display_name(plug);
    plug_gui_pick(plug_data, plug);
    // its first cycle already runs a plan with its ports
    graph_plan_update(plug_data->graph);
    // wait for the plugin to start processing
    context_sub_wait_for_start(plug_data->control_data, (void *)plug);
    plug_data->plugins_dirty = true;

    return plug->uid;
}

void *plug_plugin_return(PLUG_INFO *plug_data, unsigned int idx) {
    if (!plug_data)
        return NULL;

    // the plugins array can have gaps, walk the occupied slots in order and
    // return the idx-th one; NULL once idx is past the last occupied slot
    unsigned int found = 0;
    for (unsigned int i = 0; i < MAX_INSTANCES; i++) {
        PLUG_PLUG *cur_plug = &(plug_data->plugins[i]);
        if (!cur_plug->plug_instance)
            continue;
        if (found == idx)
            return (void *)cur_plug;
        found++;
    }
    return NULL;
}

const char *plug_plugin_name(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug)
        return NULL;
    if (!cur_plug->plug_instance)
        return NULL;

    return cur_plug->name;
}

const char *plug_plugin_uri(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug)
        return NULL;
    if (!cur_plug->plug_instance)
        return NULL;

    return lilv_node_as_uri(lilv_plugin_get_uri(cur_plug->plug));
}

uint32_t plug_plugin_uid(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug)
        return 0;
    return cur_plug->uid;
}

uint32_t plug_plugin_latency(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug)
        return 0;
    return cur_plug->latency;
}

bool plug_plugin_latency_changed(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug)
        return false;
    bool changed = cur_plug->latency_changed;
    cur_plug->latency_changed = false;
    return changed;
}

bool plug_plugin_gui_available(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    return cur_plug && cur_plug->gui_mode != PLUG_GUI_NONE;
}

bool plug_plugin_gui_shown(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    return cur_plug && cur_plug->gui_shown;
}

bool plug_plugin_gui_changed(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug)
        return false;
    bool changed = cur_plug->gui_changed;
    cur_plug->gui_changed = false;
    return changed;
}

// [main-thread] flagged for plug_plugin_gui_changed. The run sees it after the
// rings it gates
static void plug_gui_shown_set(PLUG_PLUG *plug, bool shown) {
    atomic_store_explicit(&plug->gui_shown_rt, shown, memory_order_release);
    if (plug->gui_shown == shown)
        return;
    plug->gui_shown = shown;
    plug->gui_changed = true;
}

static const void *plug_gui_extension(const LV2UI_Descriptor *desc,
                                      const char *uri) {
    return desc->extension_data ? desc->extension_data(uri) : NULL;
}

// [main-thread] cleanup() - after hide() for a shown UI of its own window -
// then the host window, our library reference last
static void plug_gui_destroy(PLUG_PLUG *plug, bool shown) {
    if (plug->gui_inst) {
        if (shown && plug->gui_show_if)
            plug->gui_show_if->hide(plug->gui_inst);
        plug->gui_desc->cleanup(plug->gui_inst);
    }
    hwin_destroy(plug->gui_win);
    if (plug->gui_lib)
        dlclose(plug->gui_lib);
    plug->gui_lib = NULL;
    plug->gui_desc = NULL;
    plug->gui_inst = NULL;
    plug->gui_idle = NULL;
    plug->gui_show_if = NULL;
    plug->gui_user_resize = NULL;
    plug->gui_win = NULL;
    plug->gui_w = 0;
    plug->gui_h = 0;
}

// [main-thread]
static int plug_gui_hide(PLUG_PLUG *plug) {
    if (!plug->gui_shown)
        return 0;
    plug_gui_destroy(plug, true);
    plug_gui_shown_set(plug, false);
    return 0;
}

// [main-thread] the user closed the host window
static void plug_gui_on_close(void *user_data) {
    plug_gui_hide((PLUG_PLUG *)user_data);
}

// [main-thread] a size the user or the window manager gave the host window,
// the window follows the UI's child. A refusal gets the UI's own size back
static void plug_gui_on_resize(void *user_data, uint32_t w, uint32_t h) {
    PLUG_PLUG *plug = (PLUG_PLUG *)user_data;
    if (!plug->gui_user_resize)
        return;
    int refused =
        plug->gui_user_resize->ui_resize(plug->gui_inst, (int)w, (int)h);
    if (refused && plug->gui_w > 0)
        hwin_resize(plug->gui_win, plug->gui_w, plug->gui_h);
}

// ui:resize, the UI's size
static int plug_gui_host_resize(LV2UI_Feature_Handle handle, int width,
                                int height) {
    PLUG_PLUG *plug = (PLUG_PLUG *)handle;
    if (!plug->gui_win || width <= 0 || height <= 0)
        return 1;
    plug->gui_w = (uint32_t)width;
    plug->gui_h = (uint32_t)height;
    hwin_resize(plug->gui_win, plug->gui_w, plug->gui_h);
    return 0;
}

static uint32_t plug_gui_port_index(LV2UI_Feature_Handle handle,
                                    const char *symbol) {
    PLUG_PORT *port = plug_find_port_by_name((PLUG_PLUG *)handle, symbol);
    return port ? port->index : LV2UI_INVALID_PORT_INDEX;
}

static void plug_gui_touch(LV2UI_Feature_Handle handle, uint32_t port_index,
                           bool grabbed) {
    (void)handle;
    (void)port_index;
    (void)grabbed;
}

// an input control with a param, the only port a float write reaches
static bool plug_gui_port_writable(const PLUG_PORT *port) {
    return port->type == PORT_TYPE_CONTROL && port->flow == PORT_FLOW_INPUT &&
           port->param_index >= 0;
}

// [main-thread] a control port's value for the UI, false for any other port
static bool plug_gui_port_value(PLUG_PLUG *plug, PLUG_PORT *port,
                                float *out) {
    if (plug_gui_port_writable(port)) {
        *out = (float)param_get_value(plug->plug_params, port->param_index, 0);
        return true;
    }
    if (port->type == PORT_TYPE_CONTROL && port->flow == PORT_FLOW_OUTPUT) {
        *out = atomic_load(&port->control_rt);
        return true;
    }
    return false;
}

// [main-thread] port_event for each control port whose value the UI has not
// got or written, for every one when all
static void plug_gui_ports_update(PLUG_PLUG *plug, bool all) {
    if (!plug->gui_inst || !plug->gui_desc->port_event)
        return;
    for (uint32_t i = 0; i < plug->num_ports; i++) {
        PLUG_PORT *port = &(plug->ports[i]);
        float value;
        if (!plug_gui_port_value(plug, port, &value) ||
            (!all && value == port->gui_sent))
            continue;
        port->gui_sent = value;
        plug->gui_desc->port_event(plug->gui_inst, i, sizeof(value), 0,
                                   &value);
    }
}

static void plug_gui_rings_free(PLUG_PLUG *plug) {
    zix_ring_free(plug->gui_ui_to_rt);
    zix_ring_free(plug->gui_rt_to_ui);
    free(plug->gui_atom);
    plug->gui_ui_to_rt = NULL;
    plug->gui_rt_to_ui = NULL;
    plug->gui_atom = NULL;
}

// [main-thread] at the first show, each ring holds a full sequence of the
// plugin's biggest atom port. False when they could not be made
static bool plug_gui_rings_make(PLUG_PLUG *plug) {
    if (plug->gui_ui_to_rt)
        return true;
    uint32_t size = (uint32_t)plug->plug_data->midi_buf_size;
    for (uint32_t i = 0; i < plug->num_ports; i++)
        if (plug->ports[i].evbuf)
            size = MAX(size, plug->ports[i].evbuf->capacity);
    plug->gui_ui_to_rt = zix_ring_new(NULL, size);
    plug->gui_rt_to_ui = zix_ring_new(NULL, size);
    if (plug->gui_rt_to_ui)
        plug->gui_atom = malloc(zix_ring_capacity(plug->gui_rt_to_ui));
    if (!plug->gui_ui_to_rt || !plug->gui_rt_to_ui || !plug->gui_atom) {
        plug_gui_rings_free(plug);
        return false;
    }
    zix_ring_mlock(plug->gui_ui_to_rt);
    zix_ring_mlock(plug->gui_rt_to_ui);
    return true;
}

// a record and its atom in one write, false when the ring has no room. Only on
// the ring's writer thread
static bool plug_gui_ring_write(ZixRing *ring, uint32_t port, LV2_URID type,
                                uint32_t size, const void *body) {
    const PLUG_GUI_ATOM rec = {.port = port,
                               .size = (uint32_t)sizeof(LV2_Atom) + size};
    const LV2_Atom atom = {.size = size, .type = type};
    ZixRingTransaction tx = zix_ring_begin_write(ring);
    if (zix_ring_amend_write(ring, &tx, &rec, sizeof(rec)) ||
        zix_ring_amend_write(ring, &tx, &atom, sizeof(atom)) ||
        zix_ring_amend_write(ring, &tx, body, size))
        return false;
    zix_ring_commit_write(ring, &tx);
    return true;
}

// [main-thread] the plugin's atoms to the shown UI. Left in the ring while
// there is none, the next show empties it
static void plug_gui_atoms_out(PLUG_PLUG *plug) {
    ZixRing *ring = plug->gui_rt_to_ui;
    if (!plug->gui_inst || !ring)
        return;
    PLUG_GUI_ATOM rec;
    while (zix_ring_read(ring, &rec, sizeof(rec)) == sizeof(rec)) {
        zix_ring_read(ring, plug->gui_atom, rec.size);
        if (plug->gui_desc->port_event)
            plug->gui_desc->port_event(plug->gui_inst, rec.port, rec.size,
                                       plug->urids->atom_eventTransfer,
                                       plug->gui_atom);
    }
}

// [audio-thread] the UI's atoms into their ports' sequences. One with no room
// waits in the ring for the next cycle, the ones behind it with it
static void plug_gui_atoms_in_rt(PLUG_PLUG *plug) {
    ZixRing *ring = plug->gui_ui_to_rt;
    PLUG_GUI_ATOM rec;
    while (zix_ring_peek(ring, &rec, sizeof(rec)) == sizeof(rec)) {
        PLUG_EVBUF_ITERATOR iter =
            plug_evbuf_end(plug->ports[rec.port].evbuf);
        LV2_Atom *atom = plug_evbuf_append(&iter, 0, rec.size);
        if (!atom)
            return;
        zix_ring_skip(ring, sizeof(rec));
        zix_ring_read(ring, atom, rec.size);
    }
}

// [main-thread] a patch:Set of a writable property with a number sets its
// param, the run sends the plugin the param's value. False for any other atom
static bool plug_gui_property_set(PLUG_PLUG *plug, const LV2_Atom *atom) {
    const PLUG_URIDS *urids = plug->urids;
    if (atom->type != plug->forge.Object ||
        atom->size < sizeof(LV2_Atom_Object_Body) ||
        ((const LV2_Atom_Object *)atom)->body.otype != urids->patch_Set)
        return false;
    const LV2_Atom *property = NULL;
    const LV2_Atom *value = NULL;
    lv2_atom_object_get((const LV2_Atom_Object *)atom, urids->patch_property,
                        &property, urids->patch_value, &value, 0);
    if (!property || !value || property->type != plug->forge.URID)
        return false;
    const PLUG_PROPERTY_PARAM *found = plug_property_param_find(
        plug, ((const LV2_Atom_URID *)property)->body);
    float fvalue;
    if (!found || !plug->controls[found->val_id]->is_writable ||
        !plug_value_as_float(plug, LV2_ATOM_BODY_CONST(value), value->size,
                             value->type, &fvalue))
        return false;
    param_set_value(plug->plug_params, found->val_id, fvalue, NULL,
                    Operation_SetValue);
    return true;
}

// [main-thread] an atom for an input atom port, to the run through the ring.
// False when it is malformed, its port's sequence can never take it or the
// ring is full
static bool plug_gui_atom_write(PLUG_PLUG *plug, const PLUG_PORT *port,
                                uint32_t buffer_size, const void *buffer) {
    if (port->type != PORT_TYPE_EVENT || port->flow != PORT_FLOW_INPUT ||
        buffer_size < sizeof(LV2_Atom))
        return false;
    const LV2_Atom *atom = (const LV2_Atom *)buffer;
    if (sizeof(LV2_Atom) + atom->size != buffer_size ||
        !plug_evbuf_fits(port->evbuf, buffer_size))
        return false;
    if ((int)port->index == plug->control_in &&
        plug_gui_property_set(plug, atom))
        return true;
    return plug_gui_ring_write(plug->gui_ui_to_rt, port->index, atom->type,
                               atom->size, LV2_ATOM_BODY_CONST(atom));
}

// a float to an input control sets its param. gui_sent takes the raw value:
// one the param clamps goes back to the UI
static void plug_gui_write(LV2UI_Controller controller, uint32_t port_index,
                           uint32_t buffer_size, uint32_t protocol,
                           const void *buffer) {
    PLUG_PLUG *plug = (PLUG_PLUG *)controller;
    PLUG_PORT *port =
        port_index < plug->num_ports ? &(plug->ports[port_index]) : NULL;
    if (protocol == 0 && buffer_size == sizeof(float) && port &&
        plug_gui_port_writable(port)) {
        memcpy(&port->gui_sent, buffer, sizeof(float));
        param_set_value(plug->plug_params, port->param_index, port->gui_sent,
                        NULL, Operation_SetValue);
        return;
    }
    if (protocol == plug->urids->atom_eventTransfer && port &&
        plug_gui_atom_write(plug, port, buffer_size, buffer))
        return;
    log_append_logfile("%s: GUI wrote %u bytes to port %u, protocol %u - "
                       "dropped\n",
                       plug->name, buffer_size, port_index, protocol);
}

// [main-thread] for the current instance, ui:parent only embedded
static void plug_gui_features_set(PLUG_PLUG *plug, bool embed) {
    PLUG_GUI_FEATURES *f = &(plug->gui_features);
    f->resize =
        (LV2UI_Resize){.handle = plug, .ui_resize = plug_gui_host_resize};
    f->port_map =
        (LV2UI_Port_Map){.handle = plug, .port_index = plug_gui_port_index};
    f->touch = (LV2UI_Touch){.handle = plug, .touch = plug_gui_touch};
    const LV2_Feature own[PLUG_GUI_OWN_FEATURES] = {
        {LV2_INSTANCE_ACCESS_URI,
         lilv_instance_get_handle(plug->plug_instance)},
        {LV2_DATA_ACCESS_URI, &(plug->features.ext_data)},
        {LV2_UI__resize, &(f->resize)},
        {LV2_UI__portMap, &(f->port_map)},
        {LV2_UI__touch, &(f->touch)},
        {LV2_UI__idleInterface, NULL},
        {LV2_UI__parent, (void *)hwin_native(plug->gui_win)}};
    memcpy(f->own, own, sizeof(own));
    size_t n = 0;
    f->list[n++] = &(plug->features.map_feature);
    f->list[n++] = &(plug->features.unmap_feature);
    f->list[n++] = &(plug->features.log_feature);
    f->list[n++] = &(plug->features.options_feature);
    for (size_t i = 0; i < PLUG_GUI_OWN_FEATURES - (embed ? 0 : 1); i++)
        f->list[n++] = &(f->own[i]);
    f->list[n] = NULL;
}

// [main-thread] its library open, its descriptor found. The step that failed,
// NULL on success
static const char *plug_gui_open(PLUG_PLUG *plug) {
    char *path = lilv_file_uri_parse(
        lilv_node_as_uri(lilv_ui_get_binary_uri(plug->gui_ui)), NULL);
    if (!path)
        return "binary path";
    plug->gui_lib = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
    lilv_free(path);
    if (!plug->gui_lib) {
        log_append_logfile("%s: %s\n", plug->name, dlerror());
        return "dlopen";
    }
    LV2UI_DescriptorFunction descriptor =
        (LV2UI_DescriptorFunction)dlsym(plug->gui_lib, "lv2ui_descriptor");
    if (!descriptor)
        return "lv2ui_descriptor";
    const char *ui_uri = lilv_node_as_uri(lilv_ui_get_uri(plug->gui_ui));
    for (uint32_t i = 0;; i++) {
        const LV2UI_Descriptor *desc = descriptor(i);
        if (!desc)
            return "descriptor";
        if (desc->URI && strcmp(desc->URI, ui_uri) == 0) {
            plug->gui_desc = desc;
            return NULL;
        }
    }
}

// [main-thread] the UI resizes itself on request and does not say the user
// should not resize it
static const LV2UI_Resize *plug_gui_user_resize(PLUG_PLUG *plug) {
    PLUG_INFO *plug_data = plug->plug_data;
    PLUG_NODES *nodes = &(plug_data->nodes);
    const LilvNode *ui_uri = lilv_ui_get_uri(plug->gui_ui);
    if (lilv_world_ask(plug_data->lv_world, ui_uri, nodes->lv2_optionalFeature,
                       nodes->ui_noUserResize) ||
        lilv_world_ask(plug_data->lv_world, ui_uri, nodes->lv2_optionalFeature,
                       nodes->ui_fixedSize))
        return NULL;
    return plug_gui_extension(plug->gui_desc, LV2_UI__resize);
}

// [main-thread] the UI instantiated and shown, embedded in a host window made
// first or in a window of its own. The step that failed, NULL on success
static const char *plug_gui_create(PLUG_PLUG *plug) {
    if (!plug_gui_rings_make(plug))
        return "atom rings";
    // main is its reader: the atoms the run wrote for an earlier UI dropped
    zix_ring_skip(plug->gui_rt_to_ui, zix_ring_read_space(plug->gui_rt_to_ui));
    const char *failed = plug_gui_open(plug);
    if (failed)
        return failed;
    const LV2UI_Descriptor *desc = plug->gui_desc;
    bool embed = plug->gui_mode == PLUG_GUI_EMBED;
    if (embed) {
        plug->gui_user_resize = plug_gui_user_resize(plug);
        HWIN_FUNCS funcs = {.on_close = plug_gui_on_close,
                            .on_resize = plug_gui_on_resize};
        uint32_t flags =
            HWIN_FIT_CHILD | (plug->gui_user_resize ? HWIN_RESIZABLE : 0);
        plug->gui_win =
            hwin_create(plug->plug_data->hwin, plug->name, PLUG_GUI_START_W,
                        PLUG_GUI_START_H, flags, funcs, plug);
        if (!plug->gui_win)
            return "hwin_create";
    } else {
        plug->gui_show_if = plug_gui_extension(desc, LV2_UI__showInterface);
        if (!plug->gui_show_if)
            return "showInterface";
    }
    plug_gui_features_set(plug, embed);
    char *bundle_path = lilv_file_uri_parse(
        lilv_node_as_uri(lilv_ui_get_bundle_uri(plug->gui_ui)), NULL);
    if (!bundle_path)
        return "bundle path";
    LV2UI_Widget widget = NULL;
    plug->gui_inst = desc->instantiate(
        desc, lilv_node_as_uri(lilv_plugin_get_uri(plug->plug)), bundle_path,
        plug_gui_write, plug, &widget, plug->gui_features.list);
    lilv_free(bundle_path);
    if (!plug->gui_inst)
        return "instantiate";
    plug_gui_ports_update(plug, true);
    plug->gui_idle = plug_gui_extension(desc, LV2_UI__idleInterface);
    if (embed) {
        hwin_show(plug->gui_win);
        return NULL;
    }
    return plug->gui_show_if->show(plug->gui_inst) == 0 ? NULL : "show";
}

// [main-thread] a failed step logged and the UI undone
static int plug_gui_show(PLUG_PLUG *plug) {
    if (!hwin_sys_open(plug->plug_data->hwin)) {
        log_append_logfile("%s: GUI not shown, no window system\n",
                           plug->name);
        return -1;
    }
    const char *failed = plug_gui_create(plug);
    if (failed) {
        log_append_logfile("%s: GUI not shown, %s failed\n", plug->name,
                           failed);
        plug_gui_destroy(plug, false);
        return -1;
    }
    plug_gui_shown_set(plug, true);
    // after the shown flag, so a property plugin's answer reaches the new UI
    atomic_store(&plug->request_update, true);
    return 0;
}

int plug_plugin_gui_toggle(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug || !cur_plug->plug_data ||
        cur_plug->gui_mode == PLUG_GUI_NONE)
        return -1;
    if (cur_plug->gui_shown)
        return plug_gui_hide(cur_plug);
    return plug_gui_show(cur_plug);
}

PRM_CONTAIN *plug_plugin_param_container(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug)
        return NULL;
    return cur_plug->plug_params;
}

bool plug_plugins_is_dirty(PLUG_INFO *plug_data) {
    if (!plug_data)
        return false;
    bool is_dirty = plug_data->plugins_dirty;
    plug_data->plugins_dirty = false;
    return is_dirty;
}

// compare scale points for sorting
static int scale_point_cmp(const ScalePoint *a, const ScalePoint *b) {
    if (a->value < b->value) {
        return -1;
    }

    if (a->value == b->value) {
        return 0;
    }

    return 1;
}
// a port control's min/max as the plugin declares them, an lv2:sampleRate
// port's as multiples of sample_rate
static void plug_control_range_read(LilvWorld *world, const LilvPlugin *plugin,
                                    PLUG_CONTROL *control, float sample_rate) {
    lilv_node_free(control->min);
    lilv_node_free(control->max);
    control->min = NULL;
    control->max = NULL;
    const LilvPort *port = lilv_plugin_get_port_by_index(plugin, control->index);
    if (!port)
        return;
    lilv_port_get_range(plugin, port, NULL, &control->min, &control->max);
    if (!control->is_sample_rate)
        return;
    if (lilv_node_is_float(control->min) || lilv_node_is_int(control->min)) {
        const float min = lilv_node_as_float(control->min) * sample_rate;
        lilv_node_free(control->min);
        control->min = lilv_new_float(world, min);
    }
    if (lilv_node_is_float(control->max) || lilv_node_is_int(control->max)) {
        const float max = lilv_node_as_float(control->max) * sample_rate;
        lilv_node_free(control->max);
        control->max = lilv_new_float(world, max);
    }
}

// create a control from control port
static PLUG_CONTROL *
new_port_control(LilvWorld *const world, const LilvPlugin *const plugin,
                 const LilvPort *const port, uint32_t port_index,
                 const float sample_rate, const PLUG_NODES *const nodes,
                 LV2_Atom_Forge *forge) {
    PLUG_CONTROL *id = (PLUG_CONTROL *)calloc(1, sizeof(PLUG_CONTROL));
    if (!id)
        return NULL;
    id->property = 0;
    id->type = PORT;
    id->node = lilv_node_duplicate(lilv_port_get_node(plugin, port));
    id->symbol = lilv_node_duplicate(lilv_port_get_symbol(plugin, port));
    id->label = lilv_port_get_name(plugin, port);
    id->index = port_index;
    id->group = lilv_port_get(plugin, port, nodes->pg_group);
    id->value_type = forge->Float;
    id->is_writable = lilv_port_is_a(plugin, port, nodes->lv2_InputPort);
    id->is_readable = lilv_port_is_a(plugin, port, nodes->lv2_OutputPort);
    id->is_toggle = lilv_port_has_property(plugin, port, nodes->lv2_toggled);
    id->is_integer = lilv_port_has_property(plugin, port, nodes->lv2_integer);

    id->is_enumeration =
        lilv_port_has_property(plugin, port, nodes->lv2_enumeration);
    id->is_logarithmic =
        lilv_port_has_property(plugin, port, nodes->pprops_logarithmic);

    id->is_sample_rate =
        lilv_port_has_property(plugin, port, nodes->lv2_sampleRate);
    lilv_port_get_range(plugin, port, &id->def, NULL, NULL);
    plug_control_range_read(world, plugin, id, sample_rate);

    LilvScalePoints *sp = lilv_port_get_scale_points(plugin, port);
    id->points = NULL;
    if (sp) {
        id->points = (ScalePoint *)malloc(lilv_scale_points_size(sp) *
                                          sizeof(ScalePoint));
        size_t np = 0;
        if (id->points) {
            LILV_FOREACH(scale_points, s, sp) {
                const LilvScalePoint *p = lilv_scale_points_get(sp, s);
                if (lilv_node_is_float(lilv_scale_point_get_value(p)) ||
                    lilv_node_is_int(lilv_scale_point_get_value(p))) {
                    id->points[np].value =
                        lilv_node_as_float(lilv_scale_point_get_value(p));
                    id->points[np].label = strdup(
                        lilv_node_as_string(lilv_scale_point_get_label(p)));
                    ++np;
                }
            }
        }

        qsort(id->points, np, sizeof(ScalePoint),
              (int (*)(const void *, const void *))scale_point_cmp);
        id->n_points = np;
        lilv_scale_points_free(sp);
    }

    return id;
}

// what plug_plugin_process_rt does not connect each cycle: controls to their
// values, atom ports to their sequences, unknown ports to nothing
static void plug_ports_connect(PLUG_PLUG *plug) {
    for (uint32_t i = 0; i < plug->num_ports; i++) {
        PLUG_PORT *const cur_port = &(plug->ports[i]);
        if (cur_port->type == PORT_TYPE_CONTROL)
            lilv_instance_connect_port(plug->plug_instance, i,
                                       &(cur_port->control));
        if (cur_port->type == PORT_TYPE_EVENT) {
            plug_evbuf_reset(cur_port->evbuf,
                             cur_port->flow == PORT_FLOW_INPUT);
            lilv_instance_connect_port(plug->plug_instance, i,
                                       plug_evbuf_get_buffer(cur_port->evbuf));
        }
        if (cur_port->flow == PORT_FLOW_UNKNOWN ||
            cur_port->type == PORT_TYPE_UNKNOWN)
            lilv_instance_connect_port(plug->plug_instance, i, NULL);
    }
}

// [main-thread] a reference on the plugin's loaded library, NULL when it is
// not loaded. dlclose it
static void *plug_library_hold(const LilvPlugin *plugin) {
    char *path = lilv_file_uri_parse(
        lilv_node_as_uri(lilv_plugin_get_library_uri(plugin)), NULL);
    if (!path)
        return NULL;
    void *lib = dlopen(path, RTLD_LOCAL | RTLD_LAZY | RTLD_NOLOAD);
    lilv_free(path);
    return lib;
}

// [main-thread] a new instance at plug_data's sample rate and block length,
// with the old one's state. The plugin is stopped. -1 when instantiating
// failed - the plugin has no instance then
static int plug_reinstantiate(PLUG_INFO *plug_data, PLUG_PLUG *plug) {
    // the UI may access the old instance, gui_shown stays for the re-show
    plug_gui_destroy(plug, plug->gui_shown);
    // atoms for and from the old instance
    if (plug->gui_ui_to_rt) {
        zix_ring_reset(plug->gui_ui_to_rt);
        zix_ring_reset(plug->gui_rt_to_ui);
    }
    LilvState *state = lilv_state_new_from_instance(
        plug->plug, plug->plug_instance, &(plug->map), NULL, NULL, NULL, NULL,
        plug_get_port_value, plug, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
        NULL);
    // they hold the old instance, and requests made for it
    plug_workers_free(plug);
    if (plug->plug_instance_activated == 1) {
        lilv_instance_deactivate(plug->plug_instance);
        plug->plug_instance_activated = 0;
    }
    // freeing the last instance would unload the library, the new one load it
    // again
    void *lib = plug_library_hold(plug->plug);
    lilv_instance_free(plug->plug_instance);
    plug->plug_instance = lilv_plugin_instantiate(
        plug->plug, plug_data->sample_rate, plug->feature_list);
    if (lib)
        dlclose(lib);
    if (!plug->plug_instance) {
        if (state)
            lilv_state_free(state);
        return -1;
    }
    plug_workers_start(plug_data, plug);
    plug_ports_connect(plug);
    if (state) {
        const LV2_Feature *features[PLUG_RESTORE_FEATURES_MAX];
        plug_restore_features(plug, features);
        lilv_state_restore(state, plug->plug_instance, plug_set_value_direct,
                           plug, 0, features);
        lilv_state_free(state);
    }
    lilv_instance_activate(plug->plug_instance);
    plug->plug_instance_activated = 1;
    // its property params are read back from the new instance, which knows
    // no position yet
    atomic_store(&plug->request_update, true);
    plug->pos_seq = 0;
    return 0;
}

// [main-thread] the lv2:sampleRate controls' ranges for plug_data's rate. Their
// params clamp the value into the new range and send it to the plugin
static void plug_controls_rescale(PLUG_INFO *plug_data, PLUG_PLUG *plug) {
    for (unsigned int i = 0; i < plug->num_controls; i++) {
        PLUG_CONTROL *control = plug->controls[i];
        if (!control || control->type != PORT || !control->is_sample_rate)
            continue;
        plug_control_range_read(plug_data->lv_world, plug->plug, control,
                                plug_data->sample_rate);
        PARAM_T min = lilv_node_as_float(control->min);
        PARAM_T max = lilv_node_as_float(control->max);
        // a control's val_id is its index in controls
        // the host's value - the plugin gets the clamped one
        if (param_set_range(plug->plug_params, (int)i, min, max, false) != 0)
            continue;
        param_set_value(plug->plug_params, (int)i,
                        plug_control_increment(control, min, max), NULL,
                        Operation_SetIncr);
    }
}

int plug_audio_config_set(PLUG_INFO *plug_data, SAMPLE_T sample_rate,
                          uint32_t block_length) {
    if (!plug_data)
        return -1;
    bool rate_changed = (float)sample_rate != plug_data->sample_rate;
    if (!rate_changed && block_length == plug_data->block_length)
        return 0;
    // [audio-thread] reads block_length for a processing plugin, so every
    // plugin stops before it changes
    for (int i = 0; i < MAX_INSTANCES; i++) {
        PLUG_PLUG *plug = &(plug_data->plugins[i]);
        if (plug->plug_instance)
            context_sub_wait_for_stop(plug_data->control_data, (void *)plug);
    }
    plug_data->sample_rate = (float)sample_rate;
    plug_data->block_length = block_length;
    // a plugin gets both only at instantiate
    int result = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        PLUG_PLUG *plug = &(plug_data->plugins[i]);
        if (!plug->plug_instance)
            continue;
        if (plug_reinstantiate(plug_data, plug) != 0) {
            context_sub_send_msg(plug_data->control_data, (void *)plug_data,
                                 is_audio_thread,
                                 "%s could not be instantiated again, removed\n",
                                 plug->name);
            plug_remove_plug(plug_data, plug->id);
            result = -1;
            continue;
        }
        if (rate_changed)
            plug_controls_rescale(plug_data, plug);
        context_sub_wait_for_start(plug_data->control_data, (void *)plug);
        // the UI plug_reinstantiate closed
        if (plug->gui_shown && plug_gui_show(plug) != 0)
            plug_gui_shown_set(plug, false);
    }
    return result;
}

// the ports that are not control ports are graph ports, on the plugin's node.
// 0 on success
static int plug_ports_create(PLUG_INFO *plug_data, PLUG_PLUG *plug) {
    if (!plug_data)
        return -1;
    if (!plug)
        return -1;
    plug->node =
        graph_node_add(plug_data->graph, plug_data->owner_tag, plug->uid,
                       GRAPH_NODE_PLAIN, plug_plugin_process_rt, plug);
    if (!plug->node)
        return -1;
    const LV2_URID atom_Chunk = plug->map.map(
        plug->map.handle, lilv_node_as_string(plug_data->nodes.atom_Chunk));
    const LV2_URID atom_Sequence = plug->map.map(
        plug->map.handle, lilv_node_as_string(plug_data->nodes.atom_Sequence));
    //--------------------------------------------------
    // initialize the ports
    plug->num_ports = lilv_plugin_get_num_ports(plug->plug);
    plug->ports = (PLUG_PORT *)calloc(plug->num_ports, sizeof(PLUG_PORT));
    if (plug->ports == NULL) {
        return -1;
    }
    float *default_values = (float *)calloc(plug->num_ports, sizeof(float));
    if (!default_values) {
        return -1;
    }
    lilv_plugin_get_port_ranges_float(plug->plug, NULL, NULL, default_values);

    for (uint32_t i = 0; i < plug->num_ports; i++) {
        PLUG_PORT *const cur_port = &(plug->ports[i]);
        cur_port->lilv_port = lilv_plugin_get_port_by_index(plug->plug, i);
        cur_port->graph_port = NULL;
        cur_port->evbuf = NULL;
        atomic_init(&cur_port->seq_dropped, 0U);
        atomic_init(&cur_port->control_rt, 0.0f);
        cur_port->buf_size = 0;
        cur_port->index = i;
        cur_port->param_index = -1;
        cur_port->control = 0.0f;
        cur_port->flow = PORT_FLOW_UNKNOWN;
        cur_port->type = PORT_TYPE_UNKNOWN;
        cur_port->port_type_urid = 0;
        const LilvNode *sym =
            lilv_port_get_symbol(plug->plug, cur_port->lilv_port);
        uint64_t sym_key =
            intern_add(&plug->port_symbols, lilv_node_as_string(sym));
        intern_set_slot(&plug->port_symbols, sym_key, i);
        const unsigned int optional =
            lilv_port_has_property(plug->plug, cur_port->lilv_port,
                                   plug_data->nodes.lv2_connectionOptional);
        // a graph port is named by the symbol, unique within the plugin
        const char *port_name = lilv_node_as_string(sym);
        unsigned int port_io = PORT_FLOW_OUTPUT;
        // set if the port is input or output
        if (lilv_port_is_a(plug->plug, cur_port->lilv_port,
                           plug_data->nodes.lv2_InputPort)) {
            cur_port->flow = PORT_FLOW_INPUT;
            port_io = PORT_FLOW_INPUT;
        } else if (lilv_port_is_a(plug->plug, cur_port->lilv_port,
                                  plug_data->nodes.lv2_OutputPort)) {
            cur_port->flow = PORT_FLOW_OUTPUT;
        } else if (!optional) {
            free(default_values);
            return -1;
        }
        // set the port buffer size, before the event port's sequence is made
        LilvNode *min_size = lilv_port_get(plug->plug, cur_port->lilv_port,
                                           plug_data->nodes.rsz_minimumSize);
        if (min_size && lilv_node_is_int(min_size)) {
            cur_port->buf_size = lilv_node_as_int(min_size);
            plug_data->buffer_size = MAX(plug_data->buffer_size,
                                         cur_port->buf_size * N_BUFFER_CYCLES);
        }
        lilv_node_free(min_size);
        // Set port types
        // control port
        if (lilv_port_is_a(plug->plug, cur_port->lilv_port,
                           plug_data->nodes.lv2_ControlPort)) {
            cur_port->type = PORT_TYPE_CONTROL;
            cur_port->control =
                isnan(default_values[i]) ? 0.0f : default_values[i];
            // Create and add to the control array the PLUG_CONTROL for this
            // control port
            PLUG_CONTROL *record = new_port_control(
                plug_data->lv_world, plug->plug, cur_port->lilv_port, i,
                plug_data->sample_rate, &(plug_data->nodes), &(plug->forge));
            if (record) {
                // add control to the controls array
                plug->controls = (PLUG_CONTROL **)realloc(
                    plug->controls,
                    sizeof(PLUG_CONTROL *) * (plug->num_controls + 1));
                plug->controls[plug->num_controls] = record;
                cur_port->param_index = plug->num_controls;
                plug->num_controls += 1;
            }
        } else if (lilv_port_is_a(plug->plug, cur_port->lilv_port,
                                  plug_data->nodes.lv2_AudioPort)) {
            cur_port->type = PORT_TYPE_AUDIO;
            cur_port->graph_port =
                graph_port_create(plug_data->graph, plug->node, PORT_TYPE_AUDIO,
                                  port_io, port_name);
            if (!cur_port->graph_port) {
                free(default_values);
                return -1;
            }
        } else if (lilv_port_is_a(plug->plug, cur_port->lilv_port,
                                  plug_data->nodes.lv2_CVPort)) {
            cur_port->type = PORT_TYPE_CV;
            cur_port->graph_port =
                graph_port_create(plug_data->graph, plug->node, PORT_TYPE_AUDIO,
                                  port_io, port_name);
            if (!cur_port->graph_port) {
                free(default_values);
                return -1;
            }
        } else if (lilv_port_is_a(plug->plug, cur_port->lilv_port,
                                  plug_data->nodes.atom_AtomPort)) {
            cur_port->type = PORT_TYPE_EVENT;
            if (lilv_port_supports_event(plug->plug, cur_port->lilv_port,
                                         plug_data->nodes.midi_MidiEvent)) {
                cur_port->port_type_urid = plug_data->urids.midi_MidiEvent;
                cur_port->graph_port =
                    graph_port_create(plug_data->graph, plug->node,
                                      PORT_TYPE_MIDI, port_io, port_name);
                if (!cur_port->graph_port) {
                    free(default_values);
                    return -1;
                }
            }
            // ready the evbuf for the events
            //--------------------
            free(cur_port->evbuf);
            // the plugin's minimum, never less than a MIDI_BUF needs
            const size_t size =
                MAX(cur_port->buf_size, plug_data->midi_buf_size);
            cur_port->evbuf = plug_evbuf_new(size, atom_Chunk, atom_Sequence);
            if (!cur_port->evbuf) {
                free(default_values);
                return -1;
            }
            //--------------------
        }
        // if not optional but we dont know the type we cant load this plugin
        else if (!optional) {
            free(default_values);
            return -1;
        }
    }
    plug_ports_connect(plug);
    // lv2:reportsLatency or the lv2:latency designation
    plug->latency_port = -1;
    if (lilv_plugin_has_latency(plug->plug)) {
        uint32_t index = lilv_plugin_get_latency_port_index(plug->plug);
        if (index < plug->num_ports &&
            plug->ports[index].type == PORT_TYPE_CONTROL &&
            plug->ports[index].flow == PORT_FLOW_OUTPUT)
            plug->latency_port = (int)index;
    }
    // find the controling port index
    const LilvPort *control_input = lilv_plugin_get_port_by_designation(
        plug->plug, plug_data->nodes.lv2_InputPort,
        plug_data->nodes.lv2_control);
    if (control_input) {
        const uint32_t index = lilv_port_get_index(plug->plug, control_input);
        if (plug->ports[index].type == PORT_TYPE_EVENT) {
            plug->control_in = index;
        }
    }
    free(default_values);

    return 0;
}

static void plug_run_rt(PLUG_PLUG *plug, unsigned int nframes) {
    if (!plug)
        return;
    if (!plug->plug_instance)
        return;

    LilvInstance *plug_instance = plug->plug_instance;
    lilv_instance_run(plug_instance, nframes);

    // Process workers for the plugin
    LV2_Handle handle = lilv_instance_get_handle(plug->plug_instance);
    jalv_worker_emit_responses(plug->worker, handle);
    jalv_worker_emit_responses(plug->state_worker, handle);
    jalv_worker_end_run(plug->worker);
}

int plug_stop_and_remove_plug(void *plug) {
    PLUG_PLUG *cur_plug = (PLUG_PLUG *)plug;
    if (!cur_plug)
        return -1;
    if (!cur_plug->plug_data)
        return -1;
    // first stop the plugin process
    context_sub_wait_for_stop(cur_plug->plug_data->control_data,
                              (void *)cur_plug);
    return plug_remove_plug(cur_plug->plug_data, cur_plug->id);
}

// [audio-thread] the cycle's position as a time:Position object in buf, which
// 256 bytes hold. NULL when buf is too small
static const LV2_Atom *plug_position_forge_rt(PLUG_PLUG *plug,
                                              const TRANSPORT_POS *tr,
                                              uint8_t *buf, uint32_t size) {
    const PLUG_URIDS *urids = plug->urids;
    LV2_Atom_Forge *forge = &plug->forge;
    lv2_atom_forge_set_buffer(forge, buf, size);
    LV2_Atom_Forge_Frame frame;
    if (!lv2_atom_forge_object(forge, &frame, 0, urids->time_Position))
        return NULL;
    lv2_atom_forge_key(forge, urids->time_frame);
    lv2_atom_forge_long(forge, tr->frame);
    lv2_atom_forge_key(forge, urids->time_speed);
    lv2_atom_forge_float(forge, tr->rolling ? 1.0f : 0.0f);
    lv2_atom_forge_key(forge, urids->time_bar);
    lv2_atom_forge_long(forge, tr->music.bar);
    lv2_atom_forge_key(forge, urids->time_barBeat);
    lv2_atom_forge_float(forge,
                         (float)(tr->music.beat - tr->music.bar_start_beat));
    lv2_atom_forge_key(forge, urids->time_beatUnit);
    lv2_atom_forge_int(forge, (int32_t)tr->music.beat_type);
    lv2_atom_forge_key(forge, urids->time_beatsPerBar);
    lv2_atom_forge_float(forge, (float)tr->music.bpb);
    lv2_atom_forge_key(forge, urids->time_beatsPerMinute);
    if (!lv2_atom_forge_float(forge, (float)tr->music.bpm))
        return NULL;
    lv2_atom_forge_pop(forge, &frame);
    return (const LV2_Atom *)buf;
}

// [audio-thread] a property's value as the type it declares
static void plug_property_value_forge_rt(LV2_Atom_Forge *forge, LV2_URID type,
                                         PARAM_T value) {
    if (type == forge->Int)
        lv2_atom_forge_int(forge, (int32_t)lrint(value));
    else if (type == forge->Long)
        lv2_atom_forge_long(forge, (int64_t)llrint(value));
    else if (type == forge->Bool)
        lv2_atom_forge_bool(forge, value != 0);
    else if (type == forge->Double)
        lv2_atom_forge_double(forge, value);
    else
        lv2_atom_forge_float(forge, (float)value);
}

// a latency port's value in whole frames, NaN and below 1 as 0
static uint32_t plug_latency_frames(float value) {
    if (!(value >= 1.0f))
        return 0;
    if (value >= (float)UINT32_MAX)
        return UINT32_MAX;
    return (uint32_t)value;
}

bool plug_plugin_process_rt(void *plug_ptr, NFRAMES_T nframes) {
    PLUG_PLUG *plug = (PLUG_PLUG *)plug_ptr;
    if (!plug)
        return false;
    // do nothing if the plugin is stopped
    if (plug->is_processing == 0)
        return false;
    if (!plug->plug)
        return false;
    if (!plug->ports)
        return false;
    PLUG_INFO *plug_data = plug->plug_data;
    // instantiated for exactly block_length (fixedBlockLength) - a buffer size
    // change, until the new instance
    if (nframes != plug_data->block_length)
        return false;
    // taken before the shown flag is read, a show stores the flag first
    const bool update = atomic_exchange(&plug->request_update, false);
    const bool gui =
        atomic_load_explicit(&plug->gui_shown_rt, memory_order_acquire);

    // time:Position for every input event port, after a jump or when the frame
    // is not where the plugin's own count puts it (a server can hold its frame
    // a few cycles after a start)
    uint8_t pos_buf[256];
    const LV2_Atom *lv2_pos = NULL;
    const TRANSPORT_POS *tr = transport_get_rt(plug_data->transport);
    if (tr) {
        if (tr->seq != plug->pos_seq || tr->frame != plug->pos_frame_next)
            lv2_pos =
                plug_position_forge_rt(plug, tr, pos_buf, sizeof(pos_buf));
        plug->pos_seq = tr->seq;
        plug->pos_frame_next =
            tr->rolling ? tr->frame + (int64_t)nframes : tr->frame;
    }

    //----------------------------------------------------------------------------------------------------
    // first connect the ports for processing
    for (uint32_t i = 0; i < plug->num_ports; i++) {
        PLUG_PORT *const cur_port = &(plug->ports[i]);
        // an input's is read-only, LV2 does not write inputs
        SAMPLE_T *a_buffer = graph_port_audio_rt(cur_port->graph_port);
        if (cur_port->type == PORT_TYPE_AUDIO) {
            lilv_instance_connect_port(plug->plug_instance, i, a_buffer);
        }
        if (cur_port->type == PORT_TYPE_CV) {
            lilv_instance_connect_port(plug->plug_instance, i, a_buffer);
        }
        if (cur_port->type == PORT_TYPE_EVENT &&
            cur_port->flow == PORT_FLOW_INPUT) {
            // clean the evbuf
            plug_evbuf_reset(cur_port->evbuf, 1);
            PLUG_EVBUF_ITERATOR iter_buf = plug_evbuf_begin(cur_port->evbuf);
            if (update) {
                LV2_Atom_Forge_Frame frame;
                uint8_t buf[MSG_BUFFER_SIZE];
                lv2_atom_forge_set_buffer(&plug->forge, buf, sizeof(buf));
                lv2_atom_forge_object(&plug->forge, &frame, 0,
                                      plug_data->urids.patch_Get);
                const LV2_Atom *get =
                    lv2_atom_forge_deref(&plug->forge, frame.ref);
                plug_evbuf_write(&iter_buf, 0, 0, get->type, get->size,
                                 LV2_ATOM_BODY_CONST(get));
            }
            if (lv2_pos)
                plug_evbuf_write(&iter_buf, 0, 0, lv2_pos->type, lv2_pos->size,
                                 LV2_ATOM_BODY_CONST(lv2_pos));
        }
        if (cur_port->type == PORT_TYPE_EVENT &&
            cur_port->flow == PORT_FLOW_OUTPUT) {
            plug_evbuf_reset(cur_port->evbuf, 0);
        }
    }
    //----------------------------------------------------------------------------------------------------
    // go through controls and either set them direclty if its control port
    // parameter or send them to the event port as atoms if its a property
    // port
    for (unsigned int ctrl_iter = 0; ctrl_iter < plug->num_controls;
         ctrl_iter++) {
        int param_changed =
            param_get_if_changed_rt(plug->plug_params, ctrl_iter);
        if (param_changed != 1)
            continue;
        PLUG_CONTROL *cur_control = plug->controls[ctrl_iter];
        if (!cur_control)
            continue;
        if (!(cur_control->is_writable))
            continue;
        PARAM_T param_value = param_get_value(plug->plug_params, ctrl_iter, 1);

        if (cur_control->type == PORT) {
            uint32_t port_index = cur_control->index;
            if (port_index < plug->num_ports) {
                PLUG_PORT *const control_port = &(plug->ports[port_index]);
                if (!control_port)
                    continue;
                control_port->control = param_value;
            }
        }

        if (cur_control->type == PROPERTY) {
            if (plug->control_in < (int)plug->num_ports &&
                plug->control_in != -1 && plug->controls) {
                PLUG_PORT *const control_port =
                    &(plug->ports[plug->control_in]);
                if (!control_port)
                    continue;
                // after the frame 0 atoms already in it
                PLUG_EVBUF_ITERATOR iter_buf =
                    plug_evbuf_end(control_port->evbuf);

                // send the value to the event port as atom
                // Dont copy the forge, only use address here - so the forge
                // should only be used in rt thread or when rt thread is
                // paused (when initializing a plugin or preset etc.)
                LV2_Atom_Forge *forge = &(plug->forge);
                LV2_Atom_Forge_Frame frame;
                uint8_t buf[MSG_BUFFER_SIZE];
                lv2_atom_forge_set_buffer(forge, buf, sizeof(buf));
                lv2_atom_forge_object(forge, &frame, 0,
                                      plug_data->urids.patch_Set);
                lv2_atom_forge_key(forge, plug_data->urids.patch_property);
                lv2_atom_forge_urid(forge, cur_control->property);
                lv2_atom_forge_key(forge, plug_data->urids.patch_value);
                plug_property_value_forge_rt(forge, cur_control->value_type,
                                             param_value);
                lv2_atom_forge_pop(forge, &frame);
                const LV2_Atom *const atom = (const LV2_Atom *)buf;
                if (plug_evbuf_write(&iter_buf, 0, 0, atom->type, atom->size,
                                     LV2_ATOM_BODY_CONST(atom)) != 0)
                    atomic_fetch_add(&control_port->seq_dropped, 1U);
            }
        }
    }
    if (gui)
        plug_gui_atoms_in_rt(plug);

    //----------------------------------------------------------------------------------------------------
    // midi last, a sequence is time ordered and the atoms above are all at
    // frame 0
    for (uint32_t i = 0; i < plug->num_ports; i++) {
        PLUG_PORT *const cur_port = &(plug->ports[i]);
        if (cur_port->flow != PORT_FLOW_INPUT ||
            cur_port->type != PORT_TYPE_EVENT || !cur_port->graph_port)
            continue;
        PLUG_EVBUF_ITERATOR iter_buf = plug_evbuf_end(cur_port->evbuf);
        MIDI_BUF_ITER it =
            midi_buf_iter(graph_port_midi_rt(cur_port->graph_port));
        MIDI_EVENT ev;
        while (midi_buf_next(&it, &ev)) {
            if (plug_evbuf_write(&iter_buf, ev.frame, 0,
                                 cur_port->port_type_urid, ev.size,
                                 ev.data) != 0)
                atomic_fetch_add(&cur_port->seq_dropped, 1U);
        }
    }

    //----------------------------------------------------------------------------------------------------
    // now run the plugin for nframes
    plug_run_rt(plug, nframes);
    if (plug->latency_port >= 0)
        atomic_store(&plug->latency_rt,
                     plug_latency_frames(
                         plug->ports[plug->latency_port].control));

    //----------------------------------------------------------------------------------------------------
    // now update the output ports and any output controls (ui)
    for (uint32_t i = 0; i < plug->num_ports; i++) {
        PLUG_PORT *const cur_port = &(plug->ports[i]);
        if (cur_port->flow != PORT_FLOW_OUTPUT)
            continue;
        if (cur_port->type == PORT_TYPE_AUDIO) {
        }
        if (cur_port->type == PORT_TYPE_CV) {
        }
        // update the output parameters (these should be sent to ui for
        // display)
        if (cur_port->type == PORT_TYPE_CONTROL) {
            atomic_store(&cur_port->control_rt, cur_port->control);
            // set param but not too often to not swamp the rt_to_ui ring
            // buffer. it does not matter that the [audio-thread] parameter
            // will be set at the same speed, since it output parameter
            if (plug_data->rt_tick == 0) {
                // set the val on param container
                param_set_value_rt(plug->plug_params, cur_port->param_index,
                                   cur_port->control);
            }
        }
        if (cur_port->type == PORT_TYPE_EVENT) {
            // NULL for an atom port without MIDI
            MIDI_BUF *midi_out = graph_port_midi_rt(cur_port->graph_port);
            for (PLUG_EVBUF_ITERATOR iter_buf =
                     plug_evbuf_begin(cur_port->evbuf);
                 plug_evbuf_is_valid(iter_buf);
                 iter_buf = plug_evbuf_next(iter_buf)) {
                uint32_t frames = 0;
                uint32_t subframes = 0;
                LV2_URID type = 0;
                uint32_t size = 0;
                void *data = NULL;
                plug_evbuf_get(iter_buf, &frames, &subframes, &type, &size,
                               &data);
                if (midi_out && type == plug_data->urids.midi_MidiEvent)
                    midi_buf_push(midi_out, frames, data, size);
                // property values the plugin changed itself (a preset, its
                // own gui). A plugin with control port params has no way
                // to tell the host about such changes
                if (type == plug_data->urids.atom_Object &&
                    size >= sizeof(LV2_Atom_Object_Body))
                    plug_patch_message_rt(plug, data, size);
                if (gui && !plug_gui_ring_write(plug->gui_rt_to_ui, i, type,
                                                size, data))
                    atomic_fetch_add(&plug->gui_dropped, 1U);
            }
        }
    }
    param_rt_resend(plug->plug_params);
    return true;
}

void plug_clean_memory(PLUG_INFO *plug_data) {
    if (!plug_data)
        return;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        plug_remove_plug(plug_data, i);
    }

    if (plug_data->symap)
        symap_free(plug_data->symap);
    zix_sem_destroy(&(plug_data->symap_lock));
    for (LilvNode **n = (LilvNode **)&plug_data->nodes; *n; ++n) {
        lilv_node_free(*n);
    }
    if (plug_data->lv_world)
        lilv_world_free(plug_data->lv_world);

    context_sub_clean(plug_data->control_data);

    if (plug_data->plugin_list.plugin_list)
        free(plug_data->plugin_list.plugin_list);
    plug_data->plugin_list.plugin_list_count = 0;
    intern_clean(&plug_data->plugin_list.uris);
    free(plug_data);
}
