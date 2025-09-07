#include "app_intrf.h"
#include "app_data.h"
#include "types.h"
#include "util_funcs/log_funcs.h"

#include <stdio.h>

//TODO no TOP array. INSTEAD:
//Groups (for example 10 total) each with cx_curr, cx_selected. nav_ function should have an argument of group number, where to go next or prev or where to enter  the cx.
//When removing cx, check each group if the cx being removed is  not in cx_curr or  cx_selected.
//TODO when writting invoke data functions, dont forget to think how all of the data will be saved, might be tricky for contexts like ports
//will need structs on Trk contexts for ports and to what they are connected
//TODO When app_intrf checks contexts and finds a cx dirty, will need a special function to remove and create the cx children again:
//go through each child with for loop and check their flags if they are eligible for recreation: children with INTRF_FLAG_CANT_DIRTY will not be removed.
//remove the children recursively. So even if childrens children will have a flag INTRF_FLAG_CANT_DIRTY they will be removed, since their parent is removed and there would be a memory leak.
//in this way if for example structure is Plugins->plug_01->load_preset->preset categories and presets, when a preset is loaded the plug_01 will be marked as dirty.
//all of the plug_01 children will be removed (parameters etc.), but load_preset has INTRF_FLAG_CANT_DIRTY and remove will leave it and its children, so the user can continue loading the presets.
//on the other hand if a new plugin plug_02 is created Plugins will be marked dirty. Since load_preset is Plugins childrens children it will be removed this time and created again.
//TODO when implementing clay or other ui, test mouse clicking; scrolling(would be nice to able to scroll any element with contents that do not fit) and selecting as soon as possible.

typedef struct _cx{
    char unique_name[MAX_UNIQUE_ID_STRING];
    char short_name[MAX_PARAM_NAME_LENGTH]; //this is used for UI as display_name. If DISPLAY_NAME_DYN flag is set, update short_name with a data function
    int idx; //index number of this CX in the cx_parent cx_children array
    uint16_t user_data_type;
    void* user_data; //user_data for this cx, that the data layer uses. IT IS FORBIDDEN TO FREE OR MODIFY THIS IN ANY OTHER WAY
    struct _cx* cx_parent;
    unsigned int cx_children_last_selected; //the child in cx_children that was last selected (address put in app_intrf->cx_selected)
    unsigned int cx_children_count;
    struct _cx** cx_children;
    uint32_t flags;
}CX;

typedef struct _app_intrf{
    CX* cx_root;
    CX* cx_curr; //current context that is entered right now
    CX* cx_selected; //the selected or last interacted context, when entering a context will be the first child
    unsigned int cx_top_count; //how many CX* in the top array
    CX** cx_top; //array for contexts flagged as _ON_TOP. These can be retrieved safely like the cx_curr context and shown to the user at any time.
    uint16_t main_user_data_type; //type for the main user_data sturct, the same type is in cx_root->user_data_type
    void* main_user_data; //main user_data struct for convenience, the same struct is in cx_root->user_data
    //return the idx child for the parent_user_data. Will return NULL if idx is out of bounds
    //flags returns the flags for this context from the data
    void* (*data_child_return)(void* parent_user_data, uint16_t parent_type, uint16_t* return_type, uint32_t* flags, unsigned int idx);
    //return the short name of the data
    int (*data_short_name_get)(void* user_data, uint16_t type, char* return_name, unsigned int return_name_len);
    //data function that updates its internal structures every cycle, should be called first before any navigation
    void (*data_update)(void* main_user_data, uint16_t main_user_data_type);
    //do something with the user_data, this is a "button" callback
    //file can be a NULL, a file on the disk, or some other char* that data needs depending on the INTRF_FLAG_
    void (*data_invoke)(void* user_data, uint16_t user_data_type, const char* file);
    //check this user_data for dirty, if it is dirty, need to remove all of its children cx and create them again.
    bool (*data_is_dirty)(void* user_data, uint16_t type);
    //destroy the whole data, user_data is the data from the cx_root CX. Used when closing the app
    void (*data_destroy)(void* user_data, uint16_t type); 
}APP_INTRF;

//append CX* to the old_array. Since the array is realloced or calloced send &(CX**) array
static int app_intrf_cx_array_append(CX*** old_array, unsigned int* array_count, CX* add_cx){
    CX** temp_array = NULL;
    if(!old_array)
	temp_array = (CX**)calloc(1, sizeof(CX*));
    else
	temp_array = (CX**)realloc(*old_array, sizeof(CX*) * (*array_count + 1));
    if(!temp_array)
	return -1;

    *old_array = temp_array;
    (*old_array)[*array_count] = add_cx;
    *array_count += 1;

    return 1;
}

