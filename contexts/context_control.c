#include "context_control.h"
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "../util_funcs/ring_buffer.h"
#include "../types.h"

typedef struct _cxcontrol_data{
    //ring buffers for audio-thread and main-thread communication
    RING_BUFFER* rt_to_ui_msgs;
    RING_BUFFER* ui_to_rt_msgs;
    //semaphore when [main-thread] needs to wait for the [audio-thread] to change something (for example stop the plugin or start the plugin)
    //wait on [main-thread] post on [audio-thread], but only when [main-thread] requested - [audio-thread] can post the semaphore only in response to [main-thread] message    
    sem_t pause_for_rt;
    void* user_data;
    CXCONTROL_RT_FUNCS rt_funcs_struct;
    CXCONTROL_UI_FUNCS ui_funcs_struct;
}CXCONTROL;

CXCONTROL* context_sub_init(void* user_data, CXCONTROL_RT_FUNCS rt_funcs_struct, CXCONTROL_UI_FUNCS ui_funcs_struct){
    CXCONTROL* cxcontrol_data = (CXCONTROL*)malloc(sizeof(CXCONTROL));
    if(!cxcontrol_data)return NULL;

    if(sem_init(&cxcontrol_data->pause_for_rt, 0, 0) != 0){
	free(cxcontrol_data);
	return NULL;
    }
    //set the struct to zeroes
    cxcontrol_data->rt_to_ui_msgs = NULL;
    cxcontrol_data->ui_to_rt_msgs = NULL;
    cxcontrol_data->user_data = NULL;

    cxcontrol_data->rt_funcs_struct = rt_funcs_struct;
    cxcontrol_data->ui_funcs_struct = ui_funcs_struct;
    
    //init ring buffers
    cxcontrol_data->rt_to_ui_msgs = ring_buffer_init(sizeof(RING_SYS_MSG), MAX_SYS_BUFFER_ARRAY_SIZE);
    if(!(cxcontrol_data->rt_to_ui_msgs)){
	context_sub_clean(cxcontrol_data);
	return NULL;
    }
    cxcontrol_data->ui_to_rt_msgs = ring_buffer_init(sizeof(RING_SYS_MSG), MAX_SYS_BUFFER_ARRAY_SIZE);
    if(!(cxcontrol_data->ui_to_rt_msgs)){
	context_sub_clean(cxcontrol_data);
	return NULL;
    }

    cxcontrol_data->user_data = user_data;
}
int context_sub_process_ui(CXCONTROL* cxcontrol_data){
    if(!cxcontrol_data)return -1;

    RING_BUFFER* ring_buffer = cxcontrol_data->rt_to_ui_msgs;
    if(!ring_buffer)return -1;
    
    unsigned int cur_items = ring_buffer_return_items(ring_buffer);
    for(unsigned int i = 0; i < cur_items; i++){
	RING_SYS_MSG cur_bit;
	int read_buffer = ring_buffer_read(ring_buffer, &cur_bit, sizeof(cur_bit));
	if(read_buffer <= 0)continue;
	
	if(cur_bit.msg_enum == MSG_PLUGIN_REQUEST_CALLBACK){
	    if(cxcontrol_data->ui_funcs_struct.subcx_callback)cxcontrol_data->ui_funcs_struct.subcx_callback(cxcontrol_data->user_data, cur_bit.scx_id);
	}
	if(cur_bit.msg_enum == MSG_PLUGIN_ACTIVATE_PROCESS){
	    if(cxcontrol_data->ui_funcs_struct.subcx_activate_start_process)cxcontrol_data->ui_funcs_struct.subcx_activate_start_process(cxcontrol_data->user_data, cur_bit.scx_id);
	}
	if(cur_bit.msg_enum == MSG_PLUGIN_RESTART){
	    if(cxcontrol_data->ui_funcs_struct.subcx_restart)cxcontrol_data->ui_funcs_struct.subcx_restart(cxcontrol_data->user_data, cur_bit.scx_id);

	}
	if(cur_bit.msg_enum == MSG_PLUGIN_SENT_STRING){
	    if(cxcontrol_data->ui_funcs_struct.send_msg)cxcontrol_data->ui_funcs_struct.send_msg(cxcontrol_data->user_data, cur_bit.msg);
	}
    }    
}
int context_sub_process_rt(CXCONTROL* cxcontrol_data){
    if(!cxcontrol_data)return -1;

    RING_BUFFER* ring_buffer = cxcontrol_data->ui_to_rt_msgs;
    if(!ring_buffer)return -1;

    unsigned int cur_items = ring_buffer_return_items(ring_buffer);
    for(unsigned int i = 0; i < cur_items; i++){
	RING_SYS_MSG cur_bit;
	int read_buffer = ring_buffer_read(ring_buffer, &cur_bit, sizeof(cur_bit));
	if(read_buffer <= 0)continue;
	if(cur_bit.msg_enum == MSG_PLUGIN_PROCESS){
	    if(cxcontrol_data->rt_funcs_struct.subcx_start_process)cxcontrol_data->rt_funcs_struct.subcx_start_process(cxcontrol_data->user_data, cur_bit.scx_id);
	    //this messages will be sent from [main-thread] only with sam_wait, so error or no error, release the semaphore
	    sem_post(&cxcontrol_data->pause_for_rt);
	}
	if(cur_bit.msg_enum == MSG_PLUGIN_STOP_PROCESS){
	    if(cxcontrol_data->rt_funcs_struct.subcx_stop_process)cxcontrol_data->rt_funcs_struct.subcx_stop_process(cxcontrol_data->user_data, cur_bit.scx_id);
	    //this messages will be sent from [main-thread] only with sam_wait, so error or no error, release the semaphore
	    sem_post(&cxcontrol_data->pause_for_rt);
	}
    }
}
int context_sub_wait_for_stop(CXCONTROL* cxcontrol_data, int subcx_id){
    if(!cxcontrol_data)return -1;
    RING_SYS_MSG send_bit;
    send_bit.msg_enum = MSG_PLUGIN_STOP_PROCESS;
    send_bit.scx_id = subcx_id;
    ring_buffer_write(cxcontrol_data->ui_to_rt_msgs, &send_bit, sizeof(send_bit));
    //lock the [main-thread]
    sem_wait(&cxcontrol_data->pause_for_rt);
}
int context_sub_wait_for_start(CXCONTROL* cxcontrol_data, int subcx_id){
    if(!cxcontrol_data)return -1;
    RING_SYS_MSG send_bit;
    send_bit.msg_enum = MSG_PLUGIN_PROCESS;
    send_bit.scx_id = subcx_id;
    ring_buffer_write(cxcontrol_data->ui_to_rt_msgs, &send_bit, sizeof(send_bit));
    //lock the [main-thread]
    sem_wait(&cxcontrol_data->pause_for_rt);
}
void context_sub_restart_msg(CXCONTROL* cxcontrol_data, int subcx_id, bool is_audio_thread){
    if(!cxcontrol_data) return;

    if(is_audio_thread){
	RING_SYS_MSG send_bit;
	send_bit.msg_enum = MSG_PLUGIN_RESTART;
	send_bit.scx_id = subcx_id;
	ring_buffer_write(cxcontrol_data->rt_to_ui_msgs, &send_bit, sizeof(send_bit));
	return;
    }

    if(cxcontrol_data->ui_funcs_struct.subcx_restart)cxcontrol_data->ui_funcs_struct.subcx_restart(cxcontrol_data->user_data, subcx_id);
}
void context_sub_send_msg(CXCONTROL* cxcontrol_data, bool is_audio_thread, const char* msg, ...){
    if(!cxcontrol_data)return;
    char send_msg[MAX_STRING_MSG_LENGTH];
    va_list args;
    va_start (args, msg);
    vsnprintf(send_msg, MAX_STRING_MSG_LENGTH, msg, args);
    va_end(args);
    if(is_audio_thread){
	RING_SYS_MSG send_bit;
	snprintf(send_bit.msg, MAX_STRING_MSG_LENGTH, "%s", send_msg);
	send_bit.msg_enum = MSG_PLUGIN_SENT_STRING;
	ring_buffer_write(cxcontrol_data->rt_to_ui_msgs, &send_bit, sizeof(send_bit));
	return;
    }

    if(cxcontrol_data->ui_funcs_struct.send_msg)cxcontrol_data->ui_funcs_struct.send_msg(cxcontrol_data->user_data, send_msg);
}
void context_sub_activate_start_process_msg(CXCONTROL* cxcontrol_data, int subcx_id, bool is_audio_thread){
    if(!cxcontrol_data) return;

    if(is_audio_thread){
	RING_SYS_MSG send_bit;
	send_bit.msg_enum = MSG_PLUGIN_ACTIVATE_PROCESS;
	send_bit.scx_id = subcx_id;
	ring_buffer_write(cxcontrol_data->rt_to_ui_msgs, &send_bit, sizeof(send_bit));
	return;
    }

    if(cxcontrol_data->ui_funcs_struct.subcx_activate_start_process)cxcontrol_data->ui_funcs_struct.subcx_activate_start_process(cxcontrol_data->user_data, subcx_id);
}
void context_sub_callback_msg(CXCONTROL* cxcontrol_data, int subcx_id, bool is_audio_thread){
    if(!cxcontrol_data) return;

    if(is_audio_thread){
	RING_SYS_MSG send_bit;
	send_bit.msg_enum = MSG_PLUGIN_REQUEST_CALLBACK;
	send_bit.scx_id = subcx_id;
	ring_buffer_write(cxcontrol_data->rt_to_ui_msgs, &send_bit, sizeof(send_bit));
	return;
    }

    if(cxcontrol_data->ui_funcs_struct.subcx_callback)cxcontrol_data->ui_funcs_struct.subcx_callback(cxcontrol_data->user_data, subcx_id);
}

int context_sub_clean(CXCONTROL* cxcontrol_data){
    if(!cxcontrol_data) return -1;

    if(cxcontrol_data->rt_to_ui_msgs)ring_buffer_clean(cxcontrol_data->rt_to_ui_msgs);
    if(cxcontrol_data->ui_to_rt_msgs)ring_buffer_clean(cxcontrol_data->ui_to_rt_msgs);

    sem_destroy(&cxcontrol_data->pause_for_rt);

    free(cxcontrol_data);
}
