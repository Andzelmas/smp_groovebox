#pragma once
#include "../engine/graph.h"
#include "../structs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The audio backend, picked at compile time (jack_backend.c). Only app_data uses
// it. It runs the process callback; its exposed ports are graph endpoint nodes,
// every other port belongs to the graph. All [main-thread]
typedef struct _audio_backend AUDIO_BACKEND;

// process runs on [audio-thread] once activated, with arg. NULL on failure
AUDIO_BACKEND *audio_backend_init(void *arg, const char *client_name,
                                  int (*process)(NFRAMES_T nframes, void *arg));
int audio_backend_activate(AUDIO_BACKEND *backend);
// stops the process callback first
void audio_backend_clean(AUDIO_BACKEND *backend);
SAMPLE_T audio_backend_sample_rate(AUDIO_BACKEND *backend);
uint32_t audio_backend_buffer_size(AUDIO_BACKEND *backend);

// registers the exposed ports, as two nodes of graph owned by (owner_tag,
// owner_uid): "in" - outputs the backend fills, "out" - inputs whose sums it
// takes. Before activate, once. 0 on success
int audio_backend_endpoints_add(AUDIO_BACKEND *backend, GRAPH *graph,
                                uint64_t owner_tag, uint64_t owner_uid);

// The ports outside the app the exposed ones can link to, the exposed ones
// among them. A backend without other programs lists only its own.

// which list to read. ALL is every port; the others are the ports a source of
// the opposite flow and the same type can be connected to
typedef enum {
    BACKEND_PORT_LIST_ALL = 0,
    BACKEND_PORT_LIST_OUT_AUDIO,
    BACKEND_PORT_LIST_OUT_MIDI,
    BACKEND_PORT_LIST_IN_AUDIO,
    BACKEND_PORT_LIST_IN_MIDI,
    BACKEND_PORT_LIST_COUNT
} BackendPortList;

// one port. name/client are borrowed, valid until the next
// audio_backend_ports_sync that rebuilds the list
typedef struct _backend_port_info {
    const char *name;   // full "client:port"
    const char *client; // the client part of name
    uint64_t key;       // minted the first time this name was seen, stable for
                        // the life of the program
    unsigned int type;  // PORT_TYPE_AUDIO or PORT_TYPE_MIDI
    unsigned int flow;  // PORT_FLOW_OUTPUT or PORT_FLOW_INPUT
    // the exposed ports' owner, as given to audio_backend_endpoints_add. Both 0
    // for a port of another program
    uint64_t owner_tag;
    uint64_t owner_uid;
} BackendPortInfo;

// what audio_backend_ports_sync rebuilt
typedef struct _backend_port_sync {
    bool ports;
    bool connections;
} BackendPortSync;

// rebuild whatever changed since the last call
BackendPortSync audio_backend_ports_sync(AUDIO_BACKEND *backend);
size_t audio_backend_port_count(AUDIO_BACKEND *backend, BackendPortList list);
bool audio_backend_port_at(AUDIO_BACKEND *backend, BackendPortList list,
                           size_t idx, BackendPortInfo *out);
bool audio_backend_port_by_key(AUDIO_BACKEND *backend, uint64_t key,
                               BackendPortInfo *out);
// ports the one named by key is connected to
size_t audio_backend_port_connection_count(AUDIO_BACKEND *backend,
                                           uint64_t key);
bool audio_backend_port_connection_at(AUDIO_BACKEND *backend, uint64_t key,
                                      size_t idx, BackendPortInfo *out);
bool audio_backend_port_keys_connected(AUDIO_BACKEND *backend, uint64_t key_a,
                                       uint64_t key_b);
// link or unlink two ports of opposite flow, either order. 0 on success
int audio_backend_connect_keys(AUDIO_BACKEND *backend, uint64_t key_a,
                               uint64_t key_b);
int audio_backend_disconnect_keys(AUDIO_BACKEND *backend, uint64_t key_a,
                                  uint64_t key_b);
