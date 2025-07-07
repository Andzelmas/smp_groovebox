#include "clap_plugins.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <threads.h>
#include <stdatomic.h>
#include "../util_funcs/log_funcs.h"
#include <clap/clap.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <semaphore.h>
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/string_funcs.h"
#include "../util_funcs/ring_buffer.h"
#include "../util_funcs/uniform_buffer.h"
#include "../util_funcs/math_funcs.h"
#include "../types.h"
#include "context_control.h"
#include "../jack_funcs/jack_funcs.h"

#include "clap_ext/clap_ext_preset_factory.h"

//what is the size of the buffer to get the formated param values to
#define MAX_VALUE_LEN 64
#define CLAP_PATH "/usr/lib/clap/"
//how many clap plugins can there be in the plugin array
#define MAX_INSTANCES 5

//size for the event lists
#define EVENT_LIST_SIZE 2048
#define EVENT_LIST_ITEMS 50

static thread_local bool is_audio_thread = false;
//struct that has the plugin preset info
typedef struct _clap_plug_preset_info{
    char short_name[MAX_PARAM_NAME_LENGTH]; //the short name, without any extensions or path symbols
    char full_path[MAX_PATH_STRING]; //the full path of the plugin preset
    char categories[MAX_PATH_STRING]; //the categories path, separated by /
}CLAP_PLUG_PRESET_INFO;

//sys port struct, that holds info about the port and a backend audio client port equivalent
typedef struct _clap_plug_port_sys{
    uint32_t channel_count; //how many channels on one clap plugin port (the sys_ports array will be the same size)
    void** sys_ports; //sys_port array per port channel - this will be exposed to the system, where user can connect other ports
}CLAP_PLUG_PORT_SYS;
//port struct that holds the sys port array and clap_audio_buffer_t array
typedef struct _clap_plug_port{
    uint32_t ports_count;
    CLAP_PLUG_PORT_SYS* sys_port_array;
    clap_audio_buffer_t* audio_ports;
}CLAP_PLUG_PORT;
//note port struct holds the audio client sys ports and info about the ports
typedef struct _clap_plug_note_port{
    uint32_t ports_count;
    void** sys_ports;
    clap_id* ids;
    uint32_t* supported_dialects;
    uint32_t* preferred_dialects;
    JACK_MIDI_CONT* midi_cont;
}CLAP_PLUG_NOTE_PORT;
//the single clap plugin struct
typedef struct _clap_plug_plug{
    int id; //plugin id on the clap_plug_info plugin array
    char plugin_id[MAX_UNIQUE_ID_STRING]; //unique plugin id that is from the clap_plugin_descriptor. Used rarely (now to match if preset container from preset-factory is can be used with the plugin)
    clap_plugin_entry_t* plug_entry; //the clap library file for this plugin
    const clap_plugin_t* plug_inst; //the plugin instance
    unsigned int plug_inst_created; //was a init function called in the descriptor for this plugin
    unsigned int plug_inst_activated; //was the activate function called on this plugin
    //0 - not processing, 1 - processing, 2 - sleeping (not processing, but [audio-thread] keeps sending params and checking if input events or input audio ir not_quiet)
    unsigned int plug_inst_processing; //is the plugin instance processing, touch only on [audio_thread]
    int plug_inst_id; //the plugin instance index in the array of the plugin factory
    char* plug_path; //the path for the clap file
    PRM_CONTAIN* plug_params; //plugin parameter container for params.c
    clap_host_t clap_host_info; //need when creating the plugin instance, this struct has this CLAP_PLUG_PLUG in the host_data var as (void*)
    CLAP_PLUG_INFO* plug_data; //CLAP_PLUG_INFO struct address for convenience
    //CLAP_PLUG_PORT holds an array of sys backend audio ports (to connect to jack for example) and the clap_audio_buffer_t arrays, to send to plugin process function
    CLAP_PLUG_PORT input_ports;
    CLAP_PLUG_PORT output_ports;
    //input and output note ports
    CLAP_PLUG_NOTE_PORT input_note_ports;
    CLAP_PLUG_NOTE_PORT output_note_ports;
    //input output event streams
    clap_input_events_t input_events;
    clap_output_events_t output_events;
    //preset factory struct for the plugin internal preset system (extension preset-factory)
    //this can be NULL, if the plugin has a preset-factory this will be created when clap_plug_presets_iterate or clap_plug_preset_load_from_path function is called
    CLAP_EXT_PRESET_FACTORY* preset_fac;
}CLAP_PLUG_PLUG;

//the main clap struct
typedef struct _clap_plug_info{
    struct _clap_plug_plug plugins[MAX_INSTANCES+1]; //array with single clap plugins
    SAMPLE_T sample_rate;
    //for clap there can be min and max buffer sizes, for not changing buffer sizes set as the same
    uint32_t min_buffer_size;
    uint32_t max_buffer_size;
    //placeholder for the host data that plugins send to the host, this has clap_host_info.host_data = NULL,
    //the clap_host_t has the CLAP_PLUG_PLUG* plug in clap_host_info.host_data. This is just for convenience - to copy to plug clap_host_info the necessary info.
    clap_host_t clap_host_info;
    //control_data struct that handles the sys messages between [audio-thread] and [main-thread] (stop plugin, start plugin, etc.)
    CXCONTROL* control_data;
    //from here hold various host extension implementation structs
    clap_host_thread_check_t ext_thread_check; //struct that holds functions to check if the thread is main or audio
    clap_host_log_t ext_log; //struct that holds function for logging messages (severity is not sent)
    clap_host_audio_ports_t ext_audio_ports; //struct that holds functions for the audio_ports extensions
    clap_host_note_ports_t ext_note_ports; //struct that holds functions for the note-ports extension
    clap_host_params_t ext_params; //struct that holds functions for the params extension
    clap_host_preset_load_t ext_preset_load; //struct that holds functions for the preset-load extension
    //address of the audio client
    void* audio_backend;
}CLAP_PLUG_INFO;

//return the clap_plug_plug id on the plugins array that has the same plug_entry (initiated) if no plugin has this plug_entry return -1
static int clap_plug_return_plug_id_with_same_plug_entry(CLAP_PLUG_INFO* plug_data, clap_plugin_entry_t* plug_entry){
    int return_id = -1;
    if(!plug_data)return -1;
    if(!plug_entry)return -1;

    for(unsigned int plug_num = 0; plug_num < MAX_INSTANCES; plug_num++){
	CLAP_PLUG_PLUG* cur_plug = &(plug_data->plugins[plug_num]);
	if(!cur_plug)continue;
	if(cur_plug->plug_entry != plug_entry)continue;
	return_id = cur_plug->id;
	break;
    }
    
    return return_id;
}
static int clap_plug_destroy_sys_ports(CLAP_PLUG_INFO* plug_data, CLAP_PLUG_PORT_SYS* ports_sys, uint32_t port_count){
    if(!plug_data)return -1;
    if(!ports_sys)return -1;
    for(uint32_t i = 0; i < port_count; i++){
	CLAP_PLUG_PORT_SYS* cur_sys_port = &(ports_sys[i]);
	uint32_t channels = cur_sys_port->channel_count;
	cur_sys_port->channel_count = 0;
	if(!cur_sys_port->sys_ports)continue;
	for(uint32_t chan = 0; chan < channels; chan ++){
	    if(!cur_sys_port->sys_ports[chan])continue;
	    app_jack_unregister_port(plug_data->audio_backend, cur_sys_port->sys_ports[chan]);
	}
	free(cur_sys_port->sys_ports);
    }
    free(ports_sys);
    return 0;
}
static int clap_plug_destroy_audio_ports(clap_audio_buffer_t* ports_audio, uint32_t port_count){
    if(!ports_audio)return -1;
    for(uint32_t i = 0; i < port_count; i++){
	clap_audio_buffer_t* cur_audio_port = &(ports_audio[i]);
	uint32_t channels = cur_audio_port->channel_count;
	cur_audio_port->channel_count = 0;
	if(cur_audio_port->data32){
	    for(uint32_t chan = 0; chan < channels; chan++){
		if(cur_audio_port->data32[chan])free(cur_audio_port->data32[chan]);
	    }
	    free(cur_audio_port->data32);
	}
	if(cur_audio_port->data64){
	    for(uint32_t chan = 0; chan < channels; chan++){
		if(cur_audio_port->data64[chan])free(cur_audio_port->data64[chan]);
	    }
	    free(cur_audio_port->data64);
	}
    }
    free(ports_audio);
    return 0;
}
static int clap_plug_destroy_ports(CLAP_PLUG_INFO* plug_data, CLAP_PLUG_PORT* port){
    if(!plug_data)return -1;
    if(!port)return -1;
    clap_plug_destroy_sys_ports(plug_data, port->sys_port_array, port->ports_count);
    port->sys_port_array = NULL;
    clap_plug_destroy_audio_ports(port->audio_ports, port->ports_count);
    port->audio_ports = NULL;
    port->ports_count = 0;
    return 0;
}

