#include "tree_index.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TREE_NAMES_START 4096
#define TREE_NODES_START 64

typedef struct _tree_node {
    uint64_t key;
    size_t parent; // level index: 0 the top level, i + 1 branches[i]
    size_t name;   // offset in names
} TREE_NODE;

typedef struct _tree_level {
    size_t first_branch; // in branch_order
    size_t branch_count;
    size_t first_leaf; // in leaf_order
    size_t leaf_count;
} TREE_LEVEL;

// level index of a branch key, false when it is not in the index
static bool tree_level_index(const TREE_INDEX *tree, uint64_t branch,
                             size_t *out) {
    if (branch == 0) {
        *out = 0;
        return true;
    }
    size_t slot = intern_slot(&tree->branch_keys, branch);
    if (slot == INTERN_SLOT_NONE)
        return false;
    *out = slot + 1;
    return true;
}

static const TREE_LEVEL *tree_level(const TREE_INDEX *tree, uint64_t branch) {
    size_t level;
    if (!tree || !tree->finished || !tree_level_index(tree, branch, &level))
        return NULL;
    return &tree->levels[level];
}

// copy name into names, returns its offset, SIZE_MAX on allocation failure
static size_t tree_name_add(TREE_INDEX *tree, const char *name) {
    size_t len = strlen(name) + 1;
    if (tree->names_len + len > tree->names_max) {
        size_t new_max = tree->names_max ? tree->names_max : TREE_NAMES_START;
        while (tree->names_len + len > new_max)
            new_max *= 2;
        char *grown = realloc(tree->names, new_max);
        if (!grown)
            return SIZE_MAX;
        tree->names = grown;
        tree->names_max = new_max;
    }
    size_t offset = tree->names_len;
    memcpy(tree->names + offset, name, len);
    tree->names_len += len;
    return offset;
}

// a new node at the end of nodes, NULL on allocation failure
static TREE_NODE *tree_node_append(TREE_NODE **nodes, size_t *count,
                                   size_t *max) {
    if (*count == *max) {
        size_t new_max = *max ? *max * 2 : TREE_NODES_START;
        TREE_NODE *grown = realloc(*nodes, new_max * sizeof(TREE_NODE));
        if (!grown)
            return NULL;
        *nodes = grown;
        *max = new_max;
    }
    return &(*nodes)[(*count)++];
}

uint64_t tree_index_branch(TREE_INDEX *tree, uint64_t parent,
                           const char *name) {
    if (!tree || !name || name[0] == '\0')
        return 0;
    size_t level;
    if (!tree_level_index(tree, parent, &level))
        return 0;

    // the parent key keeps "Bass" under two parents apart, and it is stable
    // across rebuilds, so the branch key is too
    size_t need = strlen(name) + 24;
    if (need > tree->scratch_max) {
        char *grown = realloc(tree->scratch, need);
        if (!grown)
            return 0;
        tree->scratch = grown;
        tree->scratch_max = need;
    }
    snprintf(tree->scratch, need, "%" PRIu64 "\x1f" "%s", parent, name);
    uint64_t key = intern_add(&tree->branch_keys, tree->scratch);
    if (!key)
        return 0;
    // already added in this build
    if (intern_slot(&tree->branch_keys, key) != INTERN_SLOT_NONE)
        return key;

    size_t name_offset = tree_name_add(tree, name);
    if (name_offset == SIZE_MAX)
        return 0;
    TREE_NODE *node = tree_node_append(&tree->branches, &tree->branch_count,
                                       &tree->branch_max);
    if (!node)
        return 0;
    *node = (TREE_NODE){.key = key, .parent = level, .name = name_offset};
    intern_set_slot(&tree->branch_keys, key, tree->branch_count - 1);
    tree->finished = false;
    return key;
}

uint64_t tree_index_leaf(TREE_INDEX *tree, uint64_t branch, const char *name,
                         const char *identity) {
    if (!tree || !identity || identity[0] == '\0')
        return 0;
    size_t level;
    if (!tree_level_index(tree, branch, &level))
        return 0;
    uint64_t key = intern_add(&tree->leaf_keys, identity);
    if (!key || intern_slot(&tree->leaf_keys, key) != INTERN_SLOT_NONE)
        return 0;

    size_t name_offset = tree_name_add(tree, name ? name : "");
    if (name_offset == SIZE_MAX)
        return 0;
    TREE_NODE *node =
        tree_node_append(&tree->leaves, &tree->leaf_count, &tree->leaf_max);
    if (!node)
        return 0;
    *node = (TREE_NODE){.key = key, .parent = level, .name = name_offset};
    intern_set_slot(&tree->leaf_keys, key, tree->leaf_count - 1);
    tree->finished = false;
    return key;
}

