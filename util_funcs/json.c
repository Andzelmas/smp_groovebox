#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JSON_BLOCK_MIN 4096
#define JSON_BLOCK_MAX (1024 * 1024)
#define JSON_STACK_START 64
#define JSON_BUF_START 256
#define JSON_FILE_START 4096
#define JSON_OUT_START 4096
// members of a built container before its first growth
#define JSON_BUILT_START 4
// bytes of a key quoted in an error message
#define JSON_ERR_KEY_MAX 64

typedef struct _json_block {
    struct _json_block *next;
    size_t used;
    size_t cap;
    unsigned char data[];
} JSON_BLOCK;

typedef struct {
    const char *key; // NULL in an array
    JSON_VAL *val;
} JSON_MEMBER;

struct _json_val {
    JSON_DOC *doc;
    uint8_t type; // JSON_TYPE
    bool is_int;  // a number held in i, else in d
    bool placed;
    uint32_t line;
    union {
        bool b;
        int64_t i;
        double d;
        const char *s;
        struct {
            union {
                JSON_VAL **items;
                JSON_MEMBER *members;
            };
            size_t count;
            size_t cap;
        };
    };
};

// every value, key and string lives in the blocks, freed together
struct _json_doc {
    JSON_VAL *root;
    JSON_BLOCK *blocks; // the first one is being filled
    const char *failed; // the first failed new or place, NULL while ok
};

typedef struct {
    const unsigned char *p; // the next byte
    const unsigned char *end;
    const unsigned char *line_start;
    uint32_t line;
    uint32_t depth;
    JSON_DOC *doc;
    // members of the containers being parsed, the innermost on top
    JSON_MEMBER *stack;
    size_t stack_count;
    size_t stack_max;
    // the string being decoded, or a number for strtod
    char *buf;
    size_t buf_max;
    JSON_ERR err;
} JSON_PARSER;

typedef struct {
    locale_t c;
    locale_t old;
} JSON_LOCALE;

typedef struct {
    char *buf;
    size_t len;
    size_t max;
    bool pretty;
    bool failed; // out of memory
} JSON_WRITER;

static void *json_alloc(JSON_DOC *doc, size_t size, size_t align) {
    JSON_BLOCK *head = doc->blocks;
    if (head) {
        size_t pad = -(uintptr_t)(head->data + head->used) & (align - 1);
        size_t left = head->cap - head->used;
        if (pad <= left && size <= left - pad) {
            void *p = head->data + head->used + pad;
            head->used += pad + size;
            return p;
        }
    }
    if (size > SIZE_MAX / 2)
        return NULL;
    size_t need = size + align - 1;
    size_t cap = head ? head->cap * 2 : JSON_BLOCK_MIN;
    if (cap > JSON_BLOCK_MAX)
        cap = JSON_BLOCK_MAX;
    // too big for a regular block: one of its own behind the head, which
    // keeps filling
    bool own = need > cap;
    if (own)
        cap = need;
    JSON_BLOCK *block = malloc(sizeof(JSON_BLOCK) + cap);
    if (!block)
        return NULL;
    block->cap = cap;
    size_t pad = -(uintptr_t)block->data & (align - 1);
    block->used = pad + size;
    if (own && head) {
        block->next = head->next;
        head->next = block;
    } else {
        block->next = head;
        doc->blocks = block;
    }
    return block->data + pad;
}

static bool json_locale_c(JSON_LOCALE *loc) {
    loc->c = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!loc->c)
        return false;
    loc->old = uselocale(loc->c);
    return true;
}

static void json_locale_restore(JSON_LOCALE *loc) {
    uselocale(loc->old);
    freelocale(loc->c);
}

// an error that is not a parse error
static void json_err_set(JSON_ERR *err, JSON_ERR_CODE code, const char *fmt,
                         ...) {
    *err = (JSON_ERR){.code = code};
    va_list args;
    va_start(args, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, args);
    va_end(args);
}

static void json_err_errno(JSON_ERR *err, JSON_ERR_CODE code, int e) {
    json_err_set(err, code, "%s", strerror(e));
}

// a parse error at `at` on the current line. NULL for the callers to return
static JSON_VAL *json_fail(JSON_PARSER *ps, const unsigned char *at,
                           const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(ps->err.msg, sizeof(ps->err.msg), fmt, args);
    va_end(args);
    ps->err.code = JSON_ERR_PARSE;
    ps->err.line = ps->line;
    ps->err.col = 1;
    for (const unsigned char *q = ps->line_start; q < at; q++)
        if ((*q & 0xC0) != 0x80)
            ps->err.col++;
    return NULL;
}

static JSON_VAL *json_fail_mem(JSON_PARSER *ps) {
    json_err_set(&ps->err, JSON_ERR_MEM, "out of memory");
    return NULL;
}

