#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include "contexts/clap_plugins.h"
//my libraries
#include "app_intrf.h"
//include the functions to parse the xml song files
#include "util_funcs/json_funcs.h"
//app_data
#include "app_data.h"
//contexts
#include "contexts/sampler.h"
//string manipulation
#include "util_funcs/string_funcs.h"
//log file functions
#include "util_funcs/log_funcs.h"
//how many strings in the attribute name or attribute value arrays
#define MAX_ATTRIB_ARRAY 40
//how big is the string for parameter values when they are returned as string
#define MAX_VALUE_STRING 40
//how big is the string for parameter configuration file names
#define MAX_PARAM_CONFIG_STRING 100
//the file extension we are using for songs and presets
#define FILE_EXT ".json"
/*A FEW IMPORTANT RULES - no name or path strings can contain "<__>", its added to names automatically;
  if cx, path or any string is numbered should have"-" before the number;
  dont end path names with "/" symbol, and double slashes may result */
typedef struct intrf_cx{
    //the child context
    //child and sib ar the same as left and right branches in a binary tree
    struct intrf_cx *child;
    //the sibling context (next)
    struct intrf_cx *sib;
    //the previous context (prev)
    struct intrf_cx *prev;
    //the parent context
    struct intrf_cx *parent;  
    //the name of the context
    //IMPORTANT ALL NAMES ARE UNIQUE AND HAVE THEIR PARENT NAME AFTER <__> SYMBOL, DONT USE THIS
    //FOR CX NAMES WHEN MAKING NEW CX
    char* name;
    //the short name of the cx, without parent name added
    char* short_name;
    //type of the CX
    unsigned int type;
    //this is 1 if the CX needs to be saved when saving a song or preset
    unsigned char save;
    //var to use for the ui. Can be returned and set, to have a persistent variable on the cx
    int user_int;
}CX;

typedef struct intrf_cx_main{
    struct intrf_cx list_cx;
    //the name of the file, for contexts this usually means preset file
    char *path;
}CX_MAIN;

typedef struct intrf_cx_sample{
    struct intrf_cx list_cx;
    //the name of the sample file
    char* file_path;
    //the sample unique id, that links it to the smp_data through app_data
    int id;
}CX_SAMPLE;

typedef struct intrf_cx_plugin{
    struct intrf_cx list_cx;
    //the uri of the plugin path
    char* plug_path;
    //the uri of the plugin preset
    char* preset_path;
    //the id of the plugin instance
    int id;
}CX_PLUGIN;

typedef struct intrf_cx_osc{
    struct intrf_cx list_cx;
    int id;
}CX_OSC;

//a button can hold several values if necessary, they should always be initialized at least to a NULL/0/-1
typedef struct intrf_cx_button{
    struct intrf_cx list_cx;
    char* str_val;
    unsigned char uchar_val;
    int int_val;
    int int_val_1;
}CX_BUTTON;

typedef struct intrf_cx_val{
    struct intrf_cx list_cx;
    char* val_name; //name of the parameter, that is the same as on the params container
    //this is NULL unless user creates this name in the parameter configuration file
    //this variable is not saved when saving song or presets and is only used for display purposes, when nav_get_cx_name function is called
    char* val_display_name;
    unsigned char val_type;
    int val_id;
    float float_val;
    //type of context that holds the parameter, like sampler or plugins, check appContextTypes in types.h
    unsigned char cx_type;
    //the id of the context that holds the parameter, for example the sample id on the sampler context
    int cx_id;
}CX_VAL;

//this is the struct for the app interface so that the user can interact with the architecture
typedef struct _app_intrf{
    //the app_data that holds the contexts - the foundation of the app
    APP_INFO *app_data;
    //shared dir path for samples, presets etc.
    char* shared_dir;
    //if the song file was created from a configuration. Useful to know if cx_init_cx_type function needs to decide
    //to create some parameters on a first song initialization or this is a song load and the parameters will be
    //loaded from a file
    unsigned int load_from_conf;
    //the top context that contains other contexts
    //ALWAYS HAS TO HAVE THE ROOT_CX, CURR_CX, SELECT_CX!!! Especially pay attention if a function removes cx
    //if it removes the select_cx address segmentation fault will result, dont forget to set the removed cx to new
    //one
    CX *root_cx;
    //the current context we are in
    CX *curr_cx;
    //the selected context
    CX *select_cx;
}APP_INTRF;

APP_INTRF *app_intrf_init(intrf_status_t *err_status, const char* song_path){
    //Create the app interface structure
    APP_INTRF *app_intrf = (APP_INTRF*) malloc(sizeof(APP_INTRF));
    if(!app_intrf){
        *err_status = IntrfFailedMalloc;
        return NULL;
    }
    app_intrf->shared_dir = NULL;
    app_intrf->root_cx = NULL;
    app_intrf->curr_cx = NULL;
    app_intrf->select_cx = NULL;
    app_intrf->load_from_conf = 0;
        
    /*Initialize the app info struct, that will hold the context data*/
    //everything will be empty first    
    APP_INFO *app_data = NULL;
    app_status_t app_status = 0;
    app_data = app_init(&app_status);
    app_intrf->app_data = app_data;
    //app_data failed to initialize, clean what we have so far, critical error
    if(app_status == app_failed_malloc){
        free(app_intrf);
        *err_status = AppFailedMalloc;
        return NULL;
    }    
    //smp_data failed to initialize, clean what we have so far, critical error
    if(app_status == smp_data_init_failed){
        free(app_intrf);
        *err_status = SmpFailedMalloc;
        return NULL;
    }
    //smp_jack failed to initialize, clean what we have, close
    if(app_status == smp_jack_init_failed){
        free(app_intrf);
        *err_status = SmpJackFailedMalloc;
        return NULL;
    }
    //trk_jack failed to initialize, clean what we have, close
    if(app_status == trk_jack_init_failed){
        free(app_intrf);
        *err_status = TrkJackFailedInit;
        return NULL;
    }    
    //plugin data failed to initialize
    if(app_status == plug_data_init_failed){
	free(app_intrf);
	*err_status = PlugDataFailedMalloc;
	return NULL;
    }
    //plugin jack client failed to initialize
    if(app_status == plug_jack_init_failed){
	free(app_intrf);
	*err_status = PlugJackFailedInit;
	return NULL;
    }
    
    //read the conf file if its not in this dir create it and the dir structure and the Song_01
    if(app_json_read_conf(&app_intrf->shared_dir, song_path, NULL, &app_intrf->load_from_conf, app_intrf, cx_process_from_file)!=0){
        app_intrf_close(app_intrf);
        *err_status = SongFileParseErr;
        return NULL;
    }

    //init the app_intrf members, the root_cx should be initiated by the cx_process_from_file callback
    if(app_intrf->root_cx==NULL){
	app_intrf_close(app_intrf);
	*err_status = RootMallocFailed;
	return NULL;
    }
    //root_cx should also have a child, otherwise the structure is somehow corrupted
    if(app_intrf->root_cx->child==NULL){
	app_intrf_close(app_intrf);
	*err_status = RootChildFailed;
	return NULL;
    }
    app_intrf->curr_cx = app_intrf->root_cx;
    app_intrf->select_cx = app_intrf->root_cx->child;
    //connect ports after initializing the process
    app_intrf_iterate_reset_ports(app_intrf, app_intrf->root_cx, 1);
    
    //write the song path to the log file
    CX_MAIN* root_main = (CX_MAIN*)app_intrf->root_cx;
    log_append_logfile("Opened song %s \n", root_main->path);
    
    //clap_plug_init("/usr/lib/clap/CHOWTapeModel.clap");
    return app_intrf;
}

static void cx_process_from_file(void *arg,
				 const char* cx_name, const char* parent, const char* top_name,
				 const char* attrib_names[], const char* attribs[], unsigned int attrib_size){
    //dont do anything if the name is null
    if(!cx_name)return;
    APP_INTRF *app_intrf = (APP_INTRF*) arg;
    //extract the type from the attribs
    unsigned int type = str_find_value_to_hex(attrib_names, attribs, "type", attrib_size);
    //check if the context already exists
    //BUT if the type is Val_cx_e dont check for duplicates, since when a parameter is duplicate the cx wont be created
    //but the parameter will be updated with its value anyway (for cases when parameters are created with user configuration file)
    if(app_intrf->root_cx && type != Val_cx_e){
	CX* found_cx = cx_find_name(cx_name, app_intrf->root_cx);
	if(found_cx !=NULL){
	    return;
	}
    }
    //add this context to the structure, cant be a context if there are 0 attrib values
    if(attrib_size>0)
	cx_init_cx_type(app_intrf, parent, cx_name, type, attribs, attrib_names, attrib_size);	
}
//similar to cx_process_from_file but process the parameter configuration file and create the parameters with user settings
//used as a callback in app_json_open_iterate_callback function
static void cx_process_params_from_file(void *arg,
					const char* cx_name, const char* parent, const char* top_name,
					const char* attrib_names[], const char* attribs[], unsigned int attrib_size){
    APP_INTRF *app_intrf = (APP_INTRF*) arg;
    if(!app_intrf)return;
    if(!cx_name)return;
    if(!attrib_names || !attribs)return;
    if(attrib_size <= 0) return;
    unsigned int type = str_find_value_to_hex(attrib_names, attribs, "type", attrib_size);
    if(!app_intrf->root_cx)return;
    CX* parent_cx = cx_find_name(parent, app_intrf->root_cx);
    CX* top_cx = cx_find_name(top_name, app_intrf->root_cx);
    if(!top_cx)return;
    //this cx can be in a container cx, so its name will be different to parent var, because it was created in this callback
    //in this case find the parent by val_name variable
    if(!parent_cx){
	//has to be Val_cx_e
	if((type & 0xff00) == Val_cx_e){
	    parent_cx = cx_find_with_val_name(parent, top_cx, 1);
	}
    }
    if(!parent_cx)return;

    //create new attribs and attrib_names with correct values to send to cx_init_cx_type
    const char* new_attribs[8];
    const char* new_attrib_names[8];
    
    char val_name[MAX_PARAM_CONFIG_STRING];
    snprintf(val_name, MAX_PARAM_CONFIG_STRING, "%s", cx_name);    
    new_attrib_names[0] = "val_name";
    new_attribs[0] = val_name;
    unsigned char cx_type = 0;
    int cx_id = -1;    
    if(top_cx->type == Sample_cx_e){
	CX_SAMPLE* cx_samp = (CX_SAMPLE*)top_cx;
	cx_type = Context_type_Sampler;
	cx_id = cx_samp->id;
    }
    if(top_cx->type == Plugin_cx_e){
	CX_PLUGIN* cx_plug = (CX_PLUGIN*)top_cx;
	cx_type = Context_type_Plugins;
	cx_id = cx_plug->id;
    }
    if(top_cx->type == (Main_cx_e | Trk_cx_st)){
	cx_type = Context_type_Trk;
	cx_id = 0;
    }
    if(top_cx->type == Osc_cx_e){
	CX_OSC* cx_osc = (CX_OSC*)top_cx;
	cx_type = Context_type_Synth;
	cx_id = cx_osc->id;
    }
    if(cx_id == -1)return;
    char cx_type_str[20];
    snprintf(cx_type_str, 20, "%d", cx_type);
    new_attrib_names[1] = "cx_type";
    new_attribs[1] = cx_type_str;
    char cx_id_str[20];
    snprintf(cx_id_str, 20, "%d", cx_id);
    new_attrib_names[2] = "cx_id";
    new_attribs[2] = cx_id_str;
    
    //if this is a container create it without getting any additional attributes
    if(type == (Val_cx_e | Val_Container_cx_st)){
	cx_init_cx_type(app_intrf, parent_cx->name, cx_name, type, new_attribs, new_attrib_names, 3);
	return;
    }

    int val_id = app_param_id_from_name(app_intrf->app_data, cx_type, cx_id, val_name, 0);
    if(val_id == -1)return;
    
    char* val_display_name = str_find_value_from_name(attrib_names, attribs, "display_name", attrib_size);
    float float_val = str_find_value_to_float(attrib_names, attribs, "default_val", attrib_size);
    float incr = str_find_value_to_float(attrib_names, attribs, "increment", attrib_size);
    unsigned char val_type = 0;
    app_param_get_value(app_intrf->app_data, cx_type, cx_id, val_id, &val_type, 0, 0);

    new_attrib_names[3] = "val_display_name";
    new_attribs[3] = val_display_name;
    char val_type_str[20];
    snprintf(val_type_str, 20, "%2x", val_type);
    new_attrib_names[4] = "val_type";
    new_attribs[4] = val_type_str;
    char val_id_str[40];
    snprintf(val_id_str, 40, "%2d", val_id);
    new_attrib_names[5] = "val_id";
    new_attribs[5] = val_id_str;
    char float_val_str[100];
    snprintf(float_val_str, 100, "%f", float_val);
    new_attrib_names[6] = "float_val";
    new_attribs[6] = float_val_str;
    char val_incr_str[100];
    snprintf(val_incr_str, 100, "%f", incr);
    new_attrib_names[7] = "val_incr";
    new_attribs[7] = val_incr_str;
    
    cx_init_cx_type(app_intrf, parent_cx->name, cx_name, type, new_attribs, new_attrib_names, 8);
    
    if(val_display_name)free(val_display_name);
}

