#pragma once
#include <stdlib.h>
#include <stdbool.h>
#include "../structs.h"
#include "params.h"

enum PlugStatus{
    plug_failed_malloc,
    plug_failed_world_init
};
//enum that holds the error statutes of the plugin
typedef enum PlugStatus plug_status_t;
//plugin port
typedef struct _plug_port PLUG_PORT;
//the single plugin that has plugin instance and plugin data
typedef struct _plug_plug PLUG_PLUG;
//the plugin object, that will holds the lilv world etc.
typedef struct _plug_info PLUG_INFO;
//the event buffer struct for atom sequences
typedef struct _plug_evbuf_impl PLUG_EVBUF;
//the event iterator for PLUG_EVBUF
typedef struct _plug_evbuf_iterator_impl PLUG_EVBUF_ITERATOR;
//inititialize the plugin host data
PLUG_INFO* plug_init(uint32_t block_length, SAMPLE_T samplerate, plug_status_t* plug_errors, void* audio_backend);
//return the name of the plug_id plugin
char* plug_return_plugin_name(PLUG_INFO* plug_data, int plug_id);
//get the list of the plugins in a string array format, user needs to free that array
char** plug_return_plugin_names(PLUG_INFO* plug_data, unsigned int* size);
//get the list of presets belonging to the plugin
char** plug_return_plugin_presets_names(PLUG_INFO* plug_data, unsigned int indx, unsigned int* total_presets);
//read the main-thread audio-thread comm messages and launch apropriate functions (stop, start processes etc.)
int plug_read_rt_to_ui_messages(PLUG_INFO* plug_data);
int plug_read_ui_to_rt_messages(PLUG_INFO* plug_data);
//initialize a plugin instance
//if id is not -1 a plugin will be created in that plugin index, if that index is already occupied, the previous
//plugin will be cleared
int plug_load_and_activate(PLUG_INFO* plug_data, const char* plugin_uri, const int id);
//load a plugin preset
int plug_load_preset(PLUG_INFO* plug_data, unsigned int plug_id, const char* preset_name);
//a callback to send to lilv_state_restore to set the control ports directly without circle buffer
static void plug_set_value_direct(const char* port_symbol,
				  void* data,
				  const void* value,
				  uint32_t size,
				  uint32_t type);
//find port by its name
static PLUG_PORT* plug_find_port_by_name(PLUG_PLUG* plug, const char* name);
//create controls that are properties not ports
static void plug_create_properties(PLUG_INFO* plug_data, PLUG_PLUG* plug, bool writable);
//set the samplerate of the plug_data, should usually be done before launching any plugins
void plug_set_samplerate(PLUG_INFO* plug_data, float new_sample_rate);
//set the buffer size, should be usually done before launching any plugins
void plug_set_block_length(PLUG_INFO* plug_data, uint32_t block_length);
//activate the ports, that the backend needs to activate, uses the callback function sent here
void plug_activate_backend_ports(PLUG_INFO* plug_data, PLUG_PLUG* plug);
//return the system ports of the plugin - plug_id is the number of the plugin in the plugins array.
void** plug_return_sys_ports(PLUG_INFO* plug_data, unsigned int plug_id, unsigned int* number_ports);
//set param value on the param container, also send a message to the [audio-thread] to set the value on the same rt param
int plug_param_set_value(PLUG_INFO* plug_data, int plug_id, int param_id, float param_val, unsigned char param_op);
//get in what increments the parameter changes, should be used only on [main-thread]
SAMPLE_T plug_param_get_increment(PLUG_INFO* plug_data, int plug_id, int param_id);
//get the value and type of the parameter on the [main-thread]
SAMPLE_T plug_param_get_value(PLUG_INFO* plug_data, unsigned char* val_type, unsigned int curved, int plug_id, int param_id);
//find parameter id number from the name on [main-thread]
int plug_param_id_from_name(PLUG_INFO* plug_data, int plug_id, const char* param_name);
//get the paramater value string on [main-thread]
const char* plug_param_get_string(PLUG_INFO* plug_data, int plug_id, int param_id);
//get how many parameters there are on the plugin, use on [main-thread]
int plug_param_get_num_of_params(PLUG_INFO* plug_data, int plug_id);
//connect the ports, run the plugins instances for nframes, and update the output ports, use on [audio-thread]
//get the name of the parameter, use on [main-thread]
const char* plug_param_get_name(PLUG_INFO* plug_data, int plug_id, int param_id);
void plug_process_data_rt(PLUG_INFO* plug_data, unsigned int nframes);
//run the plugin for nframes
static void plug_run_rt(PLUG_PLUG* plug, unsigned int nframes);
//remove a plugin
int  plug_remove_plug(PLUG_INFO* plug_data, const int id);
//clean the plug_data memory
void plug_clean_memory(PLUG_INFO* plug_data);
