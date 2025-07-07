#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "synth.h"
#include <math.h>
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/osc_wavelookup.h"
#include "../jack_funcs/jack_funcs.h"
#include "../util_funcs/math_funcs.h"
#include "context_control.h"
#include <threads.h>

static thread_local bool is_audio_thread = false;

//max voices that can play simultaniously
#define MAX_SYNTH_VOICES 8
//how many oscillators there should be
#define MAX_OSCS 3
//number of output Audio ports for the whole synth
#define SYNTH_OUTS 2
//number of midi in ports for the synth
#define SYNTH_IN_MIDI 1
//Samples for wavetables
#define OSC_TABLE_SAMPLES 2048
//Maximum semitones, also lower than 0, for semitone to frequency conversion
//using this number a table is built for converting semitones to frequency,
//given the starting frequency
#define MAX_SEMITONES 36
//the increments that the semitones will be incremented or decreased by the user
#define SEMITONES_INC 0.1
//the longest that the a, d or r in ADSR can be in seconds
#define ADSR_MAX_TIME 5

typedef struct _synth_adsr{
    PARAM_T amp;//the current calculated amp from the adsr
    PARAM_T r_amp; //release amp, that holds the amp that was at the release phase initiation
    unsigned int phase;//current phase of the ADSR (1 - A, 2 - D etc.)
    int time_frames;//how long the adsr is progressing
    int r_frames;//release frames to calc when to end the release phase
    PARAM_T a;
    PARAM_T d;
    PARAM_T s;
    PARAM_T r;
    SAMPLE_T samplerate;
}SYNTH_ADSR;

typedef struct _synth_voice{
    int id;
    //what midi note initialized this voice
    unsigned char midi_note;
    //midi vel that triggered this voice
    SAMPLE_T midi_vel;
    //should this voice be playing
    unsigned int playing;
    //this indicates that the voice is fully stopped and its amplitude is 0
    unsigned int stopped;
    //what the user wishes the vco level to be, its 0 when playing is 0
    //this is to interpolate so the amp does not go to big values too fast
    PRM_INTERP_VAL* vco_amp_L;
    PRM_INTERP_VAL* vco_amp_R;
    //the voice amp adsr
    SYNTH_ADSR* vco_adsr;
    //the current phase of the vco
    PARAM_T vco_ph;
    //the current phase of the detune wobble lfo
    PARAM_T wobble_ph;
    //random seed for random stuff in the voice, this should
    //be updated when the voices starts playing
    unsigned int rand_seed;
    //address of the table to use when playing
    OSC_OBJ* osc_table;
}SYNTH_VOICE;

typedef struct _synth_osc{
    int id;
    //trigger, that can be used to track various oscillator behaviour
    //for example for metronome osc this is used to test if the oscillator needs to be played
    //starts at -1, when the osc is initialized for the first time
    int trig;
    //name of the osc that will be returned to the ui
    char* name;
    //the voice array for the oscillator
    //the voice gets its parameters from this struct
    SYNTH_VOICE* osc_voices;
    //the number of voices for the oscillator, usually its the same amount for all oscillators
    //but for example the metronome only has 2
    unsigned int num_voices;
    //parameter container for the oscillator
    PRM_CONTAIN* params;
    //which voice played last
    int last_voice;
    //the buffer of the summed voices output is kept here
    SAMPLE_T* buffer_L;
    SAMPLE_T* buffer_R;
    //for convenience osc tables on the osc struct
    OSC_OBJ* triang_osc;
    OSC_OBJ* sqr_osc;
    OSC_OBJ* saw_osc;
    OSC_OBJ* sin_osc;
    //ports for this oscillator
    //the synth port array
    SYNTH_PORT* ports;
    //how many ports are there
    unsigned int num_ports;
}SYNTH_OSC;

typedef struct _synth_port{
    unsigned int id;
    //midi or audio
    unsigned int port_type;
    //in or output
    unsigned int port_flow;
    char* port_name;
    //the system port for the audio system (for ex jack)
    void* sys_port;
}SYNTH_PORT;

typedef struct _synth_data{
    //size of the single buffer (nframes in jack) for the rt thread process function cycle
    unsigned int buffer_size;
    //oscillator object with the triangle table
    OSC_OBJ* triang_osc;
    //oscillator object with the square table
    OSC_OBJ* sqr_osc;
    //oscillator object with the saw table
    OSC_OBJ* saw_osc;
    //oscillator object with the sin table
    OSC_OBJ* sin_osc;
    //table that has frequency multipliers for semitones from -MAX_SEMITONES to MAX_SAMITONES
    //in SEMITONES_INC increments
    MATH_RANGE_TABLE* semi_to_freq_table;
    //table that has a curve 0..1 in a logarithmic fashion, basically inverse amp_to_exp curve
    //but with a gentler slope (1/4 instead of 1/10 squish) good curve for midi vel curves
    MATH_RANGE_TABLE* log_curve;
    //table that has the exponential conversion for amplitude (so 0.5 amp is half the percieved loudness or 0.0313 or so)
    MATH_RANGE_TABLE* amp_to_exp;
    //sample rate for the current audio system (48000, 44100 etc.)
    SAMPLE_T samplerate;
    //should the metronome be initialized and processed
    unsigned int with_metronome;
    //the synth oscillators
    //Synth_Osc 0 is reserved for the metronome
    SYNTH_OSC* osc_array;
    //how many oscilators we have
    unsigned int num_osc;
    //midi container that holds the notes, velocities etc.
    JACK_MIDI_CONT* midi_cont;
    //this is the audio backend object to send to the audio functions
    void* audio_backend;
    //this is control for [audio-thread] and [main-thread] sys communication
    //(since there is no need to remove and add the oscillators this is used right now only for thread safe message sending)
    CXCONTROL* control_data;
}SYNTH_DATA;

