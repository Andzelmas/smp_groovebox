#include "transport.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "../types.h"
#include "../util_funcs/ring_buffer.h"

#define TRANSPORT_BAR_MAX 99999
#define TRANSPORT_CMD_QUEUE 32

// owner ids of the Trk params: source-defined, so a saved value can be matched
// back by them, never a positional index. 0 stays "no owner id"
enum trkParamId {
    TRK_PARAM_NONE = 0,
    TRK_PARAM_TEMPO,
    TRK_PARAM_BAR,
    TRK_PARAM_BEAT,
    TRK_PARAM_TICK,
    TRK_PARAM_BPB,
    TRK_PARAM_BEAT_TYPE,
    TRK_PARAM_COUNT
};

typedef enum {
    TRANSPORT_CMD_PLAY,
    TRANSPORT_CMD_STOP,
    TRANSPORT_CMD_RATE,
} TransportCmdKind;

typedef struct _transport_cmd {
    TransportCmdKind kind;
    double value;
} TRANSPORT_CMD;

struct _transport {
    PRM_CONTAIN *params;
    // val_ids by trkParamId
    int val[TRK_PARAM_COUNT];
    // [main-thread] -> [audio-thread]
    RING_BUFFER *cmds;
    // written by [audio-thread] every cycle, read by [main-thread]
    atomic_bool rolling_seen;
    // [main-thread]: the BPB Beat's range was last set for
    PARAM_T beat_range_bpb;
    // the rest is [audio-thread]'s. pos is the clock itself; its derived
    // fields are refreshed at the end of transport_cycle_rt
    TRANSPORT_POS pos;
    // frames the last cycle rolled, added at the next cycle start
    NFRAMES_T advance;
    // the last rolling cycle's grid_to, unless a locate came after it
    double grid_last;
    bool grid_jump;
    unsigned int display_cycles;
    uint64_t display_seq;
};

double transport_beat_at(const TRANSPORT_POS *pos, int64_t frame) {
    return pos->anchor_beat +
           (double)(frame - pos->anchor_frame) * pos->beats_per_frame;
}

void transport_bbt(const TRANSPORT_POS *pos, double beat, TRANSPORT_BBT *out) {
    double bpb = pos->bpb ? (double)pos->bpb : 1.0;
    int64_t bars = (int64_t)floor((beat - pos->meter_beat) / bpb);
    double start = pos->meter_beat + (double)bars * bpb;
    double in_bar = beat - start;
    // a guard: with bar lines on whole beats the quotient never rounds up
    // onto one, a bar line from elsewhere (T2 Follow) might
    if (in_bar < 0)
        in_bar = 0;
    uint32_t in_beat = (uint32_t)in_bar;
    if (in_beat >= (uint32_t)bpb)
        in_beat = (uint32_t)bpb - 1;
    uint32_t tick =
        (uint32_t)((in_bar - (double)in_beat) * TRANSPORT_TICKS_PER_BEAT);
    if (tick >= TRANSPORT_TICKS_PER_BEAT)
        tick = TRANSPORT_TICKS_PER_BEAT - 1;
    out->bar = pos->meter_bar + bars;
    out->beat = in_beat;
    out->tick = tick;
    out->bar_start_beat = start;
}

TRANSPORT_GRID transport_grid(const TRANSPORT_POS *pos, double step) {
    TRANSPORT_GRID grid = {.pos = pos, .step = step, .idx = 0};
    if (!(step > 0))
        return grid;
    // the first point after grid_from
    int64_t idx = (int64_t)floor(pos->grid_from / step) + 1;
    while ((double)(idx - 1) * step > pos->grid_from)
        idx--;
    while ((double)idx * step <= pos->grid_from)
        idx++;
    grid.idx = idx;
    return grid;
}

bool transport_grid_next(TRANSPORT_GRID *grid, NFRAMES_T *offset,
                         int64_t *idx) {
    const TRANSPORT_POS *pos = grid->pos;
    if (!pos || !(grid->step > 0) || pos->nframes == 0)
        return false;
    double point = (double)grid->idx * grid->step;
    if (!(point <= pos->grid_to))
        return false;
    // the first frame at or after the point, kept inside the cycle - the
    // range already decided the point is this cycle's
    double exact = (double)pos->anchor_frame +
                   (point - pos->anchor_beat) / pos->beats_per_frame;
    int64_t frame = (int64_t)ceil(exact);
    if (frame < pos->frame)
        frame = pos->frame;
    if (frame > pos->frame + (int64_t)pos->nframes - 1)
        frame = pos->frame + (int64_t)pos->nframes - 1;
    if (offset)
        *offset = (NFRAMES_T)(frame - pos->frame);
    if (idx)
        *idx = grid->idx;
    grid->idx++;
    return true;
}

