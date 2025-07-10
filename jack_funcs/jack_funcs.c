#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <threads.h>
#include <stdbool.h>
//my libraries
#include "jack_funcs.h"
#include "../types.h"
#include "../util_funcs/log_funcs.h"
#include "../contexts/context_control.h"
//the maximum number of bars there can be
#define MAX_BARS 1000

static thread_local bool is_audio_thread = false;

//jack main struct
typedef struct _jack_info{
    int port_size;
    //the jack client 
    jack_client_t *client;
    //the current server sample_rate
    jack_nframes_t sample_rate;
    //the current server buffer size
    jack_nframes_t buffer_size;
    //parameters for the general song track settings, like current bar, beat
    //also play, stop etc.
    PRM_CONTAIN* trk_params;
    //rt tick var, that goes from 0 to RT_CYCLES, rt thread will send info to ui thread only when rt tick var is 0
    //should only be touched on [audio-thread]
    int rt_tick;
    //control_data for sys messages, for jack currently only uses the [thread-safe] messaging system, since this does not have any subcontexts
    CXCONTROL* control_data;
}JACK_INFO;
//ticks per beat, since user should not set these anyway
double time_ticks_per_beat = 1920.0;
static int app_jack_sys_send_msg(void* user_data, const char* msg){
    //TODO for future proof, now is not used
    JACK_INFO* jack_data = (JACK_INFO*)user_data;
    log_append_logfile("%s", msg);
    return 0;
}

JACK_INFO* jack_initialize(void *arg, const char *client_name,
                           int(*process)(jack_nframes_t, void*)){
    
    JACK_INFO *jack_data = (JACK_INFO*)malloc(sizeof(JACK_INFO));
    if(!jack_data){
        return NULL;
    }
    jack_data->rt_tick = 0;
    jack_data->control_data = NULL;

    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    ui_funcs_struct.send_msg = app_jack_sys_send_msg;
    jack_data->control_data = context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if(!jack_data->control_data){
	jack_clean_memory(jack_data);
	return NULL;
    }

    jack_status_t *server_name = NULL;
    jack_options_t options = JackNullOption;
    jack_status_t status = 0;

    //initialize the general song trk parameters like current bar, beat, play etc.
    jack_data->trk_params = params_init_param_container(7, (char*[7]){"Tempo", "Bar", "Beat", "Tick", "Play", "BPB", "Beat_Type"},
							(PARAM_T[7]){100, 1, 1, 0, 0, 4, 4},
							(PARAM_T[7]){10, 1, 1, 0, 0, 2, 2}, (PARAM_T[7]){500, MAX_BARS, 16, 2000, 1, 16, 16},
							(PARAM_T[7]){1, 1, 1, floor(time_ticks_per_beat/4), 1, 1, 1},
							(unsigned char[7]){Int_type, Int_type, Int_type, Int_type, Int_type, Int_type, Int_type},
							NULL, NULL);
    if(!jack_data->trk_params){
	jack_clean_memory(jack_data);
	return NULL;
    }
    
    //open client connection
    jack_data->client = jack_client_open(client_name, options &status, server_name);
    if(jack_data->client==NULL){
        free(jack_data);
        return NULL; 
    }

    //what the process function is
    jack_set_process_callback(jack_data->client, process, arg);
    
    //call this function when the engine sample rate changes
    jack_set_sample_rate_callback(jack_data->client, sample_rate_change, jack_data);
    
    //set the callback function that updates the *pos struct that holds beat, bar, tick etc information
    jack_set_timebase_callback(jack_data->client, 0, timebbt_callback_rt, jack_data);
    
    /*write some jack client attributes to the jack_data struct*/
    //sample rate of the server
    jack_data->sample_rate = jack_get_sample_rate(jack_data->client);
    jack_data->buffer_size = jack_get_buffer_size(jack_data->client);
    
    
    return jack_data;
}

