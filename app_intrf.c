#include "app_intrf.h"
#include "app_data.h"
#include "types.h"
#include "util_funcs/log_funcs.h"

#include <stdio.h>
/*
 The INTRF layer is for creating the app_data structure
 Also, it allows UI to communicate with app_data
 For this reason no temporary cx's should be created -
 for on screen keyboards, info dialogs and similar the UI is responsible
*/

// TODO PRIORITY! think how to properly remove the contexts recursively - now
// the system is flawed. It is evident when using the _is_dirty() function:
// since the cur_cx children count is changing when the contexts are deleted
// the i++ iterator becomes incorrect, it gets bigger than children count
// and not all of the contexts are deleted as a consequence

// TODO Groups (for example 10 total) each with cx_curr, cx_selected.
// When removing cx, check each group if the cx being removed is  not in
// cx_curr or  cx_selected. Nav_ functions that exits cx_curr or invokes
// cx_selected should exit and invoke cx given as arguments and apply the
// results to the group given as an argument. OR these functions should have
// arguments of groups where to exit cx_curr and invoke cx_selected and to what
// group apply the result. This way ui can invoke cx and enter it in one group
// but update the cx_curr on another group and show the children in a different
// window for example.
// TODO SAVING should be on the app_data layer.
// app_intrf calls function with the filename  where to save  and app_data
// saves there the structs as bites. when a file is loaded the app_data creates
// its structure from the file (creates the structs in memory) and marks the
// root as dirty so app_intrf recreates its structure. saving and loading
// separate contexts (plugins, trk and similar) should work the same. user
// should be able to set a file to load on startup.
// TODO When app_intrf checks contexts and finds a cx dirty, will need a
// special function to remove and create the cx children again: go through each
// child with for loop and check their flags if they are eligible for
// recreation: children with INTRF_FLAG_CANT_DIRTY will not be removed. remove
// the children recursively. So even if childrens children will have a flag
// INTRF_FLAG_CANT_DIRTY they will be removed, since their parent is removed
// and there would be a memory leak. in this way if for example structure is
// Plugins->plug_01->load_preset->preset categories and presets, when a preset
// is loaded the plug_01 will be marked as dirty. all of the plug_01 children
// will be removed (parameters etc.), but load_preset has INTRF_FLAG_CANT_DIRTY
// and remove will leave it and its children, so the user can continue loading
// the presets. on the other hand if a new plugin plug_02 is created Plugins
// will be marked dirty. Since load_preset is Plugins childrens children it
// will be removed this time and created again.
// TODO when implementing clay or other ui, test mouse clicking;
// scrolling(would be nice to able to scroll any element with contents that do
// not fit) and selecting as soon as possible.