static int clap_plug_port_name_create(int name_size, char* full_name, int plug_id, const char* plug_inst_name, const char* port_name, int chan_num){
    if(!plug_inst_name || !port_name)return -1;
    if(plug_id < 0)return -1;
    if(name_size <= 0)return -1;
    if(!full_name)return -1;
    if(chan_num < 0){
	snprintf(full_name, name_size, "%.2d_%s_|%s", plug_id, plug_inst_name, port_name);
	return 0;
    }
    snprintf(full_name, name_size, "%.2d_%s_|%s_%d", plug_id, plug_inst_name, port_name, chan_num);
    return 0;
}
static int clap_plug_ports_rename(CLAP_PLUG_INFO* plug_data, CLAP_PLUG_PLUG* plug, CLAP_PLUG_PORT* ports, int input_ports){
    if(!plug_data)return -1;
    if(!plug)return -1;
    if(!ports)return -1;
    if(!plug->plug_inst)return -1;
    int port_name_size = app_jack_port_name_size();
    if(port_name_size <= 0)return -1;
    //TODO in clap plugin source code warns to scan ports only if the plugin is deactivated, but the rescan flag of rename allows to rescan the ports right away?
    const clap_plugin_audio_ports_t* clap_plug_ports = plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_AUDIO_PORTS);
    if(!clap_plug_ports)return -1;
    
    uint32_t port_count = ports->ports_count;
    if(port_count != clap_plug_ports->count(plug->plug_inst, input_ports))return -1;
    
    for(uint32_t i = 0; i < port_count; i++){
	CLAP_PLUG_PORT_SYS cur_sys_port = ports->sys_port_array[i];
	if(!cur_sys_port.sys_ports)continue;
	clap_audio_port_info_t port_info;
	if(!clap_plug_ports->get(plug->plug_inst, i, 0, &port_info))continue;
	for(uint32_t chan = 0; chan < cur_sys_port.channel_count; chan++){
	    char full_port_name[port_name_size];
	    if(clap_plug_port_name_create(port_name_size, full_port_name, plug->id, plug->plug_inst->desc->name, port_info.name, chan) != 0)continue;
	    if(app_jack_port_rename(plug_data->audio_backend, cur_sys_port.sys_ports[chan], full_port_name) != 0)continue;
	}
    }
    return 0;
}
static int clap_plug_create_ports(CLAP_PLUG_INFO* plug_data, int id, CLAP_PLUG_PORT* port, int input_ports){
    if(!plug_data)return -1;
    if(!port)return -1;
    if(id >= MAX_INSTANCES || id < 0)return -1;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
    if(plug->plug_inst_created != 1)return -1;
    if(!plug->plug_inst)return -1;
    int port_name_size = app_jack_port_name_size();
    if(port_name_size <= 0)return -1;
    
    const clap_plugin_audio_ports_t* clap_plug_ports = plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_AUDIO_PORTS);
    if(!clap_plug_ports)return -1;
    
    uint32_t clap_ports_count = clap_plug_ports->count(plug->plug_inst, input_ports);
    if(clap_ports_count <= 0)return 0;
    //create the sys port and clap audio buffer port arrays
    port->sys_port_array = malloc(sizeof(CLAP_PLUG_PORT_SYS) * clap_ports_count);
    if(!port->sys_port_array)return -1;
    port->audio_ports = malloc(sizeof(clap_audio_buffer_t) * clap_ports_count);
    if(!port->audio_ports){
	clap_plug_destroy_sys_ports(plug_data, port->sys_port_array, 0);
	port->sys_port_array = NULL;
	return -1;
    }
    
    for(uint32_t i = 0; i < clap_ports_count; i++){
	CLAP_PLUG_PORT_SYS* cur_sys_port = &(port->sys_port_array[i]);
	cur_sys_port->channel_count = 0;
	cur_sys_port->sys_ports = NULL;
	clap_audio_buffer_t* cur_clap_port = &(port->audio_ports[i]);
	cur_clap_port->channel_count = 0;
	cur_clap_port->constant_mask = 0;
	cur_clap_port->data32 = NULL;
	cur_clap_port->data64 = NULL;
	cur_clap_port->latency = 0;
	
	clap_audio_port_info_t port_info;
	if(!clap_plug_ports->get(plug->plug_inst, i, input_ports, &port_info))continue;
	uint32_t channels = port_info.channel_count;
	
	//create data for the backend audio client ports
	cur_sys_port->sys_ports = malloc(sizeof(void*) * channels);
	if(cur_sys_port->sys_ports){
	    for(uint32_t chan = 0; chan < channels; chan++){
		cur_sys_port->sys_ports[chan] = NULL;
		char full_port_name[port_name_size];
		if(clap_plug_port_name_create(port_name_size, full_port_name, id, plug->plug_inst->desc->name, port_info.name, chan) != 0)continue;
		unsigned int io_flow = 0x2;
		if(input_ports == 1)io_flow = 0x1;
		cur_sys_port->sys_ports[chan] = app_jack_create_port_on_client(plug_data->audio_backend, TYPE_AUDIO, io_flow, full_port_name);
		if(!cur_sys_port->sys_ports[chan])continue;
	    }
	    cur_sys_port->channel_count = channels;
	}

	//create data for the clap_audio_buffer
	cur_clap_port->data64 = NULL;
	cur_clap_port->data32 = malloc(sizeof(float*) * channels);
	if(cur_clap_port->data32){
	    for(uint32_t chan = 0; chan < channels; chan++){
		//create buffer for float32
		cur_clap_port->data32[chan] = calloc(plug_data->max_buffer_size, sizeof(float));
	    }
	    if((port_info.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) == CLAP_AUDIO_PORT_SUPPORTS_64BITS){
		cur_clap_port->data64 = malloc(sizeof(double*) * channels);
		if(cur_clap_port->data64){
		    for(uint32_t chan = 0; chan < channels; chan++){
			cur_clap_port->data64[chan] = calloc(plug_data->max_buffer_size, sizeof(double));
		    }
		}
	    }
	    cur_clap_port->constant_mask = 0;
	    cur_clap_port->latency = 0;
	    cur_clap_port->channel_count = channels;
	}
    }
    port->ports_count = clap_ports_count;
    return 0;
}
//rename the note ports
static int clap_plug_note_ports_rename(CLAP_PLUG_INFO* plug_data, CLAP_PLUG_PLUG* plug, bool input_ports){
    if(!plug_data)return -1;
    if(!plug)return -1;
    if(!plug->plug_inst)return -1;
    int port_name_size = app_jack_port_name_size();
    if(port_name_size <= 0)return -1;
    //TODO in clap plugin source code warns to scan ports only if the plugin is deactivated, but the rescan flag of rename allows to rescan the ports right away?
    const clap_plugin_note_ports_t* clap_note_ports = plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_NOTE_PORTS);
    if(!clap_note_ports)return -1;

    CLAP_PLUG_NOTE_PORT cur_port = plug->output_note_ports;
    if(input_ports)cur_port = plug->input_note_ports;
    uint32_t port_count = cur_port.ports_count;
    if(port_count != clap_note_ports->count(plug->plug_inst, input_ports))return -1;
    
    for(uint32_t i = 0; i < port_count; i++){
	void* cur_sys_port = cur_port.sys_ports[i];
	if(!cur_sys_port)continue;
	clap_note_port_info_t note_port_info;
	if(!clap_note_ports->get(plug->plug_inst, i, input_ports, &note_port_info))continue;
	char full_port_name[port_name_size];
	if(clap_plug_port_name_create(port_name_size, full_port_name, plug->id, plug->plug_inst->desc->name, note_port_info.name, -1) != 0)continue;
	app_jack_port_rename(plug_data->audio_backend, cur_sys_port, full_port_name);
    }
    return 0;
}
//clear note_port memory
static int clap_plug_note_ports_destroy(CLAP_PLUG_INFO* plug_data, CLAP_PLUG_NOTE_PORT* note_port){
    if(!plug_data)return -1;
    if(!note_port)return -1;

    for(uint32_t i = 0; i < note_port->ports_count; i++){
	void* cur_sys_port = note_port->sys_ports[i];
	if(cur_sys_port)
	    app_jack_unregister_port(plug_data->audio_backend, cur_sys_port);
	if(note_port->ids)free(note_port->ids);
	note_port->ids = NULL;
	if(note_port->preferred_dialects)free(note_port->preferred_dialects);
	note_port->preferred_dialects = NULL;
	if(note_port->supported_dialects)free(note_port->supported_dialects);
	note_port->supported_dialects = NULL;
    }
    note_port->ports_count = 0;
    if(note_port->sys_ports)free(note_port->sys_ports);
    note_port->sys_ports = NULL;
    if(note_port->midi_cont){
	app_jack_clean_midi_cont(note_port->midi_cont);
	free(note_port->midi_cont);
	note_port->midi_cont = NULL;
    }
    return 0;
}
//create note_ports
static int clap_plug_note_ports_create(CLAP_PLUG_INFO* plug_data, int id, bool input_ports){
    if(!plug_data)return -1;
    if(id >= MAX_INSTANCES || id < 0)return -1;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
    if(plug->plug_inst_created != 1)return -1;
    if(!plug->plug_inst)return -1;
    int port_name_size = app_jack_port_name_size();
    if(port_name_size <= 0)return -1;
    
    const clap_plugin_note_ports_t* clap_plug_note_ports = plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_NOTE_PORTS);
    if(!clap_plug_note_ports)return -1;
    
    uint32_t clap_ports_count = clap_plug_note_ports->count(plug->plug_inst, input_ports);
    if(clap_ports_count <= 0)return 0;
    //create the sys port pointer and other arrays per note port
    CLAP_PLUG_NOTE_PORT* note_port = &(plug->output_note_ports);
    if(input_ports)note_port = &(plug->input_note_ports);

    note_port->midi_cont = app_jack_init_midi_cont(MAX_MIDI_CONT_ITEMS);
    note_port->sys_ports = calloc(clap_ports_count, sizeof(void*));
    note_port->ports_count = clap_ports_count;
    note_port->ids = calloc(clap_ports_count, sizeof(clap_id));
    note_port->supported_dialects = calloc(clap_ports_count, sizeof(uint32_t));
    note_port->preferred_dialects = calloc(clap_ports_count, sizeof(uint32_t));
    if(!note_port->sys_ports || !note_port->ids || !note_port->supported_dialects || !note_port->preferred_dialects){
	clap_plug_note_ports_destroy(plug_data, note_port);
	return -1;
    }
    
    for(uint32_t i = 0; i < clap_ports_count; i++){
	clap_note_port_info_t note_port_info;
	if(!clap_plug_note_ports->get(plug->plug_inst, i, input_ports, &note_port_info))continue;
	char full_port_name[port_name_size];
	if(clap_plug_port_name_create(port_name_size, full_port_name, id, plug->plug_inst->desc->name, note_port_info.name, -1) != 0)continue;
	unsigned int io_flow = 0x2;
	if(input_ports == 1)io_flow = 0x1;
	note_port->sys_ports[i] = app_jack_create_port_on_client(plug_data->audio_backend, TYPE_MIDI, io_flow, full_port_name);
	note_port->ids[i] = note_port_info.id;
	note_port->preferred_dialects[i] = note_port_info.preferred_dialect;
	note_port->supported_dialects[i] = note_port_info.supported_dialects;
    }

    return 0;
}

