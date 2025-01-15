#include "clap_plugins.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <threads.h>
#include <stdatomic.h>
#include "../util_funcs/log_funcs.h"
#include <clap/clap.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <semaphore.h>
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/string_funcs.h"
#include "../util_funcs/ring_buffer.h"
#include "../types.h"
#include "params.h"
//what is the size of the buffer to get the formated param values to
#define MAX_VALUE_LEN 64
#define CLAP_PATH "/usr/lib/clap/"
//how many clap plugins can there be in the plugin array
#define MAX_INSTANCES 5

static thread_local bool is_audio_thread = false;

//the single clap plugin struct
typedef struct _clap_plug_plug{
    int id; //plugin id on the clap_plug_info plugin array
    clap_plugin_entry_t* plug_entry; //the clap library file for this plugin
    const clap_plugin_t* plug_inst; //the plugin instance
    unsigned int plug_inst_created; //was a init function called in the descriptor for this plugin
    unsigned int plug_inst_activated; //was the activate function called on this plugin
    atomic_int plug_inst_processing; //is the plugin instance processing, touch only on [audio_thread]
    int plug_inst_id; //the plugin instance index in the array of the plugin factory
    char* plug_path; //the path for the clap file
    PRM_CONTAIN* plug_params; //plugin parameter container for params.c
    clap_host_t clap_host_info; //need when creating the plugin instance, this struct has this CLAP_PLUG_PLUG in the host var as (void*)
    CLAP_PLUG_INFO* plug_data; //CLAP_PLUG_INFO struct address for convenience
}CLAP_PLUG_PLUG;

//the main clap struct
typedef struct _clap_plug_info{
    struct _clap_plug_plug plugins[MAX_INSTANCES+1]; //array with single clap plugins
    SAMPLE_T sample_rate;
    //for clap there can be min and max buffer sizes, for not changing buffer sizes set as the same
    uint32_t min_buffer_size;
    uint32_t max_buffer_size;
    //placeholder for the host data that plugins send to the host, this has clap_host_info.host_data = NULL,
    //the clap_host_t has the CLAP_PLUG_PLUG* plug in clap_host_info.host_data. This is just for convenience - to copy to plug clap_host_info the necessary info.
    clap_host_t clap_host_info;
    //ring buffer for messages from rt to ui
    RING_BUFFER* rt_to_ui_msgs;
    //ring buffer for messages from the main ui thread to the audio realtime thread
    RING_BUFFER* ui_to_rt_msgs;
    //semaphore when [main-thread] needs to wait for the [audio-thread] to change something (for example stop the plugin or start the plugin)
    //wait on [main-thread] post on [audio-thread]
    //[audio-thread] can post the semaphore only in response to [main-thread] message
    sem_t wait_for_rt;
    //from here hold various host extension implementation structs
    clap_host_thread_check_t ext_thread_check; //struct that holds functions to check if the thread is main or audio
    clap_host_log_t ext_log; //struct that holds function for logging messages (severity is not sent)
}CLAP_PLUG_INFO;

static void clap_plug_plug_wait_for_stop(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return;

    if(plug_id < 0) return;
    if(plug_id >= MAX_INSTANCES)return;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
        
    RING_SYS_MSG send_bit;
    send_bit.msg_enum = MSG_PLUGIN_STOP_PROCESS;
    send_bit.scx_id = plug_id;
    ring_buffer_write(plug_data->ui_to_rt_msgs, &send_bit, sizeof(send_bit));
    //lock [main-thread] and wait for a response from [audio-thread]
    sem_wait(&plug_data->wait_for_rt);
}
//start processing also blocks main thread, so the order of commands does not get mixed
static void clap_plug_plug_wait_for_start(CLAP_PLUG_PLUG* plug){
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    RING_SYS_MSG send_bit;
    send_bit.msg_enum = MSG_PLUGIN_PROCESS;
    send_bit.scx_id = plug->id;
    ring_buffer_write(plug_data->ui_to_rt_msgs, &send_bit, sizeof(send_bit));
    //lock [main-thread] and wait for a response from [audio-thread]
    sem_wait(&plug_data->wait_for_rt);
}

//return the clap_plug_plug id on the plugins array that has the same plug_entry (initiated) if no plugin has this plug_entry return -1
static int clap_plug_return_plug_id_with_same_plug_entry(CLAP_PLUG_INFO* plug_data, clap_plugin_entry_t* plug_entry){
    int return_id = -1;
    if(!plug_data)return -1;
    if(!plug_entry)return -1;

    for(unsigned int plug_num = 0; plug_num < MAX_INSTANCES; plug_num++){
	CLAP_PLUG_PLUG* cur_plug = &(plug_data->plugins[plug_num]);
	if(!cur_plug)continue;
	if(cur_plug->plug_entry != plug_entry)continue;
	return_id = cur_plug->id;
	break;
    }
    
    return return_id;
}

