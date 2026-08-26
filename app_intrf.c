/*
   Using from ui:
   After initiating the APP_INTRF with app_intrf_init(),
   run a loop.
   In the loop first call the nav_update function, then
   Retrieve CX* contexts (these cant be saved, they must be retrieved
   each loop cycle!).
   Check if the returned CX* are not NULL !!!!
   Display the contexts to the user.
   Let the user interact with the displayed interface by calling the
   various nav_ functions.
   In principal the context structure could be traversed without cx_selected.
   However, since the various CX* has to be retrieved each cycle after
   nav_update cx_selected is the only way to remember what the user did.
*/

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
#include "util_funcs/hash_table.h"
#include <stdint.h>
#include <string.h>
#include "app_data.h"
#include "types.h"
#include "util_funcs/log_funcs.h"
#include <stdio.h>
#include <stdlib.h>

// TODO TODAY.
// CONTEXT UNIQUE IDS implementation.
// Start with returning of the communications with unique ids. Might be good
// idea to implement this using uistates, hashmaps. The UI layer could be a
// separate api, that user can use by default or create their own.

// MAIN IDEA: contexts is the World, uilayer is the user view of the World.

//  use events, uilayer removes all references of the
//  contextid when it gets an event from the context layer that it was removed.
//  This way tombstones will not increase the memory. Each uistate has to have a
//  separate cursor that reads the context layer events.

// Instead of app_data switch types implement typed opaque data + operation
// structs/functions ALSO instead of dirty functions, all data that can be
// returned to context should have a generation number Context can get
// generation number and if it is not equal to the current generation number it
// can remove and repopulate this contexts' (but not the context itself)
// children.
// SO remove all _USER_DATA and _FLAGS from types.h. If UI needs to get flags
// these should be in app_intrf.h. All functions that return string should
// return const char* instead of return_string in arguments. Then, wont need to
// synchronize defines between layer because of string lengths
/*
struct DataObject {
    const DataOps *ops;   // how to operate on it
    DataId id;            // identity
    void *user_data;      // where the actual state lives
};

typedef enum {
    DATA_CAP_NAME        = 1 << 0,
    DATA_CAP_CHILDREN    = 1 << 1,
    DATA_CAP_ACTIONS     = 1 << 2,
    DATA_CAP_RENAME      = 1 << 3,
} DataCapabilities;

typedef struct {
    DataCapabilities capabilities;

    DataIterator *(*children)(void *user_data);
    uint16_t (*flags)(void *user_data);
    bool (*name)(void *user_data, char *buffer, size_t buffer_len);
} DataOps;

static const DataOps directory_ops = {
    .capabilities = DATA_CAP_CHILDREN | DATA_CAP_NAME,
    .children = directory_children,
    .flags = directory_flags,
    .name = directory_name,
};

static DataIterator *directory_children(void* user_data)
{
    Directory *dir = (Directory *)user_data;

    ...
}

// in functions that return DataObjects:
DataObject result = {
    .ops = &directory_ops,
    .id = id,
    .user_data = existing_directory_data
};

//this is for the context layer to use
DataIterator *data_children(void* user_data)
{
    return object->ops->children(user_data);
}
*/

// The UI can then separate showing names of contexts; current context; selected
// contexts; last_visited; currently visited etc. And: actions of the context
// (add, remove, rename, etc.). It can even show separatly the actions for the
// current context and its selected children. also, this way there can be
// multiply ui states (named groups now) and they can even appear/disappear
// dynamically.

// NEW INKOVE:
//  Action type will say "this context can be entered and has children", "this
//  context can be invoked", "this context can be removed", "this context can be
//  renamed", "this context has a value, and it can be changed", "this context
//  can create contexts", etc. Implementation should be action invocation +
//  action options (to create the add lists, save file requests and similar). UI
//  gets possible actions on context; asks for action options (arguments);
//  depending on that displays choices (generated on data layer or not) for user
//  or calls command_execute() on context layer Also, this way UI can get
//  possible actions and if an action needs options it can for example hide the
//  whole interface and only show the choices or input (for rename operation).

//  For choice arguments to be generic and work with categories ContextChoice struct
//  should have 
/*
typedef struct {
    const char *key;
    const char *value;
} ContextAttribute;

typedef struct {
    DataId id;

    const char *label;
    const char *description;

    ContextAttribute *attributes;
    size_t attribute_count;

} ContextChoice;
*/
//  Then attributes can be expanded in the future ("version", "provider" etc.)

