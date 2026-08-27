#include "ui_layer.h"
#include "app_intrf.h"

//UI_NAVIGATION_ENTRY* initial size, must be power of two
#define ENTRIES_INIT_CAPACITY 16
#define ENTRIES_MAX_LOAD_FACTOR 0.75
#define ENTRIES_MIN_LOAD_FACTOR 0.10

typedef enum{
    ENTRY_EMPTY,
    ENTRY_OCCUPIED,
    ENTRY_DELETED
}EntryState;

typedef struct _ui_navigation_entry{
    ContextId context;
    UiPurpose purpose;
    ContextId target;
    EntryState state;
}UI_NAVIGATION_ENTRY;

typedef struct _ui_navigation{
    UI_NAVIGATION_ENTRY* entries;
    size_t count;
    size_t capacity;
}UI_NAVIGATION;

typedef struct _ui_state{
    ContextId current;

    UI_SELECTION selection;

    UI_NAVIGATION navigation;
}UI_STATE;

typedef struct _ui_layer{
    APP_INTRF* app_intrf;
}UI_LAYER;

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

static int entries_resize(UI_NAVIGATION* navigation, size_t new_capacity)
{
    if(!navigation || new_capacity == 0)
        return -1;

    UI_NAVIGATION_ENTRY *old_entries = navigation->entries;
    size_t old_capacity = navigation->capacity;

    UI_NAVIGATION_ENTRY *new_entries = calloc(new_capacity, sizeof(UI_NAVIGATION_ENTRY));

    if (!new_entries)
        return -1;

    navigation->entries = new_entries;
    navigation->capacity = new_capacity;
    navigation->count = 0;

    for (size_t i = 0; i < old_capacity; i++) {
        UI_NAVIGATION_ENTRY *old = &old_entries[i];

        if (old->state != ENTRY_OCCUPIED) 
            continue;

        size_t index = entries_hash_key(old->context, old->purpose, navigation->capacity);

        for (size_t j = 0; j < navigation->capacity; j++) {
            size_t pos = (index + j) % navigation->capacity;
            UI_NAVIGATION_ENTRY *entry = &navigation->entries[pos];

            if (entry->state != ENTRY_OCCUPIED) {
                *entry = *old;
                entry->state = ENTRY_OCCUPIED;
                navigation->count++;
                break;
            }
        }
    }

    free(old_entries);
    return 0;
}

static int entries_create(UI_NAVIGATION* navigation, size_t capacity)
{
    if (capacity == 0){
        capacity = ENTRIES_INIT_CAPACITY;
    }

    navigation->entries =
        calloc(capacity, sizeof(UI_NAVIGATION_ENTRY));

    if (!navigation->entries) {
        return -1;
    }

    navigation->capacity = capacity;
    navigation->count = 0;

    return 0;
}

static int entries_remove(UI_NAVIGATION *navigation, ContextId context, UiPurpose purpose, ContextId* target)
{
    if (!navigation)
        return -1;

    size_t index = entries_hash_key(context, purpose, navigation->capacity); 

    for (size_t i = 0; i < navigation->capacity; ++i) {
        UI_NAVIGATION_ENTRY *entry = &navigation->entries[index];

        if (entry->state == ENTRY_EMPTY) {
            /*
             An empty slot terminates the probe sequence.
             The key cannot exist further along this sequence.
             */
            return -1;
        }

        if (entry->state == ENTRY_OCCUPIED && entry->context == context && entry->purpose == purpose) {
            *target = entry->target;

            /*
             * Do not make this ENTRY_EMPTY: that could break the
             * probe sequence for entries that were inserted later.
             */
            entry->state = ENTRY_DELETED;
            navigation->count--;

            /*
             * Shrink the table if it has become sparse.
             */
            if (navigation->capacity > ENTRIES_INIT_CAPACITY &&
                (double)navigation->count / navigation->capacity < ENTRIES_MIN_LOAD_FACTOR) {

                size_t new_capacity = navigation->capacity / 2;

                if (new_capacity < ENTRIES_INIT_CAPACITY)
                    new_capacity = ENTRIES_INIT_CAPACITY;

                /*
                 * Failure to shrink is not an error.
                 * The existing table remains valid.
                 */
                (void)entries_resize(navigation, new_capacity);
            }

            return 1;
        }

        /*
         * ENTRY_DELETED and non-matching EMPTY_OCCUPIED entries
         * continue the probe sequence.
         */
        index = (index + 1) % navigation->capacity;
    }

    return NULL;
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

void ui_layer_destroy(UI_LAYER* ui_layer, UI_STATE *states, size_t states_count){
    if(!ui_layer)
        return;
    app_intrf_destroy(ui_layer->app_intrf);
    free(ui_layer);
    if(!states || states_count < 1)
        return;
    // cleanup the states here
    for(size_t i = 0; i < states_count; i++){
        UI_STATE state = states[i];
        if(state.navigation.entries)
            free(state.navigation.entries);
    }
}

bool ui_layer_nav_set(UI_STATE* state, ContextId context, ContextId target, UiPurpose purpose){
    if(!state)
        return false;
    UI_NAVIGATION* navigation = &state->navigation;
    if (!navigation)
        return false;

    size_t index = entries_hash_key(context, purpose, navigation->capacity);
    /*
     * Resize before inserting if necessary.
     */
    if ((double)(navigation->count + 1) / navigation->capacity >
        ENTRIES_MAX_LOAD_FACTOR) {

         // Prevent size_t overflow.
        if (navigation->capacity > SIZE_MAX / 2)
            return false;

        size_t new_capacity = navigation->capacity * 2;

        if (entries_resize(navigation, new_capacity) != 0)
            return false;

         // Capacity changed, so the bucket index must
         // be recalculated.
        index = entries_hash_key(context, purpose, navigation->capacity);
    }

    size_t deleted_index = SIZE_MAX;

    for (size_t i = 0; i < navigation->capacity; i++) {
        UI_NAVIGATION_ENTRY *entry =
            &navigation->entries[(index + i) % navigation->capacity];

        if (entry->state == ENTRY_EMPTY) {
            /*
             * Reuse the first deleted slot, if one was found.
             */
            if (deleted_index != SIZE_MAX)
                entry = &navigation->entries[deleted_index];

            entry->context = context;
            entry->purpose = purpose;
            entry->target = target;
            entry->state = ENTRY_OCCUPIED;
            navigation->count++;

            return true;
        }

        if (entry->state == ENTRY_DELETED) {
            if (deleted_index == SIZE_MAX)
                deleted_index = (index + i) % navigation->capacity;

            continue;
        }

        /*
         * ENTRY_OCCUPIED
         */
        if (entry->context == context && entry->purpose == purpose) {

            entry->target = target;
            return true;
        }
    }

    return false;
}

bool ui_layer_nav_try_get(const UI_STATE *state, ContextId context, UiPurpose purpose, ContextId* target)
{
    if (!state)
        return false;
    const UI_NAVIGATION *navigation = &state->navigation;
    if (!navigation)
        return false;

    size_t index = entries_hash_key(context, purpose, navigation->capacity); 

    for (size_t i = 0; i < navigation->capacity; i++) {
        size_t pos = (index + i) % navigation->capacity;
        const UI_NAVIGATION_ENTRY *entry = &navigation->entries[pos];

        if (entry->state == ENTRY_EMPTY)
            return false;

        if (entry->state == ENTRY_OCCUPIED && entry->context == context && entry->purpose == purpose){
            *target = entry->target;
            return true;
        }

        /*
         ENTRY_DELETED:
         Keep probing because the key may be further
         along the probe sequence.
         */
    }

    return false;
}
