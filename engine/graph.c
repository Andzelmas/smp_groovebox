#include "graph.h"
#include "../types.h"
#include "../util_funcs/intern_table.h"
#include "../util_funcs/midi_buf.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// heap record, never moves while it exists
struct _graph_port {
    GRAPH_NODE *node;
    char *name;
    uint64_t key;
    unsigned int type;
    unsigned int flow;
    SAMPLE_T *buffer;   // audio, max_buffer_size frames
    MIDI_BUF *midi_buf; // midi
    // the other end of each edge, in connection order
    GRAPH_PORT **peers;
    size_t peer_count;
    size_t peer_max;
};

struct _graph_node {
    uint64_t owner_tag;
    uint64_t owner_uid;
    GRAPH_PROCESS_FN process;
    void *user_data;
    uint32_t latency; // frames
    bool visited;     // node_reaches scratch, false between calls
    GRAPH_PORT **ports;
    size_t port_count;
    size_t port_max;
};

struct _graph {
    uint32_t max_buffer_size;
    GRAPH_NODE **nodes;
    size_t node_count;
    size_t node_max;
    // every port, grouped by GraphPortList (each a run at list_first), nodes
    // in add order within a run. Rebuilt on every port add/remove
    GRAPH_PORT **view;
    size_t view_max;
    size_t list_first[GRAPH_PORT_LIST_COUNT];
    size_t list_count[GRAPH_PORT_LIST_COUNT];
    // one name per (owner, port name) seen this session. The slot is the
    // port's view index
    INTERN_TABLE idents;
    uint64_t generation;
};

// grow a port array to hold one more, false on allocation failure
static bool ports_reserve(GRAPH_PORT ***arr, size_t count, size_t *max) {
    if (count < *max)
        return true;
    size_t new_max = *max ? *max * 2 : 8;
    GRAPH_PORT **grown = realloc(*arr, sizeof(GRAPH_PORT *) * new_max);
    if (!grown)
        return false;
    *arr = grown;
    *max = new_max;
    return true;
}

// drop one entry, keeping the order
static void ports_remove(GRAPH_PORT **arr, size_t *count,
                         const GRAPH_PORT *port) {
    for (size_t i = 0; i < *count; i++) {
        if (arr[i] != port)
            continue;
        memmove(&arr[i], &arr[i + 1], sizeof(GRAPH_PORT *) * (*count - i - 1));
        (*count)--;
        return;
    }
}

static GraphPortList port_list(unsigned int type, unsigned int flow) {
    if (flow == PORT_FLOW_OUTPUT)
        return type == PORT_TYPE_MIDI ? GRAPH_PORT_LIST_OUT_MIDI
                                      : GRAPH_PORT_LIST_OUT_AUDIO;
    return type == PORT_TYPE_MIDI ? GRAPH_PORT_LIST_IN_MIDI
                                  : GRAPH_PORT_LIST_IN_AUDIO;
}

// the interned name of a port, malloced. '/' never occurs in the hex fields,
// so it is unique per (owner, name)
static char *port_ident(const GRAPH_NODE *node, const char *name) {
    int len = snprintf(NULL, 0, "%" PRIx64 "/%" PRIx64 "/%s", node->owner_tag,
                       node->owner_uid, name);
    if (len < 0)
        return NULL;
    char *ident = malloc((size_t)len + 1);
    if (!ident)
        return NULL;
    snprintf(ident, (size_t)len + 1, "%" PRIx64 "/%" PRIx64 "/%s",
             node->owner_tag, node->owner_uid, name);
    return ident;
}

static bool ident_present(const GRAPH *graph, const char *ident) {
    uint64_t key = intern_find(&graph->idents, ident);
    return key && intern_slot(&graph->idents, key) != INTERN_SLOT_NONE;
}

// view_max must already hold every port
static void view_rebuild(GRAPH *graph) {
    intern_detach_all(&graph->idents);
    size_t n = 0;
    for (size_t l = GRAPH_PORT_LIST_ALL + 1; l < GRAPH_PORT_LIST_COUNT; l++) {
        graph->list_first[l] = n;
        for (size_t i = 0; i < graph->node_count; i++) {
            GRAPH_NODE *node = graph->nodes[i];
            for (size_t p = 0; p < node->port_count; p++) {
                GRAPH_PORT *port = node->ports[p];
                if (port_list(port->type, port->flow) != l)
                    continue;
                intern_set_slot(&graph->idents, port->key, n);
                graph->view[n++] = port;
            }
        }
        graph->list_count[l] = n - graph->list_first[l];
    }
    graph->list_first[GRAPH_PORT_LIST_ALL] = 0;
    graph->list_count[GRAPH_PORT_LIST_ALL] = n;
}