//add child to the end of the parent cx_children array
static int app_intrf_cx_children_add(APP_INTRF* app_intrf, CX* child){
    if(!app_intrf)return -1;
    if(!child)return -1;
    if(!child->cx_parent)return -1;
    
    if(app_intrf_cx_array_append(&(child->cx_parent->cx_children), &(child->cx_parent->cx_children_count), child) != 1)
	return -1;
    child->idx = child->cx_parent->cx_children_count - 1;
    return 1;
}

static int app_intrf_cx_array_pop(CX*** old_array, unsigned int* array_count, int pop_idx){
    if(!*old_array)return -1;
    if(*array_count == 0)return -1;
    if(pop_idx < 0) return -1;
    if(pop_idx >= *array_count)return -1;

    unsigned int new_count = (*array_count) - 1;
    if(new_count == 0){
	free(*old_array);
	*old_array = NULL;
	*array_count = 0;
	return 1;
    }
    //move everything to the left
    for(unsigned int i = pop_idx; i < new_count; i++){
	(*old_array)[i] = (*old_array)[i+1];
    }

    CX** temp_array;
    temp_array = (CX**)realloc(*old_array, sizeof(CX*) * new_count);
    if(!temp_array)
	return -1;

    *old_array = temp_array;
    *array_count = new_count;
    return 1;
}

