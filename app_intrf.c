/*
    The INTRF layer is for creating the app_data structure
    Also, it allows UI to communicate with app_data
    For this reason no temporary cx's should be created -
    for on screen keyboards, info dialogs, lists of files when choosing a sample
    and similar the UI is responsible.
*/

/*
    Contexts should be added or removed only on initialization or when a
    context becomes dirty.
    App_data should only add or remove anything on initialization or
    if the user specificaly wants to do that - when he/she interacts with
    "buttons". These contexts are with the flag INTRF_FLAG_INTERACT and should
    make the context dirty after manipulating the data.
    For example: Data functions that populate/create a plugin list should only
    be called when the user interacts with a "refresh" or similar button (when
    data_invoke function is called). Or when the app is initialized, but NOT when
    the user is navigating (not when data_child_return function is called).
*/

#include "app_intrf.h"
#include <string.h>
#include "app_data.h"
#include "types.h"
#include "util_funcs/log_funcs.h"
#include <stdio.h>
#include <stdlib.h>

// TODO TODAY.
// Clap plugin lists; Lv2 and Clap plugin loading.
// TODO DAY AFTER.
// Implement groups.

/*
 TODO Groups (for example 10 total) each with cx_curr, cx_selected.
 When removing cx, check each group if the cx being removed is  not in
 cx_curr or  cx_selected. Nav_ functions that exits cx_curr or invokes
 cx_selected should exit and invoke cx given as arguments and apply the
 results to the group given as an argument. OR these functions should have
 arguments of groups where to exit cx_curr and invoke cx_selected and to what
 group apply the result. This way ui can invoke cx and enter it in one group
 but update the cx_curr on another group and show the children in a different
 window for example.
*/

/*
 TODO SAVING should be on the app_data layer.
 app_intrf calls function with the filename  where to save  and app_data
 saves there the structs as bites. when a file is loaded the app_data creates
 its structure from the file (creates the structs in memory) and marks the
 root as dirty so app_intrf recreates its structure. saving and loading
 separate contexts (plugins, trk and similar) should work the same. user
 should be able to set a file to load on startup.
*/

/*
 TODO when implementing clay or other ui, test mouse clicking;
 scrolling(would be nice to able to scroll any element with contents that do
 not fit) and selecting as soon as possible.
*/

typedef struct _cx_array{
    // the cx that was last interacted with, convenient to know for lists,
    // so user can continue from last place of visit
    unsigned int last_selected;
    unsigned int count;
    unsigned int count_max;
    struct _cx **contexts;
} CX_ARRAY;

// Struct for a single context
typedef struct _cx {
    // name of the cx, data must make sure this is unique in the context,
    // otherwise the cx will not be created
    char short_name[MAX_PARAM_NAME_LENGTH];
    int idx; // index number of this CX in the cx_parent cx_children array
    uint16_t user_data_type;
    void *user_data; // user_data for this cx, that the data layer uses. IT IS
                     // FORBIDDEN TO FREE OR MODIFY THIS IN ANY OTHER WAY
    struct _cx *cx_parent;
    uint32_t flags;

    //contexts array of children
    struct _cx_array cx_children;
} CX;

typedef struct _app_intrf {
    CX *cx_root;
    CX *cx_curr;     // current context that is entered right now
    CX *cx_selected; // the selected or last interacted context, when entering
                     // a context will be the first child
    uint16_t main_user_data_type; // type for the main user_data sturct, the
                                  // same type is in cx_root->user_data_type
    void *main_user_data; // main user_data struct for convenience, the same
                          // struct is in cx_root->user_data
    // return the idx child for the parent_user_data. Will return NULL if idx
    // is out of bounds flags returns the flags for this context from the data
    // return_name returns unique name among the parent_user_data children
    void *(*data_child_return)(void *parent_user_data, uint16_t parent_type,
                               uint16_t *return_type, uint32_t *flags,
                               char *return_name, int return_name_len,
                               unsigned int idx);
    // data function that updates its internal structures every cycle, should
    // be called first before any navigation
    void (*data_update)(void *main_user_data, uint16_t main_user_data_type);
    // do something with the user_data, this is a "button" callback
    // file can be a NULL, a file on the disk, or some other char* that data
    // needs depending on the INTRF_FLAG_
    void (*data_invoke)(void *user_data, uint16_t user_data_type,
                        const char *file);
    // check this user_data for dirty, if it is dirty, need to remove all of
    // its children cx and create them again.
    bool (*data_is_dirty)(void *user_data, uint16_t type);
    // destroy the whole data, user_data is the data from the cx_root CX. Used
    // when closing the app
    void (*data_destroy)(void *user_data, uint16_t type);
} APP_INTRF;

