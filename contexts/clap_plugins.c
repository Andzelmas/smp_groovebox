#include "clap_plugins.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <threads.h>
#include "../util_funcs/log_funcs.h"
#include <clap/clap.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/string_funcs.h"
#include "../types.h"
#include "params.h"
//what is the size of the buffer to get the formated param values to
#define MAX_VALUE_LEN 64
#define CLAP_PATH "/usr/lib/clap/"
//how many clap plugins can there be in the plugin array
#define MAX_INSTANCES 5
//number of items in the ring arrays
#define MAX_RING_BUFFER_ARRAY_SIZE 256

static thread_local bool is_audio_thread = false; //TODO need to set this to true in the process function of the plugin

//the single clap plugin struct
typedef struct _clap_plug_plug{
    int id; //plugin id on the clap_plug_info plugin array
    clap_plugin_entry_t* plug_entry; //the clap library file for this plugin
    const clap_plugin_t* plug_inst; //the plugin instance
    unsigned int plug_inst_created;
    unsigned int plug_inst_activated;
    int plug_inst_id; //the plugin instance id in the plugin factory
    char* plug_path; //the path for the clap file
    PRM_CONTAIN* plug_params; //plugin parameter container for params.c
    clap_host_t clap_host_info; //need when creating the plugin instance, this struct has this CLAP_PLUG_PLUG in the host var as (void*)
    CLAP_PLUG_INFO* plug_data; //CLAP_PLUG_INFO struct address for convenience
}CLAP_PLUG_PLUG;

//the main clap struct
typedef struct _clap_plug_info{
    struct _clap_plug_plug* plugins[MAX_INSTANCES]; //array with single clap plugins
    unsigned int total_plugs; //how many plugins there are
    int max_id; //the biggest id of a plugin in the system, if its -1 - there are no active plugins
    SAMPLE_T sample_rate;
    //for clap there can be min and max buffer sizes, for not changing buffer sizes set as the same
    uint32_t min_buffer_size;
    uint32_t max_buffer_size;
    //the clap host needed for clap function. It has the address of the CLAP_PLUG_INFO too
    clap_host_t clap_host_info;
    //ring buffer for messages from rt to ui
    RING_BUFFER* rt_to_ui_msgs;
    //ring buffer for messages from callback functions like request_restart that originate from the main thread
    //(for example a messages that a plugin needs a restart so before calling the restart function the rt process is paused)
    RING_BUFFER* ui_to_ui_msgs;
    //from here hold various host extension implementation structs
    clap_host_thread_check_t ext_thread_check; //struct that holds functions to check if the thread is main or audio
}CLAP_PLUG_INFO;

//return if this is audio_thread or not
static bool clap_plug_return_is_audio_thread(){
    return is_audio_thread;
}
//functions to return is this audio or main thread for the clap_host_thread_t extension
static bool clap_plug_ext_is_audio_thread(const clap_host_t* host){
    return clap_plug_return_is_audio_thread();
}
static bool clap_plug_ext_is_main_thread(const clap_host_t* host){
    return !(clap_plug_return_is_audio_thread());
}

const void* get_extension(const clap_host_t* host, const char* ex_id){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return NULL;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return NULL;
    if(strcmp(ex_id, CLAP_EXT_THREAD_CHECK) == 0){
	return &(plug_data->ext_thread_check);
    }
    //if there is no extension implemented that the plugin needs send the name of the extension to the ui
    RING_BUFFER* msg_buffer = plug_data->ui_to_ui_msgs;
    if(clap_plug_return_is_audio_thread())
	msg_buffer = plug_data->rt_to_ui_msgs;

    CLAP_RING_SYS_MSG send_bit;
    snprintf(send_bit.msg, MAX_STRING_MSG_LENGTH, "%s asked for ext %s\n", plug->plug_path, ex_id);
    send_bit.msg_enum = MSG_PLUGIN_SENT_STRING;
    send_bit.plug_id = plug->id;
    int ret = ring_buffer_write(msg_buffer, &send_bit, sizeof(send_bit));
    return NULL;
}