//  SO in
//  plugins.c and clap_plugins.c the plugin lists have to have stable ids. Then UI can own an item list and
//  dispatch an action for the item. Context uses the unique key given by the UI
//  and asks the data layer to do the action with the item. Data checks, if the
//  item with the key exists, if not - the context can give the result back to UI
//  layer and it can refresh the item list (request the list items from the context again and rebuild the list that the UI owns).
/*
typedef struct ActionSelectionModel
    ActionSelectionModel;

ActionSelectionModel *
context_action_selection_begin(
    Context *ctx,
    ActionId action);

const ActionNode *
action_selection_root(...);

size_t
action_selection_children(
    ...);

void
action_selection_destroy(...);

IN UI (pseudo)
void handle_node_click(
    ActionSelectionModel *model,
    ActionNodeId node_id)
{
    const ActionNode *node =
        action_selection_get_node(
            model,
            node_id);

    if (node->type == ACTION_NODE_CATEGORY) {

        ui_navigate_into(node_id);

    } else if (node->type == ACTION_NODE_CHOICE) {

        ActionInvocation invocation = {
            .action = model->action,
            .choice_id = node_id
        };

        context_dispatch(
            model->target_context,
            &invocation);
    }
}
*/
// for example (instead of context* should be contex unique id):
/*
IN DATA:
bool app_data_can_remove(void user_data);
bool app_data_can_rename(void user_data);
and similar functions

IN CONTEXT:

typedef enum {
    CONTEXT_COMMAND_REMOVE,
    CONTEXT_COMMAND_RENAME,
} ContextRequestType;

typedef struct {
    ContextRequestType type;

    union {
        struct {
        } remove;

        struct {
            const char *name;
        } rename;
        struct {
            ContextChoice choice;
        } choice;
    };
} ContextRequest;

typedef enum {
    CONTEXT_ACTION_STYLE_NORMAL,
    CONTEXT_ACTION_STYLE_DANGEROUS,
} ContextActionStyle;

typedef enum {
    CONTEXT_ACTION_REMOVE,
    CONTEXT_ACTION_RENAME,
} ContextActionType;

typedef struct {
    ContextActionType type;
    const char *label;
    const char *tooltip;
    bool enabled;
    ContextActionStyle style;
} ContextAction;

typedef enum {
    CONTEXT_OK = 0,
    CONTEXT_ERR_INVALID,
    CONTEXT_ERR_NOT_ALLOWED,
    CONTEXT_ERR_DATA,
} ContextResult;

size_t context_get_actions(
    const Context *ctx,
    ContextAction *actions,
    size_t capacity
)
{
    size_t count = 0;

    if (!ctx || !actions)
        return 0;

    if (count < capacity) {
        actions[count++] = (ContextAction) {
            .type = CONTEXT_ACTION_REMOVE,
            .enabled = context_can_remove(ctx),
            .label = "Remove"
        };
    }

    if (count < capacity) {
        actions[count++] = (ContextAction) {
            .type = CONTEXT_ACTION_RENAME,
            .enabled = context_can_rename(ctx),
            .label = "Rename"
        };
    }

    return count;
}

size_t context_get_action_arguments(
    Context *ctx,
    ActionId action,
    ActionArgument *arguments,
    size_t capacity)
{
    if (!ctx || !arguments)
        return 0;

    switch (action) {

    case ACTION_REMOVE:
        return 0;

    case ACTION_RENAME:
        if (capacity < 1)
            return 0;

        arguments[0] = (ActionArgument) {
            .name = "name",
            .label = "New name",
            .type = ACTION_VALUE_STRING,
            .required = true
        };

        return 1;

    case ACTION_ADD_NEW:
        if (capacity < 1)
            return 0;

        arguments[0] = (ActionArgument) {
            .name = "item_type",
            .label = "Type",
            .type = ACTION_VALUE_CHOICE,
            .required = true,
            for choice the ui should call action_get_action_choices()
                and then use the opaque struct that it got to iterate through
the items

        };

        return 1;

    default:
        return 0;
    }
}
ContextResult context_execute(
    Context *ctx,
    const ContextCommand *cmd)
{
    if (!ctx || !cmd)
        return CONTEXT_ERR_INVALID;

    switch (cmd->type) {

    case CONTEXT_COMMAND_REMOVE:
        return context_execute_remove(ctx);

    case CONTEXT_COMMAND_RENAME:
        return context_execute_rename(
            ctx,
            cmd->rename.name
        );

    default:
        return CONTEXT_ERR_INVALID;
    }
}
ON UI LAYER:
void ui_show_context_menu(Context *ctx)
{
    ContextAction actions[16];

    size_t count =
        context_get_actions(
            ctx,
            actions,
            16
        );

    for (size_t i = 0; i < count; ++i) {
        ui_menu_add(
            actions[i].label,
            actions[i].enabled,
            actions[i].type
        );
    }
}

UI GETS ACTION RENAME:
context_get_action_arguments(
    parent,
    CONTEXT_ACTION_RENAME,
    arguments,
    16
);
should use the returned arguments in this function
void ui_rename_selected(Context *ctx)
{
    char name[256];

    if (!ui_get_text("New name", name, sizeof(name)))
        return;

    ContextCommand cmd = {
        .type = CONTEXT_COMMAND_RENAME,
        .rename = {
            .name = name
        }
    };

    ContextResult result =
        context_execute(ctx, &cmd);

    if (result != CONTEXT_OK) {
        ui_show_error(result);
    }
}
*/

