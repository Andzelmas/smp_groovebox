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
If build folder or smp_conf.json inside of it does not exist:

mkdir build in the smp_groovebox folder.
Copy the smp_conf.json file to the build folder.

If you have the dependencies "make" and then "make run" to build and run the software.

The app uses jack2 so dont forget to run it before launching.

### Some screen grabs
The main window with different contexts visible
<img width="520" alt="smp_groovebox_screen_01" src="https://github.com/Andzelmas/smp_groovebox/assets/118473988/26c4dd8c-c630-48f9-8d9d-44aad1c95a73">

The synth context one oscillator controls. Upper right corner box is a scroll bar, beacause not all oscillator controls fit in the window (max 8 parameters in the main window because I'm planning to have 8 encoders).
<img width="522" alt="smp_groovebox_screen_02" src="https://github.com/Andzelmas/smp_groovebox/assets/118473988/bc80d589-8490-430d-a45b-2cfb099fd665">

Playhead controls
<img width="520" alt="smp_groovebox_screen_03" src="https://github.com/Andzelmas/smp_groovebox/assets/118473988/cf85e8b2-ca1b-476b-a2da-f9a1cad4ef2d">



Maintained and Written by Vidmantas Brukstus
