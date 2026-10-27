#include "settings.h"
#include "types.h"
#include "util_funcs/json.h"
#include "util_funcs/log_funcs.h"
#include "util_funcs/path_funcs.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const settings_sections[] = {SETTINGS_SECTION_PARAMS,
                                                SETTINGS_SECTION_CLI};

// dir + sub into dir_out, the settings file in it into path. True when
// something is there - the first found is used even if it can not be read
static bool settings_try(const char *dir, const char *sub, char *dir_out,
                         char *path) {
    int len = snprintf(dir_out, MAX_PATH_STRING, "%s%s", dir, sub);
    if (len < 0 || len >= MAX_PATH_STRING)
        return false;
    len = snprintf(path, MAX_PATH_STRING, "%s/%s", dir_out, SETTINGS_FILE_NAME);
    if (len < 0 || len >= MAX_PATH_STRING)
        return false;
    return access(path, F_OK) == 0;
}

// dir and path are MAX_PATH_STRING long
static bool settings_find(char *dir, char *path) {
    char exe_dir[MAX_PATH_STRING];
    if (path_exe_dir(exe_dir, sizeof(exe_dir)) == 0 &&
        settings_try(exe_dir, "", dir, path))
        return true;
    // a relative or empty XDG_CONFIG_HOME is ignored
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/' &&
        settings_try(xdg, "/" SETTINGS_DIR_NAME, dir, path))
        return true;
    const char *home = getenv("HOME");
    return home && home[0] != '\0' &&
           settings_try(home, "/.config/" SETTINGS_DIR_NAME, dir, path);
}

static bool settings_section_known(const char *name) {
    for (size_t i = 0;
         i < sizeof(settings_sections) / sizeof(settings_sections[0]); i++) {
        if (strcmp(settings_sections[i], name) == 0)
            return true;
    }
    return false;
}

static bool settings_copy(const char *s, const SETTINGS_KEY *key) {
    size_t len = strlen(s);
    if (len >= key->out_len)
        return false;
    memcpy(key->out, s, len + 1);
    return true;
}

// false when the value does not fit the key, its out untouched
static bool settings_value(const char *dir, const SETTINGS_KEY *key,
                           const JSON_VAL *v) {
    const char *s = json_as_string(v);
    switch (key->type) {
    case SETTINGS_STRING:
        return s && settings_copy(s, key);
    case SETTINGS_PATH: {
        char home[MAX_PATH_STRING];
        if (!s || path_expand_home(s, home, sizeof(home)) != 0)
            return false;
        if (home[0] == '/' || home[0] == '\0')
            return settings_copy(home, key);
        char path[MAX_PATH_STRING];
        int len = snprintf(path, sizeof(path), "%s/%s", dir, home);
        return len >= 0 && (size_t)len < sizeof(path) &&
               settings_copy(path, key);
    }
    case SETTINGS_BOOL:
        return json_as_bool(v, key->out);
    case SETTINGS_INT:
        return json_as_int(v, key->min, key->max, key->out);
    case SETTINGS_DOUBLE:
        return json_as_double(v, key->out);
    }
    return false;
}

static void settings_log_bad_value(const char *path, const char *section,
                                   const SETTINGS_KEY *key,
                                   const JSON_VAL *v) {
    char want[64] = "";
    switch (key->type) {
    case SETTINGS_STRING:
        snprintf(want, sizeof(want), "a string under %zu bytes",
                 key->out_len);
        break;
    case SETTINGS_PATH:
        snprintf(want, sizeof(want), "a path under %zu bytes", key->out_len);
        break;
    case SETTINGS_BOOL:
        snprintf(want, sizeof(want), "true or false");
        break;
    case SETTINGS_INT:
        snprintf(want, sizeof(want), "an integer from %" PRId64 " to %" PRId64,
                 key->min, key->max);
        break;
    case SETTINGS_DOUBLE:
        snprintf(want, sizeof(want), "a number");
        break;
    }
    log_append_logfile(
        "settings: %s:%u: \"%s.%s\" is not %s, using its default\n", path,
        json_line(v), section, key->key, want);
}

void settings_read(const char *section, const SETTINGS_KEY *keys,
                   size_t count) {
    char dir[MAX_PATH_STRING];
    char path[MAX_PATH_STRING];
    if (!section || !settings_find(dir, path))
        return;
    JSON_ERR err;
    JSON_DOC *doc = json_read_file(path, &err);
    if (!doc) {
        if (err.code == JSON_ERR_PARSE)
            log_append_logfile(
                "settings: %s:%u:%u: %s, \"%s\" uses its defaults\n", path,
                err.line, err.col, err.msg, section);
        else
            log_append_logfile("settings: %s: %s, \"%s\" uses its defaults\n",
                               path, err.msg, section);
        return;
    }
    log_append_logfile("settings: \"%s\" from %s\n", section, path);
    JSON_VAL *root = json_root(doc);
    if (json_type(root) != JSON_OBJECT) {
        log_append_logfile(
            "settings: %s: not a json object, \"%s\" uses its defaults\n",
            path, section);
        json_doc_free(doc);
        return;
    }
    JSON_VAL *values = NULL;
    for (size_t i = 0; i < json_count(root); i++) {
        const char *name = json_key_at(root, i);
        if (strcmp(name, section) == 0)
            values = json_at(root, i);
        else if (!settings_section_known(name))
            log_append_logfile("settings: %s:%u: unknown section \"%s\"\n",
                               path, json_line(json_at(root, i)), name);
    }
    if (values && json_type(values) != JSON_OBJECT) {
        log_append_logfile(
            "settings: %s:%u: \"%s\" is not an object, it uses its defaults\n",
            path, json_line(values), section);
        values = NULL;
    }
    for (size_t i = 0; i < json_count(values); i++) {
        const char *name = json_key_at(values, i);
        JSON_VAL *v = json_at(values, i);
        const SETTINGS_KEY *key = NULL;
        for (size_t k = 0; k < count && !key; k++) {
            if (strcmp(keys[k].key, name) == 0)
                key = &keys[k];
        }
        if (!key)
            log_append_logfile("settings: %s:%u: unknown key \"%s.%s\"\n",
                               path, json_line(v), section, name);
        else if (!settings_value(dir, key, v))
            settings_log_bad_value(path, section, key, v);
    }
    json_doc_free(doc);
}