static JSON_VAL *json_fail_unexpected(JSON_PARSER *ps, const char *expected) {
    if (ps->p == ps->end)
        return json_fail(ps, ps->p, "unexpected end of input, expected %s",
                         expected);
    unsigned char c = *ps->p;
    if (c >= 0x20 && c < 0x7F)
        return json_fail(ps, ps->p, "unexpected '%c', expected %s", c,
                         expected);
    return json_fail(ps, ps->p, "unexpected byte 0x%02X, expected %s", c,
                     expected);
}

static int json_peek(const JSON_PARSER *ps) {
    return ps->p < ps->end ? *ps->p : -1;
}

static bool json_is_digit(int c) { return c >= '0' && c <= '9'; }

static void json_skip_ws(JSON_PARSER *ps) {
    for (; ps->p < ps->end; ps->p++) {
        unsigned char c = *ps->p;
        if (c == '\n') {
            ps->line++;
            ps->line_start = ps->p + 1;
        } else if (c != ' ' && c != '\t' && c != '\r')
            return;
    }
}

static bool json_buf_reserve(JSON_PARSER *ps, size_t len) {
    if (len <= ps->buf_max)
        return true;
    size_t max = ps->buf_max ? ps->buf_max : JSON_BUF_START;
    while (max < len)
        max *= 2;
    char *grown = realloc(ps->buf, max);
    if (!grown) {
        json_fail_mem(ps);
        return false;
    }
    ps->buf = grown;
    ps->buf_max = max;
    return true;
}

static bool json_stack_push(JSON_PARSER *ps, const char *key) {
    if (ps->stack_count == ps->stack_max) {
        size_t max = ps->stack_max ? ps->stack_max * 2 : JSON_STACK_START;
        JSON_MEMBER *grown = realloc(ps->stack, max * sizeof(JSON_MEMBER));
        if (!grown) {
            json_fail_mem(ps);
            return false;
        }
        ps->stack = grown;
        ps->stack_max = max;
    }
    ps->stack[ps->stack_count++] = (JSON_MEMBER){.key = key};
    return true;
}

static JSON_VAL *json_val_alloc(JSON_DOC *doc, JSON_TYPE type) {
    JSON_VAL *v = json_alloc(doc, sizeof(JSON_VAL), _Alignof(JSON_VAL));
    if (v)
        *v = (JSON_VAL){.doc = doc, .type = (uint8_t)type};
    return v;
}

// a parsed value, placed where it is in the text
static JSON_VAL *json_val_new(JSON_PARSER *ps, JSON_TYPE type) {
    JSON_VAL *v = json_val_alloc(ps->doc, type);
    if (!v)
        return json_fail_mem(ps);
    v->placed = true;
    v->line = ps->line;
    return v;
}

// ps->buf copied into the doc
static char *json_buf_dup(JSON_PARSER *ps, size_t len) {
    char *s = json_alloc(ps->doc, len + 1, 1);
    if (!s) {
        json_fail_mem(ps);
        return NULL;
    }
    memcpy(s, ps->buf, len + 1);
    return s;
}

// bytes of the valid UTF-8 sequence at p, 0 when it is not one
static size_t json_utf8_len(const unsigned char *p, const unsigned char *end) {
    size_t avail = (size_t)(end - p);
    unsigned char c = p[0];
    size_t len;
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF)
        len = 2;
    else if (c >= 0xE0 && c <= 0xEF)
        len = 3;
    else if (c >= 0xF0 && c <= 0xF4)
        len = 4;
    else
        return 0;
    if (avail < len)
        return 0;
    for (size_t i = 1; i < len; i++)
        if ((p[i] & 0xC0) != 0x80)
            return 0;
    // overlong, a surrogate, above U+10FFFF
    if ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] >= 0xA0) ||
        (c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] >= 0x90))
        return 0;
    return len;
}

static size_t json_utf8_put(char *out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// the longest prefix of s within max bytes that ends on a whole code point
static int json_utf8_cut(const char *s, size_t len, size_t max) {
    if (len <= max)
        return (int)len;
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80)
        max--;
    return (int)max;
}

// the 4 hex digits at p, -1 when they are not
static long json_hex4(const unsigned char *p, const unsigned char *end) {
    if (end - p < 4)
        return -1;
    long cp = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = p[i];
        int h;
        if (json_is_digit(c))
            h = c - '0';
        else if (c >= 'a' && c <= 'f')
            h = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            h = c - 'A' + 10;
        else
            return -1;
        cp = cp << 4 | h;
    }
    return cp;
}

