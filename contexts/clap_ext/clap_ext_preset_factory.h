#pragma once
#include <clap/clap.h>

typedef struct _clap_ext_preset_factory CLAP_EXT_PRESET_FACTORY;

//all function only usable on the [main-thread]
void clap_ext_preset_clean(CLAP_EXT_PRESET_FACTORY* clap_ext_preset_data);
CLAP_EXT_PRESET_FACTORY* clap_ext_preset_init(const clap_plugin_entry_t* plug_entry, clap_host_t clap_host_info);
//how many single location objects in the CLAP_EXT_PRESET_FACTORY struct
uint32_t clap_ext_preset_location_count(CLAP_EXT_PRESET_FACTORY* preset_fac);
//return the name of the single location
void clap_ext_preset_location_name(CLAP_EXT_PRESET_FACTORY* preset_fac, uint32_t loc_idx, char* return_name, uint32_t name_len);
