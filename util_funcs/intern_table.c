#include "intern_table.h"
#include <stdlib.h>
#include <string.h>

// keys are 1-based so that 0 can mean "none", so this is the only place that
// turns one back into an entry
static INTERN_ENTRY *intern_entry_at(const INTERN_TABLE *table, uint64_t key) {
    if (!table || key == 0 || key > table->count)
        return NULL;
    return &table->entries[key - 1];
}

uint64_t intern_find(const INTERN_TABLE *table, const char *name) {
    if (!table || !name)
        return 0;
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].name, name) == 0)
            return i + 1;
    }
    return 0;
}

uint64_t intern_add(INTERN_TABLE *table, const char *name) {
    if (!table || !name)
        return 0;
    uint64_t key = intern_find(table, name);
    if (key)
        return key;
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
    table->entries[table->count++] =
        (INTERN_ENTRY){.name = name_copy, .slot = INTERN_SLOT_NONE};
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
    table->entries = NULL;
    table->count = 0;
    table->max = 0;
}