// When cx_curr or last_selected on a parent cx changes call this
// to refresh the cx_selected to a correct cx.
// good practice not to change cx_selected directly
static void app_intrf_cx_selected_restate(APP_INTRF *app_intrf){
    if(!app_intrf)
        return;
    if(!app_intrf->cx_curr)
        return;

    CX* cx_curr = app_intrf->cx_curr;
    app_intrf->cx_selected = cx_curr;    

    if (cx_curr->cx_children.count > 0) {
        if (cx_curr->cx_children.last_selected >= cx_curr->cx_children.count){
            cx_curr->cx_children.last_selected = 0;
        }
        unsigned int last_selected = cx_curr->cx_children.last_selected;
        app_intrf->cx_selected = cx_curr->cx_children.contexts[last_selected];
    }
}

// pop the child from the context structure 
static void app_intrf_cx_children_pop(APP_INTRF *app_intrf, CX *cx_rem){
    if (!app_intrf)
        return;
    CX* parent = cx_rem->cx_parent;
    if (parent){
        if (parent->cx_children.count > 0) {
            int child_idx = -1;
            //find the cx_rem in its parent children array
            for (int i = 0; i < parent->cx_children.count; i++){
                CX* curr_cx = parent->cx_children.contexts[i];
                if(curr_cx == cx_rem){
                    child_idx = i;
                    break;
                }
            }
            if (child_idx != -1) {
                // Remove the child_idx cx from the parent cx_children array
                unsigned int nmemb = parent->cx_children.count - 1;
                for (int i = child_idx; i < nmemb; i++) {
                    parent->cx_children.contexts[i] =
                        parent->cx_children.contexts[i + 1];
                }
                parent->cx_children.count = nmemb;
                // last member of the cx_children array has to be a NULL
                parent->cx_children.contexts[nmemb] = NULL;

                // change the parent children indices
                for (unsigned int i = 0; i < parent->cx_children.count; i++) {
                    CX *curr_cx = parent->cx_children.contexts[i];
                    if (curr_cx->idx <= child_idx)
                        continue;
                    curr_cx->idx -= 1;
                }
                // change the last selected cx in the parent cx_children array
                if (parent->cx_children.last_selected >= child_idx &&
                    parent->cx_children.last_selected > 0) {
                    parent->cx_children.last_selected -= 1;
                }
            }
        }
    }

    // if cx_curr is the same as remove_cx, change it to parent
    if (app_intrf->cx_curr == cx_rem) {
        app_intrf->cx_curr = parent;
    }

    //change cx_selected context just in case the remove_cx was that cx
    app_intrf_cx_selected_restate(app_intrf);

    //remove the cx_rem
    if(cx_rem->cx_children.contexts)free(cx_rem->cx_children.contexts);
    free(cx_rem);
}

// add child to the end of the parent cx_children array
static int app_intrf_cx_children_push(APP_INTRF *app_intrf, CX *child) {
    if (!app_intrf)
        return -1;
    if (!child)
        return -1;
    if (!child->cx_parent)
        return -1;

    CX* parent = child->cx_parent;
    unsigned int nmemb = parent->cx_children.count + 1; 
    //will need to realloc 
    if (nmemb >= parent->cx_children.count_max){
        unsigned int new_count_max = parent->cx_children.count_max * 2;
        CX **temp_array =
            realloc(parent->cx_children.contexts, sizeof(CX *) * new_count_max);
        if(!temp_array)
            return -1;
        parent->cx_children.contexts = temp_array;
        parent->cx_children.count_max = new_count_max;
    }
    parent->cx_children.count = nmemb;

    child->idx = parent->cx_children.count - 1; 
    parent->cx_children.contexts[child->idx] = child;
    parent->cx_children.contexts[child->idx + 1] = NULL;

    //change cx_selected for cases when cx_selected==cx_curr and 
    //a new cx is created inside cx_curr
    app_intrf_cx_selected_restate(app_intrf);
    return 1;
}

