#pragma once
#define MAX_STRING_MSG_LENGTH 128 //max string size for sys messages
#define MAX_SYS_BUFFER_ARRAY_SIZE 256 //max size for ring buffer arrays in sys messages between threads
#define MAX_PARAM_RING_BUFFER_ARRAY_SIZE 2048 //max size for the parameter ring buffer messaging arrays
#define RT_CYCLES 25 //in what interval the rt thread should give info to the ui thread to not overwhelm it.

//TODO not tested and not implemented, here for future development
#define SAMPLE_T_AS_DOUBLE 0//by default SAMPLE_T is float (in structs.h), if this is 1 SAMPLE_T will be double and audio buffers will use 64bits

//enum for context types
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
    Operation_SetDefValue = 0x06 // set the default value to this value
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
//stream flow directions
/*
enum FlowType{
    FLOW_INPUT = 0x1,
    FLOW_OUTPUT = 0x2
};
*/
enum FlowType{
    FLOW_UNKNOWN,
    FLOW_INPUT,
    FLOW_OUTPUT
};
//the port type for the audio client
enum PortType{
    TYPE_UNKNOWN,
    TYPE_CONTROL,
    TYPE_AUDIO,
    TYPE_EVENT,
    TYPE_MIDI,
    TYPE_CV
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
//this holds the subcontext id (plugin for example) and the enum (from MSGfromRT) to tell what to do with the subcontext
typedef struct _ring_sys_msg{
    unsigned int msg_enum; //what to do with the plugin
    char msg[MAX_STRING_MSG_LENGTH];
    int scx_id; //subcontext id on the context array that needs to be changed somehow
}RING_SYS_MSG;

//Parameter ring data struct. A message to manipulate the parameter
typedef struct _app_param_ring_data_bit{
    //the id of the object in the contex, a sample, track or plugin id or similar.
    int cx_id;
    //the parameter id of the object.
    int param_id;
    //the parameter value to what to set the parameter or what the parameter value is now
    float param_value;
    //what to do with parameter? check paramOperType
    unsigned char param_op;
}PARAM_RING_DATA_BIT;
