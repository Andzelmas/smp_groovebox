#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

//my libraries
#include "json_funcs.h"
#include "log_funcs.h"
//size of the single buffer read, when reading a file to buffer
#define JSONFILESIZE 1024
//the name of the configuration file that holds all the default values for the app
#define CONFJSONFILE "smp_conf.json"
//the name of the json key in the CONFJSONFILE that holds the last_song dir
#define LAST_SONG_KEY "last_song"
//the name of the json key in the CONFJSONFILE that holds the default_song_name
#define DEFAULT_SONG_KEY "default_song_name"
//the name of the json key in the CONFJSONFILE that holds the default shared dir structure
#define DEFAULT_SHARED_DIR_KEY "shared_dir_structure"
//the name of the json key in the CONFJSONFILE that holds the song.json default internal structure
#define DEFAULT_SONG_STRUCTURE "song_default_structure"
//the name of the json key in the CONFJSONFILE that holds the root dir of the shared directory
#define SHARED_DIR "root_dir"
//the size of string arrays, i.e const char *string_array[MAX_ATTRIB_ARRAY]
#define MAX_ATTRIB_ARRAY 200

int app_json_read_conf(char** shared_dir, const char* load_path, const char* in_parent_name, unsigned int* load_from_conf,
		       void* arg, void(*user_func)(void* arg, const char* json_name, const char* jason_parent, const char* top_name,
						   const char** attrib_names, const char** attribs, unsigned int attrib_count)){
    int return_val = 0;
    *load_from_conf = 0;
   //this will store the base dir of the song, need to free it later
    char* song_dir = NULL;
    struct json_object* parsed_fp = app_json_tokenise_path(CONFJSONFILE);
    if(!parsed_fp){
	log_append_logfile("Cant parse the json file %s\n", CONFJSONFILE);
	return_val = -1;
	goto clean_strings;
    }
    //return the directory of the shared samples
    if(shared_dir!=NULL)
	*shared_dir = app_json_iterate_find_string(parsed_fp, SHARED_DIR);
    //add song_dir string, if load_path is not null load the file from that path, else use the
    //LAST_SONG_KEY in the CONFJSONFILE
    if(load_path!=NULL){
	song_dir = (char*)malloc(sizeof(char)*strlen(load_path)+1);
	if(!song_dir){
	    log_append_logfile("Cant use the %s file name, malloc failed\n", load_path);
	    return_val = -1;
	    goto clean_all;
	}
	strcpy(song_dir, load_path);
    }
    else{
	song_dir = app_json_iterate_find_string(parsed_fp,  LAST_SONG_KEY);
    }
    
    //now travel the shared_dir_structure and create the directories if they do not exist
    struct json_object* cur_obj = NULL;
    const char* cur_name = DEFAULT_SHARED_DIR_KEY;
    cur_obj = app_json_iterate_and_find_obj(parsed_fp, cur_name);
    app_json_iterate_run_callback(cur_obj, NULL, NULL, NULL, NULL, NULL, app_json_mkdir_callback);

    char* buffer = app_json_read_to_buffer(song_dir);
    struct json_object* file_contents = NULL;
    //if failed to open the json song file, try to load the default one from the smp_conf.json
    if(!buffer){
	log_append_logfile("Cant read file %s Attempting to open the default song\n", song_dir);
	if(song_dir)free(song_dir);
	song_dir = app_json_iterate_find_string(parsed_fp, DEFAULT_SONG_KEY);
	if(!song_dir){
	    log_append_logfile("Cant find the default song name key %s\n", DEFAULT_SONG_KEY);
	    return_val = -1;
	    goto clean_all;
	}
	buffer = app_json_read_to_buffer(song_dir);
    }
    //if failed to open even the default song create a new one, from the smp_conf.json song_default_structure
    if(!buffer){
	log_append_logfile("Could not open %s song file, create a new one\n", song_dir);
	cur_name =  DEFAULT_SONG_STRUCTURE;
	cur_obj = app_json_iterate_and_find_obj(parsed_fp, cur_name);
	//we use the same iterate function just send a different callback.
	//we also send our json_object where the callback can build the json
	//structure, then we'll just need to write it out to a file
	file_contents = json_object_new_object();

	app_json_iterate_run_callback(cur_obj, NULL, in_parent_name, in_parent_name, song_dir, file_contents,
				      app_json_write_json_callback);
	if(file_contents){
	    if(app_json_write_json_to_file(file_contents, song_dir)<0){
		log_append_logfile("Failed to create the default file %s\n", song_dir);
		return_val = -1;
		goto clean_all;
	    }
	}
	//try to fill the buffer again
	buffer = app_json_read_to_buffer(song_dir);
	if(file_contents)json_object_put(file_contents);
	*load_from_conf = 1;
	file_contents = NULL;
    }
    //if the buffer is still empty an error occured
    if(!buffer){
	log_append_logfile("Failed to read the %s file and create it\n", song_dir);
	return_val = -1;
	goto clean_all;
    }
    file_contents = json_tokener_parse(buffer);
    free(buffer);
    if(!file_contents){
	log_append_logfile("Could not parse the song json file %s\n", song_dir);
	return_val = -1;
	goto clean_all;
    }
    //now send file_contents to  app_json_iterate_run_callback with the user function to create the contexts
    app_json_iterate_run_callback(file_contents, NULL, in_parent_name, in_parent_name, NULL, arg, user_func);

    //if we loaded a song file save the name to the last_song name in the configuration file
    if(in_parent_name==NULL){
	struct json_object* song_key = app_json_iterate_and_find_obj(parsed_fp, LAST_SONG_KEY);
	if(!song_key){
	    log_append_logfile("failed to extract the LAST_SONG_KEY\n");
	    return_val = -1;
	    goto clean_all;
	}
	json_object_set_string(song_key, song_dir);
	//now write the CONFJSONFILE again
	if(song_key){
	    if(app_json_write_json_to_file(parsed_fp, CONFJSONFILE)<0){
		log_append_logfile("Failed to write to the %s file\n", song_dir);
		return_val = -1;
		goto clean_all;
	    }
	}
    }
    //free memory
clean_all:
    if(parsed_fp)json_object_put(parsed_fp);
    if(file_contents)json_object_put(file_contents);
clean_strings:
    if(song_dir)free(song_dir);
    return return_val;
}

