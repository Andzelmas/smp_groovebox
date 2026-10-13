#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// A position on the song timeline, as the clock, the server's transport and
// their consumers exchange it. Knows nothing else. A beat is a beat_type note,
// as JACK and LV2 count them.

// the beat since the song start, the bar it is in and that bar's first beat,
// the tempo and meter there
typedef struct _position_music {
    double beat;
    int64_t bar; // 0-based
    double bar_start_beat;
    double bpm;
    double bpb; // beats per bar
    double beat_type;
} POSITION_MUSIC;

// the server's transport
typedef struct _position_server {
    bool rolling;
    int64_t frame;
    // the frame jumped since the last read: someone located. The server's own
    // steps (starting, stopping, cycles it skipped calling process) are none
    bool located;
    // another program leads the musical position: music is that programs'
    bool has_music;
    POSITION_MUSIC music;
} POSITION_SERVER;

// values a POSITION_MUSIC can be taken with
static inline bool position_music_valid(const POSITION_MUSIC *music) {
    return isfinite(music->beat) && isfinite(music->bar_start_beat) &&
           music->bpm > 0 && music->bpm <= 10000 && music->bpb >= 1 &&
           music->bpb <= 1024 && music->beat_type >= 1 &&
           music->beat_type <= 1024;
}

// the beat's place in its bar: the beat in the bar and the tick (of
// ticks_per_beat, tick may be NULL) in that beat, both 0-based. Kept inside the
// bar - a bar line from elsewhere may round over
static inline void position_bbt(const POSITION_MUSIC *music,
                                uint32_t ticks_per_beat, uint32_t *beat,
                                uint32_t *tick) {
    double in_bar = music->beat - music->bar_start_beat;
    if (!(in_bar > 0))
        in_bar = 0;
    double last = music->bpb >= 1 ? ceil(music->bpb) - 1 : 0;
    double in_beat = floor(in_bar);
    if (in_beat > last)
        in_beat = last;
    *beat = (uint32_t)in_beat;
    if (!tick)
        return;
    double ticks = (in_bar - in_beat) * ticks_per_beat;
    if (ticks > ticks_per_beat - 1)
        ticks = ticks_per_beat - 1;
    *tick = (uint32_t)ticks;
}
