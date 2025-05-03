#include "clap_ext_preset_factory.h"
#include <clap/clap.h>
#include <stdlib.h>
#include "../../types.h"
typedef struct _clap_ext_preset_factory{
    char** loc_names;
    uint32_t* loc_kinds;
    char** loc_locations;
    uint32_t loc_count;
}CLAP_EXT_PRESET_FACTORY;

static bool clap_ext_preset_indexer_declare_filetype(const struct clap_preset_discovery_indexer* indexer, const clap_preset_discovery_filetype_t* filetype){
    //TODO not doing with filetype as of right now
    return true;
}
static bool clap_ext_preset_indexer_declare_location(const struct clap_preset_discovery_indexer* indexer, const clap_preset_discovery_location_t* location){
    CLAP_EXT_PRESET_FACTORY* ext_preset_fac = (CLAP_EXT_PRESET_FACTORY*)indexer->indexer_data;
    
    return true;
}
static bool clap_ext_preset_indexer_declare_soundpack(const struct clap_preset_discovery_indexer* indexer, const clap_preset_discovery_soundpack_t* soundpack){
    //TODO soundpacks are not used as of right now
    return true;
}
static const void* clap_ext_preset_indexer_get_extension(const struct clap_preset_discovery_indexer* indexer, const char* extension_id){
    //TODO not checking preset extensions right now
    return NULL;
}

void clap_ext_preset_clean(CLAP_EXT_PRESET_FACTORY* clap_ext_preset_data){
    if(!clap_ext_preset_data)return;
    if(clap_ext_preset_data->loc_kinds)free(clap_ext_preset_data->loc_kinds);
    if(clap_ext_preset_data->loc_locations)free(clap_ext_preset_data->loc_locations);
    if(clap_ext_preset_data->loc_names)free(clap_ext_preset_data->loc_names);
    clap_ext_preset_data->loc_count = 0;
    free(clap_ext_preset_data);
}

CLAP_EXT_PRESET_FACTORY* clap_ext_preset_init(const clap_plugin_entry_t* plug_entry, clap_host_t clap_host_info){
    CLAP_EXT_PRESET_FACTORY* clap_ext_preset_data = calloc(1, sizeof(CLAP_EXT_PRESET_FACTORY));
    if(!clap_ext_preset_data)return NULL;
    clap_ext_preset_data->loc_count = 0;
    clap_ext_preset_data->loc_kinds = NULL;
    clap_ext_preset_data->loc_locations = NULL;
    clap_ext_preset_data->loc_names = NULL;
    //preset-discovery factory
    const clap_preset_discovery_factory_t* preset_fac = plug_entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
    if(!preset_fac){
	clap_ext_preset_clean(clap_ext_preset_data);
	return NULL;
    }
    
    uint32_t provider_count = preset_fac->count(preset_fac);
    for(uint32_t i = 0; i < provider_count; i++){
	const clap_preset_discovery_provider_descriptor_t* preset_desc = preset_fac->get_descriptor(preset_fac, i);
	if(!preset_desc)continue;
	clap_preset_discovery_indexer_t preset_indexer;
	preset_indexer.clap_version = clap_host_info.clap_version;
	preset_indexer.declare_filetype = clap_ext_preset_indexer_declare_filetype;
	preset_indexer.declare_location = clap_ext_preset_indexer_declare_location;
	preset_indexer.declare_soundpack = clap_ext_preset_indexer_declare_soundpack;
	preset_indexer.get_extension = clap_ext_preset_indexer_get_extension;
	preset_indexer.indexer_data = (void*)clap_ext_preset_data;
	preset_indexer.name = clap_host_info.name;
	preset_indexer.vendor = clap_host_info.vendor;
	preset_indexer.url = clap_host_info.url;
	preset_indexer.version = clap_host_info.version;
	const clap_preset_discovery_provider_t* preset_discovery = preset_fac->create(preset_fac, &preset_indexer, preset_desc->id);
	if(!preset_discovery)continue;
	if(!preset_discovery->init(preset_discovery))continue;

	preset_discovery->destroy(preset_discovery);
    }

    return clap_ext_preset_data;
}
