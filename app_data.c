#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "contexts/params.h"
//my libraries
#include "app_data.h"
//string functions
#include "util_funcs/string_funcs.h"
//math helper functions
#include "util_funcs/math_funcs.h"
//funcs for ring buffer manipulation
#include "util_funcs/ring_buffer.h"
//jack init functions
#include "jack_funcs/jack_funcs.h"
#include "util_funcs/log_funcs.h"
//the size of the ring buffer arrays for ui to rt and rt to ui communication
#define MAX_RING_BUFFER_ARRAY_SIZE 2048
//how many rt cycles should pass before the rt thread sends info to the ui thread, so it does not fill the
//ring buffer too fast
#define RT_TO_UI_TICK 25

//this is the global pause, to pause the rt process completely
atomic_int pause;
//the client name that will be shown in the audio client and added next to the port names
const char* client_name = "smp_grvbox";
//single bit of data in the ring buffers
//it stores few ints like value and similar
typedef struct _app_ring_data_bit{
    //the parameter container address for the parameter (where to set the parameter)
    //this should be obtained depending on the cx_type and put in here (for example
    //trk_params if the trk parameter should be set)
    PRM_CONTAIN* param_container;
    //the id of the object in the contex, a sample, track or plugin id or similar.
    int cx_id;
    //the parameter id of the object.
    int param_id;
    //the parameter value to what to set the parameter or what the parameter value is now
    float param_value;
    //what to do with parameter? check paramOperType in types.h for options
    unsigned char param_op;
    //value type of the parameter, usually used for display purposes, so we know how to correctly show
    //the param value for the user. Check the appReturnType
    unsigned char value_type;
}RING_DATA_BIT;

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
    
    //hold here if we launched the jack client process
    unsigned int trk_process_launched;

    //ring buffer for ui to real time thread communication
    RING_BUFFER* ui_to_rt_ring;
    //ring buffer for real time thread to ui communication
    RING_BUFFER* rt_to_ui_ring;
    //rt thread cycles, they go up to RT_TO_UI_TICK and then restarts
    unsigned int rt_cycle;
     //ports for trk_jack
    void* main_in_L;
    void* main_in_R;
    void* main_out_L;
    void* main_out_R;
}APP_INFO;

//internal function to wait on a atomic pause
static void app_wait_for_pause(atomic_int* atomic_pause){
    int expected = 0;
    if(atomic_compare_exchange_weak(atomic_pause, &expected, 1)){
	expected = 2;
	while(!atomic_compare_exchange_weak(atomic_pause, &expected, 2)){
	    expected = 2;
	}
    }        
}

