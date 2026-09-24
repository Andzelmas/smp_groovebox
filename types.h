#pragma once
#include "structs.h"

// the jack client this app registers as
#define APP_NAME "smp_grvbx"
// names for the various contexts
#define ROOT_NAME "grvbx"
#define PLUGINS_LV2_NAME "Lv2_Plugins"
#define PLUGINS_CLAP_NAME "Clap_Plugins"
#define SAMPLER_NAME "Sampler"
#define SYNTH_NAME "Synth"
#define TRK_NAME "Trk"
#define NAME_ADD_NEW "add_new"
#define NAME_REMOVE "remove"
#define NAME_REFRESH_LIST "refresh"

#define MAX_STRING_MSG_LENGTH 128 // max string size for sys messages

#define MAX_SHORT_NAME_LENGTH 256 // max length for a short display name (param, plugin, preset, CX...)

// max size for ring buffer arrays in sys messages between threads
#define MAX_SYS_BUFFER_ARRAY_SIZE 256

// in what interval the rt thread should give info to the ui thread to
// not overwhelm it.
#define RT_CYCLES 25

// how many midi events there can be in the jack midi container struct
#define MAX_MIDI_CONT_ITEMS 50

// max length for unique ids that use char* (for example the clap unique
// id for plugins)
#define MAX_UNIQUE_ID_STRING 128

// max length for the char* that has a filetype ("txt", "json" etc)
#define MAX_FILETYPE_STRING 20

// max length for a filepath
#define MAX_PATH_STRING 1024

// length for a string that contains the hex representation of a number
#define HEX_TYPES_CHAR_LENGTH 5

// length for a string that contains the int representation of a number
#define INT_TYPES_CHAR_LENGTH 20

// TODO not tested and not implemented, here for future development
// by default SAMPLE_T is float (in structs.h), if this is 1 SAMPLE_T will
// be double and audio buffers will use 64bits
#define SAMPLE_T_AS_DOUBLE 0

//Initial number of members for a malloced array of pointers
//This can be realloced later
#define PTR_ARRAY_COUNT 25

// wavetables
enum waveTablesType {
    SIN_WAVETABLE,
    TRIANGLE_WAVETABLE,
    SAW_WAVETABLE,
    SQUARE_WAVETABLE
};

enum FlowType { PORT_FLOW_UNKNOWN, PORT_FLOW_INPUT, PORT_FLOW_OUTPUT };

// the port type for the audio client
enum PortType {
    PORT_TYPE_UNKNOWN,
    PORT_TYPE_CONTROL,
    PORT_TYPE_AUDIO,
    PORT_TYPE_EVENT,
    PORT_TYPE_MIDI,
    PORT_TYPE_CV
};

enum MsgFromRT {
    MSG_DO_NOTHING = 0,
    // message to deactivate and activate the plugin,
    // first need to pause the process
    MSG_PLUGIN_RESTART = 1,
    // start processing the plugin, needs to be activated first
    MSG_PLUGIN_PROCESS = 2,
    // message to call a on_main_thread function in the main thread on
    // the plugin
    MSG_PLUGIN_REQUEST_CALLBACK = 3,
    // plugin side sent a debug string message
    MSG_PLUGIN_SENT_STRING = 4,
    // message that plugin needs to be activated on main thread and then
    // start_processing function called on the audio thread
    MSG_PLUGIN_ACTIVATE_PROCESS = 5,
    // message to stop processing the plugin
    MSG_PLUGIN_STOP_PROCESS = 6,
    // stop the whole context processing, usually done when
    // cleaning memory
    MSG_STOP_ALL = 7,
    // start the whole context again, usually done after
    // stopping before init of contexts
    MSG_START_ALL = 8
};

// this holds the subcontext data address (user_data) and the enum (from
// MSGfromRT) to tell what to do with the subcontext
typedef struct _ring_sys_msg {
    // what to do with the plugin
    unsigned int msg_enum;
    char msg[MAX_STRING_MSG_LENGTH];
    // user data for the function that gets called depending on
    // the msg. This can be a plugin address or a sample
    // address and etc.
    void *user_data;
} RING_SYS_MSG;