static CX *cx_init_cx_type(APP_INTRF *app_intrf, const char* parent_string, const char *name, unsigned int type,
		    const char *type_attribs[], const char *type_attrib_names[], int attrib_size){
    CX *ret_node = NULL;
    CX *parent = NULL;
    if(parent_string!=NULL){
	parent = cx_find_name(parent_string, app_intrf->root_cx);
	if(parent==NULL){
	    return NULL;
	}
    }
    //if the type is the Main_cx_e
    if((type & 0xff00) == Main_cx_e){
        CX_MAIN *cx_main = (CX_MAIN*)malloc(sizeof(CX_MAIN));
        if(!cx_main)return NULL;
        ret_node = (CX*)cx_main;
        //set the members of the base cx, that we will cast to
	ret_node->save = 1;
        //add the cx node to the cx structure
        int child_add_err = cx_add_child(parent, ret_node, name, type);
        if(child_add_err<0){
            free(cx_main);
            return NULL;
        }
	//find the path attrib
	cx_main->path = NULL;
	char* path = NULL;
	//write the attributes only if there are attribs in the string array
	if(attrib_size>0){
	    //if there is no path for Main_cx_e we cant create it, since children of this cx will depend on path
	    path = str_find_value_from_name(type_attrib_names, type_attribs, "path", attrib_size);
	    if(!path){
		cx_remove_this_and_children(ret_node);
		return NULL;
	    }
	    cx_main->path = path;
	    
	}
	//if this is the Root_cx_st add it to the app_intrf
        if(type == (Main_cx_e | Root_cx_st)){
	    app_intrf->root_cx = ret_node;
	}	
	//button to load a song or preset
	if(!cx_init_cx_type(app_intrf, ret_node->name, "load", (Button_cx_e | AddList_cx_st),
			    (const char*[1]){"02"}, (const char*[1]){"uchar_val"}, 1)){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}	
	//add buttons that all Main_cx_e have
	if(!cx_init_cx_type(app_intrf, ret_node->name, "save", (Button_cx_e | Save_cx_st), NULL, NULL, 0)){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}
	//button for saving to new song
	if(!cx_init_cx_type(app_intrf, ret_node->name, "save_new", (Button_cx_e | Save_cx_st),
			    (const char*[1]){"1"}, (const char*[1]){"uchar_val"}, 1)){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}
	
	if(type == (Main_cx_e | Trk_cx_st)){
	    //create the Trk_cx_st context parameters, also check for the user parameter configuration file for this context
	    if(helper_cx_create_cx_for_default_params(app_intrf, ret_node, "Trk_param_conf.json", Context_type_Trk, 0)<0){
		cx_remove_this_and_children(ret_node);
		return NULL;
	    }
	}
        //if this is Sampler_cx_st add a AddList_cx_st button that adds cx from a chosen file
        if(type  == (Main_cx_e | Sampler_cx_st)){
            if(!cx_init_cx_type(app_intrf, ret_node->name, "add_sample", (Button_cx_e | AddList_cx_st),
				(const char*[1]){"01"}, (const char*[1]){"uchar_val"}, 1)){
		cx_remove_this_and_children(ret_node);
		return NULL;
            }  
	}
	//button to add a new plugin
        if(type == (Main_cx_e | Plugins_cx_st)){
            if(!cx_init_cx_type(app_intrf, ret_node->name, "add_plugin", (Button_cx_e | AddList_cx_st),
				(const char*[1]){"03"}, (const char*[1]){"uchar_val"}, 1)){
		cx_remove_this_and_children(ret_node);
		return NULL;
            }
	}	
	if(type == (Main_cx_e | Synth_cx_st)){
	    //load the oscillators with their names
	    int osc_num = 0;
	    const char** osc_names = app_context_return_names(app_intrf->app_data, Context_type_Synth, &osc_num);
	    if(osc_names){
		for(int i = 0; i < osc_num; i++){
		    const char* cur_name = osc_names[i];
		    if(!cur_name)continue;
		    char osc_id[20];
		    snprintf(osc_id, 20, "%d", i);
		    if(!cx_init_cx_type(app_intrf, ret_node->name, cur_name, Osc_cx_e,
					(const char*[1]){osc_id}, (const char*[1]){"id"}, 1)){
			cx_remove_this_and_children(ret_node);
			return NULL;
		    }
		}
	    }
	    free(osc_names);
	}	
        return ret_node;
    }
    if((type & 0xff00) == Osc_cx_e){
	CX_OSC* cx_osc = malloc(sizeof(CX_OSC));
	if(!cx_osc)return NULL;
	cx_osc->id = -1;
	ret_node = (CX*)cx_osc;
	ret_node->save = 1;
	ret_node->sib = NULL;
	
	if(attrib_size>0){
	    cx_osc->id = str_find_value_to_int(type_attrib_names, type_attribs, "id", attrib_size);
	}
	if(cx_osc->id < 0){
	    free(cx_osc);
	    return NULL;
	}
	//add the osc to the cx array
        if(cx_add_child(parent, ret_node, name, type)<0){
            free(cx_osc);
            return NULL;
        }
	//initialize the parameters for this Oscillator, also check the parameters user configuration file
	if(helper_cx_create_cx_for_default_params(app_intrf, ret_node, "Synth_param_conf.json", Context_type_Synth, cx_osc->id)<0){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}

	return ret_node;
    }
    //if the type is a sample
    if((type & 0xff00) == Sample_cx_e){
	CX_SAMPLE *cx_smp = (CX_SAMPLE*)malloc(sizeof(CX_SAMPLE));
	if(!cx_smp)return NULL;	
	cx_smp->file_path = NULL;
	cx_smp->id = -1;
	int init = 0;
	ret_node = (CX*)cx_smp;
	ret_node->save = 1;
	if(attrib_size>0){
	    char* path = NULL;
	    path = str_find_value_from_name(type_attrib_names, type_attribs, "file_path", attrib_size);
	    if(!path){
		free(cx_smp);
		return NULL;
	    }
	    cx_smp->file_path = path;
	}
	//create the sample on the smp_data here, if successful it will return a unique id for the sample for
	//us to interact with
	int sample_id = app_smp_sample_init(app_intrf->app_data, cx_smp->file_path, cx_smp->id);
	//if adding the sample to smp_data failed we clear this cx
	if(sample_id<0){
	    if(cx_smp->file_path)free(cx_smp->file_path);
	    free(cx_smp);
	    return NULL;
	}
	int child_add_err = -1;
	//get the name
	char* smp_name = app_return_cx_name(app_intrf->app_data, Context_type_Sampler, sample_id, 1);
	if(smp_name){
	    child_add_err = cx_add_child(parent, ret_node, smp_name, type);
	    free(smp_name);
	}
        if(child_add_err<0){
	    if(cx_smp->file_path)free(cx_smp->file_path);
            free(cx_smp);
            return NULL;
        }
	cx_smp->id = sample_id;
	
	if(!cx_init_cx_type(app_intrf, ret_node->name, "remove", (Button_cx_e | Remove_cx_st), NULL, NULL,0)){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}
	//initialize the parameters also check for configuration file for the parameters
	if(helper_cx_create_cx_for_default_params(app_intrf, ret_node, "Sampler_param_conf.json", Context_type_Sampler, cx_smp->id)<0){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}
	return ret_node;	
    }
    if((type & 0xff00) == Plugin_cx_e){
	CX_PLUGIN *cx_plug = (CX_PLUGIN*)malloc(sizeof(CX_PLUGIN));
	if(!cx_plug)return NULL;	
	cx_plug->plug_path = NULL;
	cx_plug->preset_path = NULL;
	cx_plug->id = -1;
	int init = 0;
	ret_node = (CX*)cx_plug;
	ret_node->save = 1;
	if(attrib_size>0){
	    cx_plug->plug_path = str_find_value_from_name(type_attrib_names,
							  type_attribs, "plug_path", attrib_size);
	    if(!cx_plug->plug_path){
		free(cx_plug);
		return NULL;
	    }
	    cx_plug->preset_path = str_find_value_from_name(type_attrib_names,
							    type_attribs, "preset_path", attrib_size);
	}
	//create the plugin
	int plug_id = app_plug_init_plugin(app_intrf->app_data, cx_plug->plug_path, cx_plug->id);
	//if adding the plugin to plug_data failed we clear this cx
	if(plug_id<0){
	    if(cx_plug->plug_path)free(cx_plug->plug_path);
	    free(cx_plug);
	    return NULL;
	}
	int child_add_err = -1;
	//get the name
	char* plug_name = app_return_cx_name(app_intrf->app_data, Context_type_Plugins, plug_id, 1);
	if(plug_name){
	    child_add_err = cx_add_child(parent, ret_node, plug_name, type);
	    free(plug_name);
	}
        if(child_add_err<0){
	    if(cx_plug->plug_path)free(cx_plug->plug_path);
            free(cx_plug);
            return NULL;
        }
	cx_plug->id = plug_id;
	
	if(!cx_init_cx_type(app_intrf, ret_node->name, "remove", (Button_cx_e | Remove_cx_st), NULL, NULL,0)){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}
	//load preset if there is a preset path
	if(cx_plug->preset_path)app_plug_load_preset(app_intrf->app_data, cx_plug->preset_path, cx_plug->id);
	//button to load a preset for the plugin, if we fail to create it no big deal - presets wont load
	cx_init_cx_type(app_intrf, ret_node->name, "load_preset", (Button_cx_e | AddList_cx_st),
			(const char*[1]){"06"}, (const char*[1]){"uchar_val"}, 1);

	//create the Val_cx_e for each plugin control port
	//this function also tries to find a configuration file for parameters and use user info from there
	char plugin_config_file[MAX_PARAM_CONFIG_STRING];
	plug_name = app_return_cx_name(app_intrf->app_data, Context_type_Plugins, cx_plug->id, 0);
	if(!plug_name){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}
	snprintf(plugin_config_file, MAX_PARAM_CONFIG_STRING, "%s_param_conf.json", plug_name);
	free(plug_name);
	if(helper_cx_create_cx_for_default_params(app_intrf, ret_node, plugin_config_file, Context_type_Plugins, cx_plug->id)<0){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}
	return ret_node;	
    }	    
      
    //if the type is a Button_cx_e
    if((type & 0xff00) == Button_cx_e){
        CX_BUTTON *cx_button = (CX_BUTTON*)malloc(sizeof(CX_BUTTON));
        if(!cx_button)return NULL;
	
        ret_node = (CX*)cx_button;
	ret_node->save = 0;
	
        int child_add_err = cx_add_child(parent, ret_node, name, type);
        if(child_add_err<0){
            free(cx_button);
            return NULL;     
        }
	cx_button->str_val = NULL;
	cx_button->uchar_val = 0;
	cx_button->int_val = -1;
	cx_button->int_val_1 = -1;
	if(attrib_size>0){
	    cx_button->str_val = str_find_value_from_name(type_attrib_names, type_attribs,
							  "str_val", attrib_size);
	    cx_button->uchar_val = str_find_value_to_hex(type_attrib_names, type_attribs,
							 "uchar_val", attrib_size);
	    cx_button->int_val = str_find_value_to_int(type_attrib_names, type_attribs,
							 "int_val", attrib_size);
	    cx_button->int_val_1 = str_find_value_to_int(type_attrib_names, type_attribs,
							 "int_val_1", attrib_size);	    
	}
	    
        //AddList_cx_st button subtype
        if(type == (Button_cx_e | AddList_cx_st)){
	    //this value in the AddList will be to track how deep in directory we traveled
	    //this is so, that when int_val==0 we cant travel more out, so we dont go out the initial dir
	    cx_button->int_val = 0;
	    cx_button->str_val = NULL;
	    char* cancel_name = "cancel";

	    //if this AddList_cx_st is for connecting ports
	    if(cx_button->uchar_val == Connect_audio_purp || cx_button->uchar_val == Connect_midi_purp){
		ret_node->save = 1;
		cancel_name = "clear";
	    }
	    //create a button to cancel the currently selected file and just exit if desired
	    //for ports this creates clear button to remove ports that do not appear in audio backend
	    cx_init_cx_type(app_intrf, ret_node->name, cancel_name,
			    (Button_cx_e | Cancel_cx_st), NULL, NULL, 0);    
	}
	//settings for the Port_cx_st subtype
	if(type == (Button_cx_e | Port_cx_st)){
	    if(cx_button->int_val<0)cx_button->int_val = 0;
	    if(cx_button->int_val>0 || (cx_button->uchar_val & 0x0f) == Port_type_out)ret_node->save = 1;
	}
	
        return ret_node;
    }
    //if the type is a Val_cx_e
    if((type & 0xff00) == Val_cx_e){
        CX_VAL *cx_val = (CX_VAL*)malloc(sizeof(CX_VAL));
        if(!cx_val)return NULL;
	cx_val->val_name = NULL;
	cx_val->val_display_name = NULL; //this name is not saved and only != NULL if user sets it in the parameter conf file
	cx_val->val_type = 0;
	cx_val->val_id = -1;
	cx_val->float_val = -1000;
	cx_val->cx_type = 0;
	cx_val->cx_id = -1;
	
        ret_node = (CX*)cx_val;
	ret_node->save = 1;
	ret_node->type = type;
	
	if(attrib_size>0){
	    cx_val->val_name = str_find_value_from_name(type_attrib_names, type_attribs,
							 "val_name", attrib_size);
	    cx_val->val_display_name = str_find_value_from_name(type_attrib_names, type_attribs,
							 "val_display_name", attrib_size);	    
	    cx_val->val_type = str_find_value_to_hex(type_attrib_names, type_attribs,
							 "val_type", attrib_size);
	    cx_val->val_id = str_find_value_to_int(type_attrib_names, type_attribs,
							 "val_id", attrib_size);
	    cx_val->float_val = str_find_value_to_float(type_attrib_names, type_attribs,
							   "float_val", attrib_size);
	    cx_val->cx_type = str_find_value_to_hex(type_attrib_names, type_attribs,
							   "cx_type", attrib_size);
	    cx_val->cx_id = str_find_value_to_hex(type_attrib_names, type_attribs,
							   "cx_id", attrib_size);
	    //set new increment if there is a request for that
	    float val_inc = -1;
	    val_inc = str_find_value_to_float(type_attrib_names, type_attribs, "val_incr", attrib_size);
	    if(val_inc != -1){
	        cx_set_value_callback(app_intrf, ret_node, val_inc, Operation_SetIncr);
	    }
	}	
	//if this is a Val_cx_e container add to the cx array and return
	if(type == (Val_cx_e | Val_Container_cx_st)){
	    if(cx_add_child(parent, ret_node, name, type) < 0){
		free(cx_val);
		return NULL;
	    }
	    return ret_node;
	}	

	//if cx with this val_name exists, only set the value for the parameter but dont create the cx
	//if this Val_cx_e parent has a parent search from there, since Val_cx_e can be in a container
	CX* search_duplicate_root = parent;
	if(parent->parent){
	    if(parent->type == (Val_cx_e | Val_Container_cx_st))
		search_duplicate_root = parent->parent;
	}
	CX* val_duplicate = cx_find_with_val_name(cx_val->val_name, search_duplicate_root, 1);
	if(val_duplicate){
	    //find the param id in case the order changed
	    cx_val->val_id = app_param_id_from_name(app_intrf->app_data, cx_val->cx_type, cx_val->cx_id, cx_val->val_name, 0);
	    //set value and remove this cx
	    if(cx_val->val_id != -1)cx_set_value_callback(app_intrf, ret_node, cx_val->float_val, Operation_SetValue);
	    if(cx_val->val_name)free(cx_val->val_name);
	    free(cx_val);
	    return NULL;
	}

        int child_add_err = cx_add_child(parent, ret_node, name, type);
        if(child_add_err<0){
	    if(cx_val->val_name)free(cx_val->val_name);
            free(cx_val);
            return NULL;     
        }

	//find the parameter id, sometimes what is saved in the json file can be in a different order from the parameters in params
	//here we check the id of the same name for this purpose
	cx_val->val_id = app_param_id_from_name(app_intrf->app_data, cx_val->cx_type, cx_val->cx_id, cx_val->val_name, 0);

	if(cx_val->val_id == -1){
	    cx_remove_this_and_children(ret_node);
	    return NULL;
	}
	//to init the values on the value button we call the set_value function
	cx_set_value_callback(app_intrf, ret_node, cx_val->float_val, Operation_SetValue);
        return ret_node;
    }

    return ret_node;
}

