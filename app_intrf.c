#include "app_intrf.h"
#include "app_data.h"
#include "types.h"
#include "util_funcs/log_funcs.h"

#include <stdio.h>

//TODO params, plugins and sampler will need to be modified so that even the Parameter; Plugin or Sample has the param_container; plug_data; and smp_data in it.
//TODO need to be consistent - data functions uses void* user_data without any indices AND use structs already allocated on the contexts - modify them if they do not fit this concept.
//TODO implement "DIRTY" concept for the data structures. If for example a plugin is added, the Plugins structure (plug_data) will be marked as DIRTY by plugins.c
//TODO When app_intrf checks contexts and finds a cx dirty, will need a special function to remove and create the cx children again:
//go through each child with for loop and check their flags if they are eligible for recreation: children with INTRF_FLAG_CANT_DIRTY will not be removed.
//remove the children recursively. So even if childrens children will have a flag INTRF_FLAG_CANT_DIRTY they will be removed, since their parent is removed and there would be a memory leak.
//in this way if for example structure is Plugins->plug_01->load_preset->preset categories and presets, when a preset is loaded the plug_01 will be marked as dirty.
//all of the plug_01 children will be removed (parameters etc.), but load_preset has INTRF_FLAG_CANT_DIRTY and remove will leave it and its children, so the user can continue loading the presets.
//on the other hand if a new plugin plug_02 is created Plugins will be marked dirty. Since load_preset is Plugins childrens children it will be removed this time and created again.
//TODO when implementing clay or other ui, test mouse clicking; scrolling(would be nice to able to scroll any element with contents that do not fit) and selecting as soon as possible.
//TODO (selecting - on the ui side or app_intrf side?)

typedef struct _cx{
    char unique_name[MAX_PATH_STRING];
    char short_name[MAX_PARAM_NAME_LENGTH]; //this is used for UI as display_name. If DISPLAY_NAME_DYN flag is set, update short_name with a data function
    int idx; //index number of this CX in the cx_parent cx_children array
    uint16_t user_data_type;
    void* user_data; //user_data for this cx, that the data layer uses. IT IS FORBIDDEN TO FREE OR MODIFY THIS IN ANY OTHER WAY
    struct _cx* cx_parent;
    unsigned int cx_children_count;
    struct _cx** cx_children;
    uint32_t flags;
}CX;

typedef struct _app_intrf{
    CX* cx_root;
    CX* cx_curr; //current context that is entered right now
    CX* cx_selected; //the selected or last interacted context, when entering a context will be the first child
    uint16_t main_user_data_type; //type for the main user_data sturct, the same type is in cx_root->user_data_type
    void* main_user_data; //main user_data struct for convenience, the same struct is in cx_root->user_data
    void* (*data_child_return)(void* parent_user_data, uint16_t parent_type, uint16_t* return_type, unsigned int idx); //return the idx child for the parent_user_data. Will NULL if idx is out of bounds
    uint32_t (*data_flags_get)(void* user_data, uint16_t type); //get the flags for this cx from the data layer
    const char* (*data_short_name_get)(void* user_data, uint16_t type); //return the short name of the data
    void (*data_update)(void* main_user_data, uint16_t main_user_data_type); //data function that updates its internal structures every cycle, should be called first before any navigation
    bool (*data_is_dirty)(void* user_data, uint16_t type); //check this user_data for dirty, if it is dirty, need to remove all of its children cx and create them again.
    void (*data_destroy)(void* user_data, uint16_t type); //destroy the whole data, user_data is the data from the cx_root CX. Used when closing the app
}APP_INTRF;

