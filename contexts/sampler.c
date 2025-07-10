#include <stdlib.h>
#include <string.h>
#include <threads.h>
//my libraries
#include "../util_funcs/wav_funcs.h"
#include "../util_funcs/math_funcs.h"
#include "sampler.h"
#include "../util_funcs/log_funcs.h"
#include "../jack_funcs/jack_funcs.h"
#include "context_control.h"

//how many samples to read to SMP_SMP buffer at once
#define SINGLE_READ_SAMPLE_B 12000
//max samples that there can be
#define MAX_SAMPLES 3
//number of the parameters on each sample
#define NUM_PARAMS 1
//number of output ports for the sampler
#define OUTS 2
//number of midi in ports for the samples
#define IN_MIDI 1

static thread_local bool is_audio_thread = false;

typedef struct _smp_smp{
    //the note id, used to link to the cx struct
    int id;
    //the file path of the sample
    char* file_path;
    //the sample sample rate
    int samplerate;
    //the number of channels
    int chans;
    //the parameter container, that holds the rt and ui param arrays
    PRM_CONTAIN* params;
    //the sample buffer that holds the audio sample in memory has to be freed
    SAMPLE_T* buffer;
    //how many samples are loaded
    int samples_loaded;
    //the current playhead pos in frames of the sample
    int offset;
    //if the sample is playing
    int playing;
    //if processing == 0 the sample will not run on the [audio-thread]
    int processing;
    //what velocity was used to hit the sample
    SAMPLE_T midi_vel;
}SMP_SMP;

typedef struct _smp_port{
    //port id
    unsigned int id;
    //port type;
    unsigned int port_type;
    //port flow;
    unsigned int port_flow;
    //the name of the port
    const char* port_name;
    //the ptr to the port itself;
    void* sys_port;
}SMP_PORT;
//the drum sampler main struct that holds the samples and other data
//Only realtime thread directly modifies and reads the SMP_SMP, non realtime thread can remove it or add
//new one, but before doing so it asks the realtime thread to pause sample processing.
typedef struct _smp_info{
    //the buffer size of the audio system
    unsigned int buffer_size;
    //the samplerate of the system;
    SAMPLE_T samplerate;
    //this is the sample array
    SMP_SMP samples [MAX_SAMPLES+1]; //one sample too many in array, the last one will be to check for the end of the array
    //the array of ports for the audio_client;
    SMP_PORT* ports;
    //number of available ports
    unsigned int num_ports;
    //callback functions for the audio backend to manipulate ports, midi etc.
    //the audio client data 
    void* audio_backend;
    //the midi container that has the note pitches etc.
    JACK_MIDI_CONT* midi_cont;
    //control_data struct to control sys messages between [audio-thread] and [main-thread] (stop processing sample, start processing sample and etc.)
    CXCONTROL* control_data;
}SMP_INFO; 

//functions for thread safe string messages
//this can be called only on [main-thread]
static int smp_sys_msg(void* user_data, const char* msg){
    //TODO right now smp_data is not used, but there is a future feature to not write string messages to file so often
    SMP_INFO* smp_data = (SMP_INFO*)user_data;
    log_append_logfile("%s", msg);
    return 0;
}

static int smp_start_process(void* user_data){
    SMP_SMP* smp = (SMP_SMP*)user_data;
    if(!smp)return -1;
    if(smp->processing == 1)return 0;
    if(!smp->buffer)return -1;
    smp->processing = 1;
    return 0;
}

static int smp_stop_process(void* user_data){
    SMP_SMP* smp = (SMP_SMP*)user_data;
    if(!smp)return -1;
    if(smp->processing == 0)return 0;
    //TODO could silence the sample before stopping it here
    smp->processing = 0;
    return 0;
}

int smp_read_ui_to_rt_messages(SMP_INFO* smp_data){
    //false on [main-thread] but has to be true on [audio-thread]
    is_audio_thread = true;
    if(!smp_data)return -1;
    context_sub_process_rt(smp_data->control_data);
    
    for(unsigned int i = 0; i < MAX_SAMPLES; i++){
	SMP_SMP* smp = &(smp_data->samples[i]);
	//do nothing if the sample is stopped
	if(smp->processing == 0)continue;
	if(!smp->buffer || !smp->params)continue;
	param_msgs_process(smp->params, 1);
    }
    return 0;
}
int smp_read_rt_to_ui_messages(SMP_INFO* smp_data){
    if(!smp_data)return -1;
    context_sub_process_ui(smp_data->control_data);

    //read the param rt_to_ui messages and set the parameter values
    for(unsigned int i = 0; i < MAX_SAMPLES; i++){
	SMP_SMP* smp = &(smp_data->samples[i]);
	if(!smp->buffer || !smp->params)continue;
        param_msgs_process(smp->params, 0);
    }    
    return 0;
}

