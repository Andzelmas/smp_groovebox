#pragma once
#include "backend/audio_backend.h"
#include "contexts/clap_plugins.h"
#include "contexts/plugins.h"
#include "contexts/sampler.h"
#include "contexts/synth.h"
#include "engine/graph.h"
#include "engine/transport.h"
#include <stdint.h>

// The song file: the transport's, the synth's and the samples' param values,
// the loaded plugins with their own state, the graph's edges and the exposed
// ports' links to other programs. A song is a folder, <name>/<name>.json with
// the plugin states in a states_<n> folder beside it. Only what is the same
// every run is saved - a plugin URI or CLAP id, a param key, a sample path, a
// port name - never a session uid. Every problem is logged. [main-thread]

typedef struct _song SONG;

// a graph node's owner, as given to graph_node_add
typedef struct _song_owner {
    uint64_t tag;
    uint64_t uid;
} SONG_OWNER;

// what a song is made of
typedef struct _song_modules {
    GRAPH *graph;
    AUDIO_BACKEND *backend;
    TRANSPORT *transport;
    SYNTH_DATA *synth;
    SMP_INFO *smp;
    PLUG_INFO *lv2;
    CLAP_PLUG_INFO *clap;
    // the owners of the nodes every run has: the exposed ports, the sampler
    // and the MIDI clock
    SONG_OWNER root;
    SONG_OWNER sampler;
    SONG_OWNER midi_clock;
    // the tags of the owners whose uid is their own: an oscillator's slot, a
    // plugin's uid
    uint64_t synth_osc_tag;
    uint64_t lv2_tag;
    uint64_t clap_tag;
} SONG_MODULES;

typedef enum {
    SONG_OK,
    // the path names no song: it ends in '/', is relative with no song
    // folder, does not fit, or is "" with no current song and no song folder
    SONG_INVALID,
    // could not be written or read, or is not a song file
    SONG_FAILED,
} SONG_RESULT;

// song_dir: where a relative path goes, template_path: the song a start and
// song_new load. "" for none, both copied. NULL on failure
SONG *song_init(const SONG_MODULES *modules, const char *song_dir,
                const char *template_path);
void song_clean(SONG *song);

// save the song at path: a relative one under song_dir, ~ or absolute as it
// is. Its last part names the song folder and the file in it, a ".json" end
// dropped. "" is the current song, or a new dated one in song_dir when there
// is none. A failure leaves the song that was there. Becomes the current song
SONG_RESULT song_save(SONG *song, const char *path);
// replace the song with the one at path, a song folder as song_save takes it
// or its .json file. A file that is not a song changes nothing. Becomes the
// current song
SONG_RESULT song_load(SONG *song, const char *path);
// load the template, or without one (or one that is not a song) empty the
// song: no plugins, samples or edges, the transport's and the synth's params
// at their defaults and the MIDI clock wired to the exposed
// AUDIO_BACKEND_MIDI_CLOCK_OUT. Leaves no current song
void song_new(SONG *song);
