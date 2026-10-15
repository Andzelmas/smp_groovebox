#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/types.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
// my libraries
#include "audio_backend.h"
#include "../types.h"
#include "../util_funcs/intern_table.h"
#include "../util_funcs/midi_buf.h"

// the exposed ports, flow as JACK sees it: an input feeds the graph through
// the "in" node's output of the same name, an output takes the sum of the "out"
// node's input
static const struct {
    const char *name;
    unsigned int type;
    unsigned int flow;
} exposed_defs[] = {
    {"master_in_L", PORT_TYPE_AUDIO, PORT_FLOW_INPUT},
    {"master_in_R", PORT_TYPE_AUDIO, PORT_FLOW_INPUT},
    {"midi_in", PORT_TYPE_MIDI, PORT_FLOW_INPUT},
    {"master_out_L", PORT_TYPE_AUDIO, PORT_FLOW_OUTPUT},
    {"master_out_R", PORT_TYPE_AUDIO, PORT_FLOW_OUTPUT},
    {"midi_out", PORT_TYPE_MIDI, PORT_FLOW_OUTPUT},
    {AUDIO_BACKEND_MIDI_CLOCK_OUT, PORT_TYPE_MIDI, PORT_FLOW_OUTPUT},
};
#define EXPOSED_COUNT (sizeof(exposed_defs) / sizeof(exposed_defs[0]))
// the tick resolution the app leads with
#define JACK_TICKS_PER_BEAT 1920
// rolling reads in a row, and at least a tenth of a second, a BBT may stay
// the same while the frame moves before it counts as frozen
#define MUSIC_FROZEN_READS 3

// one cached port. conn_first/conn_count index the flat adjacency array
typedef struct _jack_port_rec {
    char *name;
    const char *client; // into the interned client list
    uint64_t key;
    unsigned int type;
    unsigned long flow;
    uint64_t owner_tag;
    uint64_t owner_uid;
    size_t conn_first;
    size_t conn_count;
} JACK_PORT_REC;

// one port this client registered, and the caller's two-number name for what
// owns it
typedef struct _port_owner {
    char *name; // full "client:port", owned
    uint64_t tag;
    uint64_t uid;
} PORT_OWNER;

// jack main struct
typedef struct _audio_backend {
    int port_size;
    // the jack client
    jack_client_t *client;
    // by exposed_defs index: the JACK port and its graph side
    jack_port_t *exposed_ports[EXPOSED_COUNT];
    GRAPH_PORT *exposed_graph_ports[EXPOSED_COUNT];
    GRAPH *graph;
    GRAPH_NODE *in_node;
    GRAPH_NODE *out_node;
    // the app's process, run by jack_process_rt
    int (*process)(NFRAMES_T nframes, void *arg);
    void *process_arg;
    // the server's current values, set by the change callbacks on JACK's
    // notification thread
    atomic_uint sample_rate;
    atomic_uint buffer_size;
    // set (true) by app_jack_port_registration_cb, on JACK's own notification
    // thread, whenever a port is registered/unregistered anywhere on the
    // system (not just by this client) - read-and-cleared by
    // audio_backend_ports_sync on [main-thread].
    atomic_bool ports_changed;
    // same as ports_changed, but set by app_jack_port_connect_cb whenever any
    // two ports are connected/disconnected (including by another program) -
    // read-and-cleared by audio_backend_ports_sync too.
    atomic_bool connections_changed;
    // ownership of the ports this client registered, filled at registration.
    // Foreign ports are simply absent
    PORT_OWNER *port_owners;
    size_t port_owner_count;
    size_t port_owner_max;
    // snapshot of every audio/midi port on the system, rebuilt by
    // audio_backend_ports_sync. Own ports first, then the others by (flow,
    // type), so each BackendPortList is a contiguous run described by
    // list_first/list_count
    JACK_PORT_REC *ports;
    size_t port_count;
    size_t list_first[BACKEND_PORT_LIST_COUNT];
    size_t list_count[BACKEND_PORT_LIST_COUNT];
    // client names, interned once so every port of a client shares one string
    char **clients;
    size_t client_count;
    size_t client_max;
    // adjacency for ports, sliced per record by conn_first/conn_count
    size_t *conns;
    size_t conn_total;
    // every port name seen this session, keyed for the whole session so a key
    // keeps meaning the same port across rebuilds. The slot is its record index
    INTERN_TABLE idents;
    // what the timebase callback asks on [audio-thread], set on
    // [main-thread]
    _Atomic(BackendTransportLeadFn) lead_fn;
    _Atomic(void *) lead_arg;
    // [main-thread] -> [audio-thread]: bit 0 leading, the rest bumped by
    // every audio_backend_transport_lead, so a new lead is judged afresh
    atomic_uint lead_state;
    // timebase callbacks so far. JACK calls its timebase master after every
    // rolling cycle, so a rolling cycle without a call means another program
    // took the lead
    atomic_uint lead_calls;
    // [audio-thread] -> [main-thread]: the lead_state another program took
    // the lead in
    atomic_uint lead_lost_state;
    // [audio-thread], jack_process_rt's: the server's transport as this cycle's
    // process sees it, the server's cycle clock and the system time it
    // started at, and whether the transport jumped since
    // audio_backend_transport_read_rt last looked
    jack_transport_state_t cycle_state;
    jack_position_t cycle_pos;
    jack_nframes_t cycle_clock;
    jack_time_t cycle_usecs;
    bool cycle_timed;
    bool cycle_seen;
    bool located;
    // [audio-thread]: whether the BBT is another program's live music. A
    // leader that moves to another driver leaves its record behind, frozen
    // - so is the one we find after we move. The last valid BBT read and how
    // long it stayed the same while rolling
    bool music_live;
    bool music_seen;
    double music_beat;
    jack_nframes_t music_frame;
    unsigned int music_frozen_reads;
    uint64_t music_frozen_frames;
    // [audio-thread], audio_backend_transport_read_rt's (a driver change
    // clears lead_call_due)
    unsigned int lead_state_seen;
    unsigned int lead_calls_seen;
    bool lead_call_due;
    bool lead_lost;
} JACK_INFO;

