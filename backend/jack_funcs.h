#pragma once
#include "audio_backend.h"

// What is left of JACK outside audio_backend.h: its sys message reads, until
// T3 of 2T. The handle is the AUDIO_BACKEND.

//read messages from ui on [audio-thread]
int app_jack_read_ui_to_rt_messages(AUDIO_BACKEND* jack_data);
//read messages from rt on [main-thread], log messages from rt thread
int app_jack_read_rt_to_ui_messages(AUDIO_BACKEND* jack_data);
