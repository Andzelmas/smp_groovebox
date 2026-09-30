#pragma once
#include <stddef.h>
#include <stdint.h>

// Mints a stable key per name. A name keeps its key for the table's whole
// life, so a list that is rebuilt can hand out the same key for the same item.
// Keys start at 1, 0 means "none". Each entry also carries a slot, where the
// owner keeps the item's current index in its own array.
// Name lookups go through a hash index, so they do not scan the entries.
// A zeroed INTERN_TABLE is a valid empty table.

#define INTERN_SLOT_NONE SIZE_MAX

typedef struct _intern_entry {
    char *name;
    uint64_t hash; // of name, kept so the index can grow without rehashing
    size_t slot; // INTERN_SLOT_NONE when the name is not currently present
} INTERN_ENTRY;

typedef struct _intern_table {
    INTERN_ENTRY *entries;
    size_t count;
    size_t max;
    // open addressing, linear probing: each position holds a key, 0 = empty.
    // index_cap is a power of two, at least twice count
    size_t *index;
    size_t index_cap;
} INTERN_TABLE;

// key of an already-interned name, 0 when it has never been seen
uint64_t intern_find(const INTERN_TABLE *table, const char *name);

// as intern_find, but mints a key for a name seen for the first time.
// 0 on allocation failure
uint64_t intern_add(INTERN_TABLE *table, const char *name);

// the key of old_name now answers to new_name. Skipped when new_name is
// already interned - that name keeps its own key
void intern_rename(INTERN_TABLE *table, const char *old_name,
                   const char *new_name);

// INTERN_SLOT_NONE when the key is unknown or not present
size_t intern_slot(const INTERN_TABLE *table, uint64_t key);
void intern_set_slot(INTERN_TABLE *table, uint64_t key, size_t slot);

// marks every name as not present, the keys stay
void intern_detach_all(INTERN_TABLE *table);

void intern_clean(INTERN_TABLE *table);