//clear all the parameters on the id plugin, the plugin should not be processing
static int clap_plug_params_destroy(CLAP_PLUG_INFO* plug_data, int id){
    if(!plug_data)return -1;
    if(id >= MAX_INSTANCES || id < 0)return -1;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
    if(plug->plug_params){
	param_clean_param_container(plug->plug_params);
	plug->plug_params = NULL;
    }

    return 0;
}
//function that uses the value_to_text function on the param extension. use on [main-thread]
//this function is on param_container and will be used when the function param_get_value_as_string is called in params.c
static unsigned int clap_plug_params_value_to_text(const void* user_data, int param_id, PARAM_T value, char* ret_string, uint32_t string_len){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)user_data;
    if(!plug)return 0;
    if(!plug->plug_inst)return 0;
    const clap_plugin_params_t* clap_params = plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
    if(!clap_params)return 0;    
    clap_param_info_t param_info;
    if(!clap_params->get_info(plug->plug_inst, param_id, &param_info))return 0;
    //TODO have to create a long string first, because Juice wrapper and some CLAP plugins do not respect the string_len given to the value_to_text function
    char long_string[MAX_STRING_MSG_LENGTH];
    unsigned int convert_err = clap_params->value_to_text(plug->plug_inst, param_info.id, (double)value, long_string, MAX_STRING_MSG_LENGTH);
    if(convert_err == 1){
	snprintf(ret_string, string_len, "%s", long_string);
    }
    return convert_err;
}
//create parameters on the id plugin, the plugin should not be processing
static int clap_plug_params_create(CLAP_PLUG_INFO* plug_data, int id){
    if(!plug_data)return -1;
    if(id >= MAX_INSTANCES || id < 0)return -1;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
    if(plug->plug_inst_created != 1)return -1;
    if(!plug->plug_inst)return -1;
    
    const clap_plugin_params_t* clap_params = plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
    if(!clap_params)return -1;

    uint32_t param_count = clap_params->count(plug->plug_inst);
    if(param_count == 0)return 0;

    char** param_names = calloc(param_count, sizeof(char*));
    PARAM_T* param_vals = calloc(param_count, sizeof(PARAM_T));
    PARAM_T* param_mins = calloc(param_count, sizeof(PARAM_T));
    PARAM_T* param_maxs = calloc(param_count, sizeof(PARAM_T));
    PARAM_T* param_incs = calloc(param_count, sizeof(PARAM_T));
    unsigned char* val_types = calloc(param_count, sizeof(char));
    PRM_USER_DATA* user_data_array = calloc(param_count, sizeof(PRM_USER_DATA));
    if(!param_names || !param_vals || !param_mins || !param_maxs || !param_incs || !val_types){
	if(param_names)free(param_names);
	if(param_vals)free(param_vals);
	if(param_mins)free(param_mins);
	if(param_maxs)free(param_maxs);
	if(param_incs)free(param_incs);
	if(val_types)free(val_types);
	if(user_data_array)free(user_data_array);
	return -1;
    }
    for(uint32_t param_id = 0; param_id < param_count; param_id++){
	//init to temp values
	param_names[param_id] = NULL;
	param_vals[param_id] = 0.0;
	param_mins[param_id] = 0.0;
	param_maxs[param_id] = 0.0;
	val_types[param_id] = Float_type;
	PRM_USER_DATA param_data;
	param_data.data = NULL;
	param_data.user_id = 0;
	user_data_array[param_id] = param_data;
	
	clap_param_info_t param_info;
	if(!clap_params->get_info(plug->plug_inst, param_id, &param_info))continue;
	param_vals[param_id] = param_info.default_value;

	param_mins[param_id] = param_info.min_value;
	param_maxs[param_id] = param_info.max_value;
	//calculate the increment
	PARAM_T param_range = abs(param_maxs[param_id] - param_mins[param_id]);
	param_incs[param_id] = param_range / 100.0;
	
	user_data_array[param_id].data = param_info.cookie;
	user_data_array[param_id].user_id = param_info.id;

	if((param_info.flags & CLAP_PARAM_IS_STEPPED) == CLAP_PARAM_IS_STEPPED){
	    val_types[param_id] = Int_type;
	    param_incs[param_id] = 1.0;
	}
	//TODO not sure what to do with periodic parameters
	if((param_info.flags & CLAP_PARAM_IS_PERIODIC) == CLAP_PARAM_IS_PERIODIC){
	}
	//TODO not sure what to do with bypass parameter
	if((param_info.flags & CLAP_PARAM_IS_BYPASS) == CLAP_PARAM_IS_BYPASS){
	}
	//TODO need to make parameter hidden status switchable and to not show these parameters to the user 
	if((param_info.flags & CLAP_PARAM_IS_HIDDEN) == CLAP_PARAM_IS_HIDDEN){
	}
	if((param_info.flags & CLAP_PARAM_IS_READONLY) == CLAP_PARAM_IS_READONLY){
	    param_incs[param_id] = 0;
	}
	param_names[param_id] = calloc(MAX_PARAM_NAME_LENGTH, sizeof(char));
	snprintf(param_names[param_id], MAX_PARAM_NAME_LENGTH, "%s", param_info.name);
    }
    PRM_CONT_USER_DATA container_user_data;
    container_user_data.user_data = (void*)plug;
    container_user_data.val_to_string = clap_plug_params_value_to_text;
    plug->plug_params = params_init_param_container(param_count, param_names, param_vals, param_mins, param_maxs, param_incs, val_types,
						    user_data_array, &container_user_data);

    for(uint32_t i = 0; i < param_count; i++){
	if(param_names[i])free(param_names[i]);
    }
    free(param_names);
    free(param_vals);
    free(param_mins);
    free(param_maxs);
    free(param_incs);
    free(val_types);
    free(user_data_array);
    return 0;
}
PRM_CONTAIN* clap_plug_param_return_param_container(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return NULL;
    if(plug_id >= MAX_INSTANCES || plug_id < 0)return NULL;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
    if(!plug->plug_params)return NULL;

    return plug->plug_params;
}
static void clap_plug_ext_params_rescan(const clap_host_t* host, clap_param_rescan_flags flags){
    if(is_audio_thread)return;
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    if(!plug->plug_params)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    if((flags & CLAP_PARAM_RESCAN_VALUES) == CLAP_PARAM_RESCAN_VALUES){
	//go through the params and simply set the values
	const clap_plugin_params_t* clap_params = plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
	if(!clap_params)return;
	uint32_t param_count = (uint32_t)param_return_num_params(plug->plug_params, 0);
	for(uint32_t param_num = 0; param_num < param_count; param_num++){
	    clap_param_info_t param_info;
	    if(!clap_params->get_info(plug->plug_inst, param_num, &param_info))continue;
	    double cur_value = 0;
	    if(!clap_params->get_value(plug->plug_inst, param_info.id, &cur_value))continue;
	    param_set_value(plug->plug_params, param_num, (PARAM_T)cur_value, NULL, Operation_SetValue, 0);
	}
    }
    if((flags & CLAP_PARAM_RESCAN_TEXT) == CLAP_PARAM_RESCAN_TEXT){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "Plugin %s requested CLAP_PARAM_RESCAN_TEXT\n", plug->plug_path);
	//TODO not sure what clap api expects here, if the text needs to be rendered again it will do so automaticaly on the next ui cycle
    }
    if((flags & CLAP_PARAM_RESCAN_INFO) == CLAP_PARAM_RESCAN_INFO){
	//go through the params and change the ui_names
	const clap_plugin_params_t* clap_params = plug->plug_inst->get_extension(plug->plug_inst, CLAP_EXT_PARAMS);
	if(!clap_params)return;
	uint32_t param_count = (uint32_t)param_return_num_params(plug->plug_params, 0);
	for(uint32_t param_num = 0; param_num < param_count; param_num++){
	    clap_param_info_t param_info;
	    if(!clap_params->get_info(plug->plug_inst, param_num, &param_info))continue;
	    param_set_value(plug->plug_params, param_num, 0.0, param_info.name, Operation_ChangeName, 0);
	}
	//TODO get if any parameter is hidden or not, and set with set_value Operation_ToggleHidden
    }
    if((flags & CLAP_PARAM_RESCAN_ALL) == CLAP_PARAM_RESCAN_ALL){
	if(plug->plug_inst_activated == 1)return;
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "Plugin %s requested CLAP_PARAM_RESCAN_ALL\n", plug->plug_path);
	//TODO app_intrf.c does not handle critical changes of parameters right now - need to overhaul the whole app_intrf for this
	//TODO right now on app_intrf.c the parameters are created only when the plugin is added.
	/*
        clap_plug_params_destroy(plug_data, plug->id);
	clap_plug_params_create(plug_data, plug->id);
	*/
    }
}
//clear param of automation and modulation, since it is not implemented yet, does nothing
static void clap_plug_ext_params_clear(const clap_host_t* host, clap_id param_id, clap_param_clear_flags flags){
    return;
}
static void clap_plug_ext_params_request_flush(const clap_host_t* host){
    if(is_audio_thread)return;
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    //since host can call either clap_plugin.process() or clap_plugin_params.flush(), simply request to process the plugin (run clap_plugin.process)
    context_sub_wait_for_start(plug_data->control_data, (void*)plug);
}

//host extension function for audio_ports - return true if a rescan with the flag is supported by this host
static bool clap_plug_ext_audio_ports_is_rescan_flag_supported(const clap_host_t* host, uint32_t flag){
    //this function is only usable on the [main-thread]
    if(is_audio_thread)return false;
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return false;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return false;
    if((flag & CLAP_AUDIO_PORTS_RESCAN_NAMES) != 0)return true;
    if((flag & CLAP_AUDIO_PORTS_RESCAN_FLAGS) != 0)return true;
    if((flag & CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT) != 0)return true;
    if((flag & CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE) != 0)return true;
    if((flag & CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR) != 0)return true;
    if((flag & CLAP_AUDIO_PORTS_RESCAN_LIST) != 0)return true;
    return false;
}

