#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A tree of named leaves under named branches, built in one go and then
// browsed one level at a time. A level lists its sub-branches first, then its
// leaves, each in the order they were first added.
// Nesting is the owner's convention: it walks its own path syntax and calls
// tree_index_branch once per level
// Keys are minted per name (see intern_table) and survive tree_index_reset,
// so a rebuilt tree hands out the same key for the same branch or leaf. Keys
// start at 1, branch key 0 is the top level.
// Every function takes a NULL tree as an empty one.

typedef struct _tree_index TREE_INDEX;

typedef struct {
    bool is_branch;
    uint64_t key;
    const char *name; // owned by the index, valid until it is changed
} TREE_ROW;

// a new empty index, NULL on allocation failure
TREE_INDEX *tree_index_new(void);

// find-or-add the branch `name` under `parent` (0 = top level) and return its
// key. 0 for an empty name, a parent not in the index, or on allocation
// failure
uint64_t tree_index_branch(TREE_INDEX *tree, uint64_t parent,
                           const char *name);

// add a leaf under `branch` (0 = top level). identity is the owner's unique
// name for it - whatever it needs to find the item again, see
// tree_index_leaf_identity. Returns its key, 0 when it was not added: identity
// already added, branch not in the index, or allocation failure
uint64_t tree_index_leaf(TREE_INDEX *tree, uint64_t branch, const char *name,
                         const char *identity);

// lay the levels out for browsing - until then every level is empty. A branch
// with no leaf anywhere under it is left out. 0 on success, -1 on failure
int tree_index_finish(TREE_INDEX *tree);

// rows at `branch`, 0 when the branch is not in the index
size_t tree_index_level_count(const TREE_INDEX *tree, uint64_t branch);
// fill *out with row idx at `branch`. false (and *out zeroed) when out of
// range
bool tree_index_level_at(const TREE_INDEX *tree, uint64_t branch, size_t idx,
                         TREE_ROW *out);

// the identity the leaf with this key was added with, NULL when it is not in
// the index. Owned by the index, valid until it is freed
const char *tree_index_leaf_identity(const TREE_INDEX *tree, uint64_t key);

// empty the index for a rebuild, the keys stay
void tree_index_reset(TREE_INDEX *tree);

void tree_index_free(TREE_INDEX *tree);
