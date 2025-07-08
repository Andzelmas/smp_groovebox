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

enum AppPluginType{
    //lv2 plugin
    LV2_plugin_type = 0x01,
    CLAP_plugin_type = 0x02
};

typedef enum AppStatus app_status_t;

//this is the struct for the data on the whole app
typedef struct _app_info APP_INFO;

//initialize the app_data, user_data_type returns USER_DATA_T_ROOT type
//the function returns APP_INFO* cast to void*
void* app_init(uint16_t* user_data_type);
//get the idx child of the parent_data, if idx is out of bounds return NULL
void* app_data_child_return(void* parent_data, uint16_t parent_type, uint16_t* return_type, unsigned int idx);
//get the short_name for the user_data, depending on the user_data_type
const char* app_data_short_name_get(void* user_data, uint16_t user_data_type);

//return the available names for plugins on the system
char** app_plug_get_plugin_names(APP_INFO* app_data, unsigned int* names_size, unsigned char** return_plug_types);

//plugin preset functions --------------------------------------------------
//get the preset struct depending on the cx_type. This can be used to return the preset short name, path etc.
void* app_plug_presets_iterate(APP_INFO* app_data, unsigned char cx_type, unsigned int idx, uint32_t iter);
//return the preset short name
int app_plug_plugin_presets_get_short_name(APP_INFO* app_data, unsigned char cx_type, void* preset_info, char* return_name, uint32_t name_len);
//return the preset full path
int app_plug_plugin_presets_get_full_path(APP_INFO* app_data, unsigned char cx_type, void* preset_info, char* return_path, uint32_t path_len);
//return preset category per iter, if returns -1 something went wrong or category under the iter does not exist
int app_plug_plugin_presets_categories_iterate(APP_INFO* app_data, unsigned char cx_type, void* preset_info, char* cur_category, uint32_t cat_len, uint32_t iter);
//clean the struct returned by the app_plug_presets_iterate function
void app_plug_presets_clean(APP_INFO* app_data, unsigned char cx_type, void* preset_info);
//load a new preset for the plugin
int app_plug_load_preset(APP_INFO* app_data, const char* preset_uri, unsigned int cx_type, const int plug_id);
//--------------------------------------------------

//initialize a plugin on the plug_data context
int app_plug_init_plugin(APP_INFO* app_data, const char* plugin_uri, unsigned char cx_type, const int id);
//initialize a sample on sampler context
int app_smp_sample_init(APP_INFO* app_data, const char* samp_path, int in_id);
//call appropriate cx_type context function to set parameter value
int app_param_set_value(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, PARAM_T param_value, unsigned char param_op);
//return the parameter increment amount (by how much the value increases or decreases)
PARAM_T app_param_get_increment(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id);
//get value, the val_type returns what type of value for display purposes.
PARAM_T app_param_get_value(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id);
//get the display name on the parameter
int app_param_get_ui_name(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, char* name, uint32_t name_len);
//get the parameter id given the context type, cx id and parameter id. Returns -1 if no such parameter is found
int app_param_id_from_name(APP_INFO* app_data, unsigned char cx_type, int cx_id, const char* param_name);
//get the parameter value formated as ret_string to display to the ui
unsigned int app_param_get_value_as_string(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, char* ret_string, uint32_t string_len);
//returns all the parameters values and names to the param_names, param_vals. User has to free these arrays
//if the current name is NULL something went wrong.
int app_param_return_all_as_string(APP_INFO* app_data, unsigned char cx_type, int cx_id, char*** param_names, char*** param_vals, unsigned int* param_num);
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
void app_stop_and_clean(void* user_data, uint16_t type);