void request_restart(const clap_host_t* host){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    RING_BUFFER* msg_buffer = plug_data->ui_to_ui_msgs;
    if(clap_plug_return_is_audio_thread())
	msg_buffer = plug_data->rt_to_ui_msgs;
    //send to ui a message that the plugin needs a restart
    CLAP_RING_SYS_MSG send_bit;
    send_bit.msg_enum = MSG_PLUGIN_RESTART;
    send_bit.plug_id = plug->id;
    ring_buffer_write(msg_buffer, &send_bit, sizeof(send_bit));    
}
//TODO not tested yet
int clap_plug_restart(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id > plug_data->max_id)return -1;

    CLAP_PLUG_PLUG* plug = plug_data->plugins[plug_id];
    if(!plug)return -1;
    if(plug->plug_inst_activated != 1)return -1;

    //now restart the plugin - deactivate and activate it
    plug->plug_inst->deactivate(plug->plug_inst);
    plug->plug_inst_activated = 0;
    bool active = plug->plug_inst->activate(plug->plug_inst, plug_data->sample_rate, plug_data->min_buffer_size, plug_data->max_buffer_size);
    if(!active)return -1;
    plug->plug_inst_activated = 1;
    return 0;
}

void request_process(const clap_host_t* host){}

void request_callback(const clap_host_t* host){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    RING_BUFFER* msg_buffer = plug_data->ui_to_ui_msgs;
    if(clap_plug_return_is_audio_thread())
	msg_buffer = plug_data->rt_to_ui_msgs;
    //send to ui a message that the plugin needs to call the on_main_thread function 
    CLAP_RING_SYS_MSG send_bit;
    send_bit.msg_enum = MSG_PLUGIN_REQUEST_CALLBACK;
    send_bit.plug_id = plug->id;
    ring_buffer_write(msg_buffer, &send_bit, sizeof(send_bit));    
}
//TODO not tested yet
int clap_plug_callback(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id > plug_data->max_id)return -1;

    CLAP_PLUG_PLUG* plug = plug_data->plugins[plug_id];
    if(!plug)return -1;
    plug->plug_inst->on_main_thread(plug->plug_inst);
    return 0;
}

//return the clap_plug_plug id on the plugins array that has the same plug_entry (initiated) if no plugin has this plug_entry return -1
static int clap_plug_return_plug_id_with_same_plug_entry(CLAP_PLUG_INFO* plug_data, clap_plugin_entry_t* plug_entry){
    int return_id = -1;
    if(!plug_data)return -1;
    if(!plug_entry)return -1;

    for(unsigned int plug_num = 0; plug_num < plug_data->total_plugs; plug_num++){
	CLAP_PLUG_PLUG* cur_plug = plug_data->plugins[plug_num];
	if(!cur_plug)continue;
	if(cur_plug->plug_entry != plug_entry)continue;
	return_id = cur_plug->id;
	break;
    }
    
    return return_id;
}

//clean the single plugin struct
static void clap_plug_plug_clean(CLAP_PLUG_INFO* plug_data, CLAP_PLUG_PLUG* plug){
    if(!plug_data)return;
    if(!plug)return;
    if(plug->plug_inst){
	if(plug->plug_inst_activated == 1)
	    plug->plug_inst->deactivate(plug->plug_inst);
	if(plug->plug_inst_created == 1)
	    plug->plug_inst->destroy(plug->plug_inst);
    }
    if(plug->plug_entry){
	if(clap_plug_return_plug_id_with_same_plug_entry(plug_data, plug->plug_entry) == -1)plug->plug_entry->deinit();
    }
    if(plug->plug_path)free(plug->plug_path);
    if(plug->plug_params)param_clean_param_container(plug->plug_params);
    free(plug);
}