APP_INFO* app_init(app_status_t *app_status){
    APP_INFO *app_data = (APP_INFO*) malloc(sizeof(APP_INFO));
    if(!app_data){
        *app_status = app_failed_malloc;
        return NULL;
    }
    //init the members to NULLS
    app_data->smp_data = NULL;
    app_data->trk_jack = NULL;
    app_data->plug_data = NULL;
    app_data->clap_plug_data = NULL;
    app_data->synth_data = NULL;
    app_data->trk_process_launched = 0;
    app_data->rt_cycle = 0;
    atomic_store(&pause,0);

    //initiate the ring buffers
    app_data->ui_to_rt_ring = ring_buffer_init(sizeof(RING_DATA_BIT), MAX_RING_BUFFER_ARRAY_SIZE);
    if(!app_data->ui_to_rt_ring){
	*app_status = app_failed_malloc;
	return NULL;
    }
    app_data->rt_to_ui_ring = ring_buffer_init(sizeof(RING_DATA_BIT), MAX_RING_BUFFER_ARRAY_SIZE);
    if(!app_data->rt_to_ui_ring){
	*app_status = app_failed_malloc;
	ring_buffer_clean(app_data->ui_to_rt_ring);
	return NULL;
    }
    
    /*init jack client for the whole program*/
    /*--------------------------------------------------*/   
    JACK_INFO *trk_jack = NULL;
    trk_jack = jack_initialize(app_data, client_name, 0, 0, 0, NULL,
			       trk_audio_process_rt, 0);
    if(!trk_jack){
	clean_memory(app_data);
	*app_status = trk_jack_init_failed;
	return NULL;
    }
    app_data->trk_jack = trk_jack;
    //create ports for trk_jack
    app_data->main_in_L = app_jack_create_port_on_client(app_data->trk_jack, 0, 1, "master_in_L");
    app_data->main_in_R = app_jack_create_port_on_client(app_data->trk_jack, 0, 1, "master_in_R");
    app_data->main_out_L = app_jack_create_port_on_client(app_data->trk_jack, 0, 2, "master_out_L");
    app_data->main_out_R = app_jack_create_port_on_client(app_data->trk_jack, 0, 2, "master_out_R");

    uint32_t buffer_size = (uint32_t)app_jack_return_buffer_size(app_data->trk_jack);
    SAMPLE_T samplerate = (SAMPLE_T)app_jack_return_samplerate(app_data->trk_jack);
    /*initiate the sampler it will be empty initialy*/
    /*-----------------------------------------------*/
    smp_status_t smp_status_err = 0;
    SMP_INFO *smp_data = NULL;
    smp_data = smp_init(buffer_size, samplerate, &smp_status_err, trk_jack);
    if(!smp_data){
        //clean app_data
        clean_memory(app_data);
        *app_status = smp_data_init_failed;
        return NULL;
    } 
    app_data->smp_data = smp_data;    

    /*--------------------------------------------------*/
    //Init the plugin data object, it will not run any plugins yet
    plug_status_t plug_errors = 0;
    app_data->plug_data = plug_init(buffer_size, samplerate, &plug_errors, trk_jack);
    if(!app_data->plug_data){
	clean_memory(app_data);
	*app_status = plug_data_init_failed;
	return NULL;
    }
    clap_plug_status_t clap_plug_errors = 0;
    app_data->clap_plug_data = clap_plug_init(buffer_size, buffer_size, samplerate, &clap_plug_errors, trk_jack);
    if(!(app_data->clap_plug_data)){
	clean_memory(app_data);
	*app_status = clap_plug_data_init_failed;
	return NULL;
    }
    //initiate the Synth data
    app_data->synth_data = synth_init((unsigned int)buffer_size, samplerate, "Synth", 1, app_data->trk_jack);
    if(!app_data->synth_data){
	clean_memory(app_data);
	*app_status = synth_data_init_failed;
	return NULL;
    }

    //now activate the jack client, it will launch the rt thread (trk_audio_process_rt function)
    if(app_jack_activate(app_data->trk_jack)!=0){
	clean_memory(app_data);
	*app_status = trk_jack_init_failed;
	return NULL;	
    }
    app_data->trk_process_launched = 1;

    return app_data;
}