static GRAPH_PORT *port_by_key(const GRAPH *graph, uint64_t key) {
    size_t slot = intern_slot(&graph->idents, key);
    if (slot >= graph->list_count[GRAPH_PORT_LIST_ALL])
        return NULL;
    return graph->view[slot];
}

static void port_info_fill(const GRAPH_PORT *port, GraphPortInfo *out) {
    out->name = port->name;
    out->key = port->key;
    out->type = port->type;
    out->flow = port->flow;
    out->owner_tag = port->node->owner_tag;
    out->owner_uid = port->node->owner_uid;
}

// remove the port from its peers' edges
static void port_detach(GRAPH_PORT *port) {
    for (size_t i = 0; i < port->peer_count; i++) {
        GRAPH_PORT *peer = port->peers[i];
        ports_remove(peer->peers, &peer->peer_count, port);
    }
    port->peer_count = 0;
}

static void port_free(GRAPH_PORT *port) {
    if (!port)
        return;
    free(port->name);
    free(port->buffer);
    midi_buf_free(port->midi_buf);
    free(port->peers);
    free(port);
}

GRAPH *graph_new(uint32_t max_buffer_size) {
    GRAPH *graph = calloc(1, sizeof(GRAPH));
    if (!graph)
        return NULL;
    graph->max_buffer_size = max_buffer_size;
    return graph;
}

void graph_free(GRAPH *graph) {
    if (!graph)
        return;
    for (size_t i = 0; i < graph->node_count; i++) {
        GRAPH_NODE *node = graph->nodes[i];
        for (size_t p = 0; p < node->port_count; p++)
            port_free(node->ports[p]);
        free(node->ports);
        free(node);
    }
    free(graph->nodes);
    free(graph->view);
    intern_clean(&graph->idents);
    free(graph);
}

uint64_t graph_generation(const GRAPH *graph) {
    return graph ? graph->generation : 0;
}

GRAPH_NODE *graph_node_add(GRAPH *graph, uint64_t owner_tag,
                           uint64_t owner_uid, GRAPH_PROCESS_FN process,
                           void *user_data) {
    if (!graph || owner_tag == 0)
        return NULL;
    if (graph->node_count == graph->node_max) {
        size_t new_max = graph->node_max ? graph->node_max * 2 : 8;
        GRAPH_NODE **grown =
            realloc(graph->nodes, sizeof(GRAPH_NODE *) * new_max);
        if (!grown)
            return NULL;
        graph->nodes = grown;
        graph->node_max = new_max;
    }
    GRAPH_NODE *node = calloc(1, sizeof(GRAPH_NODE));
    if (!node)
        return NULL;
    node->owner_tag = owner_tag;
    node->owner_uid = owner_uid;
    node->process = process;
    node->user_data = user_data;
    graph->nodes[graph->node_count++] = node;
    graph->generation++;
    return node;
}

void graph_node_remove(GRAPH *graph, GRAPH_NODE *node) {
    if (!graph || !node)
        return;
    for (size_t p = 0; p < node->port_count; p++) {
        port_detach(node->ports[p]);
        port_free(node->ports[p]);
    }
    free(node->ports);
    for (size_t i = 0; i < graph->node_count; i++) {
        if (graph->nodes[i] != node)
            continue;
        memmove(&graph->nodes[i], &graph->nodes[i + 1],
                sizeof(GRAPH_NODE *) * (graph->node_count - i - 1));
        graph->node_count--;
        break;
    }
    free(node);
    view_rebuild(graph);
    graph->generation++;
}

