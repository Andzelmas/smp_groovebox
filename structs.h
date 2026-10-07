#pragma once
#include <stdint.h>

#if SAMPLE_T_AS_DOUBLE == 1
typedef double SAMPLE_T;
#else
//what kind of samples do we use for audio buffers, we can change float or double here
typedef float SAMPLE_T;
#endif

//precision of the parameter values
typedef double PARAM_T;

//nframes type to use for buffer counts etc.
typedef uint32_t NFRAMES_T;

typedef unsigned char MIDI_DATA_T;