static char** clap_plug_get_plugin_names_from_file(const char* plug_path, unsigned int* num_of_plugins){
    void* handle;
    int* iptr;
    handle = dlopen(plug_path, RTLD_LOCAL | RTLD_LAZY);
    if(!handle){
	log_append_logfile("failed to load %s dso \n", plug_path);
    }
    
    iptr = (int*)dlsym(handle, "clap_entry");
    clap_plugin_entry_t* plug_entry = (clap_plugin_entry_t*)iptr;
    
    unsigned int init_err = plug_entry->init(plug_path);
    if(!init_err){
	log_append_logfile("failed to init the %s plugin entry\n", plug_path);
	return NULL;
    }
    
    const clap_plugin_factory_t* plug_fac = plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    uint32_t plug_count = plug_fac->get_plugin_count(plug_fac);
    if(plug_count <= 0){
	log_append_logfile("no plugins in %s dso \n", plug_path);
	return NULL;
    }
    char** plug_names = malloc(sizeof(char*));
    if(!plug_names)return NULL;
    unsigned int name_count = 0;
    for(uint32_t pl_iter = 0; pl_iter < plug_count; pl_iter++){
	const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, pl_iter);
	if(!plug_desc)continue;
	if(!(plug_desc->name))continue;
	char** temp_names = realloc(plug_names, sizeof(char*) * (name_count+1));
	if(!temp_names)continue;
	plug_names = temp_names;
	plug_names[name_count] = NULL;
	unsigned int cur_name_size = strlen(plug_desc->name) + 1;
	char* cur_name = malloc(sizeof(char) * cur_name_size);
	if(!cur_name)continue;
	snprintf(cur_name, cur_name_size, "%s", plug_desc->name);
	plug_names[name_count] = cur_name;
	name_count += 1;
    }

    if(name_count == 0){
	free(plug_names);
	return NULL;
    }
    plug_entry->deinit();
    
    *num_of_plugins = name_count;
    return plug_names;
}

//get the clap files in the directory
static char** clap_plug_get_clap_files(const char* file_path, unsigned int* size){
    char** ret_files = NULL;
    *size = 0;
    DIR* d;
    struct dirent* dir = NULL;
    d = opendir(file_path);
    if(d){
	ret_files = malloc(sizeof(char*));
	unsigned int iter = 0;
	while((dir = readdir(d)) != NULL){
	    if(strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)continue;
	    char* after_delim = NULL;
	    char* before_delim = str_split_string_delim(dir->d_name, ".", &after_delim);
	    if(!after_delim){
		if(before_delim)free(before_delim);
		continue;
	    }
	    if(strcmp(after_delim, "clap") != 0){
		if(after_delim)free(after_delim);
		if(before_delim)free(before_delim);
		continue;
	    }
	    if(after_delim)free(after_delim);
	    if(before_delim)free(before_delim);
	    char** temp_files = realloc(ret_files, sizeof(char*) * (iter+1));
	    if(!temp_files)continue;
	    ret_files = temp_files;
	    ret_files[iter] = NULL;
	    unsigned int cur_file_len = strlen(dir->d_name) + 1;
	    char* cur_file = malloc(sizeof(char) * cur_file_len);
	    if(!cur_file){
		continue;
	    }
	    snprintf(cur_file, cur_file_len, "%s", dir->d_name);
	    ret_files[iter] = cur_file;
	    iter += 1;
	}
	closedir(d);
	*size = iter;
    }
    return ret_files;
}