// the escape at q (on its backslash) as a code point, *used its bytes
static bool json_escape(JSON_PARSER *ps, const unsigned char *q, uint32_t *cp,
                        size_t *used) {
    *used = 2;
    switch (ps->end - q >= 2 ? q[1] : 0) {
    case '"':
    case '\\':
    case '/':
        *cp = q[1];
        return true;
    case 'b':
        *cp = '\b';
        return true;
    case 'f':
        *cp = '\f';
        return true;
    case 'n':
        *cp = '\n';
        return true;
    case 'r':
        *cp = '\r';
        return true;
    case 't':
        *cp = '\t';
        return true;
    case 'u':
        break;
    default:
        json_fail(ps, q, "invalid escape");
        return false;
    }
    long hi = json_hex4(q + 2, ps->end);
    if (hi < 0) {
        json_fail(ps, q, "invalid \\u escape");
        return false;
    }
    if (hi == 0) {
        json_fail(ps, q, "\\u0000 is not allowed");
        return false;
    }
    *cp = (uint32_t)hi;
    *used = 6;
    if (hi < 0xD800 || hi > 0xDFFF)
        return true;
    if (hi <= 0xDBFF && ps->end - q >= 8 && q[6] == '\\' && q[7] == 'u') {
        long lo = json_hex4(q + 8, ps->end);
        if (lo < 0) {
            json_fail(ps, q + 6, "invalid \\u escape");
            return false;
        }
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            *cp = 0x10000 + ((uint32_t)(hi - 0xD800) << 10) +
                  (uint32_t)(lo - 0xDC00);
            *used = 12;
            return true;
        }
    }
    json_fail(ps, q, "unpaired surrogate");
    return false;
}

// decode the string ps->p is on into ps->buf, NUL terminated
static bool json_scan_string(JSON_PARSER *ps, size_t *len) {
    const unsigned char *q = ps->p + 1;
    size_t n = 0;
    for (;;) {
        // the longest a step appends, and the NUL
        if (!json_buf_reserve(ps, n + 5))
            return false;
        if (q == ps->end) {
            json_fail(ps, q, "unterminated string");
            return false;
        }
        unsigned char c = *q;
        if (c == '"')
            break;
        if (c == '\\') {
            uint32_t cp;
            size_t used;
            if (!json_escape(ps, q, &cp, &used))
                return false;
            n += json_utf8_put(ps->buf + n, cp);
            q += used;
            continue;
        }
        if (c < 0x20) {
            json_fail(ps, q, "control character in a string");
            return false;
        }
        size_t k = json_utf8_len(q, ps->end);
        if (!k) {
            json_fail(ps, q, "invalid UTF-8");
            return false;
        }
        memcpy(ps->buf + n, q, k);
        n += k;
        q += k;
    }
    ps->buf[n] = '\0';
    ps->p = q + 1;
    *len = n;
    return true;
}

static JSON_VAL *json_parse_string(JSON_PARSER *ps) {
    JSON_VAL *v = json_val_new(ps, JSON_STRING);
    size_t len;
    if (!v || !json_scan_string(ps, &len) || !(v->s = json_buf_dup(ps, len)))
        return NULL;
    return v;
}

// a key not yet in the object whose members start at mark, copied into the doc
static const char *json_parse_key(JSON_PARSER *ps, size_t mark) {
    if (json_peek(ps) != '"') {
        json_fail_unexpected(ps, "a key string");
        return NULL;
    }
    const unsigned char *at = ps->p;
    size_t len;
    if (!json_scan_string(ps, &len))
        return NULL;
    for (size_t i = mark; i < ps->stack_count; i++)
        if (strcmp(ps->stack[i].key, ps->buf) == 0) {
            json_fail(ps, at, "duplicate key \"%.*s\"",
                      json_utf8_cut(ps->buf, len, JSON_ERR_KEY_MAX), ps->buf);
            return NULL;
        }
    return json_buf_dup(ps, len);
}

// an integer literal that fits int64. Not "-0", that is the double -0.0
static bool json_int_literal(const unsigned char *p, const unsigned char *end,
                             int64_t *out) {
    bool neg = *p == '-';
    if (neg)
        p++;
    uint64_t mag = 0;
    for (; p < end; p++) {
        unsigned d = *p - '0';
        if (mag > (UINT64_MAX - d) / 10)
            return false;
        mag = mag * 10 + d;
    }
    if (neg) {
        if (mag == 0 || mag > (uint64_t)INT64_MAX + 1)
            return false;
        *out = -(int64_t)(mag - 1) - 1;
    } else {
        if (mag > INT64_MAX)
            return false;
        *out = (int64_t)mag;
    }
    return true;
}

