#pragma once
#include <stdbool.h>
#include <stdint.h>

// A time-ordered buffer of MIDI 1.0 messages for one cycle, one per MIDI port.
// Every message is whole: status byte first (no running status), size matching
// it.
// ONLY new/free/dropped_take on [main-thread] others in a thread
// ([audio-thread]) that fills the buffers

// status types, the high nibble of a channel message
enum MidiType {
    MIDI_NOTE_OFF = 0x80,
    MIDI_NOTE_ON = 0x90,
    MIDI_POLY_PRESSURE = 0xA0,
    MIDI_CC = 0xB0,
    MIDI_PROGRAM = 0xC0,
    MIDI_CHANNEL_PRESSURE = 0xD0,
    MIDI_PITCH_BEND = 0xE0,
    MIDI_SYSEX = 0xF0
};

// release velocity a note-on with velocity 0 gets as a note-off
#define MIDI_RELEASE_VELOCITY_DEFAULT 64
// max sources midi_buf_merge takes
#define MIDI_BUF_MERGE_MAX 16

typedef struct _midi_buf MIDI_BUF;

typedef struct _midi_event {
    uint32_t frame;
    uint32_t size;
    const uint8_t *data; // valid until the buffer is cleared
} MIDI_EVENT;

typedef struct _midi_buf_iter {
    const MIDI_BUF *buf;
    uint32_t idx;
} MIDI_BUF_ITER;

typedef struct _midi_buf_merge_iter {
    uint32_t n;
    MIDI_BUF_ITER its[MIDI_BUF_MERGE_MAX];
    MIDI_EVENT heads[MIDI_BUF_MERGE_MAX];
    bool live[MIDI_BUF_MERGE_MAX];
} MIDI_BUF_MERGE_ITER;

// size a message with this status byte has. 0 for a data byte and for sysex,
// whose size is variable
static inline uint32_t midi_expected_size(uint8_t status) {
    if (status < 0x80)
        return 0;
    if (status < 0xF0)
        return ((status & 0xF0) == MIDI_PROGRAM ||
                (status & 0xF0) == MIDI_CHANNEL_PRESSURE)
                   ? 2
                   : 3;
    switch (status) {
    case MIDI_SYSEX:
        return 0;
    case 0xF1: // MTC quarter frame
    case 0xF3: // song select
        return 2;
    case 0xF2: // song position
        return 3;
    default: // tune request, end of sysex, real time
        return 1;
    }
}

// whether size fits the status byte. Sysex is F0 plus at least one byte
static inline bool midi_msg_valid(const uint8_t *data, uint32_t size) {
    if (size == 0)
        return false;
    if (data[0] == MIDI_SYSEX)
        return size >= 2;
    uint32_t expected = midi_expected_size(data[0]);
    return expected != 0 && size == expected;
}

// the decoders below take a valid message (midi_msg_valid, or from a MIDI_BUF)

// MidiType for a channel message, the whole status byte for a system one
static inline uint8_t midi_type(const uint8_t *data) {
    return data[0] < 0xF0 ? data[0] & 0xF0 : data[0];
}

static inline uint8_t midi_channel(const uint8_t *data) {
    return data[0] & 0x0F;
}

static inline bool midi_is_note_on(const uint8_t *data) {
    return midi_type(data) == MIDI_NOTE_ON && data[2] != 0;
}

// a note-on with velocity 0 too - from a MIDI_BUF it is already a note-off
static inline bool midi_is_note_off(const uint8_t *data) {
    return midi_type(data) == MIDI_NOTE_OFF ||
           (midi_type(data) == MIDI_NOTE_ON && data[2] == 0);
}

// capacity in bytes. An event takes 4 + its size, rounded up to 8
// [main-thread]
MIDI_BUF *midi_buf_new(uint32_t capacity);
// [main-thread]
void midi_buf_free(MIDI_BUF *buf);

// empty it for a cycle of nframes, before the cycle's first push
void midi_buf_clear(MIDI_BUF *buf, uint32_t nframes);
// false when dropped: the buffer is full, frame >= nframes, or the size does
// not fit the status byte. A frame earlier than the last pushed one is moved
// up to it, so the order holds and a note-off is never lost. Same frame keeps
// the push order. A note-on with velocity 0 is stored as a note-off
bool midi_buf_push(MIDI_BUF *buf, uint32_t frame, const uint8_t *data,
                   uint32_t size);

uint32_t midi_buf_count(const MIDI_BUF *buf);

MIDI_BUF_ITER midi_buf_iter(const MIDI_BUF *buf);
// next event in frame order, false at the end
bool midi_buf_next(MIDI_BUF_ITER *it, MIDI_EVENT *ev);

// walks the first MIDI_BUF_MERGE_MAX of srcs together, ordered by frame. Same
// frame: the lower source index first. NULL sources are empty
MIDI_BUF_MERGE_ITER midi_buf_merge_iter(const MIDI_BUF *const *srcs,
                                        uint32_t n);
// next event and its index in srcs, false at the end
bool midi_buf_merge_next(MIDI_BUF_MERGE_ITER *it, MIDI_EVENT *ev,
                         uint32_t *src);

// pushes the events of srcs to dst ordered by frame, after what dst holds -
// clear it first. Same frame: the lower source index first. dst must not be
// one of srcs. Sources past MIDI_BUF_MERGE_MAX count as dropped
void midi_buf_merge(MIDI_BUF *dst, const MIDI_BUF *const *srcs, uint32_t n);

// events dropped since the last call
// [main-thread]
uint32_t midi_buf_dropped_take(MIDI_BUF *buf);
