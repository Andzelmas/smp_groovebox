#pragma once
#include <stdbool.h>
#include "../structs.h"
#include "../util_funcs/tree_index.h"
#include "params.h"

enum ClapPlugStatus {
    clap_plug_failed_malloc = -1,
};

typedef enum ClapPlugStatus clap_plug_status_t;

typedef struct _clap_plug_info
    CLAP_PLUG_INFO; // the struct that holds all the plugin info

// read the ui_to_rt messages on the [audio-thread] and call functions to stop
// or start processing the plugin or the whole clap context. nframes is this
// cycle's.
int clap_read_ui_to_rt_messages(CLAP_PLUG_INFO *plug_data,
                                unsigned int nframes);

// read the rt_to_ui messages on the [main-thread] and call functions to
// restart, activate, write to log and similar main thread functions. some of
// these called functions will block main-thread, to wait for the plugin or the
// whole process to stop.
int clap_read_rt_to_ui_messages(CLAP_PLUG_INFO *plug_data);

// presets --------------------------------------------------
// A loaded plugin's presets from its preset-discovery factory, browsed one
// level at a time: categories first (a location, then its sub directories),
// then presets. branch is 0 for the top level, or a category row's key. Built
// the first time it is browsed and kept until clap_plug_plugin_list_init or
// the plugin is removed. A row's name is valid until then too.
size_t clap_plug_presets_level_count(void *plug, uint64_t branch);
bool clap_plug_presets_level_at(void *plug, uint64_t branch, size_t idx,
                                TREE_ROW *out);

// load the preset with this key (a preset row's key) through the plugin's
// preset-load extension. 0 on success, -1 when it failed to load, -2 when no
// preset has this key
int clap_plug_preset_load(void *plug, uint64_t key);
//--------------------------------------------------

// initiate the main plugin data struct. owner_tag is what the caller pairs
// with a plugin's uid to name the owner of that plugin's ports
CLAP_PLUG_INFO *clap_plug_init(uint32_t min_buffer_size,
                               uint32_t max_buffer_size, SAMPLE_T samplerate,
                               clap_plug_status_t *plug_error,
                               void *audio_backend, uint64_t owner_tag);

// Scans the clap paths and (re)builds the list of available plugins.
// Safe to call again while plugins are loaded. A plugin keeps its key across
// rebuilds. Loaded plugins read their presets again on the next browse
int clap_plug_plugin_list_init(CLAP_PLUG_INFO *plug_data);

// return how many plugins are in the catalogue built by
// clap_plug_plugin_list_init
unsigned int clap_plug_plugin_list_count(CLAP_PLUG_INFO *plug_data);

// get one item from the plugin_list
void *clap_plug_plugin_list_item_get(CLAP_PLUG_INFO *plug_data, unsigned int idx);

// return the display name of the plugin list item. The string is owned by
// the catalogue entry and stays valid until the next
// clap_plug_plugin_list_init. Returns NULL on error.
const char *clap_plug_plugin_list_item_name(void *plugin_item);

// key of the plugin list item, the same for the same plugin (descriptor id)
// across clap_plug_plugin_list_init calls. Never 0 for a listed item, 0 on
// error
uint64_t clap_plug_plugin_list_item_key(void *plugin_item);

// the listed item with this key, NULL when the current list has none
void *clap_plug_plugin_list_item_by_key(CLAP_PLUG_INFO *plug_data,
                                        uint64_t key);

// initiate and load plugin from the plugin_list_item. On success returns its
// identity uid (always > 0, matching clap_plug_plugin_uid); on failure
// returns 0.
uint32_t clap_plug_load_and_activate(void* plugin_item);

// return the plugin user_data
void *clap_plug_plugin_return(CLAP_PLUG_INFO *plug_data, unsigned int idx);

// return the plugin display name from user_data.
// the returned string is owned by the plugin and stays valid until the plugin
// is removed. returns NULL on error.
const char *clap_plug_plugin_name(void *plug);

// return the plugin's monotonic identity uid (0 on error). Assigned at load,
// never reused for a different plugin.
uint32_t clap_plug_plugin_uid(void *plug);

// return if the plugins array is dirty - if it changed
bool clap_plug_plugins_is_dirty(CLAP_PLUG_INFO *plug_data);

// return this plugin instance's own param container, NULL on error/none yet
PRM_CONTAIN *clap_plug_plugin_param_container(void *plug);

// the slot-th plugin slot, loaded or not; NULL past the last slot.
// [audio-thread] safe, for the per-plugin process loop
void *clap_plug_plugin_slot(CLAP_PLUG_INFO *plug_data, unsigned int slot);

// process one plugin (a clap_plug_plugin_slot handle) on the [audio-thread].
// true if it wrote its outputs this cycle (false: stopped, sleeping, error)
bool clap_plug_plugin_process_rt(void *plug, NFRAMES_T nframes);

// remove the clap plugin
int clap_plug_plug_stop_and_clean(void *plug);

// clean the plugin struct and free memory
void clap_plug_clean_memory(CLAP_PLUG_INFO *plug_data);