static JSON_VAL *json_parse_number(JSON_PARSER *ps) {
    const unsigned char *start = ps->p, *q = ps->p, *end = ps->end;
    bool integral = true;
    if (*q == '-')
        q++;
    if (q == end || !json_is_digit(*q))
        return json_fail(ps, q, "expected a digit");
    if (*q == '0') {
        if (++q < end && json_is_digit(*q))
            return json_fail(ps, q, "leading zero");
    } else
        while (q < end && json_is_digit(*q))
            q++;
    if (q < end && *q == '.') {
        integral = false;
        if (++q == end || !json_is_digit(*q))
            return json_fail(ps, q, "expected a digit after '.'");
        while (q < end && json_is_digit(*q))
            q++;
    }
    if (q < end && (*q == 'e' || *q == 'E')) {
        integral = false;
        if (++q < end && (*q == '+' || *q == '-'))
            q++;
        if (q == end || !json_is_digit(*q))
            return json_fail(ps, q, "expected a digit in the exponent");
        while (q < end && json_is_digit(*q))
            q++;
    }
    JSON_VAL *v = json_val_new(ps, JSON_NUMBER);
    if (!v)
        return NULL;
    ps->p = q;
    if (integral && json_int_literal(start, q, &v->i)) {
        v->is_int = true;
        return v;
    }
    size_t len = (size_t)(q - start);
    if (!json_buf_reserve(ps, len + 1))
        return NULL;
    memcpy(ps->buf, start, len);
    ps->buf[len] = '\0';
    v->d = strtod(ps->buf, NULL);
    if (isinf(v->d))
        return json_fail(ps, start, "number out of range");
    return v;
}

static JSON_VAL *json_parse_literal(JSON_PARSER *ps, const char *word,
                                    JSON_TYPE type, bool b) {
    size_t len = strlen(word);
    if ((size_t)(ps->end - ps->p) < len || memcmp(ps->p, word, len) != 0)
        return json_fail(ps, ps->p, "expected %s", word);
    JSON_VAL *v = json_val_new(ps, type);
    if (!v)
        return NULL;
    v->b = b;
    ps->p += len;
    return v;
}

// the members from mark up, moved off the stack into c, sized exactly
static bool json_container_fill(JSON_PARSER *ps, JSON_VAL *c, size_t mark) {
    size_t count = ps->stack_count - mark;
    const JSON_MEMBER *from = ps->stack + mark;
    ps->stack_count = mark;
    c->count = count;
    c->cap = count;
    if (!count)
        return true;
    if (c->type == JSON_OBJECT) {
        c->members = json_alloc(ps->doc, count * sizeof(JSON_MEMBER),
                                _Alignof(JSON_MEMBER));
        if (!c->members)
            return false;
        memcpy(c->members, from, count * sizeof(JSON_MEMBER));
        return true;
    }
    c->items =
        json_alloc(ps->doc, count * sizeof(JSON_VAL *), _Alignof(JSON_VAL *));
    if (!c->items)
        return false;
    for (size_t i = 0; i < count; i++)
        c->items[i] = from[i].val;
    return true;
}

static JSON_VAL *json_parse_value(JSON_PARSER *ps);

static JSON_VAL *json_parse_container(JSON_PARSER *ps, JSON_TYPE type) {
    bool is_obj = type == JSON_OBJECT;
    int closing = is_obj ? '}' : ']';
    if (ps->depth == JSON_MAX_DEPTH)
        return json_fail(ps, ps->p, "nested deeper than %d", JSON_MAX_DEPTH);
    JSON_VAL *c = json_val_new(ps, type);
    if (!c)
        return NULL;
    ps->depth++;
    size_t mark = ps->stack_count;
    ps->p++;
    json_skip_ws(ps);
    while (json_peek(ps) != closing) {
        const char *key = NULL;
        if (is_obj) {
            if (!(key = json_parse_key(ps, mark)))
                return NULL;
            json_skip_ws(ps);
            if (json_peek(ps) != ':')
                return json_fail_unexpected(ps, "':'");
            ps->p++;
            json_skip_ws(ps);
        }
        if (!json_stack_push(ps, key))
            return NULL;
        size_t at = ps->stack_count - 1;
        JSON_VAL *v = json_parse_value(ps);
        if (!v)
            return NULL;
        ps->stack[at].val = v;
        json_skip_ws(ps);
        if (json_peek(ps) == closing)
            break;
        if (json_peek(ps) != ',')
            return json_fail_unexpected(ps,
                                        is_obj ? "',' or '}'" : "',' or ']'");
        const unsigned char *comma = ps->p, *comma_line_start = ps->line_start;
        uint32_t comma_line = ps->line;
        ps->p++;
        json_skip_ws(ps);
        if (json_peek(ps) == closing) {
            ps->line = comma_line;
            ps->line_start = comma_line_start;
            return json_fail(ps, comma, "trailing comma");
        }
    }
    ps->p++;
    ps->depth--;
    if (!json_container_fill(ps, c, mark))
        return json_fail_mem(ps);
    return c;
}

static JSON_VAL *json_parse_value(JSON_PARSER *ps) {
    int c = json_peek(ps);
    switch (c) {
    case '{':
        return json_parse_container(ps, JSON_OBJECT);
    case '[':
        return json_parse_container(ps, JSON_ARRAY);
    case '"':
        return json_parse_string(ps);
    case 't':
        return json_parse_literal(ps, "true", JSON_BOOL, true);
    case 'f':
        return json_parse_literal(ps, "false", JSON_BOOL, false);
    case 'n':
        return json_parse_literal(ps, "null", JSON_NULL, false);
    }
    if (c == '-' || json_is_digit(c))
        return json_parse_number(ps);
    return json_fail_unexpected(ps, "a value");
}

