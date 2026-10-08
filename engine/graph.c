#include "graph.h"
#include "../types.h"
#include "../util_funcs/intern_table.h"
#include "../util_funcs/midi_buf.h"
#include <inttypes.h>
#include <stdatomic.h>
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
    // by type. An output's, written by its node; an input's from its 2nd
    // source on, the sum
    SAMPLE_T *buffer;   // max_buffer_size frames
    MIDI_BUF *midi_buf; // MIDI_PORT_BUF_SIZE bytes
    // [audio-thread] the cycle's buffer, by type, set by the plan before the
    // node runs - what graph_port_*_rt return
    SAMPLE_T *rt_buffer;
    MIDI_BUF *rt_midi_buf;
    // the other end of each edge, in connection order
    GRAPH_PORT **peers;
    size_t peer_count;
    size_t peer_max;
    // removed: freed once the ack reaches free_at
    GRAPH_PORT *retired_next;
    unsigned int free_at;
};

struct _graph_node {
    uint64_t owner_tag;
    uint64_t owner_uid;
    GRAPH_PROCESS_FN process;
    void *user_data;
    uint32_t latency; // frames
    bool visited;     // node_reaches scratch, false between calls
    size_t pending;   // plan_build scratch: in edges not ordered yet
    GRAPH_PORT **ports;
    size_t port_count;
    size_t port_max;
};

// one port of a plan node, its buffers copied from the GRAPH_PORT
typedef struct {
    GRAPH_PORT *port; // [audio-thread] sets only its rt slot
    unsigned int type;
    SAMPLE_T *buffer;
    MIDI_BUF *midi_buf;
    // an input's sources in connection order, into GRAPH_PLAN.audio_srcs or
    // midi_srcs by type
    size_t src_first;
    size_t src_count;
} PLAN_PORT;

typedef struct {
    GRAPH_PROCESS_FN process;
    void *user_data;
    size_t port_first; // into GRAPH_PLAN.ports: the inputs, then the outputs
    size_t in_count;
    size_t out_count;
} PLAN_NODE;

// what graph_process_rt runs, never changed once published. Buffers are
// copied in, [audio-thread] touches no GRAPH_PORT field but the rt slot
typedef struct _graph_plan GRAPH_PLAN;
struct _graph_plan {
    unsigned int gen;
    uint32_t max_frames; // what its audio buffers hold
    PLAN_NODE *nodes;    // edge order
    size_t node_count;
    PLAN_PORT *ports;
    SAMPLE_T **audio_srcs;
    MIDI_BUF **midi_srcs;
    // what an input without sources reads
    SAMPLE_T *silence;
    MIDI_BUF *midi_empty;
    // [main-thread], once replaced: freed when the ack reaches free_at
    GRAPH_PLAN *retired_next;
    unsigned int free_at;
};

// the audio buffers one resize replaced, freed when the ack reaches free_at
typedef struct _retired_bufs RETIRED_BUFS;
struct _retired_bufs {
    RETIRED_BUFS *retired_next;
    unsigned int free_at;
    size_t count;
    SAMPLE_T *bufs[];
};