RING_BUFFER* clap_get_rt_to_ui_msg_buffer(CLAP_PLUG_INFO* plug_data, unsigned int* items){
    if(!plug_data)return NULL;
    if(!(plug_data->rt_to_ui_msgs))return NULL;
    *items = ring_buffer_return_items(plug_data->rt_to_ui_msgs);
    return plug_data->rt_to_ui_msgs;
}
RING_BUFFER* clap_get_ui_to_ui_msg_buffer(CLAP_PLUG_INFO* plug_data, unsigned int* items){
    if(!plug_data)return NULL;
    if(!(plug_data->ui_to_ui_msgs))return NULL;
    *items = ring_buffer_return_items(plug_data->ui_to_ui_msgs);
    return plug_data->ui_to_ui_msgs;
}
//goes through the clap files stored on the clap_data struct and returns the names of these plugins
//each clap file can have several different plugins inside of it
char** clap_plug_return_plugin_names(unsigned int* size){
    unsigned int total_files = 0;
    char** clap_files = clap_plug_get_clap_files(CLAP_PATH, &total_files);
    if(!clap_files)return NULL;
    char** return_names = malloc(sizeof(char*));
    unsigned int total_names_found = 0;
    for(int i = 0; i < total_files; i++){
	char* cur_file = clap_files[i];
	if(!cur_file)continue;
	unsigned int total_file_name_size = strlen(CLAP_PATH) + strlen(cur_file) + 1;
	char* total_file_path = malloc(sizeof(char) * total_file_name_size);
	if(!total_file_path){
	    free(cur_file);
	    continue;
	}
	snprintf(total_file_path, total_file_name_size, "%s%s", CLAP_PATH, cur_file);

	unsigned int plug_name_count = 0;
	char** plug_names = clap_plug_get_plugin_names_from_file(total_file_path, &plug_name_count);
	if(plug_names){
	    for(int j = 0; j < plug_name_count; j++){
		char* cur_plug_name = plug_names[j];
		if(!cur_plug_name)continue;
		char** temp_return_names = realloc(return_names, sizeof(char*) * (total_names_found +1));
		if(!temp_return_names){
		    free(cur_plug_name);
		    continue;
		}
		return_names = temp_return_names;
		return_names[total_names_found] = cur_plug_name;
		total_names_found += 1;
	    }
	    free(plug_names);
	}
	free(total_file_path);
	free(cur_file);
    }
    free(clap_files);
    if(total_names_found == 0){
	free(return_names);
	return NULL;
    }
    *size = total_names_found;
    return return_names;
}