//host extension function for audio_ports - rescan ports and get what is changed (in essence create the ports again)
static void clap_plug_ext_audio_ports_rescan(const clap_host_t* host, uint32_t flags){
    //this function is only usable on the [main-thread]
    if(is_audio_thread)return;
    if(flags == 0)return;
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    if(!plug->plug_inst)return;
    //If the names of the ports changed, but nothing else did, rename the ports, this can be done with an activated plugin
    if((flags ^ CLAP_AUDIO_PORTS_RESCAN_NAMES) == 0){
	clap_plug_ports_rename(plug_data, plug, &(plug->input_ports), 1);
	clap_plug_ports_rename(plug_data, plug, &(plug->output_ports), 0);
	return;
    }
    //other flags only usable on an !active plugin instance
    if(plug->plug_inst_activated)return;
    //for other flags destroy and create the ports again
    clap_plug_destroy_ports(plug_data, &(plug->output_ports));
    clap_plug_destroy_ports(plug_data, &(plug->input_ports));
    
    clap_plug_create_ports(plug_data, plug->id, &(plug->output_ports), 0);
    clap_plug_create_ports(plug_data, plug->id, &(plug->input_ports), 1);
}
//for note-ports extension return what note dialects are supported by this host
static uint32_t clap_plug_ext_note_ports_supported_dialects(const clap_host_t* host){
    if(is_audio_thread)return 0;
    uint32_t supported = 0;
    supported = (supported | CLAP_NOTE_DIALECT_CLAP);
    supported = (supported | CLAP_NOTE_DIALECT_MIDI);
    supported = (supported | CLAP_NOTE_DIALECT_MIDI2);
    //right now MPE is not supported
    //supported = supported | CLAP_NOTE_DIALECT_MIDI_MPE;
    return supported;
}
//for note-ports extension rescan the note ports and get what is changed (create the ports again)
static void clap_plug_ext_note_ports_rescan(const clap_host_t* host, uint32_t flags){
    if(is_audio_thread)return;
    if(flags == 0)return;
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    if(!plug->plug_inst)return;
    //if only the names of the ports changed (and nothing else) rename the ports - can be done with active plugin
    if((flags ^ CLAP_NOTE_PORTS_RESCAN_NAMES) == 0){
        clap_plug_note_ports_rename(plug_data, plug, 0);
	clap_plug_note_ports_rename(plug_data, plug, 1);
	return;
    }
    //other flags need deactivated plugin instance
    if(plug->plug_inst_activated)return;
    //destroy and create the note ports again
    if((flags & CLAP_NOTE_PORTS_RESCAN_ALL) == CLAP_NOTE_PORTS_RESCAN_ALL){
	clap_plug_note_ports_destroy(plug_data, &(plug->input_note_ports));
	clap_plug_note_ports_destroy(plug_data, &(plug->output_note_ports));
	clap_plug_note_ports_create(plug_data, plug->id, 0);
	clap_plug_note_ports_create(plug_data, plug->id, 1);
    }
}

static uint32_t clap_plug_ext_events_size(const struct clap_input_events* list){
    if(!list)return 0;
    UB_EVENT* ub_ev = (UB_EVENT*)list->ctx;
    if(!ub_ev)return 0;
    return ub_size(ub_ev);
}
static const clap_event_header_t* clap_plug_ext_events_get(const struct clap_input_events* list, uint32_t index){
    if(!list)return NULL;
    UB_EVENT* ub_ev = (UB_EVENT*)list->ctx;
    if(!ub_ev)return NULL;
    clap_event_header_t* header = (clap_event_header_t*)ub_item_get(ub_ev, index);
    return header;
}
static bool clap_plug_ext_events_try_push(const struct clap_output_events* list, const clap_event_header_t* event){
    if(!list)return false;
    UB_EVENT* ub_ev = (UB_EVENT*)list->ctx;
    if(!ub_ev)return false;

    int push_err = ub_push(ub_ev, (void*)event, event->size);
    if(push_err != 0)return false;
    return true;
}

static void clap_ext_preset_load_on_error(const clap_host_t *host,  uint32_t location_kind, const char* location, const char* load_key, int32_t os_error, const char* msg){
    return;
}

static void clap_ext_preset_load_on_load(const clap_host_t *host, uint32_t location_kind, const char* location, const char* load_key){
    return;
}
//clean the single plugin struct
//before calling this the plug_inst_processing should be == 0
static int clap_plug_plug_clean(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
      
    if(plug->plug_inst){
	if(plug->plug_inst_activated == 1){
	    plug->plug_inst->deactivate(plug->plug_inst);
	    plug->plug_inst_activated = 0;
	}
	if(plug->plug_inst_created == 1){
	    plug->plug_inst->destroy(plug->plug_inst);
	    plug->plug_inst_created = 0;
	}
	plug->plug_inst = NULL;	
    }
    if(plug->plug_entry){
	if(clap_plug_return_plug_id_with_same_plug_entry(plug_data, plug->plug_entry) == -1){
	    plug->plug_entry->deinit();
	}
	plug->plug_entry = NULL;
    }
    if(plug->plug_path){
	free(plug->plug_path);
	plug->plug_path = NULL;
    }
    //clean the audio ports
    clap_plug_destroy_ports(plug_data, &(plug->output_ports));
    clap_plug_destroy_ports(plug_data, &(plug->input_ports));
    //clean the note ports
    clap_plug_note_ports_destroy(plug_data, &(plug->input_note_ports));
    clap_plug_note_ports_destroy(plug_data, &(plug->output_note_ports));
    //clean the event structs
    clap_input_events_t* in_events = &(plug->input_events);
    ub_clean((UB_EVENT*)in_events->ctx);
    in_events->ctx = NULL;
    clap_output_events_t* out_events = &(plug->output_events);
    ub_clean((UB_EVENT*)out_events->ctx);
    out_events->ctx = NULL;
    //clean the parameters
    clap_plug_params_destroy(plug_data, plug->id);
    //remove presets
    clap_ext_preset_clean(plug->preset_fac);
    plug->preset_fac = NULL;

    
    plug->plug_inst_id = -1;

    return 0;
}

int clap_plug_plug_stop_and_clean(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
    //stop this plugin process if its processing before cleaning.
    context_sub_wait_for_stop(plug_data->control_data, (void*)plug);
    return clap_plug_plug_clean(plug_data, plug_id);
}

//return if this is audio_thread or not
static bool clap_plug_return_is_audio_thread(){
    return is_audio_thread;
}
//extension functions for thread-check.h to return is this audio or main thread for the clap_host_thread_t extension
static bool clap_plug_ext_is_audio_thread(const clap_host_t* host){
    return clap_plug_return_is_audio_thread();
}
static bool clap_plug_ext_is_main_thread(const clap_host_t* host){
    return !(clap_plug_return_is_audio_thread());
}
static int clap_sys_msg(void* user_data, const char* msg){
    CLAP_PLUG_INFO* plug_data = (CLAP_PLUG_INFO*)user_data;
    //if(!plug_data)return -1;
    log_append_logfile("%s", msg);
    return 0;
}

//extension function for log.h to send messages in [thread-safe] manner
static void clap_plug_ext_log(const clap_host_t* host, clap_log_severity severity, const char* msg){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    //severity is not sent, right now dont see the need to send severity - this should be obvious from the message
    context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), msg);
}

const void* get_extension(const clap_host_t* host, const char* ex_id){    
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return NULL;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return NULL;
    if(strcmp(ex_id, CLAP_EXT_THREAD_CHECK) == 0){
	return &(plug_data->ext_thread_check);
    }
    if(strcmp(ex_id, CLAP_EXT_LOG) == 0){
	return &(plug_data->ext_log);
    }
    if(strcmp(ex_id, CLAP_EXT_AUDIO_PORTS) == 0){
	return &(plug_data->ext_audio_ports);
    }
    if(strcmp(ex_id, CLAP_EXT_NOTE_PORTS) == 0){
	return &(plug_data->ext_note_ports);
    }
    if(strcmp(ex_id, CLAP_EXT_PARAMS) == 0){
	return &(plug_data->ext_params);
    }
    if(strcmp(ex_id, CLAP_EXT_PRESET_LOAD) == 0){
	return &(plug_data->ext_preset_load);
    }
    //if there is no extension implemented that the plugin needs send the name of the extension to the ui
    context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "%s asked for ext %s\n", plug->plug_path, ex_id);

    return NULL;
}
static int clap_plug_activate_start_processing(void* user_data){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)user_data;
    if(!plug)return -1;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return -1;
    //since there was a request to start processing the plugin, it should be stopped, but just in case, send a request to stop it
    context_sub_wait_for_stop(plug_data->control_data, (void*)plug);
    
    if(plug->plug_inst_activated == 0){
	if(!plug->plug_inst)return -1;
	if(!plug->plug_inst->activate(plug->plug_inst, plug_data->sample_rate, plug_data->min_buffer_size, plug_data->max_buffer_size)){
	    return -1;
	}
    }
    plug->plug_inst_activated = 1;
    //send message to the audio thread that the plugin can be started to process
    //and wait for it to start
    context_sub_wait_for_start(plug_data->control_data, (void*)plug);

    return 0;
}
static int clap_plug_restart(void* user_data){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)user_data;
    if(!plug)return -1;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return -1;
    //send a message to rt thread to stop the plugin process and block while its stopping
    context_sub_wait_for_stop(plug_data->control_data, (void*)plug);
    
    if(plug->plug_inst_activated == 1){
	plug->plug_inst->deactivate(plug->plug_inst);
	plug->plug_inst_activated = 0;
    }
    //now when the plugin was deactivated simply call the activate and process function, that will reactivate the plugin and send a message to [audio-thread] to start processing it
    return clap_plug_activate_start_processing((void*)plug);
}
void request_restart(const clap_host_t* host){   
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    context_sub_restart_msg(plug_data->control_data, (void*)plug, clap_plug_return_is_audio_thread());
}

void request_process(const clap_host_t* host){   
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    context_sub_activate_start_process_msg(plug_data->control_data, (void*)plug, clap_plug_return_is_audio_thread());
}

static int clap_plug_callback(void* user_data){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)user_data;
    if(!plug)return -1;
    if(!plug->plug_inst)return -1;
    plug->plug_inst->on_main_thread(plug->plug_inst);
    return 0;
}
void request_callback(const clap_host_t* host){   
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    context_sub_callback_msg(plug_data->control_data, (void*)plug, clap_plug_return_is_audio_thread());
}