//clean the single plugin struct
//before calling this the plug_inst_processing should be == 0
static int clap_plug_plug_clean(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
      
    if(plug->plug_inst){
	if(plug->plug_inst_activated == 1){
	    plug->plug_inst->deactivate(plug->plug_inst);
	    plug->plug_inst_activated = 0;
	}
	if(plug->plug_inst_created == 1){
	    plug->plug_inst->destroy(plug->plug_inst);
	    plug->plug_inst_created = 0;
	}
	plug->plug_inst = NULL;	
    }
    if(plug->plug_entry){
	if(clap_plug_return_plug_id_with_same_plug_entry(plug_data, plug->plug_entry) == -1)plug->plug_entry->deinit();
	plug->plug_entry = NULL;
    }
    if(plug->plug_path){
	free(plug->plug_path);
	plug->plug_path = NULL;
    }
    if(plug->plug_params){
	param_clean_param_container(plug->plug_params);
	plug->plug_params = NULL;
    }
    plug->plug_inst_id = -1;

    return 0;
}

static int clap_plug_plug_stop_and_clean(CLAP_PLUG_INFO* plug_data, int plug_id){
    //stop this plugin process if its processing before cleaning.
    clap_plug_plug_wait_for_stop(plug_data, plug_id);
    return clap_plug_plug_clean(plug_data, plug_id);
}

//return if this is audio_thread or not
static bool clap_plug_return_is_audio_thread(){
    return is_audio_thread;
}
//extension functions for thread-check.h to return is this audio or main thread for the clap_host_thread_t extension
static bool clap_plug_ext_is_audio_thread(const clap_host_t* host){
    return clap_plug_return_is_audio_thread();
}
static bool clap_plug_ext_is_main_thread(const clap_host_t* host){
    return !(clap_plug_return_is_audio_thread());
}
//send a [thread-safe] message - check if thread is audio or main
static void clap_plug_send_msg(CLAP_PLUG_INFO* plug_data, const char* msg){
    if(clap_plug_return_is_audio_thread()){
	if(!plug_data)return;
	
	RING_SYS_MSG send_bit;
	snprintf(send_bit.msg, MAX_STRING_MSG_LENGTH, "%s", msg);
	send_bit.msg_enum = MSG_PLUGIN_SENT_STRING;
	int ret = ring_buffer_write(plug_data->rt_to_ui_msgs, &send_bit, sizeof(send_bit));
    }
    else{
	log_append_logfile("%s", msg);
    }
}
//extension function for log.h to send messages in [thread-safe] manner
static void clap_plug_ext_log(const clap_host_t* host, clap_log_severity severity, const char* msg){
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    //severity is not sent, right now dont see the need to send severity - this should be obvious from the message
    clap_plug_send_msg(plug_data, msg);
}

const void* get_extension(const clap_host_t* host, const char* ex_id){    
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return NULL;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return NULL;
    if(strcmp(ex_id, CLAP_EXT_THREAD_CHECK) == 0){
	return &(plug_data->ext_thread_check);
    }
    if(strcmp(ex_id, CLAP_EXT_LOG) == 0){
	return &(plug_data->ext_log);
    }
 
    //if there is no extension implemented that the plugin needs send the name of the extension to the ui
    char msg[MAX_STRING_MSG_LENGTH];
    snprintf(msg, MAX_STRING_MSG_LENGTH, "%s asked for ext %s\n", plug->plug_path, ex_id);
    clap_plug_send_msg(plug_data, msg);

    return NULL;
}
static int clap_plug_activate_start_processing(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
    if(!plug)return -1;
    //since there was a request to start processing the plugin, it should be stopped, but just in case, send a request to stop it
    clap_plug_plug_wait_for_stop(plug->plug_data, plug_id);
    
    if(plug->plug_inst_activated == 0){
	if(!plug->plug_inst)return -1;
	if(!plug->plug_inst->activate(plug->plug_inst, plug_data->sample_rate, plug_data->min_buffer_size, plug_data->max_buffer_size)){
	    return -1;
	}
    }
    plug->plug_inst_activated = 1;
    //send message to the audio thread that the plugin can be started to process
    //and wait for it to start
    clap_plug_plug_wait_for_start(plug);

    return 0;
}
static int clap_plug_restart(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
    //send a message to rt thread to stop the plugin process and block while its stopping
    clap_plug_plug_wait_for_stop(plug->plug_data, plug_id);
    
    if(plug->plug_inst_activated == 1){
	plug->plug_inst->deactivate(plug->plug_inst);
	plug->plug_inst_activated = 0;
    }
    //now when the plugin was deactivated simply call the activate and process function, that will reactivate the plugin and send a message to [audio-thread] to start processing it
    return clap_plug_activate_start_processing(plug_data, plug_id);
}
void request_restart(const clap_host_t* host){   
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    if(clap_plug_return_is_audio_thread()){
	//send to ui a message that the plugin needs a restart
	RING_SYS_MSG send_bit;
	send_bit.msg_enum = MSG_PLUGIN_RESTART;
	send_bit.scx_id = plug->id;
	ring_buffer_write(plug_data->rt_to_ui_msgs, &send_bit, sizeof(send_bit));
    }
    else{
	clap_plug_restart(plug_data, plug->id);
    }
  
}

