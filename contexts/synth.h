#pragma once
#include "params.h"
#include "../engine/graph.h"
#include "../types.h"
#include "../structs.h"

typedef struct _synth_voice SYNTH_VOICE;
typedef struct _synth_osc SYNTH_OSC;
typedef struct _synth_port SYNTH_PORT;
typedef struct _synth_data SYNTH_DATA;

//read sys and param messages on [audio-thread] and [main-thread]
int synth_read_ui_to_rt_messages(SYNTH_DATA* synth_data);
int synth_read_rt_to_ui_messages(SYNTH_DATA* synth_data);
//initiate the synth data, one graph node per oscillator, owned by (owner_tag,
//the oscillator's uid). transport is for app_jack_return_transport_rt, until 2T
SYNTH_DATA* synth_init (SAMPLE_T sample_rate, unsigned int with_metronome,
			GRAPH* graph, void* transport, uint64_t owner_tag);
//[main-thread] run at a new sample rate: the synth stops while its tables are
//swapped, playing voices end. 0 on success, -1 (old rate kept) on failure
int synth_sample_rate_set(SYNTH_DATA* synth_data, SAMPLE_T sample_rate);
//process one oscillator (a synth_osc_return handle) on the [audio-thread].
//true if it wrote its outputs this cycle
bool synth_osc_process_rt(void* osc, NFRAMES_T nframes);
//return how many oscillators there are
size_t synth_return_osc_num(SYNTH_DATA* synth_data);
//return the osc_num-th oscillator as an opaque handle (NULL if out of range).
//borrowed - do not free. Used as the DataObject user_data for one oscillator
//and by synth_osc_process_rt. [audio-thread] safe.
void* synth_osc_return(SYNTH_DATA* synth_data, unsigned int osc_num);
//return the display name for a handle from synth_osc_return. Owned by the synth,
//valid while the synth exists. NULL on error.
const char* synth_osc_name(void* osc);
//return the oscillator's identity uid (0 on error). Oscillators are fixed, so
//this is just the (stable) slot number.
uint32_t synth_osc_uid(void* osc);

// return this oscillator's own param container, NULL on error/none yet
PRM_CONTAIN *synth_osc_param_container(void *osc);
//clean the synth data
int synth_clean_memory(SYNTH_DATA* synth_data);
