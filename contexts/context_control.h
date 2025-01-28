/*
Context control function help with communication between the [main-thread] and the [audio-thread] context.
Without this, each context would have to have almost identical functions to set parameters <Maybe no need to control parameters, leave that on each context??>,
get parameter names when calling from [main-thread]
Also each context would have to have very similar functions to pause their subcontext (for example when plugin context is removing a plugin or clap_plugin context is doing the same).
So these functions are here for convenience. When developing and changing the way [main-thread] communicates with the [audio-thread] there is no need to change the same thing
for each context.
 */
#pragma once

typedef struct _cxcontrol_data CXCONTROL;
//init the subcontext control struct, create the ui_to_rt and rt_to_ui sys message ring buffers, init the pause semaphore
//also get the user functions for messages from [audio-thread] like Request_callback, Sent_string etc. (these are optional)
//and for messages from [main-thread] - Plugin_process and Plugin_stop_process - these are required
CXCONTROL* context_sub_init();
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
int context_sub_wait_for_stop(CXCONTROL* cxcontrol_data, subcx_id);
//send a message to start the subcontext and block the [main-thread], while it starts.
//must be called only on [main-thread]. Needs to wait for start, so that [main-thread] always sends only one message to [audio-thread] that might sem_post
int context_sub_wait_for_start(CXCONTROL* cxcontrol_data, subcx_id);
//clean the subcontext control struct, free ring buffers, destroy the pause semaphore
//the user has to be sure, that the [audio-thread] will not call context_sub_process_rt function when context_sub_clean is called from the [main_thread] 
int context_sub_clean(CXCONTROL* cxcontrol_data);
