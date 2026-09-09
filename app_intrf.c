#include "app_intrf.h"
#include "util_funcs/hash_table.h"
#include <stdint.h>
#include <string.h>
#include "app_data.h"
#include "data_object.h"
#include "data_events.h"
#include "types.h"
#include "util_funcs/log_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

// TODO TODAY.
//  use events, uilayer removes all references of the
//  contextid when it gets an event from the context layer that it was removed.
//  This way tombstones will not increase the memory. Each uistate has to have a
//  separate cursor that reads the context layer events.
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

// Implement Port connectivity, test sound. 
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

typedef struct _cx {
    // identity minted by the data layer, stable for this object's lifetime.
    // also the hash table key.
    ContextId data_id;
    int idx; // index number of this CX in the cx_parent cx_children array
    // the data layer object this cx represents. ops is static, user_data is
    // borrowed and MUST NOT be freed or modified here. The display name is not
    // stored, it is read live from data_name(&data) in nav_cx_name_return().
    DataObject data;
    struct _cx *cx_parent;

    //contexts array of children
    struct _cx_array cx_children;
} CX;

typedef struct _app_intrf {
    CX *cx_root;
    HashTable* cx_hashtable; //hash table that links ContextId -> CX*

    void *main_user_data; // the root DataObject's user_data, kept for the
                          // data_update / data_destroy calls
    // data function that updates its internal structures every cycle, should
    // be called first before any navigation
    void (*data_update)(void *root_user_data);
    // pop the next queued data event. returns false when the queue is empty.
    bool (*data_poll_event)(void *root_user_data, DataEvent *out);
    // destroy the whole data. root_user_data is the root DataObject's
    // user_data. Used when closing the app
    void (*data_destroy)(void *root_user_data);
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
            for (unsigned int i = 0; i < parent->cx_children.count; i++){
                CX* curr_cx = parent->cx_children.contexts[i];
                if(curr_cx == cx_rem){
                    child_idx = (int)i;
                    break;
                }
            }
            if (child_idx != -1) {
                // Remove the child_idx cx from the parent cx_children array
                unsigned int nmemb = parent->cx_children.count - 1;
                for (unsigned int i = (unsigned int)child_idx; i < nmemb; i++) {
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
    ht_remove(app_intrf->cx_hashtable, cx_rem->data_id);
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
                               DataObject data) {
    if (!app_intrf)
        return NULL;
    if (!data_obj_valid(&data))
        return NULL;

    ContextId cx_id = data_id(&data);
    if (cx_id == CONTEXT_ID_NULL)
        return NULL; // every context must have an identity from the data layer
    ASSERT_CTXID(cx_id); // debug: a well-formed id carries a non-zero namespace

    CX *new_cx = calloc(1, sizeof(CX));
    if (!new_cx)
        return NULL;

    new_cx->data_id = cx_id;
    new_cx->cx_children.contexts = NULL;
    new_cx->cx_children.count = 0;
    new_cx->cx_children.count_max = PTR_ARRAY_COUNT;
    new_cx->cx_parent = parent_cx;
    new_cx->data = data;
    new_cx->idx = -1;

    if (new_cx->cx_parent) {
        // add this cx to the parent cx array
        if (app_intrf_cx_children_push(app_intrf, new_cx) != 1) {
            app_intrf_cx_children_pop(app_intrf, new_cx);
            return NULL;
        }
    }

    // put the new CX into the hashtable
    if(ht_set(app_intrf->cx_hashtable, new_cx->data_id, (void*)new_cx) != 0){
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
    size_t child_count = data_child_count(&parent_cx->data);
    for (size_t iter = 0; iter < child_count; iter++) {
        DataObject child_data;
        if (!data_child_at(&parent_cx->data, iter, &child_data))
            continue;
        app_intrf_cx_create(app_intrf, parent_cx, child_data);
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
    app_intrf->data_update = app_data_update;
    app_intrf->data_poll_event = app_data_poll_event;
    app_intrf->data_destroy = app_stop_and_clean;
    //--------------------------------------------------
    app_intrf->cx_hashtable = ht_create(32);
    if(!app_intrf->cx_hashtable){
        app_intrf_destroy(app_intrf);
        return NULL;
    }

    DataObject root_obj = app_init();
    if (!data_obj_valid(&root_obj)) {
        app_intrf_destroy(app_intrf);
        return NULL;
    }
    app_intrf->main_user_data = root_obj.user_data;

    // create the cx_root
    app_intrf->cx_root = app_intrf_cx_create(app_intrf, NULL, root_obj);
    if (!app_intrf->cx_root) {
        app_intrf_destroy(app_intrf);
        return NULL;
    }
    // and create the cx_root children recursively
    app_intrf_cx_children_create(app_intrf, app_intrf->cx_root);

    return app_intrf;
}

// free cur_cx and its whole subtree, post-order: each child's subtree first,
// then unlink cur_cx from its parent, drop it from the hashtable, and free it.
// freeing a child removes it from cur_cx->cx_children (shifting the rest down),
// so we keep freeing index 0 until the array is empty.
static void cx_subtree_free(APP_INTRF *app_intrf, CX *cur_cx) {
    if (!app_intrf || !cur_cx)
        return;
    while (cur_cx->cx_children.count > 0)
        cx_subtree_free(app_intrf, cur_cx->cx_children.contexts[0]);
    app_intrf_cx_children_pop(app_intrf, cur_cx);
}

// re-sync parent_cx's direct children against the data layer, by identity:
//  - free the subtree of each CX whose data child is gone,
//  - create a CX (and materialise its subtree) for each data child not present,
//  - leave the rest untouched, so their ContextIds stay valid.
// only touches one level; deeper changes arrive as their own events.
static void app_intrf_cx_resync(APP_INTRF *app_intrf, CX *parent_cx) {
    if (!app_intrf || !parent_cx)
        return;

    size_t n = data_child_count(&parent_cx->data);

    // pass 1: drop CX whose data child is gone. walk backwards so the index
    // shift on removal does not make us skip an entry.
    for (int i = (int)parent_cx->cx_children.count - 1; i >= 0; i--) {
        CX *child = parent_cx->cx_children.contexts[i];
        bool still_there = false;
        for (size_t j = 0; j < n && !still_there; j++) {
            DataObject dobj;
            if (data_child_at(&parent_cx->data, j, &dobj) &&
                data_id(&dobj) == child->data_id)
                still_there = true;
        }
        if (!still_there)
            cx_subtree_free(app_intrf, child);
    }

    // pass 2: create CX for data children not yet materialised.
    for (size_t j = 0; j < n; j++) {
        DataObject dobj;
        if (!data_child_at(&parent_cx->data, j, &dobj))
            continue;
        ContextId cid = data_id(&dobj);
        bool have = false;
        for (unsigned int i = 0;
             i < parent_cx->cx_children.count && !have; i++)
            if (parent_cx->cx_children.contexts[i]->data_id == cid)
                have = true;
        if (have)
            continue;
        CX *created = app_intrf_cx_create(app_intrf, parent_cx, dobj);
        if (created)
            app_intrf_cx_children_create(app_intrf, created);
    }
}

// drain the data layer's event queue and reconcile the CX tree. synchronous:
// once this returns the tree matches the data layer.
static void app_intrf_process_data_events(APP_INTRF *app_intrf) {
    if (!app_intrf || !app_intrf->data_poll_event)
        return;
    DataEvent ev;
    while (app_intrf->data_poll_event(app_intrf->main_user_data, &ev)) {
        CX *cx = ht_get(app_intrf->cx_hashtable, ev.id);
        if (!cx)
            continue; // not materialised (or already gone) - nothing to do
        switch (ev.type) {
        case DATA_EVENT_CHILDREN_CHANGED:
            app_intrf_cx_resync(app_intrf, cx);
            break;
        case DATA_EVENT_CHANGED:
            // presentation-only; nav_cx_name_return reads live so there is
            // nothing structural to do. Phase E will forward this to the ui.
            break;
        }
    }
}

void app_intrf_destroy(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    // clean the data
    if (app_intrf->data_destroy)
        app_intrf->data_destroy(app_intrf->main_user_data);

    // remove the cx structure (cx_root has no parent - pop just frees it)
    if (app_intrf->cx_root)
        cx_subtree_free(app_intrf, app_intrf->cx_root);

    ht_destroy(app_intrf->cx_hashtable, NULL);
    free(app_intrf);
}

// functions for the ui layer
void nav_update(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    if (app_intrf->data_update)
        app_intrf->data_update(app_intrf->main_user_data);
    app_intrf_process_data_events(app_intrf);
}

ContextId nav_cx_root_return(APP_INTRF* app_intrf){
    if(!app_intrf)return CONTEXT_ID_NULL;

    return app_intrf->cx_root->data_id;
}

bool nav_cx_is_valid(APP_INTRF *app_intrf, ContextId context) {
    if (!app_intrf)
        return false;

    if (context == CONTEXT_ID_NULL)
        return false;

    CX *cx = ht_get(app_intrf->cx_hashtable, context);

    return cx != NULL;
}

const char *nav_cx_name_return(APP_INTRF *app_intrf, ContextId context)
{
    if (!app_intrf)
        return NULL;

    if (context == CONTEXT_ID_NULL)
        return NULL;

    CX *cx = ht_get(app_intrf->cx_hashtable, context);

    if (!cx)
        return NULL;

    // read live from the data layer. The returned string is owned by the data
    // layer and only valid until that data changes or goes away - callers must
    // copy if they need to keep it.
    return data_name(&cx->data);
}

size_t nav_cx_children_count(APP_INTRF *app_intrf, ContextId context)
{
    if (!app_intrf)
        return 0;

    if (context == CONTEXT_ID_NULL)
        return 0;

    CX *cx = ht_get(app_intrf->cx_hashtable, context);

    if (!cx)
        return 0;

    return (size_t)cx->cx_children.count;
}

ContextId nav_cx_child_at(APP_INTRF *app_intrf, ContextId parent,
                          size_t index)
{
    if (!app_intrf) return CONTEXT_ID_NULL;

    if (parent == CONTEXT_ID_NULL)
        return CONTEXT_ID_NULL;

    CX *parent_cx = ht_get(app_intrf->cx_hashtable, parent);

    if (!parent_cx)
        return CONTEXT_ID_NULL;

    if (index >= parent_cx->cx_children.count)
        return CONTEXT_ID_NULL;

    CX *child = parent_cx->cx_children.contexts[index];

    if (!child)
        return CONTEXT_ID_NULL;

    return child->data_id;
}

ContextId nav_cx_parent_return(APP_INTRF *app_intrf, ContextId context) {
    if (!app_intrf)
        return CONTEXT_ID_NULL;

    if (context == CONTEXT_ID_NULL)
        return CONTEXT_ID_NULL;

    CX *cx = ht_get(app_intrf->cx_hashtable, context);

    if (!cx)
        return CONTEXT_ID_NULL;

    if (!cx->cx_parent)
        return CONTEXT_ID_NULL;

    return cx->cx_parent->data_id;
}
