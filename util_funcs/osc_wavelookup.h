#pragma once
#include "../structs.h"
typedef struct _osc_wavetable_obj OSC_OBJ;

OSC_OBJ* osc_init_osc_wavetable(int table_type, SAMPLE_T esr);
//update the phase of the wavetable of the osc, the phasor has to be stored somewhere else
void osc_updatePhase(OSC_OBJ* osc, SAMPLE_T* phasor, SAMPLE_T freq);
//get a single sample from the wavetable, lineary interpolated
SAMPLE_T osc_getOutput(OSC_OBJ* osc, SAMPLE_T phasor, SAMPLE_T freq, int with_phaseOfs, SAMPLE_T phaseOfs);
//clean the osc struct
void osc_clean_osc_wavetable(OSC_OBJ* osc);
