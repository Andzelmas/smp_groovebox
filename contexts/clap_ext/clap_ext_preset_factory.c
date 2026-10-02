#include "clap_ext_preset_factory.h"
#include "../../types.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// a directory symlink back to an ancestor must not crawl forever
#define PRESET_CRAWL_DEPTH_MAX 32
// segments of a category path: a location, then crawled directories
#define PRESET_PATH_MAX (PRESET_CRAWL_DEPTH_MAX + 2)

typedef struct {
    char *name;
    char *location; // NULL for CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
    uint32_t kind;
} PRESET_LOCATION;

// what one indexing run carries through the provider's callbacks
typedef struct {
    const char *plugin_id;
    CLAP_EXT_PRESET_EMIT emit;
    void *user_data;
    // declared by the current provider during its init()
    PRESET_LOCATION *locations;
    size_t loc_count;
    size_t loc_max;
    char **extensions;
    size_t ext_count;
    size_t ext_max;
    bool any_extension; // a filetype without an extension matches every file
    // category path of what is being read
    const char *path[PRESET_PATH_MAX];
    size_t depth;
    // the file (or plugin location) get_metadata is reading
    const char *location;
    // the preset begin_preset started. Emitted when the next one starts or the
    // file ends, once its plugin ids are known
    bool pending;
    char *pending_name;
    char *pending_key;
    bool pending_ours;    // one of its plugin ids is plugin_id
    bool pending_has_ids; // it named any plugin id at all
} PRESET_INDEXER;

static char *preset_strdup(const char *s) { return s ? strdup(s) : NULL; }

static void preset_pending_drop(PRESET_INDEXER *ix) {
    free(ix->pending_name);
    free(ix->pending_key);
    ix->pending_name = NULL;
    ix->pending_key = NULL;
    ix->pending = false;
}

// a file preset without a name is called after its file
static void preset_file_name(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    snprintf(out, cap, "%s", slash ? slash + 1 : path);
    char *dot = strrchr(out, '.');
    if (dot && dot != out)
        *dot = '\0';
}

// emit the pending preset if it is for plugin_id. One that names no plugin id
// is taken as for every plugin of the library
static void preset_pending_flush(PRESET_INDEXER *ix) {
    if (!ix->pending)
        return;
    if (ix->pending_ours || !ix->pending_has_ids) {
        char file_name[MAX_SHORT_NAME_LENGTH];
        const char *name = ix->pending_name;
        if (!name || name[0] == '\0') {
            preset_file_name(ix->location ? ix->location
                             : ix->pending_key ? ix->pending_key
                                               : "?",
                             file_name, sizeof(file_name));
            name = file_name;
        }
        ix->emit(ix->user_data, ix->path, ix->depth, name, ix->location,
                 ix->pending_key);
    }
    preset_pending_drop(ix);
}

static void preset_meta_on_error(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    int32_t os_error, const char *error_message) {
    (void)os_error;
    PRESET_INDEXER *ix = receiver->receiver_data;
    fprintf(stderr, "preset %s: %s\n",
            ix->location ? ix->location : "(in plugin)",
            error_message ? error_message : "unreadable");
    preset_pending_drop(ix);
}

// called once per preset of the file, a container file has several
static bool preset_meta_begin_preset(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *name, const char *load_key) {
    PRESET_INDEXER *ix = receiver->receiver_data;
    preset_pending_flush(ix);
    ix->pending_name = preset_strdup(name);
    ix->pending_key = preset_strdup(load_key);
    if ((name && !ix->pending_name) || (load_key && !ix->pending_key)) {
        preset_pending_drop(ix);
        return false;
    }
    ix->pending = true;
    ix->pending_ours = false;
    ix->pending_has_ids = false;
    return true;
}

static void preset_meta_add_plugin_id(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const clap_universal_plugin_id_t *plugin_id) {
    PRESET_INDEXER *ix = receiver->receiver_data;
    if (!ix->pending || !plugin_id || !plugin_id->id)
        return;
    ix->pending_has_ids = true;
    if ((!plugin_id->abi || strcmp(plugin_id->abi, "clap") == 0) &&
        strcmp(plugin_id->id, ix->plugin_id) == 0)
        ix->pending_ours = true;
}

