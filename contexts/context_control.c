#include "context_control.h"
#include <semaphore.h>
#include <stdlib.h>
#include "../util_funcs/ring_buffer.h"
#include "../types.h"

typedef struct _cxcontrol_data{
    //ring buffers for audio-thread and main-thread communication
    RING_BUFFER* rt_to_ui_msgs;
    RING_BUFFER* ui_to_rt_msgs;
    //semaphore when [main-thread] needs to wait for the [audio-thread] to change something (for example stop the plugin or start the plugin)
    //wait on [main-thread] post on [audio-thread], but only when [main-thread] requested - [audio-thread] can post the semaphore only in response to [main-thread] message    
    sem_t pause_for_rt;
    //user functions for [audio-thread], these are required
    int (*subcx_start_process)(void* user_data, int subcx_id); //called when received a sys message from [main-thread] to start the subcontext process 
    int (*subcx_stop_process)(void* user_data, int subcx_id); //called when received a sys message from [main-thread] to stop the subcontext process 
    //user functions for [main-thread], these are optional
    int (*send_msg)(void* user_data, char* msg); //called when received a sys message from [audio-thread] to write a string message for ui [main-thread]
    int (*subcx_callback)(void* user_data, int subcx_id); //called when received a sys message from [audio-thread] to run a subcontext function on the [main-thread]
    int (*subcx_activate_start_process)(void* user_data, int subcx_id); //called when received a sys message from [audio-thread] to activate the subcontext and then send a message back to [audio-thread] to start processing it
    int (*subcx_restart)(void* user_data, int subcx_id); //called when received a sys message from [audio-thread] to restart the subcontext
}CXCONTROL;

//init the subcontext control struct, create the ui_to_rt and rt_to_ui sys message ring buffers, init the pause semaphore
//also get the user functions for messages from [audio-thread] like Request_callback, Sent_string etc. (these are optional)
//and for messages from [main-thread] - Plugin_process and Plugin_stop_process - these are required
CXCONTROL* context_sub_init(){
    CXCONTROL* cxcontrol_data = (CXCONTROL*)malloc(sizeof(CXCONTROL));
    if(!cxcontrol_data)return NULL;

    if(sem_init(&cxcontrol_data->pause_for_rt, 0, 0) != 0){
	free(cxcontrol_data);
	return NULL;
    }

    cxcontrol_data->rt_to_ui_msgs = NULL;
    cxcontrol_data->ui_to_rt_msgs = NULL;
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
    //TODO init the user functions
    
}
//process the subcontext struct on the [main-thread]
//read the rt_to_ui ring buffer sys messages and execute the user given functions for Request_callback, Sent_string and similar (if they are not null)
//called only on [main-thread]
int context_sub_process_ui();
//process the subcontext struct on the [audio-thread]
//read the ui_to_rt ring buffer sys messages and execute the user given functions for Plugin_process and Plugin_stop_process if they are not null
//after executing one of these functions sem_post the pause semaphore, because [main-thread] will be sem_wait if it sent a message for any of these functions
//called only on [audio-thread]
int context_sub_process_rt();
//send a message to stop the subcontext and block the [main-thread], while it stops.
//must be called only on [main-thread]
//if an error occures with the user function subcx_stop_process the sem_post will still be called, its better to release the semaphore, then deadlock the system on an error
int context_sub_wait_for_stop(CXCONTROL* cxcontrol_data, subcx_id);
//send a message to start the subcontext and block the [main-thread], while it starts.
//must be called only on [main-thread]. Needs to wait for start, so that [main-thread] always sends only one message to [audio-thread] that might sem_post
//if an error occures with the user function subcx_start_process the sem_post will still be called, its better to release the semaphore, then deadlock the system on an error
int context_sub_wait_for_start(CXCONTROL* cxcontrol_data, subcx_id);
//clean the subcontext control struct, free ring buffers, destroy the pause semaphore
//the user has to be sure, that the [audio-thread] will not call context_sub_process_rt function when context_sub_clean is called from the [main_thread] 
int context_sub_clean(CXCONTROL* cxcontrol_data){
    if(!cxcontrol_data) return -1;

    if(cxcontrol_data->rt_to_ui_msgs)ring_buffer_clean(cxcontrol_data->rt_to_ui_msgs);
    if(cxcontrol_data->ui_to_rt_msgs)ring_buffer_clean(cxcontrol_data->ui_to_rt_msgs);

    sem_destroy(&cxcontrol_data->pause_for_rt);

    free(cxcontrol_data);
}
