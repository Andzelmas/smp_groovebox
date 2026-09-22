#include "app_intrf.h"
#include "ids.h"
#include "util_funcs/hash_table.h"
#include <stdint.h>
#include <string.h>
#include "app_data.h"
#include "data_events.h"
#include "cx_events.h"
#include "types.h"
#include "util_funcs/log_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

// TODO
// TODAY. Implement Params: param value set, view in CLI
// Get rid of types.h if possible, and structs.h if not needed and logical too.

// TODO SAVING should be on the app_data layer. Implement with data_action (save
// on root context and the different modules)

/*  TODO another ui implementation: clay or other lib with graphical intrf
 * (maybe vulkan or opengl?)*/

// TODO another ui implementation: interface in Python.

/* TODO another ui implementation : Daemon that accepts commands through an ip
 * address.Could be used to control the program through web browser, a
 * phone.Also could be used to display the interface on a phone and controlled
 * through keybindings(a combination of interfaces) */

typedef struct _cx_array {
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
    // set by the MARK phase of app_intrf_cx_resync. A CX whose stamp does not
    // match the pass currently running is no longer in the data layer's tree
    // and gets swept. A fresh CX is calloc'd to 0 and stamps start at 1, so a
    // never-marked CX can never look current by accident.
    uint64_t sync_stamp;

    //contexts array of children
    struct _cx_array cx_children;
} CX;

// size of the context layer's change log. ui views consume it at their own
// pace, and a view that falls behind gets NAV_POLL_OVERFLOW and rebuilds.
#define APP_INTRF_CX_EVENT_RING 1024

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

    // context layer change log (see cx_events.h). cx_seq is the last assigned
    // sequence number (0 = nothing emitted); the slot for sequence s is
    // (s - 1) % APP_INTRF_CX_EVENT_RING.
    struct {
        uint64_t seq;
        CxEvent ev;
    } cx_ring[APP_INTRF_CX_EVENT_RING];

    uint64_t cx_seq;
    // emits are off during the initial tree build and during teardown (no
    // views can be listening then, and it would just churn the ring).
    bool cx_emit_enabled;
    // incremented once per app_intrf_cx_resync so every pass gets a stamp no
    // CX can already be carrying. See CX.sync_stamp.
    uint64_t sync_stamp;
} APP_INTRF;

// append one event to the change log. Overwrites the oldest slot silently;
// consumers detect that they missed events via their cursor (NAV_POLL_OVERFLOW).
static void app_intrf_emit(APP_INTRF *app_intrf, CxEventType type, ContextId id,
                           ContextId parent, size_t index) {
    if (!app_intrf || !app_intrf->cx_emit_enabled)
        return;
    uint64_t seq = ++app_intrf->cx_seq;
    size_t slot = (size_t)((seq - 1) % APP_INTRF_CX_EVENT_RING);
    app_intrf->cx_ring[slot].seq = seq;
    app_intrf->cx_ring[slot].ev = (CxEvent){
        .type = type, .id = id, .parent = parent, .index = index};
}

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

    // the tree is built; from here on structural changes are logged for the ui
    app_intrf->cx_emit_enabled = true;
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
    // post-order: children have already emitted their CX_REMOVED
    app_intrf_emit(app_intrf, CX_REMOVED, cur_cx->data_id,
                   cur_cx->cx_parent ? cur_cx->cx_parent->data_id
                                     : CONTEXT_ID_NULL,
                   cur_cx->idx >= 0 ? (size_t)cur_cx->idx : 0);
    app_intrf_cx_children_pop(app_intrf, cur_cx);
}

