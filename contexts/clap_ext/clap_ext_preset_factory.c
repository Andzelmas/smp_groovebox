#include "clap_ext_preset_factory.h"
#include "../../types.h"
#include "../../util_funcs/log_funcs.h"
#include "../../util_funcs/path_funcs.h"
#include "../../util_funcs/string_funcs.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// TODO redo so the realloc is not used so often
// Also check if realloc shrinks the array,
// because realloc can refuse to shrink anyway.

typedef struct _clap_ext_preset_container {
    char *name;
    char *location;
    char *load_key;
    uint32_t loc_kind; // the same location kind as the one on the
                       // CLAP_EXT_PRESET_SINGLE_LOC, for convenience
    char plugin_id[MAX_UNIQUE_ID_STRING]; // unique plugin_id used to match if
                                          // the preset container can be used
                                          // for the requested plugin
} CLAP_EXT_PRESET_CONTAINER;

typedef struct _clap_ext_preset_dirs {
    // TODO should calloc this to prevent losing of really long paths and to
    // save memory?
    char dir_path[MAX_PATH_STRING];
    CLAP_EXT_PRESET_CONTAINER *preset_container;
    uint32_t preset_container_count;
} CLAP_EXT_PRESET_DIRS;

typedef struct _clap_ext_preset_single_loc {
    char *loc_name;
    uint32_t loc_kind;
    char *loc_location;
    // if the loc_kind is file and loc_location is the root directory
    // preset_containers will be empty this array will be filled with the
    // CLAP_EXT_PRESET_DIRS structs. dir_path is the directory where the
    // preset_container are located
    CLAP_EXT_PRESET_DIRS *preset_dirs;
    uint32_t preset_dirs_count;
    // if the loc_kind is plugin, or loc_location is a file preset_dirs will be
    // empty and this array populated
    CLAP_EXT_PRESET_CONTAINER *preset_containers;
    uint32_t preset_containers_count;
    CLAP_EXT_PRESET_USER_FUNCS user_funcs; // user functions are copied to each
                                           // location for convenience
} CLAP_EXT_PRESET_SINGLE_LOC;

typedef struct _clap_ext_preset_factory {
    CLAP_EXT_PRESET_SINGLE_LOC *clap_ext_locations;
    uint32_t loc_count;
    char **file_extensions;
    uint32_t fileextensions_count;
    CLAP_EXT_PRESET_USER_FUNCS user_funcs;
} CLAP_EXT_PRESET_FACTORY;

