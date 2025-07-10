#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <semaphore.h>
//my libraries
#include "app_data.h"
//string functions
#include "util_funcs/string_funcs.h"
//math helper functions
#include "util_funcs/math_funcs.h"
#include "contexts/context_control.h"
#include "jack_funcs/jack_funcs.h"
#include "util_funcs/log_funcs.h"
#include "util_funcs/ring_buffer.h"
#include "contexts/params.h"
#include <threads.h>
static thread_local bool is_audio_thread = false;

typedef struct _app_info{
    //the smapler data
    SMP_INFO* smp_data;
    //jack client for the whole program
    JACK_INFO* trk_jack;
    //plugin data
    PLUG_INFO* plug_data;
    //CLAP plugin data
    CLAP_PLUG_INFO* clap_plug_data;
    //built in synth data
    SYNTH_DATA* synth_data;
    
     //main ports for the app
    void* main_in_L;
    void* main_in_R;
    void* main_out_L;
    void* main_out_R;
    //control struct for sys messages between [audio-thread] and [main-thread] (stop all processes and send messages for this context)
    CXCONTROL* control_data;
    unsigned int is_processing; //is the main jack function processing, should be touched only on [audio-thread]
}APP_INFO;

//clean memory, without pausing the [audio-thread]
static int clean_memory(APP_INFO* app_data){
    if(!app_data)return -1;
    //clean the clap plug_data memory
    if(app_data->clap_plug_data)clap_plug_clean_memory(app_data->clap_plug_data);
    //clean the lv2 plug_data memory
    if(app_data->plug_data)plug_clean_memory(app_data->plug_data);
    //clean the sampler memory
    if(app_data->smp_data) smp_clean_memory(app_data->smp_data);
    //clean the synth memory
    if(app_data->synth_data) synth_clean_memory(app_data->synth_data);

    //clean the track jack memory
    if(app_data->trk_jack)jack_clean_memory(app_data->trk_jack);

    //clean the app_data
    context_sub_clean(app_data->control_data);
    if(app_data)free(app_data);
    
    return 0;
}
static int app_stop_process(void* user_data){
    APP_INFO* app_data = (APP_INFO*)user_data;
    if(!app_data)return -1;
    app_data->is_processing = 0;
    return 0;
}
static int app_start_process(void* user_data){
    APP_INFO* app_data = (APP_INFO*)user_data;
    if(!app_data)return -1;
    app_data->is_processing = 1;
    return 0;
}
static int app_sys_msg(void* user_data, const char* msg){
    APP_INFO* app_data = (APP_INFO*)user_data;
    if(!app_data)return -1;
    log_append_logfile("%s", msg);
    return 0;
}
void* app_init(uint16_t* user_data_type){
    APP_INFO *app_data = (APP_INFO*) malloc(sizeof(APP_INFO));
    if(!app_data) return NULL;
    
    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = app_start_process;
    rt_funcs_struct.subcx_stop_process = app_stop_process;
    ui_funcs_struct.send_msg = app_sys_msg;
    app_data->control_data = context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if(!app_data->control_data){
	free(app_data);
	return NULL;
    }
    //init the members to NULLS
    app_data->smp_data = NULL;
    app_data->trk_jack = NULL;
    app_data->plug_data = NULL;
    app_data->clap_plug_data = NULL;
    app_data->synth_data = NULL;
    app_data->is_processing = 0;
    
    /*init jack client for the whole program*/
    /*--------------------------------------------------*/   
    app_data->trk_jack = jack_initialize(app_data, APP_NAME, trk_audio_process_rt);
    if(!app_data->trk_jack){
	clean_memory(app_data);
	return NULL;
    }

    uint32_t buffer_size = (uint32_t)app_jack_return_buffer_size(app_data->trk_jack);
    SAMPLE_T samplerate = (SAMPLE_T)app_jack_return_samplerate(app_data->trk_jack);
    //create ports for trk_jack
    app_data->main_in_L = app_jack_create_port_on_client(app_data->trk_jack, PORT_TYPE_AUDIO, PORT_FLOW_INPUT, "master_in_L");
    app_data->main_in_R = app_jack_create_port_on_client(app_data->trk_jack, PORT_TYPE_AUDIO, PORT_FLOW_INPUT, "master_in_R");
    app_data->main_out_L = app_jack_create_port_on_client(app_data->trk_jack, PORT_TYPE_AUDIO, PORT_FLOW_OUTPUT, "master_out_L");
    app_data->main_out_R = app_jack_create_port_on_client(app_data->trk_jack, PORT_TYPE_AUDIO, PORT_FLOW_OUTPUT, "master_out_R");
    //now activate the jack client, it will launch the rt thread (trk_audio_process_rt function)
    //but app_data->is_processing == 0, so the contexts will not be processed, only app_data sys messages (to start the processes for example)
    if(app_jack_activate(app_data->trk_jack) != 0){
	clean_memory(app_data);
	return NULL;	
    }
    /*initiate the sampler it will be empty initialy*/
    /*-----------------------------------------------*/
    smp_status_t smp_status_err = 0;
    app_data->smp_data = smp_init(buffer_size, samplerate, &smp_status_err, app_data->trk_jack);
    if(!app_data->smp_data){
        //clean app_data
        clean_memory(app_data);
        return NULL;
    } 
    /*--------------------------------------------------*/
    //Init the plugin data object, it will not run any plugins yet
    plug_status_t plug_errors = 0;
    app_data->plug_data = plug_init(buffer_size, samplerate, &plug_errors, app_data->trk_jack);
    if(!app_data->plug_data){
	clean_memory(app_data);
	return NULL;
    }
    
    clap_plug_status_t clap_plug_errors = 0;
    app_data->clap_plug_data = clap_plug_init(buffer_size, buffer_size, samplerate, &clap_plug_errors, app_data->trk_jack);
    if(!(app_data->clap_plug_data)){
	clean_memory(app_data);
	return NULL;
    }
    
    //initiate the Synth data
    app_data->synth_data = synth_init((unsigned int)buffer_size, samplerate, "Synth", 1, app_data->trk_jack);
    if(!app_data->synth_data){
	clean_memory(app_data);
	return NULL;
    }
    //now unpause the jack function again
    context_sub_wait_for_start(app_data->control_data, (void*)app_data);
    *user_data_type = USER_DATA_T_ROOT;
    return (void*)app_data;
}