static int cx_add_child(CX *parent_cx, CX *child_cx, const char *name, unsigned int type){
    if(!child_cx){
        return -1;
    }
    //malloc for the name, we will have to free it later
    char *uniq_name = NULL;
    uniq_name = (char*)malloc((strlen(name)+1)*sizeof(char));
    if(uniq_name==NULL)return -1;
    strcpy(uniq_name, name);
    //initiate the cx members
    child_cx->name = uniq_name;
    child_cx->short_name = NULL;
    child_cx->child = NULL;
    child_cx->sib = NULL;
    child_cx->prev = NULL;
    child_cx->type = type;
    child_cx->user_int = -1;
    
    if(parent_cx ==NULL){
	child_cx->short_name = (char*)malloc((strlen(name)+1)*sizeof(char));
	if(!child_cx->short_name){
	    goto abort_and_clean;
	}
	strcpy(child_cx->short_name, name);
        child_cx->parent = NULL;
        return 0;
    }
    child_cx->parent = parent_cx;
    
    //make the child name unique, by combining with the parent name
    //but only if its not already modified and does not contain <__>
    if(strstr(child_cx->name, "<__>")==NULL){
	child_cx->short_name = (char*)malloc((strlen(name)+1)*sizeof(char));
	if(!child_cx->short_name){
	    goto abort_and_clean;
	}
	strcpy(child_cx->short_name, name);
	//get the parent short name
	if(!parent_cx->short_name){
	    goto abort_and_clean;
	}
	unsigned int parent_size = strlen(parent_cx->short_name)+5;
	child_cx->name = realloc(child_cx->name, sizeof(char)*(strlen(child_cx->name)+parent_size));
	if(!child_cx->name){
	    goto abort_and_clean;
	}
	sprintf(child_cx->name, "%s<__>%s", name, parent_cx->short_name);
    }
    else{
	char* after_string = NULL;
	child_cx->short_name = str_split_string_delim(child_cx->name, "<__>", &after_string);
	if(!child_cx->short_name){
	    if(after_string)free(after_string);
	    goto abort_and_clean;
	}
	if(after_string)free(after_string);
    }
    //if the parent does not have a child cx add this there
    if(parent_cx->child==NULL){
        parent_cx->child = child_cx;
        return 0;
    }
    //if this parent has a child, travel through the childs siblings and find one that does not have any
    //then add this cx to that context sibling
    if(parent_cx->child!=NULL){
         
        CX *cur_sib = parent_cx->child;
        CX *temp_sib = NULL;
        if(cur_sib->sib)
            temp_sib = cur_sib->sib;
        while(temp_sib!=NULL){
            cur_sib = temp_sib;
            temp_sib = temp_sib->sib;
        }
        if(cur_sib==NULL){
	    goto abort_and_clean;
	}
        cur_sib->sib = child_cx;
        child_cx->prev = cur_sib;
    }

    return 0;

abort_and_clean:
    if(child_cx->name)free(child_cx->name);
    child_cx->name = NULL;
    if(child_cx->short_name)free(child_cx->short_name);
    child_cx->short_name = NULL;    
    return -1;
}

static void cx_remove_child(CX *child_cx, int only_one, unsigned int type){
    if(child_cx != NULL){
	if(only_one==1)goto clean_one;
        cx_remove_child(child_cx->child, 0, type);
        cx_remove_child(child_cx->sib, 0, type);
    clean_one:
	//if the type is not 0 only remove cx with this type
	if(type!=0){
	    if((type & 0xffff) != child_cx->type)goto next;
	}
	//if it is the direct child of its parent, now the parent will have to have this nodes sib as a child
	if(child_cx->parent!=NULL){
	    if(strcmp(child_cx->parent->child->name, child_cx->name)==0){
		child_cx->parent->child = NULL;
		if(child_cx->sib!=NULL)
		    child_cx->parent->child = child_cx->sib;
	    }
	}
	//its sibling will become its parent child and if this node has a prev node it will become the
	//siblings prev node
	if(child_cx->sib!=NULL){
	    child_cx->sib->prev = NULL;
	    if(child_cx->prev!=NULL)
		child_cx->sib->prev = child_cx->prev;
	}
	//if this node has a previous node that nodes sibling will be null, or if this node has a sib
	//this nodes previous nodes sib will become this nodes sib
	if(child_cx->prev!=NULL){
	    child_cx->prev->sib = NULL;
	    if(child_cx->sib!=NULL)
		child_cx->prev->sib = child_cx->sib;
	}
    
	//if it has children they will be transfered to this nodes parent or severed
	//so go through all the children and set their parent to NULL if this child_cx has a parent
	//add child_cx parent to its childrens parent
	if(child_cx->child!=NULL){     
	    CX *cur_sib = child_cx->child;
	    cur_sib->parent = NULL;
	    if(child_cx->parent!=NULL){
		cur_sib->parent = child_cx->parent;
		if(strcmp(child_cx->name,child_cx->parent->child->name)==0){
		    child_cx->parent->child = cur_sib;
		}
		else{
		    //go through the child_cx parents children and connect the child_cx child to the last one
		    CX *last_child = child_cx->parent->child;
		    CX *temp_cx = last_child;
		    while(temp_cx!=NULL){
			temp_cx = temp_cx->sib;
			if(temp_cx!=NULL)
			    last_child = temp_cx;
		    }
		    last_child->sib = cur_sib;
		}
	    }
	    cur_sib = cur_sib->sib;
	    while(cur_sib!=NULL){
		cur_sib->parent = NULL;
		if(child_cx->parent!=NULL)cur_sib->parent = child_cx->parent;
		cur_sib = cur_sib->sib;
	    }
	}    

	//now that we severed the child_cx from the structure we need to clean its memory, since
	//it will not be reachable anymore
	cx_clear_contexts(child_cx, 1);
    next:
    }
}

static int cx_remove_this_and_children(CX* this_cx){
    if(!this_cx)return -1;
    //first remove all the children in this_cx
    if(this_cx->child)cx_remove_child(this_cx->child, 0, 0);
    //now remove this_cx itself
    cx_remove_child(this_cx, 1, 0);
    
    return 0;
}

static CX *cx_find_name(const char* name, CX *root_node){
    CX *ret_node = NULL;
    if(root_node != NULL) {
      if(strcmp(root_node->name, name)==0){
        ret_node = root_node;
        return ret_node;
      }
      ret_node = cx_find_name(name, root_node->child);
      if(ret_node){
          return ret_node;
      }
      ret_node = cx_find_name(name, root_node->sib);
      if(ret_node){
          return ret_node;
      }
    }

    return ret_node;
}

static void cx_find_container(CX *root_node, unsigned int uchar, CX*** cx_array, unsigned int* size){
    if(!size)return;
    if(*size<0)return;
    if(root_node != NULL) {
	if(root_node->type == (Button_cx_e | AddList_cx_st)){
	    CX_BUTTON* addList_cx = (CX_BUTTON*)root_node;
	    if(addList_cx->uchar_val == uchar){
		*size = *size + 1;
		CX** array = (CX**)malloc(sizeof(CX*)*(*size));
		if(!array)return;
		array[*size-1] = root_node;
		if(*cx_array)free(*cx_array);
		*cx_array = array;
	    }
	}
	cx_find_container(root_node->child, uchar, cx_array, size);
	cx_find_container(root_node->sib, uchar, cx_array, size);
    }
}


