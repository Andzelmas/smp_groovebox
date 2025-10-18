#pragma once
// Functions that manipulate strings in some specific ways to the app
// infrastructure

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

// from a full path get only the full dir path without the file
char *str_return_dir_without_file(const char *full_path);

// from a full path get only the file name
char *str_return_file_from_path(const char *full_path);

// remove the starting path from the full_path
char *str_return_dir_without_start(const char *full_path);

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
