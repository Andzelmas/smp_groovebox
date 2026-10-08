#include "synth.h"
#include "../backend/jack_funcs.h"
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/math_funcs.h"
#include "../util_funcs/midi_buf.h"
#include "../util_funcs/osc_wavelookup.h"
#include "context_control.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

// max voices that can play simultaniously
#define MAX_SYNTH_VOICES 8
// how many oscillators there should be
#define MAX_OSCS 3
// number of output Audio ports for the whole synth
#define SYNTH_OUTS 2
// number of midi in ports for the synth
#define SYNTH_IN_MIDI 1
// Samples for wavetables
#define OSC_TABLE_SAMPLES 2048
// Maximum semitones, also lower than 0, for semitone to frequency conversion
// using this number a table is built for converting semitones to frequency,
// given the starting frequency
#define MAX_SEMITONES 36
// range of the A/D/R params - shared by the init call and
// synth_osc_build_value's curve mapping so they can't drift out of sync
#define SYNTH_ADSR_TIME_MIN 0.0
#define SYNTH_ADSR_TIME_MAX 5.0
// how many samples math_ramp_val_get_value smooths the Amp param over,
#define SYNTH_AMP_INTERP_SAMPLES 400
// how long a voice's amp ramps take, in seconds
#define SYNTH_VOICE_AMP_RAMP_SECS 0.002
// the increments that the semitones will be incremented or decreased by the
// user
#define SEMITONES_INC 0.1
// the longest that the a, d or r in ADSR can be in seconds
#define ADSR_MAX_TIME 5

// this module's own id space for an oscillator's params, passed to
// param_add_param as owner_id. Source-defined, so it is stable across runs
// and is what a saved value gets matched back by - never a positional index.
// 0 stays reserved for "no owner id".
enum synthOscParamId {
    SYNTH_PARAM_NONE = 0,
    SYNTH_PARAM_AMP,
    SYNTH_PARAM_FREQ,
    SYNTH_PARAM_SPREAD,
    SYNTH_PARAM_WOBBLE,
    SYNTH_PARAM_OCTAVE,
    SYNTH_PARAM_TABLE,
    SYNTH_PARAM_ATTACK,
    SYNTH_PARAM_DECAY,
    SYNTH_PARAM_SUSTAIN,
    SYNTH_PARAM_RELEASE,
    SYNTH_PARAM_COUNT
};

typedef struct _synth_adsr {
    PARAM_T amp;   // the current calculated amp from the adsr
    PARAM_T r_amp; // release amp, that holds the amp that was at the release
                   // phase initiation
    unsigned int phase; // current phase of the ADSR (1 - A, 2 - D etc.)
    int time_frames;    // how long the adsr is progressing
    int r_frames;       // release frames to calc when to end the release phase
    PARAM_T a;
    PARAM_T d;
    PARAM_T s;
    PARAM_T r;
    SAMPLE_T samplerate;
} SYNTH_ADSR;

typedef struct _synth_voice {
    int id;
    // what midi note initialized this voice
    unsigned char midi_note;
    // midi vel that triggered this voice
    SAMPLE_T midi_vel;
    // should this voice be playing
    unsigned int playing;
    // this indicates that the voice is fully stopped and its amplitude is 0
    unsigned int stopped;
    // what the user wishes the vco level to be, its 0 when playing is 0
    // this is to interpolate so the amp does not go to big values too fast
    MATH_RAMP_VAL *vco_amp_L;
    MATH_RAMP_VAL *vco_amp_R;
    // the voice amp adsr
    SYNTH_ADSR *vco_adsr;
    // the current phase of the vco
    PARAM_T vco_ph;
    // the current phase of the detune wobble lfo
    PARAM_T wobble_ph;
    // random seed for random stuff in the voice, this should
    // be updated when the voices starts playing
    unsigned int rand_seed;
    // address of the table to use when playing
    OSC_OBJ *osc_table;
} SYNTH_VOICE;

// an oscillator's param values for this cycle
typedef struct _synth_osc_vals {
    PARAM_T amp;
    PARAM_T freq;
    PARAM_T octave;
    PARAM_T wobble;
    PARAM_T spread;
    PARAM_T a;
    PARAM_T d;
    PARAM_T s;
    PARAM_T r;
} SYNTH_OSC_VALS;

typedef struct _synth_osc {
    int id;
    // back pointer to the owning synth, so an oscillator handle carries its
    // context (used as the DataObject user_data for a single oscillator)
    SYNTH_DATA *synth_data;
    // trigger, that can be used to track various oscillator behaviour
    // for example for metronome osc this is used to test if the oscillator
    // needs to be played starts at -1, when the osc is initialized for the
    // first time
    int trig;
    // name of the osc that will be returned to the ui
    char *name;
    // the voice array for the oscillator
    // the voice gets its parameters from this struct
    SYNTH_VOICE *osc_voices;
    // the number of voices for the oscillator, usually its the same amount for
    // all oscillators but for example the metronome only has 2
    unsigned int num_voices;
    // parameter container for the oscillator
    PRM_CONTAIN *params;
    // val_ids from param_add_param, indexed by synthOscParamId so
    // SYNTH_PARAM_NONE's slot is unused. The [audio-thread] indexes params by
    // these - never by a literal position, and never via a lookup, which
    // would scan.
    int val_id[SYNTH_PARAM_COUNT];
    // smooths the raw Amp param on [audio-thread] reads
    MATH_RAMP_VAL *amp_smooth;
    // [audio-thread] read once per cycle by synth_osc_params_read_rt
    SYNTH_OSC_VALS vals;
    // which voice played last
    int last_voice;
    // [audio-thread] this cycle's graph outputs, the voices sum into them
    SAMPLE_T *out_L;
    SAMPLE_T *out_R;
    // for convenience osc tables on the osc struct
    OSC_OBJ *triang_osc;
    OSC_OBJ *sqr_osc;
    OSC_OBJ *saw_osc;
    OSC_OBJ *sin_osc;
    // the oscillator's node and its ports
    GRAPH_NODE *node;
    SYNTH_PORT *ports;
    // how many ports are there
    unsigned int num_ports;
} SYNTH_OSC;

