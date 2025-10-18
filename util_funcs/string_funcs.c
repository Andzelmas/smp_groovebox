#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include "string_funcs.h"
#include "../types.h"

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
	if(!cur_string)continue;
	const char* cur_value = attrib_values[i];
	if(!cur_value)continue;
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

char* str_return_dir_without_file(const char* full_path){
    char *ret_string = NULL;
    if (!full_path)
        return NULL;

    char temp_string[strlen(full_path) + 1];
    strcpy(temp_string, full_path);
    char *last;
    last = strtok(temp_string, "/");
    int ret_len = 0;
    if (last) {
        ret_string = realloc(ret_string, sizeof(char) * (strlen(last) + 2));
        sprintf(ret_string, "%s", last);
        ret_len = strlen(ret_string);
        last = strtok(NULL, "/");
    }
    while (last) {
        if (strchr(last, '.') != NULL)
            goto next;
        ret_string =
            realloc(ret_string, sizeof(char) * (ret_len + strlen(last) + 2));
        if (!ret_string)
            return NULL;
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

int str_split_string_delim(const char *in_string, const char *delim,
                           char *before_delim, char *after_delim,
                           unsigned int return_char_sizes) {

    if (!in_string)
        return -1;
    char *delim_string = strstr(in_string, delim);
    if (!delim_string)
        return -1;

    unsigned int before_end = strlen(in_string) - strlen(delim_string);
    snprintf(before_delim, return_char_sizes, "%s", in_string);
    before_delim[before_end] = '\0';

    int after_size = strlen(in_string) - before_end - strlen(delim);
    if(after_size <= 0) 
        snprintf(after_delim, return_char_sizes, "");
    else
        snprintf(after_delim, return_char_sizes, "%s", delim_string + strlen(delim));

    return 1;
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
