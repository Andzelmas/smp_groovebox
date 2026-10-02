#include "clap_ext_preset_load.h"
#include "../../types.h"
#include <stdio.h>
#include <string.h>

// separates location and load_key in a preset's identity
#define PRESET_ID_SEP '\x1f'

size_t clap_ext_preset_identity(char *out, size_t cap, const char *location,
                                const char *load_key) {
    int len = snprintf(out, cap, "%s%c%s", location ? location : "",
                       PRESET_ID_SEP, load_key ? load_key : "");
    return len < 0 ? 0 : (size_t)len;
}

bool clap_ext_preset_load(const clap_plugin_t *plugin, const char *identity) {
    if (!plugin || !identity)
        return false;
    const clap_plugin_preset_load_t *preset_load =
        plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD);
    if (!preset_load)
        preset_load = plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD_COMPAT);
    if (!preset_load || !preset_load->from_location)
        return false;
    // the first separator - a load_key can hold anything
    const char *sep = strchr(identity, PRESET_ID_SEP);
    if (!sep)
        return false;
    size_t location_len = (size_t)(sep - identity);
    char location[MAX_PATH_STRING];
    if (location_len >= sizeof(location))
        return false;
    memcpy(location, identity, location_len);
    location[location_len] = '\0';
    // no location: the preset is inside the plugin. No load_key: it is the
    // whole file
    uint32_t kind = location_len ? CLAP_PRESET_DISCOVERY_LOCATION_FILE
                                 : CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
    const char *load_key = sep[1] ? sep + 1 : NULL;
    return preset_load->from_location(plugin, kind,
                                      location_len ? location : NULL, load_key);
}