static void port_owner_add(JACK_INFO *jack_data, const char *port_name,
                           uint64_t tag, uint64_t uid) {
    if (!port_name || tag == 0)
        return;
    if (jack_data->port_owner_count == jack_data->port_owner_max) {
        size_t new_max =
            jack_data->port_owner_max ? jack_data->port_owner_max * 2 : 16;
        PORT_OWNER *grown =
            realloc(jack_data->port_owners, sizeof(PORT_OWNER) * new_max);
        if (!grown)
            return;
        jack_data->port_owners = grown;
        jack_data->port_owner_max = new_max;
    }
    char *name_copy = strdup(port_name);
    if (!name_copy)
        return;
    jack_data->port_owners[jack_data->port_owner_count++] =
        (PORT_OWNER){.name = name_copy, .tag = tag, .uid = uid};
}

static void port_owners_clean(JACK_INFO *jack_data) {
    for (size_t i = 0; i < jack_data->port_owner_count; i++)
        free(jack_data->port_owners[i].name);
    free(jack_data->port_owners);
    jack_data->port_owners = NULL;
    jack_data->port_owner_count = 0;
    jack_data->port_owner_max = 0;
}

// tag recorded for this port name, 0 when this client did not register it
static uint64_t port_owner_find(JACK_INFO *jack_data, const char *port_name,
                                uint64_t *out_uid) {
    if (!port_name)
        return 0;
    for (size_t i = 0; i < jack_data->port_owner_count; i++) {
        if (strcmp(jack_data->port_owners[i].name, port_name) != 0)
            continue;
        if (out_uid)
            *out_uid = jack_data->port_owners[i].uid;
        return jack_data->port_owners[i].tag;
    }
    return 0;
}

// one copy of each client name, shared by all of its ports
static const char *client_intern(JACK_INFO *jack_data, const char *name,
                                 size_t len) {
    for (size_t i = 0; i < jack_data->client_count; i++) {
        if (strncmp(jack_data->clients[i], name, len) == 0 &&
            jack_data->clients[i][len] == '\0')
            return jack_data->clients[i];
    }
    if (jack_data->client_count == jack_data->client_max) {
        size_t new_max = jack_data->client_max ? jack_data->client_max * 2 : 8;
        char **grown = realloc(jack_data->clients, sizeof(char *) * new_max);
        if (!grown)
            return NULL;
        jack_data->clients = grown;
        jack_data->client_max = new_max;
    }
    char *copy = strndup(name, len);
    if (!copy)
        return NULL;
    jack_data->clients[jack_data->client_count++] = copy;
    return copy;
}

static void ports_cache_clear(JACK_INFO *jack_data) {
    for (size_t i = 0; i < jack_data->port_count; i++)
        free(jack_data->ports[i].name);
    free(jack_data->ports);
    jack_data->ports = NULL;
    jack_data->port_count = 0;
    memset(jack_data->list_first, 0, sizeof(jack_data->list_first));
    memset(jack_data->list_count, 0, sizeof(jack_data->list_count));
    for (size_t i = 0; i < jack_data->client_count; i++)
        free(jack_data->clients[i]);
    free(jack_data->clients);
    jack_data->clients = NULL;
    jack_data->client_count = 0;
    jack_data->client_max = 0;
    free(jack_data->conns);
    jack_data->conns = NULL;
    jack_data->conn_total = 0;
    intern_detach_all(&jack_data->idents);
}

static void port_rec_add(JACK_INFO *jack_data, const char *name,
                         unsigned int type, unsigned long flow,
                         size_t capacity) {
    if (jack_data->port_count >= capacity)
        return;
    const char *sep = strchr(name, ':');
    const char *client =
        client_intern(jack_data, name, sep ? (size_t)(sep - name) : strlen(name));
    if (!client)
        return;
    uint64_t key = intern_add(&jack_data->idents, name);
    if (!key)
        return;
    char *name_copy = strdup(name);
    if (!name_copy)
        return;
    intern_set_slot(&jack_data->idents, key, jack_data->port_count);
    JACK_PORT_REC *rec = &jack_data->ports[jack_data->port_count++];
    rec->name = name_copy;
    rec->client = client;
    rec->key = key;
    rec->type = type;
    rec->flow = flow;
    rec->owner_uid = 0;
    rec->owner_tag = port_owner_find(jack_data, name, &rec->owner_uid);
    rec->conn_first = 0;
    rec->conn_count = 0;
}

// what each BackendPortList after EXPOSED is queried with, in list order
static const struct {
    unsigned int type;
    unsigned long flow;
} port_list_query[BACKEND_PORT_LIST_COUNT - 1] = {
    {PORT_TYPE_AUDIO, JackPortIsOutput},
    {PORT_TYPE_MIDI, JackPortIsOutput},
    {PORT_TYPE_AUDIO, JackPortIsInput},
    {PORT_TYPE_MIDI, JackPortIsInput},
};

static const char **app_jack_port_names(JACK_INFO *jack_data,
                                 const char *port_name_pattern,
                                 unsigned int type_pattern,
                                 unsigned long flags) {

    const char *type = NULL;
    switch (type_pattern) {
    case PORT_TYPE_AUDIO:
        type = JACK_DEFAULT_AUDIO_TYPE;
        break;
    case PORT_TYPE_MIDI:
        type = JACK_DEFAULT_MIDI_TYPE;
        break;
    case PORT_TYPE_UNKNOWN:
        // NULL is jack_get_ports()'s own "match any type" wildcard -
        // PORT_TYPE_UNKNOWN is otherwise unused by this function, so it doubles
        // as "give me everything"
        type = NULL;
        break;
    default:
        type = JACK_DEFAULT_AUDIO_TYPE;
    }

    return jack_get_ports(jack_data->client, port_name_pattern, type, flags);
}

