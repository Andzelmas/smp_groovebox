#include "intern_table.h"
#include "string_funcs.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define INTERN_INDEX_START 64

// keys are 1-based so that 0 can mean "none", so this is the only place that
// turns one back into an entry
static INTERN_ENTRY *intern_entry_at(const INTERN_TABLE *table, uint64_t key) {
    if (!table || key == 0 || key > table->count)
        return NULL;
    return &table->entries[key - 1];
}

// index position holding name's key, or the empty position where it would go.
// The index must exist - it is never full, so the probe always ends
static size_t intern_index_pos(const INTERN_TABLE *table, const char *name,
                               uint64_t hash) {
    size_t mask = table->index_cap - 1;
    size_t pos = (size_t)hash & mask;
    while (table->index[pos]) {
        const INTERN_ENTRY *entry = &table->entries[table->index[pos] - 1];
        if (entry->hash == hash && strcmp(entry->name, name) == 0)
            break;
        pos = (pos + 1) & mask;
    }
    return pos;
}

// put every entry's key back into the (same size) index from scratch
static void intern_index_fill(INTERN_TABLE *table) {
    size_t mask = table->index_cap - 1;
    memset(table->index, 0, table->index_cap * sizeof(size_t));
    for (size_t i = 0; i < table->count; i++) {
        size_t pos = (size_t)table->entries[i].hash & mask;
        while (table->index[pos])
            pos = (pos + 1) & mask;
        table->index[pos] = i + 1;
    }
}

static bool intern_index_resize(INTERN_TABLE *table, size_t new_cap) {
    size_t *grown = malloc(new_cap * sizeof(size_t));
    if (!grown)
        return false;
    free(table->index);
    table->index = grown;
    table->index_cap = new_cap;
    intern_index_fill(table);
    return true;
}

static uint64_t intern_lookup(const INTERN_TABLE *table, const char *name,
                              uint64_t hash) {
    if (!table->index)
        return 0;
    return table->index[intern_index_pos(table, name, hash)];
}

uint64_t intern_find(const INTERN_TABLE *table, const char *name) {
    if (!table || !name)
        return 0;
    return intern_lookup(table, name, str_hash_fnv1a64(name));
}

uint64_t intern_add(INTERN_TABLE *table, const char *name) {
    if (!table || !name)
        return 0;
    uint64_t hash = str_hash_fnv1a64(name);
    uint64_t key = intern_lookup(table, name, hash);
    if (key)
        return key;
    // at most half full, so probe runs stay short
    if ((table->count + 1) * 2 > table->index_cap &&
        !intern_index_resize(table, table->index_cap ? table->index_cap * 2
                                                     : INTERN_INDEX_START))
        return 0;
    if (table->count == table->max) {
        size_t new_max = table->max ? table->max * 2 : 32;
        INTERN_ENTRY *grown =
            realloc(table->entries, sizeof(INTERN_ENTRY) * new_max);
        if (!grown)
            return 0;
        table->entries = grown;
        table->max = new_max;
    }
    char *name_copy = strdup(name);
    if (!name_copy)
        return 0;
    // the new entry is not in the index yet, so this finds an empty position
    size_t pos = intern_index_pos(table, name, hash);
    table->entries[table->count++] = (INTERN_ENTRY){
        .name = name_copy, .hash = hash, .slot = INTERN_SLOT_NONE};
    table->index[pos] = table->count;
    return table->count;
}

void intern_rename(INTERN_TABLE *table, const char *old_name,
                   const char *new_name) {
    if (!old_name || !new_name)
        return;
    if (intern_find(table, new_name))
        return;
    INTERN_ENTRY *entry = intern_entry_at(table, intern_find(table, old_name));
    if (!entry)
        return;
    char *name_copy = strdup(new_name);
    if (!name_copy)
        return;
    free(entry->name);
    entry->name = name_copy;
    entry->hash = str_hash_fnv1a64(new_name);
    // the key now belongs at the new name's hash. Renames are rare, so refill
    // rather than delete from a linear probing index
    intern_index_fill(table);
}

size_t intern_slot(const INTERN_TABLE *table, uint64_t key) {
    const INTERN_ENTRY *entry = intern_entry_at(table, key);
    return entry ? entry->slot : INTERN_SLOT_NONE;
}

void intern_set_slot(INTERN_TABLE *table, uint64_t key, size_t slot) {
    INTERN_ENTRY *entry = intern_entry_at(table, key);
    if (entry)
        entry->slot = slot;
}

void intern_detach_all(INTERN_TABLE *table) {
    if (!table)
        return;
    for (size_t i = 0; i < table->count; i++)
        table->entries[i].slot = INTERN_SLOT_NONE;
}

void intern_clean(INTERN_TABLE *table) {
    if (!table)
        return;
    for (size_t i = 0; i < table->count; i++)
        free(table->entries[i].name);
    free(table->entries);
    free(table->index);
    table->entries = NULL;
    table->count = 0;
    table->max = 0;
    table->index = NULL;
    table->index_cap = 0;
}
