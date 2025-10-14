#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// my libraries
#include "app_data.h"
// string functions
#include "types.h"
#include "util_funcs/string_funcs.h"
// math helper functions
#include "contexts/context_control.h"
#include "contexts/params.h"
#include "jack_funcs/jack_funcs.h"
#include "util_funcs/log_funcs.h"
#include "util_funcs/math_funcs.h"
#include "util_funcs/ring_buffer.h"
#include <threads.h>
static thread_local bool is_audio_thread = false;

typedef struct _app_info {
    // the smapler data
    SMP_INFO *smp_data;
    // jack client for the whole program
    JACK_INFO *trk_jack;
    // plugin data
    PLUG_INFO *plug_data;
    // CLAP plugin data
    CLAP_PLUG_INFO *clap_plug_data;
    // built in synth data
    SYNTH_DATA *synth_data;

    // main ports for the app
    void *main_in_L;
    void *main_in_R;
    void *main_out_L;
    void *main_out_R;
    // control struct for sys messages between [audio-thread] and [main-thread]
    // (stop all processes and send messages for this context)
    CXCONTROL *control_data;
    unsigned int is_processing; // is the main jack function processing, should
                                // be touched only on [audio-thread]
} APP_INFO;

// clean memory, without pausing the [audio-thread]
static int clean_memory(APP_INFO *app_data) {
    if (!app_data)
        return -1;
    // clean the clap plug_data memory
    if (app_data->clap_plug_data)
        clap_plug_clean_memory(app_data->clap_plug_data);
    // clean the lv2 plug_data memory
    if (app_data->plug_data)
        plug_clean_memory(app_data->plug_data);
    // clean the sampler memory
    if (app_data->smp_data)
        smp_clean_memory(app_data->smp_data);
    // clean the synth memory
    if (app_data->synth_data)
        synth_clean_memory(app_data->synth_data);

    // clean the track jack memory
    if (app_data->trk_jack)
        jack_clean_memory(app_data->trk_jack);

    // clean the app_data
    context_sub_clean(app_data->control_data);
    if (app_data)
        free(app_data);

    return 0;
}

// stop the whole audio process, usually to close the program
// this function is used in the CXCONTROL
static int app_stop_process(void *user_data) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data)
        return -1;
    app_data->is_processing = 0;
    return 0;
}

// start the whole audio process, usually after app_init
static int app_start_process(void *user_data) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data)
        return -1;
    app_data->is_processing = 1;
    return 0;
}

// ui function to write to the log,
// used in CXCONTROL for non-realtime messaging
static int app_sys_msg(void *user_data, const char *msg) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data)
        return -1;
    log_append_logfile("%s", msg);
    return 0;
}

// read ring buffers sent from ui to rt thread
static int app_read_rt_messages(APP_INFO *app_data) {
    is_audio_thread = true;
    if (!app_data)
        return -1;
    // first read the app_data messages
    context_sub_process_rt(app_data->control_data);
    // if the process is stopped dont process the contexts
    if (app_data->is_processing == 0)
        return 1;

    // read the jack inner messages on the [audio-thread]
    app_jack_read_ui_to_rt_messages(app_data->trk_jack);
    // read the CLAP plugins inner messages on the [audio-thread]
    if (clap_read_ui_to_rt_messages(app_data->clap_plug_data) != 0)
        return -1;
    // read the lv2 plugin messages on the [audio-thread]
    if (plug_read_ui_to_rt_messages(app_data->plug_data) != 0)
        return -1;
    // read the sampler messages on the [audio-thread]
    if (smp_read_ui_to_rt_messages(app_data->smp_data) != 0)
        return -1;
    // read the synth messages on the [audio-thread]
    if (synth_read_ui_to_rt_messages(app_data->synth_data) != 0)
        return -1;
    return 0;
}