// re-read every audio/midi port from jack. Ports of any other type are left
// out - nothing in the app can connect to one
static void ports_rebuild(JACK_INFO *jack_data) {
    ports_cache_clear(jack_data);

    const char **queried[BACKEND_PORT_LIST_COUNT - 1] = {0};
    size_t total = 0;
    for (size_t q = 0; q < BACKEND_PORT_LIST_COUNT - 1; q++) {
        queried[q] = app_jack_port_names(jack_data, NULL, port_list_query[q].type,
                                         port_list_query[q].flow);
        if (!queried[q])
            continue;
        for (size_t i = 0; queried[q][i]; i++)
            total++;
    }
    if (total > 0) {
        jack_data->ports = calloc(total, sizeof(JACK_PORT_REC));
        if (!jack_data->ports)
            total = 0;
    }
    // own ports from every query first, then each query's other ones
    for (size_t l = 0; l < BACKEND_PORT_LIST_COUNT; l++) {
        bool own = l == BACKEND_PORT_LIST_EXPOSED;
        jack_data->list_first[l] = jack_data->port_count;
        for (size_t q = 0; q < BACKEND_PORT_LIST_COUNT - 1; q++) {
            if ((!own && l != q + 1) || !queried[q])
                continue;
            for (size_t i = 0; queried[q][i]; i++) {
                if ((port_owner_find(jack_data, queried[q][i], NULL) != 0) !=
                    own)
                    continue;
                port_rec_add(jack_data, queried[q][i], port_list_query[q].type,
                             port_list_query[q].flow, total);
            }
        }
        jack_data->list_count[l] =
            jack_data->port_count - jack_data->list_first[l];
    }
    for (size_t q = 0; q < BACKEND_PORT_LIST_COUNT - 1; q++)
        free(queried[q]);
}

// index of the cached port with this key, port_count when there is none
static size_t port_index_by_key(JACK_INFO *jack_data, uint64_t key) {
    size_t slot = intern_slot(&jack_data->idents, key);
    return slot < jack_data->port_count ? slot : jack_data->port_count;
}

// lay an edge list (pairs of record indices) out as the per-record adjacency
static void conns_pack(JACK_INFO *jack_data, const size_t *edges,
                       size_t edge_count) {
    free(jack_data->conns);
    jack_data->conns = NULL;
    jack_data->conn_total = 0;
    for (size_t i = 0; i < jack_data->port_count; i++) {
        jack_data->ports[i].conn_first = 0;
        jack_data->ports[i].conn_count = 0;
    }
    if (edge_count == 0)
        return;
    for (size_t e = 0; e < edge_count * 2; e++)
        jack_data->ports[edges[e]].conn_count++;
    size_t total = 0;
    for (size_t i = 0; i < jack_data->port_count; i++) {
        jack_data->ports[i].conn_first = total;
        total += jack_data->ports[i].conn_count;
        jack_data->ports[i].conn_count = 0;
    }
    jack_data->conns = calloc(total, sizeof(size_t));
    if (!jack_data->conns)
        return;
    jack_data->conn_total = total;
    for (size_t e = 0; e < edge_count; e++) {
        size_t a = edges[e * 2];
        size_t b = edges[e * 2 + 1];
        jack_data->conns[jack_data->ports[a].conn_first +
                         jack_data->ports[a].conn_count++] = b;
        jack_data->conns[jack_data->ports[b].conn_first +
                         jack_data->ports[b].conn_count++] = a;
    }
}

// re-read the graph. Only outputs are asked, and each edge is filed on both
// ends, so the two sides can never disagree
static void connections_rebuild(JACK_INFO *jack_data) {
    conns_pack(jack_data, NULL, 0);
    if (!jack_data->client || jack_data->port_count == 0)
        return;

    size_t *edges = NULL;
    size_t edge_count = 0;
    size_t edge_max = 0;
    for (size_t i = 0; i < jack_data->port_count; i++) {
        if (jack_data->ports[i].flow != JackPortIsOutput)
            continue;
        jack_port_t *port =
            jack_port_by_name(jack_data->client, jack_data->ports[i].name);
        if (!port)
            continue;
        const char **peers = jack_port_get_connections(port);
        if (!peers)
            continue;
        for (size_t c = 0; peers[c]; c++) {
            size_t j = port_index_by_key(
                jack_data, intern_find(&jack_data->idents, peers[c]));
            if (j >= jack_data->port_count)
                continue;
            if (edge_count == edge_max) {
                size_t new_max = edge_max ? edge_max * 2 : 32;
                size_t *grown = realloc(edges, sizeof(size_t) * 2 * new_max);
                if (!grown) {
                    free(peers);
                    goto done;
                }
                edges = grown;
                edge_max = new_max;
            }
            edges[edge_count * 2] = i;
            edges[edge_count * 2 + 1] = j;
            edge_count++;
        }
        free(peers);
    }
    conns_pack(jack_data, edges, edge_count);
done:
    free(edges);
}

// re-pack the adjacency with one edge added or dropped, from what the cache
// already holds. Not re-read from jack: a connect just issued can still
// report its old state there for a cycle
static void edge_set_toggle(JACK_INFO *jack_data, size_t a, size_t b,
                            bool connect) {
    size_t cap = jack_data->conn_total / 2 + 1;
    size_t *edges = malloc(sizeof(size_t) * 2 * cap);
    if (!edges)
        return;
    size_t count = 0;
    // each edge is filed on both ends, so the output side sees each one once
    for (size_t i = 0; i < jack_data->port_count; i++) {
        if (jack_data->ports[i].flow != JackPortIsOutput)
            continue;
        for (size_t c = 0; c < jack_data->ports[i].conn_count; c++) {
            size_t peer = jack_data->conns[jack_data->ports[i].conn_first + c];
            if ((i == a && peer == b) || (i == b && peer == a))
                continue;
            edges[count * 2] = i;
            edges[count * 2 + 1] = peer;
            count++;
        }
    }
    if (connect && count < cap) {
        edges[count * 2] = a;
        edges[count * 2 + 1] = b;
        count++;
    }
    conns_pack(jack_data, edges, count);
    free(edges);
}

