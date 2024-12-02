#pragma once
#include "../structs.h"

enum ClapPlugStatus{
    clap_plug_failed_malloc = -1,
};
typedef enum ClapPlugStatus clap_plug_status_t;

//this holds the plugin id and the enum (from type.h MSGfromRT) to tell what to do with the plugin
typedef struct _clap_ring_sys_msg{
    unsigned int msg_enum; //what to do with the plugin
    int plug_id; //plugin id on the plugins array that needs to be changed somehow
}CLAP_RING_SYS_MSG;

typedef struct _clap_plug_info CLAP_PLUG_INFO; //the struct that holds all the plugin info
//return the names of the plugins in the plugin directory
char** clap_plug_return_plugin_names(unsigned int* size);
//initiate the main plugin data struct. 
CLAP_PLUG_INFO* clap_plug_init(uint32_t min_buffer_size, uint32_t max_buffer_size, SAMPLE_T samplerate, clap_plug_status_t* plug_error, void* audio_backend);
//initiate and load plugin from its name
int clap_plug_load_and_activate(CLAP_PLUG_INFO* plug_data, const char* plugin_name, int id);
//clean the plugin struct and free memory
void clap_plug_clean_memory(CLAP_PLUG_INFO* plug_data);
