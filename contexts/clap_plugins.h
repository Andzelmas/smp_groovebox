#pragma once
typedef struct _clap_plug_info CLAP_PLUG_INFO; //the struct that holds all the plugin info
//return the names of the plugins in the plugin directory
char** clap_plug_return_plugin_names(CLAP_PLUG_INFO* clap_data, unsigned int size);
//initiate the plugin path
void clap_plug_init(const char* plug_path);