BackendPortSync audio_backend_ports_sync(JACK_INFO *jack_data) {
    BackendPortSync done = {false, false};
    if (!jack_data)
        return done;
    done.ports = atomic_exchange(&jack_data->ports_changed, false);
    done.connections = atomic_exchange(&jack_data->connections_changed, false);
    if (done.ports) {
        ports_rebuild(jack_data);
        // record indices moved, so the adjacency has to be rebuilt with them
        done.connections = true;
    }
    if (done.connections)
        connections_rebuild(jack_data);
    return done;
}

static void port_info_fill(const JACK_PORT_REC *rec, BackendPortInfo *out) {
    out->name = rec->name;
    out->client = rec->client;
    out->key = rec->key;
    out->type = rec->type;
    out->flow =
        rec->flow == JackPortIsOutput ? PORT_FLOW_OUTPUT : PORT_FLOW_INPUT;
    out->owner_tag = rec->owner_tag;
    out->owner_uid = rec->owner_uid;
}

size_t audio_backend_port_count(JACK_INFO *jack_data, BackendPortList list) {
    if (!jack_data || list >= BACKEND_PORT_LIST_COUNT)
        return 0;
    return jack_data->list_count[list];
}

bool audio_backend_port_at(JACK_INFO *jack_data, BackendPortList list,
                           size_t idx, BackendPortInfo *out) {
    if (!jack_data || !out || list >= BACKEND_PORT_LIST_COUNT)
        return false;
    if (idx >= jack_data->list_count[list])
        return false;
    port_info_fill(&jack_data->ports[jack_data->list_first[list] + idx], out);
    return true;
}

bool audio_backend_port_by_key(JACK_INFO *jack_data, uint64_t key,
                               BackendPortInfo *out) {
    if (!jack_data || !out)
        return false;
    size_t i = port_index_by_key(jack_data, key);
    if (i >= jack_data->port_count)
        return false;
    port_info_fill(&jack_data->ports[i], out);
    return true;
}

size_t audio_backend_port_connection_count(JACK_INFO *jack_data, uint64_t key) {
    if (!jack_data)
        return 0;
    size_t i = port_index_by_key(jack_data, key);
    if (i >= jack_data->port_count)
        return 0;
    return jack_data->ports[i].conn_count;
}

bool audio_backend_port_connection_at(JACK_INFO *jack_data, uint64_t key,
                                      size_t idx, BackendPortInfo *out) {
    if (!jack_data || !out)
        return false;
    size_t i = port_index_by_key(jack_data, key);
    if (i >= jack_data->port_count)
        return false;
    if (idx >= jack_data->ports[i].conn_count)
        return false;
    size_t peer = jack_data->conns[jack_data->ports[i].conn_first + idx];
    if (peer >= jack_data->port_count)
        return false;
    port_info_fill(&jack_data->ports[peer], out);
    return true;
}

bool audio_backend_port_keys_connected(JACK_INFO *jack_data, uint64_t key_a,
                                       uint64_t key_b) {
    if (!jack_data)
        return false;
    size_t a = port_index_by_key(jack_data, key_a);
    size_t b = port_index_by_key(jack_data, key_b);
    if (a >= jack_data->port_count || b >= jack_data->port_count)
        return false;
    for (size_t c = 0; c < jack_data->ports[a].conn_count; c++) {
        if (jack_data->conns[jack_data->ports[a].conn_first + c] == b)
            return true;
    }
    return false;
}

static int sample_rate_change(jack_nframes_t new_sample_rate, void *arg);
static int buffer_size_change(jack_nframes_t new_buffer_size, void *arg);
static int jack_process_rt(jack_nframes_t nframes, void *arg);

// Starting does not move the frame yet
static bool transport_state_rolling(jack_transport_state_t state) {
    return state == JackTransportRolling || state == JackTransportLooping;
}

// whether someone located the transport between two process calls, elapsed
// server frames apart: it moved back, or on by more than the elapsed frames.
// The server's own steps stay inside - a stop goes back to the last rolling
// cycle's start, a start may count its Starting cycle again or hold the frame
// a few rolling cycles (pipewire-jack). Without same_clock the elapsed frames
// mean nothing, any move is a jump
static bool transport_located(jack_nframes_t from, jack_nframes_t to,
                              jack_nframes_t elapsed, bool same_clock) {
    // unsigned: a frame or clock wrapping around stays exact, a step back is
    // huge
    jack_nframes_t moved = to - from;
    if (!same_clock)
        return moved != 0;
    return moved > elapsed;
}