int app_json_open_iterate_callback(const char* file_path, const char* in_parent_name,
				   void* arg, void(*user_func)(void* arg, const char* json_name, const char* json_parent, const char* top_name, 
							       const char** attrib_names, const char** attrib_vals, unsigned int attrib_count)){
    int return_val = 0;
    char* buffer = NULL;
    buffer = app_json_read_to_buffer(file_path);
    if(!buffer){
	log_append_logfile("Cant read %s file\n", file_path);
	return_val = -1;
    }
    struct json_object* parsed_fp = NULL;
    parsed_fp = json_tokener_parse(buffer);
    free(buffer);
    if(!parsed_fp){
	log_append_logfile("Cant parse the json file %s\n", file_path);
	return_val = -1;
    }
    return_val = app_json_iterate_run_callback(parsed_fp, NULL, in_parent_name, in_parent_name, NULL, arg, user_func);

    if(parsed_fp)json_object_put(parsed_fp);
    return return_val;
}

static char* app_json_iterate_find_string(struct json_object* parsed_fp, const char* find_key){
    char* ret_string = NULL;
    struct json_object* cur_obj = NULL;
    cur_obj = app_json_iterate_and_find_obj(parsed_fp, find_key);
    if(!cur_obj)goto clean;  
    ret_string = (char*)malloc(sizeof(char)*(json_object_get_string_len(cur_obj)+1));
    if(!ret_string) {
	log_append_logfile("Could not find %s\n", find_key);
	goto clean;
    }
    strcpy(ret_string, json_object_get_string(cur_obj));    
clean:
    return ret_string;
}

