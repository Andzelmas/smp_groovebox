#pragma once
#include <clap/clap.h>
#include <stdbool.h>
#include <stddef.h>

// Loads CLAP presets through the plugin's preset-load extension. A preset is
// known by one string, its identity, built from what the preset-discovery
// factory reported for it. [main-thread]

// write a preset's identity into out. location is NULL for a preset inside
// the plugin, load_key NULL for a whole file preset. Returns the length it
// needs without the NUL - out holds it only when that is below cap
size_t clap_ext_preset_identity(char *out, size_t cap, const char *location,
                                const char *load_key);

// load the preset with this identity. false when the plugin has no preset-load
// extension or the load failed
bool clap_ext_preset_load(const clap_plugin_t *plugin, const char *identity);
