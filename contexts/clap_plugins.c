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
#include "../types.h"
#include "context_control.h"
#include "params.h"
#include "../jack_funcs/jack_funcs.h"
//what is the size of the buffer to get the formated param values to
#define MAX_VALUE_LEN 64
#define CLAP_PATH "/usr/lib/clap/"
//how many clap plugins can there be in the plugin array
#define MAX_INSTANCES 5

static thread_local bool is_audio_thread = false;
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

//the single clap plugin struct
typedef struct _clap_plug_plug{
    int id; //plugin id on the clap_plug_info plugin array
    clap_plugin_entry_t* plug_entry; //the clap library file for this plugin
    const clap_plugin_t* plug_inst; //the plugin instance
    unsigned int plug_inst_created; //was a init function called in the descriptor for this plugin
    unsigned int plug_inst_activated; //was the activate function called on this plugin
    //0 - not processing, 1 - processing, 2 - not processing, but should start processing if there are events or input audio changed.
    unsigned int plug_inst_processing; //is the plugin instance processing, touch only on [audio_thread]
    int plug_inst_id; //the plugin instance index in the array of the plugin factory
    char* plug_path; //the path for the clap file
    PRM_CONTAIN* plug_params; //plugin parameter container for params.c
    clap_host_t clap_host_info; //need when creating the plugin instance, this struct has this CLAP_PLUG_PLUG in the host var as (void*)
    CLAP_PLUG_INFO* plug_data; //CLAP_PLUG_INFO struct address for convenience
    //CLAP_PLUG_PORT holds an array of sys backend audio ports (to connect to jack for example) and the clap_audio_buffer_t arrays, to send to plugin process function
    CLAP_PLUG_PORT input_ports;
    CLAP_PLUG_PORT output_ports;
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

static int clap_plug_port_name_create(int name_size, char* full_name, int plug_id, const char* plug_inst_name, const char* port_name, uint32_t chan_num){
    if(!plug_inst_name || !port_name)return -1;
    if(plug_id < 0)return -1;
    if(name_size <= 0)return -1;
    if(!full_name)return -1;
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
	    if((port_info.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) == 1){
		cur_clap_port->data64 = malloc(sizeof(double*) * channels);
		for(uint32_t chan = 0; chan < channels; chan++){
		    cur_clap_port->data64[chan] = calloc(plug_data->max_buffer_size, sizeof(double));
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
//host extension function for audio_ports - return true if a rescan with the flag is supported by this host
static bool clap_plug_ext_audio_ports_is_rescan_flag_supported(const clap_host_t* host, uint32_t flag){
    //this function is only usable on the [main-thread]
    if(is_audio_thread)return false;
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return false;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return false;
    if(flag & CLAP_AUDIO_PORTS_RESCAN_NAMES != 0)return true;
    if(flag & CLAP_AUDIO_PORTS_RESCAN_FLAGS != 0)return true;
    if(flag & CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT != 0)return true;
    if(flag & CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE != 0)return true;
    if(flag & CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR != 0)return true;
    if(flag & CLAP_AUDIO_PORTS_RESCAN_LIST != 0)return true;
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
    if(flags ^ CLAP_AUDIO_PORTS_RESCAN_NAMES == 0){
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
    
    clap_plug_destroy_ports(plug_data, &(plug->output_ports));
    clap_plug_destroy_ports(plug_data, &(plug->input_ports));
    
    if(plug->plug_params){
	param_clean_param_container(plug->plug_params);
	plug->plug_params = NULL;
    }
    plug->plug_inst_id = -1;

    return 0;
}

int clap_plug_plug_stop_and_clean(CLAP_PLUG_INFO* plug_data, int plug_id){
    //stop this plugin process if its processing before cleaning.
    context_sub_wait_for_stop(plug_data->control_data, plug_id);
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
    context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), msg);
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
    //if there is no extension implemented that the plugin needs send the name of the extension to the ui
    context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "%s asked for ext %s\n", plug->plug_path, ex_id);

    return NULL;
}
static int clap_plug_activate_start_processing(void* user_data, int plug_id){
    CLAP_PLUG_INFO* plug_data = (CLAP_PLUG_INFO*)user_data;
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
    if(!plug)return -1;
    //since there was a request to start processing the plugin, it should be stopped, but just in case, send a request to stop it
    context_sub_wait_for_stop(plug_data->control_data, plug_id);
    
    if(plug->plug_inst_activated == 0){
	if(!plug->plug_inst)return -1;
	if(!plug->plug_inst->activate(plug->plug_inst, plug_data->sample_rate, plug_data->min_buffer_size, plug_data->max_buffer_size)){
	    return -1;
	}
    }
    plug->plug_inst_activated = 1;
    //send message to the audio thread that the plugin can be started to process
    //and wait for it to start
    context_sub_wait_for_start(plug_data->control_data, plug_id);

    return 0;
}
static int clap_plug_restart(void* user_data, int plug_id){
    CLAP_PLUG_INFO* plug_data = (CLAP_PLUG_INFO*)user_data;
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
    //send a message to rt thread to stop the plugin process and block while its stopping
    context_sub_wait_for_stop(plug_data->control_data, plug_id);
    
    if(plug->plug_inst_activated == 1){
	plug->plug_inst->deactivate(plug->plug_inst);
	plug->plug_inst_activated = 0;
    }
    //now when the plugin was deactivated simply call the activate and process function, that will reactivate the plugin and send a message to [audio-thread] to start processing it
    return clap_plug_activate_start_processing((void*)plug_data, plug_id);
}
void request_restart(const clap_host_t* host){   
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    context_sub_restart_msg(plug_data->control_data, plug->id, clap_plug_return_is_audio_thread());
}

void request_process(const clap_host_t* host){   
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    context_sub_activate_start_process_msg(plug_data->control_data, plug->id, clap_plug_return_is_audio_thread());
}

static int clap_plug_callback(void* user_data, int plug_id){
    CLAP_PLUG_INFO* plug_data = (CLAP_PLUG_INFO*)user_data;
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
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
    
    context_sub_callback_msg(plug_data->control_data, plug->id, clap_plug_return_is_audio_thread());
}

static int clap_plug_start_process(void* user_data, int plug_id){
    CLAP_PLUG_INFO* plug_data = (CLAP_PLUG_INFO*)user_data;
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);

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
static int clap_plug_stop_process(void* user_data, int plug_id){
    CLAP_PLUG_INFO* plug_data = (CLAP_PLUG_INFO*)user_data;
    if(!plug_data)return -1;
    
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);

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
    
    return 0;
}
int clap_read_rt_to_ui_messages(CLAP_PLUG_INFO* plug_data){
    if(!plug_data)return -1;
    //process the sys messages on [main-thread] (send msg, activate and process a plugin, restart plugin etc.)
    context_sub_process_ui(plug_data->control_data);
    return 0;
}

static char** clap_plug_get_plugin_names_from_file(CLAP_PLUG_INFO* plug_data, const char* plug_path, unsigned int* num_of_plugins){
    void* handle;
    int* iptr;
    handle = dlopen(plug_path, RTLD_LOCAL | RTLD_LAZY);
    if(!handle){
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "failed to load %s dso \n", plug_path);
	return NULL;
    }
    
    iptr = (int*)dlsym(handle, "clap_entry");
    clap_plugin_entry_t* plug_entry = (clap_plugin_entry_t*)iptr;
    if(!plug_entry)return NULL;
    unsigned int init_err = plug_entry->init(plug_path);
    if(!init_err){
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "failed to init %s plugin entry\n", plug_path);
	return NULL;
    }
    
    const clap_plugin_factory_t* plug_fac = plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    uint32_t plug_count = plug_fac->get_plugin_count(plug_fac);
    if(plug_count <= 0){
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "no plugins in %s dso \n", plug_path);
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
    plug_data->control_data = context_sub_init((void*)plug_data, rt_funcs_struct, ui_funcs_struct);
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

    //the array holds one more plugin, it will be an empty shell to check if the total number of plugins arent too many
    for(int i = 0; i < (MAX_INSTANCES+1); i++){
        CLAP_PLUG_PLUG* plug = &(plug_data->plugins[i]);
	plug->output_ports.audio_ports = NULL;
	plug->output_ports.sys_port_array = NULL;
	plug->output_ports.ports_count = 0;
	plug->input_ports.audio_ports = NULL;
	plug->input_ports.sys_port_array = NULL;
	plug->input_ports.ports_count = 0;
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
    }    
    /*
    void* handle;
    int* iptr;
    handle = dlopen(plug_path, RTLD_LOCAL | RTLD_LAZY);
    if(!handle){
	log_append_logfile("failed to load %s dso \n", plug_path);
    }
    
    iptr = (int*)dlsym(handle, "clap_entry");
    clap_plugin_entry_t* plug_entry = (clap_plugin_entry_t*)iptr;
    
    unsigned int init_err = plug_entry->init(plug_path);
    if(!init_err){
	log_append_logfile("failed to init the %s plugin entry\n", plug_path);
	return;
    }
    
    const clap_plugin_factory_t* plug_fac = plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    uint32_t plug_count = plug_fac->get_plugin_count(plug_fac);
    if(plug_count <= 0){
	log_append_logfile("no plugins in %s dso \n", plug_path);
	return;
    }

    //TODO iterating through the clap files should be in the clap_plug_return_plugin_names function
    //TODO when user chooses which plugin to load, go through the clap files (and the plugins in the files) and compare the plugin name
    //to the name the user sent, if its the same load this plugin
    for(uint32_t pl_iter = 0; pl_iter < plug_count; pl_iter++){
	const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, pl_iter);
	if(!plug_desc)continue;
	if(plug_desc->name){
	    log_append_logfile("Got clap_plugin %s descriptor\n", plug_desc->name);
	}
	if(plug_desc->description){
	    log_append_logfile("%s info: %s\n", plug_desc->name, plug_desc->description);
	}

	const clap_plugin_t* plug_inst = plug_fac->create_plugin(plug_fac, &clap_info_host, plug_desc->id);
	if(!plug_inst){
	    log_append_logfile("Failed to create %s plugin\n", plug_path);
	    continue;
	}

	bool inst_err = plug_inst->init(plug_inst);
	if(!inst_err){
	    log_append_logfile("Failed to init %s plugin\n", plug_path);
	    continue;
	}

	plug_inst->activate(plug_inst, 48000, 32, 1024);

	const clap_plugin_params_t* plug_inst_params = plug_inst->get_extension(plug_inst, CLAP_EXT_PARAMS);
	if(plug_inst_params){
	    uint32_t params_count = plug_inst_params->count(plug_inst);
	    log_append_logfile("found %d params\n", params_count);
	    for(uint32_t pr_iter = 0; pr_iter < params_count; pr_iter++){
		clap_param_info_t param_info;
		bool got_info = plug_inst_params->get_info(plug_inst, pr_iter, &param_info);
		if(!got_info)continue;
		log_append_logfile("Param name %s, min_val %g, max_val %g, default_val %g\n",
				   param_info.name, param_info.module,
				   param_info.min_value, param_info.max_value, param_info.default_value);
		double cur_value = 0;
		bool got_val = plug_inst_params->get_value(plug_inst, param_info.id, &cur_value);
		if(!got_val)continue;
		char val_text[MAX_VALUE_LEN];
		if(plug_inst_params->value_to_text(plug_inst, param_info.id, cur_value,
						   val_text, MAX_VALUE_LEN)){
		    log_append_logfile("Current value double %f, text %s, param is enum %d, is_stepped %d, requires_process %d\n",
				       cur_value, val_text,
				       (param_info.flags & CLAP_PARAM_IS_ENUM),
				       (param_info.flags & CLAP_PARAM_IS_STEPPED),
				       (param_info.flags & CLAP_PARAM_REQUIRES_PROCESS));
		}
	    }
	}
	
	plug_inst->deactivate(plug_inst);
	plug_inst->destroy(plug_inst);
    }

    plug_entry->deinit();
    */
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
	context_sub_send_msg(plug_data->control_data, is_audio_thread, "Could not find a plugin with %s name \n", plug_name);
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
	context_sub_send_msg(plug_data->control_data, is_audio_thread, "Could not create entry for plugin %s\n", plug_name);
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
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "could not laod plugin from name %s\n", plugin_name);
	return -1;
    }
    //this plug will only have the entry, plug_path and which plug_inst_id this plugin_name is in the plugin factory array at this point
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
    const clap_plugin_factory_t* plug_fac = plug->plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, plug->plug_inst_id);
    if(!plug_desc){
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "Could not get plugin %s descriptor\n", plug->plug_path);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    if(plug_desc->name){
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "Got clap_plugin %s descriptor\n", plug_desc->name);
    }
    if(plug_desc->description){
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "%s info: %s\n", plug_desc->name, plug_desc->description);
    }
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
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "Failed to create %s plugin\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    plug->plug_inst = plug_inst;
    plug->plug_inst_created = 1;
    
    bool inst_err = plug_inst->init(plug_inst);
    if(!inst_err){
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "Failed to init %s plugin\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }

    //Create the ports
    int port_out_err = clap_plug_create_ports(plug_data, plug->id, &(plug->output_ports), 0);
    int port_in_err = clap_plug_create_ports(plug_data, plug->id, &(plug->input_ports), 1);
    if(port_out_err == -1 || port_in_err == -1){
	context_sub_send_msg(plug_data->control_data, clap_plug_return_is_audio_thread(), "Failed to create %s plugin audio ports\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    context_sub_activate_start_process_msg(plug_data->control_data, plug->id, is_audio_thread);
    //TODO need to activate the plugin, create parameters
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
//TODO some plugin try to get event size, while events are not implemented this will save the app from crashing
uint32_t test_event_function(const struct clap_input_events *list){
    return 0;
}
//TODO while events are not implemented push function placeholder so plugins do not crash that get this function
bool test_output_event_function(const struct clap_output_events *list, const clap_event_header_t *event){
    return false;
}
void clap_process_data_rt(CLAP_PLUG_INFO* plug_data, unsigned int nframes){
    if(!plug_data)return;
    if(!plug_data->plugins)return;
    for(int id = 0; id < MAX_INSTANCES; id++){
	CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
	if(plug->plug_inst_processing == 0)continue;
	if(!plug->plug_inst)continue;
	//TODO if stopped but only sleeping, check input events or audio inputs if needs to start processing again
	if(plug->plug_inst_processing == 2){

	    continue;
	}

	clap_process_t _process = {0};
	_process.steady_time = -1;
	_process.frames_count = nframes;
	_process.transport = NULL;
	//TODO getting and setting of buffers should be more elegant and protected for NULL arrays etc.
	//copy sys input audio buffers to clap input audio buffers
	if(plug->input_ports.sys_port_array){
	    for(uint32_t port = 0; port < plug->input_ports.ports_count; port++){
		CLAP_PLUG_PORT_SYS cur_port_sys = plug->input_ports.sys_port_array[port];
		if(!cur_port_sys.sys_ports)continue;
		uint32_t channels = cur_port_sys.channel_count;
		//get the sys port
		for(uint32_t chan = 0; chan < channels; chan++){
		    if(!cur_port_sys.sys_ports[chan])continue;
		    SAMPLE_T* sys_buffer = app_jack_get_buffer_rt(cur_port_sys.sys_ports[chan], nframes);
		    if(!sys_buffer)continue;
		    for(unsigned int frame = 0; frame < nframes; frame++){
#if SAMPLE_T_AS_DOUBLE == 1
			    plug->input_ports.audio_ports[port].data64[chan][frame] = sys_buffer[frame];
#else
			    plug->input_ports.audio_ports[port].data32[chan][frame] = sys_buffer[frame];
#endif
		    }
		}
	    }
	}
	_process.audio_inputs = plug->input_ports.audio_ports;
	_process.audio_outputs = plug->output_ports.audio_ports;
	_process.audio_inputs_count = plug->input_ports.ports_count;
	_process.audio_outputs_count = plug->output_ports.ports_count;
	//TODO while events are not implemented
	clap_input_events_t in_event;
	in_event.size = test_event_function;
	_process.in_events = &(in_event);
	//TODO while events are not implemented
	clap_output_events_t out_event;
	out_event.try_push = test_output_event_function;
	_process.out_events = &(out_event);

	clap_process_status clap_status = plug->plug_inst->process(plug->plug_inst, &_process);

	//copy clap output audio buffers to sys audio buffers
	for(uint32_t port = 0; port < plug->output_ports.ports_count; port++){
	    CLAP_PLUG_PORT_SYS cur_port_sys = plug->output_ports.sys_port_array[port];
	    clap_audio_buffer_t clap_port = plug->output_ports.audio_ports[port];
	    uint32_t channels = cur_port_sys.channel_count;
	    //get the sys port
	    for(uint32_t chan = 0; chan < channels; chan++){
#if SAMPLE_T_AS_DOUBLE == 1
		    SAMPLE_T* clap_buffer = clap_port.data64[chan];
#else
		    SAMPLE_T* clap_buffer = clap_port.data32[chan];
#endif
		SAMPLE_T* sys_buffer = app_jack_get_buffer_rt(cur_port_sys.sys_ports[chan], nframes);
		memset(sys_buffer, '\0', sizeof(SAMPLE_T)*nframes);
		for(unsigned int frame = 0; frame < nframes; frame++){
		    sys_buffer[frame] = clap_buffer[frame];
		}
	    }
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
