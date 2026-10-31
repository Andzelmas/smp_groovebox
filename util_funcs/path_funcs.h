#pragma once
#include <stddef.h>
#include <stdint.h>
//return if path is directory (1) or a file (0)
int path_is_directory(const char* path);
//return if path (with the file name and extension) matches the file_ext file extension
int path_extension_matches(const char* full_path, const char* file_ext);
//check if there is at least one file in the path, matching file_ext extensions (or any file if file_ext == NULL)
//or directory if dir==1 (will not consider "." and ".." as directories)
int path_has_dir(const char* path, char** file_ext, uint32_t ext_count, uint32_t is_dir);
//copy path into out with a leading ~ replaced by $HOME. -1 when it does not fit,
//or when path starts with ~ and HOME is not set
int path_expand_home(const char* path, char* out, size_t out_len);
//the folder of this program's own binary, -1 when it can not be read or does not fit
int path_exe_dir(char* out, size_t out_len);
//make the folder path and every missing one above it, like mkdir -p. -1 (errno
//set) when path is not a folder afterwards
int path_make_dirs(const char* path);
//name as one file name into out: every char outside [A-Za-z0-9._-], and a leading '.',
//as '_'. -1 when name is empty or does not fit
int path_safe_name(const char* name, char* out, size_t out_len);
//the user's data folder: $XDG_DATA_HOME when it is absolute, else ~/.local/share. -1
//when HOME is not set or it does not fit
int path_user_data_dir(char* out, size_t out_len);
//remove the folder path and everything in it. A symlink is removed, never what it
//points to. -1 (errno set) when something could not be removed
int path_remove_tree(const char* path);
//copy the file src to dst through dst.tmp renamed over dst, so a crash leaves the old
//dst or the copy; a symlink at dst is replaced by the file. -1 (errno set) on failure,
//dst untouched
int path_copy_file(const char* src, const char* dst);