//pop the child from parent cx_children array and realloc the array
static void app_intrf_cx_children_pop(APP_INTRF* app_intrf, int child_idx, CX* parent){
    if(!app_intrf)return;
    if(!parent)return;
    if(!parent->cx_children)return;
    if(child_idx >= parent->cx_children_count)return;
    if(child_idx < 0)return;

    if(app_intrf_cx_array_pop(&(parent->cx_children), &(parent->cx_children_count), child_idx) != 1)return;
    
    //change the parent children indices
    for(unsigned int i = 0; i < parent->cx_children_count; i++){
	CX* curr_cx = parent->cx_children[i];
	if(curr_cx->idx <= child_idx)continue;
	curr_cx->idx -= 1;
    }
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
    //if the remove_cx is in the top cx array, pop it from there
    for(unsigned int i = 0; i < app_intrf->cx_top_count; i++){
	CX* curr_cx = app_intrf->cx_top[i];
	if(curr_cx != remove_cx)continue;
	app_intrf_cx_array_pop(&(app_intrf->cx_top), &(app_intrf->cx_top_count), i);
    }
    if(app_intrf->cx_top_count == 0)app_intrf->cx_top = NULL;

    
    free(remove_cx);
}
//create a new cx and return it.
//will be added to the parent_cx child array if parent_cx is given.
//if short_name==NULL will get the short_name from the data layer with the data_short_name_get function
static CX* app_intrf_cx_create(APP_INTRF* app_intrf, CX* parent_cx, void* user_data, uint16_t user_data_type, uint32_t flags, const char* short_name){
    if(!app_intrf)return NULL;
    
    CX* new_cx = calloc(1, sizeof(CX));
    if(!new_cx)return NULL;

    //add flags
    if(flags == 0){
	app_intrf_cx_remove(app_intrf, new_cx);
	return NULL;
    }
    new_cx->flags = flags;
   
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
	if(app_intrf->data_short_name_get(new_cx->user_data, new_cx->user_data_type, new_cx->short_name, MAX_PARAM_NAME_LENGTH) != 1){
	    app_intrf_cx_remove(app_intrf, new_cx);
	    return NULL;
	}
    }

    if(new_cx->cx_parent){
	//add this cx to the parent cx array
	if(app_intrf_cx_children_add(app_intrf, new_cx) != 1){
	    app_intrf_cx_remove(app_intrf, new_cx);
	    return NULL;
	}
	//create unique name with the parent short name and this cx short name
	snprintf(new_cx->unique_name, MAX_UNIQUE_ID_STRING, "%s_<__>_%s", parent_cx->short_name, new_cx->short_name);
    }

    if(!parent_cx){
	snprintf(new_cx->unique_name, MAX_UNIQUE_ID_STRING, "%s", new_cx->short_name);
    }
 
    //if the context was created succesfully check the flags and do additional work as needed
    //----------------------------------------------------------------------------------------------------


    //----------------------------------------------------------------------------------------------------
    return new_cx;
}
//create children for the parent CX* recursively
static void app_intrf_cx_children_create(APP_INTRF* app_intrf, CX* parent_cx){
    if(!app_intrf)return;
    if(!parent_cx)return;
    unsigned int iter = 0;
    uint16_t child_type = 0;
    uint32_t child_flags = 0;
    void* child_data = app_intrf->data_child_return(parent_cx->user_data, parent_cx->user_data_type, &child_type, &child_flags, iter);
    while(child_data){
	app_intrf_cx_create(app_intrf, parent_cx, child_data, child_type, child_flags, NULL);
	iter += 1;
	child_data = app_intrf->data_child_return(parent_cx->user_data, parent_cx->user_data_type, &child_type, &child_flags, iter);
    }

    for(unsigned int i = 0; i < parent_cx->cx_children_count; i++){
	CX* cur_child = parent_cx->cx_children[i];
	app_intrf_cx_children_create(app_intrf, cur_child);
    }
}
APP_INTRF* app_intrf_init(){
    APP_INTRF* app_intrf = calloc(1, sizeof(APP_INTRF));
    if(!app_intrf)return NULL;
    
    //initiate the app_intrf functions for data manipulation
    //--------------------------------------------------
    app_intrf->data_child_return = app_data_child_return;
    app_intrf->data_short_name_get = app_data_short_name_get;
    app_intrf->data_update = app_data_update;
    app_intrf->data_invoke = app_data_invoke;
    app_intrf->data_is_dirty = app_data_is_dirty;
    app_intrf->data_destroy = app_stop_and_clean;

    if(!app_intrf->data_child_return || !app_intrf->data_short_name_get){
	app_intrf_destroy(app_intrf);
	return NULL;
    }
    //--------------------------------------------------
    uint32_t root_flags = 0;
    app_intrf->main_user_data = app_init(&(app_intrf->main_user_data_type), &root_flags);
    if(!app_intrf->main_user_data || root_flags == 0){
	app_intrf_destroy(app_intrf);
	return NULL;
    }
    //create the cx_root
    app_intrf->cx_root = app_intrf_cx_create(app_intrf, NULL, app_intrf->main_user_data, app_intrf->main_user_data_type, root_flags, NULL);
    if(!app_intrf->cx_root){
	app_intrf_destroy(app_intrf);
	return NULL;
    }
    //and create the cx_root children recursively
    app_intrf_cx_children_create(app_intrf, app_intrf->cx_root);
    
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

static void app_intrf_cx_check_dirty(APP_INTRF* app_intrf, CX* cur_cx){
    if(!app_intrf)return;
    if(!cur_cx)return;

    //check if the context is dirty
    if(!app_intrf->data_is_dirty(cur_cx->user_data, cur_cx->user_data_type))return;

    //if it is remove all children recursively
    for(unsigned int i = 0; i < cur_cx->cx_children_count; i++){
	//TODO should not remove immidiate children with flag _CANT_DIRTY
	CX* cur_child = cur_cx->cx_children[i];
	app_intrf_cx_remove(app_intrf, cur_child);
    }
    //recreate the children
    //TODO children that are already created (because of the _CANT_DIRTY flag) should not be created again
    app_intrf_cx_children_create(app_intrf, cur_cx);

    //select a CX if there is no CX selected
    if(app_intrf->cx_curr == app_intrf->cx_selected && cur_cx->cx_children_count > 0)
	app_intrf->cx_selected = cur_cx->cx_children[0];
}
//iterate from root_cx through the children recursively and call the void user function
static void app_intrf_cx_children_iterate(APP_INTRF* app_intrf, CX* root_cx, void (callback_func)(APP_INTRF* app_intrf, CX* cur_cx)){
    if(!root_cx)return;
    callback_func(app_intrf, root_cx);
    if(!root_cx->cx_children || root_cx->cx_children_count == 0)return;

    for(unsigned int i = 0; i < root_cx->cx_children_count; i++){
	CX* cur_cx = root_cx->cx_children[i];
	app_intrf_cx_children_iterate(app_intrf, cur_cx, callback_func);
    }
}
//NAVIGATION functions
void nav_update(APP_INTRF* app_intrf){
    if(!app_intrf)return;
    if(app_intrf->data_update)
	app_intrf->data_update(app_intrf->main_user_data, app_intrf->main_user_data_type);
    //iterate the whole structure and check if any CX are dirty
    app_intrf_cx_children_iterate(app_intrf, app_intrf->cx_root, app_intrf_cx_check_dirty);
    //CX_TOP array recreate
    //----------------------------------------------------------------------------------------------------
    //remove all top contexts
    while(app_intrf->cx_top_count > 0){
	if(app_intrf_cx_array_pop(&(app_intrf->cx_top), &(app_intrf->cx_top_count), 0) != 1)
	    break;
    }
    //create the top contexts again
    //check if parent or one of its children has a _ON_TOP flag and add it to the cx_top array
    //go up the hierarchy from the cx_curr till the parent is NULL
    CX* this_cx = app_intrf->cx_curr;
    while(this_cx){
	CX* this_parent = this_cx->cx_parent;
	if(!this_parent)break;
	for(unsigned int i = 0; i < this_parent->cx_children_count; i++){
	    CX* child_cx = this_parent->cx_children[i];
	    if((child_cx->flags & INTRF_FLAG_ON_TOP))
		app_intrf_cx_array_append(&(app_intrf->cx_top), &(app_intrf->cx_top_count), child_cx);
	}

	this_cx = this_parent;
    }
    //----------------------------------------------------------------------------------------------------
}
CX* nav_cx_root_return(APP_INTRF* app_intrf){
    if(!app_intrf)return NULL;
    return app_intrf->cx_root;
}
CX* nav_cx_curr_return(APP_INTRF* app_intrf){
    if(!app_intrf)return NULL;
    return app_intrf->cx_curr;
}
CX* nav_cx_selected_return(APP_INTRF* app_intrf){
    if(!app_intrf)return NULL;
    return app_intrf->cx_selected;
}
CX** nav_cx_children_return(APP_INTRF* app_intrf, CX* parent, unsigned int* count){
    if(!app_intrf)return NULL;
    if(!parent)return NULL;

    *count = parent->cx_children_count;
    return parent->cx_children;
}
CX** nav_cx_top_children_return(APP_INTRF* app_intrf, unsigned int* count){
    if(!app_intrf)return NULL;
    if(!app_intrf->cx_top)return NULL;
    
    *count = app_intrf->cx_top_count;
    return app_intrf->cx_top;
}
int nav_cx_display_name_return(APP_INTRF* app_intrf, CX* cx, char* return_name, unsigned int name_len){
    if(!app_intrf)return -1;
    if(!cx)return -1;
    if(!return_name)return -1;

    snprintf(return_name, name_len, "%s", cx->short_name);
    return 1;
}

void nav_cx_selected_next(APP_INTRF* app_intrf){
    if(!app_intrf)return;
    CX* selected_cx = app_intrf->cx_selected;
    if(!selected_cx)return;
    if(!selected_cx->cx_parent)return;
    if(selected_cx->idx < 0)return;

    int new_idx = selected_cx->idx + 1;
    if(new_idx >= selected_cx->cx_parent->cx_children_count)new_idx = 0;
    selected_cx->cx_parent->cx_children_last_selected = (unsigned int)new_idx;
    app_intrf->cx_selected = selected_cx->cx_parent->cx_children[new_idx];
}
void nav_cx_selected_prev(APP_INTRF* app_intrf){
    if(!app_intrf)return;
    CX* selected_cx = app_intrf->cx_selected;
    if(!selected_cx)return;
    if(!selected_cx->cx_parent)return;
    if(selected_cx->idx < 0)return;

    int new_idx = selected_cx->idx - 1;
    if(new_idx < 0)new_idx = selected_cx->cx_parent->cx_children_count - 1;
    selected_cx->cx_parent->cx_children_last_selected = (unsigned int)new_idx;
    app_intrf->cx_selected = selected_cx->cx_parent->cx_children[new_idx];
}
int nav_cx_curr_exit(APP_INTRF* app_intrf){
    if(!app_intrf)return -1;
    if(!app_intrf->cx_curr)return -1;
    if(!app_intrf->cx_curr->cx_parent)return -1;
    
    app_intrf->cx_curr = app_intrf->cx_curr->cx_parent;
    app_intrf->cx_selected = app_intrf->cx_curr;
    //new cx_selected context
    if(app_intrf->cx_curr->cx_children_count > 0){
	app_intrf->cx_selected = app_intrf->cx_curr->cx_children[0];
	if(app_intrf->cx_curr->cx_children_last_selected < app_intrf->cx_curr->cx_children_count)
	    app_intrf->cx_selected = app_intrf->cx_curr->cx_children[app_intrf->cx_curr->cx_children_last_selected];
    }

    return 1;
}
int nav_cx_selected_invoke(APP_INTRF* app_intrf){
    if(!app_intrf)return -1;
    if(!app_intrf->cx_selected)return -1;
    CX* selected = app_intrf->cx_selected;
    //call the data invoke callback
    if(app_intrf->data_invoke)
	//TODO check flags if a filename or some other string needs to be presented to the data
	app_intrf->data_invoke(selected->user_data, selected->user_data_type, NULL);
    
    //if this context has children enter inside
    if(!(selected->flags & INTRF_FLAG_CONTAINER))return 1;
    if(selected->cx_children_count == 0)return 1;
    
    app_intrf->cx_curr = selected;
    app_intrf->cx_selected = app_intrf->cx_curr;
    //new cx_selected context
    if(app_intrf->cx_curr->cx_children_count > 0){
	app_intrf->cx_selected = app_intrf->cx_curr->cx_children[0];
	if(app_intrf->cx_curr->cx_children_last_selected < app_intrf->cx_curr->cx_children_count)
	    app_intrf->cx_selected = app_intrf->cx_curr->cx_children[app_intrf->cx_curr->cx_children_last_selected];
    }

    return 1;
}
//----------------------------------------------------------------------------------------------------