static void json_parse_root(JSON_PARSER *ps) {
    json_skip_ws(ps);
    if (ps->p == ps->end) {
        json_fail(ps, ps->p, "empty input");
        return;
    }
    ps->doc->root = json_parse_value(ps);
    if (!ps->doc->root)
        return;
    json_skip_ws(ps);
    if (ps->p != ps->end)
        json_fail(ps, ps->p, "trailing characters after the value");
}

JSON_DOC *json_parse(const char *text, size_t len, JSON_ERR *err) {
    if (!text) {
        text = "";
        len = 0;
    }
    JSON_PARSER ps = {.p = (const unsigned char *)text, .line = 1};
    ps.end = ps.p + len;
    if (ps.end - ps.p >= 3 && memcmp(ps.p, "\xEF\xBB\xBF", 3) == 0)
        ps.p += 3;
    ps.line_start = ps.p;
    JSON_LOCALE loc;
    if (!(ps.doc = calloc(1, sizeof(JSON_DOC))) || !json_locale_c(&loc))
        json_fail_mem(&ps);
    else {
        json_parse_root(&ps);
        json_locale_restore(&loc);
    }
    free(ps.stack);
    free(ps.buf);
    if (ps.err.code != JSON_OK) {
        json_doc_free(ps.doc);
        ps.doc = NULL;
    }
    if (err)
        *err = ps.err;
    return ps.doc;
}

// the whole file, malloc'd
static char *json_file_text(const char *path, size_t *len, JSON_ERR *err) {
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) {
        int e = path ? errno : EINVAL;
        json_err_errno(err, e == ENOENT ? JSON_ERR_NOT_FOUND : JSON_ERR_IO, e);
        return NULL;
    }
    struct stat st;
    // one byte over the size, so the read that finds the end needs no growth
    size_t max = fstat(fd, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size + 1
                                                       : JSON_FILE_START;
    size_t n = 0;
    char *text = malloc(max);
    while (text) {
        if (n == max) {
            char *grown = realloc(text, max * 2);
            if (!grown) {
                free(text);
                text = NULL;
                break;
            }
            text = grown;
            max *= 2;
        }
        ssize_t got = read(fd, text + n, max - n);
        if (got > 0)
            n += (size_t)got;
        else if (got == 0)
            break;
        else if (errno != EINTR) {
            json_err_errno(err, JSON_ERR_IO, errno);
            free(text);
            close(fd);
            return NULL;
        }
    }
    close(fd);
    if (!text) {
        json_err_set(err, JSON_ERR_MEM, "out of memory");
        return NULL;
    }
    *len = n;
    return text;
}

JSON_DOC *json_read_file(const char *path, JSON_ERR *err) {
    JSON_ERR local;
    size_t len;
    char *text = json_file_text(path, &len, err ? err : &local);
    if (!text)
        return NULL;
    JSON_DOC *doc = json_parse(text, len, err);
    free(text);
    return doc;
}

void json_doc_free(JSON_DOC *doc) {
    if (!doc)
        return;
    JSON_BLOCK *block = doc->blocks;
    while (block) {
        JSON_BLOCK *next = block->next;
        free(block);
        block = next;
    }
    free(doc);
}

JSON_VAL *json_root(const JSON_DOC *doc) { return doc ? doc->root : NULL; }

JSON_TYPE json_type(const JSON_VAL *v) {
    return v ? (JSON_TYPE)v->type : JSON_NONE;
}

size_t json_count(const JSON_VAL *v) {
    JSON_TYPE type = json_type(v);
    return type == JSON_ARRAY || type == JSON_OBJECT ? v->count : 0;
}

JSON_VAL *json_at(const JSON_VAL *v, size_t idx) {
    if (idx >= json_count(v))
        return NULL;
    return v->type == JSON_ARRAY ? v->items[idx] : v->members[idx].val;
}

const char *json_key_at(const JSON_VAL *obj, size_t idx) {
    if (json_type(obj) != JSON_OBJECT || idx >= obj->count)
        return NULL;
    return obj->members[idx].key;
}

JSON_VAL *json_get(const JSON_VAL *obj, const char *key) {
    if (json_type(obj) != JSON_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < obj->count; i++)
        if (strcmp(obj->members[i].key, key) == 0)
            return obj->members[i].val;
    return NULL;
}

uint32_t json_line(const JSON_VAL *v) { return v ? v->line : 0; }

bool json_as_bool(const JSON_VAL *v, bool *out) {
    if (json_type(v) != JSON_BOOL || !out)
        return false;
    *out = v->b;
    return true;
}