struct _graph {
    uint32_t max_buffer_size;
    // read-only, for inputs without sources
    SAMPLE_T *silence;
    MIDI_BUF *midi_empty;
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
    // graph_peer_*: the ports connect takes with peer_view_source, as of
    // peer_view_generation. Source 0 = nothing built
    GRAPH_PORT **peer_view;
    size_t peer_view_count;
    size_t peer_view_max;
    uint64_t peer_view_source;
    uint64_t peer_view_generation;
    // the newest plan and the gen [audio-thread] last started a cycle with
    _Atomic(GRAPH_PLAN *) plan;
    atomic_uint ack;
    unsigned int plan_gen;    // the newest plan's, 0 = none yet
    uint64_t plan_built_from; // generation it was built from
    GRAPH_PLAN *retired_plans;
    GRAPH_PORT *retired_ports;
    RETIRED_BUFS *retired_bufs;
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

// a removed port: a published plan may still hold its buffer, the next one
// does not
static void port_retire(GRAPH *graph, GRAPH_PORT *port) {
    if (graph->plan_gen == 0) {
        port_free(port);
        return;
    }
    port->free_at = graph->plan_gen + 1;
    port->retired_next = graph->retired_ports;
    graph->retired_ports = port;
}

static void plan_free(GRAPH_PLAN *plan) {
    if (!plan)
        return;
    free(plan->nodes);
    free(plan->ports);
    free(plan->audio_srcs);
    free(plan->midi_srcs);
    free(plan);
}

static void retired_bufs_free(RETIRED_BUFS *old) {
    for (size_t i = 0; i < old->count; i++)
        free(old->bufs[i]);
    free(old);
}

// free what [audio-thread] can not run any more
static void retired_reclaim(GRAPH *graph) {
    unsigned int ack = atomic_load(&graph->ack);
    for (GRAPH_PLAN **p = &graph->retired_plans; *p;) {
        GRAPH_PLAN *plan = *p;
        if (plan->free_at > ack) {
            p = &plan->retired_next;
            continue;
        }
        *p = plan->retired_next;
        plan_free(plan);
    }
    for (GRAPH_PORT **p = &graph->retired_ports; *p;) {
        GRAPH_PORT *port = *p;
        if (port->free_at > ack) {
            p = &port->retired_next;
            continue;
        }
        *p = port->retired_next;
        port_free(port);
    }
    for (RETIRED_BUFS **p = &graph->retired_bufs; *p;) {
        RETIRED_BUFS *old = *p;
        if (old->free_at > ack) {
            p = &old->retired_next;
            continue;
        }
        *p = old->retired_next;
        retired_bufs_free(old);
    }
}

GRAPH *graph_new(uint32_t max_buffer_size) {
    GRAPH *graph = calloc(1, sizeof(GRAPH));
    if (!graph)
        return NULL;
    graph->max_buffer_size = max_buffer_size;
    graph->silence = calloc(max_buffer_size, sizeof(SAMPLE_T));
    graph->midi_empty = midi_buf_new(MIDI_PORT_BUF_SIZE);
    if (!graph->silence || !graph->midi_empty) {
        free(graph->silence);
        midi_buf_free(graph->midi_empty);
        free(graph);
        return NULL;
    }
    atomic_init(&graph->plan, NULL);
    atomic_init(&graph->ack, 0U);
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
    free(graph->peer_view);
    intern_clean(&graph->idents);
    plan_free(atomic_load(&graph->plan));
    while (graph->retired_plans) {
        GRAPH_PLAN *next = graph->retired_plans->retired_next;
        plan_free(graph->retired_plans);
        graph->retired_plans = next;
    }
    while (graph->retired_ports) {
        GRAPH_PORT *next = graph->retired_ports->retired_next;
        port_free(graph->retired_ports);
        graph->retired_ports = next;
    }
    while (graph->retired_bufs) {
        RETIRED_BUFS *next = graph->retired_bufs->retired_next;
        retired_bufs_free(graph->retired_bufs);
        graph->retired_bufs = next;
    }
    free(graph->silence);
    midi_buf_free(graph->midi_empty);
    free(graph);
}

uint64_t graph_generation(const GRAPH *graph) {
    return graph ? graph->generation : 0;
}

int graph_max_buffer_size_set(GRAPH *graph, uint32_t max_buffer_size) {
    if (!graph || max_buffer_size == 0)
        return -1;
    if (max_buffer_size == graph->max_buffer_size)
        return 0;
    // every port's audio buffer, then the silence
    size_t port_total = graph->list_count[GRAPH_PORT_LIST_ALL];
    size_t count = 1;
    for (size_t i = 0; i < port_total; i++) {
        if (graph->view[i]->buffer)
            count++;
    }
    RETIRED_BUFS *old =
        malloc(sizeof(RETIRED_BUFS) + sizeof(SAMPLE_T *) * count);
    if (!old)
        return -1;
    // holds the new buffers until all are made, then the ones they replace
    old->count = 0;
    for (size_t i = 0; i < count; i++) {
        SAMPLE_T *buf = calloc(max_buffer_size, sizeof(SAMPLE_T));
        if (!buf) {
            retired_bufs_free(old);
            return -1;
        }
        old->bufs[old->count++] = buf;
    }
    size_t b = 0;
    for (size_t i = 0; i < port_total; i++) {
        GRAPH_PORT *port = graph->view[i];
        if (!port->buffer)
            continue;
        SAMPLE_T *prev = port->buffer;
        port->buffer = old->bufs[b];
        old->bufs[b++] = prev;
    }
    SAMPLE_T *prev = graph->silence;
    graph->silence = old->bufs[b];
    old->bufs[b] = prev;
    graph->max_buffer_size = max_buffer_size;
    graph->generation++;
    // the published plan runs them until the next one is acked. An rt slot
    // set at create may point at one too - only read by its node's process,
    // which the next plan gives the new buffer first
    if (graph->plan_gen == 0) {
        retired_bufs_free(old);
        return 0;
    }
    old->free_at = graph->plan_gen + 1;
    old->retired_next = graph->retired_bufs;
    graph->retired_bufs = old;
    return 0;
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
        port_retire(graph, node->ports[p]);
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
    if (flow == PORT_FLOW_OUTPUT && type == PORT_TYPE_AUDIO)
        port->buffer = calloc(graph->max_buffer_size, sizeof(SAMPLE_T));
    else if (flow == PORT_FLOW_OUTPUT)
        port->midi_buf = midi_buf_new(MIDI_PORT_BUF_SIZE);
    if (!port->name ||
        (flow == PORT_FLOW_OUTPUT && !port->buffer && !port->midi_buf)) {
        port_free(port);
        return NULL;
    }
    // until a plan has the port
    if (type == PORT_TYPE_AUDIO)
        port->rt_buffer = port->buffer ? port->buffer : graph->silence;
    else
        port->rt_midi_buf = port->midi_buf ? port->midi_buf : graph->midi_empty;
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
    port_retire(graph, port);
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

// marks from and the nodes reached from it over the edges of its ports of
// `flow` (OUTPUT: downstream, INPUT: upstream), until stop (may be NULL) is
// marked. Returns the marked nodes for nodes_unmark, NULL on allocation failure
static GRAPH_NODE **nodes_mark(const GRAPH *graph, GRAPH_NODE *from,
                               unsigned int flow, const GRAPH_NODE *stop,
                               size_t *count) {
    // breadth first, the queue doubles as the list of nodes to unmark
    GRAPH_NODE **queue = malloc(sizeof(GRAPH_NODE *) * graph->node_count);
    if (!queue)
        return NULL;
    size_t n = 0;
    from->visited = true;
    queue[n++] = from;
    for (size_t q = 0; q < n && !(stop && stop->visited); q++) {
        GRAPH_NODE *node = queue[q];
        for (size_t p = 0; p < node->port_count; p++) {
            GRAPH_PORT *port = node->ports[p];
            if (port->flow != flow)
                continue;
            for (size_t e = 0; e < port->peer_count; e++) {
                GRAPH_NODE *next = port->peers[e]->node;
                if (next->visited)
                    continue;
                next->visited = true;
                queue[n++] = next;
            }
        }
    }
    *count = n;
    return queue;
}

static void nodes_unmark(GRAPH_NODE **marked, size_t count) {
    for (size_t i = 0; i < count; i++)
        marked[i]->visited = false;
    free(marked);
}

// whether to is reachable from from along edges, from itself included. true
// on allocation failure, so a caller refuses what it can not check
static bool node_reaches(const GRAPH *graph, GRAPH_NODE *from,
                         const GRAPH_NODE *to) {
    if (from == to)
        return true;
    size_t count;
    GRAPH_NODE **marked = nodes_mark(graph, from, PORT_FLOW_OUTPUT, to, &count);
    if (!marked)
        return true;
    bool found = to->visited;
    nodes_unmark(marked, count);
    return found;
}

// the buffer an input sums 2+ sources into, kept once made
static bool input_sum_reserve(const GRAPH *graph, GRAPH_PORT *in) {
    if (in->buffer || in->midi_buf)
        return true;
    if (in->type == PORT_TYPE_AUDIO)
        in->buffer = calloc(graph->max_buffer_size, sizeof(SAMPLE_T));
    else
        in->midi_buf = midi_buf_new(MIDI_PORT_BUF_SIZE);
    return in->buffer || in->midi_buf;
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
    if (in->peer_count > 0 && !input_sum_reserve(graph, in))
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

// what edge_allowed decides per pair, for every candidate of one source with
// one search: a new edge closes a loop through exactly the nodes upstream of
// an output's node / downstream of an input's (its own node included)
static bool peer_view_update(GRAPH *graph, uint64_t source_key) {
    if (source_key != 0 && source_key == graph->peer_view_source &&
        graph->peer_view_generation == graph->generation)
        return true;
    graph->peer_view_source = 0;
    graph->peer_view_count = 0;
    GRAPH_PORT *source = port_by_key(graph, source_key);
    if (!source)
        return false;
    unsigned int other = source->flow == PORT_FLOW_OUTPUT ? PORT_FLOW_INPUT
                                                          : PORT_FLOW_OUTPUT;
    GraphPortList list = port_list(source->type, other);
    size_t first = graph->list_first[list];
    size_t n = graph->list_count[list];
    if (n > graph->peer_view_max) {
        GRAPH_PORT **grown =
            realloc(graph->peer_view, sizeof(GRAPH_PORT *) * n);
        if (!grown)
            return false;
        graph->peer_view = grown;
        graph->peer_view_max = n;
    }
    size_t marked_count;
    GRAPH_NODE **marked =
        nodes_mark(graph, source->node, other, NULL, &marked_count);
    if (!marked)
        return false;
    for (size_t i = 0; i < n; i++) {
        GRAPH_PORT *peer = graph->view[first + i];
        const GRAPH_PORT *in = other == PORT_FLOW_INPUT ? peer : source;
        bool room =
            in->type != PORT_TYPE_MIDI || in->peer_count < MIDI_BUF_MERGE_MAX;
        if (ports_linked(source, peer) || (!peer->node->visited && room))
            graph->peer_view[graph->peer_view_count++] = peer;
    }
    nodes_unmark(marked, marked_count);
    graph->peer_view_source = source_key;
    graph->peer_view_generation = graph->generation;
    return true;
}

size_t graph_peer_count(GRAPH *graph, uint64_t source_key) {
    if (!graph || !peer_view_update(graph, source_key))
        return 0;
    return graph->peer_view_count;
}

bool graph_peer_at(GRAPH *graph, uint64_t source_key, size_t idx,
                   GraphPortInfo *out) {
    if (!graph || !out || !peer_view_update(graph, source_key))
        return false;
    if (idx >= graph->peer_view_count)
        return false;
    port_info_fill(graph->peer_view[idx], out);
    return true;
}

static PLAN_PORT *plan_port_fill(PLAN_PORT *plan_port, GRAPH_PORT *port) {
    *plan_port = (PLAN_PORT){.port = port,
                             .type = port->type,
                             .buffer = port->buffer,
                             .midi_buf = port->midi_buf};
    return plan_port;
}

// nodes in edge order (Kahn, ties in add order), per node its ports, per input
// its sources. NULL on allocation failure
static GRAPH_PLAN *plan_build(GRAPH *graph) {
    size_t n = graph->node_count;
    size_t port_total = 0;
    size_t audio_total = 0;
    size_t midi_total = 0;
    for (size_t i = 0; i < n; i++) {
        GRAPH_NODE *node = graph->nodes[i];
        node->pending = 0;
        port_total += node->port_count;
        for (size_t p = 0; p < node->port_count; p++) {
            GRAPH_PORT *port = node->ports[p];
            if (port->flow != PORT_FLOW_INPUT)
                continue;
            if (port->type == PORT_TYPE_AUDIO)
                audio_total += port->peer_count;
            else
                midi_total += port->peer_count;
            node->pending += port->peer_count;
        }
    }
    GRAPH_PLAN *plan = calloc(1, sizeof(GRAPH_PLAN));
    GRAPH_NODE **order = malloc(sizeof(GRAPH_NODE *) * (n ? n : 1));
    if (plan) {
        plan->nodes = malloc(sizeof(PLAN_NODE) * (n ? n : 1));
        plan->ports = malloc(sizeof(PLAN_PORT) * (port_total ? port_total : 1));
        plan->audio_srcs =
            malloc(sizeof(SAMPLE_T *) * (audio_total ? audio_total : 1));
        plan->midi_srcs =
            malloc(sizeof(MIDI_BUF *) * (midi_total ? midi_total : 1));
    }
    if (!plan || !order || !plan->nodes || !plan->ports || !plan->audio_srcs ||
        !plan->midi_srcs)
        goto fail;

    // the queue doubles as the order
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (graph->nodes[i]->pending == 0)
            order[count++] = graph->nodes[i];
    }
    for (size_t q = 0; q < count; q++) {
        GRAPH_NODE *node = order[q];
        for (size_t p = 0; p < node->port_count; p++) {
            GRAPH_PORT *port = node->ports[p];
            if (port->flow != PORT_FLOW_OUTPUT)
                continue;
            for (size_t e = 0; e < port->peer_count; e++) {
                GRAPH_NODE *next = port->peers[e]->node;
                if (--next->pending == 0)
                    order[count++] = next;
            }
        }
    }
    // a loop - connect refuses them
    if (count != n)
        goto fail;

    size_t port_i = 0;
    size_t audio_i = 0;
    size_t midi_i = 0;
    for (size_t i = 0; i < n; i++) {
        GRAPH_NODE *node = order[i];
        PLAN_NODE *plan_node = &plan->nodes[i];
        plan_node->process = node->process;
        plan_node->user_data = node->user_data;
        plan_node->port_first = port_i;
        for (size_t p = 0; p < node->port_count; p++) {
            GRAPH_PORT *port = node->ports[p];
            if (port->flow != PORT_FLOW_INPUT)
                continue;
            PLAN_PORT *in = plan_port_fill(&plan->ports[port_i++], port);
            bool audio = port->type == PORT_TYPE_AUDIO;
            in->src_first = audio ? audio_i : midi_i;
            in->src_count = port->peer_count;
            for (size_t e = 0; e < port->peer_count; e++) {
                if (audio)
                    plan->audio_srcs[audio_i++] = port->peers[e]->buffer;
                else
                    plan->midi_srcs[midi_i++] = port->peers[e]->midi_buf;
            }
        }
        plan_node->in_count = port_i - plan_node->port_first;
        for (size_t p = 0; p < node->port_count; p++) {
            if (node->ports[p]->flow == PORT_FLOW_OUTPUT)
                plan_port_fill(&plan->ports[port_i++], node->ports[p]);
        }
        plan_node->out_count =
            port_i - plan_node->port_first - plan_node->in_count;
    }
    plan->node_count = n;
    plan->max_frames = graph->max_buffer_size;
    plan->silence = graph->silence;
    plan->midi_empty = graph->midi_empty;
    free(order);
    return plan;
fail:
    free(order);
    plan_free(plan);
    return NULL;
}

int graph_plan_update(GRAPH *graph) {
    if (!graph)
        return -1;
    retired_reclaim(graph);
    if (graph->plan_built_from == graph->generation)
        return 0;
    GRAPH_PLAN *plan = plan_build(graph);
    if (!plan)
        return -1;
    plan->gen = graph->plan_gen + 1;
    GRAPH_PLAN *old = atomic_exchange(&graph->plan, plan);
    graph->plan_gen = plan->gen;
    graph->plan_built_from = graph->generation;
    if (old) {
        old->free_at = plan->gen;
        old->retired_next = graph->retired_plans;
        graph->retired_plans = old;
    }
    return 0;
}

// the input's rt slot: silence / empty, its one source, or the sum of 2+
static void plan_input_rt(const GRAPH_PLAN *plan, const PLAN_PORT *in,
                          NFRAMES_T nframes) {
    if (in->type == PORT_TYPE_AUDIO) {
        SAMPLE_T *const *srcs = &plan->audio_srcs[in->src_first];
        SAMPLE_T *buf = plan->silence;
        if (in->src_count == 1) {
            buf = srcs[0];
        } else if (in->src_count > 1) {
            buf = in->buffer;
            memcpy(buf, srcs[0], sizeof(SAMPLE_T) * nframes);
            for (size_t s = 1; s < in->src_count; s++) {
                for (NFRAMES_T f = 0; f < nframes; f++)
                    buf[f] += srcs[s][f];
            }
        }
        in->port->rt_buffer = buf;
        return;
    }
    MIDI_BUF *const *srcs = &plan->midi_srcs[in->src_first];
    MIDI_BUF *buf = plan->midi_empty;
    if (in->src_count == 1) {
        buf = srcs[0];
    } else if (in->src_count > 1) {
        buf = in->midi_buf;
        midi_buf_clear(buf, nframes);
        midi_buf_merge(buf, (const MIDI_BUF *const *)srcs,
                       (uint32_t)in->src_count);
    }
    in->port->rt_midi_buf = buf;
}

void graph_process_rt(GRAPH *graph, NFRAMES_T nframes) {
    GRAPH_PLAN *plan = atomic_load(&graph->plan);
    if (!plan || nframes > plan->max_frames)
        return;
    // from here the older plans are free to go
    atomic_store(&graph->ack, plan->gen);
    for (size_t i = 0; i < plan->node_count; i++) {
        const PLAN_NODE *node = &plan->nodes[i];
        const PLAN_PORT *ins = &plan->ports[node->port_first];
        const PLAN_PORT *outs = ins + node->in_count;
        for (size_t p = 0; p < node->in_count; p++)
            plan_input_rt(plan, &ins[p], nframes);
        for (size_t p = 0; p < node->out_count; p++) {
            if (outs[p].type == PORT_TYPE_AUDIO) {
                outs[p].port->rt_buffer = outs[p].buffer;
                continue;
            }
            midi_buf_clear(outs[p].midi_buf, nframes);
            outs[p].port->rt_midi_buf = outs[p].midi_buf;
        }
        if (node->process && node->process(node->user_data, nframes))
            continue;
        // did not write them - downstream must not read the last cycle's
        for (size_t p = 0; p < node->out_count; p++) {
            if (outs[p].type == PORT_TYPE_AUDIO)
                memset(outs[p].buffer, 0, sizeof(SAMPLE_T) * nframes);
            else
                midi_buf_clear(outs[p].midi_buf, nframes);
        }
    }
}

void graph_midi_drops_take(GRAPH *graph, GRAPH_DROPS_FN fn, void *arg) {
    if (!graph || !fn)
        return;
    for (size_t i = 0; i < graph->list_count[GRAPH_PORT_LIST_ALL]; i++) {
        GRAPH_PORT *port = graph->view[i];
        // an output's own buffer, or an input's sum
        uint32_t dropped = midi_buf_dropped_take(port->midi_buf);
        if (dropped == 0)
            continue;
        GraphPortInfo info;
        port_info_fill(port, &info);
        fn(arg, &info, dropped);
    }
}

SAMPLE_T *graph_port_audio_rt(const GRAPH_PORT *port) {
    return port ? port->rt_buffer : NULL;
}

MIDI_BUF *graph_port_midi_rt(const GRAPH_PORT *port) {
    return port ? port->rt_midi_buf : NULL;
}