static CX *cx_find_with_str_val(const char* str_val, CX *root_node, int go_in){
    CX *ret_node = NULL;
    if(!root_node)return NULL;
    if((root_node->type & 0xff00) != Button_cx_e)goto next;
    CX_BUTTON* cx_button = (CX_BUTTON*)root_node;
    const char* this_str = cx_button->str_val;
    if(!this_str)goto next;
    if(strcmp(str_val, this_str)==0){
        ret_node = root_node;
        return ret_node;
    }
next:
    //if user does not want to go inside the buttons, only through siblings 
    if(go_in==0)goto next_sib;
    ret_node = cx_find_with_str_val(str_val, root_node->child, go_in);
    if(ret_node){
	return ret_node;
    }
next_sib:
    ret_node = cx_find_with_str_val(str_val, root_node->sib, go_in);
    if(ret_node){
	return ret_node;
    }

    return ret_node;
}

static CX *cx_find_with_val_name(const char* val_name, CX *root_node, int go_in){
    CX *ret_node = NULL;
    if(!root_node)return NULL;
    if((root_node->type & 0xff00) != Val_cx_e)goto next;
    CX_VAL* cx_val = (CX_VAL*)root_node;
    const char* this_val_name = cx_val->val_name;
    if(!this_val_name)goto next;
    if(strcmp(val_name, this_val_name)==0){
        ret_node = root_node;
        return ret_node;
    }
next:
    //if user does not want to go inside the buttons, only through siblings 
    if(go_in==0)goto next_sib;
    ret_node = cx_find_with_val_name(val_name, root_node->child, go_in);
    if(ret_node){
	return ret_node;
    }
next_sib:
    ret_node = cx_find_with_val_name(val_name, root_node->sib, go_in);
    if(ret_node){
	return ret_node;
    }

    return ret_node;
}


static void cx_clear_contexts(CX *root, int only_one){
    if(root != NULL){
	if(only_one==1)goto clean_one;
        cx_clear_contexts(root->child, 0);
        cx_clear_contexts(root->sib, 0);
    clean_one:
        //if the node has a name string it will be malloced and we need to free it
        if(root->name){
            free(root->name);
        }
	if(root->short_name)free(root->short_name);
        //clean the main contexts
        if((root->type & 0xff00) == Main_cx_e){
            //if the node has a path string it will be malloced and we need to free it
            if(((CX_MAIN*)root)->path){
                free(((CX_MAIN*)root)->path);
            }
        }
	//clean the sample contexts
	if((root->type & 0xff00) == Sample_cx_e){
            //if the node has a file_path string it will be malloced and we need to free it
            if(((CX_SAMPLE*)root)->file_path){
                free(((CX_SAMPLE*)root)->file_path);
            }
	}
	//clean the plugin context
	if((root->type & 0xff00) == Plugin_cx_e){
            //if the node has a file_path string it will be malloced and we need to free it
            if(((CX_PLUGIN*)root)->plug_path){
                free(((CX_PLUGIN*)root)->plug_path);
            }
	    if(((CX_PLUGIN*)root)->preset_path){
		free(((CX_PLUGIN*)root)->preset_path);
	    }
	}	
	//clean button malloced members
	if((root->type & 0xff00) == Button_cx_e){
            if(((CX_BUTTON*)root)->str_val){
                free(((CX_BUTTON*)root)->str_val);
            }	    
	}
	//clean Val_cx_e malloced members
	if((root->type & 0xff00) == Val_cx_e){
            if(((CX_VAL*)root)->val_name){
                free(((CX_VAL*)root)->val_name);
            }
	    if(((CX_VAL*)root)->val_display_name){
                free(((CX_VAL*)root)->val_display_name);
            }	    
	}	
        free(root);
    }
    
}

static int context_list_from_dir(APP_INTRF* app_intrf,
			  const char* dir_name, const char* parent_name, int depth){
    struct dirent **ep;
    int n = scandir(dir_name, &ep, NULL, alphasort);
    if(n==-1)return -1;
    while(n--){
	char full_path[500];
	strcpy(full_path, dir_name);
	strcat(full_path,"/");
	strcat(full_path, ep[n]->d_name);
	const char* attribs[1] = {full_path};
	const char* attrib_names[1] = {"str_val"};
	//if a dir is found
	if(ep[n]->d_type == DT_DIR){
	    if(strcmp(ep[n]->d_name,".")==0)goto next;
	    if(strcmp(ep[n]->d_name,"..")==0 && depth<=0)goto next;
	    CX* cx_dir = cx_init_cx_type(app_intrf, parent_name, ep[n]->d_name,
					 (Button_cx_e | Dir_cx_st), attribs, attrib_names, 1);
	}
	//if a file is found
	if(ep[n]->d_type == DT_REG){
	    CX* cx_file = cx_init_cx_type(app_intrf, parent_name, ep[n]->d_name,
					  (Button_cx_e | Item_cx_st), attribs, attrib_names, 1);
	}
    next:
	free(ep[n]);
    }
    free(ep);
    return 0;
}

static int context_list_from_string(APP_INTRF* app_intrf, const char* disp_name,
			     const char* string_name, const char* parent_name){
    if(!string_name)return -1;
    cx_init_cx_type(app_intrf, parent_name, disp_name, (Button_cx_e | Item_cx_st),
		    (const char*[1]){string_name}, (const char*[1]){"str_val"}, 1);
    return 0;
}

/*CALLBACK FUNCTIONS FOR THE CX CONTEXTS, FOR INTERNAL USE*/
/*--------------------------------------------------------*/
static float cx_get_value_callback(APP_INTRF* app_intrf, CX* self, unsigned char* ret_type){
    CX_VAL* cx_val = (CX_VAL*)self;
    float ret_val = 0;
    ret_val = app_param_get_value(app_intrf->app_data, cx_val->cx_type, cx_val->cx_id, cx_val->val_id, ret_type, 0, 0);
    if(ret_val == -1 && *ret_type == 0) return -1;
    cx_val->float_val = ret_val;

    //if this parameter has a weird curve, return the value from the curve table
    //we make this separate call because we dont want to store the curved value on the cx_val,
    //because it would be save converted to the save file, we need saved values for these parameters to be linear
    //otherwise on song load the converted value would be set on the parameter
    if((*ret_type & 0xff) == Curve_Float_Return_Type)
	ret_val = app_param_get_value(app_intrf->app_data, cx_val->cx_type, cx_val->cx_id, cx_val->val_id, ret_type, 1, 0);

    return ret_val;
}
static void cx_set_value_callback(APP_INTRF* app_intrf, CX* self, float set_to, unsigned char param_op){
    if(!app_intrf)return;
    if(self->type != Val_cx_e)return;
    CX_VAL* cx_val = (CX_VAL*)self;
    app_param_set_value(app_intrf->app_data, cx_val->cx_type, cx_val->cx_id, cx_val->val_id,
			set_to, param_op, 0);
}
//return 1 if context structure changed
static int cx_enter_remove_callback(APP_INTRF* app_intrf, CX* self){
    int ret_val = 0;
    if(self){
	if(self->parent){
	    //TODO is it ok to have curr_cx and select_cx set to the same cx? Cant think of why not bot a bit weird
	    app_intrf->curr_cx = self->parent->parent;
	    app_intrf->select_cx = self->parent->parent;	    
	    helper_cx_remove_cx_and_data(app_intrf, self->parent);
	    //reset the ports, so the ports have correct int_val values, because removing a plugin or similar
	    //can change the ports on the audio backend
	    app_intrf_iterate_reset_ports(app_intrf, app_intrf->root_cx, 1);
	    ret_val = 1;
	}
    }
    return ret_val;
}

static void cx_enter_save_callback(APP_INTRF* app_intrf, CX* self){
    JSONHANDLE* obj = NULL;
    char* cur_dir = NULL;
    if(!self->parent)goto finish;
    unsigned char preset = 0;
    CX* send_cx = self->parent;
    cur_dir = (char*)malloc(sizeof(char)+2);
    if(!cur_dir)goto finish;
    strcpy(cur_dir, ".");
    //check if we are saving song or preset
    if(self->parent->parent != NULL){
	send_cx = self->parent->child;
	preset = 1;
	//make the dir name for the preset context, like _song/sampler
	cur_dir = realloc(cur_dir, sizeof(char)*(strlen(app_intrf->shared_dir)+
						 strlen(self->parent->short_name)+2));
	if(!cur_dir)goto finish;
	sprintf(cur_dir, "%s/%s",app_intrf->shared_dir, self->parent->short_name);
    }
    CX_BUTTON* cx_button = (CX_BUTTON*)self;
    CX_MAIN* cx_main = NULL;
    //saving capabilities are only for main contexts
    if((self->parent->type & 0xff00) != Main_cx_e)goto finish;
    cx_main = (CX_MAIN*)self->parent;
    if(!cx_main->path)goto finish;
    unsigned char write_new = 0; 
    write_new = cx_button->uchar_val;
    //if we have to write to a new file
    //update the path of the context to the new path
    if(write_new==1){
	//find which next number of path we can use
	int cut_string_len = snprintf(NULL, 0, "%s-",self->parent->short_name);
	char* cut_string = (char*)malloc(sizeof(char) * (cut_string_len+1));
	if(!cut_string)goto finish;
	snprintf(cut_string, cut_string_len+1, "%s-",self->parent->short_name);
	int song_num = str_return_next_after_string(cur_dir, cut_string, preset);
	if(cut_string)free(cut_string);
	if(song_num==-1)goto finish;
	//now build the new path of the cx, this sets the cx_main->path
	if(helper_cx_build_new_path(app_intrf, self->parent, song_num)!=0)goto finish;
    }
    
    //create an empty object to build the structure in and continue if its not null
    if(app_json_create_obj(&obj)==0){
	//travel through the cx structure and call a callback, but dont go to siblings of the self->parent
	helper_cx_iterate_with_callback(app_intrf, send_cx, obj, helper_cx_prepare_for_save);
    }
    //write the json handle to the file in the context path
    app_json_write_handle_to_file(obj, cx_main->path, write_new, preset);
finish:
    if(cur_dir)free(cur_dir);
}
//returns 1 if the context structure changed
static int cx_enter_dir_callback(APP_INTRF *app_intrf, CX* self){
    CX_BUTTON* dir = (CX_BUTTON*)self;
    CX* self_parent = self->parent;
    int ret_val = 0;
    //increase the parents int_val which holds the dir travel depth
    //decrease it if the dir name is "..". This helps not to travel outside of the initial dir
    if(strcmp(self->short_name,"..")==0){
	((CX_BUTTON*)self_parent)->int_val-=1;
    }
    else{
	((CX_BUTTON*)self_parent)->int_val+=1;
    }    
    //save the strings to create the new dirs later, because we will remove this cx as well
    char* str_val = NULL;
    str_val = (char*)malloc(sizeof(char)*strlen(dir->str_val)+1);
    strcpy(str_val, dir->str_val);
    const char* parent_name = self_parent->name;    
    //clear the old context files and dirs, but dont touch any other contexts
    CX* next_cx = self->parent->child;
    if(next_cx){
	cx_remove_child(next_cx, 0, (Button_cx_e | Dir_cx_st));
	cx_remove_child(next_cx, 0, (Button_cx_e | Item_cx_st));
	ret_val = 1;
    }

    //create the new list from the dir context
    context_list_from_dir(app_intrf, str_val, parent_name, ((CX_BUTTON*)self_parent)->int_val);
    if(str_val)free(str_val);
    //select the first child of this dirs parent
    if(self_parent!=NULL){
	app_intrf->select_cx = self_parent->child;
    }
    return ret_val;
}
//returns 1 if context structure changed
static int cx_enter_port_callback(APP_INTRF* app_intrf, CX* self){
    if(!self->parent)return -1;
    int ret_val = 0;
    CX_BUTTON* cx_port = (CX_BUTTON*) self;
    unsigned int send_type = (cx_port->uchar_val & 0xf0);
    send_type = (send_type | Port_type_in);    
    //if this port is output port create input ports inside of it
    if((cx_port->uchar_val & 0x0f) == Port_type_out){
	cx_reset_ports(app_intrf, self, send_type, 1);
	ret_val = 1;
    }
    //if this is input port change the int_val and reset_ports, connecting or disconnecting ports
    else if((cx_port->uchar_val & 0x0f) == Port_type_in){
	if(cx_port->int_val <=0){
	    cx_port->int_val = 1;
	}
	else if(cx_port->int_val >= 1){
	    cx_port->int_val = 0;
	}
	cx_reset_ports(app_intrf, self->parent, send_type, 0);
    }
    return ret_val;
}

