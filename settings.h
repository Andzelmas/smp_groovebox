#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The app's settings file. Only the first one found is read: next to the
// program's binary, then $XDG_CONFIG_HOME/smp_groovebox/, then
// ~/.config/smp_groovebox/. Each reader keeps its own defaults and reads its
// own section once, at startup. [main-thread]

#define SETTINGS_FILE_NAME "settings.json"
#define SETTINGS_DIR_NAME "smp_groovebox"

// every section the file can have, any other top-level key is logged
#define SETTINGS_SECTION_PARAMS "params"
#define SETTINGS_SECTION_PLUGINS "plugins"
#define SETTINGS_SECTION_CLI "cli"
#define SETTINGS_SECTION_SONGS "songs"

typedef enum {
    SETTINGS_STRING,
    // a string; a leading ~ is $HOME, a relative path is relative to the
    // settings file's folder, "" stays ""
    SETTINGS_PATH,
    SETTINGS_BOOL,
    SETTINGS_INT,
    SETTINGS_DOUBLE
} SETTINGS_TYPE;

typedef struct {
    const char *key;
    SETTINGS_TYPE type;
    // char[out_len] for a string or a path, else a bool, int64_t or double
    void *out;
    size_t out_len;
    // the range of a SETTINGS_INT
    int64_t min;
    int64_t max;
} SETTINGS_KEY;

// writes the section's values into the keys' outs. An out keeps what the
// reader put there when the file or the section is missing or broken, or the
// key is missing or does not fit its type; every problem is logged
void settings_read(const char *section, const SETTINGS_KEY *keys,
                   size_t count);
