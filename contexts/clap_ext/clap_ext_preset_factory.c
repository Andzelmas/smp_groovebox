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
    char plugin_id[MAX_UNIQUE_ID_STRING]; //unique plugin_id used to match if the preset container can be used for the requested plugin
}CLAP_EXT_PRESET_CONTAINER;

typedef struct _clap_ext_preset_single_loc{
    char* loc_name;
    uint32_t loc_kind;
    char* loc_location;
    CLAP_EXT_PRESET_CONTAINER* preset_containers;
    uint32_t preset_containers_count;
    CLAP_EXT_PRESET_USER_FUNCS user_funcs; //user functions are copied to each location for convenience
}CLAP_EXT_PRESET_SINGLE_LOC;

typedef struct _clap_ext_preset_factory{
    CLAP_EXT_PRESET_SINGLE_LOC* clap_ext_locations;
    uint32_t loc_count;
    CLAP_EXT_PRESET_USER_FUNCS user_funcs;
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
    cur_loc->user_funcs = ext_preset_fac->user_funcs;

    cur_loc->loc_location = calloc(strlen(location->location) + 1, sizeof(char));
    if(cur_loc->loc_location)
	snprintf(cur_loc->loc_location, strlen(location->location) + 1, "%s", location->location);

    cur_loc->loc_name = calloc(strlen(location->name) + 1, sizeof(char));
    if(cur_loc->loc_name)
	snprintf(cur_loc->loc_name, strlen(location->name) + 1, "%s", location->name);
    
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

static void clap_ext_preset_meta_on_error(const struct clap_preset_discovery_metadata_receiver* receiver, int32_t os_error, const char* error_message){
    CLAP_EXT_PRESET_SINGLE_LOC* cur_loc = (CLAP_EXT_PRESET_SINGLE_LOC*)receiver->receiver_data;
    if(!cur_loc)return;

    if(!(cur_loc->user_funcs.ext_preset_send_msg))return;
    cur_loc->user_funcs.ext_preset_send_msg(cur_loc->user_funcs.user_data, error_message); 
}
static bool clap_ext_preset_meta_begin_preset(const struct clap_preset_discovery_metadata_receiver* receiver, const char* name, const char* load_key){
    //on errors returning true too, so the transmitter will still use the receiver callbacks, just that this preset container will not be indexed
    if(!receiver)return true;
    CLAP_EXT_PRESET_SINGLE_LOC* cur_loc = receiver->receiver_data;
    if(!cur_loc)return true;
    //location kind will be the same for all the preset containers, because it comes from the CLAP_EXT_PRESET_SINGLE_LOC
    uint32_t location_kind = cur_loc->loc_kind;

    cur_loc->preset_containers_count += 1;
    CLAP_EXT_PRESET_CONTAINER* temp_array = realloc(cur_loc->preset_containers, sizeof(CLAP_EXT_PRESET_CONTAINER) * cur_loc->preset_containers_count);
    if(!temp_array){
	cur_loc->preset_containers_count -= 1;
	return true;
    }

    cur_loc->preset_containers = temp_array;
    uint32_t cur_item = cur_loc->preset_containers_count - 1;

    CLAP_EXT_PRESET_CONTAINER* cur_container = &(cur_loc->preset_containers[cur_item]);
    cur_container->load_key = NULL;
    cur_container->loc_kind = location_kind;
    cur_container->location = NULL;
    cur_container->name = NULL;

    if(name){
	cur_container->name = calloc(strlen(name) + 1, sizeof(char));
	if(cur_container->name)
	    snprintf(cur_container->name, strlen(name) + 1, "%s", name);
    }
    if(load_key){
	cur_container->load_key = calloc(strlen(load_key) + 1, sizeof(char));
	if(cur_container->load_key)
	    snprintf(cur_container->load_key, strlen(load_key) + 1, "%s", load_key);
    }
    //TODO location on the cur_container will have to be the full filepath of this preset container if the loc_kind is FILE and we are crawling through directories
    //otherwise can use the cur_loc->loc_location
    if(cur_loc->loc_location){
	cur_container->location = calloc(strlen(cur_loc->loc_location) + 1, sizeof(char));
	if(cur_container->location)
	    snprintf(cur_container->location, strlen(cur_loc->loc_location) + 1, "%s", cur_loc->loc_location);
    }
    return true;
}
static void clap_ext_preset_meta_add_plugin_id(const struct clap_preset_discovery_metadata_receiver* receiver, const clap_universal_plugin_id_t* plugin_id){
    if(!receiver)return;
    CLAP_EXT_PRESET_SINGLE_LOC* cur_loc = receiver->receiver_data;
    if(!cur_loc)return;

    if(!plugin_id->id)return;

    uint32_t cur_item = cur_loc->preset_containers_count - 1;
    CLAP_EXT_PRESET_CONTAINER* cur_container = &(cur_loc->preset_containers[cur_item]);
    snprintf(cur_container->plugin_id, MAX_UNIQUE_ID_STRING, "%s", plugin_id->id);
}
static void clap_ext_preset_meta_set_soundpack_id(const struct clap_preset_discovery_metadata_receiver* receiver, const char* soundpack_id){
    //TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_set_flags(const struct clap_preset_discovery_metadata_receiver* receiver, uint32_t flags){
    //TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_add_creator(const struct clap_preset_discovery_metadata_receiver* receiver, const char* creator){
    //TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_set_description(const struct clap_preset_discovery_metadata_receiver* receiver, const char* description){
    //TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_set_timestamps(const struct clap_preset_discovery_metadata_receiver* receiver, clap_timestamp creation_time, clap_timestamp modification_time){
    //TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_add_feature(const struct clap_preset_discovery_metadata_receiver* receiver, const char* feature){
    //TODO not used at this moment
    return;
}
static void clap_ext_preset_meta_add_extra_info(const struct clap_preset_discovery_metadata_receiver* receiver, const char* key, const char* value){
    //TODO not used at this moment
    return;
}

CLAP_EXT_PRESET_FACTORY* clap_ext_preset_init(const clap_plugin_entry_t* plug_entry, clap_host_t clap_host_info, CLAP_EXT_PRESET_USER_FUNCS user_funcs){
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
    //copy the struct containing the user functions (send message and similar)
    clap_ext_preset_data->user_funcs = user_funcs;
    //all the locations from all the providers will be put into the single CLAP_EXT_PRESET_FACTORY struct
    //preset containers from the metada will be put into the CLAP_EXT_PRESET_SINGLE_LOC->preset_containers, but only the preset containers that can be used with the provided plugin_id
    uint32_t provider_count = preset_fac->count(preset_fac);
    uint32_t curr_idx_from = 0;
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
	//get the metada for each location and fill out the CLAP_EXT_PRESET_SINGLE_LOC->preset_containers
	uint32_t loc_idx = curr_idx_from;
	for(; loc_idx < clap_ext_preset_data->loc_count; loc_idx++){
	    CLAP_EXT_PRESET_SINGLE_LOC* cur_loc = &(clap_ext_preset_data->clap_ext_locations[loc_idx]);
	    if(!cur_loc->loc_name)continue;
	    //TODO memory leak
	    cur_loc->preset_containers = calloc(1, sizeof(CLAP_EXT_PRESET_CONTAINER));
	    if(!cur_loc->preset_containers)continue;
	    cur_loc->preset_containers_count = 0;
	    //fill the preset container array of the current preset location 
	    clap_preset_discovery_metadata_receiver_t metada_rec;
	    metada_rec.receiver_data = (void*)cur_loc;
	    metada_rec.on_error = clap_ext_preset_meta_on_error;
	    metada_rec.begin_preset = clap_ext_preset_meta_begin_preset;
	    metada_rec.add_plugin_id = clap_ext_preset_meta_add_plugin_id;
	    metada_rec.set_soundpack_id = clap_ext_preset_meta_set_soundpack_id;
	    metada_rec.set_flags = clap_ext_preset_meta_set_flags;
	    metada_rec.add_creator = clap_ext_preset_meta_add_creator;
	    metada_rec.set_description = clap_ext_preset_meta_set_description;
	    metada_rec.set_timestamps = clap_ext_preset_meta_set_timestamps;
	    metada_rec.add_feature = clap_ext_preset_meta_add_feature;
	    metada_rec.add_extra_info =clap_ext_preset_meta_add_extra_info;
	    //TODO if cur_loc->loc_kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE need to crawl the directories and call get_metadata for each file, instead of cur_loc->loc_location sending the full filename path
	    //If the cur_loc->loc_kind is a file not a directory dont crawl just call get_metadata
	    //otherwise send cur_loc->loc_location
	    preset_discovery->get_metadata(preset_discovery, cur_loc->loc_kind, "/usr/share/surge-xt/patches_factory/Basses/Attacky.fxp", &metada_rec);
	    //preset_discovery->get_metadata(preset_discovery, cur_loc->loc_kind, cur_loc->loc_location, &metada_rec);
	}

	curr_idx_from = loc_idx + 1;
	
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

char** clap_ext_preset_container_return_names(CLAP_EXT_PRESET_FACTORY* clap_ext_preset_data, const char* plugin_id){
    //TODO check if the preset container name and load_key are not null and if plug_id is the same as the preset container plugin id
}