typedef struct _synth_port {
    unsigned int id;
    // midi or audio
    unsigned int port_type;
    // in or output
    unsigned int port_flow;
    const char *port_name;
    GRAPH_PORT *graph_port;
} SYNTH_PORT;

typedef struct _synth_data {
    // 0 while [main-thread] swaps what the oscillators read (a rate change),
    // touch only on [audio-thread]
    unsigned int processing;
    // oscillator object with the triangle table
    OSC_OBJ *triang_osc;
    // oscillator object with the square table
    OSC_OBJ *sqr_osc;
    // oscillator object with the saw table
    OSC_OBJ *saw_osc;
    // oscillator object with the sin table
    OSC_OBJ *sin_osc;
    // table that has frequency multipliers for semitones from -MAX_SEMITONES to
    // MAX_SAMITONES in SEMITONES_INC increments
    MATH_RANGE_TABLE *semi_to_freq_table;
    // table that has a curve 0..1 in a logarithmic fashion, basically inverse
    // amp_to_exp curve but with a gentler slope (1/4 instead of 1/10 squish)
    // good curve for midi vel curves
    MATH_RANGE_TABLE *log_curve;
    // table that has the exponential conversion for amplitude (so 0.5 amp is
    // half the percieved loudness or 0.0313 or so)
    MATH_RANGE_TABLE *amp_to_exp;
    // sample rate for the current audio system (48000, 44100 etc.)
    SAMPLE_T samplerate;
    // should the metronome be initialized and processed
    unsigned int with_metronome;
    // the synth oscillators
    // Synth_Osc 0 is reserved for the metronome
    SYNTH_OSC *osc_array;
    // how many oscilators we have
    size_t num_osc;
    GRAPH *graph;
    // for app_jack_return_transport_rt, until 2T
    void *transport;
    // paired with an oscillator's uid as its node's owner
    uint64_t owner_tag;
    // this is control for [audio-thread] and [main-thread] sys communication:
    // thread safe messages, and the whole synth's stop/start
    CXCONTROL *control_data;
} SYNTH_DATA;

static int synth_sys_msg(void *user_data, const char *msg) {
    (void)user_data;
    log_append_logfile("%s", msg);
    return 0;
}

// user_data is the SYNTH_DATA
static int synth_start_process(void *user_data) {
    ((SYNTH_DATA *)user_data)->processing = 1;
    return 0;
}
static int synth_stop_process(void *user_data) {
    ((SYNTH_DATA *)user_data)->processing = 0;
    return 0;
}

int synth_read_ui_to_rt_messages(SYNTH_DATA *synth_data) {
    if (!synth_data)
        return -1;
    // process the sys messages, right now only need for send messages, so on
    // [audio-thread] does not do anything
    context_sub_process_rt(synth_data->control_data);

    for (unsigned int i = 0; i < MAX_OSCS; i++) {
        SYNTH_OSC *osc = &(synth_data->osc_array[i]);
        if (!osc->params)
            continue;
        param_msgs_process(osc->params, 1);
    }
    return 0;
}
int synth_read_rt_to_ui_messages(SYNTH_DATA *synth_data) {
    if (!synth_data)
        return -1;
    // process the sys messages on [main-thread] - right now only logs messages
    // from [audio-thread]
    context_sub_process_ui(synth_data->control_data);

    // read the param rt_to_ui messages and set the parameter values
    for (unsigned int i = 0; i < MAX_OSCS; i++) {
        SYNTH_OSC *osc = &(synth_data->osc_array[i]);
        if (!osc->params)
            continue;
        param_msgs_process(osc->params, 0);
    }
    return 0;
}

// init the adsr
static SYNTH_ADSR *synth_init_adsr(SAMPLE_T samplerate) {
    SYNTH_ADSR *adsr = malloc(sizeof(SYNTH_ADSR));
    adsr->a = 0.0;
    adsr->d = 0.0;
    adsr->s = 0.0;
    adsr->r = 0.0;
    adsr->amp = 0.0;
    adsr->r_amp = 0.0;
    adsr->phase = 0;
    adsr->samplerate = samplerate;
    adsr->time_frames = 0;
    adsr->r_frames = 0;

    return adsr;
}

// builds the value param_get_value returns for one of this oscillator's own
// params
static PARAM_T synth_osc_build_value(const void *user_data, int val_id,
                                     PARAM_T raw_val, unsigned int rt_params) {
    SYNTH_OSC *osc = (SYNTH_OSC *)user_data;
    if (!osc)
        return raw_val;
    if (!osc->synth_data)
        return raw_val;

    // A / D / R (params 6, 7, 9): exponential time mapping, same curve table
    // and same fit_range round-trip param_get_value used to do internally
    if (val_id == 6 || val_id == 7 || val_id == 9) {
        if (!osc->synth_data->amp_to_exp)
            return raw_val;
        PARAM_T val_norm = fit_range(SYNTH_ADSR_TIME_MAX, SYNTH_ADSR_TIME_MIN,
                                     1.0, 0.0, raw_val);
        PARAM_T val_curve = math_range_table_convert_value(
            osc->synth_data->amp_to_exp, val_norm);
        return fit_range(1.0, 0.0, SYNTH_ADSR_TIME_MAX, SYNTH_ADSR_TIME_MIN,
                         val_curve);
    }

    // Amp: smoothed so a fast knob turn doesn't click - only ever
    // read this way on [audio-thread] (rt_params == 1), matching every
    // existing param_get_value call site for this param
    if (val_id == 0 && rt_params == 1) {
        if (!osc->amp_smooth)
            return raw_val;
        return math_ramp_val_get_value(osc->amp_smooth, raw_val);
    }

    return raw_val;
}

// the oscillator's node and its ports
static int synth_osc_ports_create(SYNTH_DATA *synth_data, SYNTH_OSC *osc);

