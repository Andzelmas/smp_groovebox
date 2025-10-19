#pragma once
#include <stdbool.h>
#include "../structs.h"
#include "params.h"

enum ClapPlugStatus {
    clap_plug_failed_malloc = -1,
};

typedef enum ClapPlugStatus clap_plug_status_t;

typedef struct _clap_plug_info
    CLAP_PLUG_INFO; // the struct that holds all the plugin info

// read the ui_to_rt messages on the [audio-thread] and call functions to stop
// or start processing the plugin or the whole clap context.
int clap_read_ui_to_rt_messages(CLAP_PLUG_INFO *plug_data);

// read the rt_to_ui messages on the [main-thread] and call functions to
// restart, activate, write to log and similar main thread functions. some of
// these called functions will block main-thread, to wait for the plugin or the
// whole process to stop.
int clap_read_rt_to_ui_messages(CLAP_PLUG_INFO *plug_data);

// iterate through the presets and return the preset struct, that can be used to
// return the preset short name, path, categories user has to call
// clap_plug_presets_clean_preset function after done with the struct
void *clap_plug_presets_iterate(CLAP_PLUG_INFO *plug_data,
                                unsigned int plug_idx, uint32_t iter);

// return the short name of the preset
int clap_plug_presets_name_return(CLAP_PLUG_INFO *plug_data, void *preset_info,
                                  char *name, uint32_t name_len);

// return the full path of the preset
int clap_plug_presets_path_return(CLAP_PLUG_INFO *plug_data, void *preset_info,
                                  char *path, uint32_t path_len);

// return the category in the preset_info struct. Categories is a string,
// separated by "/", if idx is too big will return -1
int clap_plug_presets_categories_iterate(CLAP_PLUG_INFO *plug_data,
                                         void *preset_info, char *category,
                                         uint32_t category_len, uint32_t idx);

// clean the preset_info struct returned from the clap_plug_presets_iterate
// function
void clap_plug_presets_clean_preset(CLAP_PLUG_INFO *plug_data,
                                    void *preset_info);

// look for and load if found a preset from the full_path
int clap_plug_preset_load_from_path(CLAP_PLUG_INFO *plug_data, int plug_id,
                                    const char *preset_path);

// initiate the main plugin data struct.
CLAP_PLUG_INFO *clap_plug_init(uint32_t min_buffer_size,
                               uint32_t max_buffer_size, SAMPLE_T samplerate,
                               clap_plug_status_t *plug_error,
                               void *audio_backend);

// return the plugin parameter container
PRM_CONTAIN *clap_plug_param_return_param_container(CLAP_PLUG_INFO *plug_data,
                                                    int plug_id);

// initialize the plugin list
// it contains all the plugins on the system available to the user
int clap_plug_plugin_list_init(CLAP_PLUG_INFO *plug_data);

// get one item from the plugin_list
void *clap_plug_plugin_list_item_get(CLAP_PLUG_INFO *plug_data, unsigned int idx);

// get the short name of the plugin from the plugin_list
int clap_plug_plugin_list_item_name(void *plugin_item, char *return_name,
                                    unsigned int return_name_len); 

// return if the plugin_list has been changed or not
bool clap_plug_plugin_list_is_dirty(CLAP_PLUG_INFO* plug_data);

// initiate and load plugin from the plugin_list_item 
int clap_plug_load_and_activate(void* plugin_item);

// return the plugin user_data
void *clap_plug_plugin_return(CLAP_PLUG_INFO *plug_data, unsigned int idx);

// return the name of a plugin, returned name will be id_name format
int clap_plug_plugin_name(void *plug, char *return_name,
                                 unsigned int return_name_len);

// return if the plugins array is dirty - if it changed
bool clap_plug_plugins_is_dirty(CLAP_PLUG_INFO *plug_data);

// process the clap plugins, must be called on the [audio-thread]
void clap_process_data_rt(CLAP_PLUG_INFO *plug_data, unsigned int nframes);

// remove the clap plugin
int clap_plug_plug_stop_and_clean(void *plug);

// clean the plugin struct and free memory
void clap_plug_clean_memory(CLAP_PLUG_INFO *plug_data);
