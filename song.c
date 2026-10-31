#include "song.h"
#include "contexts/params.h"
#include "engine/midi_clock.h"
#include "types.h"
#include "util_funcs/json.h"
#include "util_funcs/log_funcs.h"
#include "util_funcs/path_funcs.h"
#include "util_funcs/string_funcs.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SONG_FORMAT 1
#define SONG_KIND "song"
#define SONG_EXT ".json"
#define SONG_SEP '/'
// a song folder's plugin states folder: the prefix and up to 9 digits, no
// leading 0
#define SONG_STATES_PREFIX "states_"
#define SONG_STATES_DIGITS 9
#define SONG_STATES_MAX 999999999UL
// a CLAP plugin's state in the states folder: <id> and this
#define SONG_CLAP_STATE_EXT ".clapstate"
// the owners every run has, as an edge names them. A plugin's is its id
#define SONG_OWNER_ROOT "root"
#define SONG_OWNER_SAMPLER "sampler"
#define SONG_OWNER_MIDI_CLOCK "midi_clock"
#define SONG_OWNER_SYNTH_OSC "synth_osc_"
#define SONG_PLUGIN_LV2 "lv2"
#define SONG_PLUGIN_CLAP "clap"
// dated names a save without a path tries
#define SONG_DATED_TRIES 100

struct _song {
    SONG_MODULES m;
    char song_dir[MAX_PATH_STRING];
    char template_path[MAX_PATH_STRING];
    // the last saved or loaded song's folder and file, "" for none
    char cur_dir[MAX_PATH_STRING];
    char cur_file[MAX_PATH_STRING];
};

// a plugin loaded from the file: its id there and its node's owner. dup:
// another plugin of the file has the id too
typedef struct {
    int64_t id;
    SONG_OWNER owner;
    bool dup;
} SONG_LOADED;

typedef struct {
    SONG_LOADED *items;
    size_t count;
} SONG_IDS;

static const char *const song_keys[] = {
    "format", "kind",    "states", "transport", "synth",
    "samples", "plugins", "edges",  "links"};

static bool song_fits(int len, size_t cap) {
    return len >= 0 && (size_t)len < cap;
}

// path with a leading ~ expanded, a relative one under song_dir, tidied
// ("a//b" is "a/b") into out. false (logged) when it ends in '/', is relative
// with no song folder, names nothing or does not fit
static bool song_path_full(const SONG *song, const char *path,
                           char out[MAX_PATH_STRING]) {
    size_t len = strlen(path);
    if (len == 0 || path[len - 1] == SONG_SEP) {
        log_append_logfile("song: \"%s\" ends in '/', it names no song\n",
                           path);
        return false;
    }
    char expanded[MAX_PATH_STRING];
    char joined[MAX_PATH_STRING];
    if (path_expand_home(path, expanded, sizeof(expanded)) != 0) {
        log_append_logfile("song: \"%s\" is too long or HOME is not set\n",
                           path);
        return false;
    }
    const char *full = expanded;
    if (expanded[0] != SONG_SEP) {
        if (song->song_dir[0] == '\0') {
            log_append_logfile("song: \"%s\" is relative and there is no "
                               "\"songs\".\"song_dir\"\n",
                               path);
            return false;
        }
        if (!song_fits(snprintf(joined, sizeof(joined), "%s/%s",
                                song->song_dir, expanded),
                       sizeof(joined))) {
            log_append_logfile("song: \"%s\" is too long\n", path);
            return false;
        }
        full = joined;
    }
    // the tidy drops a leading '/'
    size_t lead = full[0] == SONG_SEP ? 1 : 0;
    out[0] = SONG_SEP;
    if (str_segments_tidy(out + lead, MAX_PATH_STRING - lead, full,
                          SONG_SEP) != 0) {
        log_append_logfile("song: \"%s\" is too long\n", path);
        return false;
    }
    if (out[lead] == '\0') {
        log_append_logfile("song: \"%s\" names no song\n", path);
        return false;
    }
    return true;
}

// the folder and the json file of the song typed as path. A load also takes
// the json file itself. false (logged) when path names no song
static bool song_path_resolve(const SONG *song, const char *path, bool load,
                              char dir[MAX_PATH_STRING],
                              char file[MAX_PATH_STRING]) {
    char full[MAX_PATH_STRING];
    if (!song_path_full(song, path, full))
        return false;
    const char *slash = strrchr(full, SONG_SEP);
    const char *last = slash ? slash + 1 : full;
    size_t last_len = strlen(last);
    size_t ext_len = strlen(SONG_EXT);
    bool is_json = last_len > ext_len &&
                   strcmp(last + last_len - ext_len, SONG_EXT) == 0;
    if (load && is_json) {
        // "/song.json" is in "/"
        int dir_len = slash == full ? 1 : (int)(slash - full);
        snprintf(dir, MAX_PATH_STRING, "%.*s", dir_len, full);
        snprintf(file, MAX_PATH_STRING, "%s", full);
        return true;
    }
    size_t name_len = is_json ? last_len - ext_len : last_len;
    int dir_len = (int)(last - full + name_len);
    if (snprintf(dir, MAX_PATH_STRING, "%.*s", dir_len, full) < 0 ||
        !song_fits(snprintf(file, MAX_PATH_STRING, "%s/%.*s%s", dir,
                            (int)name_len, last, SONG_EXT),
                   MAX_PATH_STRING)) {
        log_append_logfile("song: \"%s\" is too long\n", path);
        return false;
    }
    return true;
}