static void clock_reanchor(TRANSPORT_POS *pos) {
    pos->anchor_beat = transport_beat_at(pos, pos->frame);
    pos->anchor_frame = pos->frame;
}

static void clock_rate_set(TRANSPORT_POS *pos) {
    pos->beats_per_frame = pos->bpm / (60.0 * (double)pos->rate);
}

static void clock_locate(TRANSPORT *transport, double beat) {
    TRANSPORT_POS *pos = &transport->pos;
    if (!(beat > 0))
        beat = 0;
    // one meter for the whole song: bars before the anchor are in it too
    if (beat < pos->meter_beat) {
        pos->meter_bar = 0;
        pos->meter_beat = 0;
    }
    pos->frame = (int64_t)llround(beat / pos->beats_per_frame);
    pos->anchor_frame = pos->frame;
    pos->anchor_beat = beat;
    pos->seq++;
    transport->grid_jump = true;
}

// a changed param's new value, rt side. false when the ui did not change it
static bool trk_param_changed(TRANSPORT *transport, int param, PARAM_T *out) {
    int val_id = transport->val[param];
    if (param_get_if_changed_rt(transport->params, val_id) != 1)
        return false;
    *out = param_get_value(transport->params, val_id, 1);
    return true;
}

// Bar / Beat edits: only the edited field of the exact position changes
static void clock_field_locate(TRANSPORT *transport) {
    PARAM_T bar_val = 0;
    PARAM_T beat_val = 0;
    bool bar_changed = trk_param_changed(transport, TRK_PARAM_BAR, &bar_val);
    bool beat_changed = trk_param_changed(transport, TRK_PARAM_BEAT, &beat_val);
    if (!bar_changed && !beat_changed)
        return;
    TRANSPORT_POS *pos = &transport->pos;
    double beat = transport_beat_at(pos, pos->frame);
    TRANSPORT_BBT now;
    transport_bbt(pos, beat, &now);
    double in_beat = beat - now.bar_start_beat - (double)now.beat;
    int64_t bar = bar_changed ? (int64_t)bar_val - 1 : now.bar;
    int64_t in_bar = beat_changed ? (int64_t)beat_val - 1 : (int64_t)now.beat;
    if (in_bar >= (int64_t)pos->bpb)
        in_bar = (int64_t)pos->bpb - 1;
    if (in_bar < 0)
        in_bar = 0;
    if (bar < pos->meter_bar) {
        pos->meter_bar = 0;
        pos->meter_beat = 0;
    }
    clock_locate(transport, pos->meter_beat +
                                (double)(bar - pos->meter_bar) * pos->bpb +
                                (double)in_bar + in_beat);
}

static void clock_commands(TRANSPORT *transport) {
    TRANSPORT_POS *pos = &transport->pos;
    TRANSPORT_CMD cmd;
    while (ring_buffer_read(transport->cmds, &cmd, sizeof(cmd)) == 1) {
        switch (cmd.kind) {
        case TRANSPORT_CMD_PLAY:
            if (!pos->rolling) {
                pos->rolling = true;
                pos->seq++;
            }
            break;
        case TRANSPORT_CMD_STOP:
            if (pos->rolling) {
                pos->rolling = false;
                pos->seq++;
            } else {
                clock_locate(transport, 0);
            }
            break;
        case TRANSPORT_CMD_RATE:
            if ((SAMPLE_T)cmd.value == pos->rate)
                break;
            clock_reanchor(pos);
            pos->rate = (SAMPLE_T)cmd.value;
            clock_rate_set(pos);
            pos->seq++;
            break;
        }
    }
}