static int app_json_write_json_to_file(struct json_object* obj, const char* file_path){
    int return_val = 0;
    char* dirs = NULL;
    FILE* fp = NULL;
    
    if(!obj){
	return_val = -1;
	goto clean;
    }
    char* whole_dir = NULL;
    //first try to create the dirs of the file_path, if these will fail with dir already exists thats fine
    //it means we can add the file already
    //Also if there is no / in the file_path it means there is no directories in the file_path, only the file name
    if(strchr(file_path, '/') != NULL){
	dirs = (char*)malloc(sizeof(char)*strlen(file_path)+1);
	if(!dirs){
	    log_append_logfile("Cant allocate mem for path\n");
	    return_val = -1;
	    goto clean;
	}
	strcpy(dirs, file_path);    
	char* cur_dir = NULL;
	cur_dir = strtok(dirs, "/");
	int whole_len = 0;
	whole_dir = (char*)malloc(sizeof(char)*(strlen(cur_dir)+whole_len+2));
	if(!whole_dir){
	    return_val = -1;
	    goto clean;
	}

	strcpy(whole_dir, cur_dir);
	strcat(whole_dir, "/");
	whole_len = strlen(whole_dir);
	cur_dir = strtok(NULL, "/");
	int mk_err = mkdir(whole_dir, 0777);
	if(mk_err<0 && errno!=EEXIST){
	    log_append_logfile("Cant create %s\n", whole_dir);
	    return_val = -1;
	    goto clean;
	}    
	while(cur_dir!=NULL){
	    if(strchr(cur_dir,'.')!=NULL)goto next;
	    whole_dir = realloc(whole_dir, sizeof(char)*(strlen(cur_dir)+whole_len+2));
	    if(!whole_dir){
		return_val = -1;
		goto clean;
	    }
	    strcat(whole_dir, cur_dir);
	    strcat(whole_dir, "/");
	    whole_len = strlen(whole_dir);
	    mk_err = mkdir(whole_dir, 0777);
	    if(mk_err<0 && errno!=EEXIST){
		log_append_logfile("Cant create %s\n", whole_dir);
		return_val = -1;
		goto clean;
	    }
	next:
	    cur_dir = strtok(NULL, "/");
	}
    }
    
    fp = fopen(file_path, "w");
    if(!fp){
	log_append_logfile("Cant create file %s\n", file_path);
	return_val = -1;
	goto clean;
    }
    
    fprintf(fp, "%s", json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PRETTY));

clean:
    if(whole_dir)free(whole_dir);
    if(fp)fclose(fp);
    if(dirs)free(dirs);
    return return_val;
}

static char* app_json_read_to_buffer(const char* file_path){
    char* ret_string = NULL;
    FILE* fp;
    fp = fopen(file_path, "r");
    if(!fp){
	return ret_string;
    }

    ret_string = (char*)malloc(sizeof(char)*JSONFILESIZE+1);
    if(!ret_string){
	log_append_logfile("Could not allocate memory for file buffer %s\n", file_path);
	goto clean;		
    }
    size_t read_num = fread(ret_string, sizeof(char), JSONFILESIZE, fp);
    int total_read = read_num;

    char* temp_string = NULL;
    while(read_num>=JSONFILESIZE){
	temp_string = realloc(ret_string, (total_read+read_num)*sizeof(char)+1);
	if(!temp_string){
	    log_append_logfile("Realloc on file %s failed \n", file_path);
	    goto clean;
	}
	ret_string = temp_string;
	read_num = fread(ret_string+total_read, sizeof(char), JSONFILESIZE, fp);
	total_read+=read_num;	 
    }
    temp_string = realloc(ret_string, total_read*sizeof(char)+1);
    //dont forget the null terminator
    temp_string[total_read] = '\0';
    if(!temp_string){
	log_append_logfile("Realloc after reading the file %s failed \n", file_path);
	goto clean;
    }
    ret_string = temp_string;

clean:
    if(fp)fclose(fp);
    return ret_string;
}

struct json_object* app_json_iterate_and_find_obj(struct json_object* parsed_fp, const char* find_key){
    struct json_object* ret_obj = NULL;
    if(!parsed_fp)goto clean;
    
    //iterate through the parsed_fp
    struct json_object_iterator it;
    struct json_object_iterator itEnd;

    it = json_object_iter_begin(parsed_fp);
    itEnd = json_object_iter_end(parsed_fp);
    
    while (!json_object_iter_equal(&it, &itEnd)) {
	if(strcmp(json_object_iter_peek_name(&it), find_key)==0){
	    
	    ret_obj = json_object_iter_peek_value(&it);
	    goto clean;
	}
	struct json_object* rec_obj = NULL;
	rec_obj = json_object_iter_peek_value(&it);
	
	if(json_object_get_type(rec_obj)==json_type_object){
	    ret_obj = app_json_iterate_and_find_obj(rec_obj, find_key);
	    if(ret_obj)goto clean;
	}

	json_object_iter_next(&it);
    }
    
    
clean:
    return ret_obj;
}

