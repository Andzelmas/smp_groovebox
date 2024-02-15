#pragma once
#include <jack/jack.h>
#include <jack/types.h>
//what kind of samples do we use for audio buffers, we can change float or double here
typedef jack_default_audio_sample_t SAMPLE_T;

//nframes type to use for buffer counts etc.
typedef jack_nframes_t NFRAMES_T;

typedef unsigned char MIDI_DATA_T;
