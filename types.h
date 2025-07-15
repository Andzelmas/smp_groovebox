#pragma once
#include "structs.h"

#define APP_NAME "smp_grvbx"
#define PLUGINS_NAME "Plugins"
#define SAMPLER_NAME "Sampler"
#define SYNTH_NAME "Synth"
#define TRK_NAME "Trk"
#define NAME_ADD_NEW "add_new"
#define NAME_REFRESH_LIST "refresh"

#define MAX_STRING_MSG_LENGTH 128 //max string size for sys messages
#define MAX_PARAM_NAME_LENGTH 100 //the max length for param names
#define MAX_SYS_BUFFER_ARRAY_SIZE 256 //max size for ring buffer arrays in sys messages between threads
#define MAX_PARAM_RING_BUFFER_ARRAY_SIZE 1024 //max size for the parameter ring buffer messaging arrays
#define RT_CYCLES 25 //in what interval the rt thread should give info to the ui thread to not overwhelm it.
#define MAX_MIDI_CONT_ITEMS 50 //how many midi events there can be in the jack midi container struct
#define MAX_UNIQUE_ID_STRING 128 //max length for unique ids that use char* (for example the clap unique id for plugins)
#define MAX_FILETYPE_STRING 20 //max length for the char* that has a filetype ("txt", "json" etc)
#define MAX_PATH_STRING 1024 //max length for a filepath
#define HEX_TYPES_CHAR_LENGTH 5 //length for a string that contains the hex representation of a number 
#define INT_TYPES_CHAR_LENGTH 20 //length for a string that contains the int representation of a number

//TODO not tested and not implemented, here for future development
#define SAMPLE_T_AS_DOUBLE 0//by default SAMPLE_T is float (in structs.h), if this is 1 SAMPLE_T will be double and audio buffers will use 64bits

enum userDataTypes{
    USER_DATA_T_ROOT = 1, //the root data, void* user_data casts to APP_INFO* struct
    //the Plugins context
    USER_DATA_T_PLUGINS = 2, //data that contains plugins, user_data casts to APP_INFO*
    USER_DATA_T_PLUGINS_NEW = 3, //data that contains a list to create new plugins, user_data casts to APP_INFO*
    USER_DATA_T_PLUGINS_LIST_REFRESH = 4, //data that is used to recreate the plugin list available on this machine for the user
    USER_DATA_T_PLUGINS_LV2_LIST_ITEM = 5, //data representing the lv2 plugin item that is available to load on the system
    USER_DATA_T_PLUG_LV2 = 6, //lv2 plugin data type
    USER_DATA_T_PLUG_CLAP = 7,//clap plugin data type
    //the Sampler context
    USER_DATA_T_SAMPLER = 8, //sampler data type, user_data casts to APP_INFO*
    USER_DATA_T_SAMPLE = 9, //single sample
    //the Synth context
    USER_DATA_T_SYNTH = 10, //built in synth, user_data casts to APP_INFO*
    USER_DATA_T_OSC = 11, //single oscillator in the synth
    //the Trk, audio backend context
    USER_DATA_T_JACK = 12 //audio backend data, user_data casts to APP_INFO*
};

enum intrfFlags{
    INTRF_FLAG_CONTAINER = 1 << 0, //context that contains other contexts, this will be the most common context
    INTRF_FLAG_ROOT = 1 << 1, //the parent of all contexts on the app, exiting this context closes the app
    INTRF_FLAG_CANT_DIRTY = 1 << 2, //if this context parent becomes dirty, dont remove this children. This context will be recreated only if the parent is removed.
    INTRF_FLAG_INTERACT = 1 << 3, //the user can interact with the context not only to see its children, but to for example press a button
    INTRF_FLAG_DISPLAY_NAME_DYN = 1 << 4, //name of this context should be returned by a data_short_name_get() every time when the context is displayed, because it might change at anytime
    INTRF_FLAG_LIST = 1 << 5, //this context is a list - it has _LIST_ITEM or _LIST children in it (among others)
    INTRF_FLAG_LIST_ITEM = 1 << 6, //this context is a list item
    INTRF_FLAG_ON_TOP = 1 << 7, //this context should be reachable for the user, even when navigating this contexts parents other children. For example a "delete" button when browsing files
    INTRF_FLAG_INV_FROM_FILE_NEW = 1 << 8, //whatever the data is doing with this user_data on invoke it needs a file name from the user. Data will try to create this file
    INTRF_FLAG_INV_FROM_FILE_EXISTING = 1 << 9, //whatever the data is doing with this user_data on invoke it needs an existing file name already present on disk
    INTRF_FLAG_UPDATE_CHILDREN = 1 << 10, //after creating all the children for this context a data_update_children() function needs to be called, so data can update this structure (possibly from a user config file)
    INTRF_FLAG_VAL = 1 << 11 //this context is some kind of a parameter and its values can be changed with data_val_change()
};