static int app_json_iterate_run_callback(struct json_object* parsed_fp,
					 const char* json_name, const char* json_parent, const char* top_name,
					 const char* json_file_path, 
					 void* arg,
					 void(*proc_func)(void*, const char*, const char*, const char*,
							  const char**, const char**, unsigned int)){
    int return_val = 0;

    if(!parsed_fp){
	return_val = -1;
	goto clean;
    }
    //iterate through the parsed_fp
    struct json_object_iterator it;
    struct json_object_iterator itEnd;

    it = json_object_iter_begin(parsed_fp);
    itEnd = json_object_iter_end(parsed_fp);
    //build the attrib_vals and attrib_names string arrays
    const char* attrib_vals[MAX_ATTRIB_ARRAY] = {NULL};
    const char* attrib_names[MAX_ATTRIB_ARRAY] = {NULL};
    
    unsigned int iter = 0;
    while (!json_object_iter_equal(&it, &itEnd)) {
	const char* cur_name =  json_object_iter_peek_name(&it);
	struct json_object* rec_obj = NULL;
	rec_obj = json_object_iter_peek_value(&it);
	//if the key is path, but also the json_file_path is not NULL we nead to update the path attribe
	//it means the json file is created from the default structure
	if(strcmp(cur_name,"path")==0 && json_file_path!=NULL){
	    //although the key is path the value is null, most often this means its the root
	    if(json_object_get_type(rec_obj)==json_type_null){
		json_object_get(rec_obj);
		json_object_put(rec_obj);
		rec_obj = json_object_new_string(json_file_path);
		json_object_object_add(parsed_fp, "path", rec_obj);
	    }
	}
	if(json_object_get_type(rec_obj)==json_type_string){
	    const  char* cur_val_string = json_object_get_string(rec_obj);
	    attrib_vals[iter] = cur_val_string;
	    attrib_names[iter] = cur_name;
	    iter+=1;
	}

	json_object_iter_next(&it);
    }
    (*proc_func)(arg, json_name, json_parent, top_name, attrib_names, attrib_vals, iter);
    //now go through the objects inside this json object
    //and call this function recursevily
    it = json_object_iter_begin(parsed_fp);
    while(!json_object_iter_equal(&it, &itEnd)){
	const char* cur_name =  json_object_iter_peek_name(&it);	
	struct json_object* rec_obj = NULL;
	const char* parent_name = json_name;
	if(iter<=0)parent_name = json_parent;
	rec_obj = json_object_iter_peek_value(&it);
	if(json_object_get_type(rec_obj)==json_type_object)
	    app_json_iterate_run_callback(rec_obj, cur_name, parent_name, top_name,
					  json_file_path, arg, (*proc_func));
	json_object_iter_next(&it);
    }
    
clean:
    return return_val;
}

static void  app_json_mkdir_callback(void* arg, const char* cur_name, const char* parent_name, const char* top_name,
			    const char** attrib_names, const char** attrib_vals, unsigned int attrib_size){
    if(attrib_size>0){
	//if there is a parent name create the dir with that name 
	if(parent_name){
	    int mk_err = mkdir(parent_name, 0777);
	    if(mk_err<0 && errno==EEXIST)
	    log_append_logfile("The dir %s already exists, not creating it \n", parent_name);
	}
	for(int i = 0; i<attrib_size; i++){
	    char* dir = NULL;
	    dir = (char*)malloc(sizeof(char)*strlen(attrib_vals[i])+1);
	    strcpy(dir, attrib_vals[i]);
	    if(!dir)goto next;
	    //if there is a parent name attach this to the begining of the dir 
	    if(parent_name!=NULL){
		dir = realloc(dir, sizeof(char)*strlen(dir)*strlen(parent_name)+2);
		if(!dir)goto next;
		strcpy(dir, parent_name);
		strcat(dir, "/");
		strcat(dir, attrib_vals[i]);
	    }
	    int mk_err = mkdir(dir, 0777);
	    if(mk_err<0 && errno==EEXIST)
		log_append_logfile("The dir %s already exists, not creating it\n", dir);
	next:
	    if(dir)free(dir);
	}
    }

}