void* app_data_child_return(void* parent_data, uint16_t parent_type, uint16_t* return_type, unsigned int idx){
    if(!parent_data)return NULL;
    if(parent_type == USER_DATA_T_ROOT){
	APP_INFO* app_data = (APP_INFO*)parent_data;
	switch(idx){
	case 0:
	    *return_type = USER_DATA_T_SAMPLER;
	    return (void*)app_data;
	case 1:
	    *return_type = USER_DATA_T_PLUGINS;
	    return (void*)app_data;
	case 2:
	    *return_type = USER_DATA_T_SYNTH;
	    return (void*)app_data;
	case 3:
	    *return_type = USER_DATA_T_JACK;
	    return (void*)app_data;
	}
    }

    return NULL;
}

const char* app_data_short_name_get(void* user_data, uint16_t user_data_type){
    if(user_data_type == USER_DATA_T_ROOT){
	return APP_NAME;
    }
    if(user_data_type == USER_DATA_T_PLUGINS){
	return PLUGINS_NAME;
    }
    if(user_data_type == USER_DATA_T_PLUG_LV2){
    }
    if(user_data_type == USER_DATA_T_PLUG_CLAP){
    }
    if(user_data_type == USER_DATA_T_SAMPLER){
	return SAMPLER_NAME;
    }
    if(user_data_type == USER_DATA_T_SAMPLE){
    }
    if(user_data_type == USER_DATA_T_SYNTH){
	return SYNTH_NAME;
    }
    if(user_data_type == USER_DATA_T_OSC){
    }
    if(user_data_type == USER_DATA_T_JACK){
	return TRK_NAME;
    }
    
    return NULL;
}

