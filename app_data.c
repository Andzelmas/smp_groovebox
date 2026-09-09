#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// my libraries
#include "app_data.h"
// string functions
#include "contexts/clap_plugins.h"
#include "contexts/plugins.h"
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

/* ----------------------------------------------------------------------------
 * DataObject adapter tables.
 *
 * app_data is the single place that knows how to map program state onto the
 * DataObject contract. The context modules (plugins.c, clap_plugins.c, ...)
 * stay unaware of DataObject: they only expose their own domain API and this
 * file adapts it.
 * ------------------------------------------------------------------------- */

/* Identity composition. A ContextId is 8 bits of namespace (which module /
 * whether it is a singleton) in the top byte, plus a local part below: a
 * fixed constant for singletons, a module uid for the rest. app_data is the
 * only place this is assembled; every layer above treats the ContextId as
 * opaque. Namespaces start at 1 so a real id always has a non-zero namespace. */
enum {
    DATA_NS_SINGLETON = 1,
    DATA_NS_SAMPLE = 2,
    DATA_NS_LV2_PLUG = 3,
    DATA_NS_CLAP_PLUG = 4,
    DATA_NS_SYNTH_OSC = 5,
};
#define MAKE_ID(ns, local)                                                      \
    (((ContextId)(ns) << CTXID_NS_SHIFT) | (ContextId)(uint32_t)(local))

// local part for the DATA_NS_SINGLETON namespace (one per singleton context)
enum {
    SID_ROOT = 1,
    SID_SAMPLER,
    SID_LV2_LIST,
    SID_CLAP_LIST,
    SID_SYNTH,
    SID_TRK,
};

// the Trk container: constant name, no children. user_data is unused.
static size_t trk_child_count(void *user_data) {
    (void)user_data;
    return 0;
}
static bool trk_child_at(void *user_data, size_t idx, DataObject *out) {
    (void)user_data;
    (void)idx;
    (void)out;
    return false;
}
static const char *trk_name(void *user_data) {
    (void)user_data;
    return TRK_NAME;
}
static ContextId trk_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_TRK);
}
static const DataOps trk_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = trk_id,
    .child_count = trk_child_count,
    .child_at = trk_child_at,
    .name = trk_name,
};

// single loaded sample. user_data is the SMP_SMP* from smp_sample_return.
static size_t sample_child_count(void *user_data) {
    (void)user_data;
    return 0;
}
static bool sample_child_at(void *user_data, size_t idx, DataObject *out) {
    (void)user_data;
    (void)idx;
    (void)out;
    return false;
}
static ContextId sample_id(void *user_data) {
    return MAKE_ID(DATA_NS_SAMPLE, smp_sample_uid(user_data));
}
// smp_sample_name already matches DataOps.name (const char *(*)(void *))
static const DataOps sample_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = sample_id,
    .child_count = sample_child_count,
    .child_at = sample_child_at,
    .name = smp_sample_name,
};

// container of the loaded samples. user_data is SMP_INFO*.
static size_t sampler_child_count(void *user_data) {
    size_t count = 0;
    while (smp_sample_return((SMP_INFO *)user_data, (unsigned int)count))
        count++;
    return count;
}
static bool sampler_child_at(void *user_data, size_t idx, DataObject *out) {
    void *smp = smp_sample_return((SMP_INFO *)user_data, (unsigned int)idx);
    if (!smp)
        return false;
    out->ops = &sample_ops;
    out->user_data = smp;
    return true;
}
static const char *sampler_name(void *user_data) {
    (void)user_data;
    return SAMPLER_NAME;
}
static ContextId sampler_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_SAMPLER);
}
static const DataOps sampler_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = sampler_id,
    .child_count = sampler_child_count,
    .child_at = sampler_child_at,
    .name = sampler_name,
};

// single loaded lv2 plugin. user_data is the PLUG_PLUG* from plug_plugin_return.
static size_t lv2_plugin_child_count(void *user_data) {
    (void)user_data;
    return 0;
}
static bool lv2_plugin_child_at(void *user_data, size_t idx, DataObject *out) {
    (void)user_data;
    (void)idx;
    (void)out;
    return false;
}
static ContextId lv2_plugin_id(void *user_data) {
    return MAKE_ID(DATA_NS_LV2_PLUG, plug_plugin_uid(user_data));
}
// plug_plugin_name already matches DataOps.name (const char *(*)(void *))
static const DataOps lv2_plugin_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = lv2_plugin_id,
    .child_count = lv2_plugin_child_count,
    .child_at = lv2_plugin_child_at,
    .name = plug_plugin_name,
};

