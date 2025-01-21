/*
Context control function help with communication between the [main-thread] and the [audio-thread] context.
Without this, each context would have to have almost identical functions to set parameters <Maybe no need to control parameters, leave that on each context??>,
get parameter names when calling from [main-thread]
Also each context would have to have very similar functions to pause their subcontext (for example when plugin context is removing a plugin or clap_plugin context is doing the same).
So these functions are here for convenience. When developing and changing the way [main-thread] communicates with the [audio-thread] there is no need to change the same thing
for each context.
 */
#pragma once

//send a message to stop the subcontext and block the [main-thread], while it stops.
//must be called only on [main-thread]
int context_sub_wait_for_stop();
//send a message to start the subcontext and block the [main-thread], while it starts.
//must be called only on [main-thread]. Needs to wait for start, so that [main-thread] always sends only one message to [audio-thread] that might sem_post
int context_sub_wait_for_start();