//add child to the parent cx_children array
static int app_intrf_cx_children_add(APP_INTRF* app_intrf, CX* child, CX* parent){
    if(!app_intrf)return -1;
    if(!child)return -1;
    if(!parent)return -1;
    
    CX** temp_child_array = NULL;
    if(!parent->cx_children)
	temp_child_array = (CX**)calloc(1, sizeof(CX*));
    else
	temp_child_array = (CX**)realloc(parent->cx_children, sizeof(CX*) * (parent->cx_children_count+1));	    
    if(!temp_child_array){
	return -1;
    }
    parent->cx_children = temp_child_array;
    parent->cx_children[parent->cx_children_count] = child;
    parent->cx_children_count += 1;
    child->idx = parent->cx_children_count - 1;
    return 1;
}
//pop the child from parent cx_children array and realloc the array
static void app_intrf_cx_children_pop(APP_INTRF* app_intrf, int child_idx, CX* parent){
    if(!app_intrf)return;
    if(!parent)return;
    if(!parent->cx_children)return;
    if(child_idx >= parent->cx_children_count)return;
    if(child_idx < 0)return;
    parent->cx_children[child_idx] = NULL;
    
    unsigned int new_child_count = parent->cx_children_count - 1;
    if(new_child_count == 0){
	free(parent->cx_children);
	parent->cx_children = NULL;
	parent->cx_children_count = 0;
	return;
    }
    CX** new_child_array = (CX**)malloc(sizeof(CX*) * new_child_count);
    if(!new_child_array)return;
    int iter = 0;
    for(unsigned int i = 0; i < parent->cx_children_count; i++){
	if(i == child_idx)continue;
	new_child_array[iter] = parent->cx_children[i];
	new_child_array[iter]->idx = iter;
	
	iter += 1;
    }
    parent->cx_children_count = new_child_count;
    free(parent->cx_children);
    parent->cx_children = new_child_array;
}

//remove the cx and its children recursively.
static void app_intrf_cx_remove(APP_INTRF* app_intrf, CX* remove_cx){
    if(!app_intrf)return;
    if(!remove_cx)return;
    
    //remove cx_children 
    unsigned int safety = remove_cx->cx_children_count;
    unsigned int init_children_count = remove_cx->cx_children_count;
    unsigned int remove_child_idx = 0;
    while(remove_cx->cx_children_count > 0 && safety > 0){
	safety -= 1;
	CX* cur_child = remove_cx->cx_children[remove_child_idx];
	app_intrf_cx_remove(app_intrf, cur_child);
	//the child was not popped from the remove_cx cx_children array
	//so next remove the next child in the array, this error will leave cx_children array not popped and with NULLs in some indices, better then memory leak
	if(init_children_count == remove_cx->cx_children_count){
	    remove_child_idx += 1;
	}
	init_children_count = remove_cx->cx_children_count;
    }
    
    //if remove_cx has a parent pop remove_cx from its cx_children array
    if(remove_cx->cx_parent){
	app_intrf_cx_children_pop(app_intrf, remove_cx->idx, remove_cx->cx_parent);
    }
    //if there where problems of children popping from remove_cx cx_children array, free the cx_children array
    if(remove_cx->cx_children)free(remove_cx->cx_children);
    
    //if cx_curr or cx_selected is the same as remove_cx, change them
    if(app_intrf->cx_curr == remove_cx && remove_cx->cx_parent){
	app_intrf->cx_curr = remove_cx->cx_parent;
	app_intrf->cx_selected = remove_cx->cx_parent->cx_children[0];
    }
    if(app_intrf->cx_selected == remove_cx){
	app_intrf->cx_selected = app_intrf->cx_curr;
    }
    
    free(remove_cx);
}
//create a new cx and return it.
//will be added to the parent_cx child array if parent_cx is given.
//if flags==0 will get flags from the data layer with the data_flags_get function 
//if short_name==NULL will get the short_name from the data layer with the data_short_name_get function
static CX* app_intrf_cx_create(APP_INTRF* app_intrf, CX* parent_cx, void* user_data, uint16_t user_data_type, uint32_t flags, const char* short_name){
    if(!app_intrf)return NULL;
    
    CX* new_cx = calloc(1, sizeof(CX));
    if(!new_cx)return NULL;

    //add flags
    if(!user_data && flags == 0){
	app_intrf_cx_remove(app_intrf, new_cx);
	return NULL;
    }
    new_cx->flags = flags;
    if(flags == 0){
	//TODO if flags==0 use data function to return the flags variable from the data layer for this cx
    }
   
    new_cx->cx_parent = parent_cx;
    new_cx->user_data = user_data;
    new_cx->user_data_type = user_data_type;
    new_cx->idx = -1;
    
    if(!short_name && !user_data){
	app_intrf_cx_remove(app_intrf, new_cx);
	return NULL;
    }
    if(short_name){
	snprintf(new_cx->short_name, MAX_PARAM_NAME_LENGTH, "%s", short_name);
    }
    if(!short_name){
	const char* data_short_name = app_intrf->data_short_name_get(new_cx->user_data, new_cx->user_data_type);
	if(!data_short_name){
	    app_intrf_cx_remove(app_intrf, new_cx);
	    return NULL;
	}
	snprintf(new_cx->short_name, MAX_PARAM_NAME_LENGTH, "%s", data_short_name);
    }

    if(parent_cx){
	//add this cx to the parent cx array
	if(app_intrf_cx_children_add(app_intrf, new_cx, parent_cx) != 1){
	    app_intrf_cx_remove(app_intrf, new_cx);
	    return NULL;
	}
	//create unique name with the parent short name and this cx short name
	snprintf(new_cx->unique_name, MAX_PATH_STRING, "%s_<__>_%s", parent_cx->short_name, new_cx->short_name);
    }

    if(!parent_cx){
	snprintf(new_cx->unique_name, MAX_PATH_STRING, "%s", new_cx->short_name);
    }

    //IF user_data exists, get all children recursively for this cx from the data layer
    if(user_data){
	unsigned int iter = 0;
	uint16_t child_type = 0;
	void* child_data = app_intrf->data_child_return(new_cx->user_data, new_cx->user_data_type, &child_type, iter);
	while(child_data){
	    app_intrf_cx_create(app_intrf, new_cx, child_data, child_type, 0, NULL);
	    iter += 1;
	    child_data = app_intrf->data_child_return(new_cx->user_data, new_cx->user_data_type, &child_type, iter);
	}
    }
    //TODO if the context was created succesfully check the flags and do additional work as needed
    
    return new_cx;
}

