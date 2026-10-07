#pragma once
#include <stdint.h>
#include "audio_backend.h"
#include "../contexts/params.h"
#include "../structs.h"

// What is left of JACK outside audio_backend.h: the transport and its Trk
// params, until 2T (internal transport). The handle is the AUDIO_BACKEND.

//return the transport param container (the Trk params), NULL on error
PRM_CONTAIN* app_jack_trk_param_container(void* jack_data_handle);

//read messages from ui on [audio-thread]
//update parameters, pause jack process
int app_jack_read_ui_to_rt_messages(AUDIO_BACKEND* jack_data);
//read messages from rt on [main-thread]
//update parameters, log messages from rt thread
int app_jack_read_rt_to_ui_messages(AUDIO_BACKEND* jack_data);

//return the jack transport position info to the various variables
int app_jack_return_transport_rt(void* audio_client, int32_t* cur_bar, int32_t* cur_beat,
				 int32_t* cur_tick, SAMPLE_T* ticks_per_beat, NFRAMES_T* total_frames,
				 float* bmp, float* beat_type, float* beats_per_bar);