// The callback function sent to the audio backend (at this time to jack)
// The audio processing happens here, only realtime functions are allowed here
static int trk_audio_process_rt(NFRAMES_T nframes, void *arg) {
    // get the app data
    APP_INFO *app_data = (APP_INFO *)arg;
    if (app_data == NULL) {
        return -1;
    }
    // process messages from ui to rt thread, like start processing a plugin,
    // stop_processing a plugin, update rt param values etc. if returns a 1
    // value, this means that is_processing is 0 and the function should not
    // process any contexts a value of -1 means that a fundamental error occured
    int read_err = app_read_rt_messages(app_data);
    if (read_err == 1)
        return 0;
    if (read_err == -1)
        return -1;

    // process the SAMPLER DATA
    smp_sample_process_rt(app_data->smp_data, nframes);

    // process the PLUGIN DATA
    plug_process_data_rt(app_data->plug_data, nframes);

    // process the CLAP PLUGIN DATA
    clap_process_data_rt(app_data->clap_plug_data, nframes);

    // process the SYNTH DATA
    synth_process_rt(app_data->synth_data, nframes);

    // get the buffers for the trk_data, that is used as track summer
    SAMPLE_T *trk_in_L = app_jack_get_buffer_rt(app_data->main_in_L, nframes);
    SAMPLE_T *trk_in_R = app_jack_get_buffer_rt(app_data->main_in_R, nframes);
    SAMPLE_T *trk_out_L = app_jack_get_buffer_rt(app_data->main_out_L, nframes);
    SAMPLE_T *trk_out_R = app_jack_get_buffer_rt(app_data->main_out_R, nframes);
    // copy the Master track in  - to the Master track out
    if (!trk_in_L || !trk_in_R || !trk_out_L || !trk_out_R) {
        return -1;
    }

    memcpy(trk_out_L, trk_in_L, sizeof(SAMPLE_T) * nframes);
    memcpy(trk_out_R, trk_in_R, sizeof(SAMPLE_T) * nframes);

    return 0;
}

void *app_init(uint16_t *user_data_type, uint32_t *return_flags,
               char *root_name, int root_name_len) {
    APP_INFO *app_data = (APP_INFO *)malloc(sizeof(APP_INFO));
    if (!app_data)
        return NULL;

    snprintf(root_name, root_name_len, "%s", APP_NAME);

    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = app_start_process;
    rt_funcs_struct.subcx_stop_process = app_stop_process;
    ui_funcs_struct.send_msg = app_sys_msg;
    app_data->control_data = context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if (!app_data->control_data) {
        free(app_data);
        return NULL;
    }
    // init the members to NULLS
    app_data->smp_data = NULL;
    app_data->trk_jack = NULL;
    app_data->plug_data = NULL;
    app_data->clap_plug_data = NULL;
    app_data->synth_data = NULL;
    app_data->is_processing = 0;

    /*init jack client for the whole program*/
    /*--------------------------------------------------*/
    app_data->trk_jack =
        jack_initialize(app_data, APP_NAME, trk_audio_process_rt);
    if (!app_data->trk_jack) {
        clean_memory(app_data);
        return NULL;
    }

    uint32_t buffer_size =
        (uint32_t)app_jack_return_buffer_size(app_data->trk_jack);
    SAMPLE_T samplerate =
        (SAMPLE_T)app_jack_return_samplerate(app_data->trk_jack);
    // create ports for trk_jack
    app_data->main_in_L = app_jack_create_port_on_client(
        app_data->trk_jack, PORT_TYPE_AUDIO, PORT_FLOW_INPUT, "master_in_L");
    app_data->main_in_R = app_jack_create_port_on_client(
        app_data->trk_jack, PORT_TYPE_AUDIO, PORT_FLOW_INPUT, "master_in_R");
    app_data->main_out_L = app_jack_create_port_on_client(
        app_data->trk_jack, PORT_TYPE_AUDIO, PORT_FLOW_OUTPUT, "master_out_L");
    app_data->main_out_R = app_jack_create_port_on_client(
        app_data->trk_jack, PORT_TYPE_AUDIO, PORT_FLOW_OUTPUT, "master_out_R");
    // now activate the jack client, it will launch the rt thread
    // (trk_audio_process_rt function) but app_data->is_processing == 0, so the
    // contexts will not be processed, only app_data sys messages (to start the
    // processes for example)
    if (app_jack_activate(app_data->trk_jack) != 0) {
        clean_memory(app_data);
        return NULL;
    }
    /*initiate the sampler it will be empty initialy*/
    /*-----------------------------------------------*/
    smp_status_t smp_status_err = 0;
    app_data->smp_data =
        smp_init(buffer_size, samplerate, &smp_status_err, app_data->trk_jack);
    if (!app_data->smp_data) {
        // clean app_data
        clean_memory(app_data);
        return NULL;
    }
    /*--------------------------------------------------*/
    // Init the plugin data object, it will not run any plugins yet
    plug_status_t plug_errors = 0;
    app_data->plug_data =
        plug_init(buffer_size, samplerate, &plug_errors, app_data->trk_jack);
    if (!app_data->plug_data) {
        clean_memory(app_data);
        return NULL;
    }

    clap_plug_status_t clap_plug_errors = 0;
    app_data->clap_plug_data =
        clap_plug_init(buffer_size, buffer_size, samplerate, &clap_plug_errors,
                       app_data->trk_jack);
    if (!(app_data->clap_plug_data)) {
        clean_memory(app_data);
        return NULL;
    }

    // initiate the Synth data
    app_data->synth_data = synth_init((unsigned int)buffer_size, samplerate,
                                      "Synth", 1, app_data->trk_jack);
    if (!app_data->synth_data) {
        clean_memory(app_data);
        return NULL;
    }
    // now unpause the jack function again
    context_sub_wait_for_start(app_data->control_data, (void *)app_data);
    *user_data_type = USER_DATA_T_ROOT;
    *return_flags = (INTRF_FLAG_ROOT | INTRF_FLAG_CONTAINER);
    return (void *)app_data;
}