bool json_as_double(const JSON_VAL *v, double *out) {
    if (json_type(v) != JSON_NUMBER || !out)
        return false;
    *out = v->is_int ? (double)v->i : v->d;
    return true;
}

bool json_as_int(const JSON_VAL *v, int64_t min, int64_t max, int64_t *out) {
    if (json_type(v) != JSON_NUMBER || !out)
        return false;
    int64_t i = v->i;
    if (!v->is_int) {
        // [-2^63, 2^63) converts without overflow
        if (!(v->d >= -9223372036854775808.0 && v->d < 9223372036854775808.0))
            return false;
        i = (int64_t)v->d;
        if ((double)i != v->d)
            return false;
    }
    if (i < min || i > max)
        return false;
    *out = i;
    return true;
}

const char *json_as_string(const JSON_VAL *v) {
    return json_type(v) == JSON_STRING ? v->s : NULL;
}

// s is UTF-8 throughout, *len its bytes
static bool json_utf8_valid(const char *s, size_t *len) {
    size_t n = strlen(s);
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    while (p < end) {
        size_t k = json_utf8_len(p, end);
        if (!k)
            return false;
        p += k;
    }
    *len = n;
    return true;
}

// keeps the first failure, the one the writers report
static void json_mark(JSON_DOC *doc, const char *why) {
    if (doc && !doc->failed)
        doc->failed = why;
}

JSON_DOC *json_doc_new(void) { return calloc(1, sizeof(JSON_DOC)); }

bool json_doc_ok(const JSON_DOC *doc) { return doc && !doc->failed; }

static JSON_VAL *json_new(JSON_DOC *doc, JSON_TYPE type) {
    if (!doc)
        return NULL;
    JSON_VAL *v = json_val_alloc(doc, type);
    if (!v)
        json_mark(doc, "out of memory");
    return v;
}

JSON_VAL *json_new_object(JSON_DOC *doc) { return json_new(doc, JSON_OBJECT); }

JSON_VAL *json_new_array(JSON_DOC *doc) { return json_new(doc, JSON_ARRAY); }

JSON_VAL *json_new_string(JSON_DOC *doc, const char *s) {
    size_t len;
    if (!s || !json_utf8_valid(s, &len)) {
        json_mark(doc, s ? "a string that is not UTF-8" : "a NULL string");
        return NULL;
    }
    JSON_VAL *v = json_new(doc, JSON_STRING);
    char *copy = v ? json_alloc(doc, len + 1, 1) : NULL;
    if (!copy) {
        json_mark(doc, "out of memory");
        return NULL;
    }
    v->s = memcpy(copy, s, len + 1);
    return v;
}

JSON_VAL *json_new_int(JSON_DOC *doc, int64_t i) {
    JSON_VAL *v = json_new(doc, JSON_NUMBER);
    if (v) {
        v->is_int = true;
        v->i = i;
    }
    return v;
}

JSON_VAL *json_new_double(JSON_DOC *doc, double d) {
    if (!isfinite(d)) {
        json_mark(doc, "a NaN / Inf number");
        return NULL;
    }
    JSON_VAL *v = json_new(doc, JSON_NUMBER);
    if (v)
        v->d = d;
    return v;
}

JSON_VAL *json_new_bool(JSON_DOC *doc, bool b) {
    JSON_VAL *v = json_new(doc, JSON_BOOL);
    if (v)
        v->b = b;
    return v;
}

JSON_VAL *json_new_null(JSON_DOC *doc) { return json_new(doc, JSON_NULL); }

// v is unplaced and from doc; a failure marks doc
static bool json_placeable(JSON_DOC *doc, const JSON_VAL *v) {
    const char *why = NULL;
    if (!v)
        why = "a NULL value";
    else if (v->doc != doc)
        why = "a value from another doc";
    else if (v->placed)
        why = "a value placed twice";
    json_mark(doc, why);
    return !why;
}

// c is of type, else the doc of c (or of v when c is NULL) is marked
static bool json_container_is(JSON_VAL *c, JSON_TYPE type, JSON_VAL *v) {
    if (json_type(c) == type)
        return true;
    json_mark(c   ? c->doc
              : v ? v->doc
                  : NULL,
              type == JSON_OBJECT ? "a set into a non-object"
                                  : "a push into a non-array");
    return false;
}

// room for one more member; an outgrown array stays in the doc
static bool json_container_grow(JSON_VAL *c) {
    if (c->count < c->cap)
        return true;
    size_t size =
        c->type == JSON_OBJECT ? sizeof(JSON_MEMBER) : sizeof(JSON_VAL *);
    size_t cap = c->cap ? c->cap * 2 : JSON_BUILT_START;
    void *grown = cap <= SIZE_MAX / size
                      ? json_alloc(c->doc, cap * size, _Alignof(JSON_MEMBER))
                      : NULL;
    if (!grown) {
        json_mark(c->doc, "out of memory");
        return false;
    }
    if (c->type == JSON_OBJECT) {
        if (c->count)
            memcpy(grown, c->members, c->count * size);
        c->members = grown;
    } else {
        if (c->count)
            memcpy(grown, c->items, c->count * size);
        c->items = grown;
    }
    c->cap = cap;
    return true;
}