// song_dir/song_<date>_<time>, _2, _3.. when that is taken. false (logged)
// when there is no song_dir or every name is taken
static bool song_path_dated(const SONG *song, char dir[MAX_PATH_STRING],
                            char file[MAX_PATH_STRING]) {
    if (song->song_dir[0] == '\0') {
        log_append_logfile("song: no current song and no "
                           "\"songs\".\"song_dir\", give a path to save to\n");
        return false;
    }
    time_t now = time(NULL);
    struct tm local;
    char stamp[64];
    if (!localtime_r(&now, &local) ||
        strftime(stamp, sizeof(stamp), "song_%Y-%m-%d_%H-%M-%S", &local) == 0)
        return false;
    for (unsigned int n = 1; n <= SONG_DATED_TRIES; n++) {
        char name[96];
        if (n == 1)
            snprintf(name, sizeof(name), "%s", stamp);
        else
            snprintf(name, sizeof(name), "%s_%u", stamp, n);
        if (!song_path_resolve(song, name, false, dir, file))
            return false;
        struct stat st;
        if (lstat(dir, &st) != 0 && errno == ENOENT)
            return true;
    }
    log_append_logfile("song: every song_%s name in %s is taken\n", stamp,
                       song->song_dir);
    return false;
}

// n of a states folder name the app makes, 0 for any other name
static unsigned long song_states_index(const char *name) {
    size_t prefix = strlen(SONG_STATES_PREFIX);
    if (strncmp(name, SONG_STATES_PREFIX, prefix) != 0)
        return 0;
    const char *digits = name + prefix;
    size_t len = strlen(digits);
    if (len == 0 || len > SONG_STATES_DIGITS || digits[0] == '0')
        return 0;
    for (size_t i = 0; i < len; i++)
        if (digits[i] < '0' || digits[i] > '9')
            return 0;
    return strtoul(digits, NULL, 10);
}

// the highest states folder index in dir, 0 for none
static unsigned long song_states_highest(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    unsigned long highest = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        unsigned long n = song_states_index(entry->d_name);
        if (n > highest)
            highest = n;
    }
    closedir(d);
    return highest;
}

// the states folders in dir but keep. Anything else there is the user's
static void song_states_remove_others(const char *dir, const char *keep) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (song_states_index(entry->d_name) == 0 ||
            strcmp(entry->d_name, keep) == 0)
            continue;
        char path[MAX_PATH_STRING];
        struct stat st;
        if (!song_fits(snprintf(path, sizeof(path), "%s/%s", dir,
                                entry->d_name),
                       sizeof(path)) ||
            lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (path_remove_tree(path) != 0)
            log_append_logfile("song: %s: %s, the old states stay\n", path,
                               strerror(errno));
    }
    closedir(d);
}

// whether path has a part that is a states folder name the app makes
static bool song_path_in_states(const char *path) {
    size_t pos = 0;
    const char *seg;
    size_t len;
    while (str_segment_next(path, SONG_SEP, &pos, &seg, &len)) {
        char name[64];
        if (len < sizeof(name)) {
            memcpy(name, seg, len);
            name[len] = '\0';
            if (song_states_index(name) != 0)
                return true;
        }
    }
    return false;
}

// lilv links an LV2 state to the files outside its folder. One into a states
// folder - of this song, whose old ones the save removes, or of another - is
// replaced by a copy of its file. false (logged) when a copy failed
static bool song_links_copy(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        log_append_logfile("song: %s: %s\n", dir, strerror(errno));
        return false;
    }
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[MAX_PATH_STRING];
        struct stat st;
        if (!song_fits(snprintf(path, sizeof(path), "%s/%s", dir,
                                entry->d_name),
                       sizeof(path)) ||
            lstat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            ok = song_links_copy(path);
            continue;
        }
        char target[PATH_MAX];
        // a dangling link stays as it is
        if (!S_ISLNK(st.st_mode) || !realpath(path, target) ||
            !song_path_in_states(target))
            continue;
        if (path_copy_file(target, path) != 0) {
            log_append_logfile("song: could not copy %s to %s: %s\n", target,
                               path, strerror(errno));
            ok = false;
        }
    }
    closedir(d);
    return ok;
}

// a plugin's state in states_dir: its id and ext ("" for a folder)
static bool song_state_path(const char *states_dir, int64_t id,
                            const char *ext, char out[MAX_PATH_STRING]) {
    return song_fits(snprintf(out, MAX_PATH_STRING, "%s/%" PRId64 "%s",
                              states_dir, id, ext),
                     MAX_PATH_STRING);
}

static void song_osc_word(char *out, size_t cap, uint64_t uid) {
    snprintf(out, cap, "%s%" PRIu64, SONG_OWNER_SYNTH_OSC, uid);
}

// a param a song keeps: one with a key the user can set, not the playhead
static bool song_param_kept(PRM_CONTAIN *container, int val_id) {
    const char *key = param_get_key(container, val_id);
    return key && key[0] != '\0' && !param_is_readonly(container, val_id) &&
           (param_get_flags(container, val_id) & PARAM_FLAG_NO_SAVE) == 0;
}

// container's kept params' values, by key
static JSON_VAL *song_params_write(JSON_DOC *doc, PRM_CONTAIN *container) {
    JSON_VAL *values = json_new_object(doc);
    unsigned int count = param_return_num_params(container, 0);
    for (unsigned int val_id = 0; val_id < count; val_id++) {
        if (!song_param_kept(container, (int)val_id))
            continue;
        // json has no NaN / Inf, such a value is left out
        PARAM_T value = param_get_raw_value(container, (int)val_id);
        if (isfinite(value))
            json_obj_set(values, param_get_key(container, (int)val_id),
                         json_new_double(doc, value));
    }
    return values;
}