int app_jack_read_ui_to_rt_messages(JACK_INFO* jack_data){
    //set the is_audio_thread to true so its false on [main-thread] and true on [audio-thread]
    is_audio_thread = true;

    if(!jack_data)return -1;
    //update the rt_tick
    jack_data->rt_tick +=1;
    if(jack_data->rt_tick > RT_CYCLES)jack_data->rt_tick = 0;

    context_sub_process_rt(jack_data->control_data);
    
    //read the param ui_to_rt messages and set the parameter values
    param_msgs_process(jack_data->trk_params, 1);

    //if rt params just got new parameter values from the ui they will be just changed
    //in that case jack will create a new transport object and request a transport change
    app_jack_update_transport_from_params_rt(jack_data);
    //now update the bar, beat, tick and is playing parameters from the jack transport for [audio-thread] and [main-thread]
    //update the params on [audio-thread] in rt_tick (as slow as ui [main-thread] params) since the params will only be used to set anything if the user changes some of them on the [main-thread]
    //all other contexts gets the real time beats, ticks and other transport vars from the jack_transport_query function in the app_jack_return_transport_rt function
    if(jack_data->rt_tick == 0){
	jack_position_t pos;
	jack_transport_state_t state = jack_transport_query(jack_data->client, &pos);
	unsigned int pos_ready = 1;
	if(!pos.valid)pos_ready = 0;
	if(pos.valid != JackPositionBBT)pos_ready = 0;
	if(pos_ready == 1){
	    if(state == JackTransportRolling){
		//After setting the value, get the value so the parameter is_changed will be 0 and app_jack_update_transport_from_params_rt will not create a new tranport object
		//even though a parameter was not changed by the ui
		//get the bars
		param_set_value(jack_data->trk_params, 1, (float)pos.bar, NULL, Operation_SetValue, 1);
	        param_get_value(jack_data->trk_params, 1, 0, 0, 1);
		//get the beat
		param_set_value(jack_data->trk_params, 2, (float)pos.beat, NULL, Operation_SetValue, 1);
		param_get_value(jack_data->trk_params, 2, 0, 0, 1);
		//get the tick
		param_set_value(jack_data->trk_params, 3, (float)pos.tick, NULL, Operation_SetValue, 1);
		param_get_value(jack_data->trk_params, 3, 0, 0, 1);
	    }
	    //get the isPlaying state even if the Jack transport head is not rolling
	    param_set_value(jack_data->trk_params, 4, (float)state, NULL, Operation_SetValue, 1);
	    param_get_value(jack_data->trk_params, 4, 0, 0, 1);
	}
    }
    return 0;
}

int app_jack_read_rt_to_ui_messages(JACK_INFO* jack_data){
    if(!jack_data)return -1;

    context_sub_process_ui(jack_data->control_data);
    
    //read the param rt_to_ui messages and set the parameter values
    param_msgs_process(jack_data->trk_params, 0);
    
    return 0;    
}

PRM_CONTAIN* app_jack_param_return_param_container(JACK_INFO* jack_data){
    if(!jack_data)return NULL;
    if(!jack_data->trk_params)return NULL;
    return jack_data->trk_params;
}

void app_jack_clean_midi_cont(JACK_MIDI_CONT* midi_cont){
    if(midi_cont->buf_size)free(midi_cont->buf_size);
    if(midi_cont->nframe_nums)free(midi_cont->nframe_nums);
    if(midi_cont->note_pitches)free(midi_cont->note_pitches);
    if(midi_cont->types)free(midi_cont->types);
    if(midi_cont->vel_trig)free(midi_cont->vel_trig);
}

