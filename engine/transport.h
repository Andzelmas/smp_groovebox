#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../contexts/params.h"
#include "../structs.h"

// The app's own clock: tempo, meter, rolling and the song position. One integer
// frame counter; beats, bars and ticks are derived from it, never summed. Knows
// no backend. Every function here is [main-thread], except the *_rt ones and
// the helpers (pure functions of a TRANSPORT_POS).

typedef struct _transport TRANSPORT;

// tick resolution of TRANSPORT_BBT (JACK's)
#define TRANSPORT_TICKS_PER_BEAT 1920

// one cycle's position (transport_get_rt). A beat is a beat_type note, as JACK
// and LV2 count them
typedef struct _transport_pos {
    bool rolling;
    int64_t frame;      // song position at the cycle's first frame
    NFRAMES_T nframes;  // the cycle's length
    double beat;        // beats since the song start, at frame
    double beats_per_frame;
    double bpm;
    uint32_t bpb;       // beats per bar
    uint32_t beat_type;
    int64_t bar;        // 0-based, the bar beat is in
    double bar_start_beat;
    SAMPLE_T rate;
    // bumped by every jump: locate, start, stop, tempo, meter or rate change
    uint64_t seq;
    // the mapping the helpers read: beat(f) = anchor_beat + (f - anchor_frame)
    // * beats_per_frame; bars are counted from meter_bar, a bar line at
    // meter_beat
    int64_t anchor_frame;
    double anchor_beat;
    int64_t meter_bar;
    double meter_beat;
    // the beats this cycle's grid covers, (grid_from, grid_to] - the ranges of
    // consecutive cycles join, so no grid point is lost or doubled. Empty while
    // stopped
    double grid_from;
    double grid_to;
} TRANSPORT_POS;

// a position as bar, beat in the bar and tick, all 0-based
typedef struct _transport_bbt {
    int64_t bar;
    uint32_t beat;
    uint32_t tick; // of TRANSPORT_TICKS_PER_BEAT
    double bar_start_beat;
} TRANSPORT_BBT;

// the points i * step beats inside one cycle, see transport_grid
typedef struct _transport_grid {
    const TRANSPORT_POS *pos;
    double step;
    int64_t idx; // the next point's i
} TRANSPORT_GRID;

// stopped at beat 0. NULL on failure
TRANSPORT *transport_init(SAMPLE_T rate);
// transport_cycle_rt must not run any more
void transport_clean(TRANSPORT *transport);
// the Trk params: Tempo, Bar, Beat, Tick (readonly), BPB, Beat_Type. A Bar or
// Beat edit locates, changing only that field of the current position
PRM_CONTAIN *transport_param_container(TRANSPORT *transport);
// applied at the next cycle start. Stop while stopped goes back to beat 0.
// -1 when the queue is full
int transport_play(TRANSPORT *transport);
int transport_stop(TRANSPORT *transport);
// applied at the next cycle start, the bar/beat position stays. -1 on failure
int transport_sample_rate_set(TRANSPORT *transport, SAMPLE_T rate);
// as of the last cycle
bool transport_rolling(TRANSPORT *transport);
// the rt->ui param values; Beat's range follows BPB. Once per ui cycle
void transport_update(TRANSPORT *transport);

// [audio-thread] once per cycle, before anything reads the position
void transport_cycle_rt(TRANSPORT *transport, NFRAMES_T nframes);
// [audio-thread] this cycle's position, valid until the next transport_cycle_rt
const TRANSPORT_POS *transport_get_rt(TRANSPORT *transport);

double transport_beat_at(const TRANSPORT_POS *pos, int64_t frame);
void transport_bbt(const TRANSPORT_POS *pos, double beat, TRANSPORT_BBT *out);
// the points i * step beats (step > 0) in pos's grid range
TRANSPORT_GRID transport_grid(const TRANSPORT_POS *pos, double step);
// the next point: its frame offset in the cycle and its i. false when done
bool transport_grid_next(TRANSPORT_GRID *grid, NFRAMES_T *offset,
                         int64_t *idx);
