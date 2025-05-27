#pragma once
#include <clap/clap.h>

typedef struct _clap_ext_preset_factory CLAP_EXT_PRESET_FACTORY;
typedef struct _clap_ext_preset_user_funcs{
    int (*ext_preset_send_msg)(void* user_data, const char* msg); //function to send a message to the UI - this will only be used on the [main-thread]
    void* user_data; //user data used for the user functions
}CLAP_EXT_PRESET_USER_FUNCS;
//all function only usable on the [main-thread]
//plugin_id is a unique string from the plugin id, used to compare ids from returned preset container, to decide if the preset needs to be indexed
void clap_ext_preset_clean(CLAP_EXT_PRESET_FACTORY* clap_ext_preset_data);
CLAP_EXT_PRESET_FACTORY* clap_ext_preset_init(const clap_plugin_entry_t* plug_entry, clap_host_t clap_host_info, CLAP_EXT_PRESET_USER_FUNCS user_funcs);
//return the preset name, path and category list (string where each category is separated by "/")
int clap_ext_preset_info_return(CLAP_EXT_PRESET_FACTORY* preset_fac, uint32_t idx,
				      char* name, uint32_t name_len,
				      char* path, uint32_t path_len,
				      char* categories, uint32_t categories_len);
//how many single location objects in the CLAP_EXT_PRESET_FACTORY struct
uint32_t clap_ext_preset_location_count(CLAP_EXT_PRESET_FACTORY* preset_fac);
//return the name of the single location
void clap_ext_preset_location_name(CLAP_EXT_PRESET_FACTORY* preset_fac, uint32_t loc_idx, char* return_name, uint32_t name_len);