int tree_index_finish(TREE_INDEX *tree) {
    if (!tree)
        return -1;
    size_t level_count = tree->branch_count + 1;
    TREE_LEVEL *levels = calloc(level_count, sizeof(TREE_LEVEL));
    // leaves anywhere under each level
    size_t *below = calloc(level_count, sizeof(size_t));
    size_t *branch_order = malloc((tree->branch_count + 1) * sizeof(size_t));
    size_t *leaf_order = malloc((tree->leaf_count + 1) * sizeof(size_t));
    if (!levels || !below || !branch_order || !leaf_order) {
        free(levels);
        free(below);
        free(branch_order);
        free(leaf_order);
        return -1;
    }

    for (size_t i = 0; i < tree->leaf_count; i++)
        below[tree->leaves[i].parent]++;
    // a branch is always added after its parent, so walking backwards sums a
    // whole subtree before its parent reads it
    for (size_t i = tree->branch_count; i-- > 0;)
        below[tree->branches[i].parent] += below[i + 1];

    // counts per level, then each level's start in the order arrays
    for (size_t i = 0; i < tree->branch_count; i++)
        if (below[i + 1])
            levels[tree->branches[i].parent].branch_count++;
    for (size_t i = 0; i < tree->leaf_count; i++)
        levels[tree->leaves[i].parent].leaf_count++;
    size_t next_branch = 0;
    size_t next_leaf = 0;
    for (size_t l = 0; l < level_count; l++) {
        levels[l].first_branch = next_branch;
        next_branch += levels[l].branch_count;
        levels[l].first_leaf = next_leaf;
        next_leaf += levels[l].leaf_count;
        levels[l].branch_count = 0;
        levels[l].leaf_count = 0;
    }
    // filling in add order keeps each level in add order
    for (size_t i = 0; i < tree->branch_count; i++) {
        if (!below[i + 1])
            continue;
        TREE_LEVEL *level = &levels[tree->branches[i].parent];
        branch_order[level->first_branch + level->branch_count++] = i;
    }
    for (size_t i = 0; i < tree->leaf_count; i++) {
        TREE_LEVEL *level = &levels[tree->leaves[i].parent];
        leaf_order[level->first_leaf + level->leaf_count++] = i;
    }
    free(below);

    free(tree->levels);
    free(tree->branch_order);
    free(tree->leaf_order);
    tree->levels = levels;
    tree->branch_order = branch_order;
    tree->leaf_order = leaf_order;
    tree->finished = true;
    return 0;
}

size_t tree_index_level_count(const TREE_INDEX *tree, uint64_t branch) {
    const TREE_LEVEL *level = tree_level(tree, branch);
    if (!level)
        return 0;
    return level->branch_count + level->leaf_count;
}

bool tree_index_level_at(const TREE_INDEX *tree, uint64_t branch, size_t idx,
                         TREE_ROW *out) {
    if (!out)
        return false;
    *out = (TREE_ROW){0};
    const TREE_LEVEL *level = tree_level(tree, branch);
    if (!level)
        return false;
    const TREE_NODE *node;
    if (idx < level->branch_count) {
        node = &tree->branches[tree->branch_order[level->first_branch + idx]];
        out->is_branch = true;
    } else if (idx - level->branch_count < level->leaf_count) {
        idx -= level->branch_count;
        node = &tree->leaves[tree->leaf_order[level->first_leaf + idx]];
    } else {
        return false;
    }
    out->key = node->key;
    out->name = tree->names + node->name;
    return true;
}

size_t tree_index_leaf_find(const TREE_INDEX *tree, uint64_t key) {
    if (!tree)
        return SIZE_MAX;
    return intern_slot(&tree->leaf_keys, key);
}

void tree_index_reset(TREE_INDEX *tree) {
    if (!tree)
        return;
    tree->branch_count = 0;
    tree->leaf_count = 0;
    tree->names_len = 0;
    intern_detach_all(&tree->branch_keys);
    intern_detach_all(&tree->leaf_keys);
    tree->finished = false;
}

void tree_index_clean(TREE_INDEX *tree) {
    if (!tree)
        return;
    free(tree->branches);
    free(tree->leaves);
    free(tree->names);
    free(tree->scratch);
    free(tree->levels);
    free(tree->branch_order);
    free(tree->leaf_order);
    intern_clean(&tree->branch_keys);
    intern_clean(&tree->leaf_keys);
    *tree = (TREE_INDEX){0};
}
