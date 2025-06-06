#include "path_funcs.h"
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>
#include <stdio.h>
#include "../types.h"

int path_is_directory(const char* path){
    struct stat statbuf;
    if(stat(path, &statbuf) == -1)return -1;
    int is_dir = 0;
    if((statbuf.st_mode & S_IFMT) == S_IFDIR)is_dir = 1;
    return is_dir;
}
static int path_is_regular_file(const char* path){
    struct stat statbuf;
    if(stat(path, &statbuf) == -1)return -1;
    int is_file = 0;
    if((statbuf.st_mode & S_IFMT) == S_IFREG)is_file = 1;
    return is_file;
}
static const char *path_get_filename_ext(const char *filename) {
    if(!filename)return NULL;
    const char *dot = strrchr(filename, '.');
    if(!dot || dot == filename) return NULL;
    return dot + 1;
}
int path_extension_matches(const char* full_path, const char* file_ext){
    if(!full_path)return -1;
    if(!file_ext)return -1;
    const char* this_file_ext = path_get_filename_ext(full_path);
    if(!this_file_ext)return -1;
    int found = 0;
    if(strcmp(this_file_ext, file_ext) == 0)found = 1;
    return found;
}
int path_has_dir(const char* path, char** file_ext, uint32_t ext_count, uint32_t is_dir){
    struct dirent* d;
    DIR* dir = opendir(path);
    if(!dir)return -1;
    d = readdir(dir);

    int found = 0;
    while(d != NULL){
	if(strcmp(d->d_name, ".") == 0){
	    d = readdir(dir);
	    continue;
	}
	if(strcmp(d->d_name, "..") == 0){
	    d = readdir(dir);
	    continue;
	}
	char full_path[MAX_PATH_STRING];
	snprintf(full_path, MAX_PATH_STRING, "%s/%s", path, d->d_name);
	if(path_is_regular_file(full_path) && is_dir==0){
	    if(file_ext==NULL){
		found = 1;
		break;
	    }
	    for(uint32_t i = 0; i < ext_count; i++){
		char* cur_ext = file_ext[i];
		if(path_extension_matches(full_path, cur_ext) == 1){
		    found = 1;
		    break;
		}
	    }
	    if(found == 1)break;
	}
	if(path_is_directory(full_path) && is_dir==1){
	    found = 1;
	    break;
	}
	d = readdir(dir);
    }
    closedir(dir);
    return found;
}