// create a new cx and return it.
// will be added to the parent_cx child array if parent_cx is given.
static CX *app_intrf_cx_create(APP_INTRF *app_intrf, CX *parent_cx,
                               void *user_data, uint16_t user_data_type,
                               uint32_t flags, const char *short_name) {
    if (!app_intrf)
        return NULL;
    if (!user_data)
        return NULL;
    if (flags == 0)
        return NULL;
    if (!short_name) {
        return NULL;
    }
    //two CX with same short_name under the same parent_cx cant exist
    if(parent_cx){
        for(unsigned int i = 0; i < parent_cx->cx_children.count; i++){
            CX* nb_cx = parent_cx->cx_children.contexts[i];
            if(strcmp(nb_cx->short_name, short_name)==0)
                return NULL;
        }
    }

    CX *new_cx = calloc(1, sizeof(CX));
    if (!new_cx)
        return NULL;

    new_cx->flags = flags;
    new_cx->cx_children.last_selected = 0;
    new_cx->cx_children.contexts = NULL;
    new_cx->cx_children.count = 0;
    new_cx->cx_children.count_max = PTR_ARRAY_COUNT;
    new_cx->cx_parent = parent_cx;
    new_cx->user_data = user_data;
    new_cx->user_data_type = user_data_type;
    new_cx->idx = -1;

    //if the context is not a container it will not have any children
    //if it is a container create the children array
    if ((new_cx->flags & INTRF_FLAG_CONTAINER)) {
        new_cx->cx_children.contexts =
           calloc(new_cx->cx_children.count_max, sizeof(CX *));
        if (!new_cx->cx_children.contexts) {
            app_intrf_cx_children_pop(app_intrf, new_cx);
            return NULL;
        }
    }

    snprintf(new_cx->short_name, MAX_PARAM_NAME_LENGTH, "%s", short_name);

    if (new_cx->cx_parent) {
        // add this cx to the parent cx array
        if (app_intrf_cx_children_push(app_intrf, new_cx) != 1) {
            app_intrf_cx_children_pop(app_intrf, new_cx);
            return NULL;
        }
    }

    // if the context was created succesfully check the flags and do additional
    // work as needed
    //----------------------------------------------------------------------------------------------------

    //----------------------------------------------------------------------------------------------------
    return new_cx;
}

// create children for the parent CX* recursively
static void app_intrf_cx_children_create(APP_INTRF *app_intrf, CX *parent_cx) {
    if (!app_intrf)
        return;
    if (!parent_cx)
        return;

    // create children
    unsigned int iter = 0;
    uint16_t child_type = 0;
    uint32_t child_flags = 0;
    char child_name[MAX_PARAM_NAME_LENGTH];
    void *child_data = app_intrf->data_child_return(
        parent_cx->user_data, parent_cx->user_data_type, &child_type,
        &child_flags, child_name, MAX_PARAM_NAME_LENGTH, iter);

    while (child_data) {
        app_intrf_cx_create(app_intrf, parent_cx, child_data, child_type,
                            child_flags, child_name);
        iter += 1;
        child_data = app_intrf->data_child_return(
            parent_cx->user_data, parent_cx->user_data_type, &child_type,
            &child_flags, child_name, MAX_PARAM_NAME_LENGTH, iter);
    }
    // create children for parent_cx children
    for (unsigned int i = 0; i < parent_cx->cx_children.count; i++) {
        CX *cur_child = parent_cx->cx_children.contexts[i];
        app_intrf_cx_children_create(app_intrf, cur_child);
    }
}