SYNTH_DATA *synth_init(SAMPLE_T sample_rate, unsigned int with_metronome,
                       GRAPH *graph, void *transport, uint64_t owner_tag) {

    SYNTH_DATA *synth_data = (SYNTH_DATA *)malloc(sizeof(SYNTH_DATA));
    if (!synth_data)
        return NULL;
    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = synth_start_process;
    rt_funcs_struct.subcx_stop_process = synth_stop_process;
    ui_funcs_struct.send_msg = synth_sys_msg;
    synth_data->control_data =
        context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if (!synth_data->control_data) {
        free(synth_data);
        return NULL;
    }
    // [audio-thread] sees it only through a plan with the nodes, published
    // after this
    synth_data->processing = 1;
    synth_data->samplerate = sample_rate;
    synth_data->with_metronome = with_metronome;
    synth_data->num_osc = MAX_OSCS;
    synth_data->osc_array = NULL;
    synth_data->saw_osc = NULL;
    synth_data->sqr_osc = NULL;
    synth_data->triang_osc = NULL;
    synth_data->sin_osc = NULL;
    synth_data->graph = graph;
    synth_data->transport = transport;
    synth_data->owner_tag = owner_tag;
    synth_data->semi_to_freq_table = NULL;
    synth_data->log_curve = NULL;
    synth_data->amp_to_exp = NULL;

    synth_data->semi_to_freq_table =
        math_init_range_table(MAX_SEMITONES * -1, MAX_SEMITONES, SEMITONES_INC);

    if (!synth_data->semi_to_freq_table) {
        synth_clean_memory(synth_data);
        return NULL;
    }
    // fill the semitone to frequency multiplier convert table with values
    for (unsigned int i = 0;
         i < math_range_table_get_len(synth_data->semi_to_freq_table); i++) {
        PARAM_T cur_semitones =
            math_range_table_get_value(synth_data->semi_to_freq_table, i);
        PARAM_T cur_freq_ratio = exp_range_ratio(12.0, cur_semitones);
        math_range_table_enter_value(synth_data->semi_to_freq_table, i,
                                     cur_freq_ratio);
    }

    synth_data->log_curve = math_init_range_table(0.0, 1.0, 0.005);
    if (!synth_data->log_curve) {
        synth_clean_memory(synth_data);
        return NULL;
    }
    // fill the midivel to amp table with values
    for (unsigned int i = 0;
         i < math_range_table_get_len(synth_data->log_curve); i++) {
        PARAM_T cur_val = math_range_table_get_value(synth_data->log_curve, i);
        PARAM_T cur_log = 0.0;
        if (cur_val > 0.0) {
            cur_log = 0.25 * (log10((double)cur_val) / log10(2.0)) + 1;
        }
        if (cur_val == 1.0)
            cur_log = 1.0;
        if (cur_log < 0.0)
            cur_log = 0.0;
        math_range_table_enter_value(synth_data->log_curve, i, cur_log);
    }

    synth_data->amp_to_exp = math_init_range_table(0.0, 1.0, 0.001);
    if (!synth_data->amp_to_exp) {
        synth_clean_memory(synth_data);
        return NULL;
    }
    // fill the amp exponential table
    for (unsigned int i = 0;
         i < math_range_table_get_len(synth_data->amp_to_exp); i++) {
        PARAM_T cur_amp = math_range_table_get_value(synth_data->amp_to_exp, i);
        PARAM_T cur_exp = pow(2.0, cur_amp * (10.0) - 10.0);
        if (cur_amp == 0)
            cur_exp = 0.0;
        math_range_table_enter_value(synth_data->amp_to_exp, i, cur_exp);
    }

    // init the wavetable objects
    synth_data->sin_osc =
        osc_init_osc_wavetable(SIN_WAVETABLE, synth_data->samplerate);
    if (!synth_data->sin_osc) {
        synth_clean_memory(synth_data);
        return NULL;
    }
    synth_data->triang_osc =
        osc_init_osc_wavetable(TRIANGLE_WAVETABLE, synth_data->samplerate);
    if (!synth_data->triang_osc) {
        synth_clean_memory(synth_data);
        return NULL;
    }
    synth_data->saw_osc =
        osc_init_osc_wavetable(SAW_WAVETABLE, synth_data->samplerate);
    if (!synth_data->saw_osc) {
        synth_clean_memory(synth_data);
        return NULL;
    }
    synth_data->sqr_osc =
        osc_init_osc_wavetable(SQUARE_WAVETABLE, synth_data->samplerate);
    if (!synth_data->sqr_osc) {
        synth_clean_memory(synth_data);
        return NULL;
    }

    synth_data->osc_array =
        (SYNTH_OSC *)calloc(synth_data->num_osc, sizeof(SYNTH_OSC));
    if (!synth_data->osc_array) {
        synth_clean_memory(synth_data);
        return NULL;
    }
    for (unsigned int i = 0; i < synth_data->num_osc; i++) {
        SYNTH_OSC *cur_osc = &(synth_data->osc_array[i]);
        cur_osc->synth_data = synth_data;
        cur_osc->name = NULL;
        cur_osc->triang_osc = synth_data->triang_osc;
        cur_osc->sqr_osc = synth_data->sqr_osc;
        cur_osc->saw_osc = synth_data->saw_osc;
        cur_osc->sin_osc = synth_data->sin_osc;
        cur_osc->id = i;
        cur_osc->trig = -1;
        cur_osc->last_voice = 0;
        cur_osc->num_ports = 0;
        cur_osc->params = NULL;
        cur_osc->osc_voices = NULL;
        cur_osc->ports = NULL;
        cur_osc->out_L = NULL;
        cur_osc->out_R = NULL;
        cur_osc->name = NULL;
        cur_osc->num_ports = 0;
        cur_osc->ports = NULL;
        cur_osc->num_voices = MAX_SYNTH_VOICES;
        cur_osc->name = malloc(sizeof(char) * 6);
        if (!cur_osc->name) {
            synth_clean_memory(synth_data);
            return NULL;
        }
        snprintf(cur_osc->name, 6, "Osc_%u", i);

        // create the oscillator ports
        cur_osc->num_ports = SYNTH_OUTS + SYNTH_IN_MIDI;
        cur_osc->ports =
            (SYNTH_PORT *)calloc(cur_osc->num_ports, sizeof(SYNTH_PORT));
        if (!cur_osc->ports) {
            synth_clean_memory(synth_data);
            return NULL;
        }

        for (unsigned int j = 0; j < cur_osc->num_ports; j++) {
            SYNTH_PORT *cur_port = &(cur_osc->ports[j]);
            cur_port->id = j;
            if (j == 0) {
                cur_port->port_flow = PORT_FLOW_INPUT;
                cur_port->port_type = PORT_TYPE_MIDI;
                cur_port->port_name = "midi_in";
            }
            if (j == 1) {
                cur_port->port_flow = PORT_FLOW_OUTPUT;
                cur_port->port_type = PORT_TYPE_AUDIO;
                cur_port->port_name = "out_L";
            }
            if (j == 2) {
                cur_port->port_flow = PORT_FLOW_OUTPUT;
                cur_port->port_type = PORT_TYPE_AUDIO;
                cur_port->port_name = "out_R";
            }
        }

        // if this is the 0 oscillator and the metronome should be initialized
        if (i == 0 && synth_data->with_metronome == 1) {
            cur_osc->num_voices = 2;
            if (cur_osc->name)
                free(cur_osc->name);
            cur_osc->name = malloc(sizeof(char) * 4);
            if (!cur_osc->name) {
                synth_clean_memory(synth_data);
                return NULL;
            }
            snprintf(cur_osc->name, 4, "Mtr");
            // change the ports
            free(cur_osc->ports);
            cur_osc->num_ports = SYNTH_OUTS;
            cur_osc->ports =
                (SYNTH_PORT *)calloc(cur_osc->num_ports, sizeof(SYNTH_PORT));
            if (!cur_osc->ports) {
                synth_clean_memory(synth_data);
                return NULL;
            }

            for (unsigned int j = 0; j < cur_osc->num_ports; j++) {
                SYNTH_PORT *cur_port = &(cur_osc->ports[j]);
                cur_port->id = j;
                cur_port->port_flow = PORT_FLOW_OUTPUT;
                cur_port->port_type = PORT_TYPE_AUDIO;
                cur_port->port_name = j == 0 ? "out_L" : "out_R";
            }
        }

        cur_osc->osc_voices =
            (SYNTH_VOICE *)calloc(cur_osc->num_voices, sizeof(SYNTH_VOICE));
        if (!cur_osc->osc_voices) {
            synth_clean_memory(synth_data);
            return NULL;
        }
        for (unsigned int j = 0; j < cur_osc->num_voices; j++) {
            SYNTH_VOICE *cur_voice = &(cur_osc->osc_voices[j]);
            cur_voice->vco_amp_L = math_ramp_val_init(
                1.0, (unsigned int)(SYNTH_VOICE_AMP_RAMP_SECS *
                                    synth_data->samplerate));
            cur_voice->vco_amp_R = math_ramp_val_init(
                1.0, (unsigned int)(SYNTH_VOICE_AMP_RAMP_SECS *
                                    synth_data->samplerate));
            cur_voice->vco_adsr = synth_init_adsr(synth_data->samplerate);
            cur_voice->vco_ph = 0;
            cur_voice->wobble_ph = 0;
            cur_voice->id = j;
            cur_voice->midi_note = 0;
            cur_voice->midi_vel = 0;
            cur_voice->playing = 0;
            cur_voice->stopped = 1;
            cur_voice->rand_seed = 0;
            cur_voice->osc_table = NULL;
        }

        // Amp is smoothed on [audio-thread] reads by
        // synth_osc_build_value
        cur_osc->amp_smooth =
            math_ramp_val_init(fabs(1.0 - 0.00001), SYNTH_AMP_INTERP_SAMPLES);
        PRM_CONT_USER_DATA osc_params_user_data = {.user_data = (void *)cur_osc,
                                                   .build_value =
                                                       synth_osc_build_value,
                                                   .val_to_string = NULL};
        cur_osc->params = params_init_param_container(&osc_params_user_data);
        // uid 0 - params.c mints; owner_id is this module's own enum
        cur_osc->val_id[SYNTH_PARAM_AMP] =
            param_add_param(cur_osc->params, "Amp", 0.8, 0.00001, 1, 0.01, 0,
                            SYNTH_PARAM_AMP, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_FREQ] =
            param_add_param(cur_osc->params, "Freq", 0, -12, 12, 0.1, 0,
                            SYNTH_PARAM_FREQ, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_SPREAD] =
            param_add_param(cur_osc->params, "Spread", 0, 0, 1, 0.01, 0,
                            SYNTH_PARAM_SPREAD, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_WOBBLE] =
            param_add_param(cur_osc->params, "Wobble", 0, 0, 1, 0.05, 0,
                            SYNTH_PARAM_WOBBLE, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_OCTAVE] = param_add_param(
            cur_osc->params, "Octave", 0, ((MAX_SEMITONES - 12) / 12.0) * -1,
            (MAX_SEMITONES - 12) / 12.0, 1, 0, SYNTH_PARAM_OCTAVE, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_TABLE] =
            param_add_param(cur_osc->params, "Table", 0, 0, 3, 1, 0,
                            SYNTH_PARAM_TABLE, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_ATTACK] = param_add_param(
            cur_osc->params, "A", 0.0, SYNTH_ADSR_TIME_MIN, SYNTH_ADSR_TIME_MAX,
            0.1, 0, SYNTH_PARAM_ATTACK, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_DECAY] = param_add_param(
            cur_osc->params, "D", 0.0, SYNTH_ADSR_TIME_MIN, SYNTH_ADSR_TIME_MAX,
            0.1, 0, SYNTH_PARAM_DECAY, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_SUSTAIN] =
            param_add_param(cur_osc->params, "S", 1.0, 0.0, 1.0, 0.01, 0,
                            SYNTH_PARAM_SUSTAIN, 0, 0, NULL);
        cur_osc->val_id[SYNTH_PARAM_RELEASE] = param_add_param(
            cur_osc->params, "R", 0.001, SYNTH_ADSR_TIME_MIN,
            SYNTH_ADSR_TIME_MAX, 0.1, 0, SYNTH_PARAM_RELEASE, 0, 0, NULL);

        if (synth_osc_ports_create(synth_data, cur_osc) != 0) {
            synth_clean_memory(synth_data);
            return NULL;
        }
    }

    return synth_data;
}