// tempo and meter edits, before the Bar/Beat ones so those use the new meter
static void clock_params(TRANSPORT *transport) {
    TRANSPORT_POS *pos = &transport->pos;
    PARAM_T val = 0;
    if (trk_param_changed(transport, TRK_PARAM_TEMPO, &val)) {
        // bar/beat stay continuous, frames keep counting
        clock_reanchor(pos);
        pos->bpm = val;
        clock_rate_set(pos);
        pos->seq++;
    }
    if (trk_param_changed(transport, TRK_PARAM_BPB, &val)) {
        // from the current bar's start: the running bar takes the new length
        TRANSPORT_BBT now;
        transport_bbt(pos, transport_beat_at(pos, pos->frame), &now);
        pos->meter_bar = now.bar;
        pos->meter_beat = now.bar_start_beat;
        pos->bpb = (uint32_t)val;
        pos->seq++;
    }
    if (trk_param_changed(transport, TRK_PARAM_BEAT_TYPE, &val)) {
        pos->beat_type = (uint32_t)val;
        pos->seq++;
    }
    clock_field_locate(transport);
}

// the derived fields of pos, for a cycle of nframes
static void clock_derive(TRANSPORT *transport, NFRAMES_T nframes) {
    TRANSPORT_POS *pos = &transport->pos;
    pos->nframes = nframes;
    pos->beat = transport_beat_at(pos, pos->frame);
    TRANSPORT_BBT now;
    transport_bbt(pos, pos->beat, &now);
    pos->bar = now.bar;
    pos->bar_start_beat = now.bar_start_beat;
    if (!pos->rolling || nframes == 0) {
        pos->grid_from = pos->beat;
        pos->grid_to = pos->beat;
        return;
    }
    // a locate starts the grid at its beat, a point there included
    pos->grid_from = transport->grid_jump ? nextafter(pos->beat, -INFINITY)
                                          : transport->grid_last;
    pos->grid_to = transport_beat_at(pos, pos->frame + (int64_t)nframes - 1);
    transport->grid_last = pos->grid_to;
    transport->grid_jump = false;
}

// a display value. jumped: sent even when the rt value already is it - a Bar
// edit sets it before the locate, and an older value in flight to the ui would
// stay there
static void display_set(TRANSPORT *transport, int param, PARAM_T value,
                        bool jumped) {
    int val_id = transport->val[param];
    if (jumped && param_get_value(transport->params, val_id, 1) == value)
        param_send_value_rt(transport->params, val_id);
    else
        param_set_value_rt(transport->params, val_id, value);
}

// Bar/Beat/Tick shown to the ui at its pace, and at once after a jump. Not
// marked changed, so it never reads back as a locate
static void clock_display(TRANSPORT *transport) {
    TRANSPORT_POS *pos = &transport->pos;
    bool jumped = pos->seq != transport->display_seq;
    if (++transport->display_cycles < RT_CYCLES && !jumped)
        return;
    transport->display_cycles = 0;
    transport->display_seq = pos->seq;
    TRANSPORT_BBT now;
    transport_bbt(pos, pos->beat, &now);
    display_set(transport, TRK_PARAM_BAR, (PARAM_T)(now.bar + 1), jumped);
    display_set(transport, TRK_PARAM_BEAT, (PARAM_T)(now.beat + 1), jumped);
    display_set(transport, TRK_PARAM_TICK, (PARAM_T)now.tick, jumped);
}

void transport_cycle_rt(TRANSPORT *transport, NFRAMES_T nframes) {
    if (!transport)
        return;
    TRANSPORT_POS *pos = &transport->pos;
    pos->frame += transport->advance;
    transport->advance = 0;
    param_msgs_process(transport->params, 1);
    clock_commands(transport);
    clock_params(transport);
    clock_derive(transport, nframes);
    clock_display(transport);
    param_rt_resend(transport->params);
    atomic_store(&transport->rolling_seen, pos->rolling);
    if (pos->rolling)
        transport->advance = nframes;
}

const TRANSPORT_POS *transport_get_rt(TRANSPORT *transport) {
    if (!transport)
        return NULL;
    return &transport->pos;
}

