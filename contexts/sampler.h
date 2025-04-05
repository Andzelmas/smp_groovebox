#pragma once
#include "params.h"
#include "../types.h"

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

//read sys and param messages on [audio-thread] and [main-thread]
int smp_read_ui_to_rt_messages(SMP_INFO* smp_data);
int smp_read_rt_to_ui_messages(SMP_INFO* smp_data);
//initialize the sampler to empty values
SMP_INFO* smp_init(unsigned int buffer_size, SAMPLE_T samplerate, smp_status_t *status, void* audio_backend);
//create ports in ports[n]->sys_port
int smp_activate_backend_ports(SMP_INFO* smp_data);
//the function that adds a new sample and gets its buffer from a file to memory
//if succesfull returns the id of the new sample
int smp_add(SMP_INFO *smp_data, const char* samp_path, int in_id);
//process the samples and return summed audio buffer
//uses one callback to get_buffer from the sys_ports and another to get_notes from the midi sys_port
int smp_sample_process_rt(SMP_INFO* smp_data, uint32_t nframes);
//some the cur_smp buffer to out_L and out_R according to the number of channels of cur_smp
static void smp_sum_channel_buffers_rt(SMP_SMP* cur_smp, SAMPLE_T* out_L, SAMPLE_T* out_R,
				       SAMPLE_T mult, int chans);
//param manipulation functions
PRM_CONTAIN* smp_param_return_param_container(SMP_INFO* smp_data, int smp_id);
//set value is a separate function because it needs to send info to param_ui_to_rt ring_buffer
int smp_param_set_value(SMP_INFO* smp_data, int smp_id, int param_id, float param_val, unsigned char param_op);

//copy to new malloced string and return the file path of the sample
char* smp_get_sample_file_path(SMP_INFO* smp_data, int smp_id);
//stop processing the sample and remove it
int smp_stop_and_remove_sample(SMP_INFO* smp_data, int idx);
//clean the memory
int smp_clean_memory(SMP_INFO *smp_data);