typedef struct _cx {
    // name of the cx, data must make sure this is unique in the context,
    // otherwise the cx will not be created
    char short_name[MAX_PARAM_NAME_LENGTH];
    int idx; // index number of this CX in the cx_parent cx_children array
    uint16_t user_data_type;
    void *user_data; // user_data for this cx, that the data layer uses. IT IS
                     // FORBIDDEN TO FREE OR MODIFY THIS IN ANY OTHER WAY
    struct _cx *cx_parent;
    unsigned int cx_children_last_selected; // the child in cx_children that
                                            // was last selected (address put
                                            // in app_intrf->cx_selected)
    unsigned int cx_children_count;
    struct _cx **cx_children;
    uint32_t flags;
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

// append CX* to the old_array. Since the array is realloced or calloced send
// &(CX**) array
static int app_intrf_cx_array_append(CX ***old_array, unsigned int *array_count,
                                     CX *add_cx) {
    CX **temp_array = NULL;
    if (!old_array)
        temp_array = (CX **)calloc(1, sizeof(CX *));
    else
        temp_array =
            (CX **)realloc(*old_array, sizeof(CX *) * (*array_count + 1));
    if (!temp_array)
        return -1;

    *old_array = temp_array;
    (*old_array)[*array_count] = add_cx;
    *array_count += 1;

    return 1;
}

// add child to the end of the parent cx_children array
static int app_intrf_cx_children_add(APP_INTRF *app_intrf, CX *child) {
    if (!app_intrf)
        return -1;
    if (!child)
        return -1;
    if (!child->cx_parent)
        return -1;

    if (app_intrf_cx_array_append(&(child->cx_parent->cx_children),
                                  &(child->cx_parent->cx_children_count),
                                  child) != 1)
        return -1;
    child->idx = child->cx_parent->cx_children_count - 1;
    return 1;
}

static int app_intrf_cx_array_pop(CX ***old_array, unsigned int *array_count,
                                  int pop_idx) {
    if (!*old_array)
        return -1;
    if (*array_count == 0)
        return -1;
    if (pop_idx < 0)
        return -1;
    if (pop_idx >= *array_count)
        return -1;

    unsigned int new_count = (*array_count) - 1;
    if (new_count == 0) {
        free(*old_array);
        *old_array = NULL;
        *array_count = 0;
        return 1;
    }
    // move everything to the left
    for (unsigned int i = pop_idx; i < new_count; i++) {
        (*old_array)[i] = (*old_array)[i + 1];
    }

    CX **temp_array;
    temp_array = (CX **)realloc(*old_array, sizeof(CX *) * new_count);
    if (!temp_array)
        return -1;

    *old_array = temp_array;
    *array_count = new_count;
    return 1;
}

// pop the child from parent cx_children array and realloc the array
static void app_intrf_cx_children_pop(APP_INTRF *app_intrf, int child_idx,
                                      CX *parent) {
    if (!app_intrf)
        return;
    if (!parent)
        return;
    if (!parent->cx_children)
        return;
    if (child_idx >= parent->cx_children_count)
        return;
    if (child_idx < 0)
        return;

    if (app_intrf_cx_array_pop(&(parent->cx_children),
                               &(parent->cx_children_count), child_idx) != 1)
        return;

    // change the parent children indices
    for (unsigned int i = 0; i < parent->cx_children_count; i++) {
        CX *curr_cx = parent->cx_children[i];
        if (curr_cx->idx <= child_idx)
            continue;
        curr_cx->idx -= 1;
    }
}

// remove the cx and its children recursively.
static void app_intrf_cx_remove(APP_INTRF *app_intrf, CX *remove_cx) {
    if (!app_intrf)
        return;
    if (!remove_cx)
        return;

    // remove cx_children
    unsigned int safety = remove_cx->cx_children_count;
    unsigned int init_children_count = remove_cx->cx_children_count;
    unsigned int remove_child_idx = 0;
    while (remove_cx->cx_children_count > 0 && safety > 0) {
        safety -= 1;
        CX *cur_child = remove_cx->cx_children[remove_child_idx];
        app_intrf_cx_remove(app_intrf, cur_child);
        // the child was not popped from the remove_cx cx_children array
        // so next remove the next child in the array, this error will
        // leave cx_children array not popped and with NULLs in some
        // indices, better then memory leak
        if (init_children_count == remove_cx->cx_children_count) {
            remove_child_idx += 1;
        }
        init_children_count = remove_cx->cx_children_count;
    }

    // if remove_cx has a parent pop remove_cx from its cx_children array
    if (remove_cx->cx_parent) {
        app_intrf_cx_children_pop(app_intrf, remove_cx->idx,
                                  remove_cx->cx_parent);
    }
    // if there where problems of children popping from remove_cx cx_children
    // array, free the cx_children array
    if (remove_cx->cx_children)
        free(remove_cx->cx_children);

    // if cx_curr or cx_selected is the same as remove_cx, change them
    if (app_intrf->cx_curr == remove_cx && remove_cx->cx_parent) {
        app_intrf->cx_curr = remove_cx->cx_parent;
        app_intrf->cx_selected = remove_cx->cx_parent->cx_children[0];
    }
    if (app_intrf->cx_selected == remove_cx) {
        app_intrf->cx_selected = app_intrf->cx_curr;
    }
    // TODO if the remove_cx is in any group pop from there

    free(remove_cx);
}

// create a new cx and return it.
// will be added to the parent_cx child array if parent_cx is given.
// if short_name==NULL will get the short_name from the data layer
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
    // TODO check here if there is another cx with the same short_name
    // if so remove this cx and return NULL

    CX *new_cx = calloc(1, sizeof(CX));
    if (!new_cx)
        return NULL;

    new_cx->flags = flags;
    new_cx->cx_parent = parent_cx;
    new_cx->user_data = user_data;
    new_cx->user_data_type = user_data_type;
    new_cx->idx = -1;

    snprintf(new_cx->short_name, MAX_PARAM_NAME_LENGTH, "%s", short_name);

    if (new_cx->cx_parent) {
        // add this cx to the parent cx array
        if (app_intrf_cx_children_add(app_intrf, new_cx) != 1) {
            app_intrf_cx_remove(app_intrf, new_cx);
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

    for (unsigned int i = 0; i < parent_cx->cx_children_count; i++) {
        CX *cur_child = parent_cx->cx_children[i];
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
    app_intrf->cx_selected = app_intrf->cx_curr;
    if (app_intrf->cx_curr->cx_children_count > 0)
        app_intrf->cx_selected = app_intrf->cx_curr->cx_children[0];

    return app_intrf;
}

void app_intrf_destroy(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    // clean the data
    if (app_intrf->data_destroy)
        app_intrf->data_destroy(app_intrf->main_user_data,
                                app_intrf->main_user_data_type);

    // remove the cx structure
    app_intrf_cx_remove(app_intrf, app_intrf->cx_root);

    free(app_intrf);
}

static void app_intrf_cx_check_dirty(APP_INTRF *app_intrf, CX *cur_cx) {
    if (!app_intrf)
        return;
    if (!cur_cx)
        return;

    // check if the context is dirty
    if (!app_intrf->data_is_dirty(cur_cx->user_data, cur_cx->user_data_type))
        return;
    log_append_logfile("%s is dirty, children num %d\n", cur_cx->short_name, cur_cx->cx_children_count);
    // if it is remove all children recursively
    for (unsigned int i = 0; i < cur_cx->cx_children_count; i++) {
        CX *cur_child = cur_cx->cx_children[i];
        //Dont remove immidiate children with _CANT_DIRTY flag
        if((cur_child->flags & INTRF_FLAG_CANT_DIRTY))
            continue;
        app_intrf_cx_remove(app_intrf, cur_child);
    }
    // recreate the children
    // since app_intrf_cx_children_create() checks for cx duplicates in the
    // parent_cx cx that are not remove if they have _CANT_DIRTY flag will not
    // be duplicated
    app_intrf_cx_children_create(app_intrf, cur_cx);

    // select a CX if there is no CX selected
    if (app_intrf->cx_curr == app_intrf->cx_selected &&
        cur_cx->cx_children_count > 0)
        app_intrf->cx_selected = cur_cx->cx_children[0];
}

// iterate from root_cx through the children recursively and call the void user
// function
static void app_intrf_cx_children_iterate(
    APP_INTRF *app_intrf, CX *root_cx,
    void(callback_func)(APP_INTRF *app_intrf, CX *cur_cx)) {
    if (!root_cx)
        return;
    callback_func(app_intrf, root_cx);
    if (!root_cx->cx_children || root_cx->cx_children_count == 0)
        return;

    for (unsigned int i = 0; i < root_cx->cx_children_count; i++) {
        CX *cur_cx = root_cx->cx_children[i];
        app_intrf_cx_children_iterate(app_intrf, cur_cx, callback_func);
    }
}

// NAVIGATION functions
void nav_update(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    if (app_intrf->data_update)
        app_intrf->data_update(app_intrf->main_user_data,
                               app_intrf->main_user_data_type);
    // iterate the whole structure and check if any CX are dirty
    app_intrf_cx_children_iterate(app_intrf, app_intrf->cx_root,
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

    *count = parent->cx_children_count;
    return parent->cx_children;
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
    if (new_idx >= selected_cx->cx_parent->cx_children_count)
        new_idx = 0;
    selected_cx->cx_parent->cx_children_last_selected = (unsigned int)new_idx;
    app_intrf->cx_selected = selected_cx->cx_parent->cx_children[new_idx];
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
        new_idx = selected_cx->cx_parent->cx_children_count - 1;
    selected_cx->cx_parent->cx_children_last_selected = (unsigned int)new_idx;
    app_intrf->cx_selected = selected_cx->cx_parent->cx_children[new_idx];
}

int nav_cx_curr_exit(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return -1;
    if (!app_intrf->cx_curr)
        return -1;
    if (!app_intrf->cx_curr->cx_parent)
        return -1;

    app_intrf->cx_curr = app_intrf->cx_curr->cx_parent;
    app_intrf->cx_selected = app_intrf->cx_curr;
    // new cx_selected context
    // TODO maybe this could be in a separate function -
    // other functions use this snippet too (like the nav_cx_selected_invoke
    // function)
    if (app_intrf->cx_curr->cx_children_count > 0) {
        app_intrf->cx_selected = app_intrf->cx_curr->cx_children[0];
        if (app_intrf->cx_curr->cx_children_last_selected <
            app_intrf->cx_curr->cx_children_count)
            app_intrf->cx_selected =
                app_intrf->cx_curr->cx_children
                    [app_intrf->cx_curr->cx_children_last_selected];
    }

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
    if (selected->cx_children_count == 0)
        return 1;

    app_intrf->cx_curr = selected;
    app_intrf->cx_selected = app_intrf->cx_curr;
    // new cx_selected context
    if (app_intrf->cx_curr->cx_children_count > 0) {
        app_intrf->cx_selected = app_intrf->cx_curr->cx_children[0];
        if (app_intrf->cx_curr->cx_children_last_selected <
            app_intrf->cx_curr->cx_children_count)
            app_intrf->cx_selected =
                app_intrf->cx_curr->cx_children
                    [app_intrf->cx_curr->cx_children_last_selected];
    }

    return 1;
}
//----------------------------------------------------------------------------------------------------
