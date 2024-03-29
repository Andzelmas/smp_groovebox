#include "clap_plugins.h"
#include "../util_funcs/log_funcs.h"
#include <clap/clap.h>

void clap_plug_init(const char* plug_path){
    clap_plugin_entry_t* plug_in;
    unsigned int init_err =plug_in->init(plug_path);
    log_append_logfile("plugin init err %d\n", init_err);
    //clap_entry->deinit();
}