static void preset_meta_set_soundpack_id(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *soundpack_id) {
    (void)receiver;
    (void)soundpack_id;
}
static void preset_meta_set_flags(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    uint32_t flags) {
    (void)receiver;
    (void)flags;
}
static void preset_meta_add_creator(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *creator) {
    (void)receiver;
    (void)creator;
}
static void preset_meta_set_description(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *description) {
    (void)receiver;
    (void)description;
}
static void preset_meta_set_timestamps(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    clap_timestamp creation_time, clap_timestamp modification_time) {
    (void)receiver;
    (void)creation_time;
    (void)modification_time;
}
static void preset_meta_add_feature(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *feature) {
    (void)receiver;
    (void)feature;
}
static void preset_meta_add_extra_info(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *key, const char *value) {
    (void)receiver;
    (void)key;
    (void)value;
}

// read the presets of one file, or of a plugin location (location NULL)
static void preset_read(PRESET_INDEXER *ix,
                        const clap_preset_discovery_provider_t *provider,
                        uint32_t kind, const char *location) {
    const clap_preset_discovery_metadata_receiver_t receiver = {
        .receiver_data = ix,
        .on_error = preset_meta_on_error,
        .begin_preset = preset_meta_begin_preset,
        .add_plugin_id = preset_meta_add_plugin_id,
        .set_soundpack_id = preset_meta_set_soundpack_id,
        .set_flags = preset_meta_set_flags,
        .add_creator = preset_meta_add_creator,
        .set_description = preset_meta_set_description,
        .set_timestamps = preset_meta_set_timestamps,
        .add_feature = preset_meta_add_feature,
        .add_extra_info = preset_meta_add_extra_info,
    };
    ix->location = location;
    provider->get_metadata(provider, kind, location, &receiver);
    preset_pending_flush(ix);
    ix->location = NULL;
}

static bool preset_file_matches(const PRESET_INDEXER *ix, const char *name) {
    if (ix->any_extension || ix->ext_count == 0)
        return true;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return false;
    for (size_t i = 0; i < ix->ext_count; i++)
        if (strcasecmp(dot + 1, ix->extensions[i]) == 0)
            return true;
    return false;
}

// one pass over a directory: files are read, sub directories become categories
static void preset_crawl(PRESET_INDEXER *ix,
                         const clap_preset_discovery_provider_t *provider,
                         const char *dir_path, int depth) {
    if (depth > PRESET_CRAWL_DEPTH_MAX)
        return;
    DIR *dir = opendir(dir_path);
    if (!dir)
        return;
    char path[MAX_PATH_STRING];
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        // ".", ".." and hidden files
        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name) >=
            (int)sizeof(path))
            continue;
        unsigned char type = entry->d_type;
        // the file system did not say, or a symlink - ask what it points to
        if (type == DT_UNKNOWN || type == DT_LNK) {
            struct stat st;
            if (stat(path, &st) != 0)
                continue;
            type = S_ISDIR(st.st_mode)   ? DT_DIR
                   : S_ISREG(st.st_mode) ? DT_REG
                                         : DT_UNKNOWN;
        }
        if (type == DT_DIR && ix->depth < PRESET_PATH_MAX) {
            // entry stays valid while the sub directory is read - readdir is
            // not called on this dir until it returns
            ix->path[ix->depth++] = entry->d_name;
            preset_crawl(ix, provider, path, depth + 1);
            ix->depth--;
        } else if (type == DT_REG && preset_file_matches(ix, entry->d_name)) {
            preset_read(ix, provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path);
        }
    }
    closedir(dir);
}

static bool preset_indexer_declare_filetype(
    const struct clap_preset_discovery_indexer *indexer,
    const clap_preset_discovery_filetype_t *filetype) {
    PRESET_INDEXER *ix = indexer->indexer_data;
    if (!filetype)
        return false;
    if (!filetype->file_extension || filetype->file_extension[0] == '\0') {
        ix->any_extension = true;
        return true;
    }
    if (ix->ext_count == ix->ext_max) {
        size_t new_max = ix->ext_max ? ix->ext_max * 2 : 4;
        char **grown = realloc(ix->extensions, new_max * sizeof(char *));
        if (!grown)
            return false;
        ix->extensions = grown;
        ix->ext_max = new_max;
    }
    char *ext = strdup(filetype->file_extension);
    if (!ext)
        return false;
    ix->extensions[ix->ext_count++] = ext;
    return true;
}