static int synth_osc_ports_create(SYNTH_DATA *synth_data, SYNTH_OSC *osc) {
    osc->node = graph_node_add(synth_data->graph, synth_data->owner_tag,
                               synth_osc_uid(osc), synth_osc_process_rt, osc);
    if (!osc->node)
        return -1;
    for (unsigned int i = 0; i < osc->num_ports; i++) {
        SYNTH_PORT *cur_port = &(osc->ports[i]);
        cur_port->graph_port =
            graph_port_create(synth_data->graph, osc->node, cur_port->port_type,
                              cur_port->port_flow, cur_port->port_name);
        if (!cur_port->graph_port)
            return -1;
    }
    return 0;
}

static int synth_process_adsr(SYNTH_ADSR *adsr, unsigned int voice_playing,
                              PARAM_T *ret_amp) {
    if (!adsr)
        return -1;

    PARAM_T a_frames = adsr->a * adsr->samplerate;
    PARAM_T d_frames = adsr->d * adsr->samplerate;
    PARAM_T r_frames = adsr->r * adsr->samplerate;

    if (voice_playing == 1) {
        adsr->time_frames += 1;

        if (adsr->phase == 0) {
            adsr->phase = 1;
            if (adsr->a == 0.0)
                adsr->amp = 1.0;
        }
    }
    if (voice_playing == 0 && adsr->phase != 4) {
        adsr->phase = 4;
        adsr->r_amp = adsr->amp;
        adsr->r_frames = 0;
    }
    int ret_val = 0;
    // attack phase
    if (adsr->phase == 1) {
        adsr->amp =
            fit_range(a_frames, 0.0, 1.0, 0.0, (PARAM_T)adsr->time_frames);
        ret_val = 1;

        if (adsr->amp >= 1.0 || adsr->time_frames >= a_frames) {
            adsr->phase = 2;
            adsr->amp = 1.0;
        }
    }

    if (adsr->phase == 2) {
        adsr->amp = fit_range(a_frames + d_frames, a_frames, adsr->s, 1.0,
                              (PARAM_T)adsr->time_frames);
        ret_val = 2;

        if (adsr->amp <= adsr->s ||
            adsr->time_frames >= (a_frames + d_frames)) {
            if (adsr->s > 0.0)
                adsr->phase = 3;
            if (adsr->s <= 0.0)
                adsr->phase = 4;
        }
    }

    if (adsr->phase == 3) {
        adsr->amp = adsr->s;
        ret_val = 3;
    }

    if (adsr->phase == 4) {
        adsr->amp =
            fit_range(r_frames, 0.0, 0.0, adsr->r_amp, (PARAM_T)adsr->r_frames);
        adsr->r_frames += 1;
        ret_val = 4;
        if (adsr->amp <= 0.0 || adsr->r <= 0.0) {
            adsr->amp = 0.0;
            adsr->phase = 5;
            ret_val = 5;
        }
    }
    *ret_amp = adsr->amp;
    return ret_val;
}