JACK_MIDI_CONT* app_jack_init_midi_cont(unsigned int array_size){
    JACK_MIDI_CONT* ret_midi_cont = NULL;
    ret_midi_cont = malloc(sizeof(JACK_MIDI_CONT));
    if(!ret_midi_cont)return NULL;
    ret_midi_cont->array_size = array_size;
    ret_midi_cont->num_events = 0;
    ret_midi_cont->w_pos = 0;
    ret_midi_cont->buf_size = NULL;
    ret_midi_cont->nframe_nums = NULL;
    ret_midi_cont->note_pitches = NULL;
    ret_midi_cont->types = NULL;
    ret_midi_cont->vel_trig = NULL;
 
    ret_midi_cont->buf_size = calloc(array_size, sizeof(size_t));
    if(!ret_midi_cont->buf_size){
	app_jack_clean_midi_cont(ret_midi_cont);
	free(ret_midi_cont);
	return NULL;
    }
    ret_midi_cont->nframe_nums = calloc(array_size, sizeof(NFRAMES_T));
    if(!ret_midi_cont->nframe_nums){
	app_jack_clean_midi_cont(ret_midi_cont);
	free(ret_midi_cont);
	return NULL;
    }    
    ret_midi_cont->note_pitches = calloc(array_size, sizeof(MIDI_DATA_T));
    if(!ret_midi_cont->note_pitches){
	app_jack_clean_midi_cont(ret_midi_cont);
	free(ret_midi_cont);
	return NULL;
    }    
    ret_midi_cont->types = calloc(array_size, sizeof(MIDI_DATA_T));
    if(!ret_midi_cont->types){
	app_jack_clean_midi_cont(ret_midi_cont);
	free(ret_midi_cont);
	return NULL;
    }
    ret_midi_cont->vel_trig = calloc(array_size, sizeof(MIDI_DATA_T));
    if(!ret_midi_cont->vel_trig){
	app_jack_clean_midi_cont(ret_midi_cont);
	free(ret_midi_cont);
	return NULL;
    }
    return ret_midi_cont;
}

void app_jack_midi_cont_reset(JACK_MIDI_CONT* midi_cont){
    if(!midi_cont)return;
    unsigned int size = midi_cont->array_size;
    if(size<=0)return;
    midi_cont->num_events = 0;
    midi_cont->w_pos = 0;
    memset(midi_cont->buf_size, 0, size * sizeof(size_t));
    memset(midi_cont->nframe_nums, 0, size * sizeof(NFRAMES_T));
    memset(midi_cont->note_pitches, 0, size * sizeof(MIDI_DATA_T));
    memset(midi_cont->types, 0, size * sizeof(MIDI_DATA_T));
    memset(midi_cont->vel_trig, 0, size * sizeof(MIDI_DATA_T));
}

int app_jack_port_rename(void* client_in, void* port, const char* new_port_name){
    if(!new_port_name)return -1;
    JACK_INFO* jack_data = (JACK_INFO*)client_in;
    if(!jack_data)return -1;
    jack_port_t* jack_port = (jack_port_t*)port;
    if(!jack_port)return -1;
    return jack_port_rename(jack_data->client, jack_port, new_port_name);
}

void* app_jack_create_port_on_client(void* client_in, unsigned int port_type, unsigned int io_type,
					    const char* port_name){
    JACK_INFO* jack_data = (JACK_INFO*)client_in;
    if(!jack_data)return NULL;
    jack_client_t* client = jack_data->client;
    if(!client)return NULL;
    const char* type = NULL;
    switch(port_type){
    case PORT_TYPE_AUDIO:
	type = JACK_DEFAULT_AUDIO_TYPE;
	break;
    case PORT_TYPE_MIDI:
	type = JACK_DEFAULT_MIDI_TYPE;
	break;
    default:
	type = JACK_DEFAULT_AUDIO_TYPE;
    }
    
    void* ret_port = jack_port_register(client, port_name, type, io_type, 0);
    if(!ret_port)return NULL;
    return ret_port;
}

float app_jack_return_samplerate(JACK_INFO* jack_data){
    if(!jack_data)return -1;
    if(!jack_data->client)return -1;
    return (float)jack_get_sample_rate(jack_data->client);
}
int app_jack_return_buffer_size(JACK_INFO* jack_data){
    if(!jack_data)return -1;
    if(!jack_data->client)return -1;
    return jack_get_buffer_size(jack_data->client);
}

int app_jack_activate(JACK_INFO* jack_data){
    //activate the client and launch the process function
    if(jack_activate (jack_data->client)){
        return -1;
    }
    return 0;
}

int app_jack_port_name_size(){
    return jack_port_name_size();
}

const char* app_jack_return_port_name(void* port){
    if(!port)return NULL;
    const jack_port_t* cur_port = (jack_port_t*)port;
    return jack_port_name(cur_port);
}

void* app_jack_get_buffer_rt(void* port, jack_nframes_t nframes){ 
    jack_port_t* cur_port = (jack_port_t*)port;
    if(!cur_port)return NULL;
    void* out = NULL;
    out = jack_port_get_buffer(cur_port, nframes);
    return out;
}