void request_process(const clap_host_t* host){   
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    //dont call process function directly even in the audio thread, because the plugin can be deacticated
    //and checking plug_inst_activated var would cause a data race condition, so the plugin will be checked for activated and then a message to the audio thread will be sent
    //to call the start_processing function
    if(clap_plug_return_is_audio_thread()){
	//send to ui a message that the plugin needs to be activated and then the start_processing function called
	RING_SYS_MSG send_bit;
	send_bit.msg_enum = MSG_PLUGIN_ACTIVATE_PROCESS;
	send_bit.scx_id = plug->id;
	ring_buffer_write(plug_data->rt_to_ui_msgs, &send_bit, sizeof(send_bit));
    }
    else{
	clap_plug_activate_start_processing(plug_data, plug->id);
    }

}

static int clap_plug_callback(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);
    if(!plug)return -1;
    if(!plug->plug_inst)return -1;
    plug->plug_inst->on_main_thread(plug->plug_inst);
    return 0;
}
void request_callback(const clap_host_t* host){   
    CLAP_PLUG_PLUG* plug = (CLAP_PLUG_PLUG*)host->host_data;
    if(!plug)return;
    CLAP_PLUG_INFO* plug_data = plug->plug_data;
    if(!plug_data)return;
    
    if(clap_plug_return_is_audio_thread()){
	//send to ui a message that the plugin needs to call the on_main_thread function 
	RING_SYS_MSG send_bit;
	send_bit.msg_enum = MSG_PLUGIN_REQUEST_CALLBACK;
	send_bit.scx_id = plug->id;
	ring_buffer_write(plug_data->rt_to_ui_msgs, &send_bit, sizeof(send_bit));
    }
    else{
	clap_plug_callback(plug_data, plug->id);
    }
  
}

static int clap_plug_start_process(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);

    //if plugin is already processing do nothing
    if(plug->plug_inst_processing == 1){
	sem_post(&plug_data->wait_for_rt);
	return 0;
    }

    if(!plug->plug_inst){
	sem_post(&plug_data->wait_for_rt);
	return -1;
    }
    if(!(plug->plug_inst->start_processing(plug->plug_inst))){
	sem_post(&plug_data->wait_for_rt);
	return -1;
    }

    plug->plug_inst_processing = 1;
    sem_post(&plug_data->wait_for_rt);
    return 0;
}
static int clap_plug_stop_process(CLAP_PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return -1;
    
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;

    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[plug_id]);

    //if plugin is already stopped and not processing do nothing
    if(plug->plug_inst_processing == 0){
	sem_post(&plug_data->wait_for_rt);
	return 0;
    }
    if(!plug->plug_inst){
	sem_post(&plug_data->wait_for_rt);
	return -1;
    }
    plug->plug_inst->stop_processing(plug->plug_inst);

    plug->plug_inst_processing = 0;
    sem_post(&plug_data->wait_for_rt);
    return 0;
}
int clap_read_ui_to_rt_messages(CLAP_PLUG_INFO* plug_data){
    //this is a local thread var its false on [main-thread] and true on [audio-thread]
    is_audio_thread = true;
    
    if(!plug_data)return -1;
    RING_BUFFER* ring_buffer = plug_data->ui_to_rt_msgs;
    if(!ring_buffer)return -1;
    unsigned int cur_items = ring_buffer_return_items(ring_buffer);
    if(cur_items <= 0)return 0;
    for(unsigned int i = 0; i < cur_items; i++){
	RING_SYS_MSG cur_bit;
	int read_buffer = ring_buffer_read(ring_buffer, &cur_bit, sizeof(cur_bit));
	if(read_buffer <= 0)continue;
	if(cur_bit.msg_enum == MSG_PLUGIN_PROCESS){
	    clap_plug_start_process(plug_data, cur_bit.scx_id);
	}
	if(cur_bit.msg_enum == MSG_PLUGIN_STOP_PROCESS){
	    clap_plug_stop_process(plug_data, cur_bit.scx_id);
	}
    }
    return 0;
}
int clap_read_rt_to_ui_messages(CLAP_PLUG_INFO* plug_data){
    if(!plug_data)return -1;
    RING_BUFFER* ring_buffer = plug_data->rt_to_ui_msgs;
    if(!ring_buffer)return -1;
    unsigned int cur_items = ring_buffer_return_items(ring_buffer);
    if(cur_items <= 0)return 0;
    for(unsigned int i = 0; i < cur_items; i++){
	RING_SYS_MSG cur_bit;
	int read_buffer = ring_buffer_read(ring_buffer, &cur_bit, sizeof(cur_bit));
	if(read_buffer <= 0)continue;
	if(cur_bit.msg_enum == MSG_PLUGIN_REQUEST_CALLBACK){
	    clap_plug_callback(plug_data, cur_bit.scx_id);
	}
	if(cur_bit.msg_enum == MSG_PLUGIN_ACTIVATE_PROCESS){
	    clap_plug_activate_start_processing(plug_data, cur_bit.scx_id);
	}
	if(cur_bit.msg_enum == MSG_PLUGIN_RESTART){
	    clap_plug_restart(plug_data, cur_bit.scx_id);
	}
	if(cur_bit.msg_enum == MSG_PLUGIN_SENT_STRING){
	    log_append_logfile("%s", cur_bit.msg);
	}
    }
    return 0;
}

