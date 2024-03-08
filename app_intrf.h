#pragma once

//enum that holds the types of the various contexts
enum cx_types_enum{
    //main context, a cx that has cx children and has a path  member
    Main_cx_e = 0x0100,
    //Main context subtypes
    //root cx that is the whole song, the top level cx
    Root_cx_st = 0x0001,
    //Sampler context subtype
    Sampler_cx_st = 0x0002,
    //Plugin host subtype
    Plugins_cx_st = 0x0003,
    //Trk subtype, where we can set routing of midi and audio, and which tracks to record
    Trk_cx_st = 0x0004,
    //Synth subtype
    Synth_cx_st = 0x0005,
    //button context, cx that has three different type values set to NULL, but these can be used to
    //for ex store a file path or something else
    Button_cx_e = 0x0200,
    //Button subtypes
    //This adds a list. If its a file list, entering the Item_cx_st will envoke the function of loading a preset, song
    //or similar if we are loading a sample for example first enter will copy str_val from Item_cx_st to the
    //AddList_cx_st and only on second enter (when the str_val are equal) it will load the sample - so that on
    //first enter we can preview the sample by playing it.
    //uchar_val in this button is a type, which tells what purpose this button serves -
    //look at the IntrfPurposeType enum.
    //this uses int_val to not let the user go outside the initial directory
    AddList_cx_st = 0x0001,
    //dir context, the path it holds is to know from which path to populate cx with context_list_from_dir
    Dir_cx_st = 0x0002,
    //list item, file context, the str_val is a path, plugin uri etc.
    Item_cx_st = 0x0003,
    //a button to save the context and its children, that have save 1, to a file, if its uchar_val is 0, save to
    //this file if its 1 save to new file.
    Save_cx_st = 0x0004,
    //a button to cancel the file that is selected and exit from file chooser
    //if its a port AddList, the cancel button will clear the Port_cx_st that exist only in cx but not on audio.
    //Though it will be named there clear not cancel.
    Cancel_cx_st = 0x0005,
    //a button to remove the context
    Remove_cx_st = 0x0006,
    //a button representing a port, inside it can hold other Port_cx_st, it means it is connected to them
    //only output ports hold input ports inside. Input ports dont have any children
    //str_val has the full port name
    //int_val has a number of ports that this output port is connected to, if its an input port int_val 1 means the
    //port is connected
    Port_cx_st = 0x0007,
    //transport button. int_val 0 - start play head, 1 - stop play head
    Transport_cx_st = 0x0008,    
    //one sample context.
    //it could be a button right now, but i have a feeling this struct will expand and have more members and
    //unique functions
    Sample_cx_e = 0x0300,

    //one plugin context
    Plugin_cx_e = 0x0400,
    
    //cx to hold values, that can be set and get by the user - a parameter
    //val_type intrfReturntype enum of what type the float returns, val_id - which value to set/get
    Val_cx_e = 0x0500,

    //context for the oscillator in the synth cx
    Osc_cx_e = 0x0600
};

//enum for app init failures
enum IntrfStatus{
    IntrfFailedMalloc = -1,
    AppFailedMalloc = -2,
    SmpFailedMalloc = -3,
    SmpJackFailedMalloc = -4,
    SongFileParseErr = -5,
    RootCXInitFailed = -6,
    BuildCXTreeFailed = -7,
    RootMallocFailed = -8,
    RootChildFailed = -9,
    PlugJackFailedInit = -10,
    PlugDataFailedMalloc = -11,
    TrkJackFailedInit = -12
};

//enum for purpose types
enum IntrfPurposeType{
    //list of files to load a sample
    Sample_purp = 0x01,
    //list of presets or song files to load a preset or a song
    Load_purp = 0x02,
    //list of available plugins that we can load
    addPlugin_purp = 0x03,
    //list of audio ports available to the system
    Connect_audio_purp = 0x04,
    //list of midi ports available to the system
    Connect_midi_purp = 0x05,
    //list of available presets for a Plugin_cx_e
    Load_plugin_preset_purp = 0x06
};
//enum to easily differentiate port types
enum IntrfPortType{
    Port_type_in = 0x01,
    Port_type_out = 0x02,
    Port_type_audio = 0x10,
    Port_type_midi = 0x20
};
typedef enum IntrfStatus intrf_status_t;
//the app_intrf single context, holds other cxs
typedef struct intrf_cx CX;
//this is the main cx, root, sampler etc.
typedef struct intrf_cx_main CX_MAIN;
//struct that is the cx representation of the sample, actual sample buffers live in the smp_data
typedef struct intrf_cx_sample CX_SAMPLE;
//struct cx representation of a plugin, on creation it instances a plugin
typedef struct intrf_cx_plugin CX_PLUGIN;
//struct cx representation of a oscillator
typedef struct intrf_cx_osc CX_OSC;
//this is the button cx, does something when entered
typedef struct intrf_cx_button CX_BUTTON;
//cx for user controllable parameters
typedef struct intrf_cx_val CX_VAL;