static void synth_adsr_reset(SYNTH_ADSR *adsr) {
    if (!adsr)
        return;
    adsr->a = 0.0;
    adsr->d = 0.0;
    adsr->s = 0.0;
    adsr->r = 0.0;
    adsr->amp = 0.0;
    adsr->phase = 0;
    adsr->r_amp = 0.0;
    adsr->r_frames = 0;
    adsr->time_frames = 0;
}

static void synth_adsr_update(SYNTH_ADSR *adsr, PARAM_T a, PARAM_T d, PARAM_T s,
                              PARAM_T r, PARAM_T samplerate) {
    if (!adsr)
        return;
    adsr->a = a;
    adsr->d = d;
    adsr->s = s;
    adsr->r = r;
    adsr->samplerate = samplerate;
}

// once per cycle - the Amp read steps its smoothing ramp
static void synth_osc_params_read_rt(SYNTH_OSC *osc) {
    SYNTH_OSC_VALS *vals = &(osc->vals);
    vals->amp = param_get_value(osc->params, osc->val_id[SYNTH_PARAM_AMP], 1);
    vals->freq = param_get_value(osc->params, osc->val_id[SYNTH_PARAM_FREQ], 1);
    vals->octave =
        param_get_value(osc->params, osc->val_id[SYNTH_PARAM_OCTAVE], 1);
    vals->wobble =
        param_get_value(osc->params, osc->val_id[SYNTH_PARAM_WOBBLE], 1);
    vals->spread =
        param_get_value(osc->params, osc->val_id[SYNTH_PARAM_SPREAD], 1);
    vals->a = param_get_value(osc->params, osc->val_id[SYNTH_PARAM_ATTACK], 1);
    vals->d = param_get_value(osc->params, osc->val_id[SYNTH_PARAM_DECAY], 1);
    vals->s = param_get_value(osc->params, osc->val_id[SYNTH_PARAM_SUSTAIN], 1);
    vals->r = param_get_value(osc->params, osc->val_id[SYNTH_PARAM_RELEASE], 1);
}

