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
    int (*subcx_start_process)(void* user_data, int subcx_id); //called when received a sys message from [main-thread] to start the subcontext process 
    int (*subcx_stop_process)(void* user_data, int subcx_id); //called when received a sys message from [main-thread] to stop the subcontext process 
}CXCONTROL_RT_FUNCS;
//user functions for [main-thread], set NULL if func is not necessary
typedef struct _cxcontrol_ui_funcs{
    int (*send_msg)(void* user_data, const char* msg); //writes a message for the user on the [main-thread]
    int (*subcx_callback)(void* user_data, int subcx_id); //called when received a sys message from [audio-thread] to run a subcontext function on the [main-thread]
    int (*subcx_activate_start_process)(void* user_data, int subcx_id); //called when received a sys message from [audio-thread] to activate the subcontext and then send a message back to [audio-thread] to start processing it
    int (*subcx_restart)(void* user_data, int subcx_id); //called when received a sys message from [audio-thread] to restart the subcontext
}CXCONTROL_UI_FUNCS;

//init the subcontext control struct, create the ui_to_rt and rt_to_ui sys message ring buffers, init the pause semaphore
//also get the user functions for messages from [audio-thread] like Request_callback, Sent_string etc from the rt_funcs_struct and ui_funcs_struct
CXCONTROL* context_sub_init(void* user_data, CXCONTROL_RT_FUNCS rt_funcs_struct, CXCONTROL_UI_FUNCS ui_funcs_struct);

//process the subcontext struct on the [main-thread]
//read the rt_to_ui ring buffer sys messages and execute the user given functions for Request_callback, Sent_string and similar (if they are not null)
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
int context_sub_wait_for_stop(CXCONTROL* cxcontrol_data, int subcx_id);

//send a message to start the subcontext and block the [main-thread], while it starts.
//must be called only on [main-thread]. Needs to wait for start, so that [main-thread] always sends only one message to [audio-thread] that might sem_post
//if an error occures with the user function subcx_start_process the sem_post will still be called, its better to release the semaphore, then deadlock the system on an error
int context_sub_wait_for_start(CXCONTROL* cxcontrol_data, int subcx_id);

//these functions ask the subcx_id to do something - restart for example. If is_audio_thread == 0, the apropriate user function will be called right away (for example subcx_restart)
//otherwise a message will be written to the rt_to_ui_msgs ring buffer to call that function on the [main-thread]
void context_sub_restart_msg(CXCONTROL* cxcontrol_data, int subcx_id, bool is_audio_thread);
void context_sub_send_msg(CXCONTROL* cxcontrol_data, bool is_audio_thread, const char* msg, ...);
void context_sub_activate_start_process_msg(CXCONTROL* cxcontrol_data, int subcx_id, bool is_audio_thread);
void context_sub_callback_msg(CXCONTROL* cxcontrol_data, int subcx_id, bool is_audio_thread);

//clean the subcontext control struct, free ring buffers, destroy the pause semaphore
//the user has to be sure, that the [audio-thread] will not call context_sub_process_rt function when context_sub_clean is called from the [main_thread] 
int context_sub_clean(CXCONTROL* cxcontrol_data);