static int clap_plug_start_process(void* user_data){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)user_data;
    if(!plug)return -1;

    //if plugin is already processing do nothing
    if(plug->plug_inst_processing == 1){
	return 0;
    }

    if(!plug->plug_inst){
	return -1;
    }
    if(!(plug->plug_inst->start_processing(plug->plug_inst))){
	return -1;
    }

    plug->plug_inst_processing = 1;
    return 0;
}
static int clap_plug_stop_process(void* user_data){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)user_data;
    if(!plug)return -1;
    
    //if plugin is sleeping stop it completely, since when it is sleeping [audio-thread] can still access some parts of the CLAP_PLUG_PLUG struct
    if(plug->plug_inst_processing == 2){
	plug->plug_inst_processing = 0;
	return 0;
    }
    //if plugin is already stopped and not processing do nothing
    if(plug->plug_inst_processing != 1){
	return 0;
    }
    if(!plug->plug_inst){
	return -1;
    }
    plug->plug_inst->stop_processing(plug->plug_inst);

    plug->plug_inst_processing = 0;
    return 0;
}
int clap_read_ui_to_rt_messages(CLAP_PLUG_INFO* plug_data){
    //this is a local thread var its false on [main-thread] and true on [audio-thread]
    is_audio_thread = true;
    
    if(!plug_data)return -1;
    //process the sys messages (stop, start plugin and similar)
    context_sub_process_rt(plug_data->control_data);
    
    //read the param messages for plugins on the [audio-thread]
    for(unsigned int i = 0; i < MAX_INSTANCES; i++){
	CLAP_PLUG_PLUG* cur_plug = &(plug_data->plugins[i]);
	if(cur_plug->plug_inst_processing == 0)continue;
	if(!cur_plug->plug_inst)continue;
	param_msgs_process(cur_plug->plug_params, 1);
    }
    return 0;
}
int clap_read_rt_to_ui_messages(CLAP_PLUG_INFO* plug_data){
    if(!plug_data)return -1;
    //process the sys messages on [main-thread] (send msg, activate and process a plugin, restart plugin etc.)
    context_sub_process_ui(plug_data->control_data);
    //read the param messages for plugins on the [main-thread]
    for(unsigned int i = 0; i < MAX_INSTANCES; i++){
	CLAP_PLUG_PLUG* cur_plug = &(plug_data->plugins[i]);
	if(!cur_plug->plug_inst)continue;
	param_msgs_process(cur_plug->plug_params, 0);
    }    
    return 0;
}

static char** clap_plug_get_plugin_names_from_file(CLAP_PLUG_INFO* plug_data, const char* plug_path, unsigned int* num_of_plugins){
    void* handle;
    int* iptr;
    handle = dlopen(plug_path, RTLD_LOCAL | RTLD_LAZY);
    if(!handle){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "failed to load %s dso \n", plug_path);
	return NULL;
    }
    
    iptr = (int*)dlsym(handle, "clap_entry");
    clap_plugin_entry_t* plug_entry = (clap_plugin_entry_t*)iptr;
    if(!plug_entry)return NULL;
    unsigned int init_err = plug_entry->init(plug_path);
    if(!init_err){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "failed to init %s plugin entry\n", plug_path);
	return NULL;
    }
    
    const clap_plugin_factory_t* plug_fac = plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    uint32_t plug_count = plug_fac->get_plugin_count(plug_fac);
    if(plug_count <= 0){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "no plugins in %s dso \n", plug_path);
	return NULL;
    }
    char** plug_names = malloc(sizeof(char*));
    if(!plug_names)return NULL;
    unsigned int name_count = 0;
    for(uint32_t pl_iter = 0; pl_iter < plug_count; pl_iter++){
	const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, pl_iter);
	if(!plug_desc)continue;
	if(!(plug_desc->name))continue;
	char** temp_names = realloc(plug_names, sizeof(char*) * (name_count+1));
	if(!temp_names)continue;
	plug_names = temp_names;
	plug_names[name_count] = NULL;
	unsigned int cur_name_size = strlen(plug_desc->name) + 1;
	char* cur_name = malloc(sizeof(char) * cur_name_size);
	if(!cur_name)continue;
	snprintf(cur_name, cur_name_size, "%s", plug_desc->name);
	plug_names[name_count] = cur_name;
	name_count += 1;
    }

    if(name_count == 0){
	free(plug_names);
	return NULL;
    }
    plug_entry->deinit();
    
    *num_of_plugins = name_count;
    return plug_names;
}

//get the clap files in the directory
static char** clap_plug_get_clap_files(const char* file_path, unsigned int* size){
    char** ret_files = NULL;
    *size = 0;
    DIR* d;
    struct dirent* dir = NULL;
    d = opendir(file_path);
    if(d){
	ret_files = malloc(sizeof(char*));
	unsigned int iter = 0;
	while((dir = readdir(d)) != NULL){
	    if(strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)continue;
	    char* after_delim = NULL;
	    char* before_delim = str_split_string_delim(dir->d_name, ".", &after_delim);
	    if(!after_delim){
		if(before_delim)free(before_delim);
		continue;
	    }
	    if(strcmp(after_delim, "clap") != 0){
		if(after_delim)free(after_delim);
		if(before_delim)free(before_delim);
		continue;
	    }
	    if(after_delim)free(after_delim);
	    if(before_delim)free(before_delim);
	    char** temp_files = realloc(ret_files, sizeof(char*) * (iter+1));
	    if(!temp_files)continue;
	    ret_files = temp_files;
	    ret_files[iter] = NULL;
	    unsigned int cur_file_len = strlen(dir->d_name) + 1;
	    char* cur_file = malloc(sizeof(char) * cur_file_len);
	    if(!cur_file){
		continue;
	    }
	    snprintf(cur_file, cur_file_len, "%s", dir->d_name);
	    ret_files[iter] = cur_file;
	    iter += 1;
	}
	closedir(d);
	*size = iter;
    }
    return ret_files;
}

//goes through the clap files stored on the clap_data struct and returns the names of these plugins
//each clap file can have several different plugins inside of it
char** clap_plug_return_plugin_names(CLAP_PLUG_INFO* plug_data, unsigned int* size){
    unsigned int total_files = 0;
    char** clap_files = clap_plug_get_clap_files(CLAP_PATH, &total_files);
    if(!clap_files)return NULL;
    char** return_names = malloc(sizeof(char*));
    unsigned int total_names_found = 0;
    for(int i = 0; i < total_files; i++){
	char* cur_file = clap_files[i];
	if(!cur_file)continue;
	unsigned int total_file_name_size = strlen(CLAP_PATH) + strlen(cur_file) + 1;
	char* total_file_path = malloc(sizeof(char) * total_file_name_size);
	if(!total_file_path){
	    free(cur_file);
	    continue;
	}
	snprintf(total_file_path, total_file_name_size, "%s%s", CLAP_PATH, cur_file);

	unsigned int plug_name_count = 0;
	char** plug_names = clap_plug_get_plugin_names_from_file(plug_data, total_file_path, &plug_name_count);
	if(plug_names){
	    for(int j = 0; j < plug_name_count; j++){
		char* cur_plug_name = plug_names[j];
		if(!cur_plug_name)continue;
		char** temp_return_names = realloc(return_names, sizeof(char*) * (total_names_found +1));
		if(!temp_return_names){
		    free(cur_plug_name);
		    continue;
		}
		return_names = temp_return_names;
		return_names[total_names_found] = cur_plug_name;
		total_names_found += 1;
	    }
	    free(plug_names);
	}
	free(total_file_path);
	free(cur_file);
    }
    free(clap_files);
    if(total_names_found == 0){
	free(return_names);
	return NULL;
    }
    *size = total_names_found;
    return return_names;
}
void clap_plug_presets_clean_preset(CLAP_PLUG_INFO* plug_data, void* preset_info){
    if(!plug_data)return;
    if(!preset_info)return;
    CLAP_PLUG_PRESET_INFO* cur_preset = (CLAP_PLUG_PRESET_INFO*)preset_info;
    if(!cur_preset)return;

    free(cur_preset);
}
int clap_plug_presets_name_return(CLAP_PLUG_INFO* plug_data, void* preset_info, char* name, uint32_t name_len){
    if(!plug_data)return -1;
    if(!preset_info)return -1;
    CLAP_PLUG_PRESET_INFO* cur_preset = (CLAP_PLUG_PRESET_INFO*)preset_info;
    if(!cur_preset)return -1;
    if(!cur_preset->short_name)return -1;
    snprintf(name, name_len, "%s", cur_preset->short_name);
    return 0;
}
int clap_plug_presets_path_return(CLAP_PLUG_INFO* plug_data, void* preset_info, char* path, uint32_t path_len){
    if(!plug_data)return -1;
    if(!preset_info)return -1;
    CLAP_PLUG_PRESET_INFO* cur_preset = (CLAP_PLUG_PRESET_INFO*)preset_info;
    if(!cur_preset)return -1;
    if(!cur_preset->full_path)return -1;
    snprintf(path, path_len, "%s", cur_preset->full_path);
    return 0;
}
int clap_plug_presets_categories_iterate(CLAP_PLUG_INFO* plug_data, void* preset_info, char* category, uint32_t category_len, uint32_t idx){
    if(!plug_data)return -1;
    if(!preset_info)return -1;
    CLAP_PLUG_PRESET_INFO* cur_preset = (CLAP_PLUG_PRESET_INFO*)preset_info;
    if(!cur_preset)return -1;
    if(!cur_preset->categories)return -1;
    uint32_t str_len = strlen(cur_preset->categories)+1;
    char* ret_string = (char*)malloc(sizeof(char) * str_len);
    if(!ret_string)return -1;
    char* cur_category = NULL;
    snprintf(ret_string, str_len, "%s", cur_preset->categories);
    cur_category = strtok(ret_string, "/");
    if(!cur_category && idx == 0){
	snprintf(category, category_len, "%s", cur_preset->categories);
	free(ret_string);
	return 0;
    }
    if(!cur_category && idx > 0){
	free(ret_string);
	return -1;
    }

    uint32_t cur_iter = 0;
    while(cur_category){
	if(cur_iter == idx){
	    snprintf(category, category_len, "%s", cur_category);
	    free(ret_string);
	    return 0;
	}
	cur_category = strtok(NULL, "/");
	cur_iter += 1;
    }
    free(ret_string);
    return -1;
}
void* clap_plug_presets_iterate(CLAP_PLUG_INFO* plug_data, unsigned int plug_idx, uint32_t iter){
    if(!plug_data)return NULL;
    if(plug_idx >= MAX_INSTANCES)return NULL;
    CLAP_PLUG_PLUG* cur_plug = &(plug_data->plugins[plug_idx]);
    if(!cur_plug->plug_inst)return NULL;
    //if this is the first iteration, create the preset factory struct, if the preset-factory extension is available for the plugin
    if(iter == 0){
	//create the preset-factory struct first, if the preset-factory does not exist on the plugin this will be NULL
	//first clean the current clap_ext preset struct
	clap_ext_preset_clean(cur_plug->preset_fac);
	CLAP_EXT_PRESET_USER_FUNCS preset_user_funcs;
	preset_user_funcs.ext_preset_send_msg = clap_sys_msg;
	preset_user_funcs.user_data = (void*)plug_data;
	cur_plug->preset_fac = clap_ext_preset_init(cur_plug->plug_entry, cur_plug->clap_host_info, preset_user_funcs);
    }
    //if the preset-factory extension exists iterate through the presets
    if(cur_plug->preset_fac){
	CLAP_PLUG_PRESET_INFO* preset_info = (CLAP_PLUG_PRESET_INFO*)malloc(sizeof(CLAP_PLUG_PRESET_INFO));
	if(!preset_info)return NULL;
	int err = clap_ext_preset_info_return(cur_plug->preset_fac, cur_plug->plugin_id, iter, NULL, NULL, NULL, 0,
					      preset_info->short_name, MAX_PARAM_NAME_LENGTH,
					      preset_info->full_path, MAX_PATH_STRING,
					      preset_info->categories, MAX_PATH_STRING);
	if(err == 1)return (void*)preset_info;
	clap_plug_presets_clean_preset(plug_data, (void*)preset_info);
    }
    //TODO return presets from the state extension
    return NULL;
}