// MARK phase of app_intrf_cx_resync: walk the DATA subtree under `data` and
// stamp every CX that already represents one of its nodes. Creates nothing.
// parent_id is the ContextId of `data` itself, so a CX only counts as current
// if it is already sitting under the SAME parent - one that moved elsewhere is
// deliberately left unstamped, so the sweep frees it and the add phase
// re-creates it in its new place.
static void app_intrf_cx_mark_desired(APP_INTRF *app_intrf,
                                      const DataObject *data,
                                      ContextId parent_id, uint64_t stamp) {
    size_t n = data_child_count(data);
    for (size_t i = 0; i < n; i++) {
        DataObject child;
        if (!data_child_at(data, i, &child))
            continue;
        ContextId cid = data_id(&child);
        if (cid == CONTEXT_ID_NULL)
            continue;
        CX *cx = ht_get(app_intrf->cx_hashtable, cid);
        if (cx && cx->cx_parent && cx->cx_parent->data_id == parent_id)
            cx->sync_stamp = stamp;
        app_intrf_cx_mark_desired(app_intrf, &child, cid, stamp);
    }
}

// SWEEP phase: free every CX in parent_cx's subtree the MARK phase did not
// stamp. Walks backwards because cx_subtree_free shifts the array down, and
// only descends into survivors - an unstamped node takes its whole subtree
// with it (cx_subtree_free emits CX_REMOVED post-order for all of them).
static void app_intrf_cx_sweep(APP_INTRF *app_intrf, CX *parent_cx,
                               uint64_t stamp) {
    for (int i = (int)parent_cx->cx_children.count - 1; i >= 0; i--) {
        CX *child = parent_cx->cx_children.contexts[i];
        if (!child)
            continue;
        if (child->sync_stamp != stamp) {
            cx_subtree_free(app_intrf, child);
            continue;
        }
        app_intrf_cx_sweep(app_intrf, child, stamp);
    }
}

// put parent_cx's child array into data order, keeping every idx equal to its
// array position. nav_cx_child_at reads display order straight off this array,
// so it has to match what the data layer reports. Children the data layer no
// longer lists are left after the ordered ones rather than dropped.
static void app_intrf_cx_children_reorder(APP_INTRF *app_intrf, CX *parent_cx) {
    size_t n = data_child_count(&parent_cx->data);
    unsigned int count = parent_cx->cx_children.count;
    unsigned int w = 0;
    for (size_t i = 0; i < n && w < count; i++) {
        DataObject child;
        if (!data_child_at(&parent_cx->data, i, &child))
            continue;
        ContextId cid = data_id(&child);
        if (cid == CONTEXT_ID_NULL)
            continue;
        CX *cx = ht_get(app_intrf->cx_hashtable, cid);
        if (!cx || cx->cx_parent != parent_cx)
            continue;
        if (cx->idx < 0 || (unsigned int)cx->idx >= count)
            continue;
        unsigned int cur = (unsigned int)cx->idx;
        if (cur != w) {
            CX *displaced = parent_cx->cx_children.contexts[w];
            parent_cx->cx_children.contexts[w] = cx;
            parent_cx->cx_children.contexts[cur] = displaced;
            cx->idx = (int)w;
            displaced->idx = (int)cur;
        }
        w++;
    }
}

// ADD phase: create a CX for every data node that has none, top down. A node
// created here has its whole subtree materialised by app_intrf_cx_children_
// create, so there is nothing left to descend into; only an already-existing
// node is recursed through.
//
// Creation appends, so the level is ordered once after it is built and only
// then announced - CX_ADDED carries the final index. Survivors were stamped
// by the MARK phase, so an unstamped child is one this pass created.
static void app_intrf_cx_add_missing(APP_INTRF *app_intrf, CX *parent_cx,
                                     uint64_t stamp) {
    size_t n = data_child_count(&parent_cx->data);
    for (size_t i = 0; i < n; i++) {
        DataObject child;
        if (!data_child_at(&parent_cx->data, i, &child))
            continue;
        ContextId cid = data_id(&child);
        if (cid == CONTEXT_ID_NULL)
            continue;
        // already ours, or living under a DIFFERENT parent the sweep didn't
        // reach - creating over that would ht_set the id and strand the old CX
        if (ht_get(app_intrf->cx_hashtable, cid))
            continue;
        CX *created = app_intrf_cx_create(app_intrf, parent_cx, child);
        if (!created)
            continue;
        app_intrf_cx_children_create(app_intrf, created);
    }

    app_intrf_cx_children_reorder(app_intrf, parent_cx);

    for (unsigned int i = 0; i < parent_cx->cx_children.count; i++) {
        CX *cx = parent_cx->cx_children.contexts[i];
        if (!cx)
            continue;
        if (cx->sync_stamp != stamp) {
            // announce the new node only - its subtree is materialised fresh
            // and no view could be tracking those ids yet
            app_intrf_emit(app_intrf, CX_ADDED, cx->data_id,
                           parent_cx->data_id, (size_t)i);
            continue;
        }
        app_intrf_cx_add_missing(app_intrf, cx, stamp);
    }
}