// container's kept params from values, by key: a saved value, else the
// default. A key values has and container has not is logged; where says
// whose they are
static void song_params_read(PRM_CONTAIN *container, const JSON_VAL *values,
                             const char *file, const char *where) {
    unsigned int count = param_return_num_params(container, 0);
    for (unsigned int val_id = 0; val_id < count; val_id++) {
        if (!song_param_kept(container, (int)val_id))
            continue;
        const char *key = param_get_key(container, (int)val_id);
        const JSON_VAL *v = json_get(values, key);
        double value;
        if (json_as_double(v, &value)) {
            param_set_value(container, (int)val_id, value, NULL,
                            Operation_SetValue);
            continue;
        }
        if (v)
            log_append_logfile("song: %s:%u: %s \"%s\" is not a number, set "
                               "to its default\n",
                               file, json_line(v), where, key);
        param_set_value(container, (int)val_id, 0, NULL, Operation_DefValue);
    }
    for (size_t i = 0; i < json_count(values); i++) {
        const char *key = json_key_at(values, i);
        if (param_find_key(container, key) == -1)
            log_append_logfile("song: %s:%u: %s has no param \"%s\", "
                               "skipped\n",
                               file, json_line(json_at(values, i)), where, key);
    }
}

// the id a save gives the plugin owning (tag, uid): the plugins are numbered
// from 1 in the order they are written, LV2 then CLAP. 0 for none
static int64_t song_plugin_id(const SONG *song, uint64_t tag, uint64_t uid) {
    int64_t id = 0;
    for (unsigned int i = 0;; i++) {
        void *plug = plug_plugin_return(song->m.lv2, i);
        if (!plug)
            break;
        id++;
        if (tag == song->m.lv2_tag && plug_plugin_uid(plug) == uid)
            return id;
    }
    for (unsigned int i = 0;; i++) {
        void *plug = clap_plug_plugin_return(song->m.clap, i);
        if (!plug)
            break;
        id++;
        if (tag == song->m.clap_tag && clap_plug_plugin_uid(plug) == uid)
            return id;
    }
    return 0;
}

static bool song_owner_is(const SONG_OWNER *owner, uint64_t tag,
                          uint64_t uid) {
    return owner->tag == tag && owner->uid == uid;
}

// an edge end's owner as the file has it, NULL for one the song does not know
static JSON_VAL *song_owner_value(const SONG *song, JSON_DOC *doc,
                                  uint64_t tag, uint64_t uid) {
    const SONG_MODULES *m = &song->m;
    if (song_owner_is(&m->root, tag, uid))
        return json_new_string(doc, SONG_OWNER_ROOT);
    if (song_owner_is(&m->sampler, tag, uid))
        return json_new_string(doc, SONG_OWNER_SAMPLER);
    if (song_owner_is(&m->midi_clock, tag, uid))
        return json_new_string(doc, SONG_OWNER_MIDI_CLOCK);
    if (tag == m->synth_osc_tag) {
        char word[64];
        song_osc_word(word, sizeof(word), uid);
        return json_new_string(doc, word);
    }
    int64_t id = song_plugin_id(song, tag, uid);
    return id > 0 ? json_new_int(doc, id) : NULL;
}

static JSON_VAL *song_edge_end(JSON_DOC *doc, JSON_VAL *owner,
                               const char *port) {
    JSON_VAL *end = json_new_object(doc);
    json_obj_set(end, "owner", owner);
    json_obj_set(end, "port", json_new_string(doc, port));
    return end;
}

// every edge once, from its output
static JSON_VAL *song_edges_write(const SONG *song, JSON_DOC *doc) {
    GRAPH *graph = song->m.graph;
    JSON_VAL *edges = json_new_array(doc);
    size_t count = graph_port_count(graph, GRAPH_PORT_LIST_ALL);
    for (size_t i = 0; i < count; i++) {
        GraphPortInfo from;
        if (!graph_port_at(graph, GRAPH_PORT_LIST_ALL, i, &from) ||
            from.flow != PORT_FLOW_OUTPUT)
            continue;
        GraphPortInfo to;
        for (size_t c = 0; graph_port_connection_at(graph, from.key, c, &to);
             c++) {
            JSON_VAL *from_owner =
                song_owner_value(song, doc, from.owner_tag, from.owner_uid);
            JSON_VAL *to_owner =
                song_owner_value(song, doc, to.owner_tag, to.owner_uid);
            if (!from_owner || !to_owner) {
                log_append_logfile("song: the edge %s -> %s has an owner a "
                                   "song does not know, not saved\n",
                                   from.name, to.name);
                continue;
            }
            JSON_VAL *edge = json_new_object(doc);
            json_obj_set(edge, "from", song_edge_end(doc, from_owner, from.name));
            json_obj_set(edge, "to", song_edge_end(doc, to_owner, to.name));
            json_arr_push(edges, edge);
        }
    }
    return edges;
}

// an exposed port's name without "client:" - JACK renames a second copy of
// the app
static const char *song_port_short(const BackendPortInfo *info) {
    size_t len = strlen(info->client);
    return info->name[len] == ':' ? info->name + len + 1 : info->name;
}

// every exposed port's links to other programs' ports
static JSON_VAL *song_links_write(const SONG *song, JSON_DOC *doc) {
    AUDIO_BACKEND *backend = song->m.backend;
    JSON_VAL *links = json_new_array(doc);
    size_t count = audio_backend_port_count(backend, BACKEND_PORT_LIST_EXPOSED);
    for (size_t i = 0; i < count; i++) {
        BackendPortInfo port;
        if (!audio_backend_port_at(backend, BACKEND_PORT_LIST_EXPOSED, i,
                                   &port))
            continue;
        BackendPortInfo peer;
        for (size_t c = 0;
             audio_backend_port_connection_at(backend, port.key, c, &peer);
             c++) {
            JSON_VAL *link = json_new_object(doc);
            json_obj_set(link, "port",
                         json_new_string(doc, song_port_short(&port)));
            json_obj_set(link, "peer", json_new_string(doc, peer.name));
            json_arr_push(links, link);
        }
    }
    return links;
}

