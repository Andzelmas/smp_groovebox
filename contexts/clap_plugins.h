#pragma once
#include "../structs.h"
#include "params.h"

enum ClapPlugStatus{
    clap_plug_failed_malloc = -1,
};
typedef enum ClapPlugStatus clap_plug_status_t;

typedef struct _clap_plug_info CLAP_PLUG_INFO; //the struct that holds all the plugin info
//read the ui_to_rt messages on the [audio-thread] and call functions to stop or start processing the plugin or the whole clap context.
int clap_read_ui_to_rt_messages(CLAP_PLUG_INFO* plug_data);
//read the rt_to_ui messages on the [main-thread] and call functions to restart, activate, write to log and similar main thread functions.
//some of these called functions will block main-thread, to wait for the plugin or the whole process to stop.
int clap_read_rt_to_ui_messages(CLAP_PLUG_INFO* plug_data);
//return the names of the plugins in the plugin directory
char** clap_plug_return_plugin_names(CLAP_PLUG_INFO* plug_data, unsigned int* size);
//return the names of the plugin presets (either from the internal preset-factory or the save state extension)
char** clap_plug_presets_return_names(CLAP_PLUG_INFO* plug_data, unsigned int plug_idx, unsigned int* total_presets); 
//initiate the main plugin data struct. 
CLAP_PLUG_INFO* clap_plug_init(uint32_t min_buffer_size, uint32_t max_buffer_size, SAMPLE_T samplerate, clap_plug_status_t* plug_error, void* audio_backend);
//return the plugin parameter container
PRM_CONTAIN* clap_plug_param_return_param_container(CLAP_PLUG_INFO* plug_data, int plug_id);
//initiate and load plugin from its name
int clap_plug_load_and_activate(CLAP_PLUG_INFO* plug_data, const char* plugin_name, int id);
//return the name of the plugin, caller must free the char*
char* clap_plug_return_plugin_name(CLAP_PLUG_INFO* plug_data, int plug_id);
//process the clap plugins, must be called on the [audio-thread]
void clap_process_data_rt(CLAP_PLUG_INFO* plug_data, unsigned int nframes);
//remove the clap plugin
int clap_plug_plug_stop_and_clean(CLAP_PLUG_INFO* plug_data, int plug_id);
//clean the plugin struct and free memory
void clap_plug_clean_memory(CLAP_PLUG_INFO* plug_data);