char** app_plug_get_plugin_names(APP_INFO* app_data, unsigned int* names_size, unsigned char** return_plug_types){
    if(!app_data)return NULL;
    unsigned int lv2_size = 0;
    char** lv2_names = plug_return_plugin_names(app_data->plug_data, &lv2_size);
    unsigned int clap_size = 0;
    char** clap_names = clap_plug_return_plugin_names(app_data->clap_plug_data, &clap_size);
    unsigned char* plugin_types = malloc(sizeof(unsigned char) * (lv2_size + clap_size));
    memset(plugin_types, '\0', sizeof(unsigned char) * (lv2_size + clap_size));
    
    if(!plugin_types)goto fail_clean;
    char** all_names = malloc(sizeof(char*));
    if(!all_names)goto fail_clean;
    unsigned int total_names_size = 0;
    //add lv2 and clap names to all_names
    for(int i = 0; i < (lv2_size + clap_size); i++){
	char** names = lv2_names;
	int real_iter = i;
	if(i >= lv2_size){
	    names = clap_names;
	    real_iter = i - lv2_size;
	}
	if(!names)continue;
	char* cur_name = names[real_iter];
	if(!cur_name)continue;
	char** temp_names = realloc(all_names, sizeof(char*)*(total_names_size + 1));
	if(!temp_names){
	    free(cur_name);
	    continue;
	}
	all_names = temp_names;
	all_names[total_names_size] = cur_name;
	plugin_types[total_names_size] = LV2_plugin_type;
	if(names == clap_names)plugin_types[total_names_size] = CLAP_plugin_type;
	total_names_size += 1;
    }

    if(total_names_size == 0){
	goto fail_clean;
    }

    if(lv2_names)free(lv2_names);
    if(clap_names)free(clap_names);
    *names_size = total_names_size;
    *return_plug_types = plugin_types;
    return all_names;
    
fail_clean:
    for(int i = 0; i < (lv2_size + clap_size); i++){
	char** names = lv2_names;
	int real_iter = i;
	if(i >= lv2_size){
	    names = clap_names;
	    real_iter = i - lv2_size;
	}
	if(!names)continue;
	char* cur_name = names[real_iter];
	if(!cur_name)continue;
	free(cur_name);
    }
    if(all_names)free(all_names);
    if(plugin_types)free(plugin_types);
    if(lv2_names)free(lv2_names);
    if(clap_names)free(clap_names);
    return NULL;
}
void* app_plug_presets_iterate(APP_INFO* app_data, unsigned char cx_type, unsigned int idx, uint32_t iter){
    if(!app_data)return NULL;
    if(cx_type == Context_type_Plugins)
	return plug_plugin_presets_iterate(app_data->plug_data, idx, iter);
    if(cx_type == Context_type_Clap_Plugins){
	return clap_plug_presets_iterate(app_data->clap_plug_data, idx, iter);
    }
    return NULL;
}

