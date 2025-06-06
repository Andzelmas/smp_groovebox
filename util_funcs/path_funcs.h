#pragma once
#include <stdint.h>
//return if path is directory (1) or a file (0)
int path_is_directory(const char* path);
//return if path (with the file name and extension) matches the file_ext file extension
int path_extension_matches(const char* full_path, const char* file_ext);
//check if there is at least one file in the path, matching file_ext extensions (or any file if file_ext == NULL)
//or directory if dir==1 (will not consider "." and ".." as directories)
int path_has_dir(const char* path, char** file_ext, uint32_t ext_count, uint32_t is_dir);
