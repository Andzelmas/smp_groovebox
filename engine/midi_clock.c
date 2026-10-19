#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
// my libraries
#include "midi_clock.h"
#include "../types.h"
#include "../util_funcs/midi_buf.h"

// MIDI 1.0 system messages
enum {
    MIDI_CLOCK_MSG_SPP = 0xF2,
    MIDI_CLOCK_MSG_TICK = 0xF8,
    MIDI_CLOCK_MSG_START = 0xFA,
    MIDI_CLOCK_MSG_CONTINUE = 0xFB,
    MIDI_CLOCK_MSG_STOP = 0xFC,
};
#define MIDI_CLOCK_PER_QUARTER 24
// clocks per Song Position Pointer step, a 16th
#define MIDI_CLOCK_PER_SPP 6
// the SPP's 14 bits
#define MIDI_CLOCK_SPP_WRAP 16384
// how long midi_clock_halt waits for the process
#define MIDI_CLOCK_HALT_WAIT_MS 1000

struct _midi_clock {
    TRANSPORT *transport;
    GRAPH_PORT *port;
    // [audio-thread]'s once a plan runs the node: whether the last cycle
    // rolled and its grid_to; whether the slave was told to run; a run waiting
    // for its 16th - its SPP (-1 = Start) and beat; its edge's delay and, from
    // the cycle's start, the frames until what it pushed has left that
    bool rolling;
    double grid_to;
    bool running;
    bool pending;
    int64_t pending_spp;
    double pending_beat;
    uint32_t delay;
    uint32_t held;
    // [main-thread] -> [audio-thread]: send nothing more; [audio-thread] ->
    // [main-thread]: done, a slave that may run got its Stop
    atomic_bool halt;
    atomic_bool halted;
};

static void clock_push_rt(MIDI_CLOCK *clock, MIDI_BUF *out, NFRAMES_T offset,
                          const uint8_t *msg, uint32_t size) {
    midi_buf_push(out, offset, msg, size);
    uint32_t left = offset + clock->delay + 1;
    if (left > clock->held)
        clock->held = left;
}

static void msg_push_rt(MIDI_CLOCK *clock, MIDI_BUF *out, NFRAMES_T offset,
                        uint8_t status) {
    clock_push_rt(clock, out, offset, &status, 1);
}

// the edge's delay drops what it holds, a slave that may run hears Stop at
// once. Nothing else this cycle - it would skip the delay too
static void clock_flush_rt(MIDI_CLOCK *clock, MIDI_BUF *out) {
    graph_port_midi_flush_rt(clock->port);
    // a Stop still held may be dropped
    if (clock->running || clock->held > 0) {
        uint8_t stop = MIDI_CLOCK_MSG_STOP;
        midi_buf_push(out, 0, &stop, 1);
    }
    clock->running = false;
    clock->pending = false;
    clock->held = 0;
}

// a run from grid index first goes on at the next 16th: Start at the song
// start, else that 16th's SPP and Continue
static void clock_restart_rt(MIDI_CLOCK *clock, int64_t first, double step) {
    clock->pending = true;
    if (first <= 0) {
        clock->pending_spp = -1;
        clock->pending_beat = 0;
        return;
    }
    int64_t spp = (first + MIDI_CLOCK_PER_SPP - 1) / MIDI_CLOCK_PER_SPP;
    clock->pending_spp = spp;
    // a beat, not an index: a Beat_Type change while waiting changes the step
    clock->pending_beat = (double)(spp * MIDI_CLOCK_PER_SPP) * step;
}

// the pending run, right before its first clock - a slave may drop a Continue
// not followed by a clock soon
static void clock_run_rt(MIDI_CLOCK *clock, MIDI_BUF *out, NFRAMES_T offset) {
    clock->pending = false;
    clock->running = true;
    if (clock->pending_spp < 0) {
        msg_push_rt(clock, out, offset, MIDI_CLOCK_MSG_START);
        return;
    }
    uint32_t wrapped = (uint32_t)(clock->pending_spp % MIDI_CLOCK_SPP_WRAP);
    uint8_t msg[3] = {MIDI_CLOCK_MSG_SPP, wrapped & 0x7F, wrapped >> 7};
    clock_push_rt(clock, out, offset, msg, 3);
    msg_push_rt(clock, out, offset, MIDI_CLOCK_MSG_CONTINUE);
}

