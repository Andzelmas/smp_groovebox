#pragma once
#include "params.h"

enum SmpStatus{
    smp_data_malloc_fail = -1,
    one_sample_malloc_fail = -2,
    sample_malloc_failed = -3,
    sample_load_memory_failed = -4,
    sample_attach_failed = -5
};

typedef enum SmpStatus smp_status_t;
//the sample object with its buffer, that is loaded from file
typedef struct _smp_smp SMP_SMP;
//the sampler main struct that holds the samples and other data*/
typedef struct _smp_info SMP_INFO; 

//initialize the sampler to empty values
SMP_INFO* smp_init(unsigned int buffer_size, SAMPLE_T samplerate, smp_status_t *status, void* audio_backend);
//create ports in ports[n]->sys_port
int smp_activate_backend_ports(SMP_INFO* smp_data);
//the function that adds a new sample and gets its buffer from a file to memory
//if succesfull returns the id of the new sample
int smp_add(SMP_INFO *smp_data, const char* samp_path, int in_id);
//add sample to the sample array
static int smp_add_smp_to_array(SMP_INFO* smp_data, SMP_SMP* smp, int in_id);
//process the samples and return summed audio buffer
//uses one callback to get_buffer from the sys_ports and another to get_notes from the midi sys_port
int smp_sample_process_rt(SMP_INFO* smp_data, uint32_t nframes);
//some the cur_smp buffer to out_L and out_R according to the number of channels of cur_smp
static void smp_sum_channel_buffers_rt(SMP_SMP* cur_smp, SAMPLE_T* out_L, SAMPLE_T* out_R,
				       SAMPLE_T mult, int chans);
//produce silence to out ports
void smp_produce_silence_rt(SMP_INFO* smp_data, unsigned int nframes);
//return an array of sys_ports for the sampler
//need to free the sys_port array. So not suitable for real time application
void** smp_return_sys_ports(SMP_INFO* smp_data, unsigned int* number_ports);
//return the parameter container of the smp_id sample
//to get or set values use the params.h functions
PRM_CONTAIN* smp_get_sample_param_container(SMP_INFO* smp_data, int smp_id);
//copy to new malloced string and return the file path of the sample
char* smp_get_sample_file_path(SMP_INFO* smp_data, int smp_id);
//remove a single sample
int smp_remove_sample(SMP_INFO* smp_data, unsigned int idx);
//clean the memory
int smp_clean_memory(SMP_INFO *smp_data);