static bool clap_ext_preset_indexer_declare_filetype(
    const struct clap_preset_discovery_indexer *indexer,
    const clap_preset_discovery_filetype_t *filetype) {
    if (!indexer)
        return false;
    CLAP_EXT_PRESET_FACTORY *ext_preset_fac =
        (CLAP_EXT_PRESET_FACTORY *)indexer->indexer_data;
    // TODO right now only interested in file extension, filetype description
    // and name is not saved
    ext_preset_fac->fileextensions_count += 1;
    uint32_t cur_item = ext_preset_fac->fileextensions_count - 1;
    char **temp_extensions =
        realloc(ext_preset_fac->file_extensions,
                sizeof(char *) * ext_preset_fac->fileextensions_count);
    if (!temp_extensions) {
        ext_preset_fac->fileextensions_count -= 1;
        return false;
    }
    ext_preset_fac->file_extensions = temp_extensions;
    ext_preset_fac->file_extensions[cur_item] = NULL;
    char *cur_extension = NULL;
    if (filetype->file_extension) {
        cur_extension =
            calloc(strlen(filetype->file_extension) + 1, sizeof(char));
        if (cur_extension) {
            snprintf(cur_extension, strlen(filetype->file_extension) + 1, "%s",
                     filetype->file_extension);
            ext_preset_fac->file_extensions[cur_item] = cur_extension;
        }
    }
    return true;
}
static bool clap_ext_preset_indexer_declare_location(
    const struct clap_preset_discovery_indexer *indexer,
    const clap_preset_discovery_location_t *location) {
    CLAP_EXT_PRESET_FACTORY *ext_preset_fac =
        (CLAP_EXT_PRESET_FACTORY *)indexer->indexer_data;
    if (!location)
        return false;
    ext_preset_fac->loc_count += 1;
    uint32_t cur_item = ext_preset_fac->loc_count - 1;
    CLAP_EXT_PRESET_SINGLE_LOC *temp_locations =
        realloc(ext_preset_fac->clap_ext_locations,
                sizeof(CLAP_EXT_PRESET_SINGLE_LOC) * ext_preset_fac->loc_count);
    if (!temp_locations) {
        ext_preset_fac->loc_count -= 1;
        return false;
    }
    ext_preset_fac->clap_ext_locations = temp_locations;

    CLAP_EXT_PRESET_SINGLE_LOC *cur_loc =
        &(ext_preset_fac->clap_ext_locations[cur_item]);
    cur_loc->loc_kind = location->kind;
    cur_loc->loc_location = NULL;
    cur_loc->loc_name = NULL;
    cur_loc->preset_dirs = NULL;
    cur_loc->preset_dirs_count = 0;
    cur_loc->preset_containers = NULL;
    cur_loc->preset_containers_count = 0;
    cur_loc->user_funcs = ext_preset_fac->user_funcs;

    if (location->location) {
        cur_loc->loc_location =
            calloc(strlen(location->location) + 1, sizeof(char));
        if (cur_loc->loc_location)
            snprintf(cur_loc->loc_location, strlen(location->location) + 1,
                     "%s", location->location);
    }
    if (location->name) {
        cur_loc->loc_name = calloc(strlen(location->name) + 1, sizeof(char));
        if (cur_loc->loc_name)
            snprintf(cur_loc->loc_name, strlen(location->name) + 1, "%s",
                     location->name);
    }
    return true;
}
static bool clap_ext_preset_indexer_declare_soundpack(
    const struct clap_preset_discovery_indexer *indexer,
    const clap_preset_discovery_soundpack_t *soundpack) {
    // TODO soundpacks are not used as of now
    return true;
}
static const void *clap_ext_preset_indexer_get_extension(
    const struct clap_preset_discovery_indexer *indexer,
    const char *extension_id) {
    // TODO not checking preset extensions now
    return NULL;
}

static void
clap_ext_preset_container_clean(CLAP_EXT_PRESET_CONTAINER *preset_container) {
    if (!preset_container)
        return;
    if (preset_container->name)
        free(preset_container->name);
    if (preset_container->load_key)
        free(preset_container->load_key);
    if (preset_container->location)
        free(preset_container->location);
}
static void clap_ext_single_loc_clean(CLAP_EXT_PRESET_SINGLE_LOC *single_loc) {
    if (!single_loc)
        return;
    if (single_loc->loc_location)
        free(single_loc->loc_location);
    if (single_loc->loc_name)
        free(single_loc->loc_name);
    if (single_loc->preset_containers) {
        for (uint32_t i = 0; i < single_loc->preset_containers_count; i++) {
            clap_ext_preset_container_clean(
                &(single_loc->preset_containers[i]));
        }
        free(single_loc->preset_containers);
    }
    single_loc->preset_containers_count = 0;
    if (single_loc->preset_dirs) {
        for (uint32_t i = 0; i < single_loc->preset_dirs_count; i++) {
            CLAP_EXT_PRESET_DIRS *cur_dir = &(single_loc->preset_dirs[i]);
            if (cur_dir->preset_container) {
                for (uint32_t j = 0; j < cur_dir->preset_container_count; j++) {
                    clap_ext_preset_container_clean(
                        &(cur_dir->preset_container[j]));
                }
                cur_dir->preset_container_count = 0;
                free(cur_dir->preset_container);
            }
        }
        free(single_loc->preset_dirs);
    }
    single_loc->preset_dirs_count = 0;
}
void clap_ext_preset_clean(CLAP_EXT_PRESET_FACTORY *clap_ext_preset_data) {
    if (!clap_ext_preset_data)
        return;
    if (clap_ext_preset_data->clap_ext_locations) {
        for (uint32_t i = 0; i < clap_ext_preset_data->loc_count; i++) {
            clap_ext_single_loc_clean(
                &(clap_ext_preset_data->clap_ext_locations[i]));
        }
        free(clap_ext_preset_data->clap_ext_locations);
    }
    clap_ext_preset_data->loc_count = 0;

    if (clap_ext_preset_data->file_extensions) {
        for (uint32_t i = 0; i < clap_ext_preset_data->fileextensions_count;
             i++) {
            char *cur_filextension = clap_ext_preset_data->file_extensions[i];
            if (cur_filextension)
                free(cur_filextension);
        }
        free(clap_ext_preset_data->file_extensions);
    }
    clap_ext_preset_data->fileextensions_count = 0;

    free(clap_ext_preset_data);
}

