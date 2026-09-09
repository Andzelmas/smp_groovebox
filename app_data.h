#pragma once
#include "structs.h"
#include "types.h"
#include "data_object.h"
#include "data_events.h"
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

// initialize the app_data and return the root DataObject. On failure the
// returned object's .ops is NULL. root_obj.user_data is the APP_INFO* cast to
// void*, kept by the caller for app_data_update / app_stop_and_clean.
DataObject app_init(void);

// pop the next queued data event into *out. returns false when the queue is
// empty. drained by the context layer once per nav_update, after app_data_update.
bool app_data_poll_event(void *root_user_data, DataEvent *out);

// Reads the rt_to_ui buffer and saves any context param values to their
// ui_params arrays. Might do some additional updating. root_user_data is the
// root DataObject's user_data (APP_INFO*).
void app_data_update(void *root_user_data);

// pause the [audio-thread] processing with a mutex and clean memory of the
// app_data. root_user_data is the root DataObject's user_data (APP_INFO*).
void app_stop_and_clean(void *root_user_data);