// [audio-thread] this cycle's server transport, and whether it jumped since
// the last cycle we saw
static void transport_cycle_take_rt(JACK_INFO *jack_data,
                                    jack_nframes_t nframes) {
    jack_position_t pos;
    jack_transport_state_t state =
        jack_transport_query(jack_data->client, &pos);
    // the cycle clock counts the cycles the server skipped calling us too
    jack_nframes_t clock = 0;
    jack_time_t usecs = 0;
    jack_time_t next_usecs = 0;
    float period = 0;
    bool timed = jack_get_cycle_times(jack_data->client, &clock, &usecs,
                                      &next_usecs, &period) == 0;
    if (!timed)
        clock = jack_last_frame_time(jack_data->client);
    if (jack_data->cycle_seen) {
        jack_nframes_t elapsed = clock - jack_data->cycle_clock;
        // pipewire-jack moves clients between drivers, each with its own
        // clock and transport: the clock then disagrees with the system time.
        // The slack keeps a sample rate change the same clock
        bool same_clock = true;
        unsigned int rate = atomic_load(&jack_data->sample_rate);
        if (timed && jack_data->cycle_timed && rate > 0) {
            double by_time =
                (double)(usecs - jack_data->cycle_usecs) * rate * 1e-6;
            same_clock = fabs(by_time - (double)elapsed) <=
                         (double)elapsed / 2 + (double)nframes;
        }
        if (transport_located(jack_data->cycle_pos.frame, pos.frame, elapsed,
                              same_clock))
            jack_data->located = true;
        // another driver: its BBT has to show it is live. Our lead moves with
        // us, but the move may cost a timebase call - no lost lead
        if (!same_clock) {
            jack_data->music_live = false;
            jack_data->music_seen = false;
            jack_data->lead_call_due = false;
        }
    }
    jack_data->cycle_state = state;
    jack_data->cycle_pos = pos;
    jack_data->cycle_clock = clock;
    jack_data->cycle_usecs = usecs;
    jack_data->cycle_timed = timed;
    jack_data->cycle_seen = true;
}

// JACK notification-thread callbacks (see jack_set_port_registration_callback /
// jack_set_port_connect_callback below). JACK's own docs say these run on a
// separate non-RT thread, not [audio-thread] and not [main-thread] - so these
// stay deliberately trivial: just flip an atomic flag, touch nothing else.
// does not return what changed, just that ports changed
static void app_jack_port_registration_cb(jack_port_id_t port, int registered,
                                          void *arg) {
    (void)port;
    (void)registered;
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    if (!jack_data)
        return;
    atomic_store(&jack_data->ports_changed, true);
}
static void app_jack_port_rename_cb(jack_port_id_t port, const char *old_name,
                                    const char *new_name, void *arg) {
    (void)port;
    (void)old_name;
    (void)new_name;
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    if (!jack_data)
        return;
    atomic_store(&jack_data->ports_changed, true);
}
static void app_jack_port_connect_cb(jack_port_id_t a, jack_port_id_t b,
                                     int connect, void *arg) {
    (void)a;
    (void)b;
    (void)connect;
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    if (!jack_data)
        return;
    atomic_store(&jack_data->connections_changed, true);
}

AUDIO_BACKEND *audio_backend_init(void *arg, const char *client_name,
                                  int (*process)(NFRAMES_T, void *)) {
    // every pointer NULL, so a failure below can clean at any point
    JACK_INFO *jack_data = (JACK_INFO *)calloc(1, sizeof(JACK_INFO));
    if (!jack_data) {
        return NULL;
    }
    // start dirty so the first sync builds the cache
    atomic_init(&jack_data->ports_changed, true);
    atomic_init(&jack_data->connections_changed, true);
    atomic_init(&jack_data->lead_fn, NULL);
    atomic_init(&jack_data->lead_arg, NULL);
    atomic_init(&jack_data->lead_state, 0);
    atomic_init(&jack_data->lead_calls, 0);
    atomic_init(&jack_data->lead_lost_state, 0);
    // a program that exits takes its record with it
    jack_data->music_live = true;

    jack_status_t *server_name = NULL;
    jack_options_t options = JackNullOption;
    jack_status_t status = 0;

    // open client connection
    jack_data->client =
        jack_client_open(client_name, options & status, server_name);
    if (jack_data->client == NULL) {
        audio_backend_clean(jack_data);
        return NULL;
    }

    jack_data->process = process;
    jack_data->process_arg = arg;
    jack_set_process_callback(jack_data->client, jack_process_rt, jack_data);

    // the values the app follows, read on [main-thread]
    jack_set_sample_rate_callback(jack_data->client, sample_rate_change,
                                  jack_data);
    jack_set_buffer_size_callback(jack_data->client, buffer_size_change,
                                  jack_data);

    // call this function whenever any port is registered or unregistered
    // anywhere on the system (not just this client's own ports) - must be
    // set before jack_activate(), same as the callbacks above
    jack_set_port_registration_callback(
        jack_data->client, app_jack_port_registration_cb, jack_data);
    // call this function whenever any two ports are connected or disconnected,
    // including by another program (e.g. a patchbay) - same pre-activate
    // requirement
    jack_set_port_connect_callback(jack_data->client, app_jack_port_connect_cb,
                                   jack_data);
    // a rename is its own notification, not a register/unregister pair
    jack_set_port_rename_callback(jack_data->client, app_jack_port_rename_cb,
                                  jack_data);

    // the callbacks keep them current from activate on
    atomic_store(&jack_data->sample_rate,
                 jack_get_sample_rate(jack_data->client));
    atomic_store(&jack_data->buffer_size,
                 jack_get_buffer_size(jack_data->client));

    return jack_data;
}

// port_type PORT_TYPE_AUDIO or PORT_TYPE_MIDI, flow PORT_FLOW_*
static jack_port_t *jack_port_create(JACK_INFO *jack_data,
                                     unsigned int port_type, unsigned int flow,
                                     const char *port_name, uint64_t owner_tag,
                                     uint64_t owner_uid) {
    if (!jack_data->client)
        return NULL;
    const char *type = port_type == PORT_TYPE_MIDI ? JACK_DEFAULT_MIDI_TYPE
                                                   : JACK_DEFAULT_AUDIO_TYPE;
    unsigned long flags =
        flow == PORT_FLOW_INPUT ? JackPortIsInput : JackPortIsOutput;
    jack_port_t *port =
        jack_port_register(jack_data->client, port_name, type, flags, 0);
    if (!port)
        return NULL;
    // jack prefixes the client name, so record what the port list will see
    port_owner_add(jack_data, jack_port_name(port), owner_tag, owner_uid);
    atomic_store(&jack_data->ports_changed, true);
    return port;
}