// render the voices into the osc outputs for frames [start, end), with the
// values of synth_osc_params_read_rt
static void synth_process_osc_voices(SYNTH_DATA *synth_data, SYNTH_OSC *osc,
                                     NFRAMES_T start, NFRAMES_T end) {
    if (!osc)
        return;
    if (!osc->osc_voices)
        return;
    const SYNTH_OSC_VALS *vals = &(osc->vals);

    for (unsigned int i = 0; i < osc->num_voices; i++) {
        SYNTH_VOICE *cur_voice = &(osc->osc_voices[i]);
        if (!cur_voice)
            continue;
        if (!cur_voice->osc_table)
            continue;

        if (cur_voice->stopped == 1) {
            synth_adsr_reset(cur_voice->vco_adsr);
            continue;
        }
        OSC_OBJ *osc_table = cur_voice->osc_table;
        // per segment too - a note started mid-cycle reset them
        synth_adsr_update(cur_voice->vco_adsr, vals->a, vals->d, vals->s,
                          vals->r, synth_data->samplerate);

        // this voices random value
        srand((i + 1) * cur_voice->rand_seed);
        int rand_val = rand();
        // spread randomness
        PARAM_T spread_am = 0;
        if (vals->spread != 0) {
            spread_am = fit_range((PARAM_T)RAND_MAX, 0.0, vals->spread,
                                  vals->spread * -1, (PARAM_T)rand_val);
        }
        // wobble frequency randomness
        PARAM_T wobble_freq = 1.5;
        PARAM_T wobble_am = 0.0;
        if (vals->wobble != 0) {
            wobble_freq = fit_range((PARAM_T)RAND_MAX, 0.0, wobble_freq * 0.5,
                                    wobble_freq, (PARAM_T)rand_val);
            wobble_am = fit_range((PARAM_T)RAND_MAX, 0.0, vals->wobble,
                                  vals->wobble * 0.5, (PARAM_T)rand_val);
        }

        PARAM_T freq = midi_note_to_freq(cur_voice->midi_note);
        // linear midi vel to amp
        PARAM_T midi_amp = fit_range(127.0, 0.0, 1.0, 0.0, cur_voice->midi_vel);
        // midi vel to amp with a curve
        midi_amp =
            math_range_table_convert_value(synth_data->log_curve, midi_amp);

        // process the wavetable, get buffer
        for (NFRAMES_T j = start; j < end; j++) {
            // add the octaves and semitones
            PARAM_T octaves_semitones = vals->octave * 12;
            PARAM_T freq_final = freq * math_range_table_convert_value(
                                            synth_data->semi_to_freq_table,
                                            octaves_semitones + vals->freq);
            // process the adsr
            PARAM_T adsr_amp = 1.0;
            int adsr_phase = synth_process_adsr(cur_voice->vco_adsr,
                                                cur_voice->playing, &adsr_amp);
            // spread will help spread the voices in stereo, by simply making
            // random voices more quite in left or right side
            PARAM_T spread_mult_L = 1;
            PARAM_T spread_mult_R = 1;
            if (vals->spread != 0) {
                if (spread_am < 0)
                    spread_mult_R -= (spread_am * -1);
                if (spread_am > 0)
                    spread_mult_L -= spread_am;
            }

            // randomly wobble the voices if the parameter is not 0
            if (vals->wobble != 0) {
                PARAM_T wobble_semitones =
                    osc_getOutput(osc->sin_osc, cur_voice->wobble_ph,
                                  wobble_freq, 0, 0) *
                    wobble_am;
                freq_final = freq_final * math_range_table_convert_value(
                                              synth_data->semi_to_freq_table,
                                              wobble_semitones);
                osc_updatePhase(osc->sin_osc, &(cur_voice->wobble_ph),
                                wobble_freq);
            }

            PARAM_T wave_sample_L =
                osc_getOutput(osc_table, cur_voice->vco_ph, freq_final, 0, 0);
            PARAM_T wave_sample_R = wave_sample_L;
            osc_updatePhase(osc_table, &(cur_voice->vco_ph), freq_final);

            PARAM_T interp_amp_in_L = math_ramp_val_get_value(
                cur_voice->vco_amp_L,
                vals->amp * adsr_amp * spread_mult_L * midi_amp);
            PARAM_T interp_amp_in_R = math_ramp_val_get_value(
                cur_voice->vco_amp_R,
                vals->amp * adsr_amp * spread_mult_R * midi_amp);

            osc->out_L[j] +=
                wave_sample_L * math_range_table_convert_value(
                                    synth_data->amp_to_exp, interp_amp_in_L);
            osc->out_R[j] +=
                wave_sample_R * math_range_table_convert_value(
                                    synth_data->amp_to_exp, interp_amp_in_R);

            if (adsr_phase == 5) {
                cur_voice->playing = 0;
            }
            // fully stop only when the amplitude is 0 and adsr release phase is
            // finished (adsr_phase == 5)
            if (adsr_phase == 5 && interp_amp_in_L <= 0 &&
                interp_amp_in_R <= 0) {
                cur_voice->stopped = 1;
                synth_adsr_reset(cur_voice->vco_adsr);
                break;
            }
        }
    }
}

// point out_L/out_R at this cycle's audio out ports, cleared. false if missing
static bool synth_osc_outs_rt(SYNTH_OSC *osc, NFRAMES_T nframes) {
    // the metronome osc has no midi in port
    SYNTH_PORT *l_Port = NULL;
    SYNTH_PORT *r_Port = NULL;
    if (osc->num_ports == 2) {
        l_Port = &(osc->ports[0]);
        r_Port = &(osc->ports[1]);
    } else {
        l_Port = &(osc->ports[1]);
        r_Port = &(osc->ports[2]);
    }

    osc->out_L = graph_port_audio_rt(l_Port->graph_port);
    osc->out_R = graph_port_audio_rt(r_Port->graph_port);
    if (!osc->out_L || !osc->out_R)
        return false;
    memset(osc->out_L, '\0', sizeof(SAMPLE_T) * nframes);
    memset(osc->out_R, '\0', sizeof(SAMPLE_T) * nframes);
    return true;
}

