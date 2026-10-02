#include "midi_buf.h"
#include "uniform_buffer.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// one ub item, starts UB_ALIGN aligned so frame is a plain load
typedef struct _midi_item {
    uint32_t frame;
    uint8_t data[];
} MIDI_ITEM;

typedef struct _midi_buf {
    UB_EVENT *ub;
    uint32_t nframes;
    uint32_t last_frame;
    // written on [audio-thread], taken on [main-thread]
    atomic_uint dropped;
} MIDI_BUF;

MIDI_BUF *midi_buf_new(uint32_t capacity) {
    MIDI_BUF *buf = calloc(1, sizeof(MIDI_BUF));
    if (!buf)
        return NULL;
    // an item takes at least UB_ALIGN bytes, so the capacity is the only limit
    buf->ub = ub_init(capacity, capacity / UB_ALIGN);
    if (!buf->ub) {
        free(buf);
        return NULL;
    }
    atomic_init(&buf->dropped, 0);
    return buf;
}

void midi_buf_free(MIDI_BUF *buf) {
    if (!buf)
        return;
    ub_clean(buf->ub);
    free(buf);
}

void midi_buf_clear(MIDI_BUF *buf, uint32_t nframes) {
    if (!buf)
        return;
    ub_list_reset(buf->ub);
    buf->nframes = nframes;
    buf->last_frame = 0;
}

bool midi_buf_push(MIDI_BUF *buf, uint32_t frame, const uint8_t *data,
                   uint32_t size) {
    if (!buf)
        return false;
    MIDI_ITEM *item = NULL;
    // the size bound keeps the item size from wrapping
    if (data && frame < buf->nframes &&
        size <= UINT32_MAX - sizeof(MIDI_ITEM) && midi_msg_valid(data, size))
        item = ub_push_reserve(buf->ub, (uint32_t)sizeof(MIDI_ITEM) + size);
    if (!item) {
        atomic_fetch_add(&buf->dropped, 1);
        return false;
    }
    if (frame < buf->last_frame)
        frame = buf->last_frame;
    item->frame = frame;
    memcpy(item->data, data, size);
    if (midi_type(item->data) == MIDI_NOTE_ON && item->data[2] == 0) {
        item->data[0] = MIDI_NOTE_OFF | midi_channel(item->data);
        item->data[2] = MIDI_RELEASE_VELOCITY_DEFAULT;
    }
    buf->last_frame = frame;
    return true;
}

MIDI_BUF_ITER midi_buf_iter(const MIDI_BUF *buf) {
    MIDI_BUF_ITER it = {.buf = buf, .idx = 0};
    return it;
}

bool midi_buf_next(MIDI_BUF_ITER *it, MIDI_EVENT *ev) {
    if (!it || !it->buf || !ev)
        return false;
    UB_EVENT *ub = it->buf->ub;
    const MIDI_ITEM *item = ub_item_get(ub, it->idx);
    if (!item)
        return false;
    ev->frame = item->frame;
    ev->size = ub_item_get_size(ub, it->idx) - (uint32_t)sizeof(MIDI_ITEM);
    ev->data = item->data;
    it->idx += 1;
    return true;
}

void midi_buf_merge(MIDI_BUF *dst, const MIDI_BUF *const *srcs, uint32_t n) {
    if (!dst || !srcs)
        return;
    uint32_t used = n < MIDI_BUF_MERGE_MAX ? n : MIDI_BUF_MERGE_MAX;
    MIDI_BUF_ITER its[MIDI_BUF_MERGE_MAX];
    MIDI_EVENT heads[MIDI_BUF_MERGE_MAX];
    bool live[MIDI_BUF_MERGE_MAX];
    for (uint32_t i = 0; i < used; i++) {
        its[i] = midi_buf_iter(srcs[i]);
        live[i] = midi_buf_next(&its[i], &heads[i]);
    }
    while (1) {
        // strict < keeps the lower source first on equal frames
        int best = -1;
        for (uint32_t i = 0; i < used; i++) {
            if (live[i] && (best == -1 || heads[i].frame < heads[best].frame))
                best = (int)i;
        }
        if (best == -1)
            break;
        midi_buf_push(dst, heads[best].frame, heads[best].data,
                      heads[best].size);
        live[best] = midi_buf_next(&its[best], &heads[best]);
    }
    for (uint32_t i = used; i < n; i++) {
        if (srcs[i])
            atomic_fetch_add(&dst->dropped, ub_size(srcs[i]->ub));
    }
}

uint32_t midi_buf_dropped_take(MIDI_BUF *buf) {
    if (!buf)
        return 0;
    return atomic_exchange(&buf->dropped, 0);
}