// the samples, each by its real path
static JSON_VAL *song_samples_write(const SONG *song, JSON_DOC *doc) {
    JSON_VAL *samples = json_new_array(doc);
    for (unsigned int i = 0;; i++) {
        void *smp = smp_sample_return(song->m.smp, i);
        if (!smp)
            break;
        const char *path = smp_sample_file_path(smp);
        char real[PATH_MAX];
        if (realpath(path, real))
            path = real;
        else
            log_append_logfile("song: %s: %s, saved as it is\n", path,
                               strerror(errno));
        JSON_VAL *entry = json_new_object(doc);
        json_obj_set(entry, "path", json_new_string(doc, path));
        json_obj_set(entry, "params",
                     song_params_write(doc, smp_sample_param_container(smp)));
        json_arr_push(samples, entry);
    }
    return samples;
}

// the plugins, numbered as song_plugin_id does, their states into
// states_dir. NULL (logged) when a state could not be saved
static JSON_VAL *song_plugins_write(const SONG *song, JSON_DOC *doc,
                                    const char *states_dir) {
    JSON_VAL *plugins = json_new_array(doc);
    int64_t id = 0;
    for (unsigned int i = 0;; i++) {
        void *plug = plug_plugin_return(song->m.lv2, i);
        if (!plug)
            break;
        id++;
        char dir[MAX_PATH_STRING];
        if (!song_state_path(states_dir, id, "", dir) ||
            plug_plugin_state_save(plug, dir) != 0 || !song_links_copy(dir)) {
            log_append_logfile("song: the state of %s could not be saved\n",
                               plug_plugin_name(plug));
            return NULL;
        }
        JSON_VAL *entry = json_new_object(doc);
        json_obj_set(entry, "id", json_new_int(doc, id));
        json_obj_set(entry, "kind", json_new_string(doc, SONG_PLUGIN_LV2));
        json_obj_set(entry, "uri", json_new_string(doc, plug_plugin_uri(plug)));
        json_arr_push(plugins, entry);
    }
    for (unsigned int i = 0;; i++) {
        void *plug = clap_plug_plugin_return(song->m.clap, i);
        if (!plug)
            break;
        id++;
        JSON_VAL *entry = json_new_object(doc);
        json_obj_set(entry, "id", json_new_int(doc, id));
        json_obj_set(entry, "kind", json_new_string(doc, SONG_PLUGIN_CLAP));
        json_obj_set(entry, "clap_id",
                     json_new_string(doc, clap_plug_plugin_id(plug)));
        json_arr_push(plugins, entry);
        // without a state the params are all a song has of it
        if (!clap_plug_state_available(plug)) {
            json_obj_set(entry, "params",
                         song_params_write(
                             doc, clap_plug_plugin_param_container(plug)));
            continue;
        }
        char path[MAX_PATH_STRING];
        if (!song_state_path(states_dir, id, SONG_CLAP_STATE_EXT, path) ||
            clap_plug_state_save(plug, path) != 0) {
            log_append_logfile("song: the state of %s could not be saved\n",
                               clap_plug_plugin_name(plug));
            return NULL;
        }
    }
    return plugins;
}

// the song as a json doc, the plugin states saved into states_dir. NULL
// (logged) on failure
static JSON_DOC *song_doc_build(const SONG *song, const char *states_dir,
                                const char *states_name) {
    const SONG_MODULES *m = &song->m;
    JSON_DOC *doc = json_doc_new();
    if (!doc) {
        log_append_logfile("song: out of memory, not saved\n");
        return NULL;
    }
    JSON_VAL *root = json_new_object(doc);
    json_set_root(doc, root);
    json_obj_set(root, "format", json_new_int(doc, SONG_FORMAT));
    json_obj_set(root, "kind", json_new_string(doc, SONG_KIND));
    json_obj_set(root, "states", json_new_string(doc, states_name));
    json_obj_set(root, "transport",
                 song_params_write(doc, transport_param_container(m->transport)));
    JSON_VAL *synth = json_new_object(doc);
    json_obj_set(root, "synth", synth);
    size_t osc_count = synth_return_osc_num(m->synth);
    for (unsigned int i = 0; i < osc_count; i++) {
        void *osc = synth_osc_return(m->synth, i);
        char word[64];
        song_osc_word(word, sizeof(word), synth_osc_uid(osc));
        json_obj_set(synth, word,
                     song_params_write(doc, synth_osc_param_container(osc)));
    }
    json_obj_set(root, "samples", song_samples_write(song, doc));
    JSON_VAL *plugins = song_plugins_write(song, doc, states_dir);
    if (!plugins) {
        json_doc_free(doc);
        return NULL;
    }
    json_obj_set(root, "plugins", plugins);
    json_obj_set(root, "edges", song_edges_write(song, doc));
    json_obj_set(root, "links", song_links_write(song, doc));
    return doc;
}

static void song_current_set(SONG *song, const char *dir, const char *file) {
    snprintf(song->cur_dir, sizeof(song->cur_dir), "%s", dir);
    snprintf(song->cur_file, sizeof(song->cur_file), "%s", file);
}