int app_plug_plugin_presets_get_short_name(APP_INFO* app_data, unsigned char cx_type, void* preset_info, char* return_name, uint32_t name_len){
    if(!app_data)return -1;
    if(cx_type == Context_type_Plugins)
	return plug_plugin_preset_short_name(app_data->plug_data, preset_info, return_name, name_len);
    if(cx_type == Context_type_Clap_Plugins){
	return clap_plug_presets_name_return(app_data->clap_plug_data, preset_info, return_name, name_len);
    }

    return -1;
}
int app_plug_plugin_presets_get_full_path(APP_INFO* app_data, unsigned char cx_type, void* preset_info, char* return_path, uint32_t path_len){
    if(!app_data)return -1;
    if(cx_type == Context_type_Plugins)
	return plug_plugin_preset_path(app_data->plug_data, preset_info, return_path, path_len);
    if(cx_type == Context_type_Clap_Plugins){
	return clap_plug_presets_path_return(app_data->clap_plug_data, preset_info, return_path, path_len);
    }

    return -1;
}
int app_plug_plugin_presets_categories_iterate(APP_INFO* app_data, unsigned char cx_type, void* preset_info, char* cur_category, uint32_t cat_len, uint32_t iter){
    if(!app_data)return -1;
    if(cx_type == Context_type_Plugins)
	//TODO for now lv2 presets do not have categories
	return -1;
    if(cx_type == Context_type_Clap_Plugins){
	return clap_plug_presets_categories_iterate(app_data->clap_plug_data, preset_info, cur_category, cat_len, iter);
    }

    return -1;
}
void app_plug_presets_clean(APP_INFO* app_data, unsigned char cx_type, void* preset_info){
    if(!app_data)return;
    if(cx_type == Context_type_Plugins){
	plug_plugin_preset_clean(app_data->plug_data, preset_info);
	return;
    }
    if(cx_type == Context_type_Clap_Plugins){
	clap_plug_presets_clean_preset(app_data->clap_plug_data, preset_info);
	return;
    }
}
int app_plug_load_preset(APP_INFO* app_data, const char* preset_uri, unsigned int cx_type, const int plug_id){
    if(!app_data)return -1;
    if(!preset_uri)return -1;
    if(plug_id<0) return -1;
    int return_val = -1;
    if(cx_type == Context_type_Plugins)
	return_val = plug_load_preset(app_data->plug_data, plug_id, preset_uri);
    if(cx_type == Context_type_Clap_Plugins)
	return_val = clap_plug_preset_load_from_path(app_data->clap_plug_data, plug_id, preset_uri);
    
    return return_val;
}

int app_plug_init_plugin(APP_INFO* app_data, const char* plugin_uri, unsigned char cx_type, const int id){
    if(!plugin_uri)return -1;
    if(cx_type == Context_type_Plugins)
	return plug_load_and_activate(app_data->plug_data, plugin_uri, id);
    if(cx_type == Context_type_Clap_Plugins)
	return clap_plug_load_and_activate(app_data->clap_plug_data, plugin_uri, id);
    return -1;
}

int app_smp_sample_init(APP_INFO* app_data, const char* samp_path, int in_id){
    if(!samp_path)return -1;
    if(!app_data)return -1;
    int return_id = -1;
    return_id = smp_add(app_data->smp_data, samp_path, in_id);
    return return_id;
}

static PRM_CONTAIN* app_get_context_param_container(APP_INFO* app_data, unsigned char cx_type, int cx_id){
    if(!app_data)return NULL;
    if(cx_type == Context_type_Trk){
	return app_jack_param_return_param_container(app_data->trk_jack);
    }    
    if(cx_type == Context_type_Sampler){
	return smp_param_return_param_container(app_data->smp_data, cx_id);
    }
    if(cx_type == Context_type_Synth){
	return synth_param_return_param_container(app_data->synth_data, cx_id);
    }
    if(cx_type == Context_type_Plugins){
	return plug_param_return_param_container(app_data->plug_data, cx_id);
    }
    if(cx_type == Context_type_Clap_Plugins){
	return clap_plug_param_return_param_container(app_data->clap_plug_data, cx_id);
    }
    return NULL;
}

int app_param_set_value(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, PARAM_T param_value, unsigned char param_op){
    if(!app_data)return -1;
    PRM_CONTAIN* param_cont = app_get_context_param_container(app_data, cx_type, cx_id);
    if(!param_cont)return -1;
    return param_set_value(param_cont, param_id, param_value, NULL, param_op, 0);
}