CLAP_PLUG_INFO* clap_plug_init(uint32_t min_buffer_size, uint32_t max_buffer_size, SAMPLE_T samplerate,
			       clap_plug_status_t* plug_error, void* audio_backend){
    CLAP_PLUG_INFO* plug_data = (CLAP_PLUG_INFO*)malloc(sizeof(CLAP_PLUG_INFO));
    if(!plug_data){
	*plug_error = clap_plug_failed_malloc;
	return NULL;
    }   
    memset(plug_data, '\0', sizeof(*plug_data));
    plug_data->min_buffer_size = min_buffer_size;
    plug_data->max_buffer_size = max_buffer_size;
    plug_data->max_id = -1;
    plug_data->sample_rate = samplerate;
    plug_data->total_plugs = 0;
    for(int i = 0; i < MAX_INSTANCES; i++){
	plug_data->plugins[i] = NULL;
    }
    //init the realtime audio thread messages to the main thread ring buffer
    plug_data->rt_to_ui_msgs = ring_buffer_init(sizeof(CLAP_RING_SYS_MSG), MAX_RING_BUFFER_ARRAY_SIZE);
    if(!(plug_data->rt_to_ui_msgs)){
	clap_plug_clean_memory(plug_data);
	*plug_error = clap_plug_failed_malloc;
	return NULL;
    }
    //init the ui main thread messages to the main thread ring buffer
    plug_data->ui_to_ui_msgs = ring_buffer_init(sizeof(CLAP_RING_SYS_MSG), MAX_RING_BUFFER_ARRAY_SIZE);
    if(!(plug_data->ui_to_ui_msgs)){
	clap_plug_clean_memory(plug_data);
	*plug_error = clap_plug_failed_malloc;
	return NULL;
    }
    
    clap_host_t clap_info_host;
    clap_version_t clap_v;
    clap_v.major = CLAP_VERSION_MAJOR;
    clap_v.minor = CLAP_VERSION_MINOR;
    clap_v.revision = CLAP_VERSION_REVISION;
    clap_info_host.clap_version = clap_v;
    clap_info_host.host_data = (void*)plug_data;
    clap_info_host.name = "smp_groovebox";
    clap_info_host.vendor = "bru";
    clap_info_host.url = "https://brumakes.com";
    clap_info_host.version = "0.2";
    clap_info_host.get_extension = get_extension;
    clap_info_host.request_restart = request_restart;
    clap_info_host.request_process = request_process;
    clap_info_host.request_callback = request_callback;

    plug_data->clap_host_info = clap_info_host;

    //initiate the host clap extension structs
    //thread check extension
    plug_data->ext_thread_check.is_audio_thread = clap_plug_ext_is_audio_thread;
    plug_data->ext_thread_check.is_main_thread = clap_plug_ext_is_main_thread;
    
    /*
    void* handle;
    int* iptr;
    handle = dlopen(plug_path, RTLD_LOCAL | RTLD_LAZY);
    if(!handle){
	log_append_logfile("failed to load %s dso \n", plug_path);
    }
    
    iptr = (int*)dlsym(handle, "clap_entry");
    clap_plugin_entry_t* plug_entry = (clap_plugin_entry_t*)iptr;
    
    unsigned int init_err = plug_entry->init(plug_path);
    if(!init_err){
	log_append_logfile("failed to init the %s plugin entry\n", plug_path);
	return;
    }
    
    const clap_plugin_factory_t* plug_fac = plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    uint32_t plug_count = plug_fac->get_plugin_count(plug_fac);
    if(plug_count <= 0){
	log_append_logfile("no plugins in %s dso \n", plug_path);
	return;
    }

    //TODO iterating through the clap files should be in the clap_plug_return_plugin_names function
    //TODO when user chooses which plugin to load, go through the clap files (and the plugins in the files) and compare the plugin name
    //to the name the user sent, if its the same load this plugin
    for(uint32_t pl_iter = 0; pl_iter < plug_count; pl_iter++){
	const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, pl_iter);
	if(!plug_desc)continue;
	if(plug_desc->name){
	    log_append_logfile("Got clap_plugin %s descriptor\n", plug_desc->name);
	}
	if(plug_desc->description){
	    log_append_logfile("%s info: %s\n", plug_desc->name, plug_desc->description);
	}

	const clap_plugin_t* plug_inst = plug_fac->create_plugin(plug_fac, &clap_info_host, plug_desc->id);
	if(!plug_inst){
	    log_append_logfile("Failed to create %s plugin\n", plug_path);
	    continue;
	}

	bool inst_err = plug_inst->init(plug_inst);
	if(!inst_err){
	    log_append_logfile("Failed to init %s plugin\n", plug_path);
	    continue;
	}

	plug_inst->activate(plug_inst, 48000, 32, 1024);

	const clap_plugin_params_t* plug_inst_params = plug_inst->get_extension(plug_inst, CLAP_EXT_PARAMS);
	if(plug_inst_params){
	    uint32_t params_count = plug_inst_params->count(plug_inst);
	    log_append_logfile("found %d params\n", params_count);
	    for(uint32_t pr_iter = 0; pr_iter < params_count; pr_iter++){
		clap_param_info_t param_info;
		bool got_info = plug_inst_params->get_info(plug_inst, pr_iter, &param_info);
		if(!got_info)continue;
		log_append_logfile("Param name %s, min_val %g, max_val %g, default_val %g\n",
				   param_info.name, param_info.module,
				   param_info.min_value, param_info.max_value, param_info.default_value);
		double cur_value = 0;
		bool got_val = plug_inst_params->get_value(plug_inst, param_info.id, &cur_value);
		if(!got_val)continue;
		char val_text[MAX_VALUE_LEN];
		if(plug_inst_params->value_to_text(plug_inst, param_info.id, cur_value,
						   val_text, MAX_VALUE_LEN)){
		    log_append_logfile("Current value double %f, text %s, param is enum %d, is_stepped %d, requires_process %d\n",
				       cur_value, val_text,
				       (param_info.flags & CLAP_PARAM_IS_ENUM),
				       (param_info.flags & CLAP_PARAM_IS_STEPPED),
				       (param_info.flags & CLAP_PARAM_REQUIRES_PROCESS));
		}
	    }
	}
	
	plug_inst->deactivate(plug_inst);
	plug_inst->destroy(plug_inst);
    }

    plug_entry->deinit();
    */
    return plug_data;
}

