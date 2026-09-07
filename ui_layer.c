#include "ui_layer.h"
#include "app_intrf.h"
#include <stdbool.h>
#include <stdlib.h>

//UI_STATE_ENTRY* initial size, must be power of two
#define ENTRIES_INIT_CAPACITY 16
#define ENTRIES_MAX_LOAD_FACTOR 0.75
#define ENTRIES_MIN_LOAD_FACTOR 0.10

typedef enum{
    ENTRY_EMPTY,
    ENTRY_OCCUPIED,
    ENTRY_DELETED
}EntryState;

typedef struct _ui_target_list{
    ContextId *items;
    size_t count;
    size_t capacity;
}UI_TARGET_LIST;

typedef struct _ui_state_entry{
    ContextId context;
    UiPurpose purpose;
    UI_TARGET_LIST targets;
    EntryState state;
}UI_STATE_ENTRY;

typedef struct _ui_state{
    UI_STATE_ENTRY* entries;
    size_t count;
    size_t capacity;
    
    size_t borrow_count;
}UI_STATE;

typedef struct _ui_layer{
    APP_INTRF* app_intrf;
}UI_LAYER;

static void ui_layer_nav_target_list_destroy(UI_TARGET_LIST *targets){
    if(!targets)
        return;
   targets->capacity = 0;
   targets->count = 0;
   if(targets->items)
       free(targets->items);
   targets->items = NULL;
}

static bool ui_layer_nav_target_list_init(UI_TARGET_LIST* targets, size_t capacity){
    if(!targets || capacity == 0)
        return false;

    targets->items = calloc(capacity, sizeof(ContextId));
    if(!targets->items)
        return false;

    targets->capacity = capacity;
    targets->count = 0;

    return true;
}

static size_t entries_hash_key(ContextId context, UiPurpose purpose, size_t entries_capacity){
    ContextId h = context;

    h ^= (ContextId)purpose + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);

    // final mixing
    h ^= h >> 30;
    h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> 27;
    h *= UINT64_C(0x94d049bb133111eb);
    h ^= h >> 31;
    return (size_t)h & (entries_capacity - 1);
}

static int entries_resize(UI_STATE* state, size_t new_capacity)
{
    if(!state || new_capacity == 0)
        return -1;

    UI_STATE_ENTRY *old_entries = state->entries;
    size_t old_capacity = state->capacity;

    UI_STATE_ENTRY *new_entries = calloc(new_capacity, sizeof(UI_STATE_ENTRY));

    if (!new_entries)
        return -1;

    state->entries = new_entries;
    state->capacity = new_capacity;
    state->count = 0;

    for (size_t i = 0; i < old_capacity; i++) {
        UI_STATE_ENTRY *old = &old_entries[i];

        if (old->state != ENTRY_OCCUPIED) 
            continue;

        size_t index = entries_hash_key(old->context, old->purpose, state->capacity);

        for (size_t j = 0; j < state->capacity; j++) {
            size_t pos = (index + j) % state->capacity;
            UI_STATE_ENTRY *entry = &state->entries[pos];

            if (entry->state != ENTRY_OCCUPIED) {
                *entry = *old;
                entry->state = ENTRY_OCCUPIED;
                state->count++;
                break;
            }
        }
    }

    free(old_entries);
    return 0;
}

static bool entries_create(UI_STATE* state, size_t capacity)
{
    if (capacity == 0){
        capacity = ENTRIES_INIT_CAPACITY;
    }

    state->entries =
        calloc(capacity, sizeof(UI_STATE_ENTRY));

    if (!state->entries) {
        return false;
    }

    state->capacity = capacity;
    state->count = 0;
    state->borrow_count = 0;
    for(size_t i = 0; i< state->capacity; i++){
        UI_STATE_ENTRY* entry = &state->entries[i];
        ui_layer_nav_target_list_destroy(&entry->targets);
    }

    return true;
}

UI_LAYER* ui_layer_init(){
    APP_INTRF* app_intrf = app_intrf_init();
    if(!app_intrf)
        return NULL;

    UI_LAYER* ui_layer = calloc(1, sizeof(UI_LAYER));
    if(!ui_layer){
        app_intrf_destroy(app_intrf);
        return NULL;
    }

    ui_layer->app_intrf = app_intrf;
    return ui_layer;
}

UI_STATE* ui_layer_state_init(UI_LAYER* ui_layer){
    if(!ui_layer)
        return NULL;

    UI_STATE* state = calloc(1, sizeof(UI_STATE));
    if(!state)
        return NULL;

    if(entries_create(state, ENTRIES_INIT_CAPACITY) != true){
        ui_layer_state_clear(state);
        return NULL;
    }
    return state;
}

void ui_layer_state_clear(UI_STATE* state){
    if(!state)
        return;
    if(state->entries){
        for(size_t i = 0; i < state->capacity; i++){
            UI_STATE_ENTRY* entry = &state->entries[i];
            ui_layer_nav_target_list_destroy(&entry->targets);
        }
        free(state->entries);
        state->capacity = 0;
        state->count = 0;
    }
    free(state);
}

