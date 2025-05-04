#include "clap_ext_preset_factory.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../types.h"

typedef struct _clap_ext_preset_container{
    char* name;
    char* location;
    char* load_key;
    uint32_t loc_kind; //the same location kind as the one on the CLAP_EXT_PRESET_SINGLE_LOC, for convenience
    clap_universal_plugin_id_t plugin_id; //char* id in this struct holds the unique id of the plugin this preset can be used with
}CLAP_EXT_PRESET_CONTAINER;

typedef struct _clap_ext_preset_single_loc{
    char* loc_name;
    uint32_t loc_kind;
    char* loc_location;
    CLAP_EXT_PRESET_CONTAINER* preset_containers;
    uint32_t preset_containers_count;
}CLAP_EXT_PRESET_SINGLE_LOC;

typedef struct _clap_ext_preset_factory{
    CLAP_EXT_PRESET_SINGLE_LOC* clap_ext_locations;
    uint32_t loc_count;
}CLAP_EXT_PRESET_FACTORY;

static bool clap_ext_preset_indexer_declare_filetype(const struct clap_preset_discovery_indexer* indexer, const clap_preset_discovery_filetype_t* filetype){
    //TODO not doing anything with filetype as of now
    return true;
}
static bool clap_ext_preset_indexer_declare_location(const struct clap_preset_discovery_indexer* indexer, const clap_preset_discovery_location_t* location){
    CLAP_EXT_PRESET_FACTORY* ext_preset_fac = (CLAP_EXT_PRESET_FACTORY*)indexer->indexer_data;
    if(!location)return false;
    ext_preset_fac->loc_count += 1;
    uint32_t cur_item = ext_preset_fac->loc_count - 1;
    CLAP_EXT_PRESET_SINGLE_LOC* temp_locations = realloc(ext_preset_fac->clap_ext_locations, sizeof(CLAP_EXT_PRESET_SINGLE_LOC) * ext_preset_fac->loc_count);
    if(!temp_locations){
	ext_preset_fac->loc_count -= 1;
	return false;
    }
    ext_preset_fac->clap_ext_locations = temp_locations;
    
    CLAP_EXT_PRESET_SINGLE_LOC* cur_loc = &(ext_preset_fac->clap_ext_locations[cur_item]);
    cur_loc->loc_kind = location->kind;
    cur_loc->loc_location = NULL;
    cur_loc->loc_name = NULL;
    cur_loc->preset_containers = NULL;
    cur_loc->preset_containers_count = 0;

    cur_loc->loc_location = calloc(strlen(location->location) + 1, sizeof(char));
    if(cur_loc->loc_location)
	snprintf(cur_loc->loc_location, strlen(location->location), "%s", location->location);

    cur_loc->loc_name = calloc(strlen(location->name) + 1, sizeof(char));
    if(cur_loc->loc_name)
	snprintf(cur_loc->loc_name, strlen(location->name), "%s", location->name);
    
    return true;
}
static bool clap_ext_preset_indexer_declare_soundpack(const struct clap_preset_discovery_indexer* indexer, const clap_preset_discovery_soundpack_t* soundpack){
    //TODO soundpacks are not used as of now
    return true;
}
static const void* clap_ext_preset_indexer_get_extension(const struct clap_preset_discovery_indexer* indexer, const char* extension_id){
    //TODO not checking preset extensions now
    return NULL;
}

static void clap_ext_preset_container_clean(CLAP_EXT_PRESET_CONTAINER* preset_container){
    if(!preset_container)return;
    if(preset_container->name)free(preset_container->name);
    if(preset_container->load_key)free(preset_container->load_key);
    if(preset_container->location)free(preset_container->location);
}
static void clap_ext_single_loc_clean(CLAP_EXT_PRESET_SINGLE_LOC* single_loc){
    if(!single_loc)return;
    if(single_loc->loc_location)free(single_loc->loc_location);
    if(single_loc->loc_name)free(single_loc->loc_name);
    if(single_loc->preset_containers){
        for(uint32_t i = 0; i < single_loc->preset_containers_count; i++){
	    clap_ext_preset_container_clean(&(single_loc->preset_containers[i]));
	}
	free(single_loc->preset_containers);
    }
    single_loc->preset_containers_count = 0;
}
void clap_ext_preset_clean(CLAP_EXT_PRESET_FACTORY* clap_ext_preset_data){
    if(!clap_ext_preset_data)return;
    if(clap_ext_preset_data->clap_ext_locations){
	for(uint32_t i = 0; i < clap_ext_preset_data->loc_count; i++){
	    clap_ext_single_loc_clean(&(clap_ext_preset_data->clap_ext_locations[i]));
	}
	free(clap_ext_preset_data->clap_ext_locations);
    }
    clap_ext_preset_data->loc_count = 0;
	
    free(clap_ext_preset_data);
}

CLAP_EXT_PRESET_FACTORY* clap_ext_preset_init(const clap_plugin_entry_t* plug_entry, clap_host_t clap_host_info){
    CLAP_EXT_PRESET_FACTORY* clap_ext_preset_data = calloc(1, sizeof(CLAP_EXT_PRESET_FACTORY));
    if(!clap_ext_preset_data)return NULL;
    clap_ext_preset_data->loc_count = 0;
    clap_ext_preset_data->clap_ext_locations = calloc(1, sizeof(CLAP_EXT_PRESET_SINGLE_LOC));
    if(!clap_ext_preset_data->clap_ext_locations){
	clap_ext_preset_clean(clap_ext_preset_data);
	return NULL;
    }
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

uint32_t clap_ext_preset_location_count(CLAP_EXT_PRESET_FACTORY* preset_fac){
    if(!preset_fac)return 0;
    return preset_fac->loc_count;
}
void clap_ext_preset_location_name(CLAP_EXT_PRESET_FACTORY* preset_fac, uint32_t loc_idx, char* return_name, uint32_t name_len){
    if(!preset_fac)return;
    if(loc_idx >= preset_fac->loc_count)return;
    CLAP_EXT_PRESET_SINGLE_LOC* single_location = &(preset_fac->clap_ext_locations[loc_idx]);
    if(!single_location)return;
    if(!single_location->loc_name)return;
    snprintf(return_name, name_len, "%s", single_location->loc_name);
}
