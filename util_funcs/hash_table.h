#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifndef MAX_KEY_LENGTH
#define MAX_KEY_LENGTH 1024
#endif

typedef struct HashEntry {
    char key[MAX_KEY_LENGTH];
    size_t key_len;
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
 * MUST BE POWER OF TWO
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
int ht_set(HashTable *ht, const char* key, size_t key_len, void *value);

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
void *ht_get(const HashTable *ht, const char* key, size_t key_len);

/*
 * Check whether key exists.
 *
 * Returns:
 *   1  key exists
 *   0  key does not exist
 */
int ht_contains(const HashTable *ht, const char* key, size_t key_len);

/*
 * Remove key from the table.
 *
 * Returns associated user data, or
 * NULL if key not found or error
 */
void* ht_remove(HashTable *ht, const char* key, size_t key_len);

/*
 * Destroy the hash table.
 *
 * Stored values are NOT freed.
 */
void ht_destroy(HashTable *ht);

#endif /* HASH_TABLE_H */