void ui_layer_destroy(UI_LAYER* ui_layer, UI_STATE **states, size_t states_count){
    if(!ui_layer)
        return;
    // free the ui_layer
    app_intrf_destroy(ui_layer->app_intrf);
    free(ui_layer);

    if(!states || states_count < 1)
        return;
    // cleanup the states here
    for(size_t i = 0; i < states_count; i++){
        UI_STATE* state = states[i];
        ui_layer_state_clear(state);
    }
}

bool ui_layer_state_entry_set(UI_STATE* state, ContextId context, UiPurpose purpose, size_t capacity){
    if(!state)
        return false;

    if(state->borrow_count != 0)
        return false;

    size_t index = entries_hash_key(context, purpose, state->capacity);
    /*
     * Resize before inserting if necessary.
     */
    if ((double)(state->count + 1) / state->capacity >
        ENTRIES_MAX_LOAD_FACTOR) {

         // Prevent size_t overflow.
        if (state->capacity > SIZE_MAX / 2)
            return false;

        size_t new_capacity = state->capacity * 2;

        if (entries_resize(state, new_capacity) != 0)
            return false;

         // Capacity changed, so the bucket index must
         // be recalculated.
        index = entries_hash_key(context, purpose, state->capacity);
    }

    size_t deleted_index = SIZE_MAX;

    for (size_t i = 0; i < state->capacity; i++) {
        UI_STATE_ENTRY *entry =
            &state->entries[(index + i) % state->capacity];

        if (entry->state == ENTRY_EMPTY) {
            /*
             * Reuse the first deleted slot, if one was found.
             */
            if (deleted_index != SIZE_MAX)
                entry = &state->entries[deleted_index];

            if(!ui_layer_nav_target_list_init(&entry->targets, capacity))
                return false;
            entry->context = context;
            entry->purpose = purpose;
            entry->state = ENTRY_OCCUPIED;
            state->count++;

            return true;
        }

        if (entry->state == ENTRY_DELETED) {
            if (deleted_index == SIZE_MAX)
                deleted_index = (index + i) % state->capacity;

            continue;
        }

        /*
         ENTRY_OCCUPIED so create the targets list if it is empty
         otherwise do nothing
         */
        if (entry->context == context && entry->purpose == purpose) {
            if(!entry->targets.items){
                if(!ui_layer_nav_target_list_init(&entry->targets, capacity))
                    return false;
            }

            return true;
        }
    }

    return false;
}

bool ui_layer_state_entry_remove(UI_STATE *state, ContextId context, UiPurpose purpose){
    if(!state)
        return false;


    if(state->borrow_count != 0)
        return false;

    size_t index = entries_hash_key(context, purpose, state->capacity); 

    for (size_t i = 0; i < state->capacity; ++i) {
        UI_STATE_ENTRY *entry = &state->entries[index];

        if (entry->state == ENTRY_EMPTY) {
            /*
             An empty slot terminates the probe sequence.
             The key cannot exist further along this sequence.
             */
            return false;
        }

        if (entry->state == ENTRY_OCCUPIED && entry->context == context && entry->purpose == purpose) {

            /*
             * Do not make this ENTRY_EMPTY: that could break the
             * probe sequence for entries that were inserted later.
             */
            entry->state = ENTRY_DELETED;
            state->count--;
            ui_layer_nav_target_list_destroy(&entry->targets);

            /*
             * Shrink the table if it has become sparse.
             */
            if (state->capacity > ENTRIES_INIT_CAPACITY &&
                (double)state->count / state->capacity < ENTRIES_MIN_LOAD_FACTOR) {

                size_t new_capacity = state->capacity / 2;

                if (new_capacity < ENTRIES_INIT_CAPACITY)
                    new_capacity = ENTRIES_INIT_CAPACITY;

                /*
                 * Failure to shrink is not an error.
                 * The existing table remains valid.
                 */
                (void)entries_resize(state, new_capacity);
            }

            return true;
        }

        /*
         * ENTRY_DELETED and non-matching EMPTY_OCCUPIED entries
         * continue the probe sequence.
         */
        index = (index + 1) % state->capacity;
    }

    return false;
}


// TARGET_LIST public operations
// --------------------------------------------------

UI_TARGET_LIST* ui_layer_nav_target_list_begin(UI_STATE *state, ContextId context, UiPurpose purpose){ 
    if (!state)
        return NULL;

    size_t index = entries_hash_key(context, purpose, state->capacity); 

    for (size_t i = 0; i < state->capacity; i++) {
        size_t pos = (index + i) % state->capacity;
        UI_STATE_ENTRY *entry = &state->entries[pos];

        if (entry->state == ENTRY_EMPTY){
            return NULL;
        }

        if (entry->state == ENTRY_OCCUPIED && entry->context == context && entry->purpose == purpose){
            state->borrow_count++;

            return &entry->targets;
        }

        /*
         ENTRY_DELETED:
         Keep probing because the key may be further
         along the probe sequence.
         */
    }

    return NULL;
}