PARAM_T app_param_get_increment(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id){
    if(!app_data)return -1;
    PRM_CONTAIN* param_cont = app_get_context_param_container(app_data, cx_type, cx_id);
    if(!param_cont)return -1;
    return param_get_increment(param_cont, param_id, 0);
}

int app_param_get_ui_name(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, char* name, uint32_t name_len){
    if(!app_data)return -1;
    PRM_CONTAIN* param_cont = app_get_context_param_container(app_data, cx_type, cx_id);
    if(!param_cont)return -1;
    return param_get_ui_name(param_cont, param_id, name, name_len);
}

PARAM_T app_param_get_value(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id){
    if(!app_data)return -1;
    PRM_CONTAIN* param_cont = app_get_context_param_container(app_data, cx_type, cx_id);
    if(!param_cont)return -1;
    return param_get_value(param_cont, param_id, 0, 0, 0);
}

int app_param_id_from_name(APP_INFO* app_data, unsigned char cx_type, int cx_id, const char* param_name){
    if(!app_data)return -1;
    PRM_CONTAIN* param_cont = app_get_context_param_container(app_data, cx_type, cx_id);
    if(!param_cont)return -1;
    return param_find_name(param_cont, param_name, 0);
}

unsigned int app_param_get_value_as_string(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, char* ret_string, uint32_t string_len){
    if(!app_data)return 0;
    PRM_CONTAIN* param_cont = app_get_context_param_container(app_data, cx_type, cx_id);
    if(!param_cont)return 0;
    return param_get_value_as_string(param_cont, param_id, ret_string, string_len);
}

int app_param_return_all_as_string(APP_INFO* app_data, unsigned char cx_type, int cx_id, char*** param_names, char*** param_vals, unsigned int* param_num){
    if(!app_data)return -1;
    *param_num = 0;
    PRM_CONTAIN* param_cont = app_get_context_param_container(app_data, cx_type, cx_id);
    if(!param_cont)return -1;
    *param_num = param_return_num_params(param_cont, 0);
    
    if(*param_num<=0)return -1;
    char** this_names = (char**)malloc(sizeof(char*) * (*param_num));
    if(!this_names)return -1;
    char** this_vals = (char**)malloc(sizeof(char*) * (*param_num));
    if(!this_vals)return -1;
    for(int i=0; i < *param_num; i++){
	this_names[i] = NULL;
	this_vals[i] = NULL;
	
	char cur_name[MAX_PARAM_NAME_LENGTH];
	if(param_get_name(param_cont, i, cur_name, MAX_PARAM_NAME_LENGTH) != 1)continue;
	
	char* param_name = (char*)malloc(sizeof(char) * (strlen(cur_name)+1));
	if(!param_name)continue;
	strcpy(param_name, cur_name);
	this_names[i] = param_name;
	
        float cur_val = -1;
	cur_val = app_param_get_value(app_data, cx_type, cx_id, i);
	
	int name_len = 0;
	name_len = snprintf(NULL, 0, "%f", cur_val);
	if(name_len<=0)continue;
	char* ret_val = (char*)malloc(sizeof(char)*(name_len+1));
	if(ret_val){
	    snprintf(ret_val, name_len+1, "%f", cur_val);
	    this_vals[i] = ret_val;
	}
    }
    *param_names = this_names;
    *param_vals = this_vals;
    return 0;
}

static void app_make_name_from_file_cx_id(APP_INFO* app_data, char** in_path, int cx_id, unsigned int add_id){
    if(!*in_path)return;
    char* file_name = str_return_file_from_path(*in_path);
    if(add_id)
	str_combine_str_int(&file_name, cx_id);
    if(file_name){
	if(*in_path)free(*in_path);
	*in_path = file_name;
    }
}
const char** app_context_return_names(APP_INFO* app_data, unsigned char cx_type, int* cx_num){
    if(!app_data)return NULL;

    const char** ret_name_array = NULL;
    
    if(cx_type == Context_type_Synth){
	int osc_num = synth_return_osc_num(app_data->synth_data);
	if(osc_num<=0)return NULL;
	ret_name_array = malloc(sizeof(char*) * osc_num);
	for(int i = 0; i < osc_num; i++){
	    ret_name_array[i] = synth_return_osc_name(app_data->synth_data, i);
	}
	*cx_num = osc_num;
	return ret_name_array;
    }

    return ret_name_array;
}