char** app_plug_get_plugin_names(APP_INFO* app_data, unsigned int* names_size, unsigned char** return_plug_types){
    if(!app_data)return NULL;
    unsigned int lv2_size = 0;
    char** lv2_names = plug_return_plugin_names(app_data->plug_data, &lv2_size);
    unsigned int clap_size = 0;
    char** clap_names = clap_plug_return_plugin_names(&clap_size);
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

char** app_plug_get_plugin_presets(APP_INFO* app_data, unsigned int indx, unsigned int* total_presets){
    if(!app_data)return NULL;
    return plug_return_plugin_presets_names(app_data->plug_data, indx, total_presets);
}

int app_plug_init_plugin(APP_INFO* app_data, const char* plugin_uri, unsigned char cx_type, const int id){
    if(!plugin_uri)return -1;
    //before adding the plugin request the rt process for the plug_data to pause for a while
    app_wait_for_pause(&pause);
    int return_id = -1;
    if(cx_type == Context_type_Plugins)
	return_id = plug_load_and_activate(app_data->plug_data, plugin_uri, id);
    if(cx_type == Context_type_Clap_Plugins)
	return_id = clap_plug_load_and_activate(app_data->clap_plug_data, plugin_uri, id);
    atomic_store(&pause, 0);    
    return return_id;
}

int app_plug_load_preset(APP_INFO* app_data, const char* preset_uri, unsigned int cx_type, const int plug_id){
    if(!app_data)return -1;
    if(!preset_uri)return -1;
    if(plug_id<0) return -1;
    //before loading the preset pause the plug process in the realtime function
    //TODO if plug has safe_restore we can skip the pause and load the preset, but for that we would need
    //a function in plugins to check if plugin has safe_restore and when loading a preset in such a way
    //we would need to set the control ports values with circle buffers
    app_wait_for_pause(&pause);
    int return_val = -1;
    if(cx_type == Context_type_Plugins)
	return_val = plug_load_preset(app_data->plug_data, plug_id, preset_uri);
    //TODO load preset for the CLAP plugin
    if(cx_type == Context_type_Clap_Plugins)
	return_val = -1;
    atomic_store(&pause, 0);
    
    return return_val;
}

int app_smp_sample_init(APP_INFO* app_data, const char* samp_path, int in_id){
    if(!samp_path)return -1;
    if(!app_data)return -1;
    int return_id = -1;
    app_wait_for_pause(&pause);
    return_id = smp_add(app_data->smp_data, samp_path, in_id);
    atomic_store(&pause, 0);
    return return_id;
}

static PRM_CONTAIN* app_return_param_container(APP_INFO* app_data, unsigned char cx_type, int cx_id){
    PRM_CONTAIN* return_container = NULL;

    if(!app_data)return NULL;
    if(cx_type == Context_type_Sampler){
	return_container = smp_get_sample_param_container(app_data->smp_data, cx_id);
    }
    //if this is general trk settings
    if(cx_type == Context_type_Trk){
	return_container = app_jack_return_param_container(app_data->trk_jack);
    }
    //if this is the synth
    if(cx_type == Context_type_Synth){
	return_container = synth_return_param_container(app_data->synth_data, cx_id);
    }
    if(cx_type == Context_type_Plugins){
	return_container = plug_return_param_container(app_data->plug_data, cx_id);
    }
    return return_container;
}

int app_param_set_value(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, float param_value,
			unsigned char param_op, unsigned int rt_to_ui){
    //first get the parameter container and set the parameter for the current thread directly
    //so set rt param if rt_to_ui==1 or set ui param if rt_to_ui==0
    PRM_CONTAIN* param_container = app_return_param_container(app_data, cx_type, cx_id);
    if(!param_container)return -1;
    param_set_value(param_container, param_id, param_value, param_op, rt_to_ui);
    
    RING_DATA_BIT send_bit;
    send_bit.param_container = param_container;
    send_bit.cx_id = cx_id;
    send_bit.param_id = param_id;
    send_bit.param_value = param_value;
    send_bit.param_op = param_op;
    send_bit.value_type = Float_type;
    RING_BUFFER* ring_buffer = app_data->ui_to_rt_ring;
    if(rt_to_ui == RT_TO_UI_RING_E)ring_buffer = app_data->rt_to_ui_ring;
    int ret = ring_buffer_write(ring_buffer, &send_bit, sizeof(send_bit));
    //for testing how many items are in the ring_buffer
    /*
    if(rt_to_ui==0)
	log_append_logfile("ringbuffer size %d after sending %s\n", ring_buffer_return_items(ring_buffer),param_get_name(param_container, param_id, rt_to_ui));
    */
    return ret;
}

SAMPLE_T app_param_get_increment(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id, unsigned int rt_param){
    if(!app_data)return -1;
    //get the appropriate parameter container depending on the context
    PRM_CONTAIN* param_container = app_return_param_container(app_data, cx_type, cx_id);
    if(!param_container)return -1;

    return param_get_increment(param_container, param_id, rt_param);
}

SAMPLE_T app_param_get_value(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id,
			     unsigned char* val_type, unsigned int curved, unsigned int rt_param){
    if(!app_data)return -1;
    //get the appropriate parameter container depending on the context
    PRM_CONTAIN* param_container = app_return_param_container(app_data, cx_type, cx_id);
    if(!param_container)return -1;

    return param_get_value(param_container, param_id, val_type, curved, 0, rt_param);
}

int app_param_id_from_name(APP_INFO* app_data, unsigned char cx_type, int cx_id, const char* param_name, unsigned int rt_param){
    if(!app_data)return -1;
    PRM_CONTAIN* param_container = app_return_param_container(app_data, cx_type, cx_id);
    if(!param_container)return -1;
    return param_find_name(param_container, param_name, rt_param);
}

const char* app_param_get_string(APP_INFO* app_data, unsigned char cx_type, int cx_id, int param_id,
			      unsigned int rt_param){
    if(!app_data)return NULL;
    //get the appropriate parameter container depending on the context
    PRM_CONTAIN* param_container = app_return_param_container(app_data, cx_type, cx_id);
    if(!param_container)return NULL;

    return param_get_param_string(param_container, param_id, rt_param);
}

int app_param_return_all_as_string(APP_INFO* app_data, unsigned char cx_type, int cx_id, char*** param_names, char*** param_vals,
			 char*** param_types, unsigned int* param_num){
    if(!app_data)return -1;
    //get the parameter container depending on the context
    PRM_CONTAIN* param_container = app_return_param_container(app_data, cx_type, cx_id);
    if(!param_container)return -1;
    *param_num = 0;
    *param_num = param_return_num_params(param_container);
    
    if(*param_num<=0)return -1;
    char** this_names = (char**)malloc(sizeof(char*) * (*param_num));
    if(!this_names)return -1;
    char** this_vals = (char**)malloc(sizeof(char*) * (*param_num));
    if(!this_vals)return -1;
    char** this_types = (char**)malloc(sizeof(char*) * (*param_num));
    if(!this_types)return -1;
    for(int i=0; i < *param_num; i++){
	this_names[i] = NULL;
	this_vals[i] = NULL;
	this_types[i] = NULL;
	const char* cur_name = NULL;
	cur_name = param_get_name(param_container, i, UI_PARAM_E);
	
	if(!cur_name)continue;
	char* param_name = (char*)malloc(sizeof(char) * (strlen(cur_name)+1));
	if(!param_name)continue;
	strcpy(param_name, cur_name);
	this_names[i] = param_name;
	
	unsigned char cur_type = 0;
        float cur_val = -1;
	cur_val = param_get_value(param_container, i, &cur_type, 0, 0, UI_PARAM_E);

	if(cur_type == 0)continue;
	int name_len = 0;
	name_len = snprintf(NULL, 0, "%f", cur_val);
	if(name_len<=0)continue;
	char* ret_val = (char*)malloc(sizeof(char)*(name_len+1));
	if(ret_val){
	    snprintf(ret_val, name_len+1, "%f", cur_val);
	    this_vals[i] = ret_val;
	}
	name_len = snprintf(NULL, 0, "%d", (unsigned int)cur_type);
	if(name_len<=0)continue;
	char* ret_type = (char*)malloc(sizeof(char)*(name_len+1));
	if(ret_type){
	    snprintf(ret_type, name_len+1, "%d", (unsigned int)cur_type);
	    this_types[i] = ret_type;
	}
    }
    *param_names = this_names;
    *param_vals = this_vals;
    *param_types = this_types;
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
    //TODO return the name of the CLAP plugin
    if(cx_type == Context_type_Clap_Plugins){
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
    char* temp = str_split_string_delim(full_port_name, client_name, &ret_name);
    if(temp)free(temp);
    return ret_name;
}

static const char** app_return_sys_port_names(void** sys_ports, unsigned int port_num){
    if(port_num<=0)return NULL;
    if(!sys_ports)return NULL;
    const char** name_array  = (const char**)malloc(sizeof(char*)*port_num);
    if(!name_array)return NULL;
    for(int i = 0; i<port_num; i++){
	name_array[i] = NULL;
	void* cur_port = sys_ports[i];
	if(cur_port){
	    name_array[i] = app_jack_return_port_name(cur_port);
	}
    }
    return name_array;
}

const char** app_return_context_ports(APP_INFO* app_data, unsigned int* name_num, unsigned int cx_type, unsigned int cx_id){
    unsigned int port_num = 0;
    void** sys_ports = NULL;
    if(cx_type == Context_type_Sampler){
	sys_ports = smp_return_sys_ports(app_data->smp_data, &port_num);
    }
    if(cx_type == Context_type_Plugins){
	sys_ports = plug_return_sys_ports(app_data->plug_data, cx_id, &port_num);
    }
    if(cx_type == Context_type_Synth){
	sys_ports = synth_return_sys_ports(app_data->synth_data, cx_id, &port_num);
    }
    if(port_num <= 0) return NULL;
    if(name_num) *name_num = port_num;
    const char** name_array = app_return_sys_port_names(sys_ports, port_num);
    if(sys_ports)free(sys_ports);
    return name_array;
}

int trk_audio_process_rt(NFRAMES_T nframes, void *arg){
    //get the app data
    APP_INFO *app_data = (APP_INFO*)arg;
    if(app_data==NULL)return 1;
    //Get the trk_jack first, because we need to clear the main ports in case the app shutsdown, to not
    //make any artifacts
    JACK_INFO *trk_jack = app_data->trk_jack;
    if(trk_jack==NULL)return 1;
    //get the buffers for the trk_data, that is used as track summer
    SAMPLE_T *trk_in_L = app_jack_get_buffer_rt(app_data->main_in_L, nframes);
    SAMPLE_T *trk_in_R = app_jack_get_buffer_rt(app_data->main_in_R, nframes);    
    SAMPLE_T *trk_out_L = app_jack_get_buffer_rt(app_data->main_out_L, nframes);
    SAMPLE_T *trk_out_R = app_jack_get_buffer_rt(app_data->main_out_R, nframes);
    
    //if the whole process is paused, clear the main ports
    int expected = 2;
    if(atomic_compare_exchange_weak(&pause, &expected, 2)){
	memset(trk_out_L, '\0', sizeof(SAMPLE_T)*nframes);
	memset(trk_out_R, '\0', sizeof(SAMPLE_T)*nframes);		
	goto finish;
    }
    
    //get the smp_data;
    SMP_INFO* smp_data = app_data->smp_data;
    if(smp_data==NULL)return 1;
    //get the plug_data
    PLUG_INFO* plug_data = app_data->plug_data;
    if(plug_data==NULL)return 1;

    //if there is a request to pause evertyhing in the rt process
    expected = 1;
    if(atomic_compare_exchange_weak(&pause, &expected, 1)){
	memset(trk_out_L, '\0', sizeof(SAMPLE_T)*nframes);
	memset(trk_out_R, '\0', sizeof(SAMPLE_T)*nframes);	
	atomic_store(&pause, 2);
	goto finish;
    }
    //read the ui_to_rt ring buffer and update the appropriate context rt_param arrays
    app_read_ring_buffer(app_data, UI_TO_RT_RING_E);
    //initiate the various transport processes depending on the trk parameters
    //also process the metronome
    app_transport_control_rt(app_data, nframes);
    
    //process the SAMPLER DATA
    smp_sample_process_rt(smp_data, nframes);    

    //process the PLUGIN DATA
    plug_process_data_rt(plug_data, nframes);

    //process the SYNTH DATA
    synth_process_rt(app_data->synth_data, nframes);

    //copy the Master track in  - to the Master track out
    memcpy(trk_out_L, trk_in_L, sizeof(SAMPLE_T)*nframes);
    memcpy(trk_out_R, trk_in_R, sizeof(SAMPLE_T)*nframes);

finish:
    //update the rt cycle
    app_data->rt_cycle += 1;
    if(app_data->rt_cycle > RT_TO_UI_TICK)app_data->rt_cycle = 0;

    return 0;
}

static int app_transport_control_rt(APP_INFO* app_data, NFRAMES_T nframes){
    if(!app_data)return -1;
    //if rt params just got new parameter values from the ui they will be just changed
    //in that case jack will create a new transport object and request a transport change
    app_jack_update_transport_from_params_rt(app_data->trk_jack);

     //now send transport info to the ui thread, we do it only on rt_cycle 0
    //so to not overwhelm the ui thread buffer
    if(app_data->rt_cycle == 0){
	int32_t bar = 1;
	int32_t beat = 1;
	int32_t tick = 0;
	SAMPLE_T ticks_per_beat = 0;
	NFRAMES_T total_frames = 0;
	//bpm, beat_type and beats_per_bar are used from the params so they are not used here
	float bpm = 0;
	float beat_type = 0;
	float beats_per_bar = 0;
	int isPlaying = app_jack_return_transport(app_data->trk_jack, &bar, &beat, &tick, &ticks_per_beat, &total_frames,
						  &bpm, &beat_type, &beats_per_bar);
	if(isPlaying != -1){
	    //we also get the tranport parameter container, so we can look up the value after sending it
	    //to the ring buffer, otherwise just_changed will be 1 again and the new transport object
	    //would be created each time the rt_cycle == 0
	    PRM_CONTAIN* transport_cntr = app_return_param_container(app_data, Context_type_Trk, 0);
	    unsigned char val_type = 0;	    
	    app_param_set_value(app_data, Context_type_Trk, 0, 1, (float)bar, Operation_SetValue, RT_TO_UI_RING_E);
	    param_get_value(transport_cntr, 1, &val_type, 0, 0, RT_PARAM_E);
	    
	    app_param_set_value(app_data, Context_type_Trk, 0, 2, (float)beat, Operation_SetValue, RT_TO_UI_RING_E);
	    param_get_value(transport_cntr, 2, &val_type, 0, 0, RT_PARAM_E);

	    app_param_set_value(app_data, Context_type_Trk, 0, 3, (float)tick, Operation_SetValue, RT_TO_UI_RING_E);
	    param_get_value(transport_cntr, 3, &val_type, 0, 0, RT_PARAM_E);

	    //also set the play value on the ui, just in case the transport started in some external way,
	    //not through the parameter play
	    app_param_set_value(app_data, Context_type_Trk, 0, 4, (float)isPlaying, Operation_SetValue, RT_TO_UI_RING_E);
	    param_get_value(transport_cntr, 4, &val_type, 0, 0, RT_PARAM_E);
	}
    }
}

static int app_read_ring_buffer(APP_INFO* app_data, unsigned int rt_to_ui){
    RING_BUFFER* ring_buffer = NULL;
    if(rt_to_ui == UI_TO_RT_RING_E)ring_buffer = app_data->ui_to_rt_ring;
    if(rt_to_ui == RT_TO_UI_RING_E)ring_buffer = app_data->rt_to_ui_ring;
    if(!ring_buffer)return -1;

    unsigned int rt_params = UI_PARAM_E;
    if(rt_to_ui==UI_TO_RT_RING_E)rt_params = RT_PARAM_E;
    
    unsigned int cur_items = ring_buffer_return_items(ring_buffer);
  
    if(cur_items<=0)return -1;
    for(int i = 0; i<cur_items; i++){

	RING_DATA_BIT cur_bit;
	int read_buffer = ring_buffer_read(ring_buffer, &cur_bit, sizeof(cur_bit));
	if(read_buffer<=0)continue;
	float new_val = -1;
	unsigned char val_type = 0;
	PRM_CONTAIN* param_container = cur_bit.param_container;
	if(!param_container)continue;
	param_set_value(param_container, cur_bit.param_id, cur_bit.param_value, cur_bit.param_op, rt_params);
    }
    return 1;
}

int app_update_ui_params(APP_INFO* app_data){
    int return_val = -1;
    return_val = app_read_ring_buffer(app_data, RT_TO_UI_RING_E);
    return return_val;
}

int app_smp_remove_sample(APP_INFO* app_data, unsigned int idx){
    int return_val = 0;
    if(!app_data)return -1;
    //before removing sample request the rt process to pause
    app_wait_for_pause(&pause);
    return_val = smp_remove_sample(app_data->smp_data, idx);
    atomic_store(&pause, 0);
    return return_val;
}

int app_plug_remove_plug(APP_INFO* app_data, const int id){
    int return_val = 0;
    if(!app_data)return -1;
    //before removing the plugin wait for the rt process to pause
    app_wait_for_pause(&pause);    
    return_val = plug_remove_plug(app_data->plug_data, id);
    atomic_store(&pause, 0);
    return return_val;
}

int clean_memory(APP_INFO *app_data){
    if(!app_data)return -1;
    //if the process was launched we need to pause it before clearing data
    if(app_data->trk_process_launched == 1){
	app_wait_for_pause(&pause);
    }
    //clean the clap plug_data memory
    if(app_data->clap_plug_data)clap_plug_clean_memory(app_data->clap_plug_data);
    //clean the plug_data memory
    if(app_data->plug_data)plug_clean_memory(app_data->plug_data);
    //clean the sampler memory
    if(app_data->smp_data) smp_clean_memory(app_data->smp_data);
    //clean the synth memory
    if(app_data->synth_data) synth_clean_memory(app_data->synth_data);
    
    //clean the track jack memory
    if(app_data->trk_jack)jack_clean_memory(app_data->trk_jack);

    //clean the ring buffers
    ring_buffer_clean(app_data->ui_to_rt_ring);
    ring_buffer_clean(app_data->rt_to_ui_ring);
  
    //clean the app_data
    if(app_data)free(app_data);

    return 0;
}