//enum for context types
//TODO userDataTypes replaces these, will need to delete
enum appContextTypes{
    Context_type_Sampler = 0x01,
    Context_type_Plugins = 0x02,
    Context_type_Trk = 0x03,
    Context_type_Synth = 0x04,
    Context_type_PortContainer = 0x05, //Audio or midi output ports container context
    Context_type_Clap_Plugins = 0x06 //Plugins but for CLAP plugins
};

//enum for value return types
enum appReturnType{
    Uchar_type = 0x01,
    Int_type = 0x02,
    Float_type = 0x03,
    DB_Return_Type = 0x04, // returned value should be displayed as db, so converted to log scale
    String_Return_Type = 0x05, //returned value is a string, will need to use a param function to return it
    Curve_Float_Return_Type = 0x06 //float that should be presented to the user as a special curve, from curve table on the parameter
};

//enum for operations on parameters types
enum paramOperType{
    Operation_Nothing = 0x00,
    Operation_Decrease = 0x01,
    Operation_Increase = 0x02,
    Operation_SetValue = 0x03,
    Operation_DefValue = 0x04, //set the value of the parameter to the default value
    Operation_SetIncr = 0x05, //set the increment of the parameter to this value
    Operation_SetDefValue = 0x06, // set the default value to this value
    Operation_ChangeName = 0x07, // change the name of the parameter
    Operation_ToggleHidden = 0x08 //value should change if the parameter is hidden or not
};

//wavetables
enum waveTablesType{
    SIN_WAVETABLE, 
    TRIANGLE_WAVETABLE,
    SAW_WAVETABLE,
    SQUARE_WAVETABLE
};

//parameter and ring buffer realtime values
enum paramRealtimeType{
    UI_PARAM_E = 0,
    RT_PARAM_E = 1,
    UI_TO_RT_RING_E = 0,
    RT_TO_UI_RING_E = 1
};

enum FlowType{
    PORT_FLOW_UNKNOWN,
    PORT_FLOW_INPUT,
    PORT_FLOW_OUTPUT
};
//the port type for the audio client
enum PortType{
    PORT_TYPE_UNKNOWN,
    PORT_TYPE_CONTROL,
    PORT_TYPE_AUDIO,
    PORT_TYPE_EVENT,
    PORT_TYPE_MIDI,
    PORT_TYPE_CV
};    

enum MsgFromRT{
    MSG_DO_NOTHING = 0,
    MSG_PLUGIN_RESTART = 1, //message to deactivate and activate the plugin, first need to pause the process
    MSG_PLUGIN_PROCESS = 2, //start processing the plugin, needs to be activated first
    MSG_PLUGIN_REQUEST_CALLBACK = 3, //message to call a on_main_thread function in the main thread on the plugin
    MSG_PLUGIN_SENT_STRING = 4, //plugin side sent a debug string message
    MSG_PLUGIN_ACTIVATE_PROCESS = 5, //message that plugin needs to be activated on main thread and then start_processing function called on the audio thread
    MSG_PLUGIN_STOP_PROCESS = 6, //message to stop processing the plugin
    MSG_STOP_ALL = 7, //stop the whole context processing, usually done when cleaning memory
    MSG_START_ALL = 8 //start the whole context again, usually done after stopping before init of contexts
};
//this holds the subcontext data address (user_data) and the enum (from MSGfromRT) to tell what to do with the subcontext
typedef struct _ring_sys_msg{
    unsigned int msg_enum; //what to do with the plugin
    char msg[MAX_STRING_MSG_LENGTH];
    void* user_data; //user data for the function that gets called depending on the msg. This can be a plugin address or a sample address and etc.
}RING_SYS_MSG;

//Parameter ring data struct. A message to manipulate the parameter
typedef struct _app_param_ring_data_bit{
    //the parameter id of the object.
    int param_id;
    //the parameter value to what to set the parameter or what the parameter value is now
    PARAM_T param_value;
    //this can be used to send a new name to the parameter for example
    char param_string[MAX_PARAM_NAME_LENGTH];
    //what to do with parameter? check paramOperType
    unsigned char param_op;
}PARAM_RING_DATA_BIT;