char* app_return_cx_name(APP_INFO* app_data, unsigned char cx_type, int cx_id, unsigned int add_id){
    char* ret_name = NULL;

    //if this is the sampler, return the sample file path and make the name from that
    if(cx_type == Context_type_Sampler){
	ret_name = smp_get_sample_file_path(app_data->smp_data, cx_id);
	app_make_name_from_file_cx_id(app_data, &ret_name, cx_id, add_id);
    }

    //if this is the plugin context, return the plugin name from the plugin lilv name
    if(cx_type == Context_type_Plugins){
	ret_name = plug_return_plugin_name(app_data->plug_data, cx_id);
	app_make_name_from_file_cx_id(app_data, &ret_name, cx_id, add_id);
    }
    //return the name of the clap plugin
    if(cx_type == Context_type_Clap_Plugins){
	ret_name = clap_plug_return_plugin_name(app_data->clap_plug_data, cx_id);
	app_make_name_from_file_cx_id(app_data, &ret_name, cx_id, add_id);
    }
    return ret_name;
}

int app_is_port_on_client(APP_INFO* app_data, const char* port_name){
    if(!app_data)return -1;
    int is_port = 1;
    is_port = app_jack_is_port(app_data->trk_jack, port_name);     
    return is_port;
}

int app_disconnect_ports(APP_INFO* app_data, const char* source_port, const char* dest_port){
    if(!app_data)return -1;
    if(!app_data->trk_jack)return -1;
    return app_jack_disconnect_ports(app_data->trk_jack, source_port, dest_port);
}

int app_connect_ports(APP_INFO* app_data, const char* source_port, const char* dest_port){
    if(!app_data)return -1;
    if(!app_data->trk_jack)return -1;
    return app_jack_connect_ports(app_data->trk_jack, source_port, dest_port);
}

const char** app_return_ports(APP_INFO* app_data, const char* name_pattern, unsigned int type_pattern,
			      unsigned long flags){
    return app_jack_port_names(app_data->trk_jack, name_pattern, type_pattern, flags);
}


char* app_return_short_port(APP_INFO* app_data, const char* full_port_name){
    if(!app_data)return NULL;
    char* ret_name = NULL;
    char* temp = str_split_string_delim(full_port_name, APP_NAME, &ret_name);
    if(temp)free(temp);
    return ret_name;
}

//read ring buffers sent from ui to rt thread - this is not for parameter values, but for messages like start processing a plugin and similar
static int app_read_rt_messages(APP_INFO* app_data){
    is_audio_thread = true;
    if(!app_data)return -1;
    //first read the app_data messages
    context_sub_process_rt(app_data->control_data);
    //if the process is stopped dont process the contexts
    if(app_data->is_processing == 0)return 1;
    
    //read the jack inner messages on the [audio-thread]
    app_jack_read_ui_to_rt_messages(app_data->trk_jack);
    //read the CLAP plugins inner messages on the [audio-thread]
    if(clap_read_ui_to_rt_messages(app_data->clap_plug_data) != 0)return -1;
    //read the lv2 plugin messages on the [audio-thread]
    if(plug_read_ui_to_rt_messages(app_data->plug_data) != 0)return -1;
    //read the sampler messages on the [audio-thread]
    if(smp_read_ui_to_rt_messages(app_data->smp_data) != 0)return -1;
    //read the synth messages on the [audio-thread]
    if(synth_read_ui_to_rt_messages(app_data->synth_data) != 0)return -1;
    return 0;
}

