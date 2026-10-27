#include "transport.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "../types.h"
#include "../util_funcs/ring_buffer.h"

#define TRANSPORT_BAR_MAX 99999
#define TRANSPORT_CMD_QUEUE 32
#define TRANSPORT_REQ_QUEUE 64
// beats a followed beat may differ from where the music so far puts it before
// it is a jump - tick rounding and a tempo change inside a cycle stay below
#define TRANSPORT_FOLLOW_SLACK 0.0625

// owner ids of the Trk params, 0 stays "no owner id"
enum trkParamId {
    TRK_PARAM_NONE = 0,
    TRK_PARAM_TEMPO,
    TRK_PARAM_BAR,
    TRK_PARAM_BEAT,
    TRK_PARAM_TICK,
    TRK_PARAM_BPB,
    TRK_PARAM_BEAT_TYPE,
    TRK_PARAM_SYNC,
    TRK_PARAM_PDC,
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
    // [audio-thread] -> [main-thread]: requests for the server
    RING_BUFFER *reqs;
    // written by [audio-thread] every cycle, read by [main-thread]
    atomic_bool rolling_seen;
    // the same: another program's music is followed
    atomic_bool music_seen;
    // [main-thread]: the BPB Beat's range was last set for
    PARAM_T beat_range_bpb;
    // [main-thread]: the music_seen the readonly flags were last set for
    bool music_flags;
    // the rest is [audio-thread]'s. pos is the clock itself; its derived
    // fields are refreshed at the end of transport_cycle_rt
    TRANSPORT_POS pos;
    // frames the last cycle rolled, added at the next cycle start
    NFRAMES_T advance;
    // the last rolling cycle's grid_to, unless a locate came after it
    double grid_last;
    // the last cycle followed the server
    bool synced;
    bool grid_jump;
    unsigned int display_cycles;
    uint64_t display_seq;
};

double transport_beat_at(const TRANSPORT_POS *pos, int64_t frame) {
    return pos->anchor_beat +
           (double)(frame - pos->anchor_frame) * pos->beats_per_frame;
}

void transport_music(const TRANSPORT_POS *pos, double beat,
                     POSITION_MUSIC *out) {
    double bpb = pos->music.bpb >= 1 ? pos->music.bpb : 1.0;
    int64_t bars = (int64_t)floor((beat - pos->meter_beat) / bpb);
    *out = pos->music;
    out->beat = beat;
    out->bar = pos->meter_bar + bars;
    out->bar_start_beat = pos->meter_beat + (double)bars * bpb;
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
    pos->beats_per_frame = pos->music.bpm / (60.0 * (double)pos->rate);
}

// frame is at beat from here on
static void pos_anchor(TRANSPORT_POS *pos, int64_t frame, double beat) {
    // one meter for the whole song: bars before the anchor are in it too
    if (beat < pos->meter_beat) {
        pos->meter_bar = 0;
        pos->meter_beat = 0;
    }
    pos->frame = frame;
    pos->anchor_frame = frame;
    pos->anchor_beat = beat;
}

static void clock_anchor(TRANSPORT *transport, int64_t frame, double beat) {
    pos_anchor(&transport->pos, frame, beat);
    transport->pos.seq++;
    transport->grid_jump = true;
}

// beat's frame at the current tempo from 0
static int64_t clock_frame_of(const TRANSPORT_POS *pos, double beat) {
    return (int64_t)llround(beat / pos->beats_per_frame);
}

static void clock_locate(TRANSPORT *transport, double beat) {
    if (!(beat > 0))
        beat = 0;
    clock_anchor(transport, clock_frame_of(&transport->pos, beat), beat);
}

// lost when the queue is full - the ui asked faster than it takes them
static void clock_request(TRANSPORT *transport, TransportReqKind kind,
                          int64_t frame) {
    TRANSPORT_REQ req = {.kind = kind, .frame = frame};
    ring_buffer_write(transport->reqs, &req, sizeof(req));
}