//this is the struct for the app interface so that the user can interact with the architecture
typedef struct _app_intrf APP_INTRF;

//init the app_intrf and the whole app arcitechture
//the conf_file is the configuration file that has the last loaded song and the global path for the dir structure
APP_INTRF* app_intrf_init(intrf_status_t* err_status, const char* song_path);

//parse the song xml file and create appropriate contexts
//attribs are the attribs from xml for the cx_type, and attrib_names the names for those attributes
//this does not add cx itself it calls the cx_init_cx_type function, this is a callback in the xml parser
//also the string arrays that are malloced are cleared so its our responsibility to malloc what we want to keep
//usually we dont need to malloc anything from the attrib_names
static void cx_process_from_file(void* arg,
				 const char* cx_name, const char* parent,
				 const char* attribs[], const char* attrib_names[], unsigned int attrib_size);

//initializes the root context, mallocs the CX subtype and casts it to CX 
//and also cx_add_child it to the cx structure
static CX* cx_init_cx_type(APP_INTRF* app_intrf, const char* parent_string, const char* name, unsigned int type,
			   const char* type_attribs[], const char* type_attrib_names[], int attrib_size);

//find the name of one string array and return the value from the other
static char* cx_find_attrib_name(const char* attrib_names[], const char* attrib_values[],
				 const char* find_name, int attrib_size);

//create and initialize a child for the parent context.
static int cx_add_child(CX* parent_cx, CX* child_cx, const char* name, unsigned int type);
//remove child and update the structure, this also clears the memory of the cx, by calling the cx_clear_contexts
//if type is not 0 remove_child will only remove the cx of that type
//type_child is if to use the same type for the children of the child_cx, if 0 the children of child_cx will be
//removed no matter of the type
static void cx_remove_child(CX* child_cx, int only_one, unsigned int type);
//remove this cx and its children - a convenience function
static int cx_remove_this_and_children(CX* this_cx);
//find a context based on its name
//the root_node is the node from which to start searching
static CX* cx_find_name(const char* name, CX* root_node);
//find a AddList context with the uchar variable. cx_array must be allocated (with one member), and size has to be 0
//the function will build the cx_array with the contexts that are AddList and mach the uchar (good
//for finding for ex port containers)
static void cx_find_container(CX *root_node, unsigned int uchar, CX*** cx_array, unsigned int* size);
//find cx that holds the same str_val value
//go_in == 1, go inside children while searching
static CX *cx_find_with_str_val(const char* str_val, CX *root_node, int go_in);
//travel the context and free the memory, post order traversing, so to start from leaves
static void cx_clear_contexts(CX* root, int only_one);

//create a context structure from a directory list, parent_name is needed to create contexts inside the parent
//that called this function
static int context_list_from_dir(APP_INTRF* app_intrf,
				 const char* dir_name, const char* parent_name, int depth);
//create a context from a string, used for string lists to select an item
//disp_name is the name to display, should be unique in the list, string_name will be put in the str_val
//of the item button. disp_name can be the same as string_name
static int context_list_from_string(APP_INTRF* app_intrf, const char* disp_name, 
				    const char* string_name, const char* parent_name);