// Get the parameter container for the context
// PRM_CONTAIN can be used to set, get param values, get their names, etc.
static PRM_CONTAIN *app_get_context_param_container(APP_INFO *app_data,
                                                    unsigned char cx_type,
                                                    int cx_id) {
    if (!app_data)
        return NULL;
    if (cx_type == Context_type_Trk) {
        return app_jack_param_return_param_container(app_data->trk_jack);
    }
    if (cx_type == Context_type_Sampler) {
        return smp_param_return_param_container(app_data->smp_data, cx_id);
    }
    if (cx_type == Context_type_Synth) {
        return synth_param_return_param_container(app_data->synth_data, cx_id);
    }
    if (cx_type == Context_type_Plugins) {
        return plug_param_return_param_container(app_data->plug_data, cx_id);
    }
    if (cx_type == Context_type_Clap_Plugins) {
        return clap_plug_param_return_param_container(app_data->clap_plug_data,
                                                      cx_id);
    }
    return NULL;
}

void *app_data_child_return(void *parent_data, uint16_t parent_type,
                            uint16_t *return_type, uint32_t *return_flags,
                            char *return_name, int return_name_len,
                            unsigned int idx) {
    if (!parent_data)
        return NULL;
    if (parent_type == USER_DATA_T_ROOT) {
        APP_INFO *app_data = (APP_INFO *)parent_data;
        switch (idx) {
        case 0:
            *return_type = USER_DATA_T_SAMPLER;
            snprintf(return_name, return_name_len, "%s", SAMPLER_NAME);
            *return_flags = (INTRF_FLAG_CONTAINER | INTRF_FLAG_ON_TOP);
            return (void *)app_data;
        case 1:
            *return_type = USER_DATA_T_PLUGINS;
            snprintf(return_name, return_name_len, "%s", PLUGINS_NAME);
            *return_flags = (INTRF_FLAG_CONTAINER | INTRF_FLAG_ON_TOP);
            return (void *)app_data;
        case 2:
            *return_type = USER_DATA_T_SYNTH;
            snprintf(return_name, return_name_len, "%s", SYNTH_NAME);
            *return_flags = (INTRF_FLAG_CONTAINER | INTRF_FLAG_ON_TOP);
            return (void *)app_data;
        case 3:
            *return_type = USER_DATA_T_JACK;
            snprintf(return_name, return_name_len, "%s", TRK_NAME);
            *return_flags = (INTRF_FLAG_CONTAINER | INTRF_FLAG_ON_TOP);
            return (void *)app_data;
        }
    }
    // PLUGINS context
    //----------------------------------------------------------------------------------------------------
    if (parent_type == USER_DATA_T_PLUGINS) {
        APP_INFO *app_data = (APP_INFO *)parent_data;
        // This context is to create new plugins
        // one context for lv2 lists and the other for clap plugin lists
        if (idx == 0) {
            *return_type = USER_DATA_T_PLUGINS_LV2_NEW;
            snprintf(return_name, return_name_len, "%s", NAME_LV2_ADD_NEW);
            *return_flags = (INTRF_FLAG_CONTAINER | INTRF_FLAG_LIST |
                             INTRF_FLAG_CANT_DIRTY);
            return (void *)app_data;
        }
        if (idx == 1){
            *return_type = USER_DATA_T_PLUGINS_CLAP_NEW;
            snprintf(return_name, return_name_len, "%s", NAME_CLAP_ADD_NEW);
            *return_flags = (INTRF_FLAG_CONTAINER | INTRF_FLAG_LIST |
                             INTRF_FLAG_CANT_DIRTY);
            return (void *)app_data;
        }
        // TODO idx > 1 go through loaded plugins
    }
    // Return children for the lv2 plugins list
    if (parent_type == USER_DATA_T_PLUGINS_LV2_NEW) {
        APP_INFO *app_data = (APP_INFO *)parent_data;
        if (idx == 0) {
            // button to recreate the lv2 plugin list
            *return_type = USER_DATA_T_PLUGINS_LV2_LIST_REFRESH;
            snprintf(return_name, return_name_len, "%s", NAME_REFRESH_LIST);
            *return_flags = (INTRF_FLAG_INTERACT | INTRF_FLAG_ON_TOP);
            return (void *)app_data;
        }
        if (idx > 0) {
            unsigned int iter = idx - 1;
            void *plugins_list_item_user_data =
                plug_plugin_list_item_get(app_data->plug_data, iter);
            if (plugins_list_item_user_data) {
                *return_type = USER_DATA_T_PLUGINS_LV2_LIST_ITEM;
                plug_plugin_list_item_name(plugins_list_item_user_data,
                                           return_name, return_name_len);
                *return_flags = (INTRF_FLAG_INTERACT | INTRF_FLAG_LIST_ITEM);
                return plugins_list_item_user_data;
            }
        }
    }
    // Return children for the clap plugins list
    if (parent_type == USER_DATA_T_PLUGINS_CLAP_NEW) {
        APP_INFO *app_data = (APP_INFO *)parent_data;
        if (idx == 0) {
            *return_type = USER_DATA_T_PLUGINS_CLAP_LIST_REFRESH;
            snprintf(return_name, return_name_len, "%s", NAME_REFRESH_LIST);
            *return_flags = (INTRF_FLAG_INTERACT | INTRF_FLAG_ON_TOP);
            return (void *)app_data;
        }
        if (idx > 0) {
            // TODO get a list of the clap plugins
        }
    }
    //----------------------------------------------------------------------------------------------------

    return NULL;
}

