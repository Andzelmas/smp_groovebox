#pragma once
#include "../structs.h"

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
char** clap_plug_return_plugin_names(unsigned int* size);
//initiate the main plugin data struct. 
CLAP_PLUG_INFO* clap_plug_init(uint32_t min_buffer_size, uint32_t max_buffer_size, SAMPLE_T samplerate, clap_plug_status_t* plug_error, void* audio_backend);
//initiate and load plugin from its name
int clap_plug_load_and_activate(CLAP_PLUG_INFO* plug_data, const char* plugin_name, int id);
//clean the plugin struct and free memory
void clap_plug_clean_memory(CLAP_PLUG_INFO* plug_data);