SONG_RESULT song_save(SONG *song, const char *path) {
    if (!song || !path)
        return SONG_INVALID;
    char dir[MAX_PATH_STRING];
    char file[MAX_PATH_STRING];
    if (path[0] != '\0') {
        if (!song_path_resolve(song, path, false, dir, file))
            return SONG_INVALID;
    } else if (song->cur_file[0] != '\0') {
        memcpy(dir, song->cur_dir, sizeof(dir));
        memcpy(file, song->cur_file, sizeof(file));
    } else if (!song_path_dated(song, dir, file))
        return SONG_INVALID;

    if (path_make_dirs(dir) != 0) {
        log_append_logfile("song: %s: %s, not saved\n", dir, strerror(errno));
        return SONG_FAILED;
    }
    // a states folder above every one there, so the old song stays whole
    // until the json is in place
    unsigned long highest = song_states_highest(dir);
    if (highest >= SONG_STATES_MAX) {
        log_append_logfile("song: %s has %s%lu, not saved\n", dir,
                           SONG_STATES_PREFIX, highest);
        return SONG_FAILED;
    }
    char states_name[32];
    char states_dir[MAX_PATH_STRING];
    snprintf(states_name, sizeof(states_name), "%s%lu", SONG_STATES_PREFIX,
             highest + 1);
    if (!song_fits(snprintf(states_dir, sizeof(states_dir), "%s/%s", dir,
                            states_name),
                   sizeof(states_dir)) ||
        mkdir(states_dir, 0755) != 0) {
        log_append_logfile("song: %s/%s: %s, not saved\n", dir, states_name,
                           strerror(errno));
        return SONG_FAILED;
    }
    JSON_DOC *doc = song_doc_build(song, states_dir, states_name);
    JSON_ERR err;
    bool written = doc && json_write_file(doc, file, JSON_WRITE_PRETTY, &err);
    if (doc && !written)
        log_append_logfile("song: %s: %s, not saved\n", file, err.msg);
    json_doc_free(doc);
    if (!written) {
        if (path_remove_tree(states_dir) != 0)
            log_append_logfile("song: %s: %s, left behind\n", states_dir,
                               strerror(errno));
        return SONG_FAILED;
    }
    song_states_remove_others(dir, states_name);
    song_current_set(song, dir, file);
    log_append_logfile("song: %s saved\n", file);
    return SONG_OK;
}

// removes every plugin and sample, drops every edge and every exposed port's
// link
static void song_clear(const SONG *song) {
    const SONG_MODULES *m = &song->m;
    // the links dropped, and those a load finds, are the ones there now
    audio_backend_ports_sync(m->backend);
    // a remove that fails moves on, so the walks end
    for (unsigned int i = 0;;) {
        void *plug = plug_plugin_return(m->lv2, i);
        if (!plug)
            break;
        if (plug_stop_and_remove_plug(plug) != 0)
            i++;
    }
    for (unsigned int i = 0;;) {
        void *plug = clap_plug_plugin_return(m->clap, i);
        if (!plug)
            break;
        if (clap_plug_plug_stop_and_clean(plug) != 0)
            i++;
    }
    for (unsigned int i = 0;;) {
        void *smp = smp_sample_return(m->smp, i);
        if (!smp)
            break;
        if (smp_stop_and_remove_sample(smp) != 0)
            i++;
    }
    // the edges between the nodes every run has
    size_t count = graph_port_count(m->graph, GRAPH_PORT_LIST_ALL);
    for (size_t i = 0; i < count; i++) {
        GraphPortInfo port;
        if (!graph_port_at(m->graph, GRAPH_PORT_LIST_ALL, i, &port) ||
            port.flow != PORT_FLOW_OUTPUT)
            continue;
        GraphPortInfo peer;
        for (size_t c = 0;
             graph_port_connection_at(m->graph, port.key, c, &peer);)
            if (graph_disconnect_keys(m->graph, port.key, peer.key) != 0)
                c++;
    }
    count = audio_backend_port_count(m->backend, BACKEND_PORT_LIST_EXPOSED);
    for (size_t i = 0; i < count; i++) {
        BackendPortInfo port;
        if (!audio_backend_port_at(m->backend, BACKEND_PORT_LIST_EXPOSED, i,
                                   &port))
            continue;
        BackendPortInfo peer;
        for (size_t c = 0;
             audio_backend_port_connection_at(m->backend, port.key, c, &peer);) {
            if (audio_backend_disconnect_keys(m->backend, port.key, peer.key) ==
                0)
                continue;
            log_append_logfile("song: could not unlink %s from %s\n",
                               port.name, peer.name);
            c++;
        }
    }
}

static void song_clock_wire(const SONG *song) {
    const SONG_MODULES *m = &song->m;
    if (graph_connect_keys(m->graph,
                           graph_port_find(m->graph, m->midi_clock.tag,
                                           m->midi_clock.uid,
                                           MIDI_CLOCK_PORT_NAME),
                           graph_port_find(m->graph, m->root.tag, m->root.uid,
                                           AUDIO_BACKEND_MIDI_CLOCK_OUT)) != 0)
        log_append_logfile("Could not connect the MIDI clock to %s\n",
                           AUDIO_BACKEND_MIDI_CLOCK_OUT);
}

// what a start without a song has
static void song_empty(const SONG *song) {
    const SONG_MODULES *m = &song->m;
    song_clear(song);
    song_params_read(transport_param_container(m->transport), NULL, NULL,
                     NULL);
    size_t osc_count = synth_return_osc_num(m->synth);
    for (unsigned int i = 0; i < osc_count; i++)
        song_params_read(synth_osc_param_container(synth_osc_return(m->synth, i)),
                         NULL, NULL, NULL);
    song_clock_wire(song);
}

// obj's member key when it is of type, NULL when it is missing or (logged) of
// another type
static const JSON_VAL *song_member(const JSON_VAL *obj, const char *key,
                                   JSON_TYPE type, const char *file) {
    const JSON_VAL *v = json_get(obj, key);
    if (!v || json_type(v) == type)
        return v;
    log_append_logfile("song: %s:%u: \"%s\" is not %s, skipped\n", file,
                       json_line(v), key,
                       type == JSON_ARRAY ? "an array" : "an object");
    return NULL;
}

static void song_synth_load(const SONG *song, const JSON_VAL *root,
                            const char *file) {
    const SONG_MODULES *m = &song->m;
    const JSON_VAL *synth = song_member(root, "synth", JSON_OBJECT, file);
    size_t osc_count = synth_return_osc_num(m->synth);
    for (unsigned int i = 0; i < osc_count; i++) {
        void *osc = synth_osc_return(m->synth, i);
        char word[64];
        song_osc_word(word, sizeof(word), synth_osc_uid(osc));
        song_params_read(synth_osc_param_container(osc),
                         song_member(synth, word, JSON_OBJECT, file), file,
                         word);
    }
    for (size_t i = 0; i < json_count(synth); i++) {
        const char *word = json_key_at(synth, i);
        bool known = false;
        for (unsigned int o = 0; o < osc_count && !known; o++) {
            char osc_word[64];
            song_osc_word(osc_word, sizeof(osc_word),
                          synth_osc_uid(synth_osc_return(m->synth, o)));
            known = strcmp(word, osc_word) == 0;
        }
        if (!known)
            log_append_logfile("song: %s:%u: the synth has no \"%s\", "
                               "skipped\n",
                               file, json_line(json_at(synth, i)), word);
    }
}

