#include "clap_ext_state.h"
#include "../../types.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// a preset file starts with it, its NUL included. A new format gets a new one
static const char state_file_mark[] = "smp_groovebox clap preset 1";
// what a preset file's header may take: the mark, the plugin id and the name
#define STATE_HEADER_MAX 2048
// more than any real state, so a runaway plugin or file is cut off
#define STATE_SIZE_MAX ((size_t)1 << 30)
// a preset file's identity starts with it, a preset location never does
#define STATE_ID_MARK '\x1e'

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} STATE_OUT;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} STATE_IN;

static int64_t state_write(const clap_ostream_t *stream, const void *buffer,
                           uint64_t size) {
    STATE_OUT *out = (STATE_OUT *)stream->ctx;
    if (size == 0)
        return 0;
    if (!buffer || size > STATE_SIZE_MAX - out->size)
        return -1;
    if (out->size + size > out->cap) {
        size_t cap = out->cap ? out->cap : 4096;
        while (cap < out->size + size)
            cap *= 2;
        uint8_t *grown = realloc(out->data, cap);
        if (!grown)
            return -1;
        out->data = grown;
        out->cap = cap;
    }
    memcpy(out->data + out->size, buffer, size);
    out->size += size;
    return (int64_t)size;
}

static int64_t state_read(const clap_istream_t *stream, void *buffer,
                          uint64_t size) {
    STATE_IN *in = (STATE_IN *)stream->ctx;
    size_t left = in->size - in->pos;
    size_t n = size < left ? (size_t)size : left;
    if (n == 0)
        return 0;
    if (!buffer)
        return -1;
    memcpy(buffer, in->data + in->pos, n);
    in->pos += n;
    return (int64_t)n;
}

static const clap_plugin_state_context_t *
state_context_ext(const clap_plugin_t *plugin) {
    return plugin->get_extension(plugin, CLAP_EXT_STATE_CONTEXT);
}

static const clap_plugin_state_t *state_ext(const clap_plugin_t *plugin) {
    return plugin->get_extension(plugin, CLAP_EXT_STATE);
}

bool clap_ext_state_available(const clap_plugin_t *plugin) {
    return plugin && (state_context_ext(plugin) || state_ext(plugin));
}

bool clap_ext_state_save(const clap_plugin_t *plugin, uint32_t context,
                         void **data, size_t *size) {
    if (!plugin || !data || !size)
        return false;
    STATE_OUT out = {0};
    const clap_ostream_t stream = {.ctx = &out, .write = state_write};
    const clap_plugin_state_context_t *ext_context = state_context_ext(plugin);
    const clap_plugin_state_t *ext = state_ext(plugin);
    bool saved = false;
    if (ext_context && ext_context->save)
        saved = ext_context->save(plugin, &stream, context);
    else if (ext && ext->save)
        saved = ext->save(plugin, &stream);
    if (!saved) {
        free(out.data);
        return false;
    }
    *data = out.data;
    *size = out.size;
    return true;
}

bool clap_ext_state_load(const clap_plugin_t *plugin, uint32_t context,
                         const void *data, size_t size) {
    if (!plugin || (!data && size > 0))
        return false;
    STATE_IN in = {.data = data, .size = size};
    const clap_istream_t stream = {.ctx = &in, .read = state_read};
    const clap_plugin_state_context_t *ext_context = state_context_ext(plugin);
    const clap_plugin_state_t *ext = state_ext(plugin);
    if (ext_context && ext_context->load)
        return ext_context->load(plugin, &stream, context);
    if (ext && ext->load)
        return ext->load(plugin, &stream);
    return false;
}

bool clap_ext_state_file_write(const char *path, const char *plugin_id,
                               const char *name, const void *data,
                               size_t size) {
    if (!path || !plugin_id || !name || (!data && size > 0))
        return false;
    size_t id_len = strlen(plugin_id) + 1;
    size_t name_len = strlen(name) + 1;
    if (sizeof(state_file_mark) + id_len + name_len > STATE_HEADER_MAX)
        return false;
    // written next to it and renamed over it, so a failed write leaves the old
    // file
    char tmp[MAX_PATH_STRING];
    int len = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (len < 0 || (size_t)len >= sizeof(tmp))
        return false;
    FILE *file = fopen(tmp, "wb");
    if (!file)
        return false;
    bool ok = fwrite(state_file_mark, 1, sizeof(state_file_mark), file) ==
                  sizeof(state_file_mark) &&
              fwrite(plugin_id, 1, id_len, file) == id_len &&
              fwrite(name, 1, name_len, file) == name_len &&
              (size == 0 || fwrite(data, 1, size, file) == size);
    ok = fclose(file) == 0 && ok;
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        remove(tmp);
    return ok;
}

// the name in, and where the state starts after, the first len bytes of a
// preset file. false when they are not the header of plugin_id's
static bool state_file_header(const char *buf, size_t len,
                              const char *plugin_id, const char **name,
                              size_t *state_at) {
    size_t pos = sizeof(state_file_mark);
    if (len < pos || memcmp(buf, state_file_mark, pos) != 0)
        return false;
    const char *id_end = memchr(buf + pos, '\0', len - pos);
    if (!id_end || strcmp(buf + pos, plugin_id) != 0)
        return false;
    pos = (size_t)(id_end - buf) + 1;
    const char *name_end = memchr(buf + pos, '\0', len - pos);
    if (!name_end)
        return false;
    *name = buf + pos;
    *state_at = (size_t)(name_end - buf) + 1;
    return true;
}

bool clap_ext_state_file_name(const char *path, const char *plugin_id,
                              char *name, size_t cap) {
    if (!path || !plugin_id || !name || cap == 0)
        return false;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    char buf[STATE_HEADER_MAX];
    size_t len = fread(buf, 1, sizeof(buf), file);
    fclose(file);
    const char *found;
    size_t state_at;
    if (!state_file_header(buf, len, plugin_id, &found, &state_at))
        return false;
    snprintf(name, cap, "%s", found);
    return true;
}

bool clap_ext_state_file_read(const char *path, const char *plugin_id,
                              void **data, size_t *size) {
    if (!path || !plugin_id || !data || !size)
        return false;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    long file_size = -1;
    if (fseek(file, 0, SEEK_END) == 0)
        file_size = ftell(file);
    if (file_size < 0 || (size_t)file_size > STATE_HEADER_MAX + STATE_SIZE_MAX ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    char *buf = malloc(file_size > 0 ? (size_t)file_size : 1);
    size_t len = buf ? fread(buf, 1, (size_t)file_size, file) : 0;
    fclose(file);
    const char *name;
    size_t state_at;
    if (!buf || len != (size_t)file_size ||
        !state_file_header(buf, len, plugin_id, &name, &state_at)) {
        free(buf);
        return false;
    }
    *size = len - state_at;
    if (*size == 0) {
        free(buf);
        *data = NULL;
        return true;
    }
    memmove(buf, buf + state_at, *size);
    *data = buf;
    return true;
}

size_t clap_ext_state_identity(char *out, size_t cap, const char *path) {
    int len = snprintf(out, cap, "%c%s", STATE_ID_MARK, path ? path : "");
    return len < 0 ? 0 : (size_t)len;
}

const char *clap_ext_state_identity_path(const char *identity) {
    if (!identity || identity[0] != STATE_ID_MARK)
        return NULL;
    return identity + 1;
}
