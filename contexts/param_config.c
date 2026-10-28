#include "param_config.h"
#include "../util_funcs/json.h"
#include "../util_funcs/log_funcs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PARAM_CONFIG_FORMAT 1
#define PARAM_CONFIG_CATEGORY_SEP '/'

// the "/" path of the category with this uid, "" for 0. malloc'd, NULL on
// error
static char *param_config_category_path(PRM_CONTAIN *param_container,
                                        uint32_t uid) {
    // the NUL, then each segment with its separator - the top one has none
    size_t len = 1;
    for (uint32_t cur = uid; cur != 0;) {
        int cat_id = param_category_find_uid(param_container, cur);
        const char *name = param_category_name(param_container, cat_id);
        if (!name)
            return NULL;
        len += strlen(name) + 1;
        cur = param_category_parent(param_container, cat_id);
    }
    if (uid != 0)
        len--;
    char *path = malloc(len);
    if (!path)
        return NULL;
    size_t end = len - 1;
    path[end] = '\0';
    for (uint32_t cur = uid; cur != 0;) {
        int cat_id = param_category_find_uid(param_container, cur);
        const char *name = param_category_name(param_container, cat_id);
        size_t name_len = strlen(name);
        end -= name_len;
        memcpy(path + end, name, name_len);
        cur = param_category_parent(param_container, cat_id);
        if (cur != 0)
            path[--end] = PARAM_CONFIG_CATEGORY_SEP;
    }
    return path;
}

// json has no NaN / Inf, such a value is left out
static void param_config_set_number(JSON_DOC *doc, JSON_VAL *obj,
                                    const char *key, PARAM_T value) {
    if (isfinite(value))
        json_obj_set(obj, key, json_new_double(doc, value));
}

PARAM_CONFIG_RESULT param_config_write(PRM_CONTAIN *param_container,
                                       const char *owner, const char *path) {
    if (!param_container || !owner || !path)
        return PARAM_CONFIG_FAILED;
    JSON_DOC *doc = json_doc_new();
    if (!doc) {
        log_append_logfile("param_config: %s: out of memory, not written\n",
                           path);
        return PARAM_CONFIG_FAILED;
    }
    JSON_VAL *root = json_new_object(doc);
    json_set_root(doc, root);
    json_obj_set(root, "format", json_new_int(doc, PARAM_CONFIG_FORMAT));
    json_obj_set(root, "owner", json_new_string(doc, owner));
    json_obj_set(root, "load", json_new_bool(doc, false));
    JSON_VAL *params = json_new_object(doc);
    json_obj_set(root, "params", params);

    unsigned int count = param_return_num_params(param_container, 0);
    for (unsigned int val_id = 0; val_id < count && json_doc_ok(doc);
         val_id++) {
        PARAM_USER_ITEM owner_values;
        if (!param_get_owner_values(param_container, (int)val_id,
                                    &owner_values) ||
            owner_values.key[0] == '\0')
            continue;
        JSON_VAL *info = json_new_object(doc);
        json_obj_set(info, "name", json_new_string(doc, owner_values.name));
        param_config_set_number(doc, info, "increment", owner_values.increment);
        param_config_set_number(doc, info, "default", owner_values.def_val);
        param_config_set_number(doc, info, "min",
                                param_get_min(param_container, (int)val_id));
        param_config_set_number(doc, info, "max",
                                param_get_max(param_container, (int)val_id));
        json_obj_set(info, "hidden", json_new_bool(doc, owner_values.hidden));
        char *category = param_config_category_path(param_container,
                                                    owner_values.category_uid);
        json_obj_set(info, "category", json_new_string(doc, category));
        free(category);
        JSON_VAL *entry = json_new_object(doc);
        json_obj_set(entry, "info", info);
        json_obj_set(params, owner_values.key, entry);
    }

    JSON_ERR err;
    bool written = json_write_file(
        doc, path, JSON_WRITE_PRETTY | JSON_WRITE_NO_REPLACE, &err);
    json_doc_free(doc);
    if (written) {
        log_append_logfile("param_config: %s written\n", path);
        return PARAM_CONFIG_OK;
    }
    if (err.code == JSON_ERR_EXISTS)
        return PARAM_CONFIG_EXISTS;
    log_append_logfile("param_config: %s: %s, not written\n", path, err.msg);
    return PARAM_CONFIG_FAILED;
}

