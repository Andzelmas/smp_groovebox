#pragma once
#include <clap/clap.h>
#include <stdbool.h>
#include <stddef.h>

// A plugin's state, through its state-context extension when it has one, else
// its state extension. And the host's own preset files, one state each: a
// header - this format's mark, the plugin id and the preset's name - then the
// state. [main-thread]

// a host preset file's name ends with it
#define CLAP_HOST_PRESET_EXT ".clapstate"

// the plugin has a state extension
bool clap_ext_state_available(const clap_plugin_t *plugin);

// the plugin's state for context (a clap_plugin_state_context_type) into a
// malloc'd *data, NULL for an empty one. false when the plugin has no state
// extension or the save failed
bool clap_ext_state_save(const clap_plugin_t *plugin, uint32_t context,
                         void **data, size_t *size);

// load a state clap_ext_state_save made. false when the plugin has no state
// extension or the load failed
bool clap_ext_state_load(const clap_plugin_t *plugin, uint32_t context,
                         const void *data, size_t size);

// write plugin_id's state as a preset file at path, replacing a file there.
// false when it could not be written - a file that was there stays as it was
bool clap_ext_state_file_write(const char *path, const char *plugin_id,
                               const char *name, const void *data,
                               size_t size);

// the preset name of the preset file at path into name, cut to cap. false
// when it is not a preset file of plugin_id
bool clap_ext_state_file_name(const char *path, const char *plugin_id,
                              char *name, size_t cap);

// the state in the preset file at path into a malloc'd *data, NULL for an
// empty one. false when it is not a preset file of plugin_id or could not be
// read
bool clap_ext_state_file_read(const char *path, const char *plugin_id,
                              void **data, size_t *size);

// a preset file's identity in a preset list, never one clap_ext_preset_identity
// in clap_ext_preset_load.h makes. Returns the length it needs without the NUL
// - out holds it only when that is below cap
size_t clap_ext_state_identity(char *out, size_t cap, const char *path);

// the preset file path in identity, NULL when it is not a preset file's
const char *clap_ext_state_identity_path(const char *identity);