static void cx_enter_item_callback(APP_INTRF* app_intrf, CX* self){
    CX_BUTTON* cx_file = (CX_BUTTON*)self;
    if(!self->parent)return;
    CX_BUTTON* cx_addList = (CX_BUTTON*)self->parent;
    char* f_path = cx_file->str_val;
    //dont create anything if the file path is not loaded
    if(!f_path)return;
    //if this buttons purpose is to add a new sample
    if(cx_addList->uchar_val == Sample_purp){
	//create the sample only if the str_val of self and of parent are equal - meaning if this was
	//entered twice (first enter could only play the file not load it)
	//create the cx_sample, the sample on the smp_data will be created in the cx_init_cx_type
	if(cx_addList->str_val){
	    if(strcmp(cx_addList->str_val, f_path)==0){
		CX *cx_smp = cx_init_cx_type(app_intrf, self->parent->parent->name, "Smp", Sample_cx_e,
					     (const char*[2]){f_path, "1"} ,
					     (const char*[2]){"file_path", "init"}, 2);
		free(cx_addList->str_val);
		cx_addList->str_val = NULL;
	    }
	    else{
		helper_cx_copy_str_val(self, self->parent);
	    }
	}
	else{
	    helper_cx_copy_str_val(self, self->parent);
	}
    }
    if(cx_addList->uchar_val == addPlugin_purp){
	//create the cx_plugin, the plugin instance on plug_data will be created in cx_init_cx_type
	cx_init_cx_type(app_intrf, self->parent->parent->name, "Plug", Plugin_cx_e,
			(const char*[2]){f_path, "1"}, (const char*[2]){"plug_path", "init"}, 2);
    }
    if(cx_addList->uchar_val == Load_plugin_preset_purp){
	//load the plugin preset
	if(self->parent->parent){
	    if(self->parent->parent->type == Plugin_cx_e){
		CX_PLUGIN* cx_plug = (CX_PLUGIN*)self->parent->parent;
		int load_preset = app_plug_load_preset(app_intrf->app_data, f_path, cx_plug->id);
		if(load_preset >= 0){
		    //copy the preset name to preset_path of the plugin
		    cx_plug->preset_path = (char*)malloc(sizeof(char)*(strlen(f_path)+1));
		    if(cx_plug->preset_path){
			strcpy(cx_plug->preset_path, f_path);
		    }
		}
	    }
	}
    }
    //if its a button to load a song or a preset
    if(cx_addList->uchar_val == Load_purp){
	if((self->parent->parent->type & 0xff00) == Main_cx_e){
	    char* parent_name = NULL;
	    if(self->parent->parent->type != (Main_cx_e | Root_cx_st)){
		helper_cx_clear_cx(app_intrf, self->parent->parent);
		parent_name = self->parent->parent->name;
	    }
	    //if this is a Root_cx_st
	    else{
		CX* clear_cx = self->parent->parent->child;
		while(clear_cx){
		    if(clear_cx->save == 1){
			helper_cx_clear_cx(app_intrf, clear_cx);
		    }
		    clear_cx = clear_cx->sib;
		}
	    }
	    app_json_read_conf(NULL, f_path, parent_name, &app_intrf->load_from_conf, app_intrf, cx_process_from_file);
	    CX_MAIN* cx_main = (CX_MAIN*)self->parent->parent;
	    if(cx_main->path)free(cx_main->path);
	    cx_main->path =  (char*)malloc(sizeof(char)*strlen(f_path)+1);
	    strcpy(cx_main->path, f_path);
	}
    }
    //reset the ports after loading a preset, plugin or sample, because some ports might become available
    //and we might make some connections
    app_intrf_iterate_reset_ports(app_intrf, app_intrf->root_cx, 1);
}
//returns 1 if the context structure changed
static int cx_enter_cancel_callback(APP_INTRF* app_intrf, CX* self){
    CX* parent = self->parent;
    int ret_val = 0;
    if(parent!=NULL){
	if(parent->type == (Button_cx_e | AddList_cx_st)){
	    CX_BUTTON* cx_parent = (CX_BUTTON*)parent;
	    //clean the addFile contents
	    if(self->parent->child){
		//remove the children (including the files and dirs) inside the addFile node
		cx_remove_child(self->parent->child, 0, (Button_cx_e | Item_cx_st));
		cx_remove_child(self->parent->child, 0, (Button_cx_e | Dir_cx_st));
		ret_val = 1;
		//reset the dir depth counter
		cx_parent->int_val = 0;
	    }	    
	    if(cx_parent->str_val!=NULL){
		free(cx_parent->str_val);
		cx_parent->str_val = NULL;
	    }

	    //if this list holds ports clear the ports that only exist on the cx structure
	    if(cx_parent->uchar_val == Connect_audio_purp || cx_parent->uchar_val == Connect_midi_purp){
		CX* cx_port = parent->child;
		CX* temp_port = cx_port;
		while(cx_port){
		    int is_nan = 0;
		    if(cx_port->type != (Button_cx_e | Port_cx_st))goto next_cx_port;
		    is_nan = helper_cx_is_port_nan(app_intrf, cx_port);
		next_cx_port:
		    temp_port = cx_port;
		    cx_port = cx_port->sib;
		    if(is_nan==1)cx_remove_this_and_children(temp_port);
		}
		ret_val = 1;
	    }
	}
    }
    return ret_val;
}

static void cx_enter_AddList_callback(APP_INTRF *app_intrf, CX* self){
    CX_BUTTON* cx_addList = (CX_BUTTON*)self;
    //this button has a purpose of loading and connecting ports
    if(cx_addList->uchar_val == Connect_audio_purp){
	cx_reset_ports(app_intrf, self, (Port_type_out | Port_type_audio), 1);
    }
    if(cx_addList->uchar_val == Connect_midi_purp){
	cx_reset_ports(app_intrf, self, (Port_type_out | Port_type_midi), 1);
    }    
    //add a new plugin in the plugin host or a plugin preset, we list the available plugins as strings,
    //not actual files in this case, add cx of a plugin or preset only if there is no cx with the same str_val already
    if(cx_addList->uchar_val == addPlugin_purp || cx_addList->uchar_val == Load_plugin_preset_purp){
	char** names = NULL;
	//if list plugin strings
	if(cx_addList->uchar_val == addPlugin_purp){
	    names = app_plug_get_plugin_names(app_intrf->app_data);
	}
	//if list plugin preset names
	else if(cx_addList->uchar_val == Load_plugin_preset_purp){
	    if(self->parent){
		if(self->parent->type == Plugin_cx_e){
		    CX_PLUGIN* plugin = (CX_PLUGIN*)self->parent;
		    unsigned int plug_id = plugin->id;
		    names = app_plug_get_plugin_presets(app_intrf->app_data, plug_id);
		}
	    }
	}
	if(names){
	    char* cur_name = names[0];
	    int iter = 0;
	    while(cur_name){
		if(cx_find_with_str_val(cur_name, self->child, 1))goto next_name;
		char* disp_name = str_return_file_from_path(cur_name);
		//disp_name_with_iter is here so that the display name for the plugin or its preset item in the list is unique
		//this does not influence the actual name of the plugin, the name and what to load will be determined from cur_name
		char* disp_name_with_iter = NULL;
		//+10 next to disp_name because there can be a lot of plugins
		if(disp_name)disp_name_with_iter = (char*)malloc(sizeof(char)*(strlen(disp_name)+10));
		if(disp_name_with_iter){
		    sprintf(disp_name_with_iter, "%.1d_", iter);
		    strcat(disp_name_with_iter, disp_name);
		    if(disp_name)free(disp_name);		    
		}
		if(disp_name_with_iter)context_list_from_string(app_intrf, disp_name_with_iter, cur_name, self->name);
		if(!disp_name_with_iter)context_list_from_string(app_intrf, cur_name, cur_name, self->name);
		if(disp_name_with_iter)free(disp_name_with_iter);
		
	    next_name:
		free(cur_name);		
		iter+=1;
		cur_name = names[iter];
	    }
	    free(names);
	}    
    }    
    //if this list is already filled do not fill it again (when filled it will have more children, when empty -
    //only the cancel button. This is useful for AddList_cx_st that lets choose files
    if(self->child->sib)goto finish;
    //create a sample from file if the parent type is Sample_cx_e
    if(cx_addList->uchar_val == Sample_purp){
	if(app_intrf->shared_dir!=NULL)
	    context_list_from_dir(app_intrf, app_intrf->shared_dir, self->name, 0);
    }
    //if this button has a purpose of loading presets or songs
    if(cx_addList->uchar_val == Load_purp){
	if(!self->parent)goto finish;
	//saving and loading is only for the Main_cx_e contexts
	if((self->parent->type & 0xff00) == Main_cx_e){
	    CX_MAIN* cx_main = (CX_MAIN*)self->parent;
	    if(!cx_main->path)goto finish;
	    char* dir = NULL;
	    dir = str_return_dir_without_file(cx_main->path);
	    if(self->parent->type == (Main_cx_e | Root_cx_st)){
		if(dir)free(dir);
		dir = (char*)malloc(sizeof(char)*2);
		if(!dir)goto finish;
		strcpy(dir, ".");
	    }
	    if(dir){
		context_list_from_dir(app_intrf, dir, self->name, 0);
		free(dir);
	    }
	}
    }   
finish:
}
static void cx_exit_AddList_callback(APP_INTRF* app_intrf, CX* self){
    if(!app_intrf)goto finish;
    CX_BUTTON* cx_addList = (CX_BUTTON*) self;
    if(!cx_addList)goto finish;
    if(cx_addList->str_val){
	free(cx_addList->str_val);
	cx_addList->str_val = NULL;
    }
finish:
}
//exit the main context
static void cx_exit_app_callback(APP_INTRF *app_intrf, CX* self){
     app_intrf_close(app_intrf);
}

/*THESE FUNCTIONS FILTER WHICH CALLBACK TO CALL*/
static void intrf_callback_set_value(APP_INTRF* app_intrf, CX* self, int set_to){
    unsigned int cur_type = self->type;    
    //set_value callback for value buttons
    if(cur_type == Val_cx_e){
	unsigned char param_op = Operation_Increase;
	if(set_to<0)param_op = Operation_Decrease;
	cx_set_value_callback(app_intrf, self, abs(set_to), param_op);
    }
}
static float intrf_callback_get_value(APP_INTRF* app_intrf, CX* self, unsigned char* ret_type){
    unsigned int cur_type = self->type;    
    float ret_val = -1;
    //get_value callback for value buttons
    if(cur_type == Val_cx_e){
	ret_val = cx_get_value_callback(app_intrf, self, ret_type);
    }
    return ret_val;
}
//if invoke returns 1, it means that the context structure was modified (for example nodes deleted
//when cancel button was pressed)
static int intrf_callback_enter(APP_INTRF* app_intrf, CX* self){
    unsigned int cur_type = self->type;
    int ret_val = 0;
    //enter for Button_cx_e
    if((cur_type & 0xff00)== Button_cx_e){
	if(cur_type == (Button_cx_e | Dir_cx_st))
	    ret_val = cx_enter_dir_callback(app_intrf, self);
	if(cur_type == (Button_cx_e | Item_cx_st))
	    cx_enter_item_callback(app_intrf, self);
	if(cur_type == (Button_cx_e | AddList_cx_st))
	    cx_enter_AddList_callback(app_intrf, self);
	if(cur_type == (Button_cx_e | Cancel_cx_st))
	    ret_val = cx_enter_cancel_callback(app_intrf, self);
	if(cur_type == (Button_cx_e | Save_cx_st))
	    cx_enter_save_callback(app_intrf, self);
	if(cur_type == (Button_cx_e | Remove_cx_st))
	    ret_val = cx_enter_remove_callback(app_intrf, self);
	if(cur_type == (Button_cx_e | Port_cx_st))
	    ret_val = cx_enter_port_callback(app_intrf, self);
    }
    //if this is a parameter change its value to default value
    if(cur_type == Val_cx_e){
	cx_set_value_callback(app_intrf, self, 0, Operation_DefValue);
    }
    return ret_val;
}
static void intrf_callback_exit(APP_INTRF* app_intrf, CX* self){
    unsigned int cur_type = self->type;
    //AddList_cx_st exit
    if(cur_type == (Button_cx_e | AddList_cx_st)){
	cx_exit_AddList_callback(app_intrf, self);
    }
    //exit for Main_cx_e
    if((cur_type & 0xff00) == Main_cx_e){
	if(cur_type == (Main_cx_e | Root_cx_st))
	    cx_exit_app_callback(app_intrf, self);
    }
}

/*FUNCTIONS TO NAVIGATE THE CX STRUCTURE, THEY ARE FOR THE USER*/
/*--------------------------------------------------------*/
int nav_return_curr_numchildren(APP_INTRF* app_intrf, CX* this_cx){
    if(!app_intrf)return -1;
    if(!this_cx)return -1;
    CX* next_cx = this_cx->child;
    if(!next_cx)return 0;
    int total = 0;
    while(next_cx){
	total +=1;
	next_cx = next_cx->sib;
    }
    return total;
}

