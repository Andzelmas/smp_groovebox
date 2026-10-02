#pragma once
#include <clap/clap.h>
#include <stddef.h>

// Lists the presets of a plugin's preset-discovery factory. It runs plugin
// code, so only the clap scanner process (clap_scan.c) uses it - a plugin that
// misbehaves while indexing cannot reach the app.

// one preset, in discovery order. path is its category path, depth segments:
// the location (left out when the factory has only that one), then the sub
// directories of a crawled one. location is NULL for a preset inside the
// plugin, load_key NULL for a whole file preset. All valid for the call only
typedef void (*CLAP_EXT_PRESET_EMIT)(void *user_data, const char *const *path,
                                     size_t depth, const char *name,
                                     const char *location,
                                     const char *load_key);

// emit every preset the factory lists for plugin_id (a descriptor id). A
// preset naming no plugin id counts as for every plugin of the library.
// indexer_name is how the indexer introduces itself to the factory.
// -1 when the plugin has no preset-discovery factory, 0 otherwise
int clap_ext_preset_index(const clap_plugin_entry_t *plug_entry,
                          const char *plugin_id, const char *indexer_name,
                          CLAP_EXT_PRESET_EMIT emit, void *user_data);