static int synth_sys_msg(void* user_data, const char* msg){
    SYNTH_DATA* synth_data = (SYNTH_DATA*)user_data;
    log_append_logfile("%s", msg);
    return 0;
}

int synth_read_ui_to_rt_messages(SYNTH_DATA* synth_data){
    //this is a local thread var its false on [main-thread] and true on [audio-thread]
    is_audio_thread = true;

    if(!synth_data)return -1;
    //process the sys messages, right now only need for send messages, so on [audio-thread] does not do anything
    context_sub_process_rt(synth_data->control_data);
    
    for(unsigned int i = 0; i < MAX_OSCS; i++){
	SYNTH_OSC* osc = &(synth_data->osc_array[i]);
	if(!osc->params)continue;
	param_msgs_process(osc->params, 1);
    }
    return 0;
}
int synth_read_rt_to_ui_messages(SYNTH_DATA* synth_data){
    if(!synth_data)return -1;
    //process the sys messages on [main-thread] - right now only logs messages from [audio-thread]
    context_sub_process_ui(synth_data->control_data);
    
    //read the param rt_to_ui messages and set the parameter values
    for(unsigned int i = 0; i < MAX_OSCS; i++){
	SYNTH_OSC* osc = &(synth_data->osc_array[i]);
	if(!osc->params)continue;
	param_msgs_process(osc->params, 0);
    }    
    return 0;
}

//init the adsr
static SYNTH_ADSR* synth_init_adsr(SAMPLE_T samplerate){
    SYNTH_ADSR* adsr = malloc(sizeof(SYNTH_ADSR));
    adsr->a = 0.0;
    adsr->d = 0.0;
    adsr->s = 0.0;
    adsr->r = 0.0;
    adsr->amp =  0.0;
    adsr->r_amp = 0.0;
    adsr->phase = 0;
    adsr->samplerate = samplerate;
    adsr->time_frames = 0;
    adsr->r_frames = 0;
    
    return adsr;
}

