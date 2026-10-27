#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A strict RFC 8259 JSON document: no comments, no trailing commas, no NaN /
// Infinity, a duplicate key is a parse error. A doc owns every value, key and
// string in it; values are borrowed, valid until json_doc_free. Objects keep
// their members in file / insertion order and are written back in it.
// Every read takes a NULL value as missing, so lookups chain:
// json_as_int(json_get(json_at(arr, 2), "id"), 0, 127, &id)
// Numbers are read and written in the C locale whatever the thread's locale
// is. No globals; a doc is used by one thread at a time.

#define JSON_MAX_DEPTH 256

// 4 space indent, one object member per line, an array of only scalars on
// one line
#define JSON_WRITE_PRETTY 1u
// json_write_file: JSON_ERR_EXISTS when the file is already there
#define JSON_WRITE_NO_REPLACE 2u

typedef struct _json_doc JSON_DOC;
typedef struct _json_val JSON_VAL;

typedef enum {
    JSON_NONE, // a NULL value: a missing key, an index out of range
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JSON_TYPE;

typedef enum {
    JSON_OK,
    JSON_ERR_PARSE,
    JSON_ERR_NOT_FOUND, // the file does not exist
    JSON_ERR_IO,
    JSON_ERR_MEM,
    JSON_ERR_EXISTS,
    // a write of a doc that is not ok, has no root or nests deeper than
    // JSON_MAX_DEPTH
    JSON_ERR_INVALID
} JSON_ERR_CODE;

typedef struct {
    JSON_ERR_CODE code;
    // 1-based, where the parse stopped, col in code points. 0 when the error
    // is not a parse error
    uint32_t line;
    uint32_t col;
    char msg[128];
} JSON_ERR;

// NULL on error, err (may be NULL) says why. text need not be NUL terminated
// and is not kept. A UTF-8 BOM is skipped
JSON_DOC *json_parse(const char *text, size_t len, JSON_ERR *err);
// json_parse over the whole file
JSON_DOC *json_read_file(const char *path, JSON_ERR *err);
void json_doc_free(JSON_DOC *doc);
JSON_VAL *json_root(const JSON_DOC *doc);

JSON_TYPE json_type(const JSON_VAL *v);
// members of an array or an object, else 0
size_t json_count(const JSON_VAL *v);
// an array's item or an object member's value
JSON_VAL *json_at(const JSON_VAL *v, size_t idx);
const char *json_key_at(const JSON_VAL *obj, size_t idx);
// a linear scan - a big object is better walked with json_count /
// json_key_at / json_at
JSON_VAL *json_get(const JSON_VAL *obj, const char *key);
// the line the value starts on in the text, 0 for a built value
uint32_t json_line(const JSON_VAL *v);

// false and *out untouched when v is missing, of another type or out of range
bool json_as_bool(const JSON_VAL *v, bool *out);
// any number
bool json_as_double(const JSON_VAL *v, double *out);
// an integral number in [min, max], 1.0 and 1e3 count
bool json_as_int(const JSON_VAL *v, int64_t min, int64_t max, int64_t *out);
// NULL when not a string. Owned by the doc
const char *json_as_string(const JSON_VAL *v);

// Building: a doc makes values, each is then placed once - as the root, an
// object member or an array item - within its own doc. Parsed values are
// already placed. A failed new or place marks the doc not ok and the writers
// refuse it, so a builder needs no checks:
// json_obj_set(obj, "min", json_new_double(doc, min))

// empty, no root
JSON_DOC *json_doc_new(void);
// false once a new or a place failed
bool json_doc_ok(const JSON_DOC *doc);

JSON_VAL *json_new_object(JSON_DOC *doc);
JSON_VAL *json_new_array(JSON_DOC *doc);
// s is copied. NULL when it is not UTF-8
JSON_VAL *json_new_string(JSON_DOC *doc, const char *s);
JSON_VAL *json_new_int(JSON_DOC *doc, int64_t i);
// NULL for NaN / Inf
JSON_VAL *json_new_double(JSON_DOC *doc, double d);
JSON_VAL *json_new_bool(JSON_DOC *doc, bool b);
JSON_VAL *json_new_null(JSON_DOC *doc);

// A replaced or removed value stays in the doc until it is freed.
bool json_set_root(JSON_DOC *doc, JSON_VAL *v);
// a new key goes last, an existing one keeps its place
bool json_obj_set(JSON_VAL *obj, const char *key, JSON_VAL *v);
bool json_arr_push(JSON_VAL *arr, JSON_VAL *v);
// later members keep their order. false when there is no such key; never
// marks the doc
bool json_obj_remove(JSON_VAL *obj, const char *key);

// malloc'd, NUL terminated and ending with a newline; *len (len may be NULL)
// without the NUL. NULL on error, err (may be NULL) says why
char *json_write_string(const JSON_DOC *doc, uint32_t flags, size_t *len,
                        JSON_ERR *err);
// through path.tmp in the same directory, fsync'd, then renamed over path: a
// crash leaves the old file or the new one. A replaced file keeps its
// permission bits, a symlink at path is replaced by the file
bool json_write_file(const JSON_DOC *doc, const char *path, uint32_t flags,
                     JSON_ERR *err);