static void clap_ext_preset_meta_on_error(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    int32_t os_error, const char *error_message) {
    CLAP_EXT_PRESET_SINGLE_LOC *cur_loc =
        (CLAP_EXT_PRESET_SINGLE_LOC *)receiver->receiver_data;
    if (!cur_loc)
        return;

    if (!(cur_loc->user_funcs.ext_preset_send_msg))
        return;
    cur_loc->user_funcs.ext_preset_send_msg(cur_loc->user_funcs.user_data,
                                            error_message);
    cur_loc->user_funcs.ext_preset_send_msg(cur_loc->user_funcs.user_data,
                                            "\n");
    // prevent meta_begin_preset from forming this preset if the get_metada func
    // is called for each preset file
    if (cur_loc->preset_dirs) {
        CLAP_EXT_PRESET_DIRS *cur_dir =
            &(cur_loc->preset_dirs[cur_loc->preset_dirs_count - 1]);
        CLAP_EXT_PRESET_CONTAINER *cur_container =
            &(cur_dir->preset_container[cur_dir->preset_container_count - 1]);
        if (cur_container->location)
            free(cur_container->location);
        cur_container->location = NULL;
    }
}
static bool clap_ext_preset_meta_begin_preset(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *name, const char *load_key) {
    if (!receiver)
        return false;
    CLAP_EXT_PRESET_SINGLE_LOC *cur_loc =
        (CLAP_EXT_PRESET_SINGLE_LOC *)receiver->receiver_data;
    if (!cur_loc)
        return false;
    // location kind will be the same for all the preset containers, because it
    // comes from the CLAP_EXT_PRESET_SINGLE_LOC
    uint32_t location_kind = cur_loc->loc_kind;
    CLAP_EXT_PRESET_CONTAINER *cur_container = NULL;
    // if preset_dirs are empty it means the loc_kind is plugin or file but the
    // cur_loc->location is a filepath
    if (cur_loc->preset_dirs == NULL) {
        cur_loc->preset_containers_count += 1;
        CLAP_EXT_PRESET_CONTAINER *temp_array = realloc(
            cur_loc->preset_containers, sizeof(CLAP_EXT_PRESET_CONTAINER) *
                                            cur_loc->preset_containers_count);
        if (!temp_array) {
            cur_loc->preset_containers_count -= 1;
            return false;
        }

        cur_loc->preset_containers = temp_array;
        uint32_t cur_item = cur_loc->preset_containers_count - 1;

        cur_container = &(cur_loc->preset_containers[cur_item]);
        if (cur_loc->loc_location) {
            cur_container->location =
                calloc(strlen(cur_loc->loc_location) + 1, sizeof(char));
            if (cur_container->location)
                snprintf(cur_container->location,
                         strlen(cur_loc->loc_location) + 1, "%s",
                         cur_loc->loc_location);
        }
    }
    // if preset_dirs are not empty it means get_metada is called for each
    // preset file, so the cur_container->location is already filled with the
    // full path of the file
    if (cur_loc->preset_dirs != NULL) {
        CLAP_EXT_PRESET_DIRS *cur_dir =
            &(cur_loc->preset_dirs[cur_loc->preset_dirs_count - 1]);
        cur_container =
            &(cur_dir->preset_container[cur_dir->preset_container_count - 1]);
        if (!cur_container->location)
            return false;
    }
    if (!cur_container)
        return false;

    cur_container->load_key = NULL;
    cur_container->loc_kind = location_kind;
    cur_container->name = NULL;

    if (name) {
        cur_container->name = calloc(strlen(name) + 1, sizeof(char));
        if (cur_container->name)
            snprintf(cur_container->name, strlen(name) + 1, "%s", name);
    }
    if (load_key) {
        cur_container->load_key = calloc(strlen(load_key) + 1, sizeof(char));
        if (cur_container->load_key)
            snprintf(cur_container->load_key, strlen(load_key) + 1, "%s",
                     load_key);
    }
    return true;
}
static void clap_ext_preset_meta_add_plugin_id(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const clap_universal_plugin_id_t *plugin_id) {
    if (!receiver)
        return;
    CLAP_EXT_PRESET_SINGLE_LOC *cur_loc =
        (CLAP_EXT_PRESET_SINGLE_LOC *)receiver->receiver_data;
    if (!cur_loc)
        return;

    if (!plugin_id->id)
        return;
    CLAP_EXT_PRESET_CONTAINER *cur_container = NULL;
    if (cur_loc->preset_dirs == NULL) {
        uint32_t cur_item = cur_loc->preset_containers_count - 1;
        cur_container = &(cur_loc->preset_containers[cur_item]);
    }
    if (cur_loc->preset_dirs != NULL) {
        CLAP_EXT_PRESET_DIRS *cur_preset_dir =
            &(cur_loc->preset_dirs[cur_loc->preset_dirs_count - 1]);
        cur_container = &(
            cur_preset_dir
                ->preset_container[cur_preset_dir->preset_container_count - 1]);
    }
    if (!cur_container)
        return;
    snprintf(cur_container->plugin_id, MAX_UNIQUE_ID_STRING, "%s",
             plugin_id->id);
}
static void clap_ext_preset_meta_set_soundpack_id(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *soundpack_id) {
    // TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_set_flags(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    uint32_t flags) {
    // TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_add_creator(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *creator) {
    // TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_set_description(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *description) {
    // TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_set_timestamps(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    clap_timestamp creation_time, clap_timestamp modification_time) {
    // TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_add_feature(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *feature) {
    // TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_add_extra_info(
    const struct clap_preset_discovery_metadata_receiver *receiver,
    const char *key, const char *value) {
    // TODO not used at this moment
    return;
}
static void clap_ext_preset_container_create_for_file(
    CLAP_EXT_PRESET_FACTORY *preset_data, CLAP_EXT_PRESET_SINGLE_LOC *cur_loc,
    const char *filepath,
    const clap_preset_discovery_metadata_receiver_t *metada_rec,
    const clap_preset_discovery_provider_t *preset_discovery) {
    if (!preset_data)
        return;
    if (!cur_loc)
        return;
    uint32_t cur_preset_dir_num = cur_loc->preset_dirs_count - 1;
    CLAP_EXT_PRESET_DIRS *cur_preset_dir =
        &(cur_loc->preset_dirs[cur_preset_dir_num]);
    cur_preset_dir->preset_container_count += 1;
    CLAP_EXT_PRESET_CONTAINER *temp_array =
        realloc(cur_preset_dir->preset_container,
                sizeof(CLAP_EXT_PRESET_CONTAINER) *
                    cur_preset_dir->preset_container_count);
    if (!temp_array) {
        cur_preset_dir->preset_container_count -= 1;
        return;
    }
    cur_preset_dir->preset_container = temp_array;

    uint32_t cur_container_num = cur_preset_dir->preset_container_count - 1;
    CLAP_EXT_PRESET_CONTAINER *cur_container =
        &(cur_preset_dir->preset_container[cur_container_num]);
    cur_container->load_key = NULL;
    cur_container->loc_kind = cur_loc->loc_kind;
    cur_container->location = NULL;
    cur_container->name = NULL;
    cur_container->location = calloc(strlen(filepath) + 1, sizeof(char));
    if (cur_container->location)
        snprintf(cur_container->location, strlen(filepath) + 1, "%s", filepath);
    preset_discovery->get_metadata(preset_discovery, cur_loc->loc_kind,
                                   cur_container->location, metada_rec);
}
static void clap_ext_preset_location_crawl(
    CLAP_EXT_PRESET_FACTORY *preset_data, CLAP_EXT_PRESET_SINGLE_LOC *cur_loc,
    char *location_path,
    const clap_preset_discovery_metadata_receiver_t *metada_rec,
    const clap_preset_discovery_provider_t *preset_discovery) {
    if (!preset_data)
        ;
    if (!cur_loc)
        return;
    if (!location_path)
        return;
    if (path_is_directory(location_path) != 1)
        return;
    // check if there are legit files in the location_path
    int found_files = path_has_dir(location_path, preset_data->file_extensions,
                                   preset_data->fileextensions_count, 0);

    // if found crawl the location_path for files and create preset_container
    // for each file that maches the preset filetype
    if (found_files == 1) {
        cur_loc->preset_dirs_count += 1;
        CLAP_EXT_PRESET_DIRS *temp_array =
            realloc(cur_loc->preset_dirs,
                    sizeof(CLAP_EXT_PRESET_DIRS) * cur_loc->preset_dirs_count);
        if (!temp_array) {
            cur_loc->preset_dirs_count -= 1;
        }
        if (temp_array) {
            cur_loc->preset_dirs = temp_array;
            uint32_t cur_preset_dir_num = cur_loc->preset_dirs_count - 1;
            CLAP_EXT_PRESET_DIRS *cur_preset_dir =
                &(cur_loc->preset_dirs[cur_preset_dir_num]);
            cur_preset_dir->preset_container = NULL;
            cur_preset_dir->preset_container_count = 0;
            snprintf(cur_preset_dir->dir_path, MAX_PATH_STRING, "%s",
                     location_path);
            // go through files and create the preset_containers
            struct dirent *d;
            DIR *dir = opendir(location_path);
            if (!dir)
                return;
            d = readdir(dir);
            int found = 0;
            while (d != NULL) {
                char full_path[MAX_PATH_STRING];
                snprintf(full_path, MAX_PATH_STRING, "%s/%s", location_path,
                         d->d_name);
                if (path_is_directory(full_path) == 1) {
                    d = readdir(dir);
                    continue;
                }
                if (preset_data->file_extensions == NULL) {
                    clap_ext_preset_container_create_for_file(
                        preset_data, cur_loc, full_path, metada_rec,
                        preset_discovery);
                    d = readdir(dir);
                    continue;
                }
                for (uint32_t i = 0; i < preset_data->fileextensions_count;
                     i++) {
                    char *cur_ext = preset_data->file_extensions[i];
                    if (path_extension_matches(full_path, cur_ext) == 1) {
                        clap_ext_preset_container_create_for_file(
                            preset_data, cur_loc, full_path, metada_rec,
                            preset_discovery);
                    }
                }
                d = readdir(dir);
            }
            closedir(dir);
        }
    }
    // check if there are legit directories in the location_path
    int found_dirs = path_has_dir(location_path, NULL, 0, 1);
    if (found_dirs != 1)
        return;

    // if found call this function for these directories
    struct dirent *d;
    DIR *dir = opendir(location_path);
    if (!dir)
        return;
    d = readdir(dir);
    int found = 0;
    while (d != NULL) {
        if (strcmp(d->d_name, ".") == 0) {
            d = readdir(dir);
            continue;
        }
        if (strcmp(d->d_name, "..") == 0) {
            d = readdir(dir);
            continue;
        }
        char full_path[MAX_PATH_STRING];
        snprintf(full_path, MAX_PATH_STRING, "%s/%s", location_path, d->d_name);
        clap_ext_preset_location_crawl(preset_data, cur_loc, full_path,
                                       metada_rec, preset_discovery);
        d = readdir(dir);
    }
    closedir(dir);
}
CLAP_EXT_PRESET_FACTORY *
clap_ext_preset_init(const clap_plugin_entry_t *plug_entry,
                     clap_host_t clap_host_info,
                     CLAP_EXT_PRESET_USER_FUNCS user_funcs) {
    CLAP_EXT_PRESET_FACTORY *clap_ext_preset_data =
        calloc(1, sizeof(CLAP_EXT_PRESET_FACTORY));
    if (!clap_ext_preset_data)
        return NULL;
    clap_ext_preset_data->loc_count = 0;
    clap_ext_preset_data->file_extensions = NULL;
    clap_ext_preset_data->fileextensions_count = 0;
    clap_ext_preset_data->clap_ext_locations =
        calloc(1, sizeof(CLAP_EXT_PRESET_SINGLE_LOC));
    if (!clap_ext_preset_data->clap_ext_locations) {
        clap_ext_preset_clean(clap_ext_preset_data);
        return NULL;
    }
    clap_ext_preset_data->file_extensions = calloc(1, sizeof(char *));
    if (!clap_ext_preset_data->file_extensions) {
        clap_ext_preset_clean(clap_ext_preset_data);
        return NULL;
    }
    // preset-discovery factory
    const clap_preset_discovery_factory_t *preset_fac =
        plug_entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
    if (!preset_fac) {
        clap_ext_preset_clean(clap_ext_preset_data);
        return NULL;
    }
    // copy the struct containing the user functions (send message and similar)
    clap_ext_preset_data->user_funcs = user_funcs;
    // all the locations from all the providers will be put into the single
    // CLAP_EXT_PRESET_FACTORY struct preset containers from the metada will be
    // put into the CLAP_EXT_PRESET_SINGLE_LOC->preset_containers
    uint32_t provider_count = preset_fac->count(preset_fac);
    uint32_t curr_idx_from = 0;
    for (uint32_t i = 0; i < provider_count; i++) {
        const clap_preset_discovery_provider_descriptor_t *preset_desc =
            preset_fac->get_descriptor(preset_fac, i);
        if (!preset_desc)
            continue;
        clap_preset_discovery_indexer_t preset_indexer;
        preset_indexer.clap_version = clap_host_info.clap_version;
        preset_indexer.declare_filetype =
            clap_ext_preset_indexer_declare_filetype;
        preset_indexer.declare_location =
            clap_ext_preset_indexer_declare_location;
        preset_indexer.declare_soundpack =
            clap_ext_preset_indexer_declare_soundpack;
        preset_indexer.get_extension = clap_ext_preset_indexer_get_extension;
        preset_indexer.indexer_data = (void *)clap_ext_preset_data;
        preset_indexer.name = clap_host_info.name;
        preset_indexer.vendor = clap_host_info.vendor;
        preset_indexer.url = clap_host_info.url;
        preset_indexer.version = clap_host_info.version;
        const clap_preset_discovery_provider_t *preset_discovery =
            preset_fac->create(preset_fac, &preset_indexer, preset_desc->id);
        if (!preset_discovery)
            continue;
        if (!preset_discovery->init(preset_discovery))
            continue;
        // get the metada for each location and fill out the
        // CLAP_EXT_PRESET_SINGLE_LOC->preset_containers
        uint32_t loc_idx = curr_idx_from;
        for (; loc_idx < clap_ext_preset_data->loc_count; loc_idx++) {
            CLAP_EXT_PRESET_SINGLE_LOC *cur_loc =
                &(clap_ext_preset_data->clap_ext_locations[loc_idx]);
            if (!cur_loc->loc_name)
                continue;
            cur_loc->preset_containers_count = 0;
            // fill the preset container array of the current preset location
            clap_preset_discovery_metadata_receiver_t metada_rec;
            metada_rec.receiver_data = (void *)cur_loc;
            metada_rec.on_error = clap_ext_preset_meta_on_error;
            metada_rec.begin_preset = clap_ext_preset_meta_begin_preset;
            metada_rec.add_plugin_id = clap_ext_preset_meta_add_plugin_id;
            metada_rec.set_soundpack_id = clap_ext_preset_meta_set_soundpack_id;
            metada_rec.set_flags = clap_ext_preset_meta_set_flags;
            metada_rec.add_creator = clap_ext_preset_meta_add_creator;
            metada_rec.set_description = clap_ext_preset_meta_set_description;
            metada_rec.set_timestamps = clap_ext_preset_meta_set_timestamps;
            metada_rec.add_feature = clap_ext_preset_meta_add_feature;
            metada_rec.add_extra_info = clap_ext_preset_meta_add_extra_info;
            // if the loc_kind is file and loc_location is a directory, crawl
            // through each directory creating preset_dirs on cur_loc create the
            // preset_dirs only if the directory is not empty and files inside
            // match the clap_ext_preset_data->file_extensions or all files if
            // it is NULL
            if (cur_loc->loc_kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
                path_is_directory(cur_loc->loc_location) == 1) {
                clap_ext_preset_location_crawl(clap_ext_preset_data, cur_loc,
                                               cur_loc->loc_location,
                                               &metada_rec, preset_discovery);
                continue;
            }
            // if the loc_location is not a file or loc_kind is plugin we will
            // fill the preset_containers array in the _begin_preset function
            cur_loc->preset_containers =
                calloc(1, sizeof(CLAP_EXT_PRESET_CONTAINER));
            if (!cur_loc->preset_containers)
                continue;
            // if the location kind is plugin, the plugin preset containers are
            // inside the plugin, no need to crawl through the location
            if (cur_loc->loc_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
                preset_discovery->get_metadata(
                    preset_discovery, cur_loc->loc_kind, cur_loc->loc_location,
                    &metada_rec);
                continue;
            }
            // if the location kind is a file but the location itself is not a
            // directory (hopefully a file), don't crawl - the plugin should
            // return presets from the single file
            if (cur_loc->loc_kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
                path_is_directory(cur_loc->loc_location) == 0) {
                preset_discovery->get_metadata(
                    preset_discovery, cur_loc->loc_kind, cur_loc->loc_location,
                    &metada_rec);
                continue;
            }
        }

        curr_idx_from = loc_idx + 1;

        preset_discovery->destroy(preset_discovery);
    }

    return clap_ext_preset_data;
}

