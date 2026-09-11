#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ids.h"

// Actions contract shared by the data layer (app_data.c) and the context
// layer (app_intrf.c). Dependency-free (only ids.h), included by
// data_object.h since DataOps references these types.
//
// Flow: nav_cx_actions (menu) -> nav_cx_action_args (per-action DataArgSpec)
// -> for CHOICE/MULTI_CHOICE args, nav_cx_list_count/nav_cx_list_at (pass the
// half-filled DataActionReq as `partial` so one arg's list can depend on an
// earlier arg's value) -> nav_cx_action_do. Every buffer here is caller-owned
// (stack array or a single struct); nothing in this interface allocates.

// Opaque, namespaced like ContextId (composed with MAKE_LIST_ID in
// app_data.c). Identifies which enumerable list a CHOICE/MULTI_CHOICE arg
// draws its options from. List kinds are static (compile-time constants),
// not counter-allocated - there is exactly one "LV2 catalogue" list etc. for
// the life of the program.
typedef uint64_t DataListId;
#define DATA_LIST_NONE ((DataListId)0)

typedef enum {
    DATA_ACTION_REMOVE,
    DATA_ACTION_ADD_CHOICE,
    DATA_ACTION_ADD_FILE_PATH,
    DATA_ACTION_CONNECT,      // generic bipartite toggle-connect: one source,
                               // many targets, both drawn from named lists.
} DataActionType;

typedef enum {
    DATA_ARG_STRING,
    DATA_ARG_PATH,
    DATA_ARG_CHOICE,
    DATA_ARG_MULTI_CHOICE,
} DataArgKind;

typedef enum {
    DATA_ACTION_STYLE_NORMAL,
    DATA_ACTION_STYLE_DANGEROUS,
} DataActionStyle;

typedef enum {
    DATA_ACTION_OK = 0,
    DATA_ACTION_ERR_INVALID,      // bad context / action / args, or a choice
                                   // value that isn't from the arg's list
    DATA_ACTION_ERR_NOT_ALLOWED,  // action exists but isn't enabled right now
    DATA_ACTION_ERR_STALE,        // choice value was valid, its item is gone
                                   // now - caller should re-pull the list
    DATA_ACTION_ERR_DATA,         // module rejected the request (bad file,
                                   // load failed, link failed, ...)
} DataActionResult;

// one entry in a context's action menu
typedef struct {
    DataActionType type;
    const char *label;      // borrowed, static in the data layer
    const char *tooltip;
    bool enabled;
    DataActionStyle style;
} DataAction;

// one input an action needs
typedef struct {
    const char *name;       // "path", "source", "targets" - borrowed, static
    const char *label;
    DataArgKind kind;
    bool required;
    DataListId list;        // set iff kind is CHOICE / MULTI_CHOICE, else
                             // DATA_LIST_NONE
} DataArgSpec;

#define DATA_CHOICE_LINKED (1u << 0)

// one option for a CHOICE / MULTI_CHOICE arg
typedef struct {
    uint64_t value;          // MAKE_ID(list_namespace, stable item key) -
                              // never a positional index
    const char *label;       // borrowed, valid while the module's list lives
    uint32_t flags;          // bit 0 (DATA_CHOICE_LINKED): already linked to
                              // partial->connect.source. 0 for non-connect
                              // lists (e.g. the plugin catalogue).
} DataChoice;

// the filled-in request to execute an action. Strings and the targets[]
// array are borrowed for the duration of the action_do call only - the
// module copies whatever it needs to keep past the call.
typedef struct {
    DataActionType type;
    union {
        struct {
            uint64_t choice_value;
        } add_choice;
        struct {
            const char *path;
        } add_file_path;
        struct {
            uint64_t source;           // MAKE_ID(domain_namespace, key)
            const uint64_t *targets;   // MAKE_ID(domain_namespace, key) each
            size_t target_count;
        } connect;
        // DATA_ACTION_REMOVE: no payload
    };
} DataActionReq;