static void param_config_log_skip(const char *path, const JSON_VAL *v,
                                  const char *key, const char *field,
                                  const char *want) {
    log_append_logfile("param_config: %s:%u: \"%s\" %s is not %s, skipped\n",
                       path, json_line(v), key, field, want);
}

// the user values of one "params" member into item, a field that does not fit
// is logged and left unset. The strings stay the doc's
static void param_config_read_param(PRM_CONTAIN *param_container,
                                    const char *path, const char *key,
                                    const JSON_VAL *fields,
                                    PARAM_USER_ITEM *item) {
    *item = (PARAM_USER_ITEM){.key = key};
    for (size_t i = 0; i < json_count(fields); i++) {
        const char *field = json_key_at(fields, i);
        const JSON_VAL *v = json_at(fields, i);
        if (strcmp(field, "info") == 0)
            continue;
        if (strcmp(field, "name") == 0) {
            item->name = json_as_string(v);
            if (item->name)
                item->set |= PARAM_USER_NAME;
            else
                param_config_log_skip(path, v, key, field, "a string");
        } else if (strcmp(field, "increment") == 0) {
            double inc;
            if (json_as_double(v, &inc) && inc > 0) {
                item->increment = inc;
                item->set |= PARAM_USER_INCREMENT;
            } else
                param_config_log_skip(path, v, key, field, "a number above 0");
        } else if (strcmp(field, "default") == 0) {
            double def_val;
            if (json_as_double(v, &def_val)) {
                item->def_val = def_val;
                item->set |= PARAM_USER_DEFAULT;
            } else
                param_config_log_skip(path, v, key, field, "a number");
        } else if (strcmp(field, "hidden") == 0) {
            if (json_as_bool(v, &item->hidden))
                item->set |= PARAM_USER_HIDDEN;
            else
                param_config_log_skip(path, v, key, field, "true or false");
        } else if (strcmp(field, "category") == 0) {
            const char *category = json_as_string(v);
            if (category) {
                // interned here, so new categories come in file order
                item->category_uid = param_category_intern_path(
                    param_container, category, PARAM_CONFIG_CATEGORY_SEP);
                item->set |= PARAM_USER_CATEGORY;
            } else
                param_config_log_skip(path, v, key, field, "a string");
        } else
            log_append_logfile("param_config: %s:%u: \"%s\" has unknown "
                               "field \"%s\"\n",
                               path, json_line(v), key, field);
    }
}