void app_jack_midi_clear_buffer_rt(void* buffer){    
    if(buffer){
	jack_midi_clear_buffer(buffer);
    }
}

int app_jack_midi_events_write_rt(void* buffer, jack_nframes_t time, const jack_midi_data_t* data,
				  size_t data_size){  
    int return_val = -1;
    if(buffer){
	return_val = jack_midi_event_write(buffer, time, data, data_size);
    }
    return return_val;
}

void app_jack_return_notes_vels_rt(void* midi_in, JACK_MIDI_CONT* midi_cont){  
    if(midi_in == NULL)return;
    int event_count = 0;
    event_count = jack_midi_get_event_count(midi_in);
    if(event_count<=0)return;
    midi_cont->num_events = event_count;
    if((midi_cont->num_events + midi_cont->w_pos) > midi_cont->array_size)midi_cont->num_events = (midi_cont->array_size - midi_cont->w_pos);
    //go through all the midi events
    for(int en = 0; en<midi_cont->num_events; en++){	
	jack_midi_event_t in_event;	
	if(jack_midi_event_get(&in_event, midi_in, en)!=0)return;
	
	//if the note is correct set the vel_trig array of this note index to the
	//midi event velocity
	if(midi_cont->vel_trig!=NULL)
	    midi_cont->vel_trig[midi_cont->w_pos] = in_event.buffer[2];
	if(midi_cont->note_pitches!=NULL)
	    midi_cont->note_pitches[midi_cont->w_pos] = in_event.buffer[1];
	if(midi_cont->nframe_nums!=NULL)
	    midi_cont->nframe_nums[midi_cont->w_pos] = in_event.time;
	if(midi_cont->types!=NULL)
	    midi_cont->types[midi_cont->w_pos] = in_event.buffer[0];
	if(midi_cont->buf_size!=NULL)
	    midi_cont->buf_size[midi_cont->w_pos] = in_event.size;
	midi_cont->w_pos += 1;
    }
}

int app_jack_disconnect_all_ports(JACK_INFO* jack_data, unsigned int type_pattern, unsigned long flags){
    const char** ports = app_jack_port_names(jack_data, jack_get_client_name(jack_data->client),
					type_pattern, flags);
    if(!ports)return -1;
    if(!ports[0])return -1;
    int return_val = 0;
    const char* port = ports[0];
    unsigned int iter = 0;
    while(port){
	jack_port_t* port_A = jack_port_by_name(jack_data->client, port);	
	const char** connect_ports = jack_port_get_connections(port_A);
	if(!connect_ports)goto next;
	const char* con_port = connect_ports[0];
	unsigned i = 0;
	while(con_port){
	    return_val = jack_disconnect(jack_data->client, port, con_port);
	    i+=1;
	    con_port = connect_ports[i];
	}
    next:
	if(connect_ports)free(connect_ports);
	iter+=1;
	port = ports[iter];
    }
    free(ports);
    return return_val;
}

int app_jack_is_port(JACK_INFO* jack_data, const char* port_name){
    if(!jack_data)return -1;
    if(!jack_data->client)return -1;
    jack_port_t* port = jack_port_by_name(jack_data->client, port_name);
    if(port == NULL)return 0;

    return 1;
}

int app_jack_disconnect_ports(JACK_INFO* jack_data, const char* source_port, const char* dest_port){
    if(!jack_data)return -1;
    if(!jack_data->client)return -1;
    return jack_disconnect(jack_data->client, source_port, dest_port);
}

int app_jack_connect_ports(JACK_INFO* jack_data, const char* source_port, const char* dest_port){
    if(!jack_data)return -1;
    if(!jack_data->client)return -1;
    return jack_connect(jack_data->client, source_port, dest_port);
}

const char** app_jack_port_names(JACK_INFO *jack_data, const char* port_name_pattern,
				 unsigned int type_pattern,
				 unsigned long flags){

    const char* type = NULL;
    switch(type_pattern){
    case PORT_TYPE_AUDIO:
	type = JACK_DEFAULT_AUDIO_TYPE;
	break;
    case PORT_TYPE_MIDI:
	type = JACK_DEFAULT_MIDI_TYPE;
	break;
    default:
	type = JACK_DEFAULT_AUDIO_TYPE;
    }
    
    return jack_get_ports(jack_data->client, port_name_pattern, type, flags);
}



