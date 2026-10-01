#pragma once
#include "../structs.h"
#include "../util_funcs/tree_index.h"
#include "params.h"
#include <stdbool.h>
#include <stdlib.h>

enum PlugStatus { plug_failed_malloc, plug_failed_world_init };
// enum that holds the error statutes of the plugin
typedef enum PlugStatus plug_status_t;
// plugin port
typedef struct _plug_port PLUG_PORT;
// the single plugin that has plugin instance and plugin data
typedef struct _plug_plug PLUG_PLUG;
// the plugin object, that will holds the lilv world etc.
typedef struct _plug_info PLUG_INFO;
// the event buffer struct for atom sequences
typedef struct _plug_evbuf_impl PLUG_EVBUF;
// the event iterator for PLUG_EVBUF
typedef struct _plug_evbuf_iterator_impl PLUG_EVBUF_ITERATOR;

// inititialize the plugin host data. owner_tag is what the caller pairs with a
// plugin's uid to name the owner of that plugin's ports
PLUG_INFO *plug_init(uint32_t block_length, SAMPLE_T samplerate,
                     plug_status_t *plug_errors, void *audio_backend,
                     uint64_t owner_tag);

// Scans the installed plugins and (re)builds the list of them on plug_data.
// Safe to call again while plugins are loaded. A plugin keeps its key across
// rebuilds; lilv does not forget an uninstalled one, so it stays listed.
// Loaded plugins read their presets again on the next browse
int plug_plugin_list_init(PLUG_INFO *plug_data);

// return how many plugins are in the catalogue built by plug_plugin_list_init
unsigned int plug_plugin_list_count(PLUG_INFO *plug_data);

// return an item from the the plugin list, it can be used to load a plugin
void *plug_plugin_list_item_get(PLUG_INFO *plug_data, unsigned int idx);

// return the display name of the plugin list item. The string is owned by
// the catalogue entry and stays valid until the next plug_plugin_list_init.
// Returns NULL on error.
const char *plug_plugin_list_item_name(void *plug_list_item);

// key of the plugin list item, the same for the same plugin across
// plug_plugin_list_init calls. Never 0 for a listed item, 0 on error
uint64_t plug_plugin_list_item_key(void *plug_list_item);

// the listed item with this key, NULL when the current list has none
void *plug_plugin_list_item_by_key(PLUG_INFO *plug_data, uint64_t key);

// presets --------------------------------------------------
// A loaded plugin's presets, browsed one level at a time: banks first, then
// the presets outside any bank. branch is 0 for the top level, or a bank row's
// key. Built the first time it is browsed and kept until
// plug_plugin_list_init or the plugin is removed. A row's name is valid until
// then too.
size_t plug_plugin_presets_level_count(void *plug, uint64_t branch);
bool plug_plugin_presets_level_at(void *plug, uint64_t branch, size_t idx,
                                  TREE_ROW *out);

// load the preset with this key (a preset row's key). 0 on success, -1 when
// it failed to load, -2 when no preset has this key
int plug_plugin_preset_load(void *plug, uint64_t key);
//--------------------------------------------------

// read the main-thread audio-thread comm messages and launch apropriate
// functions (stop, start processes etc.)
int plug_read_rt_to_ui_messages(PLUG_INFO *plug_data);
int plug_read_ui_to_rt_messages(PLUG_INFO *plug_data);

// initialize a plugin instance. On success returns its identity uid (always
// > 0, matching plug_plugin_uid); on failure returns 0.
uint32_t plug_load_and_activate(void *plugin_item);

// return user_data for a single plugin
void *plug_plugin_return(PLUG_INFO *plug_data, unsigned int idx);

// return the plugin display name from user_data.
// the returned string is owned by the plugin and stays valid until the plugin
// is removed. returns NULL on error.
const char *plug_plugin_name(void *plug);

// return the plugin's monotonic identity uid (0 on error). Assigned at load,
// never reused for a different plugin.
uint32_t plug_plugin_uid(void *plug);

// return this plugin instance's own param container, NULL on error/none yet
PRM_CONTAIN *plug_plugin_param_container(void *plug);

// check if the plugins array changed (became dirty)
bool plug_plugins_is_dirty(PLUG_INFO *plug_data);

// set the samplerate of the plug_data, should usually be done before launching
// any plugins
void plug_set_samplerate(PLUG_INFO *plug_data, float new_sample_rate);

// set the buffer size, should be usually done before launching any plugins
void plug_set_block_length(PLUG_INFO *plug_data, uint32_t block_length);

// activate the ports, that the backend needs to activate, uses the callback
// function sent here returns 0 on success
int plug_activate_backend_ports(PLUG_INFO *plug_data, PLUG_PLUG *plug);

// connect the ports, run the plugins instances for nframes, and update the
// output ports, use on [audio-thread]
void plug_process_data_rt(PLUG_INFO *plug_data, unsigned int nframes);

// stop processing the plugin and remove it.
// plugin will be stopped on [audio-thread], if there is no [audio-thread] this
// can result in an infinite loop
int plug_stop_and_remove_plug(void *plug);

// clean the plug_data memory
void plug_clean_memory(PLUG_INFO *plug_data);
