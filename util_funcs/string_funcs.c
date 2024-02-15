#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include "string_funcs.h"

void str_combine_str_int(char** string_in, int num){
    char* ret_string = NULL;
    if(!(*string_in))return;
    unsigned int stringlen = strlen(*string_in) +1;
    ret_string = (char*)malloc(sizeof(char) * (stringlen));
    if(!ret_string)return;
    
    strcpy(ret_string, *string_in);
    char num_string[20];
    snprintf(num_string, 20, "_%.2d", num);
    stringlen += strlen(num_string) + 1;

    ret_string = realloc(ret_string, sizeof(char) * (stringlen));
    if(!ret_string)return;

    strcat(ret_string, num_string);
    if(ret_string){
	free(*string_in);
	*string_in = ret_string;
    }
}

void str_combine_str_str(char** string_A, const char* string_B){
    char* ret_string = NULL;
    if(!(*string_A) || !string_B)return;
    unsigned int stringlen = strlen(*string_A) +1;
    ret_string = (char*)malloc(sizeof(char) * (stringlen));
    if(!ret_string)return;
    
    strcpy(ret_string, *string_A);

    stringlen += strlen(string_B) + 2;
    ret_string = realloc(ret_string, sizeof(char) * (stringlen));   
    if(!ret_string)return;
    strcat(ret_string, "_");

    strcat(ret_string, string_B);

    if(ret_string){
	free(*string_A);
	*string_A = ret_string;
    }
}

char* str_find_value_from_name(const char* attrib_names[], const char* attrib_values[],
				const char *find_name, int attrib_size){
    char* ret_value = NULL;
    if(attrib_names==NULL || attrib_values==NULL)return ret_value;
    for(int i=0; i<attrib_size; i++){
	const char* cur_string = attrib_names[i];
	const char* cur_value = attrib_values[i];
	if(cur_string==NULL)continue;
	if(cur_value==NULL)continue;
	if(strcmp(cur_string, find_name)==0){
	    ret_value  = (char*)malloc((strlen(cur_value)+1)*sizeof(char));
	    if(!ret_value)return NULL;
	    ret_value = strcpy(ret_value, cur_value);
	    return ret_value;
	}
    }

    return ret_value;
    
}

unsigned int str_find_value_to_hex(const char* attrib_names[], const char* attrib_values[],
				const char *find_name, int attrib_size){
    unsigned int ret_val = 0;
    char* from_string = NULL;
    from_string = str_find_value_from_name(attrib_names, attrib_values, find_name, attrib_size);
    if(!from_string)goto clean;
    ret_val = strtol(from_string, NULL, 16);
    free(from_string);
    
clean:
    return ret_val;
}

int str_find_value_to_int(const char* attrib_names[], const char* attrib_values[],
				const char *find_name, int attrib_size){
    int ret_val = -1;
    char* from_string = NULL;
    from_string = str_find_value_from_name(attrib_names, attrib_values, find_name, attrib_size);
    if(!from_string)goto clean;
    ret_val = strtol(from_string, NULL, 10);
    free(from_string);
    
clean:
    return ret_val;
}

float str_find_value_to_float(const char* attrib_names[], const char* attrib_values[],
				const char *find_name, int attrib_size){
    float ret_val = -1;
    char* from_string = NULL;
    from_string = str_find_value_from_name(attrib_names, attrib_values, find_name, attrib_size);
    if(!from_string)goto clean;
    ret_val = strtof(from_string, NULL);
    free(from_string);
    
clean:
    return ret_val;
}

int str_return_next_after_string(const char* dir, const char* after_string, int preset){
    //create the new song name
    int song_num = -1;
    if(!dir)return song_num;
    for(int i = 1; i<100; i++){
	//first iterate directories and find out what song number we can use
	struct dirent **ep = NULL;
	int n = scandir(dir, &ep, NULL, alphasort);
	//if failed this means the dir does not exist, so we can use any number we want for the preset or song
	//we just have to create the dir
	if(n==-1){
	    return 1;
	}		
	int this_num = -1;
	while(n--){
	    int do_proc = 0;
	    if(preset==0 && ep[n]->d_type==DT_DIR)do_proc = 1;
	    if(preset==1 && ep[n]->d_type==DT_REG)do_proc = 1;
	    if(do_proc == 1){
		if(strcmp(ep[n]->d_name,".")==0)goto next;
		if(strcmp(ep[n]->d_name,"..")==0)goto next;
		char dir_path[strlen(ep[n]->d_name)+1];
		strcpy(dir_path, ep[n]->d_name);
		char* after_delim = NULL;
		char* before_delim = str_split_string_delim(dir_path, after_string, &after_delim);
		if(before_delim)free(before_delim);
		if(after_delim){
		    int cur_num = strtol(after_delim, NULL, 10);
		    if(after_delim)free(after_delim);
		    if(i==cur_num)this_num = cur_num;
		}
	    }
	next:
	    free(ep[n]);
	}
	free(ep);

	if(this_num==-1){
	    song_num = i;
	    break;
	}
    }
    return song_num;
}