APP_INTRF *app_intrf_init() {
    APP_INTRF *app_intrf = calloc(1, sizeof(APP_INTRF));
    if (!app_intrf)
        return NULL;

    // initiate the app_intrf functions for data manipulation
    //--------------------------------------------------
    app_intrf->data_child_return = app_data_child_return;
    app_intrf->data_update = app_data_update;
    app_intrf->data_invoke = app_data_invoke;
    app_intrf->data_is_dirty = app_data_is_dirty;
    app_intrf->data_destroy = app_stop_and_clean;

    if (!app_intrf->data_child_return) {
        app_intrf_destroy(app_intrf);
        return NULL;
    }
    //--------------------------------------------------
    uint32_t root_flags = 0;
    char root_name[MAX_PARAM_NAME_LENGTH];

    app_intrf->main_user_data =
        app_init(&(app_intrf->main_user_data_type), &root_flags, root_name,
                 MAX_PARAM_NAME_LENGTH);
    if (!app_intrf->main_user_data || root_flags == 0) {
        app_intrf_destroy(app_intrf);
        return NULL;
    }
    // create the cx_root
    app_intrf->cx_root = app_intrf_cx_create(
        app_intrf, NULL, app_intrf->main_user_data,
        app_intrf->main_user_data_type, root_flags, root_name);
    if (!app_intrf->cx_root) {
        app_intrf_destroy(app_intrf);
        return NULL;
    }
    // and create the cx_root children recursively
    app_intrf_cx_children_create(app_intrf, app_intrf->cx_root);

    app_intrf->cx_curr = app_intrf->cx_root;
    app_intrf_cx_selected_restate(app_intrf);

    return app_intrf;
}

// iterate from root_cx through the children recursively and call the void user
// ok to use callback to remove cx but not to create (untested)
// root_cx - the cx from which to start iterating
// top_cx - should be same as root_cx, so iterating func knows the top cx
// leave_top - if 1, do not call callback_func for the top level cx
// filter_flags - for cx with these flags the callback function will not be
// called filter_only_top - filter_flags are only checked for the top level cx
// children, after that filter_flags are ignored
static void app_intrf_cx_children_iterate(
    APP_INTRF *app_intrf, CX *root_cx, CX *top_cx, unsigned int leave_top,
    enum intrfFlags filter_flags, unsigned int filter_only_top,
    void(callback_func)(APP_INTRF *app_intrf, CX *cur_cx)) {

    if (!root_cx)
        return;
    //init_count is necessary in case the callback_func changes the cx_children.count
    //for example when the cx are being removed with the callback_func
    unsigned int init_count = root_cx->cx_children.count;
    unsigned int iter = 0;
    while(iter < root_cx->cx_children.count){
        CX *cur_cx = root_cx->cx_children.contexts[iter];
        unsigned int go_inside = 1;

        if (filter_flags != 0) {
            // dont go inside if the exclude flags coincide
            if ((cur_cx->flags & filter_flags) && filter_only_top == 0)
                go_inside = 0;
            // dont go inside if the exclude flags coincide for the top level
            // children
            if ((cur_cx->flags & filter_flags) && filter_only_top == 1 &&
                root_cx == top_cx)
                go_inside = 0;
        }
        if (go_inside == 1)
            app_intrf_cx_children_iterate(app_intrf, cur_cx, top_cx, leave_top,
                                          filter_flags, filter_only_top,
                                          callback_func);

        iter += 1;
        if(init_count != root_cx->cx_children.count){
            iter = 0;
            init_count = root_cx->cx_children.count;
        }
    }
    
    // dont run the callback_func on the top cx
    if(leave_top == 1 && root_cx == top_cx)
        return;
    callback_func(app_intrf, root_cx);
}

void app_intrf_destroy(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    // clean the data
    if (app_intrf->data_destroy)
        app_intrf->data_destroy(app_intrf->main_user_data,
                                app_intrf->main_user_data_type);

    // remove the cx structure
    app_intrf_cx_children_iterate(app_intrf, app_intrf->cx_root,
                                  app_intrf->cx_root, 0, 0, 0,
                                  app_intrf_cx_children_pop);

    free(app_intrf);
}

// Check if cur_cx is dirty, if it is, remove and create its children
static void app_intrf_cx_check_dirty(APP_INTRF *app_intrf, CX *cur_cx) {
    if (!app_intrf)
        return;
    if (!cur_cx)
        return;
    // check if the context is dirty
    if (!app_intrf->data_is_dirty(cur_cx->user_data, cur_cx->user_data_type))
        return;
    // if it is remove all children recursively
    // but leave the cur_cx context and do not remove any of the cur_cx children
    // that has the _CANT_DIRTY flag
    app_intrf_cx_children_iterate(app_intrf, cur_cx, cur_cx, 1,
                                  INTRF_FLAG_CANT_DIRTY, 1,
                                  app_intrf_cx_children_pop);
    //create the children inside cur_cx again
    app_intrf_cx_children_create(app_intrf, cur_cx);
}