// container of the loaded lv2 plugins. user_data is PLUG_INFO*.
static size_t lv2_plugins_child_count(void *user_data) {
    size_t count = 0;
    while (plug_plugin_return((PLUG_INFO *)user_data, (unsigned int)count))
        count++;
    return count;
}
static bool lv2_plugins_child_at(void *user_data, size_t idx, DataObject *out) {
    void *plug = plug_plugin_return((PLUG_INFO *)user_data, (unsigned int)idx);
    if (!plug)
        return false;
    out->ops = &lv2_plugin_ops;
    out->user_data = plug;
    return true;
}
static const char *lv2_plugins_name(void *user_data) {
    (void)user_data;
    return PLUGINS_LV2_NAME;
}
static ContextId lv2_plugins_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_LV2_LIST);
}
static const DataOps lv2_plugins_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = lv2_plugins_id,
    .child_count = lv2_plugins_child_count,
    .child_at = lv2_plugins_child_at,
    .name = lv2_plugins_name,
};

// single loaded clap plugin. user_data is the plugin handle from
// clap_plug_plugin_return.
static size_t clap_plugin_child_count(void *user_data) {
    (void)user_data;
    return 0;
}
static bool clap_plugin_child_at(void *user_data, size_t idx, DataObject *out) {
    (void)user_data;
    (void)idx;
    (void)out;
    return false;
}
static ContextId clap_plugin_id(void *user_data) {
    return MAKE_ID(DATA_NS_CLAP_PLUG, clap_plug_plugin_uid(user_data));
}
// clap_plug_plugin_name already matches DataOps.name (const char *(*)(void *))
static const DataOps clap_plugin_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = clap_plugin_id,
    .child_count = clap_plugin_child_count,
    .child_at = clap_plugin_child_at,
    .name = clap_plug_plugin_name,
};

// container of the loaded clap plugins. user_data is CLAP_PLUG_INFO*.
static size_t clap_plugins_child_count(void *user_data) {
    size_t count = 0;
    while (clap_plug_plugin_return((CLAP_PLUG_INFO *)user_data,
                                   (unsigned int)count))
        count++;
    return count;
}
static bool clap_plugins_child_at(void *user_data, size_t idx, DataObject *out) {
    void *plug =
        clap_plug_plugin_return((CLAP_PLUG_INFO *)user_data, (unsigned int)idx);
    if (!plug)
        return false;
    out->ops = &clap_plugin_ops;
    out->user_data = plug;
    return true;
}
static const char *clap_plugins_name(void *user_data) {
    (void)user_data;
    return PLUGINS_CLAP_NAME;
}
static ContextId clap_plugins_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_CLAP_LIST);
}
static const DataOps clap_plugins_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = clap_plugins_id,
    .child_count = clap_plugins_child_count,
    .child_at = clap_plugins_child_at,
    .name = clap_plugins_name,
};

// single synth oscillator. user_data is the handle from synth_osc_return - it
// represents "oscillator number N" on the synth, not a SYNTH_OSC* pointer.
static size_t osc_child_count(void *user_data) {
    (void)user_data;
    return 0;
}
static bool osc_child_at(void *user_data, size_t idx, DataObject *out) {
    (void)user_data;
    (void)idx;
    (void)out;
    return false;
}
static ContextId osc_id(void *user_data) {
    return MAKE_ID(DATA_NS_SYNTH_OSC, synth_osc_uid(user_data));
}
// synth_osc_name already matches DataOps.name (const char *(*)(void *))
static const DataOps osc_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = osc_id,
    .child_count = osc_child_count,
    .child_at = osc_child_at,
    .name = synth_osc_name,
};

// container of the synth oscillators. user_data is SYNTH_DATA*. Oscillators are
// fixed at init (never added/removed), so there are no gaps to walk.
static size_t synth_child_count(void *user_data) {
    return synth_return_osc_num((SYNTH_DATA *)user_data);
}
static bool synth_child_at(void *user_data, size_t idx, DataObject *out) {
    void *osc = synth_osc_return((SYNTH_DATA *)user_data, (unsigned int)idx);
    if (!osc)
        return false;
    out->ops = &osc_ops;
    out->user_data = osc;
    return true;
}
static const char *synth_name(void *user_data) {
    (void)user_data;
    return SYNTH_NAME;
}
static ContextId synth_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_SYNTH);
}
static const DataOps synth_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = synth_id,
    .child_count = synth_child_count,
    .child_at = synth_child_at,
    .name = synth_name,
};