int clap_plug_preset_load_from_path(CLAP_PLUG_INFO* plug_data, int plug_id, const char* preset_path){
    if(!plug_data)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;
    CLAP_PLUG_PLUG* cur_plug = &(plug_data->plugins[plug_id]);
    if(!cur_plug->plug_inst)return -1;

    if(!cur_plug->preset_fac){
	//if the preset-factory struct does not exist try to create it, since preset_path can be fed here from a save file for example
	CLAP_EXT_PRESET_USER_FUNCS preset_user_funcs;
	preset_user_funcs.ext_preset_send_msg = clap_sys_msg;
	preset_user_funcs.user_data = (void*)plug_data;
	cur_plug->preset_fac = clap_ext_preset_init(cur_plug->plug_entry, cur_plug->clap_host_info, preset_user_funcs);
    }
    //first check if the preset_path is in the preset-factory
    if(cur_plug->preset_fac){
	uint32_t loc_kind = 0;
	char load_key[MAX_PATH_STRING];
	char location[MAX_PATH_STRING];
	int err = clap_ext_preset_info_return(cur_plug->preset_fac, cur_plug->plugin_id, 0, preset_path, &loc_kind, load_key, MAX_PATH_STRING, NULL, 0, location, MAX_PATH_STRING, NULL, 0);
	if(err == 1){
	    const clap_plugin_preset_load_t* preset_ext = cur_plug->plug_inst->get_extension(cur_plug->plug_inst, CLAP_EXT_PRESET_LOAD);
	    if(preset_ext){
		bool loaded = preset_ext->from_location(cur_plug->plug_inst, loc_kind, location, load_key);
		if(loaded){
		    return 1;
		}
	    }
	}
    }

    //TODO if the preset_path is not in the preset-factory check if its a preset in the save extension
    
    return 0;
}

CLAP_PLUG_INFO* clap_plug_init(uint32_t min_buffer_size, uint32_t max_buffer_size, SAMPLE_T samplerate,
			       clap_plug_status_t* plug_error, void* audio_backend){
    if(!audio_backend)return NULL;
    CLAP_PLUG_INFO* plug_data = (CLAP_PLUG_INFO*)malloc(sizeof(CLAP_PLUG_INFO));
    if(!plug_data){
	*plug_error = clap_plug_failed_malloc;
	return NULL;
    }
    memset(plug_data, '\0', sizeof(*plug_data));
    plug_data->audio_backend = audio_backend;
    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = clap_plug_start_process;
    rt_funcs_struct.subcx_stop_process = clap_plug_stop_process;
    ui_funcs_struct.send_msg = clap_sys_msg;
    ui_funcs_struct.subcx_activate_start_process = clap_plug_activate_start_processing;
    ui_funcs_struct.subcx_callback = clap_plug_callback;
    ui_funcs_struct.subcx_restart = clap_plug_restart;
    plug_data->control_data = context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if(!plug_data->control_data){
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
    clap_info_host.get_extension = get_extension;
    clap_info_host.request_restart = request_restart;
    clap_info_host.request_process = request_process;
    clap_info_host.request_callback = request_callback;

    plug_data->clap_host_info = clap_info_host;

    //initiate the host clap extension structs
    //thread check extension
    plug_data->ext_thread_check.is_audio_thread = clap_plug_ext_is_audio_thread;
    plug_data->ext_thread_check.is_main_thread = clap_plug_ext_is_main_thread;
    //log extension
    plug_data->ext_log.log = clap_plug_ext_log;
    //audio-ports extension
    plug_data->ext_audio_ports.is_rescan_flag_supported = clap_plug_ext_audio_ports_is_rescan_flag_supported;
    plug_data->ext_audio_ports.rescan = clap_plug_ext_audio_ports_rescan;
    //note-ports extension
    plug_data->ext_note_ports.supported_dialects = clap_plug_ext_note_ports_supported_dialects;
    plug_data->ext_note_ports.rescan = clap_plug_ext_note_ports_rescan;
    //param extension
    plug_data->ext_params.clear = clap_plug_ext_params_clear;
    plug_data->ext_params.request_flush = clap_plug_ext_params_request_flush;
    plug_data->ext_params.rescan = clap_plug_ext_params_rescan;
    //preset-load extension
    plug_data->ext_preset_load.loaded = clap_ext_preset_load_on_load;
    plug_data->ext_preset_load.on_error = clap_ext_preset_load_on_error;
    
    //the array holds one more plugin, it will be an empty shell to check if the total number of plugins arent too many
    for(int i = 0; i < (MAX_INSTANCES+1); i++){
        CLAP_PLUG_PLUG* plug = &(plug_data->plugins[i]);
	plug->clap_host_info = clap_info_host;
	plug->id = i;
	plug->plug_data = plug_data;
	plug->plug_entry = NULL;
	plug->plug_inst = NULL;
	plug->plug_inst_activated = 0;
	plug->plug_inst_created = 0;
	plug->plug_inst_id = -1;
        plug->plug_inst_processing = 0;
	plug->plug_params = NULL;
	plug->plug_path = NULL;
	plug->preset_fac = NULL;
    }    
    return plug_data;
}

//return the clap_plug_plug with plugin entry (initiated), plug_path and plug_inst_id from the plugins name, checks if  the same entry is already loaded or not first
static int clap_plug_create_plug_from_name(CLAP_PLUG_INFO* plug_data, const char* plug_name, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;
    CLAP_PLUG_PLUG* return_plug = &(plug_data->plugins[plug_id]);
    unsigned int clap_files_num = 0;
    char** clap_files = clap_plug_get_clap_files(CLAP_PATH, &clap_files_num);
    if(!clap_files)return -1;
    char* plug_name_path = NULL;
    //first find the file_path of the plug_name plugin
    for(unsigned int i = 0; i < clap_files_num; i++){
	char* cur_clap_file = clap_files[i];
	unsigned int total_file_name_size = strlen(CLAP_PATH) + strlen(cur_clap_file) + 1;
	char* total_file_path = malloc(sizeof(char) * total_file_name_size);
	if(!total_file_path)continue;
	snprintf(total_file_path, total_file_name_size, "%s%s", CLAP_PATH, cur_clap_file);
	unsigned int plug_count = 0;
	char** cur_plug_names = clap_plug_get_plugin_names_from_file(plug_data, total_file_path, &plug_count);
	if(!cur_plug_names){
	    free(total_file_path);
	    continue;
	}
	unsigned int found = 0;
	for(unsigned int name = 0; name < plug_count; name++){
	    if(!cur_plug_names[name])continue;
	    if(strcmp(cur_plug_names[name], plug_name) != 0)continue;
	    plug_name_path = cur_clap_file;
	    found = 1;
	    break;
	}
	for(unsigned int j = 0; j < plug_count; j++){
	    if(cur_plug_names[j])free(cur_plug_names[j]);
	}
	free(cur_plug_names);
	free(total_file_path);
	if(found == 1)break;
    }
    return_plug->plug_path = plug_name_path;
    //free the clap_files entries
    for(unsigned int i = 0; i < clap_files_num; i++){
	if(!clap_files[i])continue;
	if(return_plug->plug_path == clap_files[i])continue;
	free(clap_files[i]);
    }
    free(clap_files);
    
    if(!return_plug->plug_path){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "Could not find a plugin with %s name \n", plug_name);
	return -1;
    }
    //find a plugin if one exists with the same plugin path and get its entry if so
    for(unsigned int plug_num = 0; plug_num < MAX_INSTANCES; plug_num++){
	CLAP_PLUG_PLUG cur_plug = plug_data->plugins[plug_num];
	if(!cur_plug.plug_path)continue;
	if(strcmp(return_plug->plug_path, cur_plug.plug_path) != 0)continue;
	if(!(cur_plug.plug_entry))continue;
	return_plug->plug_entry = cur_plug.plug_entry;
	break;
    }

    //if return_plug was not created it means a plugin with the same plugin path is not loaded, we need to load it
    if(!(return_plug->plug_entry)){
	void* handle;
	int* iptr;
	unsigned int total_file_name_size = strlen(CLAP_PATH) + strlen(return_plug->plug_path) + 1;
	char* total_file_path = malloc(sizeof(char) * total_file_name_size);
	if(!total_file_path){
	    clap_plug_plug_stop_and_clean(plug_data, return_plug->id);
	    return -1;
	}
	snprintf(total_file_path, total_file_name_size, "%s%s", CLAP_PATH, return_plug->plug_path);
	    
	handle = dlopen(total_file_path, RTLD_LOCAL | RTLD_LAZY);
	if(!handle){
	    free(total_file_path);
	    clap_plug_plug_stop_and_clean(plug_data, return_plug->id);
	    return -1;
	}
    
	iptr = (int*)dlsym(handle, "clap_entry");
	clap_plugin_entry_t* plug_entry = (clap_plugin_entry_t*)iptr;
    
	bool init_err = plug_entry->init(total_file_path);
	free(total_file_path);
	if(!init_err){
	    clap_plug_plug_stop_and_clean(plug_data, return_plug->id);
	    return -1;
	}
	return_plug->plug_entry = plug_entry;
    }
    if(!(return_plug->plug_entry)){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "Could not create entry for plugin %s\n", plug_name);
	clap_plug_plug_stop_and_clean(plug_data, return_plug->id);
	return -1;
    }
    //now we have the plug_entry and path, find the instance id
    const clap_plugin_factory_t* plug_fac = return_plug->plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    uint32_t plug_count = plug_fac->get_plugin_count(plug_fac);
	
    for(uint32_t pl_iter = 0; pl_iter < plug_count; pl_iter++){
	const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, pl_iter);
	if(!plug_desc)continue;
	if(!(plug_desc->name))continue;
	if(strcmp(plug_desc->name, plug_name) != 0)continue;
	return_plug->plug_inst_id = pl_iter;
	break;
    }

    if(return_plug->plug_inst_id == -1){
	clap_plug_plug_stop_and_clean(plug_data, return_plug->id);
	return -1;
    }

    return 0;
}