static int smp_remove_sample(SMP_INFO* smp_data, unsigned int idx){
    if(!smp_data)return -1;
    if(idx >= MAX_SAMPLES)return -1;
    //clear the sample audio buffer
    SMP_SMP* cur_smp = &(smp_data->samples[idx]);
 
    if(cur_smp->buffer)free(cur_smp->buffer);
    cur_smp->buffer = NULL;
    if(cur_smp->file_path)free(cur_smp->file_path);
    cur_smp->file_path = NULL;
    //clean the parameter container
    if(cur_smp->params)param_clean_param_container(cur_smp->params);
    cur_smp->params = NULL;

    cur_smp->chans = 0;
    cur_smp->midi_vel = (SAMPLE_T)1.0;
    cur_smp->offset = 0;
    cur_smp->playing = 0;
    cur_smp->samplerate = 0;
    cur_smp->samples_loaded = 0;

    return 0;
}

SMP_INFO* smp_init(unsigned int buffer_size, SAMPLE_T samplerate,
		   smp_status_t *status,
		   void* audio_backend){
    /*allocate memory for the smp_data struct, that will contain the other samples*/
    SMP_INFO *smp_data = (SMP_INFO*) malloc(sizeof(SMP_INFO));
    if(!smp_data){
        *status = smp_data_malloc_fail;
        return NULL;
    }
    CXCONTROL_RT_FUNCS rt_funcs_struct = {0};
    CXCONTROL_UI_FUNCS ui_funcs_struct = {0};
    rt_funcs_struct.subcx_start_process = smp_start_process;
    rt_funcs_struct.subcx_stop_process = smp_stop_process;
    ui_funcs_struct.send_msg = smp_sys_msg;
    smp_data->control_data = context_sub_init(rt_funcs_struct, ui_funcs_struct);
    if(!smp_data->control_data){
	free(smp_data);
	*status = smp_data_malloc_fail;
	return NULL;
    }

    smp_data->midi_cont = NULL;
    smp_data->buffer_size = buffer_size;
    smp_data->samplerate = samplerate;
    //init the ports
    smp_data->num_ports = IN_MIDI + OUTS;
    smp_data->ports = (SMP_PORT*)calloc(smp_data->num_ports, sizeof(SMP_PORT));
    if(!smp_data->ports){
	*status = smp_data_malloc_fail;
	free(smp_data);
	return NULL;
    }
    for(int i = 0; i< smp_data->num_ports; i++){
	smp_data->ports[i].id = i;
	if(i==0){
	    smp_data->ports[i].port_flow = PORT_FLOW_INPUT;
	    smp_data->ports[i].port_type = PORT_TYPE_MIDI;
	    smp_data->ports[i].port_name = "sampler|midi_in";
	   
	}
	if(i==1){
	    smp_data->ports[i].port_flow = PORT_FLOW_OUTPUT;
	    smp_data->ports[i].port_type = PORT_TYPE_AUDIO;
	    smp_data->ports[i].port_name = "sampler|out_L";	    
	}
	if(i==2){
	    smp_data->ports[i].port_flow = PORT_FLOW_OUTPUT;
	    smp_data->ports[i].port_type = PORT_TYPE_AUDIO;
	    smp_data->ports[i].port_name = "sampler|out_R";	    
	}	
    }
    for(int i = 0; i < (MAX_SAMPLES+1); i++){
	 SMP_SMP* samp = &(smp_data->samples[i]);
	 samp->buffer = NULL;
	 samp->chans = 0;
	 samp->file_path = NULL;
	 samp->id = i;
	 samp->midi_vel = (SAMPLE_T)1.0;
	 samp->offset = 0;
	 samp->params = NULL;
	 samp->processing = 0;
	 samp->playing = 0;
	 samp->samplerate = 0;
	 samp->samples_loaded = 0;
    }
     
     //inititalize the callbacks from the audio backend
     smp_data->audio_backend = audio_backend;
     smp_data->midi_cont = app_jack_init_midi_cont(MAX_MIDI_CONT_ITEMS);
     if(!smp_data->midi_cont){
	 smp_clean_memory(smp_data);
	 return NULL;
     }
     
     smp_activate_backend_ports(smp_data);

     return smp_data;
}

int smp_activate_backend_ports(SMP_INFO* smp_data){
    if(!smp_data)return -1;
    if(!smp_data->audio_backend)return -1;
    if(!smp_data->ports)return -1;
    for(int i = 0; i < smp_data->num_ports; i++){
	SMP_PORT* cur_port = &(smp_data->ports[i]);
	cur_port->sys_port = app_jack_create_port_on_client(smp_data->audio_backend, cur_port->port_type,
						     cur_port->port_flow, cur_port->port_name);
    }
    return 0;
}