// Introduce connected CX* to app_intrf. 
// Implement Port connectivity, test sound. 
// Also will need memory slots per group (arrays of CX* per group). This will be useful for port connectivity so user
// can select many ports (put them into memory slots) and then disconnect or connect with one button.
// AFTER TODAY. Implement Params: Must be able to
// change amount of params during runtime Remove unecessary various log
// conversion methods in params, instead use the string callback function (like
// in clap plugin parameters)

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
   TODO one ui implementation to try is similar to cli programs:
   couple commands to interact with interface, for example 'ls' to list context children
   just to show that wildly different ui interfaces are possible
*/

/*
   TODO another ui implementation: 
   Daemon that accepts commands through an ip address.
   Could be used to control the program through web browser, a phone.
   Also could be used to display the interface on a phone and controlled through 
   keybindings (a combination of interfaces).
*/

/*
 TODO when implementing clay or other ui, test mouse clicking;
 scrolling(would be nice to able to scroll any element with contents that do
 not fit) and selecting as soon as possible.
*/

typedef struct _cx_array{
    unsigned int count;
    unsigned int count_max;
    struct _cx **contexts;
} CX_ARRAY;

typedef struct _cx_uniqueid{
    uint32_t id;
    uint32_t gen;
    uint64_t key;
}CX_ID;

typedef struct _cx {
    // name for display
    char short_name[MAX_PARAM_NAME_LENGTH];
    struct _cx_uniqueid uid; //unique context id
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
    CX_ID next_uid; //uniqueid for the next context
    HashTable* cx_hashtable; //hash table that links next_uid->key to a context

    uint16_t main_user_data_type; // type for the main user_data struct, the
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
    // check this user_data for dirty, if it is dirty, need to remove all of
    // its children cx and create them again.
    bool (*data_is_dirty)(void *user_data, uint16_t type);
    // destroy the whole data, user_data is the data from the cx_root CX. Used
    // when closing the app
    void (*data_destroy)(void *user_data, uint16_t type);
} APP_INTRF;