int sample_rate_change(jack_nframes_t new_sample_rate, void *arg){
    JACK_INFO *jack_data = (JACK_INFO*) arg;
    if(!jack_data)return -1;
    //TODO this should be [thread-safe] and not a simple var change
    //set the new sample rate on the app data struct so other functions can use that info
    jack_data->sample_rate = new_sample_rate;

    return 0;
}

void app_jack_unregister_port(void* client_in, void* port){
    if(!client_in)return;
    if(!port)return;
    JACK_INFO* jack_data = (JACK_INFO*)client_in;
    jack_client_t* client = jack_data->client;
    jack_port_unregister(client, port);
}

void jack_clean_memory(void* jack_data_in){
    JACK_INFO* jack_data = (JACK_INFO*)jack_data_in;
    if(!jack_data)return;   
    //close the jack client
    if(jack_data->client!=NULL)jack_client_close(jack_data->client);
    //clean the general parameters
    if(jack_data->trk_params)param_clean_param_container(jack_data->trk_params);

    context_sub_clean(jack_data->control_data);
    free(jack_data);
}

static int app_jack_transport(JACK_INFO* jack_data, unsigned int transport_type){
    int ret_int = -1;
    if(!jack_data)return -1;
    if(transport_type==0){
	jack_transport_stop(jack_data->client);
	return 0;
    }
    if(transport_type==1){
	jack_transport_start(jack_data->client);
	return 0;
    }

    return ret_int;
}

void timebbt_callback_rt(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t *pos,
	      int new_pos, void *arg){ 
    //the struct will be used to get a struct from the circle buffer for the time_beats_per_bar and such
    JACK_INFO* jack_data = (JACK_INFO*) arg;
    if(!jack_data)return;
    //minutes since frame 0
    double min;
    //ticks since frame 0
    long abs_tick;
    //beats since frame 0
    long abs_beat;
    //bars since frame 0, with remainder
    double abs_bars;
    //only calculate the beats and bars from frames on the first run of this function
    if(new_pos && pos->valid != JackTransportBBT){
	PRM_CONTAIN* transport_cntr = jack_data->trk_params;
	pos->valid = JackPositionBBT;
	pos->beats_per_bar = 4;
	pos->beats_per_bar = param_get_value(transport_cntr, 5, 0, 0, 1);
	pos->beat_type = 4;
	pos->beat_type = param_get_value(transport_cntr, 6, 0, 0, 1);
	
	pos->ticks_per_beat = time_ticks_per_beat;
	//get the bpm
	pos->beats_per_minute = 120;
	pos->beats_per_minute = param_get_value(transport_cntr, 0, 0, 0, 1);

	//Compute the *pos members from frame
	min = pos->frame / ((double) pos->frame_rate * 60.0);
	abs_tick = min * pos->beats_per_minute * pos->ticks_per_beat; 
	abs_beat = abs_tick / pos->ticks_per_beat;

	pos->bar = abs_beat / pos->beats_per_bar;
	pos->beat = abs_beat - (pos->bar * pos->beats_per_bar) + 1;
	pos->tick = abs_tick - (abs_beat * pos->ticks_per_beat);
	pos->bar_start_tick = pos->bar * pos->beats_per_bar * pos->ticks_per_beat;
	pos->bar++;

    }
    else
       {
	   //if new pos is not requested add the ticks each cycle
	   //TODO now tick drifts so bars and beats are calculated not from ticks, need to figure out how to
	   //calculate from ticks and not drift
	   if(!new_pos){
	       pos->tick += (nframes * pos->ticks_per_beat * pos->beats_per_minute / (pos->frame_rate * 60));
	   }
	   double bpm_sec = (double)pos->beats_per_minute / 60.0;
	   double secs = (double)pos->frame / (double)pos->frame_rate;
	   double beats = bpm_sec * secs;
	   double bar = beats/(double)pos->beats_per_bar;
	   pos->bar = (unsigned int)bar;
	   //pos->bar += 1;
	   double beat_frac = bar - (unsigned int)bar;
	   double beat_div = 1.0 / (double)pos->beats_per_bar;
	   pos->beat = (unsigned int)(beat_frac / beat_div);
	   pos->beat += 1;
	   if(pos->beat == 1){
	       pos->bar_start_tick += (pos->beats_per_bar * pos->ticks_per_beat);
	   }
	   while(pos->tick >= pos->ticks_per_beat){
	       pos->tick -= pos->ticks_per_beat;
	       /*
	       if(++pos->beat > pos->beats_per_bar){
		   pos->beat = 1;
		   ++pos->bar;
		   pos->bar_start_tick += (pos->beats_per_bar * pos->ticks_per_beat);
	       }
	       */
	   }

       }

    //if the bar exceed the max number of bars available in song stop
    if(pos->bar > MAX_BARS){
	pos->bar = MAX_BARS;
	pos->tick = 0;
	pos->beat = 1;
	app_jack_transport(jack_data, 0);
    }
}