void app_json_write_json_callback(void* arg, const char* cur_name, const char* parent_name, const char* top_name,
			    const char** attrib_names, const char** attrib_vals, unsigned int attrib_size){
    if(attrib_size>0){
	struct json_object* obj = (struct json_object*)arg;
	struct json_object* top_obj = NULL;
	struct json_object* parent_obj = NULL;

	top_obj = json_object_new_object();
	if(!top_obj)goto next;
	
	for(int i = 0; i<attrib_size; i++){
	    if(attrib_vals[i] == NULL){
		goto skip;
	    }
	    json_object* jstring = json_object_new_string(attrib_vals[i]);
	    if(!jstring){
		json_object_put(top_obj);
		goto next;
	    }
	    json_object_object_add(top_obj, attrib_names[i], jstring);
	skip:
	}
	//if this node does not have a parent it is a top level node
	if(!parent_name)
	    json_object_object_add(obj, cur_name, top_obj);
	if(parent_name){
	    parent_obj = app_json_iterate_and_find_obj(obj, parent_name);
	    if(!parent_obj){
		json_object_object_add(obj, cur_name, top_obj);
	    }
	    else{
		json_object_object_add(parent_obj, cur_name, top_obj);
	    }
	}
    }
next:

}

int app_json_write_handle_to_file(JSONHANDLE* obj, const char* cur_file,
				  unsigned int new_file, unsigned int preset){
    int return_val = 0;
    struct json_object* j_obj = (struct json_object*)obj;
    struct json_object* parsed_fp = NULL;
    if(!cur_file){
	log_append_logfile("No file to save to\n");
	return_val = -1;
	goto clean_all;
    }
    log_append_logfile("cur_file %s\n", cur_file);
    if(j_obj){
	if(app_json_write_json_to_file(j_obj, cur_file)<0){
	    log_append_logfile("Failed to create the %s file\n", cur_file);
	    return_val = -1;
	    goto clean_all;
	}
    }
    //if we saved or created a new song update the CONFJSONFILE LAST_SONG_KEY to the new song name
    if(preset == 0){
	char* buffer = NULL;
	buffer = app_json_read_to_buffer(CONFJSONFILE);
	if(!buffer){
	    log_append_logfile("Cant read from the %s configuration file\n", CONFJSONFILE);
	}
	parsed_fp = json_tokener_parse(buffer);
	free(buffer);
	if(!parsed_fp){
	    log_append_logfile("Cant parse the json file %s\n", CONFJSONFILE);
	    return_val = -1;
	    goto clean_all;
	}
	struct json_object* song_key = app_json_iterate_and_find_obj(parsed_fp, LAST_SONG_KEY);
	if(!song_key){
	    log_append_logfile("failed to extract the LAST_SONG_KEY\n");
	    return_val = -1;
	    goto clean_all;
	}
	json_object_set_string(song_key, cur_file);
	//now write the CONFJSONFILE again
	if(song_key){
	    if(app_json_write_json_to_file(parsed_fp, CONFJSONFILE)<0){
		log_append_logfile("Failed to write to the %s file\n", cur_file);
		return_val = -1;
		goto clean_all;
	    }
	}	
    }

clean_all:
    if(j_obj)json_object_put(j_obj);
    if(parsed_fp)json_object_put(parsed_fp);
    return return_val;
}

int app_json_create_obj(JSONHANDLE** obj){
    struct json_object* j_obj = json_object_new_object();
    *obj = j_obj;
    if(!obj)return -1;
    return 0;
}

struct json_object* app_json_tokenise_path(char* file_path){
    char* buffer = NULL;
    struct json_object* parsed_fp = NULL;
    
    buffer = app_json_read_to_buffer(file_path);
    if(!buffer){
	log_append_logfile("Cant read from the %s file\n", file_path);
	return NULL;
    }
    parsed_fp = json_tokener_parse(buffer);
    free(buffer);
    if(!parsed_fp){
	log_append_logfile("Cant parse the json file %s\n", file_path);
	return NULL;
    }

    return parsed_fp;
}
