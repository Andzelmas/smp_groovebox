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
//return the jack transport position info to the various variables
int app_jack_return_transport(void* audio_client, int32_t* cur_bar, int32_t* cur_beat,
			      int32_t* cur_tick, SAMPLE_T* ticks_per_beat, jack_nframes_t* total_frames,
			      float* bmp, float* beat_type, float* beats_per_bar);
//check if any parameters that control the transport head have changed, if yes - request an update with the new
//parameters to the transport head
void app_jack_update_transport_from_params_rt(JACK_INFO* jack_data);
//return the trk parameters, like bpm bar etc
PRM_CONTAIN* app_jack_return_param_container(JACK_INFO* jack_data);
//function that changes the transport head
//transport_type: 0 - transport_stop; 1 - transport_start;
//should be realtime safe, but better double check before using in realtime processes
int app_jack_transport(JACK_INFO* jack_data, unsigned int transport_type);