SYNTH_DATA* synth_init (unsigned int buffer_size, SAMPLE_T sample_rate, const char* cx_name, unsigned int with_metronome,
			void* audio_backend){

    SYNTH_DATA* synth_data = (SYNTH_DATA*)malloc(sizeof(SYNTH_DATA));
    if(!synth_data)return NULL;
    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    ui_funcs_struct.send_msg = synth_sys_msg;
    synth_data->control_data = context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if(!synth_data->control_data){
	free(synth_data);
	return NULL;
    }    
    synth_data->buffer_size = buffer_size;
    synth_data->samplerate = sample_rate;
    synth_data->with_metronome = with_metronome;
    synth_data->num_osc = MAX_OSCS;
    synth_data->osc_array = NULL;
    synth_data->saw_osc = NULL;
    synth_data->sqr_osc = NULL;
    synth_data->triang_osc = NULL;
    synth_data->sin_osc = NULL;
    synth_data->midi_cont = NULL;
    synth_data->audio_backend = audio_backend;
    synth_data->semi_to_freq_table = NULL;
    synth_data->log_curve = NULL;
    synth_data->amp_to_exp = NULL;
    
    synth_data->semi_to_freq_table = math_init_range_table(MAX_SEMITONES * -1, MAX_SEMITONES, SEMITONES_INC);
    
    if(!synth_data->semi_to_freq_table){
	synth_clean_memory(synth_data);
	return NULL;
    }
    //fill the semitone to frequency multiplier convert table with values
    for(int i = 0; i < math_range_table_get_len(synth_data->semi_to_freq_table); i++){
	PARAM_T cur_semitones = math_range_table_get_value(synth_data->semi_to_freq_table, i);
	PARAM_T cur_freq_ratio = exp_range_ratio(12.0, cur_semitones);
	math_range_table_enter_value(synth_data->semi_to_freq_table, i, cur_freq_ratio);
    }
    
    synth_data->log_curve = math_init_range_table(0.0, 1.0, 0.005);
    if(!synth_data->log_curve){
	synth_clean_memory(synth_data);
	return NULL;
    }
    //fill the midivel to amp table with values
    for(int i = 0; i < math_range_table_get_len(synth_data->log_curve); i++){
	PARAM_T cur_val = math_range_table_get_value(synth_data->log_curve, i);
	PARAM_T cur_log = 0.0;
	if(cur_val > 0.0){
	    cur_log = 0.25*(log10((double)cur_val)/log10(2.0)) + 1;
	}
	if(cur_val == 1.0) cur_log = 1.0;
	if(cur_log < 0.0) cur_log = 0.0;
	math_range_table_enter_value(synth_data->log_curve, i, cur_log);
    }

    synth_data->amp_to_exp = math_init_range_table(0.0, 1.0, 0.001);
    if(!synth_data->amp_to_exp){
	synth_clean_memory(synth_data);
	return NULL;
    }
    //fill the amp exponential table
    for(int i = 0; i < math_range_table_get_len(synth_data->amp_to_exp); i++){
	PARAM_T cur_amp = math_range_table_get_value(synth_data->amp_to_exp, i);
	PARAM_T cur_exp = pow(2.0, cur_amp * (10.0) - 10.0);
	if(cur_amp == 0) cur_exp = 0.0;
	math_range_table_enter_value(synth_data->amp_to_exp, i, cur_exp);
    }

    
    //init the midi container
    synth_data->midi_cont = app_jack_init_midi_cont(MAX_MIDI_CONT_ITEMS);
    if(!synth_data->midi_cont){
	synth_clean_memory(synth_data);
	return NULL;
    }

    //init the wavetable objects
    synth_data->sin_osc = osc_init_osc_wavetable(SIN_WAVETABLE, synth_data->samplerate);
    if(!synth_data->sin_osc){
	synth_clean_memory(synth_data);
	return NULL;
    }
    synth_data->triang_osc = osc_init_osc_wavetable(TRIANGLE_WAVETABLE, synth_data->samplerate);
    if(!synth_data->triang_osc){
	synth_clean_memory(synth_data);
	return NULL;
    }    
    synth_data->saw_osc = osc_init_osc_wavetable(SAW_WAVETABLE, synth_data->samplerate);
    if(!synth_data->saw_osc){
	synth_clean_memory(synth_data);
	return NULL;
    }
    synth_data->sqr_osc = osc_init_osc_wavetable(SQUARE_WAVETABLE, synth_data->samplerate);
    if(!synth_data->sqr_osc){
	synth_clean_memory(synth_data);
	return NULL;
    }

    synth_data->osc_array = (SYNTH_OSC*)calloc(synth_data->num_osc, sizeof(SYNTH_OSC));
    if(!synth_data->osc_array){
	synth_clean_memory(synth_data);
	return NULL;
    }
    for(int i = 0; i < synth_data->num_osc; i++){
	SYNTH_OSC* cur_osc = &(synth_data->osc_array[i]);
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
	cur_osc->buffer_L = NULL;
	cur_osc->buffer_R = NULL;
	cur_osc->name = NULL;
	cur_osc->num_ports = 0;
	cur_osc->ports = NULL;
	cur_osc->num_voices = MAX_SYNTH_VOICES;	
	//initiate the buffer
	cur_osc->buffer_L = calloc(synth_data->buffer_size, sizeof(SAMPLE_T));
	cur_osc->buffer_R = calloc(synth_data->buffer_size, sizeof(SAMPLE_T));
	if(!cur_osc->buffer_L || !cur_osc->buffer_R){
	    synth_clean_memory(synth_data);
	    return NULL;
	}
	cur_osc->name = malloc(sizeof(char) * 6);
	if(!cur_osc->name){
	    synth_clean_memory(synth_data);
	    return NULL;
	}
	sprintf(cur_osc->name, "Osc_%u", i);
	
	//create the oscillator ports
	cur_osc->num_ports = SYNTH_OUTS + SYNTH_IN_MIDI;
	cur_osc->ports = (SYNTH_PORT*)calloc(cur_osc->num_ports, sizeof(SYNTH_PORT));
	if(!cur_osc->ports){
	    synth_clean_memory(synth_data);
	    return NULL;
	}

	for(int j = 0; j < cur_osc->num_ports; j++){
	    SYNTH_PORT* cur_port = &(cur_osc->ports[j]);
	    
	    unsigned int name_len = strlen(cx_name);
	    name_len += strlen(cur_osc->name);
	    
	    cur_port->id = j;
	    if(j==0){
		cur_port->port_flow = FLOW_INPUT;
		cur_port->port_type = TYPE_MIDI;
		name_len += 10;
		cur_port->port_name = malloc(sizeof(char) * name_len);
		if(!cur_port->port_name){
		    synth_clean_memory(synth_data);
		    return NULL;
		}
		sprintf(cur_port->port_name, "%s|%s|midi_in", cx_name, cur_osc->name, i);
	    }
	    if(j==1){
		cur_port->port_flow = FLOW_OUTPUT;
		cur_port->port_type = TYPE_AUDIO;
		name_len += 8;
		cur_port->port_name = malloc(sizeof(char) * name_len);
		if(!cur_port->port_name){
		    synth_clean_memory(synth_data);
		    return NULL;
		}
		sprintf(cur_port->port_name, "%s|%s|out_L", cx_name, cur_osc->name, i);
	    }
	    if(j==2){
		cur_port->port_flow = FLOW_OUTPUT;
		cur_port->port_type = TYPE_AUDIO;
		name_len += 8;
		cur_port->port_name = malloc(sizeof(char) * name_len);
		if(!cur_port->port_name){
		    synth_clean_memory(synth_data);
		    return NULL;
		}
		sprintf(cur_port->port_name, "%s|%s|out_R", cx_name, cur_osc->name, i);
	    }
	}
	
	//if this is the 0 oscillator and the metronome should be initialized 
	if(i == 0 && synth_data->with_metronome == 1){
	    cur_osc->num_voices = 2;
	    if(cur_osc->name)free(cur_osc->name);
	    cur_osc->name = malloc(sizeof(char) * 4);
	    if(!cur_osc->name){
		synth_clean_memory(synth_data);
		return NULL;
	    }
	    sprintf(cur_osc->name, "Mtr");
	    //change the ports
	    synth_clean_ports(synth_data, &(cur_osc->ports), cur_osc->num_ports);
	    cur_osc->num_ports = SYNTH_OUTS;
	    cur_osc->ports = (SYNTH_PORT*)calloc(cur_osc->num_ports, sizeof(SYNTH_PORT));
	    if(!cur_osc->ports){
		synth_clean_memory(synth_data);
		return NULL;
	    }

	    for(int j = 0; j < cur_osc->num_ports; j++){
		SYNTH_PORT* cur_port = &(cur_osc->ports[j]);
		cur_port->id = j;
		unsigned int name_len = strlen(cx_name);
		name_len += strlen(cur_osc->name);		
		if(j==0){
		    cur_port->port_flow = FLOW_OUTPUT;
		    cur_port->port_type = TYPE_AUDIO;
		    name_len += 8;
		    cur_port->port_name = malloc(sizeof(char) * name_len);
		    if(!cur_port->port_name){
			synth_clean_memory(synth_data);
			return NULL;
		    }
		    sprintf(cur_port->port_name, "%s|%s|out_L", cx_name, cur_osc->name);
		}
		if(j==1){
		    cur_port->port_flow = FLOW_OUTPUT;
		    cur_port->port_type = TYPE_AUDIO;
		    name_len += 8;
		    cur_port->port_name = malloc(sizeof(char) * name_len);
		    if(!cur_port->port_name){
			synth_clean_memory(synth_data);
			return NULL;
		    }
		    sprintf(cur_port->port_name, "%s|%s|out_R", cx_name, cur_osc->name);
		}
	    }
	}
	
	cur_osc->osc_voices = (SYNTH_VOICE*)calloc(cur_osc->num_voices, sizeof(SYNTH_VOICE));
	if(!cur_osc->osc_voices){
	    synth_clean_memory(synth_data);
	    return NULL;
	}
	for(int j = 0; j < cur_osc->num_voices; j++){
	    SYNTH_VOICE* cur_voice = &(cur_osc->osc_voices[j]);
	    cur_voice->vco_amp_L = params_init_interpolated_val(1.0, (unsigned int)(0.002 * synth_data->samplerate));
	    cur_voice->vco_amp_R = params_init_interpolated_val(1.0, (unsigned int)(0.002 * synth_data->samplerate));
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

	cur_osc->params = params_init_param_container(10, (char* [10]){"Amp", "Freq", "Spread", "Wobble", "Octave", "Table", "A", "D", "S", "R"},
						      (PARAM_T [10]){0.8, 0, 0, 0, 0, 0, 0.0, 0.0, 1.0, 0.001},
						      (PARAM_T [10]){0.00001, -12, 0, 0, ((MAX_SEMITONES - 12)/12)*-1, 0, 0.0, 0.0, 0.0, 0.0},
						      (PARAM_T [10]){1, 12, 1, 1, (MAX_SEMITONES - 12)/12, 3, 5.0, 5.0, 1.0, 5.0},
						      (PARAM_T [10]){0.01, 0.1, 0.01, 0.05, 1, 1, 0.1, 0.1, 0.01, 0.1},
						      (unsigned char [10]){DB_Return_Type, Float_type, Float_type, Float_type, Int_type, String_Return_Type,
							  Curve_Float_Return_Type, Curve_Float_Return_Type, Float_type, Curve_Float_Return_Type},
						      NULL, NULL);
	//write strings to parameters that are String_Return_Type
	param_set_param_strings(cur_osc->params, 5, (char* [4]){"sin", "triang", "saw", "sqr"}, 4);
	//put a curve table for the params that should be returned as curves
	param_add_curve_table(cur_osc->params, 6, synth_data->amp_to_exp);
	param_add_curve_table(cur_osc->params, 7, synth_data->amp_to_exp);
	param_add_curve_table(cur_osc->params, 9, synth_data->amp_to_exp);

	synth_activate_backend_ports(synth_data, cur_osc);
    }
    
    return synth_data;   
}

PRM_CONTAIN* synth_param_return_param_container(SYNTH_DATA* synth_data, int osc_id){
    if(!synth_data)return NULL;
    if(osc_id >= MAX_OSCS)return NULL;
    SYNTH_OSC* cur_osc = &(synth_data->osc_array[osc_id]);
    if(!cur_osc->params)return NULL;
    return cur_osc->params;
}

int synth_activate_backend_ports(SYNTH_DATA* synth_data, SYNTH_OSC* osc){
    if(!synth_data)return -1;
    if(!synth_data->audio_backend)return -1;
    if(!osc->ports)return -1;
    for(int i = 0; i < osc->num_ports; i++){
	SYNTH_PORT* cur_port = &(osc->ports[i]);
	cur_port->sys_port = app_jack_create_port_on_client(synth_data->audio_backend, cur_port->port_type,
								cur_port->port_flow, cur_port->port_name);
    }

    return 0;
}

static int synth_process_adsr(SYNTH_ADSR* adsr, unsigned int voice_playing, PARAM_T* ret_amp){
    if(!adsr)return -1;

    PARAM_T a_frames = adsr->a * adsr->samplerate;
    PARAM_T d_frames = adsr->d * adsr->samplerate;
    PARAM_T r_frames = adsr->r * adsr->samplerate;
    
    if(voice_playing == 1){
	adsr->time_frames += 1;
	
	if(adsr->phase == 0){
	    adsr->phase = 1;
	    if(adsr->a == 0.0) adsr->amp = 1.0;
	}
    }
    if(voice_playing == 0 && adsr->phase != 4) {
	adsr->phase = 4;
	adsr->r_amp = adsr->amp;
	adsr->r_frames = 0;
    }
    int ret_val = 0;
    //attack phase
    if(adsr->phase == 1){
	adsr->amp = fit_range(a_frames, 0.0, 1.0, 0.0, (PARAM_T)adsr->time_frames);
	ret_val = 1;
	
	if(adsr->amp >= 1.0 || adsr->time_frames >= a_frames){
	    adsr->phase = 2;
	    adsr->amp = 1.0;
	}	
    }

    if(adsr->phase == 2){
	adsr->amp = fit_range(a_frames + d_frames, a_frames, adsr->s, 1.0, (PARAM_T)adsr->time_frames);
	ret_val = 2;
	
	if(adsr->amp <= adsr->s || adsr->time_frames >= (a_frames + d_frames)){
	    if(adsr->s > 0.0) adsr->phase = 3;
	    if(adsr->s <= 0.0) adsr->phase = 4;
	}
    }

    if(adsr->phase == 3){
	adsr->amp = adsr->s;
	ret_val = 3;
    }

    if(adsr->phase == 4){
	adsr->amp = fit_range(r_frames, 0.0, 0.0, adsr->r_amp, (PARAM_T)adsr->r_frames);
	adsr->r_frames += 1;
	ret_val = 4;
	if(adsr->amp <= 0.0 || adsr->r <= 0.0){
	    adsr->amp = 0.0;
	    adsr->phase = 5;
	    ret_val = 5;
	}
    }
    *ret_amp = adsr->amp;
    return ret_val;
}

static void synth_adsr_reset(SYNTH_ADSR* adsr){
    if(!adsr)return;
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

static void synth_adsr_update(SYNTH_ADSR* adsr, PARAM_T a, PARAM_T d, PARAM_T s, PARAM_T r, PARAM_T samplerate){
    if(!adsr)return;
    adsr->a = a;
    adsr->d = d;
    adsr->s = s;
    adsr->r = r;
    adsr->samplerate = samplerate;
}

static void synth_process_osc_voices(SYNTH_DATA* synth_data, SYNTH_OSC* osc, NFRAMES_T nframes){
    if(!osc)return;
    if(!osc->osc_voices)return;

    memset(osc->buffer_L, '\0', sizeof(SAMPLE_T) * nframes);
    memset(osc->buffer_R, '\0', sizeof(SAMPLE_T) * nframes);
    
    //interpolate the amp value
    PARAM_T amp_in = param_get_value(osc->params, 0, 0, 1, 1);
    PARAM_T freq_in = param_get_value(osc->params, 1, 0, 0, 1);
    PARAM_T octave_in =  param_get_value(osc->params, 4, 0, 0, 1);
    PARAM_T wobble = param_get_value(osc->params, 3, 0, 0, 1);
    PARAM_T spread = param_get_value(osc->params, 2, 0, 0, 1);
    //get the adsr values from the user parameters
    PARAM_T vco_a = param_get_value(osc->params, 6, 1, 0, 1);
    PARAM_T vco_d = param_get_value(osc->params, 7, 1, 0, 1);
    PARAM_T vco_s = param_get_value(osc->params, 8, 0, 0, 1);
    PARAM_T vco_r = param_get_value(osc->params, 9, 1, 0, 1);

    for(int i = 0; i < osc->num_voices; i++){
	SYNTH_VOICE* cur_voice = &(osc->osc_voices[i]);
	if(!cur_voice)continue;
	if(!cur_voice->osc_table)continue;
	
	if(cur_voice->stopped == 1){
	    synth_adsr_reset(cur_voice->vco_adsr);
	    continue;
	}
	OSC_OBJ* osc_table = cur_voice->osc_table;
	//update the adsr values on the voice with the user values
	synth_adsr_update(cur_voice->vco_adsr, vco_a, vco_d, vco_s, vco_r, synth_data->samplerate);
	
	//this voices random value
	srand((i + 1) * cur_voice->rand_seed);
	int rand_val = rand();
	//spread randomness
	PARAM_T spread_am = 0;
	if(spread != 0){
	    spread_am = fit_range((PARAM_T)RAND_MAX, 0.0, spread, spread * -1, (PARAM_T)rand_val);
	}	    
	//wobble frequency randomness
	PARAM_T wobble_freq = 1.5;
	PARAM_T wobble_am = 0.0;
	if(wobble!=0){
	    wobble_freq = fit_range((PARAM_T)RAND_MAX, 0.0, wobble_freq * 0.5, wobble_freq, (PARAM_T)rand_val);
	    wobble_am = fit_range((PARAM_T)RAND_MAX, 0.0, wobble, wobble * 0.5, (PARAM_T)rand_val);
	}
	    
	PARAM_T freq = midi_note_to_freq(cur_voice->midi_note);	
	//linear midi vel to amp
	PARAM_T midi_amp = fit_range(127.0, 0.0, 1.0, 0.0, cur_voice->midi_vel);
	//midi vel to amp with a curve
	midi_amp = math_range_table_convert_value(synth_data->log_curve, midi_amp);
	
	//process the wavetable, get buffer
	for(int j = 0; j < nframes; j++){     
	    //add the octaves and semitones
	    PARAM_T octaves_semitones = octave_in * 12;
	    PARAM_T freq_final = freq * math_range_table_convert_value(synth_data->semi_to_freq_table, octaves_semitones + freq_in);
	    //process the adsr
	    PARAM_T adsr_amp = 1.0;
	    int adsr_phase = synth_process_adsr(cur_voice->vco_adsr, cur_voice->playing, &adsr_amp);
	    //spread will help spread the voices in stereo, by simply making random voices more quite in
	    //left or right side
	    PARAM_T spread_mult_L = 1;
	    PARAM_T spread_mult_R = 1;
	    if(spread!=0){
		if(spread_am < 0) spread_mult_R -= (spread_am * -1);
		if(spread_am > 0) spread_mult_L -= spread_am;
	    }
	    
	    //randomly wobble the voices if the parameter is not 0
	    if(wobble!=0){
		PARAM_T wobble_semitones = osc_getOutput(osc->sin_osc, cur_voice->wobble_ph, wobble_freq, 0, 0) * wobble_am;
		freq_final = freq_final * math_range_table_convert_value(synth_data->semi_to_freq_table, wobble_semitones);	
		osc_updatePhase(osc->sin_osc, &(cur_voice->wobble_ph), wobble_freq);
	    }

	    PARAM_T wave_sample_L = osc_getOutput(osc_table, cur_voice->vco_ph, freq_final, 0, 0);
	    PARAM_T wave_sample_R = wave_sample_L;
	    osc_updatePhase(osc_table, &(cur_voice->vco_ph), freq_final);

	    PARAM_T interp_amp_in_L = params_interp_val_get_value(cur_voice->vco_amp_L, amp_in * adsr_amp * spread_mult_L * midi_amp);
	    PARAM_T interp_amp_in_R = params_interp_val_get_value(cur_voice->vco_amp_R, amp_in * adsr_amp * spread_mult_R * midi_amp);	    
	    
	    osc->buffer_L[j] += wave_sample_L * math_range_table_convert_value(synth_data->amp_to_exp, interp_amp_in_L);
	    osc->buffer_R[j] += wave_sample_R * math_range_table_convert_value(synth_data->amp_to_exp, interp_amp_in_R);

	    if(adsr_phase == 5){
		cur_voice->playing = 0;
	    }
	    //fully stop only when the amplitude is 0 and adsr release phase is finished (adsr_phase == 5)
	    if(adsr_phase == 5 && interp_amp_in_L <= 0 && interp_amp_in_R <= 0){
		cur_voice->stopped = 1;
		synth_adsr_reset(cur_voice->vco_adsr);
		break;
	    }
	}
    }

    //now copy the buffers to the ports
    SYNTH_PORT* l_Port = NULL;
    SYNTH_PORT* r_Port = NULL;
    if(osc->num_ports == 2){
	l_Port = &(osc->ports[0]);
	r_Port = &(osc->ports[1]);
    }
    else{
	l_Port = &(osc->ports[1]);
	r_Port = &(osc->ports[2]);
    }
    if(!l_Port || !r_Port) return;
    
    SAMPLE_T* out_L = app_jack_get_buffer_rt(l_Port->sys_port, nframes);
    SAMPLE_T* out_R = app_jack_get_buffer_rt(r_Port->sys_port, nframes);
    if(!out_L || !out_R) return;
    //zeroe out the outputs
    
    memset(out_L, '\0', sizeof(SAMPLE_T) * nframes);
    memset(out_R, '\0', sizeof(SAMPLE_T) * nframes);

    memcpy(out_L, osc->buffer_L, sizeof(SAMPLE_T) * nframes);
    memcpy(out_R, osc->buffer_R, sizeof(SAMPLE_T) * nframes);
}

//this function finds a voice to play, does not produce any sound though
static void synth_play_osc_rt(SYNTH_OSC* osc, MIDI_DATA_T vel, MIDI_DATA_T note, PARAM_T rand_seed){
    if(!osc)return;   
    //go through the voices and if its stopped, play it
    if(!osc->osc_voices)return;
    unsigned int found_voice = 0;
    unsigned int next_voice = osc->last_voice + 1;
    unsigned int count = 0;
    SYNTH_VOICE* to_play_voice = NULL;
    while(found_voice == 0){
	if(next_voice >= osc->num_voices) next_voice = 0;
	SYNTH_VOICE* cur_voice = &(osc->osc_voices[next_voice]);
	if(!cur_voice)break;
	if(cur_voice->stopped == 1){
	    to_play_voice = cur_voice;
	    //calc a random phase for this voice, so each voice does not start on the same phase
	    srand((unsigned int)(vel * (to_play_voice->id+1)*45999));
	    to_play_voice->vco_ph = fit_range((PARAM_T)RAND_MAX, 0.0, 1.0, 0.0, (PARAM_T)rand());
	    found_voice = 1;
	    break;
	}

	next_voice += 1;
	count +=1;
	//if all voices are playing break and then simply choose the next voice after the last_voice
	//and reset it after this loop at the end of this funciton
	if(count >= osc->num_voices) break;
    }

    if(found_voice == 0){
	//if all voices where busy, find next voice after the last voice played and play it, also reseting it
	next_voice = osc->last_voice + 1;
	if(next_voice >= osc->num_voices) next_voice = 0;
	SYNTH_VOICE* cur_voice = &(osc->osc_voices[next_voice]);
	if(!cur_voice)return;
	to_play_voice = cur_voice;
    }

    //if we found the next voice to play reset it
    if(to_play_voice){
	to_play_voice->playing = 1;
	to_play_voice->stopped = 0;
	to_play_voice->rand_seed = (unsigned int)rand_seed;
	synth_adsr_reset(to_play_voice->vco_adsr);
	to_play_voice->midi_note = note;
	to_play_voice->midi_vel = vel;
	osc->last_voice = to_play_voice->id;
	
	//set which table to play for the voice
	//its set before playing the voice so the table does not change while the sound is playing
	to_play_voice->osc_table = osc->sin_osc;
	PARAM_T table  = param_get_value(osc->params, 5, 0, 0, 1);
	if(table == SIN_WAVETABLE)to_play_voice->osc_table = osc->sin_osc;	
	if(table == TRIANGLE_WAVETABLE)to_play_voice->osc_table = osc->triang_osc;
	if(table == SAW_WAVETABLE)to_play_voice->osc_table = osc->saw_osc;
	if(table == SQUARE_WAVETABLE)to_play_voice->osc_table = osc->sqr_osc;
    }
}
//stop a voice of the osc, that matches the note given
//if stop_all == 1, stop all the voices of the oscillator
static void synth_stop_osc_rt(SYNTH_OSC* osc, MIDI_DATA_T vel, MIDI_DATA_T note, unsigned int stop_all){
    if(!osc)return;
    if(!osc->osc_voices)return;
    //go through voices and stop them all or just the voice that was played with the note
    //dont reset the stopped voices, because they will reset themselfs when adsr goes to 0
    for(int i = 0; i < osc->num_voices; i++){
	SYNTH_VOICE* cur_voice = &(osc->osc_voices[i]);
	if(!cur_voice)continue;
	if(stop_all == 1){
	    cur_voice->playing = 0;
	    continue;
	}
	if(note == cur_voice->midi_note && cur_voice->playing == 1){
	    cur_voice->playing = 0;
	}
    }
}

static int synth_metronome_process_rt(SYNTH_DATA* synth_data, SYNTH_OSC* metro_osc, NFRAMES_T nframes, int32_t beat, int playhead){
    if(!metro_osc)return -1;
    if(playhead == 0){
	synth_stop_osc_rt(metro_osc, 127, 0, 1);
    }
    if(playhead == 1){
	//calculate if we need to trigger the metronome oscillator
	unsigned int play_osc = 0;
	if(metro_osc->trig != beat){
	    metro_osc->trig = beat;
	    play_osc = 1;
	}

	if(play_osc == 1){
	    MIDI_DATA_T pitch = 60;
	    if(beat == 1)pitch = 72;
	    synth_play_osc_rt(metro_osc, 127, pitch, beat);
	}
    }

    //now process the voices of this oscilator
    synth_process_osc_voices(synth_data, metro_osc, nframes);  
    
    return 0;
}

int synth_process_rt(SYNTH_DATA* synth_data, NFRAMES_T nframes){
    if(!synth_data)return -1;
    if(!synth_data->audio_backend)return -1;
    if(!synth_data->osc_array)return -1;
    if(synth_data->num_osc <=0 ) return -1;

    //if there is a metronome process it
    if(synth_data->with_metronome == 1){
	int32_t bar = 1;
	int32_t beat = 1;
	int32_t tick = 0;
	SAMPLE_T ticks_per_beat = 0;
	NFRAMES_T total_frames = 0;
	float bpm = 0;
	float beat_type = 0;
	float beats_per_bar = 0;
	int isPlaying = app_jack_return_transport_rt(synth_data->audio_backend, &bar, &beat, &tick, &ticks_per_beat, &total_frames,
						     &bpm, &beat_type, &beats_per_bar);
	if(isPlaying != -1){
	    SYNTH_OSC* cur_osc = &(synth_data->osc_array[0]);
	    if(!cur_osc)return -1;
	    synth_metronome_process_rt(synth_data, cur_osc, nframes, beat, isPlaying);
	}
    }

    //here we process all the oscillators except the metronome, if there is a metronome
    int i = 0;
    if(synth_data->with_metronome == 1) i = 1;
    for(i; i < synth_data->num_osc; i++){
	if(i >= synth_data->num_osc) continue;
	SYNTH_OSC* cur_osc = &(synth_data->osc_array[i]);
	//get the notes to the midi container
	app_jack_midi_cont_reset(synth_data->midi_cont);
	SYNTH_PORT* midi_port = &(cur_osc->ports[0]);
	void* midi_buffer = app_jack_get_buffer_rt(midi_port->sys_port, nframes);
	app_jack_return_notes_vels_rt(midi_buffer, synth_data->midi_cont);
	for(int cur_frame = 0; cur_frame < nframes; cur_frame++){
	    MIDI_DATA_T this_vel = 0;
	    MIDI_DATA_T this_type = 0;
	    MIDI_DATA_T this_pitch = 0;	    
	    for(int i = 0; i < synth_data->midi_cont->num_events; i++){
		if(synth_data->midi_cont->nframe_nums[i] != cur_frame)continue;
		this_vel = synth_data->midi_cont->vel_trig[i];
		this_type = synth_data->midi_cont->types[i];
		this_pitch = synth_data->midi_cont->note_pitches[i];
	    }
	    //note on event
	    if((this_type & 0xf0) == 0x90){
		synth_play_osc_rt(cur_osc, this_vel, this_pitch, (cur_frame + this_vel + (i*988)));
	    }
	    //note off event
	    if((this_type & 0xf0) == 0x80){
		synth_stop_osc_rt(cur_osc, this_vel, this_pitch, 0);
	    }
	}

	//now process the voices of this oscilator
	//TODO this function could return the highest value or average added to the buffer
	//Then could have a parameter that is readable only and update here and in app_data send it to rt_to_ui ring buffer
	//(just go through just changed rt parameters at the end of the rt thread and write to rt_to_ui thread)
	//This way could for example show the user what are the oscillator levels.
	synth_process_osc_voices(synth_data, cur_osc, nframes);    
    }

    return 0;
}

PRM_CONTAIN* synth_return_param_container(SYNTH_DATA* synth_data, unsigned int osc_num){
    if(!synth_data)return NULL;
    if(osc_num >= synth_data->num_osc)return NULL;
    SYNTH_OSC* cur_osc = &(synth_data->osc_array[osc_num]);
    if(!cur_osc)return NULL;
    return cur_osc->params;
}

const char* synth_return_osc_name(SYNTH_DATA* synth_data, unsigned int osc_num){
    if(!synth_data)return NULL;
    if(osc_num >= synth_data->num_osc)return NULL;
    SYNTH_OSC* cur_osc = &(synth_data->osc_array[osc_num]);
    if(!cur_osc)return NULL;
    return cur_osc->name;
}

int synth_return_osc_num(SYNTH_DATA* synth_data){
    if(!synth_data)return -1;
    return synth_data->num_osc;
}

static int synth_clean_ports(SYNTH_DATA* synth_data, SYNTH_PORT** osc_ports, unsigned int num_ports){
    if(!synth_data)return -1;
    if(!osc_ports)return -1;
    for(int i = 0; i < num_ports; i++){
	SYNTH_PORT* ports = *osc_ports;
	SYNTH_PORT* cur_port = &(ports[i]);
	if(cur_port->port_name)free(cur_port->port_name);
	if(cur_port->sys_port){
	    app_jack_unregister_port(synth_data->audio_backend, cur_port->sys_port);
	}
    }
    free(*osc_ports);
}

static int synth_clean_osc(SYNTH_DATA* synth_data, SYNTH_OSC* synth_osc){
    if(!synth_osc)return -1;
    if(synth_osc->params)param_clean_param_container(synth_osc->params);
    synth_osc->params = NULL;
    if(synth_osc->osc_voices){
	for(int i = 0; i < synth_osc->num_voices; i++){
	    SYNTH_VOICE* cur_voice = &(synth_osc->osc_voices[i]);
	    if(cur_voice){
		if(cur_voice->vco_amp_L)free(cur_voice->vco_amp_L);
		if(cur_voice->vco_amp_R)free(cur_voice->vco_amp_R);
		if(cur_voice->vco_adsr)free(cur_voice->vco_adsr);
	    }
	}
	free(synth_osc->osc_voices);
	synth_osc->osc_voices = NULL;
    }
    if(synth_osc->buffer_L)free(synth_osc->buffer_L);
    synth_osc->buffer_L = NULL;
    if(synth_osc->buffer_R)free(synth_osc->buffer_R);
    synth_osc->buffer_R = NULL;
    if(synth_osc->ports)synth_clean_ports(synth_data, &(synth_osc->ports), synth_osc->num_ports);
    synth_osc->ports = NULL;
    if(synth_osc->name)free(synth_osc->name);
    synth_osc->name = NULL;
}

int synth_clean_memory(SYNTH_DATA* synth_data){
    if(!synth_data)return -1;
    if(synth_data->midi_cont){
	app_jack_clean_midi_cont(synth_data->midi_cont);
	free(synth_data->midi_cont);
    }
    if(synth_data->osc_array){
	for(int i = 0; i < synth_data->num_osc; i++){
	    synth_clean_osc(synth_data, &(synth_data->osc_array[i]));
	}
	free(synth_data->osc_array);
    }
    if(synth_data->triang_osc)osc_clean_osc_wavetable(synth_data->triang_osc);
    if(synth_data->saw_osc)osc_clean_osc_wavetable(synth_data->saw_osc);
    if(synth_data->sqr_osc)osc_clean_osc_wavetable(synth_data->sqr_osc);
    if(synth_data->sin_osc)osc_clean_osc_wavetable(synth_data->sin_osc);
    if(synth_data->semi_to_freq_table)math_range_table_clean(synth_data->semi_to_freq_table);
    if(synth_data->log_curve)math_range_table_clean(synth_data->log_curve);
    if(synth_data->amp_to_exp)math_range_table_clean(synth_data->amp_to_exp);

    context_sub_clean(synth_data->control_data);
    
    free(synth_data);

    return 0;
}