static void clock_cycle_rt(MIDI_CLOCK *clock) {
    MIDI_BUF *out = graph_port_midi_rt(clock->port);
    uint32_t delay = graph_port_delay_rt(clock->port);
    bool delay_changed = delay != clock->delay;
    clock->delay = delay;
    if (atomic_load(&clock->halt)) {
        clock_flush_rt(clock, out);
        atomic_store(&clock->halted, true);
        return;
    }
    const TRANSPORT_POS *pos = transport_get_rt(clock->transport);
    // a new delay re-syncs a rolling slave as a jump: stopped now, the run
    // goes on at a 16th from the next cycle
    if (delay_changed && pos->rolling && clock->rolling) {
        clock_flush_rt(clock, out);
        clock->rolling = false;
        return;
    }
    // the grid ranges of a run join, so a gap is a locate
    bool jumped =
        pos->rolling && clock->rolling && pos->grid_from != clock->grid_to;
    if ((jumped || !pos->rolling) && clock->running) {
        msg_push_rt(clock, out, 0, MIDI_CLOCK_MSG_STOP);
        clock->running = false;
    }
    if (!pos->rolling) {
        clock->rolling = false;
        clock->pending = false;
        return;
    }
    // beats are beat_type notes
    double step = pos->music.beat_type / (4.0 * MIDI_CLOCK_PER_QUARTER);
    TRANSPORT_GRID grid = transport_grid(pos, step);
    if (jumped || !clock->rolling)
        clock_restart_rt(clock, grid.idx, step);
    NFRAMES_T offset = 0;
    int64_t idx = 0;
    while (transport_grid_next(&grid, &offset, &idx)) {
        if (clock->pending) {
            if ((double)idx * step < clock->pending_beat)
                continue;
            clock_run_rt(clock, out, offset);
        }
        msg_push_rt(clock, out, offset, MIDI_CLOCK_MSG_TICK);
    }
    clock->rolling = true;
    clock->grid_to = pos->grid_to;
}

// [audio-thread] GRAPH_PROCESS_FN
static bool midi_clock_process_rt(void *arg, NFRAMES_T nframes) {
    MIDI_CLOCK *clock = (MIDI_CLOCK *)arg;
    clock_cycle_rt(clock);
    clock->held = clock->held > nframes ? clock->held - nframes : 0;
    return true;
}

MIDI_CLOCK *midi_clock_init(GRAPH *graph, TRANSPORT *transport,
                            uint64_t owner_tag, uint64_t owner_uid) {
    if (!graph || !transport)
        return NULL;
    MIDI_CLOCK *clock = (MIDI_CLOCK *)calloc(1, sizeof(MIDI_CLOCK));
    if (!clock)
        return NULL;
    clock->transport = transport;
    atomic_init(&clock->halt, false);
    atomic_init(&clock->halted, false);
    GRAPH_NODE *node = graph_node_add(graph, owner_tag, owner_uid,
                                      GRAPH_NODE_PLAIN, midi_clock_process_rt,
                                      clock);
    if (node)
        clock->port = graph_port_create(graph, node, PORT_TYPE_MIDI,
                                        PORT_FLOW_OUTPUT, MIDI_CLOCK_PORT_NAME);
    if (!clock->port) {
        // no plan can have the node yet, so it goes at once
        graph_node_remove(graph, node);
        free(clock);
        return NULL;
    }
    return clock;
}

void midi_clock_halt(MIDI_CLOCK *clock) {
    if (!clock)
        return;
    atomic_store(&clock->halt, true);
    // the graph may not run any more - never wait for good
    const struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};
    for (int i = 0; i < MIDI_CLOCK_HALT_WAIT_MS && !atomic_load(&clock->halted);
         i++)
        nanosleep(&ms, NULL);
}

void midi_clock_clean(MIDI_CLOCK *clock) {
    free(clock);
}