// root object. user_data is APP_INFO*.
static size_t root_child_count(void *user_data) {
    (void)user_data;
    return 5;
}
static bool root_child_at(void *user_data, size_t idx, DataObject *out) {
    APP_INFO *app_data = (APP_INFO *)user_data;
    if (!app_data)
        return false;
    switch (idx) {
    case 0:
        out->ops = &sampler_ops;
        out->user_data = app_data->smp_data;
        return true;
    case 1:
        out->ops = &lv2_plugins_ops;
        out->user_data = app_data->plug_data;
        return true;
    case 2:
        out->ops = &clap_plugins_ops;
        out->user_data = app_data->clap_plug_data;
        return true;
    case 3:
        out->ops = &synth_ops;
        out->user_data = app_data->synth_data;
        return true;
    case 4:
        out->ops = &trk_ops;
        out->user_data = app_data->trk_jack;
        return true;
    default:
        return false;
    }
}
static const char *root_name(void *user_data) {
    (void)user_data;
    return APP_NAME;
}
static ContextId root_id(void *user_data) {
    (void)user_data;
    return MAKE_ID(DATA_NS_SINGLETON, SID_ROOT);
}
static const DataOps root_ops = {
    .capabilities = DATA_CAP_NAME | DATA_CAP_CHILDREN,
    .id = root_id,
    .child_count = root_child_count,
    .child_at = root_child_at,
    .name = root_name,
};

// dispatch table for the temporary app_data_is_dirty bridge
static bool sampler_is_dirty(void *user_data) {
    return smp_samples_is_dirty((SMP_INFO *)user_data);
}
static bool lv2_plugins_is_dirty(void *user_data) {
    return plug_plugins_is_dirty((PLUG_INFO *)user_data);
}
static bool clap_plugins_is_dirty(void *user_data) {
    return clap_plug_plugins_is_dirty((CLAP_PLUG_INFO *)user_data);
}

DataObject app_init(void) {
    const DataObject invalid = {0};
    APP_INFO *app_data = (APP_INFO *)malloc(sizeof(APP_INFO));
    if (!app_data)
        return invalid;

    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = app_start_process;
    rt_funcs_struct.subcx_stop_process = app_stop_process;
    ui_funcs_struct.send_msg = app_sys_msg;
    app_data->control_data = context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if (!app_data->control_data) {
        free(app_data);
        return invalid;
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
        return invalid;
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
        return invalid;
    }
    /*initiate the sampler it will be empty initialy*/
    /*-----------------------------------------------*/
    smp_status_t smp_status_err = 0;
    app_data->smp_data =
        smp_init(buffer_size, samplerate, &smp_status_err, app_data->trk_jack);
    if (!app_data->smp_data) {
        // clean app_data
        clean_memory(app_data);
        return invalid;
    }
    /*--------------------------------------------------*/
    // Init the plugin data object, it will not run any plugins yet
    plug_status_t plug_errors = 0;
    app_data->plug_data =
        plug_init(buffer_size, samplerate, &plug_errors, app_data->trk_jack);
    if (!app_data->plug_data) {
        clean_memory(app_data);
        return invalid;
    }

    clap_plug_status_t clap_plug_errors = 0;
    app_data->clap_plug_data =
        clap_plug_init(buffer_size, buffer_size, samplerate, &clap_plug_errors,
                       app_data->trk_jack);
    if (!(app_data->clap_plug_data)) {
        clean_memory(app_data);
        return invalid;
    }

    // initiate the Synth data
    app_data->synth_data = synth_init((unsigned int)buffer_size, samplerate,
                                      "Synth", 1, app_data->trk_jack);
    if (!app_data->synth_data) {
        clean_memory(app_data);
        return invalid;
    }
    // now unpause the jack function again
    context_sub_wait_for_start(app_data->control_data, (void *)app_data);

    DataObject root = {.ops = &root_ops, .user_data = (void *)app_data};
    return root;
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

bool app_data_is_dirty(const DataObject *obj) {
    if (!data_obj_valid(obj))
        return false;
    // TEMPORARY BRIDGE: dispatch on the known ops tables until the
    // generation / removal notification system replaces this.
    if (obj->ops == &sampler_ops)
        return sampler_is_dirty(obj->user_data);
    if (obj->ops == &lv2_plugins_ops)
        return lv2_plugins_is_dirty(obj->user_data);
    if (obj->ops == &clap_plugins_ops)
        return clap_plugins_is_dirty(obj->user_data);
    return false;
}

void app_data_update(void *root_user_data) {
    if (!root_user_data)
        return;
    APP_INFO *app_data = (APP_INFO *)root_user_data;
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

void app_stop_and_clean(void *root_user_data) {
    if (!root_user_data)
        return;
    APP_INFO *app_data = (APP_INFO *)root_user_data;
    context_sub_wait_for_stop(app_data->control_data, root_user_data);

    clean_memory(app_data);
}