static void *song_sample_find(SMP_INFO *smp_data, uint32_t uid) {
    for (unsigned int i = 0;; i++) {
        void *smp = smp_sample_return(smp_data, i);
        if (!smp || smp_sample_uid(smp) == uid)
            return smp;
    }
}

static void song_samples_load(const SONG *song, const JSON_VAL *root,
                              const char *file) {
    const JSON_VAL *samples = song_member(root, "samples", JSON_ARRAY, file);
    for (size_t i = 0; i < json_count(samples); i++) {
        const JSON_VAL *entry = json_at(samples, i);
        const char *path = json_as_string(json_get(entry, "path"));
        if (!path) {
            log_append_logfile("song: %s:%u: a sample without a \"path\", "
                               "skipped\n",
                               file, json_line(entry));
            continue;
        }
        uint32_t uid = smp_add(song->m.smp, path, -1);
        void *smp = uid ? song_sample_find(song->m.smp, uid) : NULL;
        if (!smp) {
            log_append_logfile("song: %s:%u: sample %s could not be loaded, "
                               "skipped\n",
                               file, json_line(entry), path);
            continue;
        }
        song_params_read(smp_sample_param_container(smp),
                         song_member(entry, "params", JSON_OBJECT, file), file,
                         path);
    }
}

static void song_ids_add(SONG_IDS *ids, int64_t id, SONG_OWNER owner) {
    for (size_t i = 0; i < ids->count; i++) {
        if (ids->items[i].id == id) {
            ids->items[i].dup = true;
            return;
        }
    }
    ids->items[ids->count++] = (SONG_LOADED){.id = id, .owner = owner};
}

static void *song_lv2_find(PLUG_INFO *plug_data, uint32_t uid) {
    for (unsigned int i = 0;; i++) {
        void *plug = plug_plugin_return(plug_data, i);
        if (!plug || plug_plugin_uid(plug) == uid)
            return plug;
    }
}

static void *song_clap_find(CLAP_PLUG_INFO *plug_data, uint32_t uid) {
    for (unsigned int i = 0;; i++) {
        void *plug = clap_plug_plugin_return(plug_data, i);
        if (!plug || clap_plug_plugin_uid(plug) == uid)
            return plug;
    }
}

static void song_state_failed(const char *file, const JSON_VAL *entry,
                              const char *name) {
    log_append_logfile("song: %s:%u: the state of %s did not load, it is at "
                       "its defaults\n",
                       file, json_line(entry), name);
}

// one plugin of the file, loaded with its state from states_dir ("" for
// none) and added to ids
static void song_plugin_load(const SONG *song, const JSON_VAL *entry,
                             const char *file, const char *states_dir,
                             SONG_IDS *ids) {
    const SONG_MODULES *m = &song->m;
    int64_t id;
    const char *kind = json_as_string(json_get(entry, "kind"));
    if (!json_as_int(json_get(entry, "id"), 1, INT32_MAX, &id) || !kind) {
        log_append_logfile("song: %s:%u: a plugin without an \"id\" from 1 or "
                           "a \"kind\", skipped\n",
                           file, json_line(entry));
        return;
    }
    bool lv2 = strcmp(kind, SONG_PLUGIN_LV2) == 0;
    if (!lv2 && strcmp(kind, SONG_PLUGIN_CLAP) != 0) {
        log_append_logfile("song: %s:%u: unknown plugin kind \"%s\", "
                           "skipped\n",
                           file, json_line(entry), kind);
        return;
    }
    const char *identity =
        json_as_string(json_get(entry, lv2 ? "uri" : "clap_id"));
    void *item = lv2 ? plug_plugin_list_item_by_uri(m->lv2, identity)
                     : clap_plug_plugin_list_item_by_id(m->clap, identity);
    if (!item) {
        log_append_logfile("song: %s:%u: plugin %s is not installed, skipped "
                           "with its edges\n",
                           file, json_line(entry), identity ? identity : "?");
        return;
    }
    uint32_t uid = lv2 ? plug_load_and_activate(item)
                       : clap_plug_load_and_activate(item);
    void *plug = !uid  ? NULL
                 : lv2 ? song_lv2_find(m->lv2, uid)
                       : song_clap_find(m->clap, uid);
    if (!plug) {
        log_append_logfile("song: %s:%u: plugin %s could not be loaded, "
                           "skipped with its edges\n",
                           file, json_line(entry), identity);
        return;
    }
    song_ids_add(ids, id, (SONG_OWNER){lv2 ? m->lv2_tag : m->clap_tag, uid});

    char path[MAX_PATH_STRING];
    if (lv2) {
        if (states_dir[0] == '\0' ||
            !song_state_path(states_dir, id, "", path) ||
            plug_plugin_state_load(plug, path) != 0)
            song_state_failed(file, entry, plug_plugin_name(plug));
        return;
    }
    const JSON_VAL *params = song_member(entry, "params", JSON_OBJECT, file);
    if (params) {
        song_params_read(clap_plug_plugin_param_container(plug), params, file,
                         clap_plug_plugin_name(plug));
        return;
    }
    if (states_dir[0] == '\0' ||
        !song_state_path(states_dir, id, SONG_CLAP_STATE_EXT, path) ||
        clap_plug_state_load(plug, path) != 0)
        song_state_failed(file, entry, clap_plug_plugin_name(plug));
}