static PARAM_CONFIG_RESULT param_config_apply(PRM_CONTAIN *param_container,
                                              const char *owner,
                                              const char *path,
                                              const JSON_VAL *root) {
    if (json_type(root) != JSON_OBJECT) {
        log_append_logfile("param_config: %s: not a json object, not applied\n",
                           path);
        return PARAM_CONFIG_FAILED;
    }
    const JSON_VAL *format = NULL;
    const JSON_VAL *file_owner = NULL;
    const JSON_VAL *load = NULL;
    const JSON_VAL *params = NULL;
    for (size_t i = 0; i < json_count(root); i++) {
        const char *name = json_key_at(root, i);
        const JSON_VAL *v = json_at(root, i);
        if (strcmp(name, "format") == 0)
            format = v;
        else if (strcmp(name, "owner") == 0)
            file_owner = v;
        else if (strcmp(name, "load") == 0)
            load = v;
        else if (strcmp(name, "params") == 0)
            params = v;
        else
            log_append_logfile("param_config: %s:%u: unknown key \"%s\"\n",
                               path, json_line(v), name);
    }

    int64_t format_num;
    if (!json_as_int(format, PARAM_CONFIG_FORMAT, PARAM_CONFIG_FORMAT,
                     &format_num)) {
        log_append_logfile(
            "param_config: %s: \"format\" is not %d, not applied\n", path,
            PARAM_CONFIG_FORMAT);
        return PARAM_CONFIG_FAILED;
    }
    const char *file_owner_name = json_as_string(file_owner);
    if (!file_owner_name || strcmp(file_owner_name, owner) != 0) {
        log_append_logfile(
            "param_config: %s: \"owner\" is not \"%s\", not applied\n", path,
            owner);
        return PARAM_CONFIG_FAILED;
    }
    bool load_on;
    if (!json_as_bool(load, &load_on)) {
        log_append_logfile(
            "param_config: %s: \"load\" is not true or false, not applied\n",
            path);
        return PARAM_CONFIG_FAILED;
    }
    if (!load_on) {
        if (param_user_table_set(param_container, NULL, 0) != 0)
            return PARAM_CONFIG_FAILED;
        return PARAM_CONFIG_NOT_LOADED;
    }
    if (params && json_type(params) != JSON_OBJECT) {
        log_append_logfile(
            "param_config: %s:%u: \"params\" is not an object, not applied\n",
            path, json_line(params));
        return PARAM_CONFIG_FAILED;
    }

    size_t count = json_count(params);
    PARAM_USER_ITEM *items = NULL;
    if (count > 0) {
        items = malloc(count * sizeof(PARAM_USER_ITEM));
        if (!items) {
            log_append_logfile("param_config: %s: out of memory, not applied\n",
                               path);
            return PARAM_CONFIG_FAILED;
        }
    }
    size_t item_count = 0;
    size_t lacking = 0;
    for (size_t i = 0; i < count; i++) {
        const char *key = json_key_at(params, i);
        const JSON_VAL *fields = json_at(params, i);
        if (json_type(fields) != JSON_OBJECT) {
            log_append_logfile(
                "param_config: %s:%u: \"%s\" is not an object, skipped\n", path,
                json_line(fields), key);
            continue;
        }
        if (param_find_key(param_container, key) == -1)
            lacking++;
        param_config_read_param(param_container, path, key, fields,
                                &items[item_count++]);
    }
    // kept in the table, a param the owner adds later takes its entry
    if (lacking > 0)
        log_append_logfile(
            "param_config: %s: %zu params are not on \"%s\" now, kept for "
            "when it adds them\n",
            path, lacking, owner);
    int set =
        param_user_table_set(param_container, items, (unsigned int)item_count);
    free(items);
    if (set != 0) {
        log_append_logfile("param_config: %s: out of memory, not applied\n",
                           path);
        return PARAM_CONFIG_FAILED;
    }
    return PARAM_CONFIG_OK;
}

PARAM_CONFIG_RESULT param_config_read(PRM_CONTAIN *param_container,
                                      const char *owner, const char *path) {
    if (!param_container || !owner || !path)
        return PARAM_CONFIG_FAILED;
    JSON_ERR err;
    JSON_DOC *doc = json_read_file(path, &err);
    if (!doc) {
        if (err.code == JSON_ERR_NOT_FOUND)
            return PARAM_CONFIG_MISSING;
        if (err.code == JSON_ERR_PARSE)
            log_append_logfile("param_config: %s:%u:%u: %s, not applied\n",
                               path, err.line, err.col, err.msg);
        else
            log_append_logfile("param_config: %s: %s, not applied\n", path,
                               err.msg);
        return PARAM_CONFIG_FAILED;
    }
    PARAM_CONFIG_RESULT result =
        param_config_apply(param_container, owner, path, json_root(doc));
    json_doc_free(doc);
    return result;
}