//return the clap_plug_plug with plugin entry (initiated), plug_path and plug_inst_id from the plugins name, checks if  the same entry is already loaded or not first
static CLAP_PLUG_PLUG* clap_plug_create_plug_from_name(CLAP_PLUG_INFO* plug_data, const char* plug_name){
    CLAP_PLUG_PLUG* return_plug = NULL;
    return_plug = malloc(sizeof(CLAP_PLUG_PLUG));
    if(!return_plug)return NULL;
    return_plug->id = -1;
    return_plug->plug_entry = NULL;
    return_plug->plug_inst = NULL;
    return_plug->plug_inst_created = 0;
    return_plug->plug_inst_activated = 0;
    return_plug->plug_inst_id = -1;
    return_plug->plug_params = NULL;
    return_plug->plug_path = NULL;
    return_plug->plug_data = plug_data;
    
    if(!plug_data)return NULL;
    unsigned int clap_files_num = 0;
    char** clap_files = clap_plug_get_clap_files(CLAP_PATH, &clap_files_num);
    for(unsigned int i = 0; i < clap_files_num; i++){
	char* cur_clap_file = clap_files[i];
	if(!cur_clap_file)continue;
	
	//check if a plugin with the same name already exists if yes get its plugin entry
	for(unsigned int plug_num = 0; plug_num < plug_data->total_plugs; plug_num++){
	    CLAP_PLUG_PLUG* cur_plug = plug_data->plugins[plug_num];
	    if(!cur_plug)continue;
	    char* cur_path = cur_plug->plug_path;
	    if(!cur_path)continue;
	    if(strcmp(cur_clap_file, cur_path) != 0)continue;
	    if(!(cur_plug->plug_entry))continue;
	    char* plug_path = malloc(sizeof(char) * (strlen(cur_clap_file)+1));
	    if(!plug_path)continue;
	    snprintf(plug_path, (strlen(cur_clap_file)+1), "%s", cur_clap_file);
	    return_plug->plug_entry = cur_plug->plug_entry;
	    return_plug->plug_path = plug_path;
	}

	//if return_plug was not created it means a plugin with the same cur_clap_file path is not loaded, we need to load it
	if(!(return_plug->plug_path)){
	    void* handle;
	    int* iptr;
	    unsigned int total_file_name_size = strlen(CLAP_PATH) + strlen(cur_clap_file) + 1;
	    char* total_file_path = malloc(sizeof(char) * total_file_name_size);
	    if(!total_file_path)continue;
	    snprintf(total_file_path, total_file_name_size, "%s%s", CLAP_PATH, cur_clap_file);
	    
	    handle = dlopen(total_file_path, RTLD_LOCAL | RTLD_LAZY);
	    if(!handle)continue;
    
	    iptr = (int*)dlsym(handle, "clap_entry");
	    clap_plugin_entry_t* plug_entry = (clap_plugin_entry_t*)iptr;
    
	    unsigned int init_err = plug_entry->init(total_file_path);
	    free(total_file_path);
	    if(!init_err)continue;
	    char* plug_path = malloc(sizeof(char) * (strlen(cur_clap_file)+1));
	    if(!plug_path){
		plug_entry->deinit();
		continue;
	    }
	    snprintf(plug_path, (strlen(cur_clap_file)+1), "%s", cur_clap_file);
	    return_plug->plug_path = plug_path;
	    return_plug->plug_entry = plug_entry;
	}
	if(!(return_plug->plug_path))continue;
	//now we have the plug_entry and path, find the plugin name and init the plugin
	const clap_plugin_factory_t* plug_fac = return_plug->plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	uint32_t plug_count = plug_fac->get_plugin_count(plug_fac);
	
	for(uint32_t pl_iter = 0; pl_iter < plug_count; pl_iter++){
	    const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, pl_iter);
	    if(!plug_desc)continue;
	    if(!(plug_desc->name))continue;
	    if(strcmp(plug_desc->name, plug_name) != 0)continue;
	    return_plug->plug_inst_id = pl_iter;
	    break;
	}
	if(return_plug->plug_inst_id == -1){
	    if(clap_plug_return_plug_id_with_same_plug_entry(plug_data, return_plug->plug_entry) == -1)
		return_plug->plug_entry->deinit();
	    return_plug->plug_entry = NULL;
	    free(return_plug->plug_path);
	    return_plug->plug_path = NULL;
	    continue;
	}
	break;
    }

    //free the clap_files entries
    for(unsigned int i = 0; i < clap_files_num; i++){
	if(clap_files[i])free(clap_files[i]);
    }
    if(clap_files)free(clap_files);
    if(!(return_plug->plug_path)){
	clap_plug_plug_clean(plug_data, return_plug);
	return_plug = NULL;
    }
    return return_plug;
}