// an edge end's owner as the file names it. false (logged) when it is no
// owner of this song
static bool song_owner_read(const SONG *song, const SONG_IDS *ids,
                            const JSON_VAL *v, const char *file,
                            SONG_OWNER *out) {
    const SONG_MODULES *m = &song->m;
    int64_t id;
    if (json_as_int(v, 1, INT64_MAX, &id)) {
        for (size_t i = 0; i < ids->count; i++) {
            if (ids->items[i].id != id)
                continue;
            if (ids->items[i].dup) {
                log_append_logfile("song: %s:%u: two plugins have id %" PRId64
                                   ", the edge skipped\n",
                                   file, json_line(v), id);
                return false;
            }
            *out = ids->items[i].owner;
            return true;
        }
        log_append_logfile("song: %s:%u: no plugin with id %" PRId64
                           " loaded, the edge skipped\n",
                           file, json_line(v), id);
        return false;
    }
    const char *word = json_as_string(v);
    size_t osc_prefix = strlen(SONG_OWNER_SYNTH_OSC);
    if (word && strcmp(word, SONG_OWNER_ROOT) == 0) {
        *out = m->root;
        return true;
    }
    if (word && strcmp(word, SONG_OWNER_SAMPLER) == 0) {
        *out = m->sampler;
        return true;
    }
    if (word && strcmp(word, SONG_OWNER_MIDI_CLOCK) == 0) {
        *out = m->midi_clock;
        return true;
    }
    if (word && strncmp(word, SONG_OWNER_SYNTH_OSC, osc_prefix) == 0 &&
        word[osc_prefix] >= '0' && word[osc_prefix] <= '9') {
        char *end = NULL;
        unsigned long long slot = strtoull(word + osc_prefix, &end, 10);
        if (*end == '\0' && slot <= UINT32_MAX) {
            *out = (SONG_OWNER){m->synth_osc_tag, slot};
            return true;
        }
    }
    log_append_logfile("song: %s:%u: an edge end without a known \"owner\", "
                       "skipped\n",
                       file, json_line(v));
    return false;
}

static void song_edge_load(const SONG *song, const SONG_IDS *ids,
                           const JSON_VAL *edge, const char *file) {
    GRAPH *graph = song->m.graph;
    const JSON_VAL *from = json_get(edge, "from");
    const JSON_VAL *to = json_get(edge, "to");
    const char *from_port = json_as_string(json_get(from, "port"));
    const char *to_port = json_as_string(json_get(to, "port"));
    if (!from_port || !to_port) {
        log_append_logfile("song: %s:%u: an edge without its two ports, "
                           "skipped\n",
                           file, json_line(edge));
        return;
    }
    SONG_OWNER from_owner;
    SONG_OWNER to_owner;
    if (!song_owner_read(song, ids, json_get(from, "owner"), file,
                         &from_owner) ||
        !song_owner_read(song, ids, json_get(to, "owner"), file, &to_owner))
        return;
    uint64_t from_key =
        graph_port_find(graph, from_owner.tag, from_owner.uid, from_port);
    uint64_t to_key = graph_port_find(graph, to_owner.tag, to_owner.uid, to_port);
    if (!from_key || !to_key) {
        log_append_logfile("song: %s:%u: port %s is not there, the edge %s -> "
                           "%s skipped\n",
                           file, json_line(edge), from_key ? to_port : from_port,
                           from_port, to_port);
        return;
    }
    if (graph_connect_keys(graph, from_key, to_key) != 0)
        log_append_logfile("song: %s:%u: %s -> %s could not be connected\n",
                           file, json_line(edge), from_port, to_port);
}

// the exposed port with this short name
static bool song_exposed_find(AUDIO_BACKEND *backend, const char *name,
                              BackendPortInfo *out) {
    size_t count = audio_backend_port_count(backend, BACKEND_PORT_LIST_EXPOSED);
    for (size_t i = 0; i < count; i++) {
        if (audio_backend_port_at(backend, BACKEND_PORT_LIST_EXPOSED, i, out) &&
            strcmp(song_port_short(out), name) == 0)
            return true;
    }
    return false;
}

// another program's port named name that port can link to: opposite flow,
// same type
static bool song_peer_find(AUDIO_BACKEND *backend, const BackendPortInfo *port,
                           const char *name, BackendPortInfo *out) {
    bool want_output = port->flow != PORT_FLOW_OUTPUT;
    BackendPortList list =
        port->type == PORT_TYPE_MIDI
            ? (want_output ? BACKEND_PORT_LIST_OUT_MIDI
                           : BACKEND_PORT_LIST_IN_MIDI)
            : (want_output ? BACKEND_PORT_LIST_OUT_AUDIO
                           : BACKEND_PORT_LIST_IN_AUDIO);
    size_t count = audio_backend_port_count(backend, list);
    for (size_t i = 0; i < count; i++) {
        if (audio_backend_port_at(backend, list, i, out) &&
            strcmp(out->name, name) == 0)
            return true;
    }
    return false;
}

static void song_link_load(const SONG *song, const JSON_VAL *link,
                           const char *file) {
    AUDIO_BACKEND *backend = song->m.backend;
    const char *port_name = json_as_string(json_get(link, "port"));
    const char *peer_name = json_as_string(json_get(link, "peer"));
    if (!port_name || !peer_name) {
        log_append_logfile("song: %s:%u: a link without a \"port\" and a "
                           "\"peer\", skipped\n",
                           file, json_line(link));
        return;
    }
    BackendPortInfo port;
    BackendPortInfo peer;
    if (!song_exposed_find(backend, port_name, &port)) {
        log_append_logfile("song: %s:%u: no exposed port %s, the link "
                           "skipped\n",
                           file, json_line(link), port_name);
        return;
    }
    if (!song_peer_find(backend, &port, peer_name, &peer)) {
        log_append_logfile("song: %s:%u: %s is not there to link %s to, "
                           "skipped\n",
                           file, json_line(link), peer_name, port_name);
        return;
    }
    if (audio_backend_connect_keys(backend, port.key, peer.key) != 0)
        log_append_logfile("song: %s:%u: could not link %s to %s\n", file,
                           json_line(link), port_name, peer_name);
}

