#include <jack/types.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
// my libraries
#include "../contexts/context_control.h"
#include "../types.h"
#include "../util_funcs/log_funcs.h"
#include "jack_funcs.h"
// the maximum number of bars there can be
#define MAX_BARS 1000

// this module's own id space for the transport params, passed to
// param_add_param as owner_id. Source-defined, so it is stable across runs and
// is what a saved value gets matched back by - never a positional index. 0
// stays reserved for "no owner id".
enum trkParamId {
    TRK_PARAM_NONE = 0,
    TRK_PARAM_TEMPO,
    TRK_PARAM_BAR,
    TRK_PARAM_BEAT,
    TRK_PARAM_TICK,
    TRK_PARAM_PLAY,
    TRK_PARAM_BPB,
    TRK_PARAM_BEAT_TYPE,
    TRK_PARAM_COUNT
};

static thread_local bool is_audio_thread = false;


// a port name and the identity minted for it. Outlives the port list, so a
// key keeps meaning the same port across rebuilds and across a port that
// disappears and comes back
typedef struct _port_ident {
    char *name;
    size_t rec; // index into ports, SIZE_MAX when not currently present
} PORT_IDENT;

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
typedef struct _jack_info {
    int port_size;
    // the jack client
    jack_client_t *client;
    // the current server sample_rate
    jack_nframes_t sample_rate;
    // the current server buffer size
    jack_nframes_t buffer_size;
    // parameters for the general song track settings, like current bar, beat
    // also play, stop etc.
    PRM_CONTAIN *trk_params;
    // val_ids of the transport params, captured from param_add_param at init.
    // The [audio-thread] transport code indexes params constantly and must not
    // look anything up to do it - param_find_owner_id is a linear scan and has
    // no business in the audio callback - so resolve the mapping once, here.
    // indexed by trkParamId, so TRK_PARAM_NONE's slot is unused
    int trk_val[TRK_PARAM_COUNT];
    // rt tick var, that goes from 0 to RT_CYCLES, rt thread will send info to
    // ui thread only when rt tick var is 0 should only be touched on
    // [audio-thread]
    int rt_tick;
    // control_data for sys messages, for jack currently only uses the
    // [thread-safe] messaging system, since this does not have any subcontexts
    CXCONTROL *control_data;
    // set (true) by app_jack_port_registration_cb, on JACK's own notification
    // thread, whenever a port is registered/unregistered anywhere on the
    // system (not just by this client) - read-and-cleared by
    // app_jack_ports_sync on [main-thread].
    atomic_bool ports_changed;
    // same as ports_changed, but set by app_jack_port_connect_cb whenever any
    // two ports are connected/disconnected (including by another program) -
    // read-and-cleared by app_jack_ports_sync too.
    atomic_bool connections_changed;
    // ownership of the ports this client registered, filled at registration.
    // Foreign ports are simply absent
    PORT_OWNER *port_owners;
    size_t port_owner_count;
    size_t port_owner_max;
    // snapshot of every audio/midi port on the system, rebuilt by
    // app_jack_ports_sync. Grouped by (flow, type), so each JackPortList is a
    // contiguous run described by list_first/list_count
    JACK_PORT_REC *ports;
    size_t port_count;
    size_t list_first[JACK_PORT_LIST_COUNT];
    size_t list_count[JACK_PORT_LIST_COUNT];
    // client names, interned once so every port of a client shares one string
    char **clients;
    size_t client_count;
    size_t client_max;
    // adjacency for ports, sliced per record by conn_first/conn_count
    size_t *conns;
    size_t conn_total;
    // every port name seen this session. A record's key is its index here + 1,
    // so 0 is free to mean "no port"
    PORT_IDENT *idents;
    size_t ident_count;
    size_t ident_max;
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

// order carries no meaning here, so the hole is filled from the end
static void port_owner_remove(JACK_INFO *jack_data, const char *port_name) {
    if (!port_name)
        return;
    for (size_t i = 0; i < jack_data->port_owner_count; i++) {
        if (strcmp(jack_data->port_owners[i].name, port_name) != 0)
            continue;
        free(jack_data->port_owners[i].name);
        jack_data->port_owner_count--;
        if (i != jack_data->port_owner_count)
            jack_data->port_owners[i] =
                jack_data->port_owners[jack_data->port_owner_count];
        return;
    }
}

static void port_owner_rename(JACK_INFO *jack_data, const char *old_name,
                              const char *new_name) {
    if (!old_name || !new_name)
        return;
    // a leftover entry under the new name would shadow the renamed one
    port_owner_remove(jack_data, new_name);
    for (size_t i = 0; i < jack_data->port_owner_count; i++) {
        if (strcmp(jack_data->port_owners[i].name, old_name) != 0)
            continue;
        char *name_copy = strdup(new_name);
        if (!name_copy)
            return;
        free(jack_data->port_owners[i].name);
        jack_data->port_owners[i].name = name_copy;
        return;
    }
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

// keys are 1-based so that 0 can mean "no port", so this is the only place
// that turns one back into a slot. NULL when the key names none
static PORT_IDENT *port_ident_at(JACK_INFO *jack_data, uint64_t key) {
    if (key == 0 || key > jack_data->ident_count)
        return NULL;
    return &jack_data->idents[key - 1];
}

// key of an already-seen port name, 0 when it has never been seen
static uint64_t port_ident_find(JACK_INFO *jack_data, const char *port_name) {
    if (!port_name)
        return 0;
    for (size_t i = 0; i < jack_data->ident_count; i++) {
        if (strcmp(jack_data->idents[i].name, port_name) == 0)
            return i + 1;
    }
    return 0;
}

// as port_ident_find, but mints an identity for a name seen for the first time
static uint64_t port_ident_intern(JACK_INFO *jack_data, const char *port_name) {
    uint64_t key = port_ident_find(jack_data, port_name);
    if (key)
        return key;
    if (jack_data->ident_count == jack_data->ident_max) {
        size_t new_max = jack_data->ident_max ? jack_data->ident_max * 2 : 32;
        PORT_IDENT *grown =
            realloc(jack_data->idents, sizeof(PORT_IDENT) * new_max);
        if (!grown)
            return 0;
        jack_data->idents = grown;
        jack_data->ident_max = new_max;
    }
    char *name_copy = strdup(port_name);
    if (!name_copy)
        return 0;
    jack_data->idents[jack_data->ident_count++] =
        (PORT_IDENT){.name = name_copy, .rec = SIZE_MAX};
    return jack_data->ident_count;
}

// jack keeps the port across a rename, so the identity stays and only its
// name changes. A new name already interned wins instead - the port called X
// is the one keyed under X
static void port_ident_rename(JACK_INFO *jack_data, const char *old_name,
                              const char *new_name) {
    if (!old_name || !new_name)
        return;
    if (port_ident_find(jack_data, new_name))
        return;
    uint64_t key = port_ident_find(jack_data, old_name);
    PORT_IDENT *ident = port_ident_at(jack_data, key);
    if (!ident)
        return;
    char *name_copy = strdup(new_name);
    if (!name_copy)
        return;
    free(ident->name);
    ident->name = name_copy;
}

static void port_idents_clean(JACK_INFO *jack_data) {
    for (size_t i = 0; i < jack_data->ident_count; i++)
        free(jack_data->idents[i].name);
    free(jack_data->idents);
    jack_data->idents = NULL;
    jack_data->ident_count = 0;
    jack_data->ident_max = 0;
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
    for (size_t i = 0; i < jack_data->ident_count; i++)
        jack_data->idents[i].rec = SIZE_MAX;
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
    uint64_t key = port_ident_intern(jack_data, name);
    PORT_IDENT *ident = port_ident_at(jack_data, key);
    if (!ident)
        return;
    char *name_copy = strdup(name);
    if (!name_copy)
        return;
    ident->rec = jack_data->port_count;
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

// what each JackPortList after ALL is queried with, in list order
static const struct {
    unsigned int type;
    unsigned long flow;
} port_list_query[JACK_PORT_LIST_COUNT - 1] = {
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

    const char **queried[JACK_PORT_LIST_COUNT - 1] = {0};
    size_t total = 0;
    for (size_t q = 0; q < JACK_PORT_LIST_COUNT - 1; q++) {
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
    for (size_t q = 0; q < JACK_PORT_LIST_COUNT - 1; q++) {
        jack_data->list_first[q + 1] = jack_data->port_count;
        if (queried[q]) {
            for (size_t i = 0; queried[q][i]; i++)
                port_rec_add(jack_data, queried[q][i], port_list_query[q].type,
                             port_list_query[q].flow, total);
            free(queried[q]);
        }
        jack_data->list_count[q + 1] =
            jack_data->port_count - jack_data->list_first[q + 1];
    }
    jack_data->list_first[JACK_PORT_LIST_ALL] = 0;
    jack_data->list_count[JACK_PORT_LIST_ALL] = jack_data->port_count;
}

// index of the cached port with this key, port_count when there is none
static size_t port_index_by_key(JACK_INFO *jack_data, uint64_t key) {
    const PORT_IDENT *ident = port_ident_at(jack_data, key);
    if (!ident || ident->rec >= jack_data->port_count)
        return jack_data->port_count;
    return ident->rec;
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
            size_t j =
                port_index_by_key(jack_data, port_ident_find(jack_data, peers[c]));
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

JackPortSync app_jack_ports_sync(JACK_INFO *jack_data) {
    JackPortSync done = {false, false};
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

static void port_info_fill(const JACK_PORT_REC *rec, JackPortInfo *out) {
    out->name = rec->name;
    out->client = rec->client;
    out->key = rec->key;
    out->type = rec->type;
    out->flow = rec->flow;
    out->owner_tag = rec->owner_tag;
    out->owner_uid = rec->owner_uid;
}

size_t app_jack_port_count(JACK_INFO *jack_data, JackPortList list) {
    if (!jack_data || list >= JACK_PORT_LIST_COUNT)
        return 0;
    return jack_data->list_count[list];
}

bool app_jack_port_at(JACK_INFO *jack_data, JackPortList list, size_t idx,
                      JackPortInfo *out) {
    if (!jack_data || !out || list >= JACK_PORT_LIST_COUNT)
        return false;
    if (idx >= jack_data->list_count[list])
        return false;
    port_info_fill(&jack_data->ports[jack_data->list_first[list] + idx], out);
    return true;
}

bool app_jack_port_by_key(JACK_INFO *jack_data, uint64_t key,
                          JackPortInfo *out) {
    if (!jack_data || !out)
        return false;
    size_t i = port_index_by_key(jack_data, key);
    if (i >= jack_data->port_count)
        return false;
    port_info_fill(&jack_data->ports[i], out);
    return true;
}

size_t app_jack_port_connection_count(JACK_INFO *jack_data, uint64_t key) {
    if (!jack_data)
        return 0;
    size_t i = port_index_by_key(jack_data, key);
    if (i >= jack_data->port_count)
        return 0;
    return jack_data->ports[i].conn_count;
}

bool app_jack_port_connection_at(JACK_INFO *jack_data, uint64_t key, size_t idx,
                                 JackPortInfo *out) {
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

bool app_jack_port_keys_connected(JACK_INFO *jack_data, uint64_t key_a,
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

// ticks per beat, since user should not set these anyway
double time_ticks_per_beat = 1920.0;
static int app_jack_sys_send_msg(void *user_data, const char *msg) {
    // TODO for future proof, now is not used
    (void)user_data;
    log_append_logfile("%s", msg);
    return 0;
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

JACK_INFO *jack_initialize(void *arg, const char *client_name,
                           int (*process)(jack_nframes_t, void *)) {

    JACK_INFO *jack_data = (JACK_INFO *)malloc(sizeof(JACK_INFO));
    if (!jack_data) {
        return NULL;
    }
    jack_data->rt_tick = 0;
    jack_data->control_data = NULL;
    jack_data->port_owners = NULL;
    jack_data->port_owner_count = 0;
    jack_data->port_owner_max = 0;
    jack_data->ports = NULL;
    jack_data->port_count = 0;
    memset(jack_data->list_first, 0, sizeof(jack_data->list_first));
    memset(jack_data->list_count, 0, sizeof(jack_data->list_count));
    jack_data->clients = NULL;
    jack_data->client_count = 0;
    jack_data->client_max = 0;
    jack_data->conns = NULL;
    jack_data->conn_total = 0;
    jack_data->idents = NULL;
    jack_data->ident_count = 0;
    jack_data->ident_max = 0;
    // start dirty so the first sync builds the cache
    atomic_init(&jack_data->ports_changed, true);
    atomic_init(&jack_data->connections_changed, true);

    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    ui_funcs_struct.send_msg = app_jack_sys_send_msg;
    jack_data->control_data =
        context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if (!jack_data->control_data) {
        jack_clean_memory(jack_data);
        return NULL;
    }

    jack_status_t *server_name = NULL;
    jack_options_t options = JackNullOption;
    jack_status_t status = 0;

    // initialize the general song trk parameters like current bar, beat, play
    // etc.
    jack_data->trk_params = params_init_param_container(NULL);
    // uid 0 - params.c mints; owner_id is this module's own enum. Keep the
    // returned val_ids: the transport code on [audio-thread] indexes by them.
    jack_data->trk_val[TRK_PARAM_TEMPO] =
        param_add_param(jack_data->trk_params, "Tempo", 100, 10, 500, 1, 0,
                        TRK_PARAM_TEMPO, 0, 0, NULL);
    jack_data->trk_val[TRK_PARAM_BAR] =
        param_add_param(jack_data->trk_params, "Bar", 1, 1, MAX_BARS, 1, 0,
                        TRK_PARAM_BAR, 0, 0, NULL);
    jack_data->trk_val[TRK_PARAM_BEAT] =
        param_add_param(jack_data->trk_params, "Beat", 1, 1, 16, 1, 0,
                        TRK_PARAM_BEAT, 0, 0, NULL);
    jack_data->trk_val[TRK_PARAM_TICK] = param_add_param(
        jack_data->trk_params, "Tick", 0, 0, 2000,
        floor(time_ticks_per_beat / 4), 0, TRK_PARAM_TICK, 0, 0, NULL);
    jack_data->trk_val[TRK_PARAM_PLAY] =
        param_add_param(jack_data->trk_params, "Play", 0, 0, 1, 1, 0,
                        TRK_PARAM_PLAY, 0, 0, NULL);
    jack_data->trk_val[TRK_PARAM_BPB] =
        param_add_param(jack_data->trk_params, "BPB", 4, 2, 16, 1, 0,
                        TRK_PARAM_BPB, 0, 0, NULL);
    jack_data->trk_val[TRK_PARAM_BEAT_TYPE] =
        param_add_param(jack_data->trk_params, "Beat_Type", 4, 2, 16, 1, 0,
                        TRK_PARAM_BEAT_TYPE, 0, 0, NULL);
    // a failed add returns -1, which would silently index out of range on the
    //[audio-thread] later - refuse to start instead
    if (!jack_data->trk_params) {
        jack_clean_memory(jack_data);
        return NULL;
    }
    for (int i = TRK_PARAM_NONE + 1; i < TRK_PARAM_COUNT; i++) {
        if (jack_data->trk_val[i] < 0) {
            jack_clean_memory(jack_data);
            return NULL;
        }
    }

    // open client connection
    jack_data->client =
        jack_client_open(client_name, options & status, server_name);
    if (jack_data->client == NULL) {
        free(jack_data);
        return NULL;
    }

    // what the process function is
    jack_set_process_callback(jack_data->client, process, arg);

    // call this function when the engine sample rate changes
    jack_set_sample_rate_callback(jack_data->client, sample_rate_change,
                                  jack_data);

    // set the callback function that updates the *pos struct that holds beat,
    // bar, tick etc information
    jack_set_timebase_callback(jack_data->client, 0, timebbt_callback_rt,
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
    // a rename is its own notification, not a register/unregister pair - own
    // renames re-key the cache in app_jack_port_rename, this catches the rest
    jack_set_port_rename_callback(jack_data->client, app_jack_port_rename_cb,
                                  jack_data);

    /*write some jack client attributes to the jack_data struct*/
    // sample rate of the server
    jack_data->sample_rate = jack_get_sample_rate(jack_data->client);
    jack_data->buffer_size = jack_get_buffer_size(jack_data->client);

    return jack_data;
}

// return the transport param container, NULL on error
PRM_CONTAIN *app_jack_trk_param_container(void *jack_data_handle) {
    JACK_INFO *jack_data = (JACK_INFO *)jack_data_handle;
    if (!jack_data)
        return NULL;
    return jack_data->trk_params;
}

int app_jack_read_ui_to_rt_messages(JACK_INFO *jack_data) {
    // set the is_audio_thread to true so its false on [main-thread] and true on
    // [audio-thread]
    is_audio_thread = true;

    if (!jack_data)
        return -1;
    // update the rt_tick
    jack_data->rt_tick += 1;
    if (jack_data->rt_tick > RT_CYCLES)
        jack_data->rt_tick = 0;

    context_sub_process_rt(jack_data->control_data);

    // read the param ui_to_rt messages and set the parameter values
    param_msgs_process(jack_data->trk_params, 1);

    // if rt params just got new parameter values from the ui they will be just
    // changed in that case jack will create a new transport object and request
    // a transport change
    app_jack_update_transport_from_params_rt(jack_data);
    // now update the bar, beat, tick and is playing parameters from the jack
    // transport for [audio-thread] and [main-thread] update the params on
    // [audio-thread] in rt_tick (as slow as ui [main-thread] params) since the
    // params will only be used to set anything if the user changes some of them
    // on the [main-thread] all other contexts gets the real time beats, ticks
    // and other transport vars from the jack_transport_query function in the
    // app_jack_return_transport_rt function
    if (jack_data->rt_tick == 0) {
        jack_position_t pos;
        jack_transport_state_t state =
            jack_transport_query(jack_data->client, &pos);
        unsigned int pos_ready = 1;
        if (!pos.valid)
            pos_ready = 0;
        if (pos.valid != JackPositionBBT)
            pos_ready = 0;
        if (pos_ready == 1) {
            if (state == JackTransportRolling) {
                // After setting the value, get the value so the parameter
                // is_changed will be 0 and
                // app_jack_update_transport_from_params_rt will not create a
                // new tranport object even though a parameter was not changed
                // by the ui get the bars
                param_set_value_rt(jack_data->trk_params,
                                   jack_data->trk_val[TRK_PARAM_BAR], (float)pos.bar);
                param_get_value(jack_data->trk_params, jack_data->trk_val[TRK_PARAM_BAR],
                                1);
                // get the beat
                param_set_value_rt(jack_data->trk_params,
                                   jack_data->trk_val[TRK_PARAM_BEAT], (float)pos.beat);
                param_get_value(jack_data->trk_params, jack_data->trk_val[TRK_PARAM_BEAT],
                                1);
                // get the tick
                param_set_value_rt(jack_data->trk_params,
                                   jack_data->trk_val[TRK_PARAM_TICK], (float)pos.tick);
                param_get_value(jack_data->trk_params, jack_data->trk_val[TRK_PARAM_TICK],
                                1);
            }
            // get the isPlaying state even if the Jack transport head is not
            // rolling
            param_set_value_rt(jack_data->trk_params, jack_data->trk_val[TRK_PARAM_PLAY],
                               (float)state);
            param_get_value(jack_data->trk_params, jack_data->trk_val[TRK_PARAM_PLAY], 1);
        }
    }
    return 0;
}

int app_jack_read_rt_to_ui_messages(JACK_INFO *jack_data) {
    if (!jack_data)
        return -1;

    context_sub_process_ui(jack_data->control_data);

    // read the param rt_to_ui messages and set the parameter values
    param_msgs_process(jack_data->trk_params, 0);

    return 0;
}

void app_jack_clean_midi_cont(JACK_MIDI_CONT *midi_cont) {
    if (midi_cont->buf_size)
        free(midi_cont->buf_size);
    if (midi_cont->nframe_nums)
        free(midi_cont->nframe_nums);
    if (midi_cont->note_pitches)
        free(midi_cont->note_pitches);
    if (midi_cont->types)
        free(midi_cont->types);
    if (midi_cont->vel_trig)
        free(midi_cont->vel_trig);
}

JACK_MIDI_CONT *app_jack_init_midi_cont(unsigned int array_size) {
    JACK_MIDI_CONT *ret_midi_cont = NULL;
    ret_midi_cont = malloc(sizeof(JACK_MIDI_CONT));
    if (!ret_midi_cont)
        return NULL;
    ret_midi_cont->array_size = array_size;
    ret_midi_cont->num_events = 0;
    ret_midi_cont->w_pos = 0;
    ret_midi_cont->buf_size = NULL;
    ret_midi_cont->nframe_nums = NULL;
    ret_midi_cont->note_pitches = NULL;
    ret_midi_cont->types = NULL;
    ret_midi_cont->vel_trig = NULL;

    ret_midi_cont->buf_size = calloc(array_size, sizeof(size_t));
    if (!ret_midi_cont->buf_size) {
        app_jack_clean_midi_cont(ret_midi_cont);
        free(ret_midi_cont);
        return NULL;
    }
    ret_midi_cont->nframe_nums = calloc(array_size, sizeof(NFRAMES_T));
    if (!ret_midi_cont->nframe_nums) {
        app_jack_clean_midi_cont(ret_midi_cont);
        free(ret_midi_cont);
        return NULL;
    }
    ret_midi_cont->note_pitches = calloc(array_size, sizeof(MIDI_DATA_T));
    if (!ret_midi_cont->note_pitches) {
        app_jack_clean_midi_cont(ret_midi_cont);
        free(ret_midi_cont);
        return NULL;
    }
    ret_midi_cont->types = calloc(array_size, sizeof(MIDI_DATA_T));
    if (!ret_midi_cont->types) {
        app_jack_clean_midi_cont(ret_midi_cont);
        free(ret_midi_cont);
        return NULL;
    }
    ret_midi_cont->vel_trig = calloc(array_size, sizeof(MIDI_DATA_T));
    if (!ret_midi_cont->vel_trig) {
        app_jack_clean_midi_cont(ret_midi_cont);
        free(ret_midi_cont);
        return NULL;
    }
    return ret_midi_cont;
}

void app_jack_midi_cont_reset(JACK_MIDI_CONT *midi_cont) {
    if (!midi_cont)
        return;
    unsigned int size = midi_cont->array_size;
    if (size <= 0)
        return;
    midi_cont->num_events = 0;
    midi_cont->w_pos = 0;
    memset(midi_cont->buf_size, 0, size * sizeof(size_t));
    memset(midi_cont->nframe_nums, 0, size * sizeof(NFRAMES_T));
    memset(midi_cont->note_pitches, 0, size * sizeof(MIDI_DATA_T));
    memset(midi_cont->types, 0, size * sizeof(MIDI_DATA_T));
    memset(midi_cont->vel_trig, 0, size * sizeof(MIDI_DATA_T));
}

int app_jack_port_rename(void *client_in, void *port,
                         const char *new_port_name) {
    if (!new_port_name)
        return -1;
    JACK_INFO *jack_data = (JACK_INFO *)client_in;
    if (!jack_data)
        return -1;
    jack_port_t *jack_port = (jack_port_t *)port;
    if (!jack_port)
        return -1;

    // jack_port_name points at the port's own storage, so the old name has to
    // be taken before the rename overwrites it
    char *old_name = strdup(jack_port_name(jack_port));
    int result = jack_port_rename(jack_data->client, jack_port, new_port_name);
    if (result == 0 && old_name) {
        const char *new_name = jack_port_name(jack_port);
        port_ident_rename(jack_data, old_name, new_name);
        port_owner_rename(jack_data, old_name, new_name);
        atomic_store(&jack_data->ports_changed, true);
    }
    free(old_name);
    return result;
}

void *app_jack_create_port_on_client(void *client_in, unsigned int port_type,
                                     unsigned int io_type,
                                     const char *port_name, uint64_t owner_tag,
                                     uint64_t owner_uid) {
    JACK_INFO *jack_data = (JACK_INFO *)client_in;
    if (!jack_data)
        return NULL;
    jack_client_t *client = jack_data->client;
    if (!client)
        return NULL;
    const char *type = NULL;
    switch (port_type) {
    case PORT_TYPE_AUDIO:
        type = JACK_DEFAULT_AUDIO_TYPE;
        break;
    case PORT_TYPE_MIDI:
        type = JACK_DEFAULT_MIDI_TYPE;
        break;
    default:
        type = JACK_DEFAULT_AUDIO_TYPE;
    }

    void *ret_port = jack_port_register(client, port_name, type, io_type, 0);
    if (!ret_port)
        return NULL;
    // jack prefixes the client name, so record what the port list will see
    port_owner_add(jack_data, jack_port_name((jack_port_t *)ret_port),
                   owner_tag, owner_uid);
    atomic_store(&jack_data->ports_changed, true);
    return ret_port;
}

float app_jack_return_samplerate(JACK_INFO *jack_data) {
    if (!jack_data)
        return -1;
    if (!jack_data->client)
        return -1;
    return (float)jack_get_sample_rate(jack_data->client);
}
int app_jack_return_buffer_size(JACK_INFO *jack_data) {
    if (!jack_data)
        return -1;
    if (!jack_data->client)
        return -1;
    return jack_get_buffer_size(jack_data->client);
}

int app_jack_activate(JACK_INFO *jack_data) {
    // activate the client and launch the process function
    if (jack_activate(jack_data->client)) {
        return -1;
    }
    return 0;
}

int app_jack_port_name_size() { return jack_port_name_size(); }

void *app_jack_get_buffer_rt(void *port, jack_nframes_t nframes) {
    jack_port_t *cur_port = (jack_port_t *)port;
    if (!cur_port)
        return NULL;
    void *out = NULL;
    out = jack_port_get_buffer(cur_port, nframes);
    return out;
}

void app_jack_midi_clear_buffer_rt(void *buffer) {
    if (buffer) {
        jack_midi_clear_buffer(buffer);
    }
}

int app_jack_midi_events_write_rt(void *buffer, jack_nframes_t time,
                                  const jack_midi_data_t *data,
                                  size_t data_size) {
    int return_val = -1;
    if (buffer) {
        return_val = jack_midi_event_write(buffer, time, data, data_size);
    }
    return return_val;
}

void app_jack_return_notes_vels_rt(void *midi_in, JACK_MIDI_CONT *midi_cont) {
    if (midi_in == NULL)
        return;
    int event_count = 0;
    event_count = jack_midi_get_event_count(midi_in);
    if (event_count <= 0)
        return;
    midi_cont->num_events = event_count;
    if ((midi_cont->num_events + midi_cont->w_pos) > midi_cont->array_size)
        midi_cont->num_events = (midi_cont->array_size - midi_cont->w_pos);
    // go through all the midi events
    for (unsigned int en = 0; en < midi_cont->num_events; en++) {
        jack_midi_event_t in_event;
        if (jack_midi_event_get(&in_event, midi_in, en) != 0)
            return;

        // if the note is correct set the vel_trig array of this note index to
        // the midi event velocity
        if (midi_cont->vel_trig != NULL)
            midi_cont->vel_trig[midi_cont->w_pos] = in_event.buffer[2];
        if (midi_cont->note_pitches != NULL)
            midi_cont->note_pitches[midi_cont->w_pos] = in_event.buffer[1];
        if (midi_cont->nframe_nums != NULL)
            midi_cont->nframe_nums[midi_cont->w_pos] = in_event.time;
        if (midi_cont->types != NULL)
            midi_cont->types[midi_cont->w_pos] = in_event.buffer[0];
        if (midi_cont->buf_size != NULL)
            midi_cont->buf_size[midi_cont->w_pos] = in_event.size;
        midi_cont->w_pos += 1;
    }
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

int app_jack_connect_keys(JACK_INFO *jack_data, uint64_t key_a,
                          uint64_t key_b) {
    return port_keys_link(jack_data, key_a, key_b, true);
}

int app_jack_disconnect_keys(JACK_INFO *jack_data, uint64_t key_a,
                             uint64_t key_b) {
    return port_keys_link(jack_data, key_a, key_b, false);
}

int sample_rate_change(jack_nframes_t new_sample_rate, void *arg) {
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    if (!jack_data)
        return -1;
    // TODO this should be [thread-safe] and not a simple var change
    // set the new sample rate on the app data struct so other functions can use
    // that info
    jack_data->sample_rate = new_sample_rate;

    return 0;
}

void app_jack_unregister_port(void *client_in, void *port) {
    if (!client_in)
        return;
    if (!port)
        return;
    JACK_INFO *jack_data = (JACK_INFO *)client_in;
    jack_client_t *client = jack_data->client;
    port_owner_remove(jack_data, jack_port_name((jack_port_t *)port));
    jack_port_unregister(client, port);
    atomic_store(&jack_data->ports_changed, true);
}

void jack_clean_memory(void *jack_data_in) {
    JACK_INFO *jack_data = (JACK_INFO *)jack_data_in;
    if (!jack_data)
        return;
    // close the jack client
    if (jack_data->client != NULL)
        jack_client_close(jack_data->client);
    // clean the general parameters
    if (jack_data->trk_params)
        param_clean_param_container(jack_data->trk_params);

    context_sub_clean(jack_data->control_data);
    ports_cache_clear(jack_data);
    port_idents_clean(jack_data);
    port_owners_clean(jack_data);
    free(jack_data);
}

static int app_jack_transport(JACK_INFO *jack_data,
                              unsigned int transport_type) {
    int ret_int = -1;
    if (!jack_data)
        return -1;
    if (transport_type == 0) {
        jack_transport_stop(jack_data->client);
        return 0;
    }
    if (transport_type == 1) {
        jack_transport_start(jack_data->client);
        return 0;
    }

    return ret_int;
}

void timebbt_callback_rt(jack_transport_state_t state, jack_nframes_t nframes,
                         jack_position_t *pos, int new_pos, void *arg) {
    (void)state;
    // the struct will be used to get a struct from the circle buffer for the
    // time_beats_per_bar and such
    JACK_INFO *jack_data = (JACK_INFO *)arg;
    if (!jack_data)
        return;
    // minutes since frame 0
    double min;
    // ticks since frame 0
    long abs_tick;
    // beats since frame 0
    long abs_beat;
    // bars since frame 0, with remainder
    // TODO now, not used, because of drift debugging
    //  double abs_bars;
    // only calculate the beats and bars from frames on the first run of this
    // function
    if (new_pos && pos->valid != JackPositionBBT) {
        PRM_CONTAIN *transport_cntr = jack_data->trk_params;
        pos->valid = JackPositionBBT;
        pos->beats_per_bar = 4;
        pos->beats_per_bar =
            param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_BPB], 1);
        pos->beat_type = 4;
        pos->beat_type =
            param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_BEAT_TYPE], 1);

        pos->ticks_per_beat = time_ticks_per_beat;
        // get the bpm
        pos->beats_per_minute = 120;
        pos->beats_per_minute =
            param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_TEMPO], 1);

        // Compute the *pos members from frame
        min = pos->frame / ((double)pos->frame_rate * 60.0);
        abs_tick = min * pos->beats_per_minute * pos->ticks_per_beat;
        abs_beat = abs_tick / pos->ticks_per_beat;

        pos->bar = abs_beat / pos->beats_per_bar;
        pos->beat = abs_beat - (pos->bar * pos->beats_per_bar) + 1;
        pos->tick = abs_tick - (abs_beat * pos->ticks_per_beat);
        pos->bar_start_tick =
            pos->bar * pos->beats_per_bar * pos->ticks_per_beat;
        pos->bar++;

    } else {
        // if new pos is not requested add the ticks each cycle
        // TODO now tick drifts so bars and beats are calculated not from ticks,
        // need to figure out how to calculate from ticks and not drift
        if (!new_pos) {
            pos->tick += (nframes * pos->ticks_per_beat *
                          pos->beats_per_minute / (pos->frame_rate * 60));
        }
        double bpm_sec = (double)pos->beats_per_minute / 60.0;
        double secs = (double)pos->frame / (double)pos->frame_rate;
        double beats = bpm_sec * secs;
        double bar = beats / (double)pos->beats_per_bar;
        pos->bar = (unsigned int)bar;
        // pos->bar += 1;
        double beat_frac = bar - (unsigned int)bar;
        double beat_div = 1.0 / (double)pos->beats_per_bar;
        pos->beat = (unsigned int)(beat_frac / beat_div);
        pos->beat += 1;
        if (pos->beat == 1) {
            pos->bar_start_tick += (pos->beats_per_bar * pos->ticks_per_beat);
        }
        while (pos->tick >= pos->ticks_per_beat) {
            pos->tick -= pos->ticks_per_beat;
            /*
            if(++pos->beat > pos->beats_per_bar){
                pos->beat = 1;
                ++pos->bar;
                pos->bar_start_tick += (pos->beats_per_bar *
            pos->ticks_per_beat);
            }
            */
        }
    }

    // if the bar exceed the max number of bars available in song stop
    if (pos->bar > MAX_BARS) {
        pos->bar = MAX_BARS;
        pos->tick = 0;
        pos->beat = 1;
        app_jack_transport(jack_data, 0);
    }
}

void app_jack_update_transport_from_params_rt(JACK_INFO *jack_data) {
    if (!jack_data)
        return;

    PRM_CONTAIN *transport_cntr = jack_data->trk_params;
    if (!transport_cntr)
        return;

    // check if any parameters have changed
    int params_changed = param_get_if_any_changed_rt(transport_cntr);
    if (params_changed != 1)
        return;

    // get the current pos of transport
    jack_position_t old_pos;
    jack_transport_query(jack_data->client, &old_pos);

    // create the new pos
    jack_position_t new_pos;
    new_pos.valid = JackPositionBBT;
    new_pos.frame_rate = old_pos.frame_rate;

    new_pos.beats_per_minute =
        param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_TEMPO], 1);
    new_pos.beats_per_bar =
        param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_BPB], 1);

    new_pos.bar = param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_BAR], 1);
    new_pos.beat = param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_BEAT], 1);
    if (new_pos.beat > new_pos.beats_per_bar) {
        new_pos.beat = 1;
        new_pos.bar += 1;
    }

    new_pos.tick = param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_TICK], 1);

    new_pos.beat_type =
        param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_BEAT_TYPE], 1);
    new_pos.ticks_per_beat = time_ticks_per_beat;

    // first calculate how many beats from frame 0, with remainder
    double seconds_passed =
        (((new_pos.bar) * new_pos.beats_per_bar) + (new_pos.beat - 1)) +
        (new_pos.tick / new_pos.ticks_per_beat);
    // now calculate how many seconds has passed since frame 0
    seconds_passed = (seconds_passed / new_pos.beats_per_minute) * 60;
    // now calculate the new frames since frame 0
    jack_nframes_t frame = ceil(seconds_passed * old_pos.frame_rate);
    new_pos.frame = frame;

    float play = param_get_value(transport_cntr, jack_data->trk_val[TRK_PARAM_PLAY], 1);
    app_jack_transport(jack_data, (int)play);

    jack_transport_reposition(jack_data->client, &new_pos);
}

int app_jack_return_transport_rt(void *audio_client, int32_t *cur_bar,
                                 int32_t *cur_beat, int32_t *cur_tick,
                                 SAMPLE_T *ticks_per_beat,
                                 jack_nframes_t *total_frames, float *bpm,
                                 float *beat_type, float *beats_per_bar) {
    if (!audio_client)
        return -1;
    JACK_INFO *jack_data = (JACK_INFO *)audio_client;
    if (!jack_data)
        return -1;

    int isPlaying = 0;

    jack_position_t pos;
    jack_transport_state_t state =
        jack_transport_query(jack_data->client, &pos);
    if (state != JackTransportStopped)
        isPlaying = 1;

    if (pos.valid)
        *total_frames = pos.frame;

    if (pos.valid != JackPositionBBT)
        return -1;
    *bpm = pos.beats_per_minute;
    *beat_type = pos.beat_type;
    *beats_per_bar = pos.beats_per_bar;
    *cur_bar = pos.bar;
    *cur_beat = pos.beat;
    *cur_tick = pos.tick;
    *ticks_per_beat = (SAMPLE_T)pos.ticks_per_beat;

    return isPlaying;
}
