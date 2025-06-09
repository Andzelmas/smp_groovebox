# smp_groovebox
## Still early development. Some parts of code can change completely.
## Simple groovebox for linux based systems. Works on rpi too.
Check out https://brumakes.com/programming.php for development blog and some videos.

Everything is written in C.
As of today its possible to load lv2 plugins, route audio/midi streams, use a very simple synth and even more simple sampler.

Furthermore, CLAP implementation is on its way. Now it is possible to load CLAP plugins, use their audio buffers, send midi events, change parameters and use parameter config files the same way as with lv2 plugins.
Also, can use internal CLAP presets if the plugin supports the preset-factory extension.
Short term still need to implement event input output for all cases and save/load states extension. Other Clap extensions are of lower priority and I will implement them as I go along.

This software uses minimal dependencies and its ui is using ncurses, so it can be used from a terminal and without any graphical environment.
Also, the gui adapts to small screens, so works nicely with rpi touch screens - this was the main catalyst to make this software in the first place.

The lv2 plugin loader uses adapted jalv lv2 host code by David Robillard, check it out here https://gitlab.com/drobilla/jalv

The synth uses oscillator lookup table code by Nigel Redmon here https://www.earlevel.com/main/2012/05/25/a-wavetable-oscillator-the-code/

### Installing
### Dependencies: jack2, libsndfile, json-c, lilv, ncurses, clap

### When all dependencies installed:

- Command "make"
- Run jack2
- "Make run" command or run the smp_sampler in build/ directory to run the application.

### ui_conf.json file in the build directory
Inside users can change some aspects of the user interface.

max_main_contexts - how many contexts are shown in the middle window. Number of parameters for a plugin and similar.

keyboard_shortcuts:

cx_1 ... to cx_22 - shortcuts to select the 1 to 22 context in the middle window.

cx_inc, cx_dec, cx_enter - increase, decrease the selected parameter context value or enter the selected context.

cx_btn_0 ... to cx_btn_4 - shortcuts to enter the button contexts such as load, save, save new, etc. on the button row.

cx_root_0 ... to cx_root_4 - shortcuts to enter the main contexts such as sampler, plugins, synth etc.

cx_exit - exit the current context. When exiting the top context this will exit the program.

### User parameter configuration json files
When first launched the program will create _param_conf.json files for various contexts (for example Synth_param_conf.json).
Also when a plugin is added it will create the _param_conf.json file for that type of plugin.

Some json parameter configuration files exist in the EXTRA/Param_Presets directory for plugins that I use the most. To try them out, copy them into the build directory.

In the parameter files it is possible to change "display_name", "default_val", "increment" keys and the order of the parameters (just move around the parameter blocks).

Display_name is the name that will be shown on the screen, can be duplicates. Delete the display_name line if you want the display name of the parameter to be updated from the parameter data.
This is useful if the plugin can change its parameter names during runtime - for example Surge can change the effects parameter names, depending on what effect type is chosen.

Default_val - value that will be set for the parameter on the first load, if a save file for this context does not exist.

Increment - is by how much the parameter increases or decreases (very useful when the parameter is integer but the developer does not specify that on the parameter itself).

User can also add parameter containers in the config files. If you have this:

```
{
  "Tempo":{
    "type":"0500",
    "display_name":"Tempo",
    "default_val":"100",
    "increment":"1"
  },
  "Bar":{
    "type":"0500",
    "display_name":"Bar",
    "default_val":"1",
    "increment":"1"
  },
  "Beat":{
    "type":"0500",
    "display_name":"Beat",
    "default_val":"1",
    "increment":"1"
  }
}
```

You can add a container like so:

```
{
  "My_Container":{
    "type":"0501",
    "Tempo":{
      "type":"0500",
      "display_name":"Tempo",
      "default_val":"100",
      "increment":"1"
    },
    "Bar":{
      "type":"0500",
      "display_name":"Bar",
      "default_val":"1",
      "increment":"1"
    }
  },
  "Beat":{
    "type":"0500",
    "display_name":"Beat",
    "default_val":"1",
    "increment":"1"
  }
}
```
The name of the container ("My_Container" in the example) has to be unique not only among the containers but also the parameter names (not the display_names).

Also, there has to be a "type":"0501" 
Furthermore mind where to place colons.

If you mess up with the config file, just delete it and a new one will be created.

Like everything else in this software this is tested only superficially. 

### Some screen grabs
The main window with different contexts visible

<img width="520" alt="smp_groovebox_screen_01" src="https://github.com/Andzelmas/smp_groovebox/assets/118473988/26c4dd8c-c630-48f9-8d9d-44aad1c95a73">

Here is the same screen made very small. It is still possible to navigate all the contexts with the scroll bar in the upper right corner

<img width="214" alt="smp_groovebox_screen_small" src="https://github.com/Andzelmas/smp_groovebox/assets/118473988/d5f12b8a-ab83-43c1-8272-5b6e11822f3a">

The synth context one oscillator controls. Upper right corner box is a scroll bar, because not all oscillator controls fit in the window (max 8 parameters in the main window because I'm planning to have 8 encoders).

<img width="522" alt="smp_groovebox_screen_02" src="https://github.com/Andzelmas/smp_groovebox/assets/118473988/bc80d589-8490-430d-a45b-2cfb099fd665">

Playhead controls

<img width="520" alt="smp_groovebox_screen_03" src="https://github.com/Andzelmas/smp_groovebox/assets/118473988/cf85e8b2-ca1b-476b-a2da-f9a1cad4ef2d">



### Maintained and Written by Vidmantas Brukstus