int smp_add(SMP_INFO *smp_data, const char* samp_path, int in_id){
    if(!smp_data)return -1;
    if(in_id >= MAX_SAMPLES)return -1;
    int smp_id = in_id;
    //find empty sample if id is -1
    if(smp_id == -1){
	for(int i = 0; i < (MAX_SAMPLES+1); i++){
	    SMP_SMP* cur_smp = &(smp_data->samples[i]);
	    if(cur_smp->file_path)continue;
	    smp_id = cur_smp->id;
	    break;
	}
    }
    if(smp_id == -1 || smp_id >= MAX_SAMPLES)return -1;
    //remove sample if this sample slot is occupied for some reason
    smp_stop_and_remove_sample(smp_data, smp_id);
    
    SMP_SMP *cur_smp = &(smp_data->samples[smp_id]);
    //init the sample parameters to default values
    cur_smp->params = params_init_param_container(NUM_PARAMS, (char*[1]){"Note"}, (PARAM_T[1]){40}, (PARAM_T[1]){0},
						  (PARAM_T[1]){127}, (PARAM_T[1]){1}, (unsigned char[1]){Uchar_type}, NULL, NULL);
    
    //TODO samplerate is not needed, when we load sample to memory we also need to convert it to the system
    //sample rate, when system sample rate changes, the jack callback of samplerate change
    //calls a function in app_data to change variables on various structs that depend on the samplerate
    //one of those members is the cur_smp->buffer- the app_data function will call the function in
    //smp_data to adapt the buffer to the new sample rate.
    SF_INFO samp_props;
    //load sample to memory, remember that sample channels can differ
    int load_err = 0;
    load_err = load_wav_mem(&samp_props, SINGLE_READ_SAMPLE_B,
			    samp_path, &cur_smp->buffer);
    //if could not load the file, free memory this sample will not be loaded
    if(load_err<0){
	smp_stop_and_remove_sample(smp_data, smp_id);
        return sample_load_memory_failed;
    }
    //if the buffer was loaded succesfuly the load_err will contain the number of samples loaded
    cur_smp->samples_loaded = load_err;
    //write the samplerate and number of channels
    cur_smp->samplerate = samp_props.samplerate;
    cur_smp->chans = samp_props.channels;

    //malloc the file_path of the sample
    cur_smp->file_path = (char*)malloc(sizeof(char) * (strlen(samp_path)+1));
    if(!cur_smp->file_path){
	smp_stop_and_remove_sample(smp_data, smp_id);
	return -1;
    }

    strcpy(cur_smp->file_path, samp_path);
    //now this sample can start processing
    context_sub_wait_for_start(smp_data->control_data, (void*)cur_smp);
    return smp_id;
}

int smp_sample_process_rt(SMP_INFO* smp_data, uint32_t nframes){
    SMP_PORT* midi_port = &(smp_data->ports[0]);
    SMP_PORT* out_L_port = &(smp_data->ports[1]);
    SMP_PORT* out_R_port = &(smp_data->ports[2]);    
    void* midi_buffer = app_jack_get_buffer_rt(midi_port->sys_port , nframes);
    SAMPLE_T* out_L = app_jack_get_buffer_rt(out_L_port->sys_port, nframes);
    SAMPLE_T* out_R = app_jack_get_buffer_rt(out_R_port->sys_port, nframes);
    if(!midi_buffer || !out_L || !out_R)return -1;
    memset(out_L, '\0', sizeof(SAMPLE_T)*nframes);
    memset(out_R, '\0', sizeof(SAMPLE_T)*nframes); 

    if(!smp_data->midi_cont)return -1;
    //get the notes
    app_jack_midi_cont_reset(smp_data->midi_cont);
    app_jack_return_notes_vels_rt(midi_buffer, smp_data->midi_cont);
    JACK_MIDI_CONT* midi_cont = smp_data->midi_cont;
    //go through each sample
    for(unsigned int iter = 0; iter < MAX_SAMPLES; iter++){
	SMP_SMP* cur_smp = &(smp_data->samples[iter]);
	if(cur_smp->processing == 0)continue;
	if(!cur_smp->params)continue;
        //if the sample is not ready go to another
        if(cur_smp->buffer==NULL)continue;
	if(cur_smp->chans<=0)continue;
	if(cur_smp->samples_loaded <=0)continue;
	//get the note parameter from the current samples rt_param array
	unsigned char cur_note = (unsigned char)param_get_value(cur_smp->params, 0, 0, 0, 1);
	//go through the frames
	//TODO really like that goes through each frame, but not sure how to find the
	//midi event differently
	for(int cur_frame = 0; cur_frame < nframes; cur_frame++){
	    //find if there is a note of a sample in the notes
	    unsigned char this_vel = 0;
	    for(int i = 0; i<midi_cont->num_events; i++){
		//if the note is played not on this time slice skip it
		if(midi_cont->nframe_nums[i] != cur_frame)continue;
		//we are only looking for note on
		if((midi_cont->types[i] & 0xf0) != 0x90)continue;
		if(midi_cont->note_pitches[i] == cur_note){
		    this_vel = midi_cont->vel_trig[i];
		    break;
		}
	    }
	    if(this_vel>0){
		//play this sample if the midi trigger is more than 0
		cur_smp->playing = 1;
		//convert the midi velocity and apply to sample
		//TODO should not be linear
		SAMPLE_T cur_vel = fit_range(127.0, 0.0, 1.0, 0.0, (SAMPLE_T)this_vel);
		cur_smp->midi_vel = cur_vel;
		//sample playhead to 0
		cur_smp->offset = 0;
	    }
	    //if sample is not playing go to next frame
	    if(cur_smp->playing == 0)continue;
	    smp_sum_channel_buffers_rt(cur_smp, &(out_L[cur_frame]), &(out_R[cur_frame]),
				       cur_smp->midi_vel, OUTS);
	    //increase the playhead if the playhead is at the end go to the start 
	    //and stop playing the sample. We offset by the number of the channels, because the files are saved
	    //in the buffer as interleaved
	    cur_smp->offset += cur_smp->chans;
	    if(cur_smp->offset>=cur_smp->samples_loaded){
		cur_smp->offset = 0;
		cur_smp->playing = 0;
	    }

	    //TODO a very simple summing here, maybe add and then normalize the out_L and out_R
	    if(out_L[cur_frame] > 1.0) out_L[cur_frame] = 1.0;
	    if(out_R[cur_frame] > 1.0) out_R[cur_frame] = 1.0;
	}
    }
    
    return 0;
}

