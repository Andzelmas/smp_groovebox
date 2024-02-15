#include <stdlib.h>
#include <string.h>
//my libraries
#include "../util_funcs/wav_funcs.h"
#include "../util_funcs/math_funcs.h"
#include "sampler.h"
#include "../types.h"
#include "../util_funcs/log_funcs.h"
#include "../jack_funcs/jack_funcs.h"

//how many samples to read to SMP_SMP buffer at once
#define SINGLE_READ_SAMPLE_B 12000
//max samples that there can be
#define MAX_SAMPLES 100
//number of the parameters on each sample
#define NUM_PARAMS 1
//number of output ports for the sampler
#define OUTS 2
//number of midi in ports for the samples
#define IN_MIDI 1
//max midi events to store in the midi_cont container
#define MIDI_CONT_ARRAY_SIZE 50

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
    //what is the maximum id in the smp_data currently, this is for optimization, so that for loops dont go
    //through the whole MAX_SAMPLES array
    int sample_max_id;
    //this is the sample array
    SMP_SMP* samples [MAX_SAMPLES];
    //the array of ports for the audio_client;
    SMP_PORT* ports;
    //number of available ports
    unsigned int num_ports;
    //callback functions for the audio backend to manipulate ports, midi etc.
    //the audio client data 
    void* audio_backend;
    //the midi container that has the note pitches etc.
    JACK_MIDI_CONT* midi_cont;    
}SMP_INFO; 

SMP_INFO* smp_init(unsigned int buffer_size, SAMPLE_T samplerate,
		   smp_status_t *status,
		   void* audio_backend){
    /*allocate memory for the smp_data struct, that will contain the other samples*/
    SMP_INFO *smp_data = (SMP_INFO*) malloc(sizeof(SMP_INFO));
    if(!smp_data){
        *status = smp_data_malloc_fail;
        return NULL;
    }
    smp_data->midi_cont = NULL;
    smp_data->buffer_size = buffer_size;
    smp_data->samplerate = samplerate;
    
    smp_data->sample_max_id = -1;  
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
	    smp_data->ports[i].port_flow = FLOW_INPUT;
	    smp_data->ports[i].port_type = TYPE_MIDI;
	    smp_data->ports[i].port_name = "sampler|midi_in";
	   
	}
	if(i==1){
	    smp_data->ports[i].port_flow = FLOW_OUTPUT;
	    smp_data->ports[i].port_type = TYPE_AUDIO;
	    smp_data->ports[i].port_name = "sampler|out_L";	    
	}
	if(i==2){
	    smp_data->ports[i].port_flow = FLOW_OUTPUT;
	    smp_data->ports[i].port_type = TYPE_AUDIO;
	    smp_data->ports[i].port_name = "sampler|out_R";	    
	}	
    }
     for(int i = 0; i < MAX_SAMPLES; i++){
	smp_data->samples[i] = NULL;
    }
     
     //inititalize the callbacks from the audio backend
     smp_data->audio_backend = audio_backend;
     smp_data->midi_cont = app_jack_init_midi_cont(MIDI_CONT_ARRAY_SIZE);
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
    int return_var = -1;
    SMP_SMP *cur_smp = (SMP_SMP*)malloc(sizeof(SMP_SMP)); 
    if(!cur_smp){
	return_var = sample_malloc_failed;
	goto done;
    }

    //init the sample parameters to default values
    cur_smp->params = params_init_param_container(NUM_PARAMS, (const char*[1]){"Note"}, (float[1]){40}, (float[1]){0},
						  (float[1]){127}, (float[1]){1}, (unsigned char[1]){Uchar_type});
    
    //first populate the new sample members
    cur_smp->offset = 0;
    //TODO samplerate is not needed, when we load sample to memory we also need to convert it to the system
    //sample rate, when system sample rate changes, the jack callback of samplerate change
    //calls a function in app_data to change variables on various structs that depend on the samplerate
    //one of those members is the cur_smp->buffer- the app_data function will call the function in
    //smp_data to adapt the buffer to the new sample rate.
    cur_smp->file_path = NULL;
    cur_smp->samplerate = 0;
    cur_smp->chans = 0;
    cur_smp->playing = 0;
    cur_smp->midi_vel = (SAMPLE_T)1.0;
    cur_smp->buffer = NULL;
    cur_smp->samples_loaded = 0;
    SF_INFO samp_props;
    //load sample to memory, remember that sample channels can differ
    int load_err = 0;
    load_err = load_wav_mem(&samp_props, SINGLE_READ_SAMPLE_B,
			    samp_path, &cur_smp->buffer);
    //if could not load the file, free memory this sample will not be loaded
    if(load_err<0){
	if(cur_smp->buffer)free(cur_smp->buffer);
	free(cur_smp);
	return_var = sample_load_memory_failed;
	goto done;
    }
    //if the buffer was loaded succesfuy the load_err will contain the number of samples loaded
    cur_smp->samples_loaded = load_err;
    //write the samplerate and number of channels
    cur_smp->samplerate = samp_props.samplerate;
    cur_smp->chans = samp_props.channels;
    
    int cur_id = smp_add_smp_to_array(smp_data, cur_smp, in_id);
    if(cur_id == -1){
	return_var = -1;
	if(cur_smp->buffer)free(cur_smp->buffer);
	free(cur_smp);
	goto done;
    }
    cur_smp->id = cur_id;
    int max_id = smp_data->sample_max_id;
    if(cur_id > max_id){
	smp_data->sample_max_id = cur_id;
    }

    //malloc the file_path of the sample
    cur_smp->file_path = (char*)malloc(sizeof(char) * (strlen(samp_path)+1));
    if(!cur_smp->file_path){
	smp_remove_sample(smp_data, cur_smp->id);
	return -1;
    }

    strcpy(cur_smp->file_path, samp_path);

   
    return_var = cur_smp->id;
    
