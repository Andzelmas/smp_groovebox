#pragma once
#include <stdint.h>
#include "graph.h"
#include "transport.h"

// MIDI clock out of the transport: 24 clocks per quarter note while rolling,
// Start / Continue / Stop and Song Position Pointer, on one graph node's MIDI
// output. A change of its edge's compensation delay while rolling re-syncs the
// slave as a locate. Knows no backend. Every function here is [main-thread]

typedef struct _midi_clock MIDI_CLOCK;

// the node's MIDI output
#define MIDI_CLOCK_PORT_NAME "clock_out"

// adds a node of graph owned by (owner_tag, owner_uid), reading transport.
// NULL on failure
MIDI_CLOCK *midi_clock_init(GRAPH *graph, TRANSPORT *transport,
                            uint64_t owner_tag, uint64_t owner_uid);
// before the process halts (quit): a slave that may run gets Stop at once,
// what the edge's delay holds is dropped; then nothing more is sent. Waits
// until a cycle pushed it, at most a second
void midi_clock_halt(MIDI_CLOCK *clock);
// graph_process_rt must not run any more. The graph frees the node
void midi_clock_clean(MIDI_CLOCK *clock);
