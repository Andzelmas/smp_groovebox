/*
Context control function help with communication between the [main-thread] and the [audio-thread] context.
Without this, each context would have to have almost identical functions to set parameters <Maybe no need to control parameters, leave that on each context??>,
get parameter names when calling from [main-thread]
Also each context would have to have very similar functions to pause their subcontext (for example when plugin context is removing a plugin or clap_plugin context is doing the same).
So these functions are here for convenience. When developing and changing the way [main-thread] communicates with the [audio-thread] there is no need to change the same thing
for each context.
 */
#pragma once
#include <stdbool.h>

typedef struct _cxcontrol_data CXCONTROL;
//user functions for [audio-thread], set NULL if func is not necessary
typedef struct _cxcontrol_rt_funcs{
    int (*subcx_start_process)(void* user_data); //called when received a sys message from [main-thread] to start the subcontext process 
    int (*subcx_stop_process)(void* user_data); //called when received a sys message from [main-thread] to stop the subcontext process 
}CXCONTROL_RT_FUNCS;
//user functions for [main-thread], set NULL if func is not necessary
typedef struct _cxcontrol_ui_funcs{
    int (*send_msg)(void* user_data, const char* msg); //writes a message for the user on the [main-thread]
}CXCONTROL_UI_FUNCS;

//init the subcontext control struct, create the ui_to_rt and rt_to_ui sys message ring buffers, init the pause semaphore
//also get the user functions for messages like Sent_string, Plugin_process etc from the rt_funcs_struct and ui_funcs_struct
CXCONTROL* context_sub_init(CXCONTROL_RT_FUNCS rt_funcs_struct, CXCONTROL_UI_FUNCS ui_funcs_struct);

//process the subcontext struct on the [main-thread]
//read the rt_to_ui ring buffer sys messages and execute the user given functions for Sent_string (if not null)
//called only on [main-thread]
int context_sub_process_ui(CXCONTROL* cxcontrol_data);

//process the subcontext struct on the [audio-thread]
//read the ui_to_rt ring buffer sys messages and execute the user given functions for Plugin_process and Plugin_stop_process if they are not null
//after executing one of these functions sem_post the pause semaphore, because [main-thread] will be sem_wait if it sent a message for any of these functions
//called only on [audio-thread]
int context_sub_process_rt(CXCONTROL* cxcontrol_data);

//send a message to stop the subcontext and block the [main-thread], while it stops.
//must be called only on [main-thread]
//if an error occures with the user function subcx_stop_process the sem_post will still be called, its better to release the semaphore, then deadlock the system on an error
int context_sub_wait_for_stop(CXCONTROL* cxcontrol_data, void* user_data);

//send a message to start the subcontext and block the [main-thread], while it starts.
//must be called only on [main-thread]. Needs to wait for start, so that [main-thread] always sends only one message to [audio-thread] that might sem_post
//if an error occures with the user function subcx_start_process the sem_post will still be called, its better to release the semaphore, then deadlock the system on an error
int context_sub_wait_for_start(CXCONTROL* cxcontrol_data, void* user_data);

//send a message for the user. If is_audio_thread == 0, send_msg is called right away - so send_msg must be safe to call from any non [audio-thread]
//otherwise the message is written to the rt_to_ui_msgs ring buffer and send_msg is called with it on the [main-thread]
void context_sub_send_msg(CXCONTROL* cxcontrol_data, void* user_data, bool is_audio_thread, const char* msg, ...);

//clean the subcontext control struct, free ring buffers, destroy the pause semaphore
//the user has to be sure, that the [audio-thread] will not call context_sub_process_rt function when context_sub_clean is called from the [main_thread] 
int context_sub_clean(CXCONTROL* cxcontrol_data);