void app_data_invoke(void *user_data, uint16_t user_data_type,
                     const char *file) {
    if (!user_data)
        return;
    // PLUGINS context
    //----------------------------------------------------------------------------------------------------
    // user pressed on the button to recreate the lv2 plugin list
    if (user_data_type == USER_DATA_T_PLUGINS_LV2_LIST_REFRESH){
        APP_INFO* app_data = (APP_INFO*)user_data;
        plug_plugin_list_init(app_data->plug_data);
    }
    // user pressed on the button to recreate the clap plugin list
    if (user_data_type == USER_DATA_T_PLUGINS_CLAP_LIST_REFRESH){
        APP_INFO* app_data = (APP_INFO*)user_data;
        // TODO function to init the clap plugin list
    }
    //----------------------------------------------------------------------------------------------------
}

bool app_data_is_dirty(void *user_data, uint16_t user_data_type) {
    if (!user_data)
        return false;
    // PLUGINS context
    //----------------------------------------------------------------------------------------------------
    // check if the lv2 plugin list to add new plugins is dirty
    if (user_data_type == USER_DATA_T_PLUGINS_LV2_NEW) {
        APP_INFO *app_data = (APP_INFO *)user_data;
        return plug_plugin_list_is_dirty(app_data->plug_data);
    }
    //----------------------------------------------------------------------------------------------------
    return false;
}


void app_data_update(void *user_data, uint16_t user_data_type) {
    if (user_data_type != USER_DATA_T_ROOT)
        return;
    if (!user_data)
        return;
    APP_INFO *app_data = (APP_INFO *)user_data;
    // read app_data messages from [audio-thread] on the [main-thread]
    context_sub_process_ui(app_data->control_data);
    // read messages for jack from rt thread on [main-thread]
    app_jack_read_rt_to_ui_messages(app_data->trk_jack);
    // read messages from rt thread on [main-thread] for CLAP plugins
    clap_read_rt_to_ui_messages(app_data->clap_plug_data);
    // read messages from the rt thread on the [main-thread] for lv2 plugins
    plug_read_rt_to_ui_messages(app_data->plug_data);
    // read messages from the rt thread on the [main-thread] for sampler
    smp_read_rt_to_ui_messages(app_data->smp_data);
    // read messages from the rt thread on the [main-thread] for the synth
    // context
    synth_read_rt_to_ui_messages(app_data->synth_data);
}

void app_stop_and_clean(void *user_data, uint16_t type) {
    if (type != USER_DATA_T_ROOT)
        return;
    if (!user_data)
        return;
    APP_INFO *app_data = (APP_INFO *)user_data;
    context_sub_wait_for_stop(app_data->control_data, user_data);

    clean_memory(app_data);
}