int clap_plug_load_and_activate(CLAP_PLUG_INFO* plug_data, const char* plugin_name, int id){
    int return_id = -1;
    if(!plug_data)return -1;
    if(!plugin_name)return -1;
    if(plug_data->total_plugs >= MAX_INSTANCES)return -1;
    //get the plug from the name, this plug will only have the entry, plug_path and which plug_inst_id this plugin_name is in the plugin factory at this point
    CLAP_PLUG_PLUG* plug = clap_plug_create_plug_from_name(plug_data, plugin_name);
    if(!plug){
	log_append_logfile("could not load plugin from name %s\n", plugin_name);
	return -1;
    }
    const clap_plugin_factory_t* plug_fac = plug->plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, plug->plug_inst_id);
    if(!plug_desc){
	log_append_logfile("Could not get plugin %s descriptor \n", plug->plug_path);
	clap_plug_plug_clean(plug_data, plug);
	return -1;
    }
    if(plug_desc->name){
	log_append_logfile("Got clap_plugin %s descriptor\n", plug_desc->name);
    }
    if(plug_desc->description){
	log_append_logfile("%s info: %s\n", plug_desc->name, plug_desc->description);
    }
    //add the clap_host_t struct to the plug
    clap_host_t clap_info_host;
    clap_info_host.clap_version = plug_data->clap_host_info.clap_version;
    clap_info_host.host_data = (void*)plug;
    clap_info_host.name = plug_data->clap_host_info.name;
    clap_info_host.vendor = plug_data->clap_host_info.vendor;
    clap_info_host.url = plug_data->clap_host_info.url;
    clap_info_host.version = plug_data->clap_host_info.version;
    clap_info_host.get_extension = plug_data->clap_host_info.get_extension;
    clap_info_host.request_restart = plug_data->clap_host_info.request_restart;
    clap_info_host.request_process = plug_data->clap_host_info.request_process;
    clap_info_host.request_callback = plug_data->clap_host_info.request_callback;
    plug->clap_host_info = clap_info_host;
    //create plugin instance
    const clap_plugin_t* plug_inst = plug_fac->create_plugin(plug_fac, &(plug->clap_host_info) , plug_desc->id);
    if(!plug_inst){
	log_append_logfile("Failed to create %s plugin\n", plugin_name);
	clap_plug_plug_clean(plug_data, plug);
	return -1;
    }
    
    plug->plug_inst_created = 1;
    
    bool inst_err = plug_inst->init(plug_inst);
    if(!inst_err){
	log_append_logfile("Failed to init %s plugin\n", plugin_name);
	clap_plug_plug_clean(plug_data, plug);
	return -1;
    }
    plug->plug_inst = plug_inst;

    //TODO the order of the further todo list should be considered, at this point the plugin is created but in its deactivated state, but the plugin can access all the host extension functions at this point
    //Thats why need to consider if the plugin should be added to the plugin array even before the creation of the plugin instance (for the correc plug->id and etc.)
    //TODO need to activate the plugin, create parameters, get ports, create ports on the audio_client and etc.
    //TODO after everything need to add the plugin to the plugin array on the plug_data
    
    //TODO now cleaning for testing
    clap_plug_plug_clean(plug_data, plug);
    
    return return_id;
}

void clap_plug_clean_memory(CLAP_PLUG_INFO* plug_data){
    if(!plug_data)return;
    for(int i = 0; i < MAX_INSTANCES; i++){
	clap_plug_plug_clean(plug_data, plug_data->plugins[i]);
	plug_data->plugins[i] = NULL;
    }
    //clean the ring buffers
    ring_buffer_clean(plug_data->rt_to_ui_msgs);
    ring_buffer_clean(plug_data->ui_to_ui_msgs);
    free(plug_data);
}