// the song in root replaces the one there. dir is its folder
static void song_apply(const SONG *song, const char *dir, const char *file,
                       const JSON_VAL *root) {
    const SONG_MODULES *m = &song->m;
    for (size_t i = 0; i < json_count(root); i++) {
        const char *key = json_key_at(root, i);
        bool known = false;
        for (size_t k = 0; k < sizeof(song_keys) / sizeof(song_keys[0]); k++)
            known = known || strcmp(key, song_keys[k]) == 0;
        if (!known)
            log_append_logfile("song: %s:%u: unknown key \"%s\"\n", file,
                               json_line(json_at(root, i)), key);
    }
    const JSON_VAL *states = json_get(root, "states");
    const char *states_name = json_as_string(states);
    char states_dir[MAX_PATH_STRING] = "";
    if (states_name && song_states_index(states_name) != 0)
        snprintf(states_dir, sizeof(states_dir), "%s/%s", dir, states_name);
    else if (states)
        log_append_logfile("song: %s:%u: \"states\" is not a %s<n> name, the "
                           "plugins load without their states\n",
                           file, json_line(states), SONG_STATES_PREFIX);

    song_clear(song);
    song_params_read(transport_param_container(m->transport),
                     song_member(root, "transport", JSON_OBJECT, file), file,
                     "transport");
    song_synth_load(song, root, file);
    song_samples_load(song, root, file);

    const JSON_VAL *plugins = song_member(root, "plugins", JSON_ARRAY, file);
    size_t plugin_count = json_count(plugins);
    SONG_IDS ids = {0};
    if (plugin_count > 0) {
        ids.items = malloc(plugin_count * sizeof(SONG_LOADED));
        if (!ids.items) {
            log_append_logfile("song: %s: out of memory, no plugins loaded\n",
                               file);
            plugin_count = 0;
        }
    }
    for (size_t i = 0; i < plugin_count; i++)
        song_plugin_load(song, json_at(plugins, i), file, states_dir, &ids);

    const JSON_VAL *edges = song_member(root, "edges", JSON_ARRAY, file);
    for (size_t i = 0; i < json_count(edges); i++)
        song_edge_load(song, &ids, json_at(edges, i), file);
    free(ids.items);
    const JSON_VAL *links = song_member(root, "links", JSON_ARRAY, file);
    for (size_t i = 0; i < json_count(links); i++)
        song_link_load(song, json_at(links, i), file);
}

// reads and checks the song file, then the song in it replaces the one there.
// false (logged) when the file could not be read or is not a song - nothing
// changed then
static bool song_load_file(const SONG *song, const char *dir,
                           const char *file) {
    JSON_ERR err;
    JSON_DOC *doc = json_read_file(file, &err);
    if (!doc) {
        if (err.code == JSON_ERR_PARSE)
            log_append_logfile("song: %s:%u:%u: %s, not loaded\n", file,
                               err.line, err.col, err.msg);
        else
            log_append_logfile("song: %s: %s, not loaded\n", file, err.msg);
        return false;
    }
    const JSON_VAL *root = json_root(doc);
    const char *kind = json_as_string(json_get(root, "kind"));
    int64_t format;
    if (!kind || strcmp(kind, SONG_KIND) != 0) {
        log_append_logfile("song: %s has no \"kind\": \"%s\", not loaded\n",
                           file, SONG_KIND);
        json_doc_free(doc);
        return false;
    }
    if (!json_as_int(json_get(root, "format"), SONG_FORMAT, SONG_FORMAT,
                     &format)) {
        log_append_logfile("song: %s: \"format\" is not %d, not loaded\n",
                           file, SONG_FORMAT);
        json_doc_free(doc);
        return false;
    }
    song_apply(song, dir, file, root);
    json_doc_free(doc);
    log_append_logfile("song: %s loaded\n", file);
    return true;
}

SONG_RESULT song_load(SONG *song, const char *path) {
    if (!song || !path)
        return SONG_INVALID;
    if (path[0] == '\0') {
        log_append_logfile("song: give a song to load\n");
        return SONG_INVALID;
    }
    char dir[MAX_PATH_STRING];
    char file[MAX_PATH_STRING];
    if (!song_path_resolve(song, path, true, dir, file))
        return SONG_INVALID;
    if (!song_load_file(song, dir, file))
        return SONG_FAILED;
    song_current_set(song, dir, file);
    return SONG_OK;
}

void song_new(SONG *song) {
    if (!song)
        return;
    // a save never writes over the template
    song_current_set(song, "", "");
    if (song->template_path[0] != '\0') {
        char dir[MAX_PATH_STRING];
        char file[MAX_PATH_STRING];
        if (song_path_resolve(song, song->template_path, true, dir, file) &&
            song_load_file(song, dir, file))
            return;
        log_append_logfile("song: the template %s did not load, an empty "
                           "song\n",
                           song->template_path);
    }
    song_empty(song);
}

SONG *song_init(const SONG_MODULES *modules, const char *song_dir,
                const char *template_path) {
    if (!modules || !song_dir || !template_path)
        return NULL;
    SONG *song = calloc(1, sizeof(SONG));
    if (!song)
        return NULL;
    song->m = *modules;
    if (!song_fits(snprintf(song->song_dir, sizeof(song->song_dir), "%s",
                            song_dir),
                   sizeof(song->song_dir)) ||
        !song_fits(snprintf(song->template_path, sizeof(song->template_path),
                            "%s", template_path),
                   sizeof(song->template_path))) {
        free(song);
        return NULL;
    }
    return song;
}

void song_clean(SONG *song) { free(song); }