static int clap_ext_preset_container_info_return(
    CLAP_EXT_PRESET_CONTAINER *cur_preset, uint32_t *loc_kind, char *load_key,
    uint32_t load_key_len, char *name, uint32_t name_len, char *path,
    uint32_t path_len) {
    if (!cur_preset)
        return -1;
    if (loc_kind)
        *loc_kind = cur_preset->loc_kind;
    if (cur_preset->location && path)
        snprintf(path, path_len, "%s", cur_preset->location);
    if (cur_preset->name && name)
        snprintf(name, name_len, "%s", cur_preset->name);
    if (cur_preset->load_key && load_key)
        snprintf(load_key, load_key_len, "%s", cur_preset->load_key);

    return 1;
}
int clap_ext_preset_info_return(CLAP_EXT_PRESET_FACTORY *preset_fac,
                                char *plug_id, uint32_t idx,
                                const char *preset_path, uint32_t *loc_kind,
                                char *load_key, uint32_t load_key_len,
                                char *name, uint32_t name_len, char *path,
                                uint32_t path_len, char *categories,
                                uint32_t categories_len) {
    if (!preset_fac)
        return -1;
    if (preset_fac->loc_count <= 0)
        return -1;
    uint32_t iter = 0;
    uint32_t count = 0;
    for (uint32_t loc_id = 0; loc_id < preset_fac->loc_count; loc_id++) {
        CLAP_EXT_PRESET_SINGLE_LOC *cur_loc =
            &(preset_fac->clap_ext_locations[loc_id]);
        // if the preset containers are in separate directories
        if (cur_loc->preset_dirs) {
            for (uint32_t dir_id = 0; dir_id < cur_loc->preset_dirs_count;
                 dir_id++) {
                CLAP_EXT_PRESET_DIRS *cur_dir = &(cur_loc->preset_dirs[dir_id]);
                iter = count;
                count += cur_dir->preset_container_count;
                if (count <= idx)
                    continue;
                for (uint32_t preset_id = 0;
                     preset_id < cur_dir->preset_container_count; preset_id++) {
                    CLAP_EXT_PRESET_CONTAINER *cur_preset =
                        &(cur_dir->preset_container[preset_id]);
                    if (iter != idx && !preset_path) {
                        iter += 1;
                        continue;
                    }
                    if (!cur_preset->location)
                        continue;
                    if (preset_path) {
                        if (strcmp(cur_preset->location, preset_path) != 0)
                            continue;
                    }

                    if (categories) {
                        char *after_delim = NULL;
                        char *before_delim = str_split_string_delim(
                            cur_dir->dir_path, cur_loc->loc_location,
                            &after_delim);
                        if (after_delim)
                            snprintf(categories, categories_len, "%s%s",
                                     cur_loc->loc_name, after_delim);
                        else
                            snprintf(categories, categories_len, "%s",
                                     cur_loc->loc_name);
                        if (after_delim)
                            free(after_delim);
                        if (before_delim)
                            free(before_delim);
                    }
                    // only return presets that have the same unique plugin_id
                    // string
                    if (strcmp(cur_preset->plugin_id, plug_id) != 0)
                        continue;
                    return clap_ext_preset_container_info_return(
                        cur_preset, loc_kind, load_key, load_key_len, name,
                        name_len, path, path_len);
                }
            }
        }
        // if the preset containers are stored on the cur_loc directly, not in
        // separate directories
        if (!cur_loc->preset_dirs && cur_loc->preset_containers) {
            for (uint32_t preset_id = 0;
                 preset_id < cur_loc->preset_containers_count; preset_id++) {
                CLAP_EXT_PRESET_CONTAINER *cur_preset =
                    &(cur_loc->preset_containers[preset_id]);
                if (iter != idx && !preset_path) {
                    iter += 1;
                    continue;
                }
                if (preset_path) {
                    if (strcmp(cur_preset->location, preset_path) != 0)
                        continue;
                }
                if (cur_loc->loc_name && categories)
                    snprintf(categories, categories_len, "%s",
                             cur_loc->loc_name);
                // only return presets that have the same unique plugin_id
                // string
                if (strcmp(cur_preset->plugin_id, plug_id) != 0)
                    continue;
                return clap_ext_preset_container_info_return(
                    cur_preset, loc_kind, load_key, load_key_len, name,
                    name_len, path, path_len);
            }
        }
    }
    return 0;
}

uint32_t clap_ext_preset_location_count(CLAP_EXT_PRESET_FACTORY *preset_fac) {
    if (!preset_fac)
        return 0;
    return preset_fac->loc_count;
}
void clap_ext_preset_location_name(CLAP_EXT_PRESET_FACTORY *preset_fac,
                                   uint32_t loc_idx, char *return_name,
                                   uint32_t name_len) {
    if (!preset_fac)
        return;
    if (loc_idx >= preset_fac->loc_count)
        return;
    CLAP_EXT_PRESET_SINGLE_LOC *single_location =
        &(preset_fac->clap_ext_locations[loc_idx]);
    if (!single_location)
        return;
    if (!single_location->loc_name)
        return;
    snprintf(return_name, name_len, "%s", single_location->loc_name);
}
