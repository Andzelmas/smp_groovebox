#pragma once
#include <json-c/json.h>
//handle for building json objects from external functions
typedef void JSONHANDLE;
//read the initial conf file if necessary create the shared dir structure
//then try to read the song file, if failed create song dir structure and the song.json file
//shared_samples - the shared song samples directory, to return to the function caller
//load_path - the path of the file to load, if NULL load the song from last_song key in smp_conf.json
//load_from_conf is a reference to a variable where 1 is returned if the file was created from configuration and
//not loaded from a song file
int app_json_read_conf(char** shared_dir, const char* load_path, const char* in_parent_name, unsigned int* load_from_conf,
		       void* arg, void(*user_func)(void*arg, const char* cur_name, const char* parent_name, const char* top_node_name,
						   const char** attrib_names, const char** attrib_vals,
						   unsigned int attrib_size));
//simple file_path json iterator. run the user function for each entry in json file
//to the function the key attribs are sent as strings (name and value) also the key name, the key parent and the number of the attributes
int app_json_open_iterate_callback(const char* file_path, const char* in_parent_name,
				   void* arg, void(*user_func)(void* arg, const char* json_name, const char* json_parent, const char* top_node_name,
							       const char** attrib_names, const char** attrib_vals, unsigned int attrib_count));
//find recursevily string find_key in the parsed_fp json object and return a malloced string of the value
static char* app_json_iterate_find_string(struct json_object* parsed_fp, const char* find_key);
//write the whole json object to a file with json_object_to_json_string_ext
static int app_json_write_json_to_file(struct json_object* obj, const char* file_path);
//load a file to the buffer and return it, needs to be freed later
static char* app_json_read_to_buffer(const char* file_path);
//iterate recursively through the json object and find a key and return the object of that key, value pair
struct json_object* app_json_iterate_and_find_obj(struct json_object* parsed_fp, const char* find_key);
//recursevily iterate through the json object and run a callback, this is external, just have to use a
//function to retrieve the root json object
//parsed_fp - the json object from which to start the iteration
//json_name the string name of the parsed_fp, can be NULL callback function should check for that
//json_parent the name of the parent of the parsed_fp, can be NULL callback function should check for that
//arg the user argument, could be anything, it will be sent to the callback function
//(*proc_func) the processing function that will be called for each key in the parsed_fp, it will recieve the
//current json_object name, its parent name, a string array of the key names, string array of the values and
//the size of the arrays
//json_file_path is used in app_json_iterate_run_callback if it finds a path key it travels into the
//json_file_path/path json file and travels that json file, but this is not sent into the callback
static int app_json_iterate_run_callback(struct json_object* parsed_fp,  const char* json_name, const char* top_name, 
					 const char* json_parent,
					 const char* json_file_path,
					 void *arg,
					 void(*proc_func)(void*arg, const char*cur_name, const char* parent_name, const char* top_node_name,
							  const char** attrib_names, const char** attrib_vals,
							  unsigned int attrib_size));

//the callback function that creates a dir for attrib_vals value if it does not exist yet
static void app_json_mkdir_callback(void* arg, const char* cur_name, const char* parent_name, const char* top_name,
			    const char* attrib_names[], const char* attrib_vals[], unsigned  int attrib_size);

//callback that writes to a json object from the attrib_names and attrib_vals when iterating another json file
//or cx structure or anything similar that needs to build a json object
void  app_json_write_json_callback(void* arg, const char* cur_name, const char* parent_name, const char* top_name,
				   const char** attrib_names, const char** attrib_vals, unsigned int attrib_size);
//this writes a json file from the JSONHANDLE (json_object in essence)
int app_json_write_handle_to_file(JSONHANDLE* obj, const char* cur_file,
				  unsigned int new_file, unsigned int preset);
//create an empty json object
int app_json_create_obj(JSONHANDLE** obj);
//given file path tokenise the contents of the file and return a json_object
struct json_object* app_json_tokenise_path(char* file_path);