CX** nav_return_children(APP_INTRF* app_intrf, CX* this_cx, unsigned int* children_num, unsigned int ret_type){
    if(!app_intrf)return NULL;
    if(!this_cx)return NULL;
    CX* next_cx = this_cx->child;
    if(!next_cx)return NULL;
    int total = 0;
    CX** cx_array = (CX**)malloc(sizeof(CX*));
    if(!cx_array)return NULL;    
    while(next_cx){
	if(ret_type==1){
	    //dont count the buttons like save, cancel etc.
	    if(next_cx->type == (Button_cx_e | Save_cx_st) || next_cx->type == (Button_cx_e | Cancel_cx_st)
	       || next_cx->type == (Button_cx_e | Remove_cx_st)
	       || next_cx->type == (Button_cx_e | AddList_cx_st))goto next;
	}
	if(ret_type==2){
	    //only return the buttons like save, cancel etc.
	    if(next_cx->type != (Button_cx_e | Save_cx_st) && next_cx->type != (Button_cx_e | Cancel_cx_st)
	       && next_cx->type != (Button_cx_e | Remove_cx_st)
	       && next_cx->type != (Button_cx_e | AddList_cx_st))goto next;
	}	
	CX** temp_array = realloc(cx_array, sizeof(CX*)*(total+1));
	if(temp_array){
	    cx_array = temp_array;
	    cx_array[total] = next_cx;
	    total +=1;
	}
    next:
	next_cx = next_cx->sib;
    }
    //if this_cx is a sampler or plugin or oscillator return connectAudio and connectMidi(for plugin) output if buttons are requested
    if(ret_type==2){
	if(this_cx->type == (Main_cx_e | Sampler_cx_st) || (this_cx->type & 0xff00) == Plugin_cx_e || (this_cx->type & 0xff00) == Osc_cx_e){
	    CX** found_array = (CX**)malloc(sizeof(CX*));
	    unsigned int size = 0;
	    found_array[0] = NULL;
	    if(found_array)cx_find_container(app_intrf->root_cx, Connect_audio_purp, &found_array, &size);
	    if(found_array){
		for(int i = 0; i<size; i++){
		    CX* cur_container = found_array[i];
		    if(cur_container){
			CX** temp_array = realloc(cx_array, sizeof(CX*)*(total+1));
			if(temp_array){
			    cx_array = temp_array;
			    cx_array[total] = cur_container;
			    total += 1;
			}
		    }
		}
	    }
	    if(found_array)free(found_array);
	}
	if((this_cx->type & 0xff00) == Plugin_cx_e){
	    CX** found_array = (CX**)malloc(sizeof(CX*));
	    unsigned int size = 0;
	    found_array[0] = NULL;
	    if(found_array)cx_find_container(app_intrf->root_cx, Connect_midi_purp, &found_array, &size);
	    if(found_array){
		for(int i = 0; i<size; i++){
		    CX* cur_container = found_array[i];
		    if(cur_container){
			CX** temp_array = realloc(cx_array, sizeof(CX*)*(total+1));
			if(temp_array){
			    cx_array = temp_array;
			    cx_array[total] = cur_container;
			    total += 1;
			}
		    }
		}
	    }
	    if(found_array)free(found_array);
	}
    }
    
    if(children_num)*children_num = total;
    return cx_array;    
}

unsigned int nav_return_need_to_highlight(CX* this_cx){
    if(!this_cx)return -1;
    //if this is a port cx, we need to highlight the connected input ports
    if(this_cx->type == (Button_cx_e | Port_cx_st)){
	CX_BUTTON* cx_port = (CX_BUTTON*)this_cx;
	if((cx_port->uchar_val & 0x0f) == Port_type_in && cx_port->int_val == 1)return 1;
	//if the port is output and int_val_1 is 0 it means this port is not for this context - we can
	//hide or similar in the ui
	if((cx_port->uchar_val & 0x0f) == Port_type_out && cx_port->int_val_1 == 0)return 2;
    }

    return 0;
}

int nav_set_cx_user_int(CX* this_cx, int new_int){
    if(!this_cx) return -1;
    this_cx->user_int = new_int;
    return 0;
}
int nav_return_cx_user_int(CX* this_cx){
    if(!this_cx) return -1;
    return this_cx->user_int;
}

unsigned int nav_return_cx_type(CX* this_cx){
    if(!this_cx)return -1;
    return this_cx->type;
}

//navigating out from the context
int nav_exit_cur_context(APP_INTRF *app_intrf){
    //there is no current context node, something went wrong
    if(!app_intrf->curr_cx){
        return -1;
    }
    int return_val = 0;
    CX *cur_cx = app_intrf->curr_cx;   
    if(cur_cx->parent){
        app_intrf->curr_cx = cur_cx->parent;
        app_intrf->select_cx = cur_cx;  
    }
    else{
	return_val = -2;
    }
    intrf_callback_exit(app_intrf, cur_cx);
    return return_val;
}
//changed_structure will be 1 if the cx structure changed after the invoke
CX* nav_invoke_cx(APP_INTRF* app_intrf, CX* select_cx, int* changed_structure){
    if(!app_intrf)return NULL;
    if(!select_cx)return NULL;

    *changed_structure = intrf_callback_enter(app_intrf, select_cx);
    
    return app_intrf->select_cx;
}

int nav_enter_cx(APP_INTRF* app_intrf, CX* select_cx){
    if(!app_intrf)return -1;
    if(!select_cx)return -1;
    //if this cx does not have children set current cx to its parent and select_cx to this cx
    if(!select_cx->child){
	if(select_cx->parent)
	    app_intrf->curr_cx = select_cx->parent;
	app_intrf->select_cx = select_cx;
	//if this is an empty Val_cx_e container remove it from the cx array
	if(select_cx->type == (Val_cx_e | Val_Container_cx_st)){
	    cx_remove_this_and_children(select_cx);
	    app_intrf->select_cx = app_intrf->curr_cx->child;
	    if(!app_intrf->select_cx)app_intrf->select_cx = app_intrf->curr_cx;
	}	
    }
    else{
        app_intrf->curr_cx = select_cx;
        app_intrf->select_cx = select_cx->child;
    }
    return 0;
}

//set the selected cx
int nav_set_select_cx(APP_INTRF* app_intrf, CX* sel_cx){
    if(!app_intrf)return -1;
    if(!sel_cx)return -1;
    app_intrf->select_cx = sel_cx;

    return 0;
}

//navigating to the next context, go to sib until NULL basically
int nav_next_context(APP_INTRF *app_intrf){
    //there is no current context node, something went wrong
    if(!app_intrf->select_cx){
        return -1;
    }
    
    CX *select_cx = app_intrf->select_cx;
    CX *ret_cx = NULL;
    ret_cx = select_cx->sib;
    //if we ran out of siblings, just go to the start, which is the current selected contexts parents, child
    if(ret_cx==NULL)
        ret_cx = select_cx->parent->child;
    app_intrf->select_cx = ret_cx;
    return 0;
}

//navigating to the prev context, go to prev until NULL
int nav_prev_context(APP_INTRF *app_intrf){
    //there is no current context node, something went wrong
    if(!app_intrf->select_cx){
        return -1;
    }
    
    CX *select_cx = app_intrf->select_cx;
    CX *ret_cx = NULL;
    ret_cx = select_cx->prev;
    //if we ran out of siblings, just go to the end
    if(ret_cx==NULL){
        //go till we reach the sibling NULL
        CX *temp_cx = select_cx;
        while(temp_cx!=NULL){
            ret_cx = temp_cx;
            temp_cx = temp_cx->sib;
        }
    }
    app_intrf->select_cx = ret_cx;
    return 0;
}

CX* nav_ret_root_cx(APP_INTRF* app_intrf){
    if(!app_intrf)return NULL;
    CX* ret_cx = NULL;
    if(app_intrf->root_cx!=NULL)
	ret_cx = app_intrf->root_cx;

    return ret_cx;
}

CX* nav_ret_curr_cx(APP_INTRF* app_intrf){
    if(!app_intrf)return NULL;
    CX* ret_cx = NULL;
    if(app_intrf->curr_cx!=NULL)
	ret_cx = app_intrf->curr_cx;

    return ret_cx;
}

CX* nav_ret_select_cx(APP_INTRF* app_intrf){
    CX* ret_cx = NULL;
    if(app_intrf->select_cx!=NULL)
	ret_cx = app_intrf->select_cx;

    return ret_cx;
}

float nav_get_cx_value(APP_INTRF* app_intrf, CX* sel_cx, unsigned char* ret_type){
    float ret_val = -1;
    if(sel_cx==NULL)return ret_val;
    ret_val = intrf_callback_get_value(app_intrf, sel_cx, ret_type);
    return ret_val;
}

int nav_get_cx_value_as_string(APP_INTRF* app_intrf, CX* sel_cx, char* ret_string, int name_len){
    if(sel_cx==NULL)return -1;
    unsigned char ret_type = 0;
    float cx_val = intrf_callback_get_value(app_intrf, sel_cx, &ret_type);
    if(cx_val == -1 && ret_type == 0)return -1;
    CX_VAL* param_cx = NULL;
    if(sel_cx->type == Val_cx_e)param_cx = (CX_VAL*) sel_cx;
    if(!param_cx)return -1;
    if(name_len>0){
	if((ret_type & 0xff) == Uchar_type)snprintf(ret_string, name_len, "%02X", (unsigned int) cx_val);
	if((ret_type & 0xff) == Int_type)snprintf(ret_string, name_len, "%d", (int) cx_val);
	if((ret_type & 0xff) == Float_type)snprintf(ret_string, name_len, "%g", cx_val);
	if((ret_type & 0xff) == DB_Return_Type)snprintf(ret_string, name_len, "%0.2fDB", log10((double)cx_val) * 20);
	if((ret_type & 0xff) == Curve_Float_Return_Type)snprintf(ret_string, name_len, "%g", cx_val);
	if((ret_type & 0xff) == String_Return_Type && param_cx != NULL){
	    const char* param_string = app_param_get_string(app_intrf->app_data, param_cx->cx_type, param_cx->cx_id, param_cx->val_id, 0);
	    if(param_string == NULL){
		return -1;
	    }
	    snprintf(ret_string, name_len, "%s", param_string);
	}
    }
    return 0;
}

int nav_set_cx_value(APP_INTRF* app_intrf, CX* select_cx, int set_to){
    if(!select_cx){
        return -1;
    }
    intrf_callback_set_value(app_intrf, select_cx, set_to);
    return 0;
}

int nav_update_params(APP_INTRF* app_intrf){
    if(!app_intrf)return -1;
    if(!app_intrf->app_data)return -1;
    return app_update_ui_params(app_intrf->app_data);
}

const char* nav_get_cx_name(APP_INTRF* app_intrf, CX* select_cx){
    const char* ret_string = NULL;
    if(!select_cx)return NULL;
    if(!select_cx->short_name)return NULL;
    ret_string = select_cx->short_name;
    //if this is a parameter user potentialy can set its name in parameter configuration file
    //in that case val_display_name != NULL, so return this name to show on the ui
    if(select_cx->type == Val_cx_e){
	CX_VAL* cx_val = (CX_VAL*)select_cx;
	if(!cx_val)return ret_string;
	if(cx_val->val_display_name){
	    ret_string = cx_val->val_display_name;
	}
    }
    return ret_string;
}

static int app_intrf_close(APP_INTRF *app_intrf){
    if(!app_intrf)return -1;
    //clean the whole app context and contexts owned by it
    //this function is in app_data.c
    clean_memory(app_intrf->app_data);
    
    //clean the app_intrf struct itself
    if(app_intrf->root_cx)cx_remove_child(app_intrf->root_cx, 0, 0);
    //free malloced members of app_intrf
    if(app_intrf->shared_dir)free(app_intrf->shared_dir);

    //free the app_intrf itself
    free(app_intrf);

    return 0;
}