/*CALLBACK FUNCTIONS FOR THE CX CONTEXTS, FOR INTERNAL USE*/
/*--------------------------------------------------------*/
//get values of a cx, chooses what cx depending on parent cx type
//chooses which value depending on the int_val of the button
//returns float, but also returns in ret_type what type of value is the original value
static float cx_get_value_callback(APP_INTRF* app_intrf, CX* self, unsigned char* ret_type);
//set the values of a cx, param_op to increase decrease or set the value, check the paramOperType in types.h
static void cx_set_value_callback(APP_INTRF* app_intrf, CX* self, float set_to, unsigned char param_op);
//callback for remove button, that removes its parent context
//returns 1 if context structure changed
static int cx_enter_remove_callback(APP_INTRF* app_intrf, CX* self);
//callback to save the current context to a file, as a preset or a song file - the process is the same
//uses app_json_write_json_callback to write into the JSONHANDlE object, that can be used to save a cx
//structure to a file
static void cx_enter_save_callback(APP_INTRF* app_intrf, CX* self);
//enter dir callback, this callback lists the dirs and files in its str_val member and clears the file and dir
//contexts created before
//returns 1 if the context structure changed
static int cx_enter_dir_callback(APP_INTRF* app_intrf, CX* self);
//enter port, add its str_val to the addList str_val, if the value is not NULL connect these ports
//returns 1 if the context structure changed
static int cx_enter_port_callback(APP_INTRF* app_intrf, CX* self);
//enter transport button and depending on what it does (int_val == 0 - start, 1 - stop,
//2 - skip bar int_val_1 amount forward, 3 - skip bar int_val_1 amount backward
static void cx_enter_transport_callback(APP_INTRF* app_intrf,CX* self);
//enter file callback, load a plugin, smaple, preset etc.
static void cx_enter_item_callback(APP_INTRF* app_intrf, CX* self);
//cancel button, depending on the parents type does different things
//returns 1 if the context structure changed
static int cx_enter_cancel_callback(APP_INTRF* app_intrf, CX* self);
//callback that creates a list of files when called (uses the context_list_from_dir)
static void cx_enter_AddList_callback(APP_INTRF* app_intrf, CX* self);
//callback that simply clears the AddList str_val to NULL and frees it
static void cx_exit_AddList_callback(APP_INTRF* app_intrf, CX* self);
//callback function for the Main_cx_e when the context is left
static void cx_exit_app_callback(APP_INTRF* app_intrf, CX* self);

/*THESE FUNCTIONS FILTER WHICH CALLBACK TO CALL*/
//filters out which set_value callback to call depending on type and subtype
static void intrf_callback_set_value(APP_INTRF* app_intrf, CX* self, int set_to);
//filters out which get_value callback to call depending on type and subtype
//returns a float but from ret_type can see what type of data it is originally
static float intrf_callback_get_value(APP_INTRF* app_intrf, CX* self, unsigned char* ret_type);
//filters out which enter callback to call depending on type and subtype
//returns 1 if the context structure changed
static int intrf_callback_enter(APP_INTRF* app_intrf, CX* self);
//filters out which exit callback to call depending on type and subtype
static void intrf_callback_exit(APP_INTRF* app_intrf, CX* self);

