#include "path_funcs.h"
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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

int path_expand_home(const char* path, char* out, size_t out_len){
    if(!path || !out || out_len == 0)return -1;
    const char* home = "";
    const char* rest = path;
    if(path[0] == '~' && (path[1] == '/' || path[1] == '\0')){
	home = getenv("HOME");
	if(!home || home[0] == '\0')return -1;
	rest = path + 1;
    }
    int len = snprintf(out, out_len, "%s%s", home, rest);
    if(len < 0 || (size_t)len >= out_len)return -1;
    return 0;
}

int path_exe_dir(char* out, size_t out_len){
    if(!out || out_len == 0)return -1;
    char exe[MAX_PATH_STRING];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe));
    if(len <= 0 || (size_t)len >= sizeof(exe))return -1;
    exe[len] = '\0';
    char* slash = strrchr(exe, '/');
    if(!slash)return -1;
    *slash = '\0';
    int dir_len = snprintf(out, out_len, "%s", exe);
    if(dir_len < 0 || (size_t)dir_len >= out_len)return -1;
    return 0;
}

//mkdir, fine with a folder already there
static int path_make_dir(const char* dir){
    if(mkdir(dir, 0755) == 0 || errno == EEXIST)return 0;
    int err = errno;
    if(path_is_directory(dir) == 1)return 0;
    errno = err;
    return -1;
}

int path_make_dirs(const char* path){
    if(!path || path[0] == '\0'){
	errno = ENOENT;
	return -1;
    }
    char dir[MAX_PATH_STRING];
    int len = snprintf(dir, sizeof(dir), "%s", path);
    if(len < 0 || (size_t)len >= sizeof(dir)){
	errno = ENAMETOOLONG;
	return -1;
    }
    //every '/' past the first char ends a parent
    for(char* cur = dir + 1; *cur != '\0'; cur++){
	if(*cur != '/')continue;
	*cur = '\0';
	if(path_make_dir(dir) != 0)return -1;
	*cur = '/';
    }
    if(path_make_dir(dir) != 0)return -1;
    if(path_is_directory(dir) != 1){
	errno = ENOTDIR;
	return -1;
    }
    return 0;
}

static int path_safe_char(char c){
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

int path_safe_name(const char* name, char* out, size_t out_len){
    if(!name || !out || name[0] == '\0')return -1;
    size_t len = strlen(name);
    if(len >= out_len)return -1;
    for(size_t i = 0; i < len; i++)
	out[i] = path_safe_char(name[i]) && !(i == 0 && name[i] == '.') ? name[i] : '_';
    out[len] = '\0';
    return 0;
}

int path_user_data_dir(char* out, size_t out_len){
    if(!out || out_len == 0)return -1;
    //a relative or empty XDG_DATA_HOME is ignored
    const char* xdg = getenv("XDG_DATA_HOME");
    int len;
    if(xdg && xdg[0] == '/')
	len = snprintf(out, out_len, "%s", xdg);
    else{
	const char* home = getenv("HOME");
	if(!home || home[0] == '\0')return -1;
	len = snprintf(out, out_len, "%s/.local/share", home);
    }
    if(len < 0 || (size_t)len >= out_len)return -1;
    return 0;
}