char* str_return_dir_without_file(const char* full_path){
    char* ret_string = NULL;
    if(!full_path)return NULL;
    char temp_string[strlen(full_path)+1];
    if(!temp_string)return NULL;
    strcpy(temp_string, full_path);
    char* last;
    last = strtok(temp_string, "/");
    int ret_len = 0;
    if(last){
	ret_string = realloc(ret_string, sizeof(char)*(strlen(last)+2));
	sprintf(ret_string, "%s", last);
	ret_len = strlen(ret_string);
	last = strtok(NULL, "/");
    }
    while(last){
	if(strchr(last, '.')!=NULL)goto next;
	ret_string = realloc(ret_string, sizeof(char)*(ret_len+strlen(last)+2));
	if(!ret_string)return NULL;
	strcat(ret_string, "/");
	strcat(ret_string, last);
	ret_len = strlen(ret_string);
    next:
	last = strtok(NULL, "/");
    }

    
    return ret_string;
}

char* str_return_file_from_path(const char* full_path){
    if(!full_path)return NULL;
    char* ret_string = NULL;
    
    char temp_string[strlen(full_path)+1];
    strcpy(temp_string, full_path);
    
    char* last = NULL;
    char* tok = NULL;
    tok = strtok(temp_string, "/");
    if(!tok)return NULL;
    
    while(tok){
	last = tok;
	tok = strtok(NULL, "/");
    }
    if(last){
	ret_string = (char*)malloc(sizeof(char)* (strlen(last)+1)) ;
	if(!ret_string)return NULL;
	strcpy(ret_string, last);
    }
    
    return ret_string;
}

char* str_return_dir_without_start(const char* full_path){
    char* ret_string = NULL;
    if(!full_path)return NULL;
    char temp_string[strlen(full_path)+1];
    strcpy(temp_string, full_path);
    char* last;
    last = strtok(temp_string, "/");
    int ret_len = 0;
    if(!last)return NULL;
    if(last){
	last = strtok(NULL, "/");
	if(!last)return NULL;
	ret_string = realloc(ret_string, sizeof(char)*(strlen(last)+2));
	sprintf(ret_string, "%s", last);
	ret_len = strlen(ret_string);
	last = strtok(NULL, "/");
    }
    while(last){
	ret_string = realloc(ret_string, sizeof(char)*(ret_len+strlen(last)+2));
	if(!ret_string)return NULL;
	strcat(ret_string, "/");
	strcat(ret_string, last);
	ret_len = strlen(ret_string);
    next:
	last = strtok(NULL, "/");
    }

    return ret_string;
}

char* str_split_string_delim(const char* in_string, const char* delim, char** after_delim){
    if(!in_string)return NULL;
    char* delim_string = strstr(in_string, delim);
    if(!delim_string)return NULL;
    char* ret_string = delim_string + strlen(delim);
   
    unsigned int before_size = strlen(in_string)-strlen(delim_string);
    char* before_delim = (char*)malloc(sizeof(char)*(before_size+1));
    if(!before_delim)return NULL;
    memcpy(before_delim, in_string, before_size);
    before_delim[before_size] = '\0';

    unsigned int after_size = strlen(in_string) - before_size - strlen(delim);
    if(after_size <= 0){
	*after_delim = NULL;
	return before_delim;
    }
    char* ret_after_delim = (char*)malloc(sizeof(char)*(after_size+1));
    if(!ret_after_delim){
	after_delim = NULL;
	return before_delim;
    }
    memcpy(ret_after_delim, ret_string, after_size);
    ret_after_delim[after_size] = '\0';
    *after_delim = ret_after_delim;
    
    return before_delim;
}

int str_append_to_string(char** append_string, const char* in_string, ...){
    if(!in_string)return -1;
    if(!append_string)return -1;
    char buffer[4096];
    va_list args;
    va_start(args, in_string);
    int ret = vsnprintf(buffer, sizeof(buffer), in_string, args);
    va_end(args);
    if(ret<0)return -1;
    char* temp_string = NULL;
    if(!(*append_string)){
	temp_string = (char*)malloc(sizeof(char)*(ret+1));
	if(!temp_string)return -1;
	*append_string = temp_string;
	strcpy(*append_string, buffer);
    }
    else if(*append_string){
	temp_string = realloc(*append_string, sizeof(char)*(strlen(*append_string)+ret+1));
	if(!temp_string)return -1;
	*append_string = temp_string;
	strcat(*append_string, buffer);
    }

    return 0;
}