SAMPLE_T audio_backend_sample_rate(AUDIO_BACKEND *jack_data) {
    if (!jack_data || !jack_data->client)
        return -1;
    return (SAMPLE_T)atomic_load(&jack_data->sample_rate);
}

uint32_t audio_backend_buffer_size(AUDIO_BACKEND *jack_data) {
    if (!jack_data || !jack_data->client)
        return 0;
    return atomic_load(&jack_data->buffer_size);
}

int audio_backend_activate(AUDIO_BACKEND *jack_data) {
    // activate the client and launch the process function
    if (!jack_data || jack_activate(jack_data->client)) {
        return -1;
    }
    return 0;
}

static void *jack_buffer_rt(jack_port_t *port, NFRAMES_T nframes) {
    if (!port)
        return NULL;
    return jack_port_get_buffer(port, nframes);
}

// clear buf for nframes and fill it with the events of the midi in port
static void jack_midi_in_rt(jack_port_t *port, NFRAMES_T nframes,
                            MIDI_BUF *buf) {
    midi_buf_clear(buf, nframes);
    void *jack_buf = jack_buffer_rt(port, nframes);
    if (!jack_buf)
        return;
    uint32_t count = jack_midi_get_event_count(jack_buf);
    for (uint32_t i = 0; i < count; i++) {
        jack_midi_event_t ev;
        if (jack_midi_event_get(&ev, jack_buf, i) != 0)
            continue;
        midi_buf_push(buf, ev.time, ev.buffer, (uint32_t)ev.size);
    }
}

// replace the events of the midi out port with the events of buf
static void jack_midi_out_rt(jack_port_t *port, NFRAMES_T nframes,
                             const MIDI_BUF *buf) {
    void *jack_buf = jack_buffer_rt(port, nframes);
    if (!jack_buf)
        return;
    jack_midi_clear_buffer(jack_buf);
    MIDI_BUF_ITER it = midi_buf_iter(buf);
    MIDI_EVENT ev;
    // events a full jack buffer refuses are counted by jack itself
    // (jack_midi_get_lost_event_count)
    while (midi_buf_next(&it, &ev))
        jack_midi_event_write(jack_buf, ev.frame, ev.data, ev.size);
}

// [audio-thread] the "in" node: the exposed inputs into its outputs
static bool endpoint_in_process_rt(void *arg, NFRAMES_T nframes) {
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    for (size_t i = 0; i < EXPOSED_COUNT; i++) {
        if (exposed_defs[i].flow != PORT_FLOW_INPUT)
            continue;
        jack_port_t *port = jack_data->exposed_ports[i];
        GRAPH_PORT *graph_port = jack_data->exposed_graph_ports[i];
        if (exposed_defs[i].type == PORT_TYPE_MIDI) {
            jack_midi_in_rt(port, nframes, graph_port_midi_rt(graph_port));
            continue;
        }
        SAMPLE_T *out = graph_port_audio_rt(graph_port);
        SAMPLE_T *in = jack_buffer_rt(port, nframes);
        if (in)
            memcpy(out, in, sizeof(SAMPLE_T) * nframes);
        else
            memset(out, 0, sizeof(SAMPLE_T) * nframes);
    }
    return true;
}

// [audio-thread] the "out" node: its inputs' sums into the exposed outputs
static bool endpoint_out_process_rt(void *arg, NFRAMES_T nframes) {
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    for (size_t i = 0; i < EXPOSED_COUNT; i++) {
        if (exposed_defs[i].flow != PORT_FLOW_OUTPUT)
            continue;
        jack_port_t *port = jack_data->exposed_ports[i];
        GRAPH_PORT *graph_port = jack_data->exposed_graph_ports[i];
        if (exposed_defs[i].type == PORT_TYPE_MIDI) {
            jack_midi_out_rt(port, nframes, graph_port_midi_rt(graph_port));
            continue;
        }
        SAMPLE_T *out = jack_buffer_rt(port, nframes);
        if (out)
            memcpy(out, graph_port_audio_rt(graph_port),
                   sizeof(SAMPLE_T) * nframes);
    }
    return true;
}

// [audio-thread] the exposed outputs start empty - the "out" node only fills
// them when the graph runs (not while stopped, or nframes bigger than its
// buffers)
static int jack_process_rt(jack_nframes_t nframes, void *arg) {
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    transport_cycle_take_rt(jack_data, nframes);
    for (size_t i = 0; i < EXPOSED_COUNT; i++) {
        if (exposed_defs[i].flow != PORT_FLOW_OUTPUT)
            continue;
        void *out = jack_buffer_rt(jack_data->exposed_ports[i], nframes);
        if (!out)
            continue;
        if (exposed_defs[i].type == PORT_TYPE_MIDI)
            jack_midi_clear_buffer(out);
        else
            memset(out, 0, sizeof(SAMPLE_T) * nframes);
    }
    return jack_data->process(nframes, jack_data->process_arg);
}

int audio_backend_endpoints_add(AUDIO_BACKEND *jack_data, GRAPH *graph,
                                uint64_t owner_tag, uint64_t owner_uid) {
    if (!jack_data || !graph || jack_data->graph)
        return -1;
    jack_data->graph = graph;
    jack_data->in_node =
        graph_node_add(graph, owner_tag, owner_uid, GRAPH_NODE_CAPTURE,
                       endpoint_in_process_rt, jack_data);
    jack_data->out_node =
        graph_node_add(graph, owner_tag, owner_uid, GRAPH_NODE_PLAYBACK,
                       endpoint_out_process_rt, jack_data);
    if (!jack_data->in_node || !jack_data->out_node)
        return -1;
    for (size_t i = 0; i < EXPOSED_COUNT; i++) {
        bool to_graph = exposed_defs[i].flow == PORT_FLOW_INPUT;
        jack_data->exposed_ports[i] = jack_port_create(
            jack_data, exposed_defs[i].type, exposed_defs[i].flow,
            exposed_defs[i].name, owner_tag, owner_uid);
        jack_data->exposed_graph_ports[i] = graph_port_create(
            graph, to_graph ? jack_data->in_node : jack_data->out_node,
            exposed_defs[i].type, to_graph ? PORT_FLOW_OUTPUT : PORT_FLOW_INPUT,
            exposed_defs[i].name);
        if (!jack_data->exposed_ports[i] || !jack_data->exposed_graph_ports[i])
            return -1;
    }
    return 0;
}