int app_intrf_iterate_reset_ports(APP_INTRF* app_intrf, CX* top_cx, unsigned int create){
    if(!app_intrf)return -1;
    if(!top_cx)return -1;
    //this might be a container for ports
    if(top_cx->type == (Button_cx_e | AddList_cx_st)){
	CX_BUTTON* cx_addList = (CX_BUTTON*)top_cx;
	//this is a container for output ports, check what kind and call the reset ports function
	if(cx_addList->uchar_val == Connect_audio_purp){
	    cx_reset_ports(app_intrf, top_cx, (Port_type_out | Port_type_audio), 1);
	}
	if(cx_addList->uchar_val == Connect_midi_purp){
	    cx_reset_ports(app_intrf, top_cx, (Port_type_out | Port_type_midi), 1);
	}    
    }
    
    app_intrf_iterate_reset_ports(app_intrf, top_cx->sib, create);
    app_intrf_iterate_reset_ports(app_intrf, top_cx->child, create);
    
    return 0;
}

static int cx_reset_ports(APP_INTRF* app_intrf, CX* port_parent, unsigned int type, unsigned int create){
    if(!app_intrf)return -1;
    if(!port_parent)return -1;
    CX_BUTTON* port_contain = (CX_BUTTON*)port_parent;

    //first disconnect all ports
    unsigned int midi = 0;
    if((type & 0xf0) == Port_type_midi)midi = 1;
    helper_cx_disconnect_ports(app_intrf, port_parent, 0);
    //return only output ports, since we connect output to input
    unsigned int in_out = 1;
    if((type & 0x0f) == Port_type_out) in_out = 2;
    if(create == 1){
	//create the cx ports, but only if no ports exist with this str_val already
	const char** ports = app_return_ports(app_intrf->app_data, NULL, midi, in_out);
	if(!ports)return -1;
	if(ports){
	    const char* port = ports[0];
	    int iter = 0;
	    char type_str[12];
	    snprintf(type_str, 12, "%.2x", (unsigned int)type);
	    while(port){
		if(!port_parent->child){
		    cx_init_cx_type(app_intrf, port_parent->name, port, (Button_cx_e | Port_cx_st),
				    (const char*[2]){port, type_str}, (const char*[2]){"str_val", "uchar_val"}, 2);
		}
		else if(!cx_find_with_str_val(port, port_parent->child, 0)){
		    cx_init_cx_type(app_intrf, port_parent->name, port, (Button_cx_e | Port_cx_st),
				    (const char*[2]){port, type_str}, (const char*[2]){"str_val", "uchar_val"}, 2);
		}
	    
		iter+=1;
		port = ports[iter];
	    }
	    free(ports);
	}
    }
    //connect the ports inside the AddList or the current output port
    helper_cx_connect_ports(app_intrf, port_parent);
    
    return 0;
}
/*HELPER FUNCTIONS FOR MUNDAIN CX MANIPULATION*/
static int helper_cx_is_port_nan(APP_INTRF* app_intrf, CX* cx_port){
    if(!cx_port->parent)return -1;
    if(cx_port->type != (Button_cx_e | Port_cx_st))return -1;
    if(cx_port->parent->type != (Button_cx_e | AddList_cx_st))return -1;
    CX_BUTTON* cx_parent = (CX_BUTTON*)cx_port->parent;
    unsigned int midi = 0;
    if(cx_parent->uchar_val == Connect_midi_purp)midi = 1;
    const char** ports = app_return_ports(app_intrf->app_data, NULL, midi, 2);
    if(!ports)return -1;
    unsigned int iter = 0;
    unsigned int found = 0;
    CX_BUTTON* cur_port = (CX_BUTTON*)cx_port;
    if(!cur_port->str_val)return -1;
    const char* port = ports[0];
    int return_val = 0;
    while(port){
	if(strcmp(port, cur_port->str_val)==0){
	    found = 1;
	    break;
	}
	iter +=1;
	port = ports[iter];
    }
    if(found==0)return_val = 1;
    free(ports);

    return return_val;
}

static int helper_cx_disconnect_ports(APP_INTRF* app_intrf, CX* top_cx, unsigned int disc_all){
    if(!top_cx)return -1;
    CX* next_cx = top_cx;
    if(top_cx->type == (Button_cx_e | AddList_cx_st))next_cx = top_cx->child;
    while(next_cx){
	if(next_cx->type != (Button_cx_e | Port_cx_st))goto next;
	if(!next_cx->child)goto next;
	CX_BUTTON* parent_port = (CX_BUTTON*)next_cx;
	if(!parent_port->str_val)goto next;
	//reset the int_val of the port, because it will be connected if its children have a int_val == 1
	//in the connect function
	parent_port->int_val = 0;	
	CX* child_cx = next_cx->child;
	while(child_cx){
	    if(child_cx->type != (Button_cx_e | Port_cx_st))goto next_child;    
	    CX_BUTTON* child_port = (CX_BUTTON*) child_cx;
	    if(child_port->int_val>0 && disc_all == 0)goto next_child;
	    if(!child_port->str_val)goto next_child;
	    int con_err = app_disconnect_ports(app_intrf->app_data, parent_port->str_val, child_port->str_val);
	    //if connection is a success increase int_val that holds how many connections the port has
	    if(con_err == 0){
		if(parent_port->int_val<=0){
		    parent_port->int_val = 0;
		}
		if(disc_all==1)child_port->int_val = 0;
		child_cx->save = 0;
	    }
	next_child:
	    child_cx = child_cx->sib;
	}
    next:
	//if the top cx is a port dont go through its siblings just disconnect this ports ports
	if(top_cx->type == (Button_cx_e | Port_cx_st)){
	    next_cx = NULL;
	}
	else{
	//if the top cx is AddList go through the siblings, since we need to disconnect all ports
	    next_cx = next_cx->sib;
	}	
    }
    
    return 0;
}

static int helper_cx_connect_ports(APP_INTRF* app_intrf, CX* top_cx){
    if(!top_cx)return -1;
    CX* next_cx = top_cx;
    if(top_cx->type == (Button_cx_e | AddList_cx_st))next_cx = top_cx->child;
    const char** this_cx_port_names = NULL;
    //how many ports on this cx if its a plugin or sampler
    unsigned int this_port_num = 0;
    //if this sampler get the sampler port names
    if(app_intrf->curr_cx->type == (Main_cx_e | Sampler_cx_st)){
	this_cx_port_names = app_return_context_ports(app_intrf->app_data, &this_port_num, Context_type_Sampler, 0);
    }
    //if this is a plugin get port names for this plugin
    if(app_intrf->curr_cx->type == Plugin_cx_e){
	CX_PLUGIN* cur_plugin = (CX_PLUGIN*)app_intrf->curr_cx;
	this_cx_port_names = app_return_context_ports(app_intrf->app_data, &this_port_num, Context_type_Plugins, cur_plugin->id);
    }
    //if this is an oscillator get port names for this oscillator
    if(app_intrf->curr_cx->type == Osc_cx_e){
	CX_OSC* cur_osc = (CX_OSC*)app_intrf->curr_cx;
	this_cx_port_names = app_return_context_ports(app_intrf->app_data, &this_port_num, Context_type_Synth, cur_osc->id);
    }
    while(next_cx){
	if(next_cx->type != (Button_cx_e | Port_cx_st))goto next;
	CX_BUTTON* parent_port = (CX_BUTTON*)next_cx;
	if(!parent_port->str_val)goto next;	
	//if there is no this_cx_port_names list set int_val_1 to 1 for all ports so they are not hidden
	//we do it before checking if this port has children because when just created output ports will not have
	//children, until user invokes the port (goes inside of it)
	parent_port->int_val_1 = 1;
	if(this_cx_port_names && this_port_num>0){
	    unsigned int found_port_name = 0;
	    for(int i=0; i<this_port_num; i++){
		const char* cur_name = this_cx_port_names[i];
		if(!cur_name)continue;
		if(strcmp(cur_name, parent_port->str_val)==0){
		    found_port_name = 1;
		}
	    }
	    if(found_port_name==0)parent_port->int_val_1 = 0;
	}

	if(!next_cx->child)goto next;
	//reset the int_val of the port, because it will be connected if its children have a int_val == 1
	parent_port->int_val = 0;
	CX* child_cx = next_cx->child;
	while(child_cx){
	    if(child_cx->type != (Button_cx_e | Port_cx_st))goto next_child;    
	    CX_BUTTON* child_port = (CX_BUTTON*) child_cx;
	    if(child_port->int_val!=1)goto next_child;
	    if(!child_port->str_val)goto next_child;
	    int con_err = app_connect_ports(app_intrf->app_data, parent_port->str_val, child_port->str_val);
	    //if connection is a success increase int_val that holds how many connections the port has
	    if(con_err == 0 || con_err == EEXIST){
		parent_port->int_val +=1;
		child_cx->save = 1;
	    }

	next_child:
	    child_cx = child_cx->sib;
	}
    next:
	//if the top cx is a port dont go through its siblings just connect this ports ports
	if(top_cx->type == (Button_cx_e | Port_cx_st)){
	    next_cx = NULL;
	}
	else{
	//if the top cx is AddList go through the siblings, since we need to connect all ports
	    next_cx = next_cx->sib;
	}
    }

    if(this_cx_port_names)free(this_cx_port_names);
    
    return 0;
}

static void helper_cx_prepare_for_param_conf(void* arg, APP_INTRF* app_intrf, CX* top_cx){
    if(top_cx->type != Val_cx_e) return;
    //parameter cant be without a parent
    if(!top_cx->parent) return;
    const char* attrib_names[MAX_ATTRIB_ARRAY] = {NULL};
    const char* attrib_vals[MAX_ATTRIB_ARRAY] = {NULL};
    
    char type_string[12];
    snprintf(type_string, 12, "%.4x", top_cx->type);
    attrib_names[0] = "type";
    attrib_vals[0] = type_string;
    
    CX_VAL* cx_val = (CX_VAL*)top_cx;
    //for default configuration the name and display_name are the same
    //user can change the display_name
    attrib_names[1] = "display_name";
    attrib_vals[1] = cx_val->val_name;

    attrib_names[2] = "default_val";
    char default_val[100];
    snprintf(default_val, 100, "%g", cx_val->float_val);
    attrib_vals[2] = default_val;

    attrib_names[3] = "increment";
    char incr_val[100];
    snprintf(incr_val, 100, "%g", app_param_get_increment(app_intrf->app_data, cx_val->cx_type, cx_val->cx_id, cx_val->val_id, 0));
    attrib_vals[3] = incr_val;
    app_json_write_json_callback(arg, cx_val->val_name, NULL, NULL, attrib_names, attrib_vals, 4);
}

