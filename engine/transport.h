#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../contexts/params.h"
#include "../structs.h"
#include "position.h"

// The app's own clock: tempo, meter, rolling and the song position. One integer
// frame counter; beats, bars and ticks are derived from it, never summed. Knows
// no backend. Every function here is [main-thread], except the *_rt ones and
// the helpers (pure functions of a TRANSPORT_POS).

typedef struct _transport TRANSPORT;

// the Tick param's resolution
#define TRANSPORT_TICKS_PER_BEAT 1920

// one cycle's position (transport_get_rt)
typedef struct _transport_pos {
    bool rolling;
    int64_t frame;      // song position at the cycle's first frame
    NFRAMES_T nframes;  // the cycle's length
    POSITION_MUSIC music; // at frame
    double beats_per_frame;
    SAMPLE_T rate;
    // bumped by every jump: locate, start, stop, tempo, meter or rate change,
    // ours or the server's
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

// the points i * step beats inside one cycle, see transport_grid
typedef struct _transport_grid {
    const TRANSPORT_POS *pos;
    double step;
    int64_t idx; // the next point's i
} TRANSPORT_GRID;

// the Sync param: how the clock relates to the server's transport. Follow and
// Lead both take its rolling and frame; Lead also gives it the musical position
typedef enum {
    TRANSPORT_SYNC_OFF = 0,
    TRANSPORT_SYNC_FOLLOW,
    TRANSPORT_SYNC_LEAD,
} TransportSync;

typedef enum {
    TRANSPORT_REQ_START,
    TRANSPORT_REQ_STOP,
    TRANSPORT_REQ_LOCATE,
} TransportReqKind;

// what the server's transport has to do while synced: Play, Stop and Bar/Beat
// edits only take effect once the server reports them. Decided on
// [audio-thread], which knows the state
typedef struct _transport_req {
    TransportReqKind kind;
    int64_t frame; // TRANSPORT_REQ_LOCATE
} TRANSPORT_REQ;

// stopped at beat 0. NULL on failure
TRANSPORT *transport_init(SAMPLE_T rate);
// transport_cycle_rt must not run any more
void transport_clean(TRANSPORT *transport);
// the Trk params: Tempo, Bar, Beat, Tick (readonly), BPB, Beat_Type, Sync. A
// Bar or Beat edit locates, changing only that field of the current position.
// Tempo, BPB and Beat_Type are readonly while another program leads
PRM_CONTAIN *transport_param_container(TRANSPORT *transport);
// the Sync param's ui value
TransportSync transport_sync(TRANSPORT *transport);
// sets the Sync param as a ui edit does. -1 on failure
int transport_sync_set(TRANSPORT *transport, TransportSync sync);
// the next request for the server, in order. false when there is none
bool transport_request_take(TRANSPORT *transport, TRANSPORT_REQ *out);
// applied at the next cycle start - while synced, as requests for the server.
// Stop while stopped goes back to beat 0. -1 when the queue is full
int transport_play(TRANSPORT *transport);
int transport_stop(TRANSPORT *transport);
// applied at the next cycle start, the bar/beat position stays. -1 on failure
int transport_sample_rate_set(TRANSPORT *transport, SAMPLE_T rate);
// as of the last cycle
bool transport_rolling(TRANSPORT *transport);
// the rt->ui param values; Beat's range follows BPB, the readonly flags who
// leads. Once per ui cycle
void transport_update(TRANSPORT *transport);

// [audio-thread] once per cycle, before anything reads the position. server is
// the server's transport (NULL when there is none), ignored while Sync is Off
void transport_cycle_rt(TRANSPORT *transport, NFRAMES_T nframes,
                        const POSITION_SERVER *server);
// [audio-thread] this cycle's position, valid until the next transport_cycle_rt
const TRANSPORT_POS *transport_get_rt(TRANSPORT *transport);
// [audio-thread] after transport_cycle_rt: the musical position at frame, to
// lead the server's transport. A frame outside this cycle is a locate
void transport_music_at_rt(TRANSPORT *transport, int64_t frame,
                           POSITION_MUSIC *out);

double transport_beat_at(const TRANSPORT_POS *pos, int64_t frame);
// the musical position of beat under pos's tempo and meter
void transport_music(const TRANSPORT_POS *pos, double beat,
                     POSITION_MUSIC *out);
// the points i * step beats (step > 0) in pos's grid range
TRANSPORT_GRID transport_grid(const TRANSPORT_POS *pos, double step);
// the next point: its frame offset in the cycle and its i. false when done
bool transport_grid_next(TRANSPORT_GRID *grid, NFRAMES_T *offset,
                         int64_t *idx);
