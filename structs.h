#pragma once
#include <jack/jack.h>
#include <jack/types.h>

#if SAMPLE_T_AS_DOUBLE == 1
typedef double SAMPLE_T;
#else
//what kind of samples do we use for audio buffers, we can change float or double here
typedef jack_default_audio_sample_t SAMPLE_T;
#endif

//nframes type to use for buffer counts etc.
typedef jack_nframes_t NFRAMES_T;

typedef unsigned char MIDI_DATA_T;