static void helper_cx_prepare_for_save(void* arg, APP_INTRF* app_intrf, CX* top_cx){
    //dont save if no need to save the node
    if(top_cx->save != 1)return;
    const char* parent_name = NULL;
    const char* attrib_names[MAX_ATTRIB_ARRAY] = {NULL};
    const char* attrib_vals[MAX_ATTRIB_ARRAY] = {NULL};
    //all cx should have a type
    char type_string[12];
    snprintf(type_string, 12, "%.4x", top_cx->type);
    attrib_names[0] = "type";
    attrib_vals[0] = type_string;
    unsigned int iter = 1;
    //add parent name if the cx has a parent (parent should be NULL only for the root cx)
    if(top_cx->parent) parent_name = top_cx->parent->name;
    //if the cx is of type CX_MAIN
    if((top_cx->type & 0xff00) == Main_cx_e){
	CX_MAIN* cx_main = (CX_MAIN*)top_cx;
	attrib_names[1] = "path";
	attrib_vals[1] = cx_main->path;
	iter = 2;
    }
    if((top_cx->type & 0xff00) == Sample_cx_e){
	CX_SAMPLE* cx_smp = (CX_SAMPLE*)top_cx;
	attrib_names[1] = "file_path";
	attrib_vals[1] = cx_smp->file_path;
	    
	attrib_names[2] = "id";
	char id_string[20];
	snprintf(id_string, 20, "%2d", cx_smp->id);
	attrib_vals[2] = id_string;
	    
	iter = 3;
    }
    if((top_cx->type & 0xff00) == Plugin_cx_e){
	CX_PLUGIN* cx_plug = (CX_PLUGIN*)top_cx;
	attrib_names[1] = "plug_path";
	attrib_vals[1] = cx_plug->plug_path;

	attrib_names[2] = "preset_path";
	attrib_vals[2] = cx_plug->preset_path;
	    
	attrib_names[3] = "id";
	char id_string[20];
	snprintf(id_string, 20, "%2d", cx_plug->id);
	attrib_vals[3] = id_string;
	    
	iter = 4;
    }
    if((top_cx->type & 0xff00) == Osc_cx_e){
	CX_OSC* cx_osc = (CX_OSC*)top_cx;
	attrib_names[1] = "id";
	char id_string[20];
	snprintf(id_string, 20, "%d", cx_osc->id);
	attrib_vals[1] = id_string;
	
	iter = 2;
    }
    if((top_cx->type & 0xff00) == Val_cx_e){
	CX_VAL* cx_val = (CX_VAL*)top_cx;
	//just in case get the value from the ui_param so the value is 100% up to date before saving
	//since the value on the cx updates only when we get the value with the cx_get_value_callback
	//but dont do this if its a subtype of Val_cx_e, for example a container
	if(top_cx->type == Val_cx_e){
	    unsigned char ret_type = 0;
	    float updated_val = app_param_get_value(app_intrf->app_data, cx_val->cx_type, cx_val->cx_id, cx_val->val_id, &ret_type, 0, 0);
	    if(updated_val != -1 && ret_type != 0){
		cx_val->float_val = updated_val;
	    }
	}
	attrib_names[1] = "val_name";
	attrib_vals[1] = cx_val->val_name;
	    
	attrib_names[2] = "val_type";
	char val_type[12];
	snprintf(val_type, 12, "%2x", cx_val->val_type);
	attrib_vals[2] = val_type;
	    
	attrib_names[3] = "val_id";
	char val_id[40];
	snprintf(val_id, 40, "%2d", cx_val->val_id);
	attrib_vals[3] = val_id;

	attrib_names[4] = "float_val";
	char float_val[100];
	snprintf(float_val, 100, "%f", cx_val->float_val);
	attrib_vals[4] = float_val;

	attrib_names[5] = "cx_type";
	char cx_type[12];
	snprintf(cx_type, 12, "%d", cx_val->cx_type);
	attrib_vals[5] = cx_type;
	    
	attrib_names[6] = "cx_id";
	char cx_id[20];
	snprintf(cx_id, 20, "%d", cx_val->cx_id);
	attrib_vals[6] = cx_id;

	iter = 7;	    
    }
    if((top_cx->type & 0xff00) == Button_cx_e){
	CX_BUTTON* cx_button = (CX_BUTTON*)top_cx;
	attrib_names[1] = "str_val";
	attrib_vals[1] = cx_button->str_val;
	    
	attrib_names[2] = "uchar_val";
	char val_uchar[12];
	snprintf(val_uchar, 12, "%2x", (unsigned int)(cx_button->uchar_val));
	attrib_vals[2] = val_uchar;

	attrib_names[3] = "int_val";
	char val_int[20];
	snprintf(val_int, 20, "%d", (int)(cx_button->int_val));
	attrib_vals[3] = val_int;

	attrib_names[4] = "int_val_1";
	char val_int_1[20];
	snprintf(val_int_1, 20, "%d", (int)(cx_button->int_val_1));
	attrib_vals[4] = val_int_1;

	iter = 5;
    }

    app_json_write_json_callback(arg, top_cx->name, parent_name, parent_name, attrib_names, attrib_vals, iter);
}


static int helper_cx_iterate_with_callback(APP_INTRF* app_intrf, CX* top_cx, void* arg,
					   void(*proc_func)(void* arg, APP_INTRF* app_intrf, CX* in_cx)){
    if(top_cx == NULL)
	return -1;
    
    (*proc_func)(arg, app_intrf, top_cx);
    helper_cx_iterate_with_callback(app_intrf, top_cx->sib, arg, proc_func);
    helper_cx_iterate_with_callback(app_intrf, top_cx->child, arg, proc_func);

    return 0;
}

static int helper_cx_remove_cx_and_data(APP_INTRF* app_intrf, CX* rem_cx){
    if(!app_intrf)return -1;
    if(!rem_cx)return -1;
    //if this is a sample remove it in smp_data
    if((rem_cx->type & 0xff00) == Sample_cx_e){
	CX_SAMPLE* cx_smp = (CX_SAMPLE*)rem_cx;
	app_smp_remove_sample(app_intrf->app_data, cx_smp->id);
    }
    if((rem_cx->type & 0xff00) == Plugin_cx_e){
	CX_PLUGIN* cx_plug = (CX_PLUGIN*)rem_cx;
	app_plug_remove_plug(app_intrf->app_data, cx_plug->id);
    }
    if(rem_cx->type == (Button_cx_e | AddList_cx_st)){
	CX_BUTTON* cx_addList = (CX_BUTTON*)rem_cx;
	//if this is a container for ports, disconnect ports to clear the connections
	if(cx_addList->uchar_val == Connect_audio_purp || cx_addList->uchar_val == Connect_midi_purp){
	    helper_cx_disconnect_ports(app_intrf, rem_cx, 1);
	}
    }
    cx_remove_this_and_children(rem_cx);
    
    return 0;
}

static int helper_cx_clear_cx(APP_INTRF* app_intrf, CX* self_cx){
    if(!app_intrf || !self_cx)return -1;
    CX* start_cx = self_cx->child;
    while(start_cx){
	unsigned int rem = 0;
	if(start_cx->save == 1){
	    helper_cx_remove_cx_and_data(app_intrf, start_cx);
	    rem = 1;
	}
	if(rem == 0)
	    start_cx = start_cx->sib;
	if(rem == 1)
	    start_cx = self_cx->child;
    }

    return 0;
}

static int helper_cx_build_new_path(APP_INTRF* app_intrf, CX* self, int num){
    if(!self)return-1;
    //works only for Main_cx_e since only these cx can be saved to presets or songs in case of top level cx
    if((self->type & 0xff00)!=Main_cx_e)return -1;
    CX_MAIN* cx_main = (CX_MAIN*)self;
    char* new_path = NULL;
    if(self->type  == (Main_cx_e | Root_cx_st)){
	new_path = (char*)malloc(sizeof(char)*(2*(strlen(self->short_name))+5+strlen(FILE_EXT)));
	if(!new_path)return -1;
	sprintf(new_path, "%s-%.2d/%s%s", self->short_name, num, self->short_name, FILE_EXT);
    }
    else{
	new_path = (char*)malloc(sizeof(char)*(2*(strlen(self->short_name))
					       +strlen(FILE_EXT)
					       +strlen(app_intrf->shared_dir)
					       +6));
	if(!new_path)return -1;
	sprintf(new_path, "%s/%s/%s-%.2d%s", app_intrf->shared_dir, self->short_name, self->short_name,
		num, FILE_EXT);
    }
    if(cx_main->path)free(cx_main->path);
    cx_main->path = new_path;
    
    return 0;
}
static char* helper_cx_combine_paths(CX* self){
    char* ret_path = NULL;
    CX* parent = NULL;
    CX_MAIN* parent_main = NULL;
    CX_MAIN* cx_main_self = NULL;
    if(self==NULL)return NULL;
    parent = self->parent;
    if((self->type & 0xff00) != Main_cx_e)return NULL;
     cx_main_self = (CX_MAIN*)self;
    if(cx_main_self->path==NULL)return NULL;
    parent_main = (CX_MAIN*)parent;
    if(parent==NULL){
	ret_path = (char*)malloc(sizeof(char)*strlen(cx_main_self->path)+1);
	if(!ret_path)return NULL;
	strcpy(ret_path, cx_main_self->path);
	return ret_path;
    }
    if((parent->type & 0xff00) != Main_cx_e)return NULL;
    //if has a parent, combine the parents path with self path
    //if parent does not have a path return self path
    if(parent_main->path==NULL){
	ret_path = (char*)malloc(sizeof(char)*strlen(cx_main_self->path)+1);
	if(!ret_path)return NULL;
	strcpy(ret_path, cx_main_self->path);
	return ret_path;
    }
    ret_path = (char*)malloc(sizeof(char)*(strlen(cx_main_self->path)+strlen(parent_main->path)+1));
    if(!ret_path)return NULL;
    char temp_string[strlen(parent_main->path)+1];
    strcpy(temp_string, parent_main->path);
    strtok(temp_string, "/");
    strcpy(ret_path, temp_string);
    strcat(ret_path, "/");
    strcat(ret_path, cx_main_self->path);

    return ret_path;
}

static int helper_cx_copy_str_val(CX* from_cx, CX* to_cx){
    if(!from_cx || !to_cx)return -1;
    
    CX_BUTTON* cx_from_button = (CX_BUTTON*)from_cx;
    CX_BUTTON* cx_to_button = (CX_BUTTON*)to_cx;
    //if the str_val on from_cx does not exist we cannot proceed
    if(cx_from_button->str_val==NULL)return -1;

    //if the str_val is occupied remove it first
    if(cx_to_button->str_val!=NULL){
	free(cx_to_button->str_val);
	cx_to_button->str_val = NULL;
    }
    cx_to_button->str_val  = (char*)malloc(sizeof(char)*(strlen(cx_from_button->str_val)+1));
    if(cx_to_button->str_val)
	strcpy(cx_to_button->str_val, cx_from_button->str_val);

    return 0;
}

static int helper_cx_create_cx_for_default_params(APP_INTRF* app_intrf, CX* parent_node, const char* config_path,
						  unsigned char cx_type, int cx_id){
    if(!config_path)return -1;
    if(access(config_path, R_OK) != 0){
	unsigned int param_num;
	char** param_names = NULL;
	char** param_vals = NULL;
	char** param_types = NULL;

	int got_params = app_param_return_all_as_string(app_intrf->app_data, cx_type, cx_id,
							&param_names, &param_vals, &param_types, &param_num);  
	if(got_params >= 0 && param_num > 0 && param_names !=NULL){	    	
	    for(int i=0; i<param_num; i++){
		if(param_names[i] == NULL)continue;
		if(param_vals[i]==NULL || param_types[i]==NULL)continue;
		char val_id[40];
		snprintf(val_id, 40, "%d", i);
		char val_cx_type [12];
		snprintf(val_cx_type, 12, "%d", cx_type);
		char val_cx_id [20];
		snprintf(val_cx_id, 20, "%d", cx_id);
		CX* created_cx = cx_init_cx_type(app_intrf, parent_node->name, param_names[i], Val_cx_e,
				    (const char*[6]){param_names[i], val_id, param_types[i], param_vals[i], val_cx_type, val_cx_id},
						 (const char*[6]){"val_name", "val_id", "val_type", "float_val", "cx_type", "cx_id"}, 6);
		//dont stop if there was an error creating the val, the val simply will not be created
		/*
		if(!created_cx){
		    return -1;
		}
		*/
		if(param_names[i])free(param_names[i]);
		if(param_vals[i])free(param_vals[i]);
		if(param_types[i])free(param_types[i]);

	    }
	}
	if(param_types)free(param_types);
	if(param_vals)free(param_vals);
	if(param_names)free(param_names);
	JSONHANDLE* obj = NULL;
	if(app_json_create_obj(&obj)==0){
	    helper_cx_iterate_with_callback(app_intrf, parent_node->child, obj, helper_cx_prepare_for_param_conf);
	}
	//write the json handle to the file in the context path
	app_json_write_handle_to_file(obj, config_path, 1, 1);
    }
    else{
	//TODO how to setup user names for parameter values - similar to lv2 ScalePoints.
	app_json_open_iterate_callback(config_path, parent_node->name, app_intrf, cx_process_params_from_file);
    }
    return 0;
}

const char *app_intrf_write_err(const int *err_status){
    if(*err_status == IntrfFailedMalloc)return "Failed to allocate memory for the app_intrf struct, CLOSING...\n";
    if(*err_status == AppFailedMalloc)return "Failed to allocate memory for the app_data struct, CLOSING...\n";
    if(*err_status == SmpFailedMalloc)return "Failed to allocate memory for the drm_data struct, CLOSING...\n";
    if(*err_status == SmpJackFailedMalloc)return "Jack client for the sampler failed to initialize, CLOSING...\n";
    if(*err_status == SongFileParseErr)return "Failed to parse and create the configuration xmls, CLOSING...\n";
    if(*err_status == RootCXInitFailed)return "Failed to initialize the root context, CLOSING...\n";
    if(*err_status == BuildCXTreeFailed)
        return "Failed to parse song xml and to initialize the context structure, CLOSING...\n";
    if(*err_status == RootMallocFailed)
        return "Failed to initialize the root context, CLOSING...\n";
    if(*err_status == RootChildFailed)
        return "The root_cx does not have a child, context structure corrupted, CLOSING...\n";    
    if(*err_status == PlugDataFailedMalloc)
	return "The plug_data failed to initialize, CLOSING...\n";
    if(*err_status == PlugJackFailedInit)
	return "The plug_data failed to initialize, CLOSING...\n";
    if(*err_status == TrkJackFailedInit)
	return "The plug_data failed to initialize, CLOSING...\n";    
    
}