// this function finds a voice to play, does not produce any sound though
static void synth_play_osc_rt(SYNTH_OSC *osc, MIDI_DATA_T vel, MIDI_DATA_T note,
                              PARAM_T rand_seed) {
    if (!osc)
        return;
    // go through the voices and if its stopped, play it
    if (!osc->osc_voices)
        return;
    unsigned int found_voice = 0;
    unsigned int next_voice = osc->last_voice + 1;
    unsigned int count = 0;
    SYNTH_VOICE *to_play_voice = NULL;
    while (found_voice == 0) {
        if (next_voice >= osc->num_voices)
            next_voice = 0;
        SYNTH_VOICE *cur_voice = &(osc->osc_voices[next_voice]);
        if (!cur_voice)
            break;
        if (cur_voice->stopped == 1) {
            to_play_voice = cur_voice;
            // calc a random phase for this voice, so each voice does not start
            // on the same phase
            srand((unsigned int)(vel * (to_play_voice->id + 1) * 45999));
            to_play_voice->vco_ph =
                fit_range((PARAM_T)RAND_MAX, 0.0, 1.0, 0.0, (PARAM_T)rand());
            found_voice = 1;
            break;
        }

        next_voice += 1;
        count += 1;
        // if all voices are playing break and then simply choose the next voice
        // after the last_voice and reset it after this loop at the end of this
        // funciton
        if (count >= osc->num_voices)
            break;
    }

    if (found_voice == 0) {
        // if all voices where busy, find next voice after the last voice played
        // and play it, also reseting it
        next_voice = osc->last_voice + 1;
        if (next_voice >= osc->num_voices)
            next_voice = 0;
        SYNTH_VOICE *cur_voice = &(osc->osc_voices[next_voice]);
        if (!cur_voice)
            return;
        to_play_voice = cur_voice;
    }

    // if we found the next voice to play reset it
    if (to_play_voice) {
        to_play_voice->playing = 1;
        to_play_voice->stopped = 0;
        to_play_voice->rand_seed = (unsigned int)rand_seed;
        synth_adsr_reset(to_play_voice->vco_adsr);
        to_play_voice->midi_note = note;
        to_play_voice->midi_vel = vel;
        osc->last_voice = to_play_voice->id;

        // set which table to play for the voice
        // its set before playing the voice so the table does not change while
        // the sound is playing
        to_play_voice->osc_table = osc->sin_osc;
        PARAM_T table =
            param_get_value(osc->params, osc->val_id[SYNTH_PARAM_TABLE], 1);
        if (table == SIN_WAVETABLE)
            to_play_voice->osc_table = osc->sin_osc;
        if (table == TRIANGLE_WAVETABLE)
            to_play_voice->osc_table = osc->triang_osc;
        if (table == SAW_WAVETABLE)
            to_play_voice->osc_table = osc->saw_osc;
        if (table == SQUARE_WAVETABLE)
            to_play_voice->osc_table = osc->sqr_osc;
    }
}
// stop a voice of the osc, that matches the note given
// if stop_all == 1, stop all the voices of the oscillator
static void synth_stop_osc_rt(SYNTH_OSC *osc, MIDI_DATA_T vel, MIDI_DATA_T note,
                              unsigned int stop_all) {
    (void)vel;
    if (!osc)
        return;
    if (!osc->osc_voices)
        return;
    // go through voices and stop them all or just the voice that was played
    // with the note dont reset the stopped voices, because they will reset
    // themselfs when adsr goes to 0
    for (unsigned int i = 0; i < osc->num_voices; i++) {
        SYNTH_VOICE *cur_voice = &(osc->osc_voices[i]);
        if (!cur_voice)
            continue;
        if (stop_all == 1) {
            cur_voice->playing = 0;
            continue;
        }
        if (note == cur_voice->midi_note && cur_voice->playing == 1) {
            cur_voice->playing = 0;
        }
    }
}

// one segment, triggered at frame 0 from the transport
static int synth_metronome_process_rt(SYNTH_DATA *synth_data,
                                      SYNTH_OSC *metro_osc, NFRAMES_T nframes) {
    if (!metro_osc)
        return -1;
    int32_t bar = 1;
    int32_t beat = 1;
    int32_t tick = 0;
    SAMPLE_T ticks_per_beat = 0;
    NFRAMES_T total_frames = 0;
    float bpm = 0;
    float beat_type = 0;
    float beats_per_bar = 0;
    int playhead = app_jack_return_transport_rt(
        synth_data->transport, &bar, &beat, &tick, &ticks_per_beat,
        &total_frames, &bpm, &beat_type, &beats_per_bar);
    if (playhead == 0) {
        synth_stop_osc_rt(metro_osc, 127, 0, 1);
    }
    if (playhead == 1) {
        // calculate if we need to trigger the metronome oscillator
        unsigned int play_osc = 0;
        if (metro_osc->trig != beat) {
            metro_osc->trig = beat;
            play_osc = 1;
        }

        if (play_osc == 1) {
            MIDI_DATA_T pitch = 60;
            if (beat == 1)
                pitch = 72;
            synth_play_osc_rt(metro_osc, 127, pitch, beat);
        }
    }

    synth_process_osc_voices(synth_data, metro_osc, 0, nframes);

    return 0;
}

// render up to each MIDI event, then apply it - every event acts at its frame
static void synth_osc_midi_process_rt(SYNTH_DATA *synth_data,
                                      SYNTH_OSC *osc, NFRAMES_T nframes) {
    SYNTH_PORT *midi_port = &(osc->ports[0]);
    NFRAMES_T pos = 0;
    MIDI_BUF_ITER it = midi_buf_iter(graph_port_midi_rt(midi_port->graph_port));
    MIDI_EVENT ev;
    while (midi_buf_next(&it, &ev)) {
        if (ev.frame > pos) {
            synth_process_osc_voices(synth_data, osc, pos, ev.frame);
            pos = ev.frame;
        }
        if (midi_is_note_on(ev.data))
            synth_play_osc_rt(
                osc, ev.data[2], ev.data[1],
                (ev.frame + ev.data[2] + ((uint32_t)osc->id * 988)));
        else if (midi_is_note_off(ev.data))
            synth_stop_osc_rt(osc, ev.data[2], ev.data[1], 0);
    }
    synth_process_osc_voices(synth_data, osc, pos, nframes);
}

bool synth_osc_process_rt(void *osc_ptr, NFRAMES_T nframes) {
    SYNTH_OSC *osc = (SYNTH_OSC *)osc_ptr;
    if (!osc)
        return false;
    SYNTH_DATA *synth_data = osc->synth_data;
    if (!synth_data || synth_data->processing == 0)
        return false;
    if (!synth_osc_outs_rt(osc, nframes))
        return false;

    synth_osc_params_read_rt(osc);
    // osc 0 is the metronome, if there is one
    if (osc->id == 0 && synth_data->with_metronome == 1)
        synth_metronome_process_rt(synth_data, osc, nframes);
    else
        synth_osc_midi_process_rt(synth_data, osc, nframes);
    // TODO the highest value or average of the outputs could go to a
    // read-only param, to show for example Osc volume levels
    return true;
}

void *synth_osc_return(SYNTH_DATA *synth_data, unsigned int osc_num) {
    if (!synth_data)
        return NULL;
    if (osc_num >= synth_data->num_osc)
        return NULL;
    return (void *)&(synth_data->osc_array[osc_num]);
}

const char *synth_osc_name(void *osc) {
    SYNTH_OSC *cur_osc = (SYNTH_OSC *)osc;
    if (!cur_osc)
        return NULL;
    return cur_osc->name;
}