// re-sync parent_cx's whole subtree against the data layer by identity.
// Survivors are left untouched, so their ContextIds - and anything a ui view
// filed under them - stay valid.
//
// It diffs the whole subtree, not one level, because the data layer nests
// (params inside categories) and a change below the first level is invisible
// to a single-level comparison. Every removal MUST land before any addition:
// the same ContextId can leave one parent and arrive under another in a single
// reconciliation, and adding first would ht_set the new CX and then ht_remove
// that id on the way out, leaving a live CX that nothing can look up again.
static void app_intrf_cx_resync(APP_INTRF *app_intrf, CX *parent_cx) {
    if (!app_intrf || !parent_cx)
        return;

    uint64_t stamp = ++app_intrf->sync_stamp;
    app_intrf_cx_mark_desired(app_intrf, &parent_cx->data,
                              parent_cx->data_id, stamp);
    app_intrf_cx_sweep(app_intrf, parent_cx, stamp);
    app_intrf_cx_add_missing(app_intrf, parent_cx, stamp);
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
            // presentation-only (name/value); nothing structural. forward it so
            // ui views can refresh what they draw for this context.
            app_intrf_emit(app_intrf, CX_CHANGED, cx->data_id,
                           cx->cx_parent ? cx->cx_parent->data_id
                                         : CONTEXT_ID_NULL,
                           cx->idx >= 0 ? (size_t)cx->idx : 0);
            break;
        }
    }
}

// run one synchronous cycle: pull fresh state from the data layer, then
// drain its event queue and reconcile the CX tree against it. Shared by
// nav_update (per-frame) and nav_cx_action_do
static void app_intrf_sync(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    if (app_intrf->data_update)
        app_intrf->data_update(app_intrf->main_user_data);
    app_intrf_process_data_events(app_intrf);
}