// pop the child from the context structure 
static void app_intrf_cx_children_pop(APP_INTRF* app_intrf, CX* cx_rem){
    if(!app_intrf)
        return;
    if(!cx_rem)
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
            }
        }
    }

    //remove the cx_rem
    if(cx_rem->cx_children.contexts)free(cx_rem->cx_children.contexts);
    ht_remove(app_intrf->cx_hashtable, cx_rem->uid.key);
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
    if(!parent->cx_children.contexts){
        parent->cx_children.contexts =
            calloc(parent->cx_children.count_max, sizeof(CX *));
        if (!parent->cx_children.contexts) {
            return -1;
        }
    }
    else if (nmemb >= parent->cx_children.count_max){
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

    CX *new_cx = calloc(1, sizeof(CX));
    if (!new_cx)
        return NULL;

    new_cx->flags = flags;
    new_cx->uid.id = app_intrf->next_uid.id;
    new_cx->uid.gen = app_intrf->next_uid.gen;
    new_cx->uid.key = ht_make_key(new_cx->uid.id, new_cx->uid.gen);
    app_intrf->next_uid.id += 1;
    new_cx->cx_children.contexts = NULL;
    new_cx->cx_children.count = 0;
    new_cx->cx_children.count_max = PTR_ARRAY_COUNT;
    new_cx->cx_parent = parent_cx;
    new_cx->user_data = user_data;
    new_cx->user_data_type = user_data_type;
    new_cx->idx = -1;

    snprintf(new_cx->short_name, MAX_PARAM_NAME_LENGTH, "%s", short_name);

    if (new_cx->cx_parent) {
        // add this cx to the parent cx array
        if (app_intrf_cx_children_push(app_intrf, new_cx) != 1) {
            app_intrf_cx_children_pop(app_intrf, new_cx);
            return NULL;
        }
    }

    // put the new CX into the hashtable
    if(ht_set(app_intrf->cx_hashtable, new_cx->uid.key, (void*)new_cx) != 0){
        app_intrf_cx_children_pop(app_intrf, new_cx);
        return NULL;
    }
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
    app_intrf->data_is_dirty = app_data_is_dirty;
    app_intrf->data_destroy = app_stop_and_clean;

    if (!app_intrf->data_child_return) {
        app_intrf_destroy(app_intrf);
        return NULL;
    }
    //--------------------------------------------------
    app_intrf->next_uid.gen = 0;
    app_intrf->next_uid.id = 1;
    app_intrf->cx_hashtable = ht_create(32);
    if(!app_intrf->cx_hashtable){
        app_intrf_destroy(app_intrf);
        return NULL;
    }

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

    return app_intrf;
}

// iterate from root_cx through the children recursively and call the void user func
// ok to use callback to remove cx but not to create (untested)
// root_cx - the cx from which to start iterating
// top_cx - should be same as root_cx, so iterating func knows the top cx
// leave_top - if 1, do not call callback_func for the top level cx
static void app_intrf_cx_children_iterate(
    APP_INTRF *app_intrf, CX *root_cx, CX *top_cx, unsigned int leave_top,
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

        if (go_inside == 1)
            app_intrf_cx_children_iterate(app_intrf, cur_cx, top_cx, leave_top,
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

//TEMP FUNC for testing
//print the id and gen per context
static void print_id_gen(APP_INTRF* app_intrf, CX* cur_cx){
    printf("key: %"PRIu64"\n", cur_cx->uid.key);
}

void app_intrf_destroy(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    // clean the data
    if (app_intrf->data_destroy)
        app_intrf->data_destroy(app_intrf->main_user_data,
                                app_intrf->main_user_data_type);

    //TEMP FOR TESTING printout all contexts ids and gens
    app_intrf_cx_children_iterate(app_intrf, app_intrf->cx_root, app_intrf->cx_root, 0, print_id_gen);
    // remove the cx structure
    app_intrf_cx_children_iterate(app_intrf, app_intrf->cx_root,
                                  app_intrf->cx_root, 0,
                                  app_intrf_cx_children_pop);

    ht_destroy(app_intrf->cx_hashtable, NULL);
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
    // but leave the cur_cx context 
    app_intrf_cx_children_iterate(app_intrf, cur_cx, cur_cx, 1,
                                  app_intrf_cx_children_pop);
    // create the children inside cur_cx again
    app_intrf->next_uid.gen += 1;
    app_intrf->next_uid.id = 0;
    app_intrf_cx_children_create(app_intrf, cur_cx);
}

// functions for the ui layer
void nav_update(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    if (app_intrf->data_update)
        app_intrf->data_update(app_intrf->main_user_data,
                               app_intrf->main_user_data_type);
    // iterate the whole structure and check if any CX are dirty
    app_intrf_cx_children_iterate(app_intrf, app_intrf->cx_root,
                                  app_intrf->cx_root, 0,
                                  app_intrf_cx_check_dirty);
}

uint64_t nav_cx_root_return(APP_INTRF* app_intrf){
    if(!app_intrf)return 0;

    return app_intrf->cx_root->uid.key; 
}

const char* nav_cx_display_name_return(APP_INTRF *app_intrf, uint64_t key){ 
    if (!app_intrf) {
        return NULL;
    }
    void* cx_data = ht_get(app_intrf->cx_hashtable, key);
    if (!cx_data)
        return NULL;
    CX* cx_curr = (CX*)cx_data;
    return cx_curr->short_name;
}