void app_jack_update_transport_from_params_rt(JACK_INFO* jack_data){   
    if(!jack_data)return;

    PRM_CONTAIN* transport_cntr = jack_data->trk_params;
    if(!transport_cntr)return;

    //check if any parameters have changed
    int params_changed = param_get_if_any_changed(transport_cntr, 1);
    if(params_changed != 1) return;

    //get the current pos of transport
    jack_position_t old_pos;
    jack_transport_query(jack_data->client, &old_pos);
    
    //create the new pos
    jack_position_t new_pos;
    new_pos.valid = JackPositionBBT;
    new_pos.frame_rate = old_pos.frame_rate;
    
    new_pos.beats_per_minute = param_get_value(transport_cntr, 0, 0, 0, 1);
    new_pos.beats_per_bar = param_get_value(transport_cntr, 5, 0, 0, 1);
    
    new_pos.bar = param_get_value(transport_cntr, 1, 0, 0, 1);
    new_pos.beat = param_get_value(transport_cntr, 2, 0, 0, 1);
    if(new_pos.beat>new_pos.beats_per_bar){
	new_pos.beat = 1;
	new_pos.bar +=1;
    }
    
    new_pos.tick = param_get_value(transport_cntr, 3, 0, 0, 1);
        
    new_pos.beat_type = param_get_value(transport_cntr, 6, 0, 0, 1);
    new_pos.ticks_per_beat = time_ticks_per_beat;

    //first calculate how many beats from frame 0, with remainder
    double seconds_passed = (((new_pos.bar) * new_pos.beats_per_bar) + (new_pos.beat - 1)) +
	(new_pos.tick/new_pos.ticks_per_beat);
    //now calculate how many seconds has passed since frame 0
    seconds_passed = (seconds_passed / new_pos.beats_per_minute) * 60;
    //now calculate the new frames since frame 0
    jack_nframes_t frame = ceil(seconds_passed * old_pos.frame_rate);
    new_pos.frame = frame;

    float play = param_get_value(transport_cntr, 4, 0, 0, 1);
    app_jack_transport(jack_data, (int)play);
    
    jack_transport_reposition(jack_data->client, &new_pos);
}

int app_jack_return_transport_rt(void* audio_client, int32_t* cur_bar, int32_t* cur_beat,
			      int32_t* cur_tick, SAMPLE_T* ticks_per_beat, jack_nframes_t* total_frames,
			      float* bpm, float* beat_type, float* beats_per_bar){
    if(!audio_client)return -1;
    JACK_INFO* jack_data = (JACK_INFO*)audio_client;
    if(!jack_data)return -1;
    
    int isPlaying = 0;
    
    jack_position_t pos;
    jack_transport_state_t state = jack_transport_query(jack_data->client, &pos);
    if(state != JackTransportStopped)isPlaying = 1;

    if(pos.valid) *total_frames = pos.frame;
    
    if(pos.valid != JackPositionBBT)return -1;
    *bpm = pos.beats_per_minute;
    *beat_type = pos.beat_type;
    *beats_per_bar = pos.beats_per_bar;
    *cur_bar = pos.bar;
    *cur_beat = pos.beat;
    *cur_tick = pos.tick;
    *ticks_per_beat = (SAMPLE_T)pos.ticks_per_beat;
    
    return isPlaying;
}