void app_intrf_destroy(APP_INTRF *app_intrf) {
    if (!app_intrf)
        return;
    app_intrf->cx_emit_enabled = false; // no events during teardown
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
    app_intrf_sync(app_intrf);
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

void nav_cursor_init(APP_INTRF *app_intrf, NavCursor *cursor) {
    if (!cursor)
        return;
    // start just past the newest event: the view sees only future changes
    cursor->next_seq = app_intrf ? app_intrf->cx_seq + 1 : 1;
}

NavPollResult nav_poll_event(APP_INTRF *app_intrf, NavCursor *cursor,
                             CxEvent *out) {
    if (!app_intrf || !cursor || !out)
        return NAV_POLL_EMPTY;

    if (cursor->next_seq > app_intrf->cx_seq)
        return NAV_POLL_EMPTY; // caught up (or nothing has been emitted yet)

    // oldest sequence still retained in the ring
    uint64_t oldest = (app_intrf->cx_seq > APP_INTRF_CX_EVENT_RING)
                          ? app_intrf->cx_seq - APP_INTRF_CX_EVENT_RING + 1
                          : 1;
    if (cursor->next_seq < oldest) {
        cursor->next_seq = app_intrf->cx_seq + 1; // resync from the head
        return NAV_POLL_OVERFLOW;
    }

    size_t slot = (size_t)((cursor->next_seq - 1) % APP_INTRF_CX_EVENT_RING);
    *out = app_intrf->cx_ring[slot].ev;
    cursor->next_seq += 1;
    return NAV_POLL_EVENT;
}

// general context capabilities available to the user
const char* nav_cx_value_as_string(APP_INTRF* app_intrf, ContextId context){
    if(!app_intrf || context == CONTEXT_ID_NULL)
        return NULL;
    CX* cx = ht_get(app_intrf->cx_hashtable, context);
    if(!cx)
        return NULL;
    return data_value_as_string(&cx->data); 
};
bool nav_cx_is_hidden(APP_INTRF* app_intrf, ContextId context){
    if(!app_intrf || context == CONTEXT_ID_NULL)
        return false;
    CX* cx = ht_get(app_intrf->cx_hashtable, context);
    if(!cx)
        return false;
    return data_is_hidden(&cx->data); 
}

// ACTIONS - pure pass-throughs: resolve context to its CX, call through to
// the DataObject. app_intrf knows no specific action, arg or list; see
// data_actions.h for the contract these all share end to end.
size_t nav_cx_actions(APP_INTRF *app_intrf, ContextId context, DataAction *out,
                      size_t cap) {
    if (!app_intrf || context == CONTEXT_ID_NULL)
        return 0;
    CX *cx = ht_get(app_intrf->cx_hashtable, context);
    if (!cx)
        return 0;
    return data_action_list(&cx->data, out, cap);
}

size_t nav_cx_action_args(APP_INTRF *app_intrf, ContextId context,
                          DataActionType type, DataArgSpec *out, size_t cap) {
    if (!app_intrf || context == CONTEXT_ID_NULL)
        return 0;
    CX *cx = ht_get(app_intrf->cx_hashtable, context);
    if (!cx)
        return 0;
    return data_action_args(&cx->data, type, out, cap);
}

size_t nav_cx_list_count(APP_INTRF *app_intrf, ContextId context,
                         DataListId list, const DataActionReq *partial) {
    if (!app_intrf || context == CONTEXT_ID_NULL)
        return 0;
    CX *cx = ht_get(app_intrf->cx_hashtable, context);
    if (!cx)
        return 0;
    return data_list_count(&cx->data, list, partial);
}

bool nav_cx_list_at(APP_INTRF *app_intrf, ContextId context, DataListId list,
                    const DataActionReq *partial, size_t idx,
                    DataChoice *out) {
    if (!out)
        return false;
    out->value = 0;
    out->label = NULL;
    out->flags = 0;
    if (!app_intrf || context == CONTEXT_ID_NULL)
        return false;
    CX *cx = ht_get(app_intrf->cx_hashtable, context);
    if (!cx)
        return false;
    return data_list_at(&cx->data, list, partial, idx, out);
}

DataActionResult nav_cx_action_do(APP_INTRF *app_intrf, ContextId context,
                                  const DataActionReq *req,
                                  ContextId *out_new) {
    if (out_new)
        *out_new = CONTEXT_ID_NULL;
    if (!app_intrf || context == CONTEXT_ID_NULL || !req)
        return DATA_ACTION_ERR_INVALID;
    CX *cx = ht_get(app_intrf->cx_hashtable, context);
    if (!cx)
        return DATA_ACTION_ERR_INVALID;

    ContextId new_id = CONTEXT_ID_NULL;
    DataActionResult result = data_action_do(&cx->data, req, &new_id);

    // synchronously reconcile before returning to the caller, so
    // the CX tree, its change log, and out_new below are already consistent
    // with whatever the action just did.
    app_intrf_sync(app_intrf);

    // only hand new_id back once it actually resolves to a materialised CX -
    // the resync above is what creates it. Defensive: if for some reason it
    // didn't (e.g. the action's container wasn't the one that changed),
    // *out_new stays CONTEXT_ID_NULL rather than naming a dangling id.
    if (out_new && new_id != CONTEXT_ID_NULL &&
        ht_get(app_intrf->cx_hashtable, new_id))
        *out_new = new_id;

    return result;
}
