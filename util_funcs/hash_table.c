#include "hash_table.h"

#include <stdlib.h>

#define HT_INITIAL_CAPACITY 16
#define HT_MAX_LOAD_FACTOR  0.75
#define HT_MIN_LOAD_FACTOR  0.10

/*
 * 64-bit integer mixing function.
 *
 * Based on the finalizer used by MurmurHash3.
 */
static size_t hash_key(uint64_t key, size_t capacity)
{
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    key *= UINT64_C(0xc4ceb9fe1a85ec53);
    key ^= key >> 33;

    return (size_t)(key % capacity);
}

static int ht_resize(HashTable *ht, size_t new_capacity)
{
    if (!ht || new_capacity == 0)
        return -1;

    HashEntry **new_buckets =
        calloc(new_capacity, sizeof(*new_buckets));

    if (!new_buckets)
        return -1;

    /*
     * Rehash all existing entries.
     *
     * We reuse the existing HashEntry objects; only their
     * bucket links are changed.
     */
    for (size_t i = 0; i < ht->capacity; i++) {
        HashEntry *entry = ht->buckets[i];

        while (entry) {
            HashEntry *next = entry->next;

            size_t index =
                hash_key(entry->key, new_capacity);

            entry->next = new_buckets[index];
            new_buckets[index] = entry;

            entry = next;
        }
    }

    free(ht->buckets);

    ht->buckets = new_buckets;
    ht->capacity = new_capacity;

    return 0;
}

HashTable *ht_create(size_t capacity)
{
    if (capacity == 0)
        capacity = HT_INITIAL_CAPACITY;

    HashTable *ht = malloc(sizeof(*ht));

    if (!ht)
        return NULL;

    ht->buckets =
        calloc(capacity, sizeof(*ht->buckets));

    if (!ht->buckets) {
        free(ht);
        return NULL;
    }

    ht->capacity = capacity;
    ht->size = 0;

    return ht;
}

int ht_set(HashTable *ht, uint64_t key, void *value)
{
    if (!ht)
        return -1;

    size_t index = hash_key(key, ht->capacity);

    /*
     * If key already exists, replace the value.
     */
    HashEntry *entry = ht->buckets[index];

    while (entry) {
        if (entry->key == key) {
            entry->value = value;
            return 0;
        }

        entry = entry->next;
    }

    /*
     * Resize before inserting if necessary.
     */
    if ((double)(ht->size + 1) / ht->capacity >
        HT_MAX_LOAD_FACTOR) {

        /*
         * Prevent size_t overflow.
         */
        if (ht->capacity > SIZE_MAX / 2)
            return -1;

        size_t new_capacity = ht->capacity * 2;

        if (ht_resize(ht, new_capacity) != 0)
            return -1;

        /*
         * Capacity changed, so the bucket index must
         * be recalculated.
         */
        index = hash_key(key, ht->capacity);
    }

    entry = malloc(sizeof(*entry));

    if (!entry)
        return -1;

    entry->key = key;
    entry->value = value;

    entry->next = ht->buckets[index];
    ht->buckets[index] = entry;

    ht->size++;

    return 0;
}

void *ht_get(const HashTable *ht, uint64_t key)
{
    if (!ht)
        return NULL;

    size_t index = hash_key(key, ht->capacity);

    HashEntry *entry = ht->buckets[index];

    while (entry) {
        if (entry->key == key)
            return entry->value;

        entry = entry->next;
    }

    return NULL;
}

int ht_contains(const HashTable *ht, uint64_t key)
{
    if (!ht)
        return 0;

    size_t index = hash_key(key, ht->capacity);

    HashEntry *entry = ht->buckets[index];

    while (entry) {
        if (entry->key == key)
            return 1;

        entry = entry->next;
    }

    return 0;
}

void* ht_remove(HashTable *ht, uint64_t key)
{
    if (!ht)
        return NULL;

    size_t index = hash_key(key, ht->capacity);

    HashEntry **current = &ht->buckets[index];

    void* user_data = NULL;

    while (*current) {
        HashEntry *entry = *current;

        if (entry->key == key) {
            *current = entry->next;
            user_data = entry->value;

            free(entry);
            ht->size--;

            /*
             * Shrink the table if it has become sparse.
             */
            if (ht->capacity > HT_INITIAL_CAPACITY &&
                (double)ht->size / ht->capacity <
                    HT_MIN_LOAD_FACTOR) {

                size_t new_capacity = ht->capacity / 2;

                if (new_capacity < HT_INITIAL_CAPACITY)
                    new_capacity = HT_INITIAL_CAPACITY;

                /*
                 * Failure to shrink is not an error.
                 * The existing table remains valid.
                 */
                (void)ht_resize(ht, new_capacity);
            }

            return user_data;
        }

        current = &entry->next;
    }

    return NULL;
}

void ht_destroy(HashTable *ht)
{
    if (!ht)
        return;

    for (size_t i = 0; i < ht->capacity; i++) {
        HashEntry *entry = ht->buckets[i];

        while (entry) {
            HashEntry *next = entry->next;

            free(entry);
            entry = next;
        }
    }

    free(ht->buckets);
    free(ht);
}