APP_INTRF* app_intrf_init(){
    APP_INTRF* app_intrf = calloc(1, sizeof(APP_INTRF));
    if(!app_intrf)return NULL;
    
    //initiate the app_intrf functions for data manipulation
    //--------------------------------------------------
    app_intrf->data_child_return = app_data_child_return;
    app_intrf->data_short_name_get = app_data_short_name_get;
    app_intrf->data_update = app_data_update;
    app_intrf->data_destroy = app_stop_and_clean;

    if(!app_intrf->data_child_return || !app_intrf->data_short_name_get){
	app_intrf_destroy(app_intrf);
	return NULL;
    }
    //--------------------------------------------------
    
    app_intrf->main_user_data = app_init(&(app_intrf->main_user_data_type));
    if(!app_intrf->main_user_data){
	app_intrf_destroy(app_intrf);
	return NULL;
    }

    app_intrf->cx_root = app_intrf_cx_create(app_intrf, NULL, app_intrf->main_user_data, app_intrf->main_user_data_type, 0, NULL);
    if(!app_intrf->cx_root){
	app_intrf_destroy(app_intrf);
	return NULL;
    }

    app_intrf->cx_curr = app_intrf->cx_root;
    app_intrf->cx_selected = app_intrf->cx_curr;
    if(app_intrf->cx_curr->cx_children_count > 0)
	app_intrf->cx_selected = app_intrf->cx_curr->cx_children[0];
    
    return app_intrf;
}

void app_intrf_destroy(APP_INTRF* app_intrf){
    if(!app_intrf)return;
    
    //clean the data 
    if(app_intrf->data_destroy)
	app_intrf->data_destroy(app_intrf->main_user_data, app_intrf->main_user_data_type);

    //remove the cx structure
    app_intrf_cx_remove(app_intrf, app_intrf->cx_root);
    
    free(app_intrf);
}


//NAVIGATION functions
void nav_update(APP_INTRF* app_intrf){
    if(!app_intrf)return;
    if(app_intrf->data_update)
	app_intrf->data_update(app_intrf->main_user_data, app_intrf->main_user_data_type);
    //TODO go through the contexts to check if any are dirty.
    //TODO if a cx is dirty remove its children and create them again
}
CX* nav_cx_curr_return(APP_INTRF* app_intrf){
    if(!app_intrf)return NULL;
    return app_intrf->cx_curr;
}
CX* nav_cx_child_return(APP_INTRF* app_intrf, CX* parent, unsigned int child_idx){
    if(!app_intrf)return NULL;
    if(!parent)return NULL;
    if(child_idx >= parent->cx_children_count)return NULL;

    return parent->cx_children[child_idx];
}
int nav_cx_display_name_return(APP_INTRF* app_intrf, CX* cx, char* return_name, unsigned int name_len){
    if(!app_intrf)return -1;
    if(!cx)return -1;
    if(!return_name)return -1;

    snprintf(return_name, name_len, "%s", cx->short_name);
    return 1;
}
//----------------------------------------------------------------------------------------------------