// jack_connect wants the output first, so the pair is ordered here. Two ports
// of the same flow cannot be linked at all
static int port_keys_link(JACK_INFO *jack_data, uint64_t key_a, uint64_t key_b,
                          bool connect) {
    if (!jack_data || !jack_data->client)
        return -1;
    size_t a = port_index_by_key(jack_data, key_a);
    size_t b = port_index_by_key(jack_data, key_b);
    if (a >= jack_data->port_count || b >= jack_data->port_count)
        return -1;
    if (jack_data->ports[a].flow == jack_data->ports[b].flow)
        return -1;
    if (connect && jack_data->ports[a].owner_tag != 0 &&
        jack_data->ports[b].owner_tag != 0)
        return -1;
    size_t out = jack_data->ports[a].flow == JackPortIsOutput ? a : b;
    size_t in = (out == a) ? b : a;
    int result =
        connect ? jack_connect(jack_data->client, jack_data->ports[out].name,
                               jack_data->ports[in].name)
                : jack_disconnect(jack_data->client, jack_data->ports[out].name,
                                  jack_data->ports[in].name);
    if (result != 0)
        return result;
    edge_set_toggle(jack_data, out, in, connect);
    return 0;
}

int audio_backend_connect_keys(JACK_INFO *jack_data, uint64_t key_a,
                               uint64_t key_b) {
    return port_keys_link(jack_data, key_a, key_b, true);
}

int audio_backend_disconnect_keys(JACK_INFO *jack_data, uint64_t key_a,
                                  uint64_t key_b) {
    return port_keys_link(jack_data, key_a, key_b, false);
}

// [audio-thread] whether this read's BBT (valid, at beat) is live music: it
// changes while the frame rolls on. One that stays the same a few reads (the
// server can show a BBT a cycle late) over a tenth of a second (a slow tempo
// moves less than a tick per small cycle) is a frozen record until it moves
static bool music_live_rt(JACK_INFO *jack_data, bool valid, bool rolling,
                          jack_nframes_t frame, double beat) {
    if (!valid) {
        jack_data->music_seen = false;
        return false;
    }
    if (rolling && jack_data->music_seen && frame != jack_data->music_frame) {
        if (beat != jack_data->music_beat) {
            jack_data->music_live = true;
            jack_data->music_frozen_reads = 0;
            jack_data->music_frozen_frames = 0;
        } else {
            jack_data->music_frozen_reads++;
            jack_data->music_frozen_frames +=
                (jack_nframes_t)(frame - jack_data->music_frame);
            if (jack_data->music_frozen_reads >= MUSIC_FROZEN_READS &&
                jack_data->music_frozen_frames * 10 >=
                    atomic_load(&jack_data->sample_rate))
                jack_data->music_live = false;
        }
    }
    jack_data->music_seen = true;
    jack_data->music_beat = beat;
    jack_data->music_frame = frame;
    return jack_data->music_live;
}

// the BBT's bar start in beats. JACK does not document bar_start_tick; taken as
// the ticks from the song start, bar 1 at 0 - so each bar before is 1..1024
// beats (position_music_valid). Outside that, the bars before count at the
// current meter. Bars below 1 are taken as is
static double bbt_bar_start_beat(const jack_position_t *pos) {
    double beat = pos->bar_start_tick / pos->ticks_per_beat;
    if (pos->bar < 1)
        return beat;
    double bars_before = (double)pos->bar - 1;
    // a tick of slack for rounding
    if (beat >= bars_before - 1.0 / pos->ticks_per_beat &&
        beat <= bars_before * 1024)
        return beat;
    return bars_before * round(pos->beats_per_bar);
}

bool audio_backend_transport_read_rt(JACK_INFO *jack_data,
                                     POSITION_SERVER *out) {
    if (!jack_data || !jack_data->client || !out)
        return false;
    const jack_position_t *pos = &jack_data->cycle_pos;
    bool rolling = transport_state_rolling(jack_data->cycle_state);
    unsigned int lead_state = atomic_load(&jack_data->lead_state);
    bool leading = lead_state & 1;
    unsigned int calls =
        atomic_load_explicit(&jack_data->lead_calls, memory_order_relaxed);
    if (!leading || lead_state != jack_data->lead_state_seen ||
        calls != jack_data->lead_calls_seen)
        jack_data->lead_lost = false;
    else if (jack_data->lead_call_due && !jack_data->lead_lost) {
        jack_data->lead_lost = true;
        atomic_store(&jack_data->lead_lost_state, lead_state);
    }
    jack_data->lead_state_seen = lead_state;
    jack_data->lead_calls_seen = calls;
    jack_data->lead_call_due = leading && rolling;

    *out = (POSITION_SERVER){
        .rolling = rolling, .frame = pos->frame, .located = jack_data->located};
    jack_data->located = false;
    // ticks_per_beat set, so the BBT converts
    POSITION_MUSIC music = {0};
    bool valid = (pos->valid & JackPositionBBT) && pos->ticks_per_beat > 0;
    if (valid) {
        double bar_start_beat = bbt_bar_start_beat(pos);
        music = (POSITION_MUSIC){
            .beat = bar_start_beat + (double)(pos->beat - 1) +
                    (double)pos->tick / pos->ticks_per_beat,
            .bar = (int64_t)pos->bar - 1,
            .bar_start_beat = bar_start_beat,
            .bpm = pos->beats_per_minute,
            .bpb = pos->beats_per_bar,
            .beat_type = pos->beat_type,
        };
        valid = position_music_valid(&music);
    }
    // tracked while we lead too, so a takeover finds it live
    bool live =
        music_live_rt(jack_data, valid, rolling, pos->frame, music.beat);
    // our own BBT is no music to follow
    if (!live || (leading && !jack_data->lead_lost))
        return true;
    out->has_music = true;
    out->music = music;
    return true;
}

