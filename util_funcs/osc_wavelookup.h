#pragma once
#include "../structs.h"
typedef struct _osc_wavetable_obj OSC_OBJ;

OSC_OBJ* osc_init_osc_wavetable(int table_type, PARAM_T esr);
//update the phase of the wavetable of the osc, the phasor has to be stored somewhere else
void osc_updatePhase(OSC_OBJ* osc, PARAM_T* phasor, PARAM_T freq);
//get a single sample from the wavetable, lineary interpolated
PARAM_T osc_getOutput(OSC_OBJ* osc, PARAM_T phasor, PARAM_T freq, int with_phaseOfs, PARAM_T phaseOfs);
//clean the osc struct
void osc_clean_osc_wavetable(OSC_OBJ* osc);