bool json_set_root(JSON_DOC *doc, JSON_VAL *v) {
    if (!doc || !json_placeable(doc, v))
        return false;
    v->placed = true;
    doc->root = v;
    return true;
}

bool json_obj_set(JSON_VAL *obj, const char *key, JSON_VAL *v) {
    if (!json_container_is(obj, JSON_OBJECT, v) || !json_placeable(obj->doc, v))
        return false;
    size_t len;
    if (!key || !json_utf8_valid(key, &len)) {
        json_mark(obj->doc, key ? "a key that is not UTF-8" : "a NULL key");
        return false;
    }
    for (size_t i = 0; i < obj->count; i++)
        if (strcmp(obj->members[i].key, key) == 0) {
            obj->members[i].val = v;
            v->placed = true;
            return true;
        }
    char *copy = json_alloc(obj->doc, len + 1, 1);
    if (!copy) {
        json_mark(obj->doc, "out of memory");
        return false;
    }
    if (!json_container_grow(obj))
        return false;
    obj->members[obj->count++] =
        (JSON_MEMBER){.key = memcpy(copy, key, len + 1), .val = v};
    v->placed = true;
    return true;
}

bool json_arr_push(JSON_VAL *arr, JSON_VAL *v) {
    if (!json_container_is(arr, JSON_ARRAY, v) ||
        !json_placeable(arr->doc, v) || !json_container_grow(arr))
        return false;
    arr->items[arr->count++] = v;
    v->placed = true;
    return true;
}

bool json_obj_remove(JSON_VAL *obj, const char *key) {
    if (json_type(obj) != JSON_OBJECT || !key)
        return false;
    for (size_t i = 0; i < obj->count; i++)
        if (strcmp(obj->members[i].key, key) == 0) {
            memmove(obj->members + i, obj->members + i + 1,
                    (obj->count - i - 1) * sizeof(JSON_MEMBER));
            obj->count--;
            return true;
        }
    return false;
}

