#pragma once
#include <jack/jack.h>
#include <jack/midiport.h>
#include <stdbool.h>
#include <stdint.h>
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
    //the index of the event that should be written to in the buffer
    NFRAMES_T w_pos;
    //the total size of each of the arrays on this struct
    unsigned int array_size;
}JACK_MIDI_CONT;

//jack main struct
typedef struct _jack_info JACK_INFO;

//function that initializes the client
JACK_INFO* jack_initialize(void *arg, const char *client_name,
                           int(*process)(jack_nframes_t, void*));
//return the transport param container (the Trk params), NULL on error
PRM_CONTAIN* app_jack_trk_param_container(void* jack_data_handle);

//read messages from ui on [audio-thread]
//update parameters, pause jack process
int app_jack_read_ui_to_rt_messages(JACK_INFO* jack_data);
//read messages from rt on [main-thread]
//update parameters, log messages from rt thread
int app_jack_read_rt_to_ui_messages(JACK_INFO* jack_data);

//clean the midi container
void app_jack_clean_midi_cont(JACK_MIDI_CONT* midi_cont);
//initate the midi container where velocities, note pitches etc will be stored
JACK_MIDI_CONT* app_jack_init_midi_cont(unsigned int array_size);
//reset all the arrays of the midi container to 0
void app_jack_midi_cont_reset(JACK_MIDI_CONT* midi_cont);
//rename the port on client
int app_jack_port_rename(void* client_in, void* port, const char* new_port_name);
//register ports on a jack client if its known to the data
//owner_tag/owner_uid are two numbers the caller uses together to identify what
//the port belongs to - jack interprets neither, it only hands them back on the
//port's JackPortInfo. A zero owner_tag means "not recorded"
void* app_jack_create_port_on_client(void* client_in, unsigned int port_type, unsigned int io_type,
					    const char* port_name, uint64_t owner_tag,
					    uint64_t owner_uid);

//which cached list to read. ALL is every port; the others are the ports a
//source of the opposite flow and the same type can be connected to
typedef enum {
    JACK_PORT_LIST_ALL = 0,
    JACK_PORT_LIST_OUT_AUDIO,
    JACK_PORT_LIST_OUT_MIDI,
    JACK_PORT_LIST_IN_AUDIO,
    JACK_PORT_LIST_IN_MIDI,
    JACK_PORT_LIST_COUNT
} JackPortList;

//one cached port. name/client are borrowed and stay valid until the next
//app_jack_ports_sync that rebuilds the port list
typedef struct _jack_port_info {
    const char* name;   //full "client:port"
    const char* client; //the client part of name
    uint64_t key;       //identity minted the first time this port name was
                        //seen, stable for the life of the program
    unsigned int type;  //PORT_TYPE_AUDIO or PORT_TYPE_MIDI
    unsigned long flow; //JackPortIsOutput or JackPortIsInput
    //as given to app_jack_create_port_on_client. Both 0 for a port this client
    //did not register
    uint64_t owner_tag;
    uint64_t owner_uid;
} JackPortInfo;

//what app_jack_ports_sync rebuilt
typedef struct _jack_port_sync {
    bool ports;
    bool connections;
} JackPortSync;

//rebuild whatever jack's notifications invalidated since the last call. The
//only consumer of the port/connection dirty flags - everything else reads the
//cache through the accessors below
JackPortSync app_jack_ports_sync(JACK_INFO* jack_data);
size_t app_jack_port_count(JACK_INFO* jack_data, JackPortList list);
bool app_jack_port_at(JACK_INFO* jack_data, JackPortList list, size_t idx,
                      JackPortInfo* out);
bool app_jack_port_by_key(JACK_INFO* jack_data, uint64_t key, JackPortInfo* out);
//ports the one named by key is currently connected to
size_t app_jack_port_connection_count(JACK_INFO* jack_data, uint64_t key);
bool app_jack_port_connection_at(JACK_INFO* jack_data, uint64_t key, size_t idx,
                                 JackPortInfo* out);
bool app_jack_port_keys_connected(JACK_INFO* jack_data, uint64_t key_a,
                                  uint64_t key_b);
//link or unlink two cached ports. They must be of opposite flow; the pair is
//ordered for jack here, so either order works. 0 on success
int app_jack_connect_keys(JACK_INFO* jack_data, uint64_t key_a, uint64_t key_b);
int app_jack_disconnect_keys(JACK_INFO* jack_data, uint64_t key_a,
                             uint64_t key_b);
//return the smaple rate of a jack client (of the server really)
float app_jack_return_samplerate(JACK_INFO* jack_data);
//return the buffer size
int app_jack_return_buffer_size(JACK_INFO* jack_data);
//activate the jack client
int app_jack_activate(JACK_INFO *jack_data);
//get buffer from a jack port 
void* app_jack_get_buffer_rt(void* port, jack_nframes_t nframes);
//clear the buffer of midi out buffer
void app_jack_midi_clear_buffer_rt(void* buffer);
//write to the midi out buffer
int app_jack_midi_events_write_rt(void* buffer, jack_nframes_t time, const jack_midi_data_t* data,
				  size_t data_size);
//return three arrays for the midi_in, notes played, velocities and the times for each in nframe
void app_jack_return_notes_vels_rt(void* midi_in, JACK_MIDI_CONT* midi_cont);

//return the size allowed for the port name
int app_jack_port_name_size();
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