// synced, a jump is the server's to make
static void clock_jump(TRANSPORT *transport, double beat, bool synced) {
    if (!(beat > 0))
        beat = 0;
    if (synced)
        clock_request(transport, TRANSPORT_REQ_LOCATE,
                      clock_frame_of(&transport->pos, beat));
    else
        clock_locate(transport, beat);
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
static void clock_field_locate(TRANSPORT *transport, bool synced,
                               bool music) {
    PARAM_T bar_val = 0;
    PARAM_T beat_val = 0;
    bool bar_changed = trk_param_changed(transport, TRK_PARAM_BAR, &bar_val);
    bool beat_changed = trk_param_changed(transport, TRK_PARAM_BEAT, &beat_val);
    if (!bar_changed && !beat_changed)
        return;
    TRANSPORT_POS *pos = &transport->pos;
    POSITION_MUSIC now;
    transport_music(pos, transport_beat_at(pos, pos->frame), &now);
    uint32_t now_beat = 0;
    position_bbt(&now, TRANSPORT_TICKS_PER_BEAT, &now_beat, NULL);
    double in_beat = now.beat - now.bar_start_beat - (double)now_beat;
    int64_t bar = bar_changed ? (int64_t)bar_val - 1 : now.bar;
    int64_t in_bar = beat_changed ? (int64_t)beat_val - 1 : (int64_t)now_beat;
    if (in_bar >= (int64_t)pos->music.bpb)
        in_bar = (int64_t)pos->music.bpb - 1;
    if (in_bar < 0)
        in_bar = 0;
    // at once even when synced: the jump is counted in the new numbering.
    // Followed music keeps its own bar lines
    if (bar < pos->meter_bar && !music) {
        pos->meter_bar = 0;
        pos->meter_beat = 0;
        pos->seq++;
    }
    double bars = (double)(bar - pos->meter_bar) * pos->music.bpb;
    clock_jump(transport, pos->meter_beat + bars + (double)in_bar + in_beat,
               synced);
}

static void clock_commands(TRANSPORT *transport, bool synced) {
    TRANSPORT_POS *pos = &transport->pos;
    TRANSPORT_CMD cmd;
    while (ring_buffer_read(transport->cmds, &cmd, sizeof(cmd)) == 1) {
        switch (cmd.kind) {
        case TRANSPORT_CMD_PLAY:
            if (pos->rolling)
                break;
            if (synced) {
                clock_request(transport, TRANSPORT_REQ_START, 0);
                break;
            }
            pos->rolling = true;
            pos->seq++;
            break;
        case TRANSPORT_CMD_STOP:
            if (synced) {
                clock_request(transport,
                              pos->rolling ? TRANSPORT_REQ_STOP
                                           : TRANSPORT_REQ_LOCATE,
                              0);
            } else if (pos->rolling) {
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

// tempo and meter edits, before the Bar/Beat ones so those use the new meter.
// Taken but not applied while another program's music is followed
static void clock_params(TRANSPORT *transport, bool synced, bool music) {
    TRANSPORT_POS *pos = &transport->pos;
    PARAM_T val = 0;
    if (trk_param_changed(transport, TRK_PARAM_TEMPO, &val) && !music) {
        // bar/beat stay continuous, frames keep counting
        clock_reanchor(pos);
        pos->music.bpm = val;
        clock_rate_set(pos);
        pos->seq++;
    }
    if (trk_param_changed(transport, TRK_PARAM_BPB, &val) && !music) {
        // from the current bar's start: the running bar takes the new length
        POSITION_MUSIC now;
        transport_music(pos, transport_beat_at(pos, pos->frame), &now);
        pos->meter_bar = now.bar;
        pos->meter_beat = now.bar_start_beat;
        pos->music.bpb = floor(val);
        pos->seq++;
    }
    if (trk_param_changed(transport, TRK_PARAM_BEAT_TYPE, &val) && !music) {
        pos->music.beat_type = floor(val);
        pos->seq++;
    }
    clock_field_locate(transport, synced, music);
}

// the server's rolling and frame. A locate by someone, or Sync just turned on,
// anchors at the current tempo from 0 as our own locates; any other frame the
// mapping already covers
static void clock_follow(TRANSPORT *transport, const POSITION_SERVER *server) {
    TRANSPORT_POS *pos = &transport->pos;
    if (server->located || !transport->synced)
        clock_anchor(transport, server->frame,
                     (double)server->frame * pos->beats_per_frame);
    else
        pos->frame = server->frame;
    if (server->rolling != pos->rolling) {
        pos->rolling = server->rolling;
        pos->seq++;
    }
}

// another program's music: this cycle's frame is at its beat, its tempo and
// meter (in whole beats, as ours), its bar's start a bar line
static void clock_music(TRANSPORT *transport, const POSITION_MUSIC *music) {
    TRANSPORT_POS *pos = &transport->pos;
    double bpb = round(music->bpb);
    double beat_type = round(music->beat_type);
    if (fabs(music->beat - transport_beat_at(pos, pos->frame)) >
        TRANSPORT_FOLLOW_SLACK) {
        pos->seq++;
        transport->grid_jump = true;
    } else if (music->bpm != pos->music.bpm || bpb != pos->music.bpb ||
               beat_type != pos->music.beat_type) {
        pos->seq++;
    }
    pos->music.bpm = music->bpm;
    clock_rate_set(pos);
    pos->music.bpb = bpb;
    pos->music.beat_type = beat_type;
    pos->anchor_frame = pos->frame;
    pos->anchor_beat = music->beat;
    pos->meter_bar = music->bar;
    pos->meter_beat = music->bar_start_beat;
}

// the derived fields of pos, for a cycle of nframes
static void clock_derive(TRANSPORT *transport, NFRAMES_T nframes) {
    TRANSPORT_POS *pos = &transport->pos;
    pos->nframes = nframes;
    POSITION_MUSIC now;
    transport_music(pos, transport_beat_at(pos, pos->frame), &now);
    pos->music = now;
    if (!pos->rolling || nframes == 0) {
        pos->grid_from = now.beat;
        pos->grid_to = now.beat;
        return;
    }
    // a locate starts the grid at its beat, a point there included
    pos->grid_from = transport->grid_jump ? nextafter(now.beat, -INFINITY)
                                          : transport->grid_last;
    pos->grid_to = transport_beat_at(pos, pos->frame + (int64_t)nframes - 1);
    // a followed beat can start a hair behind the last cycle's end
    if (pos->grid_to < pos->grid_from)
        pos->grid_to = pos->grid_from;
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

// Bar/Beat/Tick (and followed tempo and meter) shown to the ui at its pace,
// and at once after a jump. Not marked changed, so it never reads back as an
// edit
static void clock_display(TRANSPORT *transport, bool music) {
    TRANSPORT_POS *pos = &transport->pos;
    bool jumped = pos->seq != transport->display_seq;
    if (++transport->display_cycles < RT_CYCLES && !jumped)
        return;
    transport->display_cycles = 0;
    transport->display_seq = pos->seq;
    uint32_t beat = 0;
    uint32_t tick = 0;
    position_bbt(&pos->music, TRANSPORT_TICKS_PER_BEAT, &beat, &tick);
    display_set(transport, TRK_PARAM_BAR, (PARAM_T)(pos->music.bar + 1),
                jumped);
    display_set(transport, TRK_PARAM_BEAT, (PARAM_T)(beat + 1), jumped);
    display_set(transport, TRK_PARAM_TICK, (PARAM_T)tick, jumped);
    if (!music)
        return;
    PRM_CONTAIN *params = transport->params;
    param_set_value_rt(params, transport->val[TRK_PARAM_TEMPO], pos->music.bpm);
    param_set_value_rt(params, transport->val[TRK_PARAM_BPB], pos->music.bpb);
    param_set_value_rt(params, transport->val[TRK_PARAM_BEAT_TYPE],
                       pos->music.beat_type);
}

void transport_cycle_rt(TRANSPORT *transport, NFRAMES_T nframes,
                        const POSITION_SERVER *server) {
    if (!transport)
        return;
    TRANSPORT_POS *pos = &transport->pos;
    pos->frame += transport->advance;
    transport->advance = 0;
    param_msgs_process(transport->params, 1);
    int sync = (int)param_get_value(transport->params,
                                    transport->val[TRK_PARAM_SYNC], 1);
    const POSITION_SERVER *synced = sync != TRANSPORT_SYNC_OFF ? server : NULL;
    bool music = synced && synced->has_music &&
                 position_music_valid(&synced->music);
    if (synced)
        clock_follow(transport, synced);
    transport->synced = synced != NULL;
    if (music)
        clock_music(transport, &synced->music);
    clock_commands(transport, synced != NULL);
    clock_params(transport, synced != NULL, music);
    clock_derive(transport, nframes);
    clock_display(transport, music);
    param_rt_resend(transport->params);
    atomic_store(&transport->rolling_seen, pos->rolling);
    atomic_store(&transport->music_seen, music);
    if (pos->rolling)
        transport->advance = nframes;
}

bool transport_request_take(TRANSPORT *transport, TRANSPORT_REQ *out) {
    if (!transport || !out)
        return false;
    return ring_buffer_read(transport->reqs, out, sizeof(*out)) == 1;
}

void transport_music_at_rt(TRANSPORT *transport, int64_t frame,
                           POSITION_MUSIC *out) {
    if (!transport || !out)
        return;
    TRANSPORT_POS at = transport->pos;
    int64_t end = at.frame + (at.rolling ? (int64_t)at.nframes : 0);
    // as the next cycle will anchor it
    if (frame < at.frame || frame > end)
        pos_anchor(&at, frame, (double)frame * at.beats_per_frame);
    transport_music(&at, transport_beat_at(&at, frame), out);
}

const TRANSPORT_POS *transport_get_rt(TRANSPORT *transport) {
    if (!transport)
        return NULL;
    return &transport->pos;
}

// Sync and PDC by name, the rest plain numbers. [main-thread]
static unsigned int trk_val_to_string(const void *user_data, int val_id,
                                      PARAM_T value, char *ret_string,
                                      uint32_t string_len) {
    const TRANSPORT *transport = (const TRANSPORT *)user_data;
    if (!transport || !ret_string || string_len == 0)
        return 0;
    static const char *const sync_names[] = {"Off", "Follow", "Lead"};
    static const char *const pdc_names[] = {"Off", "On"};
    int written = 0;
    if (val_id == transport->val[TRK_PARAM_SYNC] && value >= 0 &&
        value <= TRANSPORT_SYNC_LEAD)
        written =
            snprintf(ret_string, string_len, "%s", sync_names[(int)value]);
    else if (val_id == transport->val[TRK_PARAM_PDC] && value >= 0 &&
             value <= 1)
        written = snprintf(ret_string, string_len, "%s", pdc_names[(int)value]);
    else
        written = snprintf(ret_string, string_len, "%g", value);
    return written > 0 ? 1 : 0;
}

TRANSPORT *transport_init(SAMPLE_T rate) {
    if (!(rate > 0))
        return NULL;
    TRANSPORT *transport = (TRANSPORT *)calloc(1, sizeof(TRANSPORT));
    if (!transport)
        return NULL;
    PRM_CONT_USER_DATA params_user_data = {
        .user_data = transport, .val_to_string = trk_val_to_string};
    transport->params = params_init_param_container(&params_user_data);
    transport->cmds = ring_buffer_init(sizeof(TRANSPORT_CMD), TRANSPORT_CMD_QUEUE);
    transport->reqs =
        ring_buffer_init(sizeof(TRANSPORT_REQ), TRANSPORT_REQ_QUEUE);
    if (!transport->params || !transport->cmds || !transport->reqs) {
        transport_clean(transport);
        return NULL;
    }
    // uid 0 - params.c mints one
    PRM_CONTAIN *params = transport->params;
    transport->val[TRK_PARAM_TEMPO] =
        param_add_param(params, "Tempo", "tempo", 100, 10, 500, 1, 0,
                        TRK_PARAM_TEMPO, 0, 0, NULL);
    transport->val[TRK_PARAM_BAR] =
        param_add_param(params, "Bar", "bar", 1, 1, TRANSPORT_BAR_MAX, 1, 0,
                        TRK_PARAM_BAR, 0, 0, NULL);
    transport->val[TRK_PARAM_BEAT] = param_add_param(
        params, "Beat", "beat", 1, 1, 4, 1, 0, TRK_PARAM_BEAT, 0, 0, NULL);
    transport->val[TRK_PARAM_TICK] = param_add_param(
        params, "Tick", "tick", 0, 0, TRANSPORT_TICKS_PER_BEAT - 1, 1, 0,
        TRK_PARAM_TICK, PARAM_FLAG_READONLY, 0, NULL);
    transport->val[TRK_PARAM_BPB] = param_add_param(
        params, "BPB", "bpb", 4, 1, 16, 1, 0, TRK_PARAM_BPB, 0, 0, NULL);
    transport->val[TRK_PARAM_BEAT_TYPE] =
        param_add_param(params, "Beat_Type", "beat_type", 4, 2, 16, 1, 0,
                        TRK_PARAM_BEAT_TYPE, 0, 0, NULL);
    transport->val[TRK_PARAM_SYNC] = param_add_param(
        params, "Sync", "sync", TRANSPORT_SYNC_OFF, TRANSPORT_SYNC_OFF,
        TRANSPORT_SYNC_LEAD, 1, 0, TRK_PARAM_SYNC, PARAM_FLAG_ENUM, 0, NULL);
    transport->val[TRK_PARAM_PDC] =
        param_add_param(params, "PDC", "pdc", 0, 0, 1, 1, 0, TRK_PARAM_PDC,
                        PARAM_FLAG_ENUM, 0, NULL);
    // a failed add is -1, which the rt side would index with
    for (int i = TRK_PARAM_NONE + 1; i < TRK_PARAM_COUNT; i++) {
        if (transport->val[i] < 0) {
            transport_clean(transport);
            return NULL;
        }
    }
    transport->beat_range_bpb = 4;
    atomic_init(&transport->rolling_seen, false);
    atomic_init(&transport->music_seen, false);

    TRANSPORT_POS *pos = &transport->pos;
    pos->music.bpm = 100;
    pos->music.bpb = 4;
    pos->music.beat_type = 4;
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
    if (transport->reqs)
        ring_buffer_clean(transport->reqs);
    free(transport);
}

PRM_CONTAIN *transport_param_container(TRANSPORT *transport) {
    if (!transport)
        return NULL;
    return transport->params;
}

TransportSync transport_sync(TRANSPORT *transport) {
    if (!transport)
        return TRANSPORT_SYNC_OFF;
    return (TransportSync)(int)param_get_value(
        transport->params, transport->val[TRK_PARAM_SYNC], 0);
}

int transport_sync_set(TRANSPORT *transport, TransportSync sync) {
    if (!transport)
        return -1;
    return param_set_value(transport->params, transport->val[TRK_PARAM_SYNC],
                           (PARAM_T)sync, NULL, Operation_SetValue);
}

bool transport_pdc(TRANSPORT *transport) {
    if (!transport)
        return false;
    return param_get_value(transport->params, transport->val[TRK_PARAM_PDC],
                           0) != 0;
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
    // another program's tempo and meter are not ours to edit
    bool music = atomic_load(&transport->music_seen);
    if (music != transport->music_flags) {
        static const int followed[] = {TRK_PARAM_TEMPO, TRK_PARAM_BPB,
                                       TRK_PARAM_BEAT_TYPE};
        for (size_t i = 0; i < sizeof(followed) / sizeof(followed[0]); i++) {
            int val_id = transport->val[followed[i]];
            uint32_t flags = param_get_flags(params, val_id);
            flags = music ? flags | PARAM_FLAG_READONLY
                          : flags & ~(uint32_t)PARAM_FLAG_READONLY;
            param_set_value(params, val_id, (PARAM_T)flags, NULL,
                            Operation_SetFlags);
        }
        transport->music_flags = music;
    }
    PARAM_T bpb = param_get_value(params, transport->val[TRK_PARAM_BPB], 0);
    if (bpb == transport->beat_range_bpb)
        return;
    // from the owner: a clamped Beat is not a locate
    if (param_set_range(params, transport->val[TRK_PARAM_BEAT], 1, bpb,
                        true) == 0)
        transport->beat_range_bpb = bpb;
}
