# smp_groovebox
## Simple groovebox for linux based systems. Works on rpi too.

Everything is written in C.
As of today its possible to load lv2 plugins, route audio/midi streams, use a very simple synth and even more simple sampler.

This software uses minimal dependencies and its ui is using ncurses, so it can be used from a terminal and without any graphical environment.
Further more the gui adapts to small screens, so works nicely with rpi touch screens - this was the main catalyst to make this software in the first place.

The lv2 plugin loader uses adapted jalv lv2 host code by David Robillard, check it out here https://gitlab.com/drobilla/jalv

The synth uses oscillator lookup table code by Nigel Redmon here https://www.earlevel.com/main/2012/05/25/a-wavetable-oscillator-the-code/

### Installing
### Dependencies: jack2, libsndfile, json-c, lilv, ncurses
mkdir build in the smp_groovebox folder.
Copy the smp_conf.json file to the build folder.

If you have the dependencies "make" and then "make run" to build and run the software.

Maintained and Written by Vidmantas Brukstus