static void json_put(JSON_WRITER *w, const char *s, size_t n) {
    if (w->failed)
        return;
    if (n > w->max - w->len) {
        size_t max = w->max ? w->max : JSON_OUT_START;
        while (n > max - w->len) {
            if (max > SIZE_MAX / 2) {
                w->failed = true;
                return;
            }
            max *= 2;
        }
        char *grown = realloc(w->buf, max);
        if (!grown) {
            w->failed = true;
            return;
        }
        w->buf = grown;
        w->max = max;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void json_puts(JSON_WRITER *w, const char *s) {
    json_put(w, s, strlen(s));
}

static void json_newline(JSON_WRITER *w, int depth) {
    json_put(w, "\n", 1);
    for (int i = 0; i < depth; i++)
        json_put(w, "    ", 4);
}

// escapes " \ and control chars, the rest of UTF-8 raw
static void json_write_str(JSON_WRITER *w, const char *s) {
    json_put(w, "\"", 1);
    const char *run = s;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        json_put(w, run, (size_t)(s - run));
        run = s + 1;
        char esc[8];
        switch (c) {
        case '"':
            json_put(w, "\\\"", 2);
            break;
        case '\\':
            json_put(w, "\\\\", 2);
            break;
        case '\b':
            json_put(w, "\\b", 2);
            break;
        case '\f':
            json_put(w, "\\f", 2);
            break;
        case '\n':
            json_put(w, "\\n", 2);
            break;
        case '\r':
            json_put(w, "\\r", 2);
            break;
        case '\t':
            json_put(w, "\\t", 2);
            break;
        default:
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            json_put(w, esc, 6);
        }
    }
    json_put(w, run, (size_t)(s - run));
    json_put(w, "\"", 1);
}

static void json_write_number(JSON_WRITER *w, const JSON_VAL *v) {
    char num[32];
    if (v->is_int)
        snprintf(num, sizeof(num), "%" PRId64, v->i);
    else
        // the shortest that reads back to the same bits
        for (int prec = 15; prec <= 17; prec++) {
            snprintf(num, sizeof(num), "%.*g", prec, v->d);
            if (strtod(num, NULL) == v->d)
                break;
        }
    json_puts(w, num);
}

static bool json_scalars_only(const JSON_VAL *arr) {
    for (size_t i = 0; i < arr->count; i++) {
        JSON_TYPE type = json_type(arr->items[i]);
        if (type == JSON_ARRAY || type == JSON_OBJECT)
            return false;
    }
    return true;
}

// depth: containers open around v. false when v nests deeper than
// JSON_MAX_DEPTH
static bool json_write_val(JSON_WRITER *w, const JSON_VAL *v, int depth) {
    switch (v->type) {
    case JSON_NULL:
        json_puts(w, "null");
        return true;
    case JSON_BOOL:
        json_puts(w, v->b ? "true" : "false");
        return true;
    case JSON_NUMBER:
        json_write_number(w, v);
        return true;
    case JSON_STRING:
        json_write_str(w, v->s);
        return true;
    }
    if (depth == JSON_MAX_DEPTH)
        return false;
    bool is_obj = v->type == JSON_OBJECT;
    if (!v->count) {
        json_puts(w, is_obj ? "{}" : "[]");
        return true;
    }
    bool lines = w->pretty && (is_obj || !json_scalars_only(v));
    json_put(w, is_obj ? "{" : "[", 1);
    for (size_t i = 0; i < v->count; i++) {
        if (i)
            json_put(w, ",", 1);
        if (lines)
            json_newline(w, depth + 1);
        else if (i && w->pretty)
            json_put(w, " ", 1);
        if (is_obj) {
            json_write_str(w, v->members[i].key);
            json_puts(w, w->pretty ? ": " : ":");
        }
        if (!json_write_val(w, json_at(v, i), depth + 1))
            return false;
    }
    if (lines)
        json_newline(w, depth);
    json_put(w, is_obj ? "}" : "]", 1);
    return true;
}

char *json_write_string(const JSON_DOC *doc, uint32_t flags, size_t *len,
                        JSON_ERR *err) {
    JSON_ERR local;
    if (!err)
        err = &local;
    *err = (JSON_ERR){0};
    if (!doc || doc->failed || !doc->root) {
        if (doc && doc->failed)
            json_err_set(err, JSON_ERR_INVALID, "the doc is not ok: %s",
                         doc->failed);
        else
            json_err_set(err, JSON_ERR_INVALID, "%s",
                         doc ? "the doc has no root" : "no doc");
        return NULL;
    }
    JSON_WRITER w = {.pretty = flags & JSON_WRITE_PRETTY};
    JSON_LOCALE loc;
    if (!json_locale_c(&loc)) {
        json_err_set(err, JSON_ERR_MEM, "out of memory");
        return NULL;
    }
    bool fits = json_write_val(&w, doc->root, 0);
    json_locale_restore(&loc);
    // the newline and the NUL
    json_put(&w, "\n", 2);
    if (!fits || w.failed) {
        free(w.buf);
        if (!fits)
            json_err_set(err, JSON_ERR_INVALID, "nested deeper than %d",
                         JSON_MAX_DEPTH);
        else
            json_err_set(err, JSON_ERR_MEM, "out of memory");
        return NULL;
    }
    if (len)
        *len = w.len - 1;
    return w.buf;
}

static int json_write_all(int fd, const char *p, size_t len) {
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

// text written to path through path.tmp, the errno of the failed step
static int json_file_put(const char *path, const char *tmp, const char *text,
                         size_t len, bool no_replace) {
    struct stat st;
    bool keep_mode = !no_replace && stat(path, &st) == 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
        return errno;
    int e = 0;
    if (keep_mode && fchmod(fd, st.st_mode & 07777) != 0)
        e = errno;
    if (!e)
        e = json_write_all(fd, text, len);
    if (!e && fsync(fd) != 0)
        e = errno;
    if (close(fd) != 0 && !e)
        e = errno;
    if (!e && (no_replace ? link(tmp, path) : rename(tmp, path)) != 0)
        e = errno;
    // a rename took the tmp with it
    if (e || no_replace)
        unlink(tmp);
    return e;
}

bool json_write_file(const JSON_DOC *doc, const char *path, uint32_t flags,
                     JSON_ERR *err) {
    JSON_ERR local;
    if (!err)
        err = &local;
    if (!path) {
        json_err_errno(err, JSON_ERR_IO, EINVAL);
        return false;
    }
    size_t len;
    char *text = json_write_string(doc, flags, &len, err);
    if (!text)
        return false;
    size_t path_len = strlen(path);
    char *tmp = malloc(path_len + sizeof(".tmp"));
    int e = ENOMEM;
    if (tmp) {
        memcpy(tmp, path, path_len);
        memcpy(tmp + path_len, ".tmp", sizeof(".tmp"));
        e = json_file_put(path, tmp, text, len, flags & JSON_WRITE_NO_REPLACE);
    }
    free(tmp);
    free(text);
    if (e == EEXIST && (flags & JSON_WRITE_NO_REPLACE))
        json_err_errno(err, JSON_ERR_EXISTS, e);
    else if (e == ENOMEM)
        json_err_set(err, JSON_ERR_MEM, "out of memory");
    else if (e)
        json_err_errno(err, JSON_ERR_IO, e);
    return !e;
}