int trk_audio_process_rt(NFRAMES_T nframes, void *arg){   
    //get the app data
    APP_INFO *app_data = (APP_INFO*)arg;
    if(app_data==NULL){
	return -1;
    }
    //process messages from ui to rt thread, like start processing a plugin, stop_processing a plugin, update rt param values etc.
    //if returns a 1 value, this means that is_processing is 0 and the function should not process any contexts
    //a value of -1 means that a fundamental error occured
    int read_err = app_read_rt_messages(app_data);
    if(read_err == 1)return 0;
    if(read_err == -1)return -1;
    
    //process the SAMPLER DATA
    smp_sample_process_rt(app_data->smp_data, nframes);    

    //process the PLUGIN DATA
    plug_process_data_rt(app_data->plug_data, nframes);

    //process the CLAP PLUGIN DATA
    clap_process_data_rt(app_data->clap_plug_data, nframes);
    
    //process the SYNTH DATA
    synth_process_rt(app_data->synth_data, nframes);
    
    //get the buffers for the trk_data, that is used as track summer
    SAMPLE_T *trk_in_L = app_jack_get_buffer_rt(app_data->main_in_L, nframes);
    SAMPLE_T *trk_in_R = app_jack_get_buffer_rt(app_data->main_in_R, nframes);    
    SAMPLE_T *trk_out_L = app_jack_get_buffer_rt(app_data->main_out_L, nframes);
    SAMPLE_T *trk_out_R = app_jack_get_buffer_rt(app_data->main_out_R, nframes);    
    //copy the Master track in  - to the Master track out
    if(!trk_in_L || !trk_in_R || !trk_out_L || !trk_out_R){
	return -1;
    }
    
    memcpy(trk_out_L, trk_in_L, sizeof(SAMPLE_T)*nframes);
    memcpy(trk_out_R, trk_in_R, sizeof(SAMPLE_T)*nframes);

    return 0;
}

void app_data_update(void* user_data, uint16_t user_data_type){
    if(user_data_type != USER_DATA_T_ROOT)return;
    if(!user_data)return;
    APP_INFO* app_data = (APP_INFO*)user_data;
    //read app_data messages from [audio-thread] on the [main-thread]
    context_sub_process_ui(app_data->control_data);
    //read messages for jack from rt thread on [main-thread]
    app_jack_read_rt_to_ui_messages(app_data->trk_jack);
    //read messages from rt thread on [main-thread] for CLAP plugins
    clap_read_rt_to_ui_messages(app_data->clap_plug_data);
    //read messages from the rt thread on the [main-thread] for lv2 plugins
    plug_read_rt_to_ui_messages(app_data->plug_data);
    //read messages from the rt thread on the [main-thread] for sampler
    smp_read_rt_to_ui_messages(app_data->smp_data);
    //read messages from the rt thread on the [main-thread] for the synth context
    synth_read_rt_to_ui_messages(app_data->synth_data);
}

int app_subcontext_remove(APP_INFO* app_data, unsigned char cx_type, int id){
    if(!app_data)return -1;
    if(cx_type == Context_type_Clap_Plugins){
	clap_plug_plug_stop_and_clean(app_data->clap_plug_data, id);
    }
    if(cx_type == Context_type_Plugins){
	plug_stop_and_remove_plug(app_data->plug_data, id);
    }
    if(cx_type == Context_type_Sampler){
	smp_stop_and_remove_sample(app_data->smp_data, id);
    }
    return 0;
}

void app_stop_and_clean(void* user_data, uint16_t type){
    if(type != USER_DATA_T_ROOT)return;
    if(!user_data)return;
    APP_INFO* app_data = (APP_INFO*)user_data;
    context_sub_wait_for_stop(app_data->control_data, user_data);
    
    clean_memory(app_data);
}