// not on [audio-thread]: pipewire-jack's start and stop lock its thread loop
int audio_backend_transport_start(JACK_INFO *jack_data) {
    if (!jack_data || !jack_data->client)
        return -1;
    jack_transport_start(jack_data->client);
    return 0;
}

int audio_backend_transport_stop(JACK_INFO *jack_data) {
    if (!jack_data || !jack_data->client)
        return -1;
    jack_transport_stop(jack_data->client);
    return 0;
}

int audio_backend_transport_locate(JACK_INFO *jack_data, int64_t frame) {
    if (!jack_data || !jack_data->client || frame < 0 || frame > UINT32_MAX)
        return -1;
    return jack_transport_locate(jack_data->client, (jack_nframes_t)frame);
}

// [audio-thread] right after process, while this client is the timebase
// master: the BBT of the frame the server shows it with
static void jack_timebase_rt(jack_transport_state_t state,
                             jack_nframes_t nframes, jack_position_t *pos,
                             int new_pos, void *arg) {
    (void)new_pos;
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    atomic_fetch_add_explicit(&jack_data->lead_calls, 1, memory_order_relaxed);
    BackendTransportLeadFn fn = atomic_load(&jack_data->lead_fn);
    if (!fn)
        return;
    // pipewire-jack gives this cycle's frame but shows the BBT with the next
    // cycle's - answered for that one. JACK2 gives the next frame already
    int64_t frame = pos->frame;
    if (state == JackTransportRolling &&
        pos->frame == jack_data->cycle_pos.frame)
        frame += nframes;
    POSITION_MUSIC music = {0};
    fn(atomic_load(&jack_data->lead_arg), frame, &music);
    if (!position_music_valid(&music))
        return;
    uint32_t beat = 0;
    uint32_t tick = 0;
    position_bbt(&music, JACK_TICKS_PER_BEAT, &beat, &tick);
    pos->valid =
        (jack_position_bits_t)((pos->valid | JackPositionBBT) &
                               ~JackBBTFrameOffset);
    pos->bar = (int32_t)(music.bar + 1);
    pos->beat = (int32_t)beat + 1;
    pos->tick = (int32_t)tick;
    pos->bar_start_tick = music.bar_start_beat * JACK_TICKS_PER_BEAT;
    pos->beats_per_bar = (float)music.bpb;
    pos->beat_type = (float)music.beat_type;
    pos->ticks_per_beat = JACK_TICKS_PER_BEAT;
    pos->beats_per_minute = music.bpm;
    pos->bbt_offset = 0;
}

int audio_backend_transport_lead(JACK_INFO *jack_data,
                                 BackendTransportLeadFn fn, void *arg) {
    if (!jack_data || !jack_data->client)
        return -1;
    unsigned int next = (atomic_load(&jack_data->lead_state) | 1) + 1;
    if (!fn) {
        // released first, so the server stops showing our BBT before a cycle
        // can take it for another program's. Fails when another program took
        // the lead - nothing to release then
        if (atomic_load(&jack_data->lead_state) & 1)
            jack_release_timebase(jack_data->client);
        atomic_store(&jack_data->lead_state, next);
        atomic_store(&jack_data->lead_fn, NULL);
        return 0;
    }
    // before the server can call back
    atomic_store(&jack_data->lead_arg, arg);
    atomic_store(&jack_data->lead_fn, fn);
    // unconditional: takes the lead from another program
    if (jack_set_timebase_callback(jack_data->client, 0, jack_timebase_rt,
                                   jack_data) != 0) {
        atomic_store(&jack_data->lead_state, next);
        atomic_store(&jack_data->lead_fn, NULL);
        return -1;
    }
    // the server calls back from the next cycle on
    atomic_store(&jack_data->lead_state, next | 1);
    return 0;
}

bool audio_backend_transport_lead_lost(JACK_INFO *jack_data) {
    if (!jack_data)
        return false;
    // a verdict on an earlier lead never matches
    unsigned int lead_state = atomic_load(&jack_data->lead_state);
    return (lead_state & 1) &&
           atomic_load(&jack_data->lead_lost_state) == lead_state;
}

// JACK's notification thread. [main-thread] follows the value in app_data
static int sample_rate_change(jack_nframes_t new_sample_rate, void *arg) {
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    if (!jack_data)
        return -1;
    atomic_store(&jack_data->sample_rate, new_sample_rate);
    return 0;
}

static int buffer_size_change(jack_nframes_t new_buffer_size, void *arg) {
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    if (!jack_data)
        return -1;
    atomic_store(&jack_data->buffer_size, new_buffer_size);
    return 0;
}

void audio_backend_clean(AUDIO_BACKEND *jack_data) {
    if (!jack_data)
        return;
    // close the jack client, the process callback stops with it
    if (jack_data->client != NULL)
        jack_client_close(jack_data->client);
    // the graph outlives the backend
    graph_node_remove(jack_data->graph, jack_data->in_node);
    graph_node_remove(jack_data->graph, jack_data->out_node);
    ports_cache_clear(jack_data);
    intern_clean(&jack_data->idents);
    port_owners_clean(jack_data);
    free(jack_data);
}