GRAPH_PORT *graph_port_create(GRAPH *graph, GRAPH_NODE *node,
                              unsigned int type, unsigned int flow,
                              const char *name) {
    if (!graph || !node || !name)
        return NULL;
    if (type != PORT_TYPE_AUDIO && type != PORT_TYPE_MIDI)
        return NULL;
    if (flow != PORT_FLOW_INPUT && flow != PORT_FLOW_OUTPUT)
        return NULL;
    char *ident = port_ident(node, name);
    if (!ident)
        return NULL;
    uint64_t key = 0;
    if (!ident_present(graph, ident))
        key = intern_add(&graph->idents, ident);
    free(ident);
    if (!key)
        return NULL;
    if (!ports_reserve(&node->ports, node->port_count, &node->port_max))
        return NULL;
    if (!ports_reserve(&graph->view, graph->list_count[GRAPH_PORT_LIST_ALL],
                       &graph->view_max))
        return NULL;

    GRAPH_PORT *port = calloc(1, sizeof(GRAPH_PORT));
    if (!port)
        return NULL;
    port->node = node;
    port->key = key;
    port->type = type;
    port->flow = flow;
    port->name = strdup(name);
    if (type == PORT_TYPE_AUDIO)
        port->buffer = calloc(graph->max_buffer_size, sizeof(SAMPLE_T));
    else
        port->midi_buf = midi_buf_new(MIDI_PORT_BUF_SIZE);
    if (!port->name || (!port->buffer && !port->midi_buf)) {
        port_free(port);
        return NULL;
    }
    node->ports[node->port_count++] = port;
    view_rebuild(graph);
    graph->generation++;
    return port;
}

void graph_port_remove(GRAPH *graph, GRAPH_PORT *port) {
    if (!graph || !port)
        return;
    port_detach(port);
    ports_remove(port->node->ports, &port->node->port_count, port);
    port_free(port);
    view_rebuild(graph);
    graph->generation++;
}

int graph_port_rename(GRAPH *graph, GRAPH_PORT *port, const char *name) {
    if (!graph || !port || !name)
        return -1;
    if (strcmp(port->name, name) == 0)
        return 0;
    char *old_ident = port_ident(port->node, port->name);
    char *new_ident = port_ident(port->node, name);
    char *name_copy = strdup(name);
    int result = -1;
    if (!old_ident || !new_ident || !name_copy)
        goto done;
    if (ident_present(graph, new_ident))
        goto done;
    // skipped when the new name was interned before - it keeps its own key
    intern_rename(&graph->idents, old_ident, new_ident);
    uint64_t key = intern_add(&graph->idents, new_ident);
    if (!key)
        goto done;
    if (key != port->key) {
        size_t slot = intern_slot(&graph->idents, port->key);
        intern_set_slot(&graph->idents, port->key, INTERN_SLOT_NONE);
        intern_set_slot(&graph->idents, key, slot);
        port->key = key;
    }
    free(port->name);
    port->name = name_copy;
    name_copy = NULL;
    graph->generation++;
    result = 0;
done:
    free(old_ident);
    free(new_ident);
    free(name_copy);
    return result;
}

size_t graph_port_count(const GRAPH *graph, GraphPortList list) {
    if (!graph || list >= GRAPH_PORT_LIST_COUNT)
        return 0;
    return graph->list_count[list];
}

bool graph_port_at(const GRAPH *graph, GraphPortList list, size_t idx,
                   GraphPortInfo *out) {
    if (!graph || !out || list >= GRAPH_PORT_LIST_COUNT)
        return false;
    if (idx >= graph->list_count[list])
        return false;
    port_info_fill(graph->view[graph->list_first[list] + idx], out);
    return true;
}

bool graph_port_by_key(const GRAPH *graph, uint64_t key, GraphPortInfo *out) {
    if (!graph || !out)
        return false;
    GRAPH_PORT *port = port_by_key(graph, key);
    if (!port)
        return false;
    port_info_fill(port, out);
    return true;
}

size_t graph_port_connection_count(const GRAPH *graph, uint64_t key) {
    if (!graph)
        return 0;
    GRAPH_PORT *port = port_by_key(graph, key);
    return port ? port->peer_count : 0;
}

bool graph_port_connection_at(const GRAPH *graph, uint64_t key, size_t idx,
                              GraphPortInfo *out) {
    if (!graph || !out)
        return false;
    GRAPH_PORT *port = port_by_key(graph, key);
    if (!port || idx >= port->peer_count)
        return false;
    port_info_fill(port->peers[idx], out);
    return true;
}

