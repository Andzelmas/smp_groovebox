# smp_groovebox

## Notes when refactor finished
### Ports
Latency is not calculated correctly if the external output ports are connected to the external input ports - a feedback loop is formed.
Also, latency is incorrect if a program connected to the external ports does not report it's latency (has no latency callback function).
Internal ports prohibit forming feedback loops.
### Settings
smp_groovebox searches for a config file in program's own folder/settings.json; $XDG_CONFIG_HOME/smp_groovebox/settings.json and ~/.config/smp_groovebox/settings.json, in that order.
Change to the settings needs a restart.
#### settings.json keys:
parameter general settings
{"params":
Where to save and load the parameter config json files for each plugin, context.
{"param_config_dir": "path_to_param_configs" }
If this is set to a valid dir, the app saves default config files for context parameters (for example for a plugin parameters).
~ is expanded to home for paths. Relative path is relative to the settings file's own folder.
The user can modify those files to change the parameter display names, their value increment steps, add categories, etc.
A saved json file will look like this:
{
    "format": 1,
    "owner": "clap:org.surge-synth-team.surge-xt",
    "load": false,
    "params": {
        "3742": {
            "info": {
                "name": "Osc 1 Pitch",
                "increment": 0.01,
                "default": 0.5,
                "category": "Osc 1"
            }
        }
    }
}
"info": is for the parameters values, and should not be changed. Modify the values, by adding the keys of the values under the "info", like this:
{
    "format": 1,
    "owner": "clap:org.surge-synth-team.surge-xt",
    "load": false,
    "params": {
        "3742": {
            "info": {
                "name": "Osc 1 Pitch",
                "increment": 0.01,
                "default": 0.5,
                "category": "Osc 1"
            },
            "name": "Pitch",
            "category": "Oscillators"
        }
    }
}
user parameter changes will not be loaded unless "load" is set to true
}

plugin settings
{"plugins":
Where to save the clap plugin presets made with "Save preset", a folder per plugin inside.
{"clap_preset_dir": "path_to_clap_presets" }
Defaults to $XDG_DATA_HOME/smp_groovebox/clap_presets, or ~/.local/share/smp_groovebox/clap_presets without XDG_DATA_HOME. "" turns saving clap presets off.
}
### Presets
"Save preset" on a loaded plugin saves its current state as a preset under a name; a preset of the same name is replaced.
Saved presets are listed in the plugin's preset list under "User". A "/" in the name puts the preset into categories there: "keys/piano_1" is listed as User/keys/piano_1.
Since the file name is made safe by replacing weird symbols with "_" "keys/piano_1" and "keys_piano_1" will make a single preset file (the second will overwrite the first).
LV2 presets are saved as bundles in ~/.lv2, where other LV2 hosts find them too (they show the whole name, "keys/piano_1").
Clap has no standard place for presets saved by a host, so they go to the clap_preset_dir folder.

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
