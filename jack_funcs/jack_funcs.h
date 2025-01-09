#pragma once
#include <jack/jack.h>
#include <jack/midiport.h>
#include "../structs.h"
#include "../contexts/params.h"
//struct to keep midi events info
//its in the definition because its more convenient to access the members of this struct
typedef struct _jack_midi_cont{
    //buffer to store the note pitches
    MIDI_DATA_T* note_pitches;
    //buffer to store the velocities that triggered the note
    MIDI_DATA_T* vel_trig;
    //the types of the midi events
    MIDI_DATA_T* types;
    //when the events happened
    NFRAMES_T* nframe_nums;
    //number of bytes of data in the midi event buffer (that holds the note pitches etc.)
    size_t* buf_size;
    //the number of events that occured
    NFRAMES_T num_events;
    //the total size of each of the arrays on this struct
    unsigned int array_size;
}JACK_MIDI_CONT;

//jack main struct
typedef struct _jack_info JACK_INFO;

//function that initializes the client
JACK_INFO* jack_initialize(void *arg, const char *client_name,
			   int ports_num, unsigned int* ports_types,
			   unsigned int *io_types, const char** ports_names,
                           int(*process)(jack_nframes_t, void*), unsigned int create_ports);
//read messages from ui on [audio-thread]
//update parameters, pause jack process
int app_jack_read_ui_to_rt_messages(JACK_INFO* jack_data);
//read messages from rt on [main-thread]
//update parameters, log messages from rt thread
int app_jack_read_rt_to_ui_messages(JACK_INFO* jack_data);
//set parameter value on [main-thread] and send a message to set the parameter on [audio-thread] too
//only use on [main-thread]
int app_jack_param_set_value(JACK_INFO* jack_data, int param_id, float param_val, unsigned char param_op);
//get how much the parameter changes, use only on [main-thread]
SAMPLE_T app_jack_param_get_increment(JACK_INFO* jack_data, int param_id);
//get the value of the parameter, use on [main-thread]
//val_type will return what type of value the param holds, returns 0 if there was an error getting the value
SAMPLE_T app_jack_param_get_value(JACK_INFO* jack_data, unsigned char* val_type, unsigned int curved, int param_id);
//get the id of the parameter from its name, use on [main-thread]
int app_jack_param_id_from_name(JACK_INFO* jack_data, const char* param_name);
//get the string from parameter if the value is a list of strings, use on [main-thread]
const char* app_jack_param_get_string(JACK_INFO* jack_data, int param_id);
//how many parameters there are, use on [main-thread]
int app_jack_param_get_num_of_params(JACK_INFO* jack_data);
//get the name of the parameter, use on [main-thread]
const char* app_jack_param_get_name(JACK_INFO* jack_data, int param_id);
//clean the midi container
void app_jack_clean_midi_cont(JACK_MIDI_CONT* midi_cont);
//initate the midi container where velocities, note pitches etc will be stored
JACK_MIDI_CONT* app_jack_init_midi_cont(unsigned int array_size);
//reset all the arrays of the midi container to 0
void app_jack_midi_cont_reset(JACK_MIDI_CONT* midi_cont);
//initializes the ports
static int init_jack_ports(JACK_INFO *jack_data, int ports_num, unsigned int* ports_types,
		    unsigned int *io_types,
		    const char** ports_names);
//register ports on a jack client if its known to the data
void* app_jack_create_port_on_client(void* client_in, unsigned int port_type, unsigned int io_type,
					    const char* port_name);
//return the smaple rate of a jack client (of the server really)
float app_jack_return_samplerate(JACK_INFO* jack_data);
//return the buffer size
int app_jack_return_buffer_size(JACK_INFO* jack_data);
//activate the jack client
int app_jack_activate(JACK_INFO *jack_data);
//return buffer from a jack port name
void* app_jack_get_buffer_from_name(JACK_INFO *jack_data, jack_nframes_t nframes,
				       const char* name, unsigned int* event_num);
//get buffer from a jack port 
void* app_jack_get_buffer_rt(void* port, jack_nframes_t nframes);
//clear the buffer of midi out buffer
void app_jack_midi_clear_buffer_rt(void* buffer);
//write to the midi out buffer
int app_jack_midi_events_write_rt(void* buffer, jack_nframes_t time, const jack_midi_data_t* data,
				  size_t data_size);
//return three arrays for the midi_in, notes played, velocities and the times for each in nframe
void app_jack_return_notes_vels_rt(void* midi_in, JACK_MIDI_CONT* midi_cont);
//disconnect ports belonging to this client
int app_jack_disconnect_all_ports(JACK_INFO* jack_data, unsigned int type_pattern, unsigned long flags);
//check if port with the port_name exists on the client
int app_jack_is_port(JACK_INFO* jack_data, const char* port_name);
//disconnect two ports
int app_jack_disconnect_ports(JACK_INFO* jack_data, const char* source_port, const char* dest_port);
//connect two ports together
int app_jack_connect_ports(JACK_INFO* jack_data, const char* source_port, const char* dest_port);
//return the name of a single port
const char* app_jack_return_port_name(void* port);
//return a list of jack port names
const char** app_jack_port_names(JACK_INFO *jack_data, const char* port_name_pattern,
				 unsigned int type_pattern,
				 unsigned long flags);
//function callback when server changes the sample_rate
int sample_rate_change(jack_nframes_t new_sample_rate, void *arg);
//unregister port from client
void app_jack_unregister_port(void* client, void* port);
//clean the memory of the jack_data
void jack_clean_memory(void* jack_data);
//callback to update the *pos struct that holds bar, beat, tick, etc information. Realtime function, cant wait!
void timebbt_callback_rt(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t *pos,
			 int new_pos, void *arg);
//check if any parameters that control the transport head have changed, if yes - request an update with the new
//parameters to the transport head
void app_jack_update_transport_from_params_rt(JACK_INFO* jack_data);
//return the jack transport position info to the various variables
int app_jack_return_transport_rt(void* audio_client, int32_t* cur_bar, int32_t* cur_beat,
				 int32_t* cur_tick, SAMPLE_T* ticks_per_beat, jack_nframes_t* total_frames,
				 float* bmp, float* beat_type, float* beats_per_bar);