void nav_update(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    if (app_intrf->data_update)
        app_intrf->data_update(app_intrf->main_user_data,
                               app_intrf->main_user_data_type);
    // iterate the whole structure and check if any CX are dirty
    app_intrf_cx_children_iterate(app_intrf, app_intrf->cx_root,
                                  app_intrf->cx_root, 0, 0, 0,
                                  app_intrf_cx_check_dirty);
}

CX *nav_cx_root_return(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return NULL;
    return app_intrf->cx_root;
}

CX *nav_cx_curr_return(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return NULL;
    return app_intrf->cx_curr;
}

CX *nav_cx_selected_return(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return NULL;
    return app_intrf->cx_selected;
}

CX **nav_cx_children_return(APP_INTRF *app_intrf, CX *parent,
                            unsigned int *count) {
    if (!app_intrf)
        return NULL;
    if (!parent)
        return NULL;

    *count = parent->cx_children.count;
    return parent->cx_children.contexts;
}

int nav_cx_display_name_return(APP_INTRF *app_intrf, CX *cx, char *return_name,
                               unsigned int name_len) {
    if (!app_intrf)
        return -1;
    if (!cx)
        return -1;
    if (!return_name)
        return -1;
    // TODO Should check the flag _DISPLAY_NAME_DYN
    // for this flag should call special data function for returning dynamic
    // names this is needed for parameters and similar contexts
    snprintf(return_name, name_len, "%s", cx->short_name);
    return 1;
}

void nav_cx_selected_next(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    CX *selected_cx = app_intrf->cx_selected;
    if (!selected_cx)
        return;
    if (!selected_cx->cx_parent)
        return;
    if (selected_cx->idx < 0)
        return;

    int new_idx = selected_cx->idx + 1;
    if (new_idx >= selected_cx->cx_parent->cx_children.count)
        new_idx = 0;
    selected_cx->cx_parent->cx_children.last_selected = (unsigned int)new_idx;
    app_intrf_cx_selected_restate(app_intrf);
}

void nav_cx_selected_prev(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    CX *selected_cx = app_intrf->cx_selected;
    if (!selected_cx)
        return;
    if (!selected_cx->cx_parent)
        return;
    if (selected_cx->idx < 0)
        return;

    int new_idx = selected_cx->idx - 1;
    if (new_idx < 0)
        new_idx = selected_cx->cx_parent->cx_children.count - 1;
    selected_cx->cx_parent->cx_children.last_selected = (unsigned int)new_idx;
    app_intrf_cx_selected_restate(app_intrf);
}

int nav_cx_curr_exit(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return -1;
    if (!app_intrf->cx_curr)
        return -1;
    if (!app_intrf->cx_curr->cx_parent)
        return -1;

    app_intrf->cx_curr = app_intrf->cx_curr->cx_parent;
    app_intrf_cx_selected_restate(app_intrf);
    return 1;
}

int nav_cx_selected_invoke(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return -1;
    if (!app_intrf->cx_selected)
        return -1;
    CX *selected = app_intrf->cx_selected;
    // call the data invoke callback
    if (app_intrf->data_invoke)
        // TODO check flags if a filename or some other string needs to be
        // presented to the data If data needs a file string, or after invoke
        // the user needs to do another action the UI should be informed. This
        // could be done through flags on cx or this function can return codes
        // what needs to be done
        app_intrf->data_invoke(selected->user_data, selected->user_data_type,
                               NULL);

    // if this context has children enter inside
    if (!(selected->flags & INTRF_FLAG_CONTAINER))
        return 1;
    if (selected->cx_children.count == 0)
        return 1;

    app_intrf->cx_curr = selected;
    app_intrf_cx_selected_restate(app_intrf);
    return 1;
}
//----------------------------------------------------------------------------------------------------
