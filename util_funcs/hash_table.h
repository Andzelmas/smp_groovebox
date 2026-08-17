#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct HashEntry {
    uint64_t key;
    void *value;
    struct HashEntry *next;
} HashEntry;

typedef struct {
    HashEntry **buckets;
    size_t capacity;
    size_t size;
} HashTable;

/*
 * Create a hash table.
 *
 * If capacity is 0, HT_INITIAL_CAPACITY is used.
 *
 * Returns NULL on allocation failure.
 */
HashTable *ht_create(size_t capacity);

/*
 * Insert or replace a key/value pair.
 *
 * If key already exists, its value is replaced.
 *
 * Returns:
 *   0  success
 *  -1  allocation failure or invalid table
 *
 * The hash table does not take ownership of value.
 */
int ht_set(HashTable *ht, uint64_t key, void *value);

/*
 * Get the value associated with key.
 *
 * Returns:
 *   value pointer if found
 *   NULL if not found
 *
 * Note that NULL cannot be distinguished from a stored NULL value
 * using this function alone. Use ht_contains() when necessary.
 */
void *ht_get(const HashTable *ht, uint64_t key);

/*
 * Check whether key exists.
 *
 * Returns:
 *   1  key exists
 *   0  key does not exist
 */
int ht_contains(const HashTable *ht, uint64_t key);

/*
 * Remove key from the table.
 *
 * The associated value is NOT freed.
 *
 * Returns:
 *   0  key was removed
 *  -1  key was not found or table is invalid
 */
int ht_remove(HashTable *ht, uint64_t key);

/*
 * Destroy the hash table.
 *
 * Stored values are NOT freed.
 */
void ht_destroy(HashTable *ht);

#endif /* HASH_TABLE_H */