static bool ports_linked(const GRAPH_PORT *a, const GRAPH_PORT *b) {
    for (size_t i = 0; i < a->peer_count; i++) {
        if (a->peers[i] == b)
            return true;
    }
    return false;
}

bool graph_port_keys_connected(const GRAPH *graph, uint64_t key_a,
                               uint64_t key_b) {
    if (!graph)
        return false;
    GRAPH_PORT *a = port_by_key(graph, key_a);
    GRAPH_PORT *b = port_by_key(graph, key_b);
    return a && b && ports_linked(a, b);
}

// the keys as an output and an input of the same type, false otherwise
static bool ports_pair(const GRAPH *graph, uint64_t key_a, uint64_t key_b,
                       GRAPH_PORT **out, GRAPH_PORT **in) {
    GRAPH_PORT *a = port_by_key(graph, key_a);
    GRAPH_PORT *b = port_by_key(graph, key_b);
    if (!a || !b || a->flow == b->flow || a->type != b->type)
        return false;
    *out = a->flow == PORT_FLOW_OUTPUT ? a : b;
    *in = a->flow == PORT_FLOW_OUTPUT ? b : a;
    return true;
}

// whether to is reachable from from along edges, from itself included. true
// on allocation failure, so a caller refuses what it can not check
static bool node_reaches(const GRAPH *graph, GRAPH_NODE *from,
                         const GRAPH_NODE *to) {
    if (from == to)
        return true;
    // breadth first, the queue doubles as the list of nodes to unmark
    GRAPH_NODE **queue = malloc(sizeof(GRAPH_NODE *) * graph->node_count);
    if (!queue)
        return true;
    size_t count = 0;
    from->visited = true;
    queue[count++] = from;
    for (size_t q = 0; q < count && !to->visited; q++) {
        GRAPH_NODE *node = queue[q];
        for (size_t p = 0; p < node->port_count; p++) {
            GRAPH_PORT *port = node->ports[p];
            if (port->flow != PORT_FLOW_OUTPUT)
                continue;
            for (size_t e = 0; e < port->peer_count; e++) {
                GRAPH_NODE *next = port->peers[e]->node;
                if (next->visited)
                    continue;
                next->visited = true;
                queue[count++] = next;
            }
        }
    }
    bool found = to->visited;
    for (size_t q = 0; q < count; q++)
        queue[q]->visited = false;
    free(queue);
    return found;
}

// a new edge out -> in: no loop, room for one more MIDI source
static bool edge_allowed(const GRAPH *graph, GRAPH_PORT *out,
                         const GRAPH_PORT *in) {
    if (in->type == PORT_TYPE_MIDI && in->peer_count >= MIDI_BUF_MERGE_MAX)
        return false;
    return !node_reaches(graph, in->node, out->node);
}

bool graph_port_keys_connectable(const GRAPH *graph, uint64_t key_a,
                                 uint64_t key_b) {
    GRAPH_PORT *out, *in;
    if (!graph || !ports_pair(graph, key_a, key_b, &out, &in))
        return false;
    return ports_linked(out, in) || edge_allowed(graph, out, in);
}

int graph_connect_keys(GRAPH *graph, uint64_t key_a, uint64_t key_b) {
    GRAPH_PORT *out, *in;
    if (!graph || !ports_pair(graph, key_a, key_b, &out, &in))
        return -1;
    if (ports_linked(out, in))
        return 0;
    if (!edge_allowed(graph, out, in))
        return -1;
    if (!ports_reserve(&out->peers, out->peer_count, &out->peer_max) ||
        !ports_reserve(&in->peers, in->peer_count, &in->peer_max))
        return -1;
    out->peers[out->peer_count++] = in;
    in->peers[in->peer_count++] = out;
    graph->generation++;
    return 0;
}

int graph_disconnect_keys(GRAPH *graph, uint64_t key_a, uint64_t key_b) {
    if (!graph)
        return -1;
    GRAPH_PORT *a = port_by_key(graph, key_a);
    GRAPH_PORT *b = port_by_key(graph, key_b);
    if (!a || !b || !ports_linked(a, b))
        return -1;
    ports_remove(a->peers, &a->peer_count, b);
    ports_remove(b->peers, &b->peer_count, a);
    graph->generation++;
    return 0;
}