TRANSPORT *transport_init(SAMPLE_T rate) {
    if (!(rate > 0))
        return NULL;
    TRANSPORT *transport = (TRANSPORT *)calloc(1, sizeof(TRANSPORT));
    if (!transport)
        return NULL;
    transport->params = params_init_param_container(NULL);
    transport->cmds = ring_buffer_init(sizeof(TRANSPORT_CMD), TRANSPORT_CMD_QUEUE);
    if (!transport->params || !transport->cmds) {
        transport_clean(transport);
        return NULL;
    }
    // uid 0 - params.c mints one
    PRM_CONTAIN *params = transport->params;
    transport->val[TRK_PARAM_TEMPO] = param_add_param(
        params, "Tempo", 100, 10, 500, 1, 0, TRK_PARAM_TEMPO, 0, 0, NULL);
    transport->val[TRK_PARAM_BAR] = param_add_param(
        params, "Bar", 1, 1, TRANSPORT_BAR_MAX, 1, 0, TRK_PARAM_BAR, 0, 0, NULL);
    transport->val[TRK_PARAM_BEAT] = param_add_param(
        params, "Beat", 1, 1, 4, 1, 0, TRK_PARAM_BEAT, 0, 0, NULL);
    transport->val[TRK_PARAM_TICK] =
        param_add_param(params, "Tick", 0, 0, TRANSPORT_TICKS_PER_BEAT - 1, 1,
                        0, TRK_PARAM_TICK, PARAM_FLAG_READONLY, 0, NULL);
    transport->val[TRK_PARAM_BPB] = param_add_param(
        params, "BPB", 4, 2, 16, 1, 0, TRK_PARAM_BPB, 0, 0, NULL);
    transport->val[TRK_PARAM_BEAT_TYPE] = param_add_param(
        params, "Beat_Type", 4, 2, 16, 1, 0, TRK_PARAM_BEAT_TYPE, 0, 0, NULL);
    // a failed add is -1, which the rt side would index with
    for (int i = TRK_PARAM_NONE + 1; i < TRK_PARAM_COUNT; i++) {
        if (transport->val[i] < 0) {
            transport_clean(transport);
            return NULL;
        }
    }
    transport->beat_range_bpb = 4;
    atomic_init(&transport->rolling_seen, false);

    TRANSPORT_POS *pos = &transport->pos;
    pos->bpm = 100;
    pos->bpb = 4;
    pos->beat_type = 4;
    pos->rate = rate;
    clock_rate_set(pos);
    // consumers start at 0 = nothing seen yet
    pos->seq = 1;
    transport->grid_jump = true;
    clock_derive(transport, 0);
    return transport;
}

void transport_clean(TRANSPORT *transport) {
    if (!transport)
        return;
    if (transport->params)
        param_clean_param_container(transport->params);
    if (transport->cmds)
        ring_buffer_clean(transport->cmds);
    free(transport);
}

PRM_CONTAIN *transport_param_container(TRANSPORT *transport) {
    if (!transport)
        return NULL;
    return transport->params;
}

static int transport_cmd_send(TRANSPORT *transport, TransportCmdKind kind,
                              double value) {
    if (!transport)
        return -1;
    TRANSPORT_CMD cmd = {.kind = kind, .value = value};
    return ring_buffer_write(transport->cmds, &cmd, sizeof(cmd)) == 1 ? 0 : -1;
}

int transport_play(TRANSPORT *transport) {
    return transport_cmd_send(transport, TRANSPORT_CMD_PLAY, 0);
}

int transport_stop(TRANSPORT *transport) {
    return transport_cmd_send(transport, TRANSPORT_CMD_STOP, 0);
}

int transport_sample_rate_set(TRANSPORT *transport, SAMPLE_T rate) {
    if (!(rate > 0))
        return -1;
    return transport_cmd_send(transport, TRANSPORT_CMD_RATE, rate);
}

bool transport_rolling(TRANSPORT *transport) {
    if (!transport)
        return false;
    return atomic_load(&transport->rolling_seen);
}

void transport_update(TRANSPORT *transport) {
    if (!transport)
        return;
    PRM_CONTAIN *params = transport->params;
    param_msgs_process(params, 0);
    PARAM_T bpb = param_get_value(params, transport->val[TRK_PARAM_BPB], 0);
    if (bpb == transport->beat_range_bpb)
        return;
    // from the owner: a clamped Beat is not a locate
    if (param_set_range(params, transport->val[TRK_PARAM_BEAT], 1, bpb,
                        true) == 0)
        transport->beat_range_bpb = bpb;
}