uint32_t synth_osc_uid(void *osc) {
    SYNTH_OSC *cur_osc = (SYNTH_OSC *)osc;
    if (!cur_osc)
        return 0;
    // oscillators are fixed at init and never removed, so the slot number is a
    // permanent identity on its own - no monotonic counter needed
    return (uint32_t)(cur_osc->id + 1);
}

PRM_CONTAIN *synth_osc_param_container(void *osc) {
    SYNTH_OSC *cur_osc = (SYNTH_OSC *)osc;
    if (!cur_osc)
        return NULL;
    return cur_osc->params;
}

size_t synth_return_osc_num(SYNTH_DATA *synth_data) {
    if (!synth_data)
        return 0;
    return synth_data->num_osc;
}

// a new ramp of samples in place of *ramp, the old one kept on failure
static void synth_ramp_renew(MATH_RAMP_VAL **ramp, unsigned int samples) {
    MATH_RAMP_VAL *fresh = math_ramp_val_init(1.0, samples);
    if (!fresh)
        return;
    free(*ramp);
    *ramp = fresh;
}

int synth_sample_rate_set(SYNTH_DATA *synth_data, SAMPLE_T sample_rate) {
    if (!synth_data)
        return -1;
    if (sample_rate == synth_data->samplerate)
        return 0;
    // built for a rate, made before the stop
    OSC_OBJ *tables[] = {
        osc_init_osc_wavetable(SIN_WAVETABLE, sample_rate),
        osc_init_osc_wavetable(TRIANGLE_WAVETABLE, sample_rate),
        osc_init_osc_wavetable(SAW_WAVETABLE, sample_rate),
        osc_init_osc_wavetable(SQUARE_WAVETABLE, sample_rate)};
    size_t table_count = sizeof(tables) / sizeof(tables[0]);
    for (size_t t = 0; t < table_count; t++) {
        if (tables[t])
            continue;
        for (size_t f = 0; f < table_count; f++)
            osc_clean_osc_wavetable(tables[f]);
        return -1;
    }
    // [audio-thread] reads the tables, the rate and the voices
    context_sub_wait_for_stop(synth_data->control_data, (void *)synth_data);
    OSC_OBJ **current[] = {&synth_data->sin_osc, &synth_data->triang_osc,
                           &synth_data->saw_osc, &synth_data->sqr_osc};
    for (size_t t = 0; t < table_count; t++) {
        OSC_OBJ *old = *current[t];
        *current[t] = tables[t];
        tables[t] = old;
    }
    synth_data->samplerate = sample_rate;
    unsigned int ramp_samples =
        (unsigned int)(SYNTH_VOICE_AMP_RAMP_SECS * sample_rate);
    for (size_t i = 0; i < synth_data->num_osc; i++) {
        SYNTH_OSC *osc = &(synth_data->osc_array[i]);
        osc->sin_osc = synth_data->sin_osc;
        osc->triang_osc = synth_data->triang_osc;
        osc->saw_osc = synth_data->saw_osc;
        osc->sqr_osc = synth_data->sqr_osc;
        // a playing voice ends, its table and ramps were for the old rate
        for (unsigned int j = 0; osc->osc_voices && j < osc->num_voices; j++) {
            SYNTH_VOICE *voice = &(osc->osc_voices[j]);
            voice->playing = 0;
            voice->stopped = 1;
            voice->osc_table = NULL;
            synth_adsr_reset(voice->vco_adsr);
            synth_ramp_renew(&voice->vco_amp_L, ramp_samples);
            synth_ramp_renew(&voice->vco_amp_R, ramp_samples);
        }
    }
    context_sub_wait_for_start(synth_data->control_data, (void *)synth_data);
    // the old tables, no voice holds them any more
    for (size_t t = 0; t < table_count; t++)
        osc_clean_osc_wavetable(tables[t]);
    return 0;
}

static int synth_clean_osc(SYNTH_DATA *synth_data, SYNTH_OSC *synth_osc) {
    if (!synth_osc)
        return -1;
    if (synth_osc->params)
        param_clean_param_container(synth_osc->params);
    synth_osc->params = NULL;
    if (synth_osc->amp_smooth)
        free(synth_osc->amp_smooth);
    synth_osc->amp_smooth = NULL;
    if (synth_osc->osc_voices) {
        for (unsigned int i = 0; i < synth_osc->num_voices; i++) {
            SYNTH_VOICE *cur_voice = &(synth_osc->osc_voices[i]);
            if (cur_voice) {
                if (cur_voice->vco_amp_L)
                    free(cur_voice->vco_amp_L);
                if (cur_voice->vco_amp_R)
                    free(cur_voice->vco_amp_R);
                if (cur_voice->vco_adsr)
                    free(cur_voice->vco_adsr);
            }
        }
        free(synth_osc->osc_voices);
        synth_osc->osc_voices = NULL;
    }
    // its ports go with it
    graph_node_remove(synth_data->graph, synth_osc->node);
    synth_osc->node = NULL;
    free(synth_osc->ports);
    synth_osc->ports = NULL;
    if (synth_osc->name)
        free(synth_osc->name);
    synth_osc->name = NULL;

    return 0;
}

int synth_clean_memory(SYNTH_DATA *synth_data) {
    if (!synth_data)
        return -1;
    if (synth_data->osc_array) {
        for (unsigned int i = 0; i < synth_data->num_osc; i++) {
            synth_clean_osc(synth_data, &(synth_data->osc_array[i]));
        }
        free(synth_data->osc_array);
    }
    if (synth_data->triang_osc)
        osc_clean_osc_wavetable(synth_data->triang_osc);
    if (synth_data->saw_osc)
        osc_clean_osc_wavetable(synth_data->saw_osc);
    if (synth_data->sqr_osc)
        osc_clean_osc_wavetable(synth_data->sqr_osc);
    if (synth_data->sin_osc)
        osc_clean_osc_wavetable(synth_data->sin_osc);
    if (synth_data->semi_to_freq_table)
        math_range_table_clean(synth_data->semi_to_freq_table);
    if (synth_data->log_curve)
        math_range_table_clean(synth_data->log_curve);
    if (synth_data->amp_to_exp)
        math_range_table_clean(synth_data->amp_to_exp);

    context_sub_clean(synth_data->control_data);

    free(synth_data);

    return 0;
}
