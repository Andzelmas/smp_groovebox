#pragma once
#include <stdatomic.h>
#include "structs.h"
#include "types.h"
//sampler context
#include "contexts/sampler.h"
//plugin context
#include "contexts/plugins.h"
#include "contexts/clap_plugins.h"
#include "contexts/synth.h"

//enum for app init failures
enum AppStatus{
    app_failed_malloc = -1,
    smp_data_init_failed = -2,
    smp_jack_init_failed = -3,
    trk_jack_init_failed = -4,
    plug_data_init_failed = -5,
    plug_jack_init_failed = -6,
    synth_data_init_failed = -7,
    clap_plug_data_init_failed = -8
};
enum AppPluginType{
    //lv2 plugin
    LV2_plugin_type = 0x01,
    CLAP_plugin_type = 0x02
};

typedef enum AppStatus app_status_t;

//this is the struct for the data on the whole app
typedef struct _app_info APP_INFO;
//initialize the app_data
//sample_paths - paths to the sample files that should be loaded on the app init
//samples_num - how many samples we have
APP_INFO* app_init(app_status_t *app_status);
//return the available names for plugins on the system
char** app_plug_get_plugin_names(APP_INFO* app_data, unsigned int* names_size, unsigned char** return_plug_types);
//return the available names of the presets available for the plugin
char** app_plug_get_plugin_presets(APP_INFO* app_data, unsigned int indx, unsigned int* total_presets);
//initialize a plugin on the plug_data context
int app_plug_init_plugin(APP_INFO* app_data, const char* plugin_uri, unsigned char cx_type, const int id);
//load a new preset for the plugin
int app_plug_load_preset(APP_INFO* app_data, const char* preset_uri, unsigned int cx_type, const int plug_id);
//initialize a sample on sampler context
int app_smp_sample_init(APP_INFO* app_data, const char* samp_path, int in_id);
//call appropriate cx_type context function to set parameter value
int app_param_set_value(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, PARAM_T param_value, unsigned char param_op);
//return the parameter increment amount (by how much the value increases or decreases)
PARAM_T app_param_get_increment(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id);
//get value, the val_type returns what type of value for display purposes.
//curved == 1 the parameter should be returned from the curve_table if there is one on the parameter
PARAM_T app_param_get_value(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, unsigned char* val_type, unsigned int curved);
//get the parameter id given the context type, cx id and parameter id. Returns -1 if no such parameter is found
int app_param_id_from_name(APP_INFO* app_data, unsigned char cx_type, int cx_id, const char* param_name);
//get the string of the String_Return_Type parameter
const char* app_param_get_string(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id);
//returns all the parameters values, types and names to the param_names, param_vals and param_types arrays. User has to free these arrays
//if the current name is NULL something went wrong. Also if the parameter val is -1 and the param type is 0 something went wrong.
int app_param_return_all_as_string(APP_INFO* app_data, unsigned char cx_type, int cx_id, char*** param_names, char*** param_vals,
				   char*** param_types, unsigned int* param_num);
//returns all names of the cx of the context type, for example all names of the oscillators of the synth
const char** app_context_return_names(APP_INFO* app_data, unsigned char cx_type, int* cx_num);
//return the name of a context by its cx_id (for example the name of a sample by its id, or name of a plugin by its id)
//if add_id == 1 will add the id to the end of the string
char* app_return_cx_name(APP_INFO* app_data, unsigned char cx_type, int cx_id, unsigned int add_id);
//check if a port_name port exists on the audio client
int app_is_port_on_client(APP_INFO* app_data, const char* port_name);
//wrapper for disconnecting two ports
int app_disconnect_ports(APP_INFO* app_data, const char* source_port, const char* dest_port);
//wrapper to connect ports
int app_connect_ports(APP_INFO* app_data, const char* source_port, const char* dest_port);
//return all ports belonging to the system, not only for this client but physical ones too
const char** app_return_ports(APP_INFO* app_data, const char* name_pattern, unsigned int type_pattern,
			      unsigned long flags);
//remove the clien name from the full port name
char* app_return_short_port(APP_INFO* app_data, const char* full_port_name);
//realtime callback for the track context, as usual has to use only _rt functions
int trk_audio_process_rt(jack_nframes_t nframes, void *arg);
//Reads the rt_to_ui buffer and saves any context param values to their ui_params arrays.
int app_update_ui_params(APP_INFO* app_data);
//realtime callback for the plugin host data, as always only _rt functions inside
int plug_audio_process_rt(NFRAMES_T nframes, void *arg);
//remove subcontext, plugin, clap plugin or sample
int app_subcontext_remove(APP_INFO* app_data, unsigned char cx_type, int id);
//pause the [audio-thread] processing with a mutex and clean memory of the app_data
int app_stop_and_clean(APP_INFO *app_data);