static char** clap_plug_get_plugin_names_from_file(const char* plug_path, unsigned int* num_of_plugins){
    void* handle;
    int* iptr;
    handle = dlopen(plug_path, RTLD_LOCAL | RTLD_LAZY);
    if(!handle){
	log_append_logfile("failed to load %s dso \n", plug_path);
	return NULL;
    }
    
    iptr = (int*)dlsym(handle, "clap_entry");
    clap_plugin_entry_t* plug_entry = (clap_plugin_entry_t*)iptr;
    if(!plug_entry)return NULL;
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
    if(sem_init(&plug_data->wait_for_rt, 0, 0) != 0){
	free(plug_data);
	return NULL;
    }
    plug_data->min_buffer_size = min_buffer_size;
    plug_data->max_buffer_size = max_buffer_size;
    plug_data->sample_rate = samplerate;
    //init the realtime audio thread messages to the main thread ring buffer
    plug_data->rt_to_ui_msgs = ring_buffer_init(sizeof(RING_SYS_MSG), MAX_SYS_BUFFER_ARRAY_SIZE);
    if(!(plug_data->rt_to_ui_msgs)){
        free(plug_data);
	*plug_error = clap_plug_failed_malloc;
	return NULL;
    }
    //init the ui main thread to audio realtime thread messages ring buffer
    plug_data->ui_to_rt_msgs = ring_buffer_init(sizeof(RING_SYS_MSG), MAX_SYS_BUFFER_ARRAY_SIZE);
    if(!(plug_data->ui_to_rt_msgs)){
        free(plug_data);
	ring_buffer_clean(plug_data->rt_to_ui_msgs);
	*plug_error = clap_plug_failed_malloc;
	return NULL;
    }
    
    clap_host_t clap_info_host;
    clap_version_t clap_v;
    clap_v.major = CLAP_VERSION_MAJOR;
    clap_v.minor = CLAP_VERSION_MINOR;
    clap_v.revision = CLAP_VERSION_REVISION;
    clap_info_host.clap_version = clap_v;
    clap_info_host.host_data = NULL;
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
    //log extension
    plug_data->ext_log.log = clap_plug_ext_log;

    //the array holds one more plugin, it will be an empty shell to check if the total number of plugins arent too many
    for(int i = 0; i < (MAX_INSTANCES+1); i++){
        CLAP_PLUG_PLUG* plug = &(plug_data->plugins[i]);
	plug->clap_host_info = clap_info_host;
	plug->id = i;
	plug->plug_data = plug_data;
	plug->plug_entry = NULL;
	plug->plug_inst = NULL;
	plug->plug_inst_activated = 0;
	plug->plug_inst_created = 0;
	plug->plug_inst_id = -1;
        plug->plug_inst_processing = 0;
	plug->plug_params = NULL;
	plug->plug_path = NULL;
    }    
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
static int clap_plug_create_plug_from_name(CLAP_PLUG_INFO* plug_data, const char* plug_name, int plug_id){
    if(!plug_data)return -1;
    if(plug_id < 0)return -1;
    if(plug_id >= MAX_INSTANCES)return -1;
    CLAP_PLUG_PLUG* return_plug = &(plug_data->plugins[plug_id]);
    
    unsigned int clap_files_num = 0;
    char** clap_files = clap_plug_get_clap_files(CLAP_PATH, &clap_files_num);
    for(unsigned int i = 0; i < clap_files_num; i++){
	char* cur_clap_file = clap_files[i];
	if(!cur_clap_file)continue;
	
	//check if a plugin with the same name already exists if yes get its plugin entry
	for(unsigned int plug_num = 0; plug_num < MAX_INSTANCES; plug_num++){
	    CLAP_PLUG_PLUG cur_plug = plug_data->plugins[plug_num];
	    char* cur_path = cur_plug.plug_path;
	    if(!cur_path)continue;
	    if(strcmp(cur_clap_file, cur_path) != 0)continue;
	    if(!(cur_plug.plug_entry))continue;
	    char* plug_path = malloc(sizeof(char) * (strlen(cur_clap_file)+1));
	    if(!plug_path)continue;
	    snprintf(plug_path, (strlen(cur_clap_file)+1), "%s", cur_clap_file);
	    return_plug->plug_entry = cur_plug.plug_entry;
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
	clap_plug_plug_stop_and_clean(plug_data, return_plug->id);
	return -1;
    }
    return 0;
}

int clap_plug_load_and_activate(CLAP_PLUG_INFO* plug_data, const char* plugin_name, int id){
    int return_id = -1;
    if(!plug_data)return -1;
    if(!plugin_name)return -1;
    if(id >= MAX_INSTANCES)return -1;
    //if id is negative find an empty slot in the plugins array and create the plugin there
    if(id < 0){
	for(int i = 0; i < (MAX_INSTANCES+1); i++){
	    CLAP_PLUG_PLUG cur_plug = plug_data->plugins[i];
	    //if there is a path for this plugin the slot is not empty
	    if(cur_plug.plug_inst)continue;
	    id = cur_plug.id;
	    break;
	}
    }
    //if the id is still not within range an error happened, cant create the plugin
    if(id < 0 || id >= MAX_INSTANCES)return -1;
    //if id is in the possible range, clean the slot just in case its occupied 
    clap_plug_plug_stop_and_clean(plug_data, id);
    
    if(clap_plug_create_plug_from_name(plug_data, plugin_name, id) < 0){
	log_append_logfile("could not load plugin from name %s\n", plugin_name);
	return -1;
    }
    //this plug will only have the entry, plug_path and which plug_inst_id this plugin_name is in the plugin factory array at this point
    CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
    const clap_plugin_factory_t* plug_fac = plug->plug_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* plug_desc = plug_fac->get_plugin_descriptor(plug_fac, plug->plug_inst_id);
    if(!plug_desc){
	log_append_logfile("Could not get plugin %s descriptor \n", plug->plug_path);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
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
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    plug->plug_inst = plug_inst;
    plug->plug_inst_created = 1;
    
    bool inst_err = plug_inst->init(plug_inst);
    if(!inst_err){
	log_append_logfile("Failed to init %s plugin\n", plugin_name);
	clap_plug_plug_stop_and_clean(plug_data, plug->id);
	return -1;
    }
    

    //TODO the order of the further todo list should be considered, at this point the plugin is created but in its deactivated state, but the plugin can access all the host extension functions at this point
    //TODO need to activate the plugin, create parameters, get ports, create ports on the audio_client and etc.
    //TODO now cleaning for testing
    clap_plug_plug_stop_and_clean(plug_data, plug->id);
    return_id = plug->id;
    return return_id;
}

void clap_process_data_rt(CLAP_PLUG_INFO* plug_data, unsigned int nframes){
    if(!plug_data)return;
    if(!plug_data->plugins)return;
    for(int id = 0; id < MAX_INSTANCES; id++){
	CLAP_PLUG_PLUG* plug = &(plug_data->plugins[id]);
	if(plug->plug_inst_processing == 0)continue;
	if(!plug->plug_inst)continue;
    }
}

void clap_plug_clean_memory(CLAP_PLUG_INFO* plug_data){
    if(!plug_data)return;
    for(int i = 0; i < MAX_INSTANCES; i++){
	clap_plug_plug_clean(plug_data, i);
    }

    //clean the ring buffers
    ring_buffer_clean(plug_data->rt_to_ui_msgs);
    ring_buffer_clean(plug_data->ui_to_rt_msgs);

    sem_destroy(&plug_data->wait_for_rt);
    
    free(plug_data);
}