int clap_plug_load_and_activate(CLAP_PLUG_INFO* plug_data, const char* plugin_name, int id){
    int return_id = -1;
    if(!plug_data)return -1;
    if(!plugin_name)return -1;
    if(id >= MAX_INSTANCES)return -1;
    //if id is negative find an empty slot in the plugins array and create the plugin there
    if(id < 0){
	for(int i = 0; i < (MAX_INSTANCES+1); i++){
	    CLAP_PLUG_PLUG cur_plug = plug_data->plugins[i];
	    //if there is a path for this plugin the slot is not empty
	    if(cur_plug.plug_inst)continue;
	    id = cur_plug.id;
	    break;
	}
    }
    //if the id is still not within range an error happened, cant create the plugin
    if(id < 0 || id >= MAX_INSTANCES)return -1;
    //if id is in the possible range, clean the slot just in case its occupied 
    clap_plug_plug_stop_and_clean(plug_data, id);
    
    if(clap_plug_create_plug_from_name(plug_data, plugin_name, id) < 0){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "could not laod plugin from name %s\n", plugin_name);
	return -1;
    }
    //this plug will only have the entry, plug_path and which plug_inst_id this plugin_name is in the plugin factory array at this point
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
    const clap_plugin_factory_t* plug_fac = plug->plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, plug->plug_inst_id);
    if(!plug_desc){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "Could not get plugin %s descriptor\n", plug->plug_path);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    if(plug_desc->name){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "Got clap_plugin %s descriptor\n", plug_desc->name);
    }
    if(plug_desc->description){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "%s info: %s\n", plug_desc->name, plug_desc->description);
    }
    snprintf(plug->plugin_id, MAX_UNIQUE_ID_STRING, "%s", plug_desc->id);
    //add the clap_host_t struct to the plug
    clap_host_t clap_info_host;
    clap_info_host.clap_version = plug_data->clap_host_info.clap_version;
    clap_info_host.host_data = (void*)plug;
    clap_info_host.name = plug_data->clap_host_info.name;
    clap_info_host.vendor = plug_data->clap_host_info.vendor;
    clap_info_host.url = plug_data->clap_host_info.url;
    clap_info_host.version = plug_data->clap_host_info.version;
    clap_info_host.get_extension = plug_data->clap_host_info.get_extension;
    clap_info_host.request_restart = plug_data->clap_host_info.request_restart;
    clap_info_host.request_process = plug_data->clap_host_info.request_process;
    clap_info_host.request_callback = plug_data->clap_host_info.request_callback;
    plug->clap_host_info = clap_info_host;
    //create plugin instance
    const clap_plugin_t* plug_inst = plug_fac->create_plugin(plug_fac, &(plug->clap_host_info) , plug_desc->id);
    if(!plug_inst){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "Failed to create %s plugin\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    plug->plug_inst = plug_inst;
    plug->plug_inst_created = 1;    
    bool inst_err = plug->plug_inst->init(plug->plug_inst);
    if(!inst_err){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "Failed to init %s plugin\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }

    //Create the ports
    int port_out_err = clap_plug_create_ports(plug_data, plug->id, &(plug->output_ports), 0);
    int port_in_err = clap_plug_create_ports(plug_data, plug->id, &(plug->input_ports), 1);
    if(port_out_err == -1 || port_in_err == -1){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "Failed to create %s plugin audio ports\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    
    //Initiate the event lists
    //TODO TEST if EVENT_LIST_SIZE and EVENT_LIST_ITEMS are good enough sizes
    clap_input_events_t* in_events = &(plug->input_events);
    in_events->ctx = (void*)ub_init(EVENT_LIST_SIZE, EVENT_LIST_ITEMS);
    in_events->get = clap_plug_ext_events_get;
    in_events->size = clap_plug_ext_events_size;   
    clap_output_events_t* out_events = &(plug->output_events);
    out_events->ctx = (void*)ub_init(EVENT_LIST_SIZE, EVENT_LIST_ITEMS);
    out_events->try_push = clap_plug_ext_events_try_push;
    if(!in_events->ctx || !out_events->ctx){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "Failed to create %s plugin clap event structs\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    
    //Initiate the note ports on the audio client backend
    int note_port_out_err = clap_plug_note_ports_create(plug_data, plug->id, 0);
    int note_port_in_err = clap_plug_note_ports_create(plug_data, plug->id, 1);
    if(note_port_in_err == -1 || note_port_out_err == -1){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "Failed to create %s plugin note ports\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    //create the parameter cotainer
    if(clap_plug_params_create(plug_data, plug->id) != 0){
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, clap_plug_return_is_audio_thread(), "Failed to create %s plugin parameters container\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    
    //start processing the plugin
    context_sub_activate_start_process_msg(plug_data->control_data, (void*)plug, is_audio_thread);

    return_id = plug->id;
    return return_id;
}

char* clap_plug_return_plugin_name(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return NULL;
    if(plug_id < 0 || plug_id >= MAX_INSTANCES)return NULL;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
    if(!plug->plug_inst)return NULL;
    const char* name_string = plug->plug_inst->desc->name;
    char* ret_string = malloc(sizeof(char) * (strlen(name_string)+1));
    if(!ret_string)return NULL;
    strcpy(ret_string, name_string);
    return ret_string;
}
//return -1 on error, return 0 if successful but the output was quiet and return 1 if successful and the output not quiet
static int clap_prepare_input_ports(CLAP_PLUG_INFO* plug_data, CLAP_PLUG_PORT* input_ports, unsigned int nframes){
    if(!plug_data)return -1;
    if(!input_ports)return -1;
    if(!input_ports->sys_port_array)return -1;
    if(!input_ports->audio_ports)return -1;
    if(input_ports->ports_count <= 0)return 0;
    int not_quiet = 0;
    for(uint32_t port = 0; port < input_ports->ports_count; port++){
	CLAP_PLUG_PORT_SYS cur_port_sys = input_ports->sys_port_array[port];
	clap_audio_buffer_t cur_clap_port = input_ports->audio_ports[port];
	//TODO nothing is done with the latency property
        uint32_t channels = cur_port_sys.channel_count;
	for(uint32_t chan = 0; chan < channels; chan++){
	    SAMPLE_T* sys_buffer = NULL;
	    if(cur_port_sys.sys_ports)
		sys_buffer = app_jack_get_buffer_rt(cur_port_sys.sys_ports[chan], nframes);
	    for(unsigned int frame = 0; frame < nframes; frame++){
		SAMPLE_T cur_frame = 0.0;
		if(sys_buffer)
		    cur_frame = sys_buffer[frame];
		if(not_quiet == 0 && cur_frame != 0)not_quiet = 1;
#if SAMPLE_T_AS_DOUBLE == 1
		if(cur_clap_port.data64)
		    cur_clap_port.data64[chan][frame] = cur_frame;
#else
		if(cur_clap_port.data32)
		    cur_clap_port.data32[chan][frame] = cur_frame;
#endif
	    }
	}
    }
    
    return not_quiet;
}
//process the output audio ports, by copying them to the system output ports
//return -1 on error, return 0 if successful but the output was quiet and return 1 if successful and the output not quiet
static int clap_prepare_output_ports(CLAP_PLUG_INFO* plug_data, CLAP_PLUG_PORT* output_ports, unsigned int nframes, bool fill_zeroes){
    if(!plug_data)return -1;
    if(!output_ports)return -1;
    if(!output_ports->sys_port_array)return -1;
    if(!output_ports->audio_ports)return -1;
    if(output_ports->ports_count <= 0)return -1;
    int not_quiet = 0;
    for(uint32_t port = 0; port < output_ports->ports_count; port++){
	CLAP_PLUG_PORT_SYS cur_port_sys = output_ports->sys_port_array[port];
	clap_audio_buffer_t clap_port = output_ports->audio_ports[port];
	uint32_t channels = cur_port_sys.channel_count;
	//TODO nothing is done with the latency property
	for(uint32_t chan = 0; chan < channels; chan++){
	    SAMPLE_T* clap_buffer = NULL;
#if SAMPLE_T_AS_DOUBLE == 1
	    if(clap_port.data64)
		clap_buffer = clap_port.data64[chan];
#else
	    if(clap_port.data32)
		clap_buffer = clap_port.data32[chan];
#endif
	    SAMPLE_T* sys_buffer = NULL;
	    if(cur_port_sys.sys_ports)
		sys_buffer = app_jack_get_buffer_rt(cur_port_sys.sys_ports[chan], nframes);
	    if(!sys_buffer)continue;
	    memset(sys_buffer, '\0', sizeof(SAMPLE_T)*nframes);
	    if(!clap_buffer || fill_zeroes)continue;
	    //if the buffer is constant leave the sys port buffer filled with zeroes
	    if((clap_port.constant_mask & (1 << chan)) != 0)continue;
	    for(unsigned int frame = 0; frame < nframes; frame++){
		sys_buffer[frame] = clap_buffer[frame];
		if(clap_buffer[frame] != 0 && not_quiet == 0)not_quiet = 1;
	    }
	}
    }
    return not_quiet;
}
//TODO get events from parameters, midi etc. and use this data
//return -1 on error, return 0 if successful but the output was quiet and return 1 if successful and the output not quiet
static int clap_output_events_read(CLAP_PLUG_INFO* plug_data, unsigned int nframes, CLAP_PLUG_PLUG* plug){
    if(!plug_data)return -1;
    if(!plug)return -1;
    clap_output_events_t* out_events = &(plug->output_events);
    if(!out_events->ctx)return -1;
    UB_EVENT* ub_out = (UB_EVENT*)out_events->ctx;
    int not_quiet = 0;
    uint32_t events_count = ub_size(ub_out);
    //TODO to implement output event processing need to find a plugin that outputs them
    if(events_count > 0){
	not_quiet = 1;
	context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "events %d\n", events_count);
    }
    return not_quiet;
}
//return -1 on error, return 0 if successful but the output was quiet and return 1 if successful and the output not quiet
static int clap_input_events_prepare(CLAP_PLUG_INFO* plug_data, unsigned int nframes, CLAP_PLUG_PLUG* plug){
    if(!plug_data)return -1;
    if(!plug)return -1;
    clap_input_events_t* in_events = &(plug->input_events);
    if(!in_events->ctx)return -1;
    UB_EVENT* ub_in = (UB_EVENT*)in_events->ctx;
    //reset the event array
    ub_list_reset(ub_in);
    int not_quiet = 0;
    //TODO right now smp_groovebox does not handle automation or modulation so simply put the parameter changes into the event buffer first on the 0 offset frame
    //put parameter changes into the event queue
    uint32_t param_count = param_return_num_params(plug->plug_params, 1);
    for(uint32_t param_idx = 0; param_idx < param_count; param_idx++){
	if(param_get_if_changed(plug->plug_params, (int)param_idx, 1) != 1)continue;
	if(not_quiet == 0)not_quiet = 1;
	clap_event_header_t head;
	head.flags = CLAP_EVENT_IS_LIVE;
	head.size = sizeof(clap_event_param_value_t);
	head.space_id = CLAP_CORE_EVENT_SPACE_ID;
	head.time = 0;
	head.type = CLAP_EVENT_PARAM_VALUE;

	clap_event_param_value_t param_val;
	param_val.channel = -1;
	PRM_USER_DATA param_user_data;
	param_user_data.data = NULL;
	param_user_data.user_id = -1;
	param_user_data_return(plug->plug_params, (int)param_idx, &param_user_data, 1);
	param_val.cookie = param_user_data.data;
	param_val.header = head;
	param_val.key = -1;
	param_val.note_id = -1;
	param_val.param_id = param_user_data.user_id;
	param_val.port_index = -1;
	param_val.value = (double)param_get_value(plug->plug_params, (int)param_idx, 0, 0, 1);
	ub_push(ub_in, (void*)&(param_val), (uint32_t)sizeof(clap_event_param_value_t));
    }
    //TODO how to add transport events so everything is sorted by the frames offset?
    
    //write to input note ports of the plugin
    CLAP_PLUG_NOTE_PORT* note_ports = &(plug->input_note_ports);
    if(note_ports->ports_count == 0)return not_quiet;
    //Add the system midi messages
    for(uint32_t port_num = 0; port_num < note_ports->ports_count; port_num++){
	if(!(note_ports->sys_ports[port_num]))continue;
	app_jack_midi_cont_reset(note_ports->midi_cont);
	void* midi_buffer = app_jack_get_buffer_rt(note_ports->sys_ports[port_num], nframes);
	if(!midi_buffer)continue;
	app_jack_return_notes_vels_rt(midi_buffer, note_ports->midi_cont);
	uint32_t note_dialect = note_ports->preferred_dialects[port_num];
	//write the midi1 messages, depending on the preferred dialect of the note port
	for(uint32_t midi_num = 0; midi_num < note_ports->midi_cont->num_events; midi_num++){
	    if(not_quiet == 0)not_quiet = 1;
	    clap_event_header_t clap_head;
	    clap_head.time = note_ports->midi_cont->nframe_nums[midi_num];
	    clap_head.flags = CLAP_EVENT_IS_LIVE;
	    clap_head.space_id = CLAP_CORE_EVENT_SPACE_ID;
	    
	    int32_t note_id = -1; 
	    int16_t port_index = (int16_t)port_num;
	    int16_t channel = 0;
	    int16_t key = 0;
	    double  velocity = 0;
	    
	    MIDI_DATA_T type = note_ports->midi_cont->types[midi_num];
	    //Handle note off and on events
	    if((type & 0xf0) == 0x80 || (type & 0xf0) == 0x90){
		if((note_dialect & CLAP_NOTE_DIALECT_CLAP) == CLAP_NOTE_DIALECT_CLAP){
		    clap_head.type = CLAP_EVENT_NOTE_OFF;
		    if((type & 0xf0) == 0x90)
			clap_head.type = CLAP_EVENT_NOTE_ON;
		    clap_head.size = sizeof(clap_event_note_t);
		    channel = (int16_t)(type & 0x0f);
		    key = (int16_t)note_ports->midi_cont->note_pitches[midi_num];      
		    velocity = (double)fit_range(127, 0, 1, 0, (SAMPLE_T)note_ports->midi_cont->vel_trig[midi_num]);
		    
		    clap_event_note_t clap_note;
		    clap_note.channel = channel;
		    clap_note.header = clap_head;
		    clap_note.key = key;
		    clap_note.note_id = note_id;
		    clap_note.port_index = port_index;
		    clap_note.velocity = velocity;
		    ub_push(ub_in, (void*)&(clap_note), (uint32_t)sizeof(clap_note));
		    continue;
		}
		if((note_dialect & CLAP_NOTE_DIALECT_MIDI) == CLAP_NOTE_DIALECT_MIDI){
		    clap_head.type = CLAP_EVENT_MIDI;
		    clap_head.size = sizeof(clap_event_midi_t);
		    clap_event_midi_t clap_midi;
		    clap_midi.data[0] = (uint8_t)type;
		    clap_midi.data[1] = (uint8_t)note_ports->midi_cont->note_pitches[midi_num];
		    clap_midi.data[2] = (uint8_t)note_ports->midi_cont->vel_trig[midi_num];
		    clap_midi.header = clap_head;
		    clap_midi.port_index = port_index;
		    ub_push(ub_in, (void*)&(clap_midi), (uint32_t)sizeof(clap_midi));
		    continue;
		}
	    }
	    //Handle Polyphonic aftertouch events
	    if((type & 0xf0) == 0xA0){
	    }
	    //Control mode change events
	    if((type & 0xf0) == 0xB0){
	    }
	    //Handle program change events
	    if((type & 0xf0) == 0xC0){
	    }
	    //Handle Channel aftertouch events
	    if((type & 0xf0) == 0xD0){
	    }
	    //Handle pitch bend change events
	    if((type & 0xf0) == 0xE0){
	    }
	    //Handle system events (Timing clock, start, continue etc.)
	    if((type & 0xf0) == 0xF0){
	    }    
	}
    }
    return not_quiet;
}

void clap_process_data_rt(CLAP_PLUG_INFO* plug_data, unsigned int nframes){
    if(!plug_data)return;
    if(!plug_data->plugins)return;
    for(int id = 0; id < MAX_INSTANCES; id++){
	CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
	//do nothing if the plugin is completely stopped
	if(plug->plug_inst_processing == 0)continue;
	
	clap_process_t _process = {0};
	_process.steady_time = -1;
	_process.frames_count = nframes;
	_process.transport = NULL;

	//copy sys input audio buffers to clap input audio buffers
	int in_audio_not_quiet = clap_prepare_input_ports(plug_data, &(plug->input_ports), nframes);
	_process.audio_inputs = NULL;
	_process.audio_inputs_count = 0;
	if(in_audio_not_quiet != -1){
	    _process.audio_inputs_count = plug->input_ports.ports_count;	
	    _process.audio_inputs = plug->input_ports.audio_ports;
	}
	
	_process.audio_outputs = plug->output_ports.audio_ports;
	_process.audio_outputs_count = plug->output_ports.ports_count;

	//write messages from midi, parameters and similar to the input events
	int in_event_not_quiet = clap_input_events_prepare(plug_data, nframes, plug);
	_process.in_events = &(plug->input_events);
	
	//reset the output event array
	clap_output_events_t* out_events = &(plug->output_events);
	ub_list_reset((UB_EVENT*)out_events->ctx);
	_process.out_events = out_events;
	
	//the plugin is sleeping, check if it needs to wake up
	if(plug->plug_inst_processing == 2){
	    if(in_audio_not_quiet == 0 && in_event_not_quiet == 0) continue;
	    //if audio or event input was not quiet from the system ports start the plugin and continue the process
	    if(!(plug->plug_inst->start_processing(plug->plug_inst))) continue;
	    plug->plug_inst_processing = 1;
	}

	clap_process_status clap_status = plug->plug_inst->process(plug->plug_inst, &_process);

	if(clap_status == CLAP_PROCESS_ERROR){
	    //process failed so fill the system output with zeroes and do nothing with output events
	    clap_prepare_output_ports(plug_data, &(plug->output_ports), nframes, true);
	    context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "ERROR Processing discard buffer\n");
	}
	if(clap_status == CLAP_PROCESS_CONTINUE){
	    clap_prepare_output_ports(plug_data, &(plug->output_ports), nframes, false);
	    clap_output_events_read(plug_data, nframes, plug);
	}
	if(clap_status == CLAP_PROCESS_CONTINUE_IF_NOT_QUIET){
	    int not_quiet_audio = clap_prepare_output_ports(plug_data, &(plug->output_ports), nframes, false);
	    int not_quiet_events = clap_output_events_read(plug_data, nframes, plug);
	    context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "Process if NOT_QUIET\n");
	    //process successful but outputs are quiet send this plugin to sleep
	    if(not_quiet_audio == 0 && not_quiet_events == 0){
		plug->plug_inst->stop_processing(plug->plug_inst);
		plug->plug_inst_processing = 2;
	    }
	}
	if(clap_status == CLAP_PROCESS_TAIL){
	    context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "Process if TAIL\n");
	    clap_prepare_output_ports(plug_data, &(plug->output_ports), nframes, false);
	    clap_output_events_read(plug_data, nframes, plug);
	    //TODO implement tail extension
	}
	if(clap_status == CLAP_PROCESS_SLEEP){
	    context_sub_send_msg(plug_data->control_data, (void*)plug_data, is_audio_thread, "SLEEP for now\n");
	    clap_prepare_output_ports(plug_data, &(plug->output_ports), nframes, false);
	    clap_output_events_read(plug_data, nframes, plug);
	    //no need to process further so plugin goes to sleep
	    plug->plug_inst->stop_processing(plug->plug_inst);
	    plug->plug_inst_processing = 2;
	}
    }
}

void clap_plug_clean_memory(CLAP_PLUG_INFO* plug_data){
    if(!plug_data)return;
    for(int i = 0; i < MAX_INSTANCES; i++){
	clap_plug_plug_clean(plug_data, i);
    }

    context_sub_clean(plug_data->control_data);
    
    free(plug_data);
}