done:
    return return_var;
}

static int smp_add_smp_to_array(SMP_INFO* smp_data, SMP_SMP* smp, int in_id){
    int return_val = -1;
    if(in_id!=-1){
	if(in_id>=MAX_SAMPLES)goto finish;
	if(smp_data->samples[in_id] !=NULL){
	    smp_remove_sample(smp_data, in_id);
	}
	smp_data->samples[in_id] = smp;
	return_val = in_id;
	goto finish;
    }
    else{
	int max_id = smp_data->sample_max_id;
	for(int i = 0; i < max_id+2; i++){
	    if(smp_data->samples[i] == NULL){
		smp_data->samples[i] = smp;
		return_val = i;
		goto finish;
	    }
	}
    }
    
finish:
    return return_val;
}

int smp_sample_process_rt(SMP_INFO* smp_data, uint32_t nframes){
    SMP_PORT* midi_port = &(smp_data->ports[0]);
    SMP_PORT* out_L_port = &(smp_data->ports[1]);
    SMP_PORT* out_R_port = &(smp_data->ports[2]);    
    void* midi_buffer = app_jack_get_buffer_rt(midi_port->sys_port , nframes);
    SAMPLE_T* out_L = app_jack_get_buffer_rt(out_L_port->sys_port, nframes);
    SAMPLE_T* out_R = app_jack_get_buffer_rt(out_R_port->sys_port, nframes);
    //if there are no samples do nothing
    int max_sample = smp_data->sample_max_id;
    memset(out_L, '\0', sizeof(SAMPLE_T)*nframes);
    memset(out_R, '\0', sizeof(SAMPLE_T)*nframes); 
    if(max_sample < 0){
	return -1;
    }
    if(!smp_data->midi_cont)return -1;
    //get the notes
    app_jack_midi_cont_reset(smp_data->midi_cont);
    app_jack_return_notes_vels_rt(midi_buffer, smp_data->midi_cont);
    JACK_MIDI_CONT* midi_cont = smp_data->midi_cont;
    //go through each sample
    for(unsigned int iter = 0; iter < max_sample+1; iter++){
	if(smp_data->samples[iter] == NULL)continue;
	SMP_SMP* cur_smp = smp_data->samples[iter];
	if(!cur_smp->params)continue;
        //if the sample is not ready go to another
        if(cur_smp->buffer==NULL)continue;
	if(cur_smp->chans<=0)continue;
	if(cur_smp->samples_loaded <=0)continue;
	//get the note parameter from the current samples rt_param array
	unsigned char type = 0;
	unsigned char cur_note = (unsigned char)param_get_value(cur_smp->params, 0, &type, 0, 0, 1);
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

void smp_produce_silence_rt(SMP_INFO* smp_data, unsigned int nframes){
    if(!smp_data)return;
    SMP_PORT* out_L_port = &(smp_data->ports[1]);
    SMP_PORT* out_R_port = &(smp_data->ports[2]);    
    SAMPLE_T* out_L = app_jack_get_buffer_rt(out_L_port->sys_port, nframes);
    SAMPLE_T* out_R = app_jack_get_buffer_rt(out_R_port->sys_port, nframes);    
    memset(out_L, '\0', sizeof(SAMPLE_T)*nframes);
    memset(out_R, '\0', sizeof(SAMPLE_T)*nframes);
}

void** smp_return_sys_ports(SMP_INFO* smp_data, unsigned int* number_ports){
    if(!smp_data)return NULL;
    int port_num = smp_data->num_ports;
    if(port_num<=0)return NULL;
    if(number_ports)*number_ports = port_num;
    void** ret_sys_ports = (void**)malloc(sizeof(void*) * port_num);
    for(int i = 0; i<port_num; i++){
	ret_sys_ports[i] = NULL;
	SMP_PORT cur_port = smp_data->ports[i];
	void* cur_sys_port = cur_port.sys_port;
	if(cur_sys_port){
	    ret_sys_ports[i] = cur_sys_port;
	}
    }
    return ret_sys_ports;
}

PRM_CONTAIN* smp_get_sample_param_container(SMP_INFO* smp_data, int smp_id){
    if(!smp_data)return NULL;
    if(smp_id > smp_data->sample_max_id)return NULL;
    SMP_SMP* cur_sample = smp_data->samples[smp_id];
    if(!cur_sample)return NULL;
    return cur_sample->params;
}

char* smp_get_sample_file_path(SMP_INFO* smp_data, int smp_id){
    if(!smp_data)return NULL;
    if(smp_id > smp_data->sample_max_id)return NULL;
    SMP_SMP* cur_smp = smp_data->samples[smp_id];
    if(!cur_smp)return NULL;
    if(!cur_smp->file_path)return NULL;
    char* ret_name = (char*)malloc(sizeof(char) * (strlen(cur_smp->file_path) + 1));
    if(!ret_name)return NULL;
    strcpy(ret_name, cur_smp->file_path);
    return ret_name;
}

int smp_remove_sample(SMP_INFO* smp_data, unsigned int idx){
    //dont do anything while the structure is being read
    int return_val = 0;
    int expected = 0;
    //clear the sample audio buffer
    SMP_SMP* cur_smp = smp_data->samples[idx];
    if(!cur_smp){
	return_val = -1;
	goto done;
    }
    int max_sample = smp_data->sample_max_id;
    if(idx > max_sample){
	return_val = -1;
	goto done;
    }    
    if(cur_smp->buffer!=NULL)free(cur_smp->buffer);
    if(cur_smp->file_path)free(cur_smp->file_path);
    //clean the parameter container
    if(cur_smp->params)param_clean_param_container(cur_smp->params);
    
    cur_smp->buffer = NULL;
    
    smp_data->samples[idx] = NULL;    
    free(cur_smp);
    //find the biggest id if this idx was the biggest
    if(max_sample == idx){
	int max_id = -1;
	for(unsigned int i = 0; i<max_sample+1; i++){
	    cur_smp = smp_data->samples[i];
	    if(!cur_smp)continue;
	    if(cur_smp->id >= max_id){
		max_id = cur_smp->id;
	    }
	}
	smp_data->sample_max_id = max_id;
    }

done:
    return return_val;
}

int smp_clean_memory(SMP_INFO *smp_data){
    if(!smp_data)return -1;
    //clean the samples and their buffers
    int max_sample = smp_data->sample_max_id;  
    if(max_sample < 0)goto no_clean_samples;
    
    for(int i = 0; i<max_sample+1; i++){
	if(smp_data->samples[i]==NULL)continue;
	smp_remove_sample(smp_data, i);
    }
no_clean_samples:
    if(smp_data->ports){
	for(int i = 0; i< smp_data->num_ports; i++){
	    SMP_PORT* port = &(smp_data->ports[i]);
	    if(port->sys_port){
		app_jack_unregister_port(smp_data->audio_backend, port->sys_port);
	    }
	}
	free(smp_data->ports);
    }
    //remove the midi_container
    if(smp_data->midi_cont){
	app_jack_clean_midi_cont(smp_data->midi_cont);
	free(smp_data->midi_cont);
    }
    
    free(smp_data);

    return 0;
}
