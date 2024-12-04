#pragma once
#include "../structs.h"
#include "../util_funcs/ring_buffer.h"

#define MAX_STRING_MSG_LENGTH 128

enum ClapPlugStatus{
    clap_plug_failed_malloc = -1,
};
typedef enum ClapPlugStatus clap_plug_status_t;

//this holds the plugin id and the enum (from type.h MSGfromRT) to tell what to do with the plugin
typedef struct _clap_ring_sys_msg{
    unsigned int msg_enum; //what to do with the plugin
    char msg[MAX_STRING_MSG_LENGTH];
    int plug_id; //plugin id on the plugins array that needs to be changed somehow
}CLAP_RING_SYS_MSG;

typedef struct _clap_plug_info CLAP_PLUG_INFO; //the struct that holds all the plugin info

//call the start_processing function on the plugin, can only be used on the audio rt thread
//also the plugin has to be activated (plug_inst_activated == 1), since this function cant even check this var, because of a data race
int clap_plug_start_processing(CLAP_PLUG_INFO* plug_data, int plug_id);
//call the on_main_thread callback function on plug_id plugin, usually done after request_callback call
int clap_plug_callback(CLAP_PLUG_INFO* plug_data, int plug_id);
//activate the plugin, start_processing - send a message to the rt audio thread to call the start_processing function too
int clap_plug_activate(CLAP_PLUG_INFO* plug_data, int plug_id, unsigned int start_processing);
//restart the plug_id plugin, should be called only from main thread and will only restart a plugin that is activated
//start_processing - should the plugin call start_processing function (will happen on the audio thread possibly with a delay)
int clap_plug_restart(CLAP_PLUG_INFO* plug_data, int plug_id, unsigned int start_processing);
//return the ui to rt communication buffer, and put how many items are there into items var
//should be called only on the rt audio thread
RING_BUFFER* clap_get_ui_to_rt_msg_buffer(CLAP_PLUG_INFO* plug_data, unsigned int* items);
//return the rt to ui communication buffer, and put how many items are there into items var
RING_BUFFER* clap_get_rt_to_ui_msg_buffer(CLAP_PLUG_INFO* plug_data, unsigned int* items);
//return the ui to ui communication buffer, and put how many items are there into items var
RING_BUFFER* clap_get_ui_to_ui_msg_buffer(CLAP_PLUG_INFO* plug_data, unsigned int* items);
//return the names of the plugins in the plugin directory
char** clap_plug_return_plugin_names(unsigned int* size);
//initiate the main plugin data struct. 
CLAP_PLUG_INFO* clap_plug_init(uint32_t min_buffer_size, uint32_t max_buffer_size, SAMPLE_T samplerate, clap_plug_status_t* plug_error, void* audio_backend);
//initiate and load plugin from its name
int clap_plug_load_and_activate(CLAP_PLUG_INFO* plug_data, const char* plugin_name, int id);
//clean the plugin struct and free memory
void clap_plug_clean_memory(CLAP_PLUG_INFO* plug_data);
