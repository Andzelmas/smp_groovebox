#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../structs.h"
#include "../util_funcs/midi_buf.h"

// The app's own routing: nodes, their ports and the edges between them. The
// edges never form a loop between nodes. Knows no backend. Every function here
// is [main-thread], except the *_rt ones.

typedef struct _graph GRAPH;
typedef struct _graph_node GRAPH_NODE;
typedef struct _graph_port GRAPH_PORT;

// true = wrote its outputs this cycle, false = the plan zeroes / empties them
typedef bool (*GRAPH_PROCESS_FN)(void *user_data, NFRAMES_T nframes);

// which list to read. ALL is every port; the others are the ports a source of
// the opposite flow and the same type can be connected to
typedef enum {
    GRAPH_PORT_LIST_ALL = 0,
    GRAPH_PORT_LIST_OUT_AUDIO,
    GRAPH_PORT_LIST_OUT_MIDI,
    GRAPH_PORT_LIST_IN_AUDIO,
    GRAPH_PORT_LIST_IN_MIDI,
    GRAPH_PORT_LIST_COUNT
} GraphPortList;

// one port. name is borrowed, valid until the port is renamed or removed
typedef struct _graph_port_info {
    const char *name;
    uint64_t key;      // minted per (owner, name): stable for the session, a
                       // port re-created under the same owner and name gets
                       // it back. Never saved
    unsigned int type; // PORT_TYPE_AUDIO or PORT_TYPE_MIDI
    unsigned int flow; // PORT_FLOW_OUTPUT or PORT_FLOW_INPUT
    // the node's owner, as given to graph_node_add
    uint64_t owner_tag;
    uint64_t owner_uid;
} GraphPortInfo;

// max_buffer_size: frames an audio port's buffer holds. NULL on failure
GRAPH *graph_new(uint32_t max_buffer_size);
// graph_process_rt must not run any more
void graph_free(GRAPH *graph);

// bumped by every change to nodes, ports or edges
uint64_t graph_generation(const GRAPH *graph);

// owner_tag/owner_uid name what the node belongs to - the graph interprets
// neither, it hands them back on its ports' GraphPortInfo. Both must be final,
// owner_tag non-zero. Nodes may share an owner. process may be NULL
GRAPH_NODE *graph_node_add(GRAPH *graph, uint64_t owner_tag,
                           uint64_t owner_uid, GRAPH_PROCESS_FN process,
                           void *user_data);
// its ports and their edges go with it. The plan running may call process
// until a newer one is acked - stop the owner first, keep user_data valid
void graph_node_remove(GRAPH *graph, GRAPH_NODE *node);

// type PORT_TYPE_AUDIO or PORT_TYPE_MIDI, flow PORT_FLOW_INPUT or
// PORT_FLOW_OUTPUT. name is copied, unique among the ports of the node's
// owner. NULL on failure or a taken name
GRAPH_PORT *graph_port_create(GRAPH *graph, GRAPH_NODE *node,
                              unsigned int type, unsigned int flow,
                              const char *name);
// its edges go with it. The owner stops reading it (graph_port_*_rt) first
void graph_port_remove(GRAPH *graph, GRAPH_PORT *port);
// keeps the key, unless the owner had a removed port of that name - then that
// port's key. 0 on success, -1 on failure or a taken name
int graph_port_rename(GRAPH *graph, GRAPH_PORT *port, const char *name);

size_t graph_port_count(const GRAPH *graph, GraphPortList list);
bool graph_port_at(const GRAPH *graph, GraphPortList list, size_t idx,
                   GraphPortInfo *out);
bool graph_port_by_key(const GRAPH *graph, uint64_t key, GraphPortInfo *out);
// ports the one named by key is connected to, in connection order
size_t graph_port_connection_count(const GRAPH *graph, uint64_t key);
bool graph_port_connection_at(const GRAPH *graph, uint64_t key, size_t idx,
                              GraphPortInfo *out);
bool graph_port_keys_connected(const GRAPH *graph, uint64_t key_a,
                               uint64_t key_b);
// whether graph_connect_keys takes the pair, true when already connected
bool graph_port_keys_connectable(const GRAPH *graph, uint64_t key_a,
                                 uint64_t key_b);
// an edge from an output to an input of the same type, either order. Refused
// when it closes a loop (a node feeding itself, directly or through others) or
// gives a MIDI input more than MIDI_BUF_MERGE_MAX sources. 0 on success or
// when already connected
int graph_connect_keys(GRAPH *graph, uint64_t key_a, uint64_t key_b);
// 0 when the edge was there
int graph_disconnect_keys(GRAPH *graph, uint64_t key_a, uint64_t key_b);

// builds a new plan when the graph changed since the last one and hands it to
// graph_process_rt; frees old plans and removed ports once it no longer runs
// them. Call regularly. -1 when the build failed - the old plan stays, the
// next call retries
int graph_plan_update(GRAPH *graph);
// [audio-thread] one cycle of the newest plan: the nodes in edge order, each
// once its inputs are ready. nframes <= max_buffer_size
void graph_process_rt(GRAPH *graph, NFRAMES_T nframes);

// [audio-thread] inside its node's process: the port's buffer this cycle,
// NULL for the other type. An input's is read-only: silence / empty without
// sources, the source's own with one, their sum with 2+. A MIDI output is
// cleared for nframes before the node runs
SAMPLE_T *graph_port_audio_rt(const GRAPH_PORT *port);
MIDI_BUF *graph_port_midi_rt(const GRAPH_PORT *port);