static void smp_sum_channel_buffers_rt(SMP_SMP* cur_smp, SAMPLE_T* out_L, SAMPLE_T* out_R,
			     SAMPLE_T mult, int chans){
    //the cur_smp has the same number of channels as the system
    if(cur_smp->chans == chans){
	*out_L += cur_smp->buffer[cur_smp->offset] * mult;
	*out_R += cur_smp->buffer[cur_smp->offset + 1] * mult;
    }
    //if there are less sample channels then the port buffers
    if(cur_smp->chans < chans){
	*out_L += cur_smp->buffer[cur_smp->offset] * mult;
	*out_R += cur_smp->buffer[cur_smp->offset+(cur_smp->chans-1)] * mult;
    }    
}

PRM_CONTAIN* smp_param_return_param_container(SMP_INFO* smp_data, int smp_id){
    if(!smp_data)return NULL;
    if(smp_id >= MAX_SAMPLES || smp_id < 0)return NULL;
    SMP_SMP* smp = &(smp_data->samples[smp_id]);
    if(!smp->buffer || !smp->params)return NULL;
    return smp->params;
}

char* smp_get_sample_file_path(SMP_INFO* smp_data, int smp_id){
    if(!smp_data)return NULL;
    if(smp_id >= MAX_SAMPLES)return NULL;
    SMP_SMP* cur_smp = &(smp_data->samples[smp_id]);
    if(!cur_smp->file_path)return NULL;
    char* ret_name = (char*)malloc(sizeof(char) * (strlen(cur_smp->file_path) + 1));
    if(!ret_name)return NULL;
    strcpy(ret_name, cur_smp->file_path);
    return ret_name;
}

int smp_stop_and_remove_sample(SMP_INFO* smp_data, int idx){
    if(idx >= MAX_SAMPLES || idx < 0)return -1;
    SMP_SMP* cur_smp = &(smp_data->samples[idx]);
    //stop processing the sample
    context_sub_wait_for_stop(smp_data->control_data, (void*)cur_smp);
    return smp_remove_sample(smp_data, idx);
}

int smp_clean_memory(SMP_INFO *smp_data){
    if(!smp_data)return -1;    
    for(int i = 0; i<MAX_SAMPLES; i++){
	smp_remove_sample(smp_data, i);
    }

    if(smp_data->ports){
	for(int i = 0; i< smp_data->num_ports; i++){
	    SMP_PORT* port = &(smp_data->ports[i]);
	    if(port->sys_port){
		app_jack_unregister_port(smp_data->audio_backend, port->sys_port);
	    }
	}
	free(smp_data->ports);
    }
    smp_data->ports = NULL;
    //remove the midi_container
    if(smp_data->midi_cont){
	app_jack_clean_midi_cont(smp_data->midi_cont);
	free(smp_data->midi_cont);
    }
    smp_data->midi_cont = NULL;

    context_sub_clean(smp_data->control_data);
    free(smp_data);

    return 0;
}