/*FUNCTIONS TO NAVIGATE THE CX STRUCTURE, THEY ARE FOR THE USER*/
//these functions set app_intrf->curr_cx and app_intrf->select_cx, write debug messages and gives the
//user CX structs to call special functions with, like list children and so on
//These are abstract and general functions, with general names like nav_next_context and similar
//It does not care about the types of contexts it cares what children, siblings, parents the contexts have
/*-------------------------------------------------------------*/
//return how many children the cx has
int nav_return_numchildren(APP_INTRF* app_intrf, CX* this_cx);
//return the children to user provided CX array of this_cx
//ret_type is what to return; 0 - return all children; 1 - return only main children, no buttons like cancel etc;
//2 - return only buttons like cancel, load etc.
CX** nav_return_children(APP_INTRF* app_intrf, CX* this_cx, unsigned int* children_num, unsigned int ret_type);
//returns if the cx need to be highlighted (for example if the sample or a port is selected)
unsigned int nav_return_need_to_highlight(CX* this_cx);
//return the type of the cx
unsigned int nav_return_cx_type(CX* this_cx);
//navigation function to exit from the current context.
//returns -2 if the current cx does not have a parent
int nav_exit_cur_context(APP_INTRF* app_intrf);
//invoke the cx, calling its enter function. Some functions delete cx when invoked, so this function returns
//the app_intrf->select_cx, because after invoking usually we call nav_enter_cx on a cx, if it is deleted with
//invoke function the cx wont be there anymore, that is why this function returns a cx.
//change_structure will be 1 if the invoke changed the contexts structure
CX* nav_invoke_cx(APP_INTRF* app_intrf, CX* select_cx, int* changed_structure);
//enter the current context. changes the app_intrf->curr_cx, but does not invoke the cx
int nav_enter_cx(APP_INTRF* app_intrf, CX* select_cx);
//set the select context
int nav_set_select_cx(APP_INTRF* app_intrf, CX* sel_cx);
//go to the next context in the current context
int nav_next_context(APP_INTRF* app_intrf);
//go to the previous context, wraps around like the go to the next context
int nav_prev_context(APP_INTRF* app_intrf);
//return the root_cx
CX* nav_ret_root_cx(APP_INTRF* app_intrf);
//return the curr_cx
CX* nav_ret_curr_cx(APP_INTRF* app_intrf);
//return the currently selected cx
CX* nav_ret_select_cx(APP_INTRF* app_intrf);
//get the value of the cx, ret_type returns what type the value is originally
float nav_get_cx_value(APP_INTRF* app_intrf, CX* sel_cx, unsigned char* ret_type);
//get the value of the cx and return it as a malloced string (that needs to be freed)
char* nav_get_cx_value_as_string(APP_INTRF* app_intrf, CX* sel_cx);
//set the value of the selected cx - this will be a Button_cx_e that will increment or lower the apropriate
//value of its parent cx (set_to<0 decrease the value, set_to>0 - increase the value).
//though set_to is int, the actual value on the parameter will be float, the context will convert this value
//to the correct precision (for example instead of 1, for a filter parameter this could become 0.01 for example)
int nav_set_cx_value(APP_INTRF* app_intrf, CX* select_cx, int set_to);
//update the parameter values for the ui, should be called regulery in a loop for example
int nav_update_params(APP_INTRF* app_intrf);
//get the cx name
const char* nav_get_cx_name(APP_INTRF* app_intrf, CX* select_cx);
//function that cleans all the allocated memory, closes the app
static int app_intrf_close(APP_INTRF* app_intrf);
//iterates cx, finds AddList_cx_st that has ports and calls cx_reset_ports on that AddList cx
//top_cx can be root_cx
int app_intrf_iterate_reset_ports(APP_INTRF* app_intrf, CX* top_cx, unsigned int create);
//function that disconnects ports, creates the ports cx from all ports (if a port cx does not already exist) and
//connect ports. The port_parent should be an output port or a AddList that holds such ports. It does not
//iterate through cx structure to find these ports.
//create - if we need to create the Port_cx_st for returned ports.
static int cx_reset_ports(APP_INTRF* app_intrf, CX* port_parent, unsigned int type, unsigned int create);
/*HELPER FUNCTIONS FOR MUNDAIN CX MANIPULATION*/
//return 1 if the port exists only on cx but not on audio backend
static int helper_cx_is_port_nan(APP_INTRF* app_intrf, CX* cx_port);
//goes through Port_cx_st and disconnects ports
static int helper_cx_disconnect_ports(APP_INTRF* app_intrf, CX* top_cx, unsigned int disc_all);
//goes through Port_cx_st and connects ports if there is a Port_cx_st inside
static int helper_cx_connect_ports(APP_INTRF* app_intrf, CX* top_cx);
//iterates through the cx structure from the top_cx and calls a callback function
//in_sib - travel to siblings of the top_cx or not
static int helper_cx_iterate_with_callback(APP_INTRF* app_intrf, CX* top_cx, void* arg,
				    void(*proc_func)(void*arg, const char* cur_name, const char* parent_name,
						     const char** attrib_names, const char** attrib_vals,
						     unsigned int attrib_size));
//removes the rem_cx, its children and depending on type the data on data (for example sample in smp_data)
static int helper_cx_remove_cx_and_data(APP_INTRF* app_intrf, CX* rem_cx);
//clears the cx in the self_cx that have save == 1
static int helper_cx_clear_cx(APP_INTRF* app_intrf, CX* self_cx);
//build new path for the Main_cx_e
//num is the number of the song or preset in case of the other contexts
static int helper_cx_build_new_path(APP_INTRF* app_intrf, CX* self, int num);
//combine cx and its parents path attributes
static char* helper_cx_combine_paths(CX* self);
//copies str_val from from_cx to to_cx (for CX_BUTTON contexts)
static int helper_cx_copy_str_val(CX* from_cx, CX* to_cx);
//create cx contexts from default context parameters (for example for sample)
//parent_node - for which cx to create the parameters
static int helper_cx_create_cx_for_default_params(APP_INTRF* app_intrf, CX* parent_node, unsigned char cx_type, unsigned int cx_id);
//parse the IntrfStatus enum errors and return a readable string
const char *app_intrf_write_err(const int* err_status);

