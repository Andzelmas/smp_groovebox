#pragma once
#include <stddef.h>
#include <stdint.h>
// Functions that manipulate strings in some specific ways to the app
// infrastructure

// FNV-1a 64-bit, for hashing a URI/path/port name into a stable key. The same
// string always hashes to the same value, so a key survives a re-scan that
// moved the item. Not cryptographic - collisions are unlikely, not impossible
uint64_t str_hash_fnv1a64(const char *s);

// a display name told apart from loaded ones of the same name: used holds the
// numbers those have. Writes name for the lowest free number 1, "<num> name"
// for one above. Returns the number, *name_at = where name starts in out
uint32_t str_numbered_name(char *out, size_t cap, const char *name,
                           const uint32_t *used, size_t used_count,
                           size_t *name_at);

// combines a string with integer and padding and _ symbols
// the result is in the string_in, but it is freed if succesfully combined
void str_combine_str_int(char **string_in, int num);

// combines two strings and _ symbol, final result is in string_A,
// but the initial string_A is freed if the string is succesfully combined
void str_combine_str_str(char **string_A, const char *string_B);

// it searches the attrib_names array for the find_name string and
// returns an malloced string that holds a value, that is gotten from the
// attrib_values string array the returned value will have to be freed.
char *str_find_value_from_name(const char *attrib_names[],
                               const char *attrib_values[],
                               const char *find_name, int attrib_size);

// same as str_find_value_from_name but returns a unsigned int for type hex, if
// string not found returns 0
unsigned int str_find_value_to_hex(const char *attrib_names[],
                                   const char *attrib_values[],
                                   const char *find_name, int attrib_size);

// same as str_find_value_from_name but returns a int, if string not found
// returns -1
int str_find_value_to_int(const char *attrib_names[],
                          const char *attrib_values[], const char *find_name,
                          int attrib_size);

// returns float if string not found returns -1
float str_find_value_to_float(const char *attrib_names[],
                              const char *attrib_values[],
                              const char *find_name, int attrib_size);

// from a full path get only the file name
char *str_return_file_from_path(const char *full_path);

// return a string before and after the delimeter
// return_char_sizes is the sizes of the before_delim and after_delim strings
// after_delim is "" if there is nothing after delim
// returns -1 if delim is not found or on failure
int str_split_string_delim(const char *in_string, const char *delim,
                           char *before_delim, char *after_delim,
                           unsigned int return_char_sizes);

// appends to a string a given string, makes use of realloc and malloc. Accepts
// a string and vars like printf
int str_append_to_string(char **append_string, const char *in_string, ...);
