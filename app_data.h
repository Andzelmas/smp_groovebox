#pragma once
#include "structs.h"
#include "types.h"
#include <stdatomic.h>
// sampler context
#include "contexts/sampler.h"
// plugin context
#include "contexts/clap_plugins.h"
#include "contexts/plugins.h"
#include "contexts/synth.h"

enum AppPluginType {
    // lv2 plugin
    LV2_plugin_type = 0x01,
    CLAP_plugin_type = 0x02
};

// this is the struct for the data on the whole app
typedef struct _app_info APP_INFO;

// initialize the app_data, user_data_type returns USER_DATA_T_ROOT type, flags
// are the root/main context flags the function returns APP_INFO* cast to void*
void *app_init(uint16_t *user_data_type, uint32_t *return_flags,
               char *root_name, int root_name_len);

// get the idx child of the parent_data, if idx is out of bounds return NULL
// return_type is the type of the returned user_data, to know what to cast void*
// user_data to, flags are for the UI side of things return_name will be unique,
// but only among the children in parent_data
// ALL DATA THAT THIS FUNCTION GETS SHOULD BE CREATED on initialization or in app_data_invoke()
// For example plugin lists, preset lists and the like should be created in plugin contexts,
// when initializing the plugin context or a specific plugin or when the
// user asks to "refresh" these lists.
void *app_data_child_return(void *parent_data, uint16_t parent_type,
                            uint16_t *return_type, uint32_t *return_flags,
                            char *return_name, int return_name_len,
                            unsigned int idx);

// invoke the user_data, this is a callback for "buttons"
void app_data_invoke(void *user_data, uint16_t user_data_type,
                     const char *file);

// check if the user_data is dirty and the context needs to be recreated
bool app_data_is_dirty(void *user_data, uint16_t user_data_type);

// Reads the rt_to_ui buffer and saves any context param values to their
// ui_params arrays. Might do some additional updating
void app_data_update(void *user_data, uint16_t user_data_type);

// pause the [audio-thread] processing with a mutex and clean memory of the
// app_data
void app_stop_and_clean(void *user_data, uint16_t type);