UiTargetListAddResult ui_layer_nav_target_list_add(UI_TARGET_LIST* targets, ContextId context_insert){
    if(context_insert == CONTEXT_ID_INVALID)
        return UI_TARGET_LIST_ADD_ERROR;
    if(!targets)
        return UI_TARGET_LIST_ADD_ERROR;
    if(!targets->items)
        return UI_TARGET_LIST_ADD_ERROR;

    size_t index = targets->count;
    // is the targets array full
    if(targets->count >= targets->capacity){
        // return that the array is full
        return UI_TARGET_LIST_ADD_FULL;
    }

    targets->items[index] = context_insert;
    targets->count++;

    return UI_TARGET_LIST_ADD_SUCCESS;
}

bool ui_layer_nav_target_list_remove(UI_TARGET_LIST* targets, size_t idx){
    if(!targets)return false;
    if(!targets->items)return false;
    if(idx >= targets->count)return false;

    for (size_t i = idx; i + 1 < targets->count; i++) {
        targets->items[i] = targets->items[i + 1];
    }

    targets->count--;
    targets->items[targets->count] = CONTEXT_ID_INVALID;

    return true;
}

size_t ui_layer_nav_target_list_capacity(UI_TARGET_LIST* targets){
    if(!targets)
        return 0;
    return targets->capacity;
}

size_t ui_layer_nav_target_list_count(UI_TARGET_LIST* targets){
    if(!targets)
        return 0;
    return targets->count;
}

ContextId ui_layer_nav_target_list_get(UI_TARGET_LIST* targets, size_t idx){
    if(!targets)
        return CONTEXT_ID_INVALID;
    if(!targets->items)
        return CONTEXT_ID_INVALID;
    if(idx >= targets->count)
        return CONTEXT_ID_INVALID;
    return targets->items[idx];
}

bool ui_layer_nav_target_list_resize(UI_TARGET_LIST* targets, size_t new_capacity){
    if(!targets)
        return false;
    if(!targets->items)
        return false;
    if(new_capacity == 0 || new_capacity < targets->count)return false;

    ContextId* new_items = calloc(new_capacity, sizeof(ContextId));
    if(!new_items)return false;

    for(size_t i = 0; i < targets->count; i++){
        new_items[i] = targets->items[i];
    }
    targets->capacity = new_capacity;
    free(targets->items);
    targets->items = new_items;

    return true;
}

bool ui_layer_nav_target_list_clear(UI_TARGET_LIST* targets){
    if(!targets)
        return false;

    for (size_t i = 0; i < targets->count; i++)
        targets->items[i] = CONTEXT_ID_INVALID;

    targets->count = 0;

    return true;
}

void ui_layer_nav_target_list_end(UI_STATE *state){
    if(!state)
        return;
    state->borrow_count--;
}
// TARGET_LIST operations END
// --------------------------------------------------

// UI LAYER Functions for the user interface

void ui_layer_update_cycle(UI_LAYER* ui_layer){
    if(!ui_layer)
        return;
    nav_update(ui_layer->app_intrf);
}

ContextId ui_layer_state_root_return(UI_LAYER* ui_layer){
    if (!ui_layer)
        return CONTEXT_ID_INVALID;

    return nav_cx_root_return(ui_layer->app_intrf); 
}

bool ui_layer_context_valid(UI_LAYER *ui_layer, ContextId context)
{
    if (!ui_layer)
        return false;

    return nav_cx_is_valid(ui_layer->app_intrf, context);
}

const char *ui_layer_context_name_return(UI_LAYER *ui_layer, ContextId context)
{
    if (!ui_layer)
        return NULL;

    return nav_cx_name_return(ui_layer->app_intrf, context);
}

uint32_t ui_layer_context_flags_return(UI_LAYER *ui_layer, ContextId context)
{
    if (!ui_layer)
        return 0;

    return nav_cx_flags_return(ui_layer->app_intrf, context);
}

size_t ui_layer_context_children_count(UI_LAYER *ui_layer, ContextId context)
{
    if (!ui_layer)
        return 0;

    return nav_cx_children_count(ui_layer->app_intrf, context);
}

ContextId ui_layer_context_child_at(UI_LAYER *ui_layer, ContextId parent, size_t index)
{
    if (!ui_layer)
        return CONTEXT_ID_INVALID;

    return nav_cx_child_at(ui_layer->app_intrf, parent, index);
}

ContextId ui_layer_context_parent_return(UI_LAYER *ui_layer, ContextId context)
{
    if (!ui_layer)
        return CONTEXT_ID_INVALID;

    return nav_cx_parent_return(ui_layer->app_intrf, context);
}
