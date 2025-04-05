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
//set value is a separate function, since it needs to send a message to the param_ui_to_rt ring_buffer
int synth_param_set_value(SYNTH_DATA* synth_data, int osc_id, int param_id, float param_val, unsigned char param_op);

//activate the audio ports
int synth_activate_backend_ports(SYNTH_DATA* synth_data, SYNTH_OSC* osc);
//return the parameter container for the osc_num oscillator
PRM_CONTAIN* synth_return_param_container(SYNTH_DATA* synth_data, unsigned int osc_num);
//return the name of the osc_num oscillators
const char* synth_return_osc_name(SYNTH_DATA* synth_data, unsigned int osc_num);
//return how many oscillators there are
int synth_return_osc_num(SYNTH_DATA* synth_data);
//clean the ports
static int synth_clean_ports(SYNTH_DATA* synth_data, SYNTH_PORT** osc_ports, unsigned int num_ports);
//clean one oscillator
static int synth_clean_osc(SYNTH_DATA* synth_data, SYNTH_OSC* synth_osc);
//clean the synth data
int synth_clean_memory(SYNTH_DATA* synth_data);
