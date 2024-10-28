#include "clap_plugins.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "../util_funcs/log_funcs.h"
#include <clap/clap.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/string_funcs.h"
#include "params.h"
//what is the size of the buffer to get the formated param values to
#define MAX_VALUE_LEN 64
#define CLAP_PATH "/usr/lib/clap/"
//how many clap plugins can there be in the plugin array
#define MAX_INSTANCES 5
//the single clap plugin struct
typedef struct _clap_plug_plug{
    int id; //plugin id on the clap_plug_info plugin array
    clap_plugin_entry_t* plug_entry; //the clap library file for this plugin
    clap_plugin_t* plug_inst; //the plugin instance
    unsigned int plug_inst_id; //the plugin instance id in the plugin factory
    char* plug_path; //the path for the clap file
    PRM_CONTAIN* plug_params; //plugin parameter container for params.c
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
}CLAP_PLUG_INFO;

const void* get_extension(const clap_host_t* host, const char* ex_id){
    log_append_logfile("clap_plugin requested extension %s\n", ex_id);

    return NULL;
}

void request_restart(const clap_host_t* host){};
void request_process(const clap_host_t* host){};
void request_callback(const clap_host_t* host){};

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

CLAP_PLUG_INFO* clap_plug_init(uint32_t min_buffer_size, uint32_t max_buffer_size, SAMPLE_T samplerate, clap_plug_status_t* plug_error, void* audio_backend){
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
}

void clap_plug_clean_memory(CLAP_PLUG_INFO* plug_data){
    if(!plug_data)return;
    for(int i = 0; i < plug_data->max_id + 1; i++){
	//TODO should be a function that cleans this individual plugin
    }
    free(plug_data);
}
