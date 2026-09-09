#pragma once
#include "params.h"
#include "../types.h"
#include "../structs.h"

typedef struct _synth_voice SYNTH_VOICE;
typedef struct _synth_osc SYNTH_OSC;
typedef struct _synth_port SYNTH_PORT;
typedef struct _synth_data SYNTH_DATA;

//read sys and param messages on [audio-thread] and [main-thread]
int synth_read_ui_to_rt_messages(SYNTH_DATA* synth_data);
int synth_read_rt_to_ui_messages(SYNTH_DATA* synth_data);
//initiate the synth data
SYNTH_DATA* synth_init (unsigned int buffer_size, SAMPLE_T sample_rate, const char* cx_name, unsigned int with_metronome,
			void* audio_backend);
//process the synth_data oscillators
int synth_process_rt(SYNTH_DATA* synth_data, NFRAMES_T nframes);
//functions for param manipulation, should be called only on [main-thread]
PRM_CONTAIN* synth_param_return_param_container(SYNTH_DATA* synth_data, int osc_id);

//activate the audio ports
int synth_activate_backend_ports(SYNTH_DATA* synth_data, SYNTH_OSC* osc);
//return how many oscillators there are
size_t synth_return_osc_num(SYNTH_DATA* synth_data);
//return the osc_num-th oscillator as an opaque handle (NULL if out of range).
//borrowed - do not free. Used as the DataObject user_data for one oscillator.
void* synth_osc_return(SYNTH_DATA* synth_data, unsigned int osc_num);
//return the display name for a handle from synth_osc_return. Owned by the synth,
//valid while the synth exists. NULL on error.
const char* synth_osc_name(void* osc);
//return the oscillator's identity uid (0 on error). Oscillators are fixed, so
//this is just the (stable) slot number.
uint32_t synth_osc_uid(void* osc);
//clean the ports
static int synth_clean_ports(SYNTH_DATA* synth_data, SYNTH_PORT** osc_ports, unsigned int num_ports);
//clean one oscillator
static int synth_clean_osc(SYNTH_DATA* synth_data, SYNTH_OSC* synth_osc);
//clean the synth data
int synth_clean_memory(SYNTH_DATA* synth_data);