static bool preset_indexer_declare_location(
    const struct clap_preset_discovery_indexer *indexer,
    const clap_preset_discovery_location_t *location) {
    PRESET_INDEXER *ix = indexer->indexer_data;
    if (!location)
        return false;
    // a file location needs a path
    if (location->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
        !location->location)
        return false;
    if (ix->loc_count == ix->loc_max) {
        size_t new_max = ix->loc_max ? ix->loc_max * 2 : 4;
        PRESET_LOCATION *grown =
            realloc(ix->locations, new_max * sizeof(PRESET_LOCATION));
        if (!grown)
            return false;
        ix->locations = grown;
        ix->loc_max = new_max;
    }
    PRESET_LOCATION *loc = &ix->locations[ix->loc_count];
    loc->name = strdup(location->name && location->name[0] ? location->name
                                                            : "Presets");
    loc->location = location->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE
                        ? strdup(location->location)
                        : NULL;
    loc->kind = location->kind;
    if (!loc->name || (location->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
                       !loc->location)) {
        free(loc->name);
        free(loc->location);
        return false;
    }
    ix->loc_count++;
    return true;
}

static bool preset_indexer_declare_soundpack(
    const struct clap_preset_discovery_indexer *indexer,
    const clap_preset_discovery_soundpack_t *soundpack) {
    (void)indexer;
    (void)soundpack;
    return true;
}

static const void *preset_indexer_get_extension(
    const struct clap_preset_discovery_indexer *indexer,
    const char *extension_id) {
    (void)indexer;
    (void)extension_id;
    return NULL;
}

// forget the current provider's declarations
static void preset_declarations_clear(PRESET_INDEXER *ix) {
    for (size_t i = 0; i < ix->loc_count; i++) {
        free(ix->locations[i].name);
        free(ix->locations[i].location);
    }
    ix->loc_count = 0;
    for (size_t i = 0; i < ix->ext_count; i++)
        free(ix->extensions[i]);
    ix->ext_count = 0;
    ix->any_extension = false;
}

static void preset_location_index(PRESET_INDEXER *ix,
                                  const clap_preset_discovery_provider_t *prov,
                                  const PRESET_LOCATION *loc) {
    if (loc->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
        preset_read(ix, prov, loc->kind, NULL);
        return;
    }
    struct stat st;
    if (stat(loc->location, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode))
        preset_crawl(ix, prov, loc->location, 0);
    else
        preset_read(ix, prov, loc->kind, loc->location);
}

int clap_ext_preset_index(const clap_plugin_entry_t *plug_entry,
                          const char *plugin_id, const char *indexer_name,
                          CLAP_EXT_PRESET_EMIT emit, void *user_data) {
    if (!plug_entry || !plugin_id || !emit)
        return -1;
    const clap_preset_discovery_factory_t *factory =
        plug_entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
    if (!factory)
        factory = plug_entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT);
    if (!factory)
        return -1;

    PRESET_INDEXER ix = {
        .plugin_id = plugin_id, .emit = emit, .user_data = user_data};
    const clap_preset_discovery_indexer_t indexer = {
        .clap_version = CLAP_VERSION,
        .name = indexer_name ? indexer_name : "indexer",
        .indexer_data = &ix,
        .declare_filetype = preset_indexer_declare_filetype,
        .declare_location = preset_indexer_declare_location,
        .declare_soundpack = preset_indexer_declare_soundpack,
        .get_extension = preset_indexer_get_extension,
    };
    uint32_t provider_count = factory->count(factory);
    for (uint32_t i = 0; i < provider_count; i++) {
        const clap_preset_discovery_provider_descriptor_t *desc =
            factory->get_descriptor(factory, i);
        if (!desc)
            continue;
        const clap_preset_discovery_provider_t *provider =
            factory->create(factory, &indexer, desc->id);
        if (!provider)
            continue;
        // init() is where the provider declares its filetypes and locations
        if (provider->init(provider)) {
            // a lone location needs no category of its own
            bool lone = provider_count == 1 && ix.loc_count == 1;
            for (size_t l = 0; l < ix.loc_count; l++) {
                ix.depth = 0;
                if (!lone)
                    ix.path[ix.depth++] = ix.locations[l].name;
                preset_location_index(&ix, provider, &ix.locations[l]);
            }
        }
        provider->destroy(provider);
        preset_declarations_clear(&ix);
    }
    free(ix.locations);
    free(ix.extensions);
    return 0;
}
