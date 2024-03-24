#include <lilv-0/lilv/lilv.h>
#include <lv2/state/state.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/midi/midi.h>
#include <lv2/parameters/parameters.h>
#include <lv2/presets/presets.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/patch/patch.h>
#include <lv2/time/time.h>
#include <lv2/options/options.h>
#include <lv2/log/log.h>
#include <lv2/port-groups/port-groups.h>
#include <lv2/port-props/port-props.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/ui/ui.h>
#include <lv2/worker/worker.h>
#include <lv2/data-access/data-access.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
//my libraries
#include "plugins.h"
//my math funcs
#include "../util_funcs/math_funcs.h"
//symap for map to id and back
#include "../util_funcs/jalv/symap.h"
//for workers feature
#include "../util_funcs/jalv/worker.h"
#include "../util_funcs/jalv/zix/sem.h"
//for log functions
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/string_funcs.h"
#include "../jack_funcs/jack_funcs.h"
//a simple macro to get the max of the two values
#ifndef MAX
#  define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
//maximum instances of plugins
#define MAX_INSTANCES 5
//buffer cycles for ui, for example to get the buffer_size we midi_buf_size * N_BUFFER_CYCLES
#define N_BUFFER_CYCLES 4
//default block_length, should be set to the client buffer size
#define DEFAULT_BLOCK_LENGTH 512
//default sample_rate should be set by the audio client
#define DEFAULT_SAMPLE_RATE 48000
//default midi buffer size (and event too)
#define DEFAULT_MIDI_BUF_SIZE 4096U
//the size of the midi containers on the plugins
#define MIDI_CONT_SIZE 50
//lilv nodes for more convinient coding, when for ex we need to tell port class by providing a livnode
typedef struct _plug_nodes{
    LilvNode* atom_AtomPort;
    LilvNode* atom_Chunk;
    LilvNode* atom_Float;
    LilvNode* atom_Path;
    LilvNode* atom_Sequence;
    LilvNode* lv2_AudioPort;
    LilvNode* lv2_CVPort;
    LilvNode* lv2_ControlPort;
    LilvNode* lv2_InputPort;
    LilvNode* lv2_OutputPort;
    LilvNode* lv2_connectionOptional;
    LilvNode* lv2_control;
    LilvNode* lv2_default;
    LilvNode* lv2_enumeration;
    LilvNode* lv2_extensionData;
    LilvNode* lv2_integer;
    LilvNode* lv2_maximum;
    LilvNode* lv2_minimum;
    LilvNode* lv2_name;
    LilvNode* lv2_reportsLatency;
    LilvNode* lv2_sampleRate;
    LilvNode* lv2_symbol;
    LilvNode* lv2_toggled;
    LilvNode* midi_MidiEvent;
    LilvNode* pg_group;
    LilvNode* pprops_logarithmic;
    LilvNode* pprops_notOnGUI;
    LilvNode* pprops_rangeSteps;
    LilvNode* pset_Preset;
    LilvNode* pset_bank;
    LilvNode* rdfs_comment;
    LilvNode* rdfs_label;
    LilvNode* rdfs_range;
    LilvNode* rsz_minimumSize;
    LilvNode* ui_showInterface;
    LilvNode* work_interface;
    LilvNode* work_schedule;
    LilvNode* end;
} PLUG_NODES;

typedef struct _plug_urids{
    LV2_URID atom_Float;
    LV2_URID atom_Int;
    LV2_URID atom_Object;
    LV2_URID atom_Path;
    LV2_URID atom_String;
    LV2_URID atom_eventTransfer;
    LV2_URID bufsz_maxBlockLength;
    LV2_URID bufsz_minBlockLength;
    LV2_URID bufsz_sequenceSize;
    LV2_URID log_Error;
    LV2_URID log_Trace;
    LV2_URID log_Warning;
    LV2_URID midi_MidiEvent;
    LV2_URID param_sampleRate;
    LV2_URID patch_Get;
    LV2_URID patch_Put;
    LV2_URID patch_Set;
    LV2_URID patch_body;
    LV2_URID patch_property;
    LV2_URID patch_value;
    LV2_URID time_Position;
    LV2_URID time_bar;
    LV2_URID time_barBeat;
    LV2_URID time_beatUnit;
    LV2_URID time_beatsPerBar;
    LV2_URID time_beatsPerMinute;
    LV2_URID time_frame;
    LV2_URID time_speed;
    LV2_URID ui_scaleFactor;
    LV2_URID ui_updateRate;
} PLUG_URIDS;

//struct to hold the event data
typedef struct _plug_evbuf_impl{
  uint32_t          capacity;
  uint32_t          atom_Chunk;
  uint32_t          atom_Sequence;
  uint32_t          pad; // So buf has correct atom alignment
  LV2_Atom_Sequence buf;
}PLUG_EVBUF;
//iterator for the PLUG_EVBUF
typedef struct _plug_evbuf_iterator_impl{
    PLUG_EVBUF* evbuf;
    uint32_t offset;
}PLUG_EVBUF_ITERATOR;


//what type of flow the port has - in or out.
enum PortFlow { FLOW_UNKNOWN, FLOW_INPUT, FLOW_OUTPUT };
//por type
enum PortType { TYPE_UNKNOWN, TYPE_CONTROL, TYPE_AUDIO, TYPE_EVENT, TYPE_CV };

typedef struct _plug_port {
    const LilvPort* lilv_port; ///< LV2 port
    enum PortType type;      ///< Data type
    enum PortFlow flow;      ///< Data flow direction
    void* sys_port;  ///< For audio/MIDI ports, otherwise NULL
    PLUG_EVBUF* evbuf;     ///< For MIDI ports, otherwise NULL
    void* widget;    ///< Control widget, if applicable
    size_t buf_size;  ///< Custom buffer size, or 0
    uint32_t index;     ///< Port index
    float control;   ///< For control ports, otherwise 0.0f
    //for convenience we can save the current port urid of its type
    LV2_URID port_type_urid;    
}PLUG_PORT;

typedef struct _plug_features{
    //TODO now the log feature is barebones and very simple, just vprintf or printf
    LV2_Log_Log log;
    LV2_Feature log_feature;
    LV2_Feature map_feature;
    LV2_Feature unmap_feature;
    LV2_State_Make_Path make_path;
    LV2_Feature make_path_feature;
    LV2_Worker_Schedule sched;
    LV2_Feature sched_feature;
    LV2_Worker_Schedule ssched;
    LV2_Feature state_sched_feature;
    LV2_Feature safe_restore_feature;
    LV2_Options_Option options[5];
    LV2_Feature options_feature;
    LV2_Extension_Data_Feature ext_data;
}PLUG_FEATURES;

typedef struct _plug_plug{
    //uri map
    Symap* symap;
    //symap lock
    ZixSem symap_lock;    
    //the plugin data that is used by the instance
    const LilvPlugin* plug;
    //the activated plugin instance
    LilvInstance* plug_instance;
    //the instance id to link it to cx
    int id;
    //audio backend midi container
    JACK_MIDI_CONT* midi_cont;
    //array of available ports for the plugin
    PLUG_PORT* ports;
    //how many ports do we have on plugin
    uint32_t num_ports;
    //index of the controling port
    uint32_t control_in;
    //uri to id and id to uri
    LV2_URID_Map map;
    LV2_URID_Unmap unmap;
    //features and feature list
    PLUG_FEATURES features;
    const LV2_Feature** feature_list;
    //for workers feature
    JalvWorker* worker;
    JalvWorker* state_worker;
    ZixSem work_lock;
    //state, preset safe restore flag
    bool safe_restore;
    //urids of the system, for convenience
    PLUG_URIDS* urids;
    //the current preset
    LilvState* preset;
    //do we need to update, after a preset loading for example
    bool request_update;
    //atom forge for types and general convenience
    LV2_Atom_Forge forge;
}PLUG_PLUG;

typedef struct _plug_info{
    //uri map
    Symap* symap;
    //symap lock
    ZixSem symap_lock;
    //urids
    PLUG_URIDS urids;
    //lilv world object
    LilvWorld* lv_world;
    //array of plugins 
    struct _plug_plug* plugins[MAX_INSTANCES];
    //how many plugins there are
    int total_plugs;
    //the biggest id of a plugin in the system, if its -1, no plugins are active right now
    int max_id;
    //sample_rate
    float sample_rate;
    //block_length
    uint32_t block_length;
    //midi buffer size
    size_t midi_buf_size;
    //buffer size for communication with the plugin
    uint32_t buffer_size;
    //node for convenience
    PLUG_NODES nodes;
    //-----------------------
    //callback functions that should be supplied by the audio backend
    //the address to the audio client
    void* audio_backend;
}PLUG_INFO;

//internal functions to manipulate evbuf buffer for midi and other events
static PLUG_EVBUF* plug_evbuf_new(uint32_t capacity, uint32_t atom_Chunk, uint32_t atom_Sequence){
    const size_t buffer_size = sizeof(PLUG_EVBUF) + sizeof(LV2_Atom_Sequence) + capacity;
    PLUG_EVBUF* evbuf = (PLUG_EVBUF*)malloc(buffer_size);
    if(evbuf){
	memset(evbuf, 0, sizeof(*evbuf));
	evbuf->capacity = capacity;
	evbuf->atom_Chunk = atom_Chunk;
	evbuf->atom_Sequence = atom_Sequence;
    }
    return evbuf;
}

static void plug_evbuf_reset(PLUG_EVBUF* evbuf, unsigned int is_input){
    if(is_input == 1){
	evbuf->buf.atom.size = sizeof(LV2_Atom_Sequence_Body);
	evbuf->buf.atom.type = evbuf->atom_Sequence;
    }
    else{
	evbuf->buf.atom.size = evbuf->capacity;
	evbuf->buf.atom.type = evbuf->atom_Chunk;
    }
}

static void* plug_evbuf_get_buffer(PLUG_EVBUF* evbuf){
    return &(evbuf->buf);
}

static PLUG_EVBUF_ITERATOR plug_evbuf_begin(PLUG_EVBUF* evbuf){
    PLUG_EVBUF_ITERATOR iter = {evbuf, 0};
    return iter;
}

static uint32_t plug_evbuf_get_size(PLUG_EVBUF* evbuf){
    assert(evbuf->buf.atom.type != evbuf->atom_Sequence ||
	   evbuf->buf.atom.size >= sizeof(LV2_Atom_Sequence_Body));
    return evbuf->buf.atom.type == evbuf->atom_Sequence ?
	evbuf->buf.atom.size - sizeof(LV2_Atom_Sequence_Body) : 0;
}

static bool plug_evbuf_is_valid(PLUG_EVBUF_ITERATOR iter){
    return iter.offset < plug_evbuf_get_size(iter.evbuf);
}

static PLUG_EVBUF_ITERATOR plug_evbuf_next(const PLUG_EVBUF_ITERATOR iter){
    if(!plug_evbuf_is_valid(iter)){
	return iter;
    }

    LV2_Atom_Sequence* aseq = &iter.evbuf->buf;
    LV2_Atom_Event* aev = (LV2_Atom_Event*)((char*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, aseq) +
					    iter.offset);
    const uint32_t offset = iter.offset + lv2_atom_pad_size(sizeof(LV2_Atom_Event) + aev->body.size);
    PLUG_EVBUF_ITERATOR next = {iter.evbuf, offset};
    return next;
}

static bool plug_evbuf_get(PLUG_EVBUF_ITERATOR iter, uint32_t* frames, uint32_t* subframes, uint32_t* type,
		    uint32_t* size, void** data){
    *frames = *subframes = *type = *size = 0;
    *data = NULL;

    if(!plug_evbuf_is_valid(iter)){
	return false;
    }

    LV2_Atom_Sequence* aseq = &iter.evbuf->buf;
    LV2_Atom_Event* aev = (LV2_Atom_Event*)((char*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, aseq) +
					    iter.offset);
    *frames = aev->time.frames;
    *subframes = 0;
    *type = aev->body.type;
    *size = aev->body.size;
    *data = LV2_ATOM_BODY(&aev->body);

    return true;
}

static int  plug_evbuf_write(PLUG_EVBUF_ITERATOR* iter, uint32_t frames, uint32_t subframes, uint32_t type,
		      uint32_t size, const void* data){
    (void)subframes;

    LV2_Atom_Sequence* aseq = &iter->evbuf->buf;
    if(iter->evbuf->capacity - sizeof(LV2_Atom) - aseq->atom.size < sizeof(LV2_Atom_Event) + size){
	return -1;
    }

    LV2_Atom_Event* aev = (LV2_Atom_Event*)((char*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, aseq) +
					    iter->offset);

    aev->time.frames = frames;
    aev->body.type = type;
    aev->body.size = size;
    memcpy(LV2_ATOM_BODY(&aev->body), data, size);

    size = lv2_atom_pad_size(sizeof(LV2_Atom_Event) + size);
    aseq->atom.size += size;
    iter->offset += size;

    return 0;
}

//internal function to initialize the nodes
static void plug_init_nodes(LilvWorld* const world, PLUG_NODES* const nodes){
    nodes->atom_AtomPort = lilv_new_uri(world, LV2_ATOM__AtomPort);
    nodes->atom_Chunk = lilv_new_uri(world, LV2_ATOM__Chunk);
    nodes->atom_Float = lilv_new_uri(world, LV2_ATOM__Float);
    nodes->atom_Path = lilv_new_uri(world, LV2_ATOM__Path);
    nodes->atom_Sequence = lilv_new_uri(world, LV2_ATOM__Sequence);
    nodes->lv2_AudioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
    nodes->lv2_CVPort = lilv_new_uri(world, LV2_CORE__CVPort);
    nodes->lv2_ControlPort= lilv_new_uri(world, LV2_CORE__ControlPort);
    nodes->lv2_InputPort = lilv_new_uri(world, LV2_CORE__InputPort);
    nodes->lv2_OutputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
    nodes->lv2_connectionOptional = lilv_new_uri(world, LV2_CORE__connectionOptional);
    nodes->lv2_control = lilv_new_uri(world, LV2_CORE__control);
    nodes->lv2_default = lilv_new_uri(world, LV2_CORE__default);
    nodes->lv2_enumeration = lilv_new_uri(world, LV2_CORE__enumeration);
    nodes->lv2_extensionData = lilv_new_uri(world, LV2_CORE__extensionData);
    nodes->lv2_integer = lilv_new_uri(world, LV2_CORE__integer);
    nodes->lv2_maximum = lilv_new_uri(world, LV2_CORE__maximum);
    nodes->lv2_minimum = lilv_new_uri(world, LV2_CORE__minimum);
    nodes->lv2_name = lilv_new_uri(world, LV2_CORE__name);
    nodes->lv2_reportsLatency = lilv_new_uri(world, LV2_CORE__reportsLatency);
    nodes->lv2_sampleRate = lilv_new_uri(world, LV2_CORE__sampleRate);
    nodes->lv2_symbol = lilv_new_uri(world, LV2_CORE__symbol);
    nodes->lv2_toggled = lilv_new_uri(world, LV2_CORE__toggled);
    nodes->midi_MidiEvent = lilv_new_uri(world, LV2_MIDI__MidiEvent);
    nodes->pg_group = lilv_new_uri(world, LV2_PORT_GROUPS__group);
    nodes->pprops_logarithmic = lilv_new_uri(world, LV2_PORT_PROPS__logarithmic);
    nodes->pprops_notOnGUI = lilv_new_uri(world, LV2_PORT_PROPS__notOnGUI);
    nodes->pprops_rangeSteps = lilv_new_uri(world, LV2_PORT_PROPS__rangeSteps);
    nodes->pset_Preset = lilv_new_uri(world, LV2_PRESETS__Preset);
    nodes->pset_bank = lilv_new_uri(world, LV2_PRESETS__bank);
    nodes->rdfs_comment = lilv_new_uri(world, LILV_NS_RDFS "comment");
    nodes->rdfs_label = lilv_new_uri(world, LILV_NS_RDFS "label");
    nodes->rdfs_range = lilv_new_uri(world, LILV_NS_RDFS "range");
    nodes->rsz_minimumSize = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
    nodes->ui_showInterface = lilv_new_uri(world, LV2_UI__showInterface);
    nodes->work_interface = lilv_new_uri(world, LV2_WORKER__interface);
    nodes->work_schedule = lilv_new_uri(world, LV2_WORKER__schedule);
    nodes->end = NULL;
}
//internal function to initialize urids
static void plug_init_urids(Symap* const symap, PLUG_URIDS* const urids){
    urids->atom_Float = symap_map(symap, LV2_ATOM__Float);
    urids->atom_Int = symap_map(symap, LV2_ATOM__Int);
    urids->atom_Object = symap_map(symap, LV2_ATOM__Object);
    urids->atom_Path = symap_map(symap, LV2_ATOM__Path);
    urids->atom_String = symap_map(symap, LV2_ATOM__String);
    urids->atom_eventTransfer = symap_map(symap, LV2_ATOM__eventTransfer);
    urids->bufsz_maxBlockLength = symap_map(symap, LV2_BUF_SIZE__maxBlockLength);
    urids->bufsz_minBlockLength = symap_map(symap, LV2_BUF_SIZE__minBlockLength);
    urids->bufsz_sequenceSize = symap_map(symap, LV2_BUF_SIZE__sequenceSize);
    urids->log_Error = symap_map(symap, LV2_LOG__Error);
    urids->log_Trace = symap_map(symap, LV2_LOG__Trace);
    urids->log_Warning = symap_map(symap, LV2_LOG__Warning);
    urids->midi_MidiEvent = symap_map(symap, LV2_MIDI__MidiEvent);
    urids->param_sampleRate = symap_map(symap, LV2_PARAMETERS__sampleRate);
    urids->patch_Get = symap_map(symap, LV2_PATCH__Get);
    urids->patch_Put = symap_map(symap, LV2_PATCH__Put);
    urids->patch_Set = symap_map(symap, LV2_PATCH__Set);
    urids->patch_body = symap_map(symap, LV2_PATCH__body);
    urids->patch_property = symap_map(symap, LV2_PATCH__property);
    urids->patch_value = symap_map(symap, LV2_PATCH__value);
    urids->time_Position = symap_map(symap, LV2_TIME__Position);
    urids->time_bar = symap_map(symap, LV2_TIME__bar);
    urids->time_barBeat = symap_map(symap, LV2_TIME__barBeat);
    urids->time_beatUnit = symap_map(symap, LV2_TIME__beatUnit);
    urids->time_beatsPerBar = symap_map(symap, LV2_TIME__beatsPerBar);
    urids->time_beatsPerMinute = symap_map(symap, LV2_TIME__beatsPerMinute);
    urids->time_frame = symap_map(symap, LV2_TIME__frame);
    urids->time_speed = symap_map(symap, LV2_TIME__speed);
    urids->ui_scaleFactor = symap_map(symap, LV2_UI__scaleFactor);
    urids->ui_updateRate = symap_map(symap, LV2_UI__updateRate);

}
//internal function to map uri to id, this will be used in a feature sent to the plugin
static LV2_URID map_uri(LV2_URID_Map_Handle handle, const char* uri){
  PLUG_INFO* plug_data = (PLUG_INFO*)handle;
  zix_sem_wait(&(plug_data->symap_lock));
  const LV2_URID id = symap_map(plug_data->symap, uri);
  zix_sem_post(&(plug_data->symap_lock));
  return id;
}
//internal function to map id to uri, this will be used in a feature sent to the plugin
static const char* unmap_uri(LV2_URID_Unmap_Handle handle, LV2_URID urid){
  PLUG_INFO* plug_data = (PLUG_INFO*)handle;
  zix_sem_wait(&(plug_data->symap_lock));  
  const char* uri = symap_unmap(plug_data->symap, urid);
  zix_sem_post(&(plug_data->symap_lock));  
  return uri;
}
//internal function for printf for the log feature
static int plug_vprintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, va_list ap){
    PLUG_INFO* const plug_data = (PLUG_INFO*)handle;
    return vfprintf(stderr, fmt, ap);
}
static int plug_printf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, ...){
    va_list args;
    va_start(args, fmt);
    const int ret = plug_vprintf(handle, type, fmt, args);
    va_end(args);
    return ret;
}
PLUG_INFO* plug_init(uint32_t block_length, SAMPLE_T samplerate,
		     plug_status_t* plug_errors,
		     void* audio_backend){
    PLUG_INFO* plug_data = (PLUG_INFO*)malloc(sizeof(PLUG_INFO));
    memset(plug_data, '\0', sizeof(*plug_data));
    if(!plug_data){
	*plug_errors = plug_failed_malloc;
	return NULL;
    }
    plug_data->lv_world = lilv_world_new();
    //init the plugin instances to 0
    for(int i = 0; i<MAX_INSTANCES; i++){
	plug_data->plugins[i] = NULL;
    }
    plug_data->max_id = -1;
    plug_data->total_plugs = 0;
    plug_data->sample_rate = (float)samplerate;
    plug_data->block_length = (uint32_t)block_length;
    plug_data->midi_buf_size = (size_t)DEFAULT_MIDI_BUF_SIZE;
    plug_data->buffer_size = plug_data->midi_buf_size * N_BUFFER_CYCLES;
    
    lilv_world_load_all(plug_data->lv_world);    
    plug_data->symap = symap_new();
    zix_sem_init(&(plug_data->symap_lock), 1);    
    plug_init_urids(plug_data->symap, &(plug_data->urids));
    plug_init_nodes(plug_data->lv_world, &(plug_data->nodes));

    plug_data->audio_backend = audio_backend;
    
    return plug_data;
}

char* plug_return_plugin_name(PLUG_INFO* plug_data, int plug_id){
    if(!plug_data)return NULL;
    if(plug_data->max_id < 0)return NULL;
    if(plug_id > plug_data->max_id)return NULL;

    PLUG_PLUG* cur_plug = plug_data->plugins[plug_id];
    if(!cur_plug)return NULL;

    char* ret_string = NULL;
    
    const LilvNode* name_node = lilv_plugin_get_uri(cur_plug->plug);
    const char* name_string = lilv_node_as_string(name_node);

    ret_string = (char*)malloc(sizeof(char) * (strlen(name_string)+1));
    if(!ret_string)return NULL;
    strcpy(ret_string, name_string);
    
    return ret_string;
}

char** plug_return_plugin_names(PLUG_INFO* plug_data){
    if(!plug_data)return NULL;
    const LilvPlugins* plugins = lilv_world_get_all_plugins(plug_data->lv_world);
    if(!plugins)return NULL;
    LilvIter* plug_iter = lilv_plugins_begin(plugins);
    char** name_array = NULL;
    unsigned int name_size = lilv_plugins_size(plugins)+1;
    if(name_size<=0)return NULL;
    name_array = (char**)malloc(sizeof(char*)*(name_size));
    if(!name_array)return NULL;
    for(int i = 0; i<name_size; i++){
	name_array[i] = NULL;
    }
    int iter = 0;
    while(!lilv_plugins_is_end(plugins, plug_iter)){
	const LilvPlugin* cur_plug = lilv_plugins_get(plugins, plug_iter);
	const LilvNode* cur_name = lilv_plugin_get_uri(cur_plug);
	const char* name_string = lilv_node_as_string(cur_name);
	char* save_string = (char*)malloc(sizeof(char)*(strlen(name_string)+1));
	if(save_string){
	    strcpy(save_string, name_string);
	    name_array[iter] = save_string;
	}
	plug_iter = lilv_plugins_next(plugins, plug_iter);
	iter+=1;
    }
    
    return name_array;
}

char** plug_return_plugin_presets_names(PLUG_INFO* plug_data, unsigned int indx){
    if(!plug_data)return NULL;
    if(!plug_data->plugins)return NULL;
    PLUG_PLUG* plug = plug_data->plugins[indx];
    if(!plug)return NULL;
    LilvNodes* presets = lilv_plugin_get_related(plug->plug, plug_data->nodes.pset_Preset);
    if(!presets)return NULL;
    LilvIter* preset_iter = lilv_nodes_begin(presets);
    char** preset_name_array = NULL;
    unsigned int name_size = lilv_nodes_size(presets)+1;
    if(name_size<=0)return NULL;
    preset_name_array = (char**)malloc(sizeof(char*)*(name_size));
    if(!preset_name_array)return NULL;
    for(int i = 0; i<name_size; i++){
	preset_name_array[i] = NULL;
    }

    unsigned int iter = 0;
    while(!lilv_nodes_is_end(presets, preset_iter)){
	const LilvNode* cur_node = lilv_nodes_get(presets, preset_iter);
	const char* cur_name = lilv_node_as_string(cur_node);
	if(cur_name){
	    char* save_name = (char*)malloc(sizeof(char)*(strlen(cur_name)+1));
	    if(save_name){
		strcpy(save_name, cur_name);
		preset_name_array[iter] = save_name;
	    }
	}
	
	preset_iter = lilv_nodes_next(presets, preset_iter);
	iter+=1;
    }
    
    lilv_nodes_free(presets);
    return preset_name_array;
}

int plug_load_and_activate(PLUG_INFO* plug_data, const char* plugin_uri, const int id){
    int return_val = -1;
    if(!plug_data){
	return_val = -1;
	goto done;
    }
    if(!plug_data->lv_world){
	return_val = -1;
	goto done;
    }
    if(!plugin_uri){
	return_val = -1;
	goto done;
    }
    if(plug_data->total_plugs >= MAX_INSTANCES)goto done;
    
    const LilvPlugin* plugins = lilv_world_get_all_plugins(plug_data->lv_world);
    LilvNode* name_uri = lilv_new_uri(plug_data->lv_world, plugin_uri);
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins, name_uri);
    lilv_node_free(name_uri);    
    if(!plugin){
	return_val = -1;
	goto done;
    }
    //malloc fill the members of the plugin
    PLUG_PLUG* plug = (PLUG_PLUG*)malloc(sizeof(PLUG_PLUG));
    if(!plug){
	return_val = -1;
	goto done;
    }
    plug->midi_cont = NULL;
    plug->plug = NULL;
    plug->plug_instance = NULL;
    plug->plug = plugin;
    plug->feature_list = NULL;
    plug->worker = NULL;
    plug->state_worker = NULL;   
    plug->preset = NULL;
    plug->request_update = false;
    zix_sem_init(&(plug->work_lock), 1);
    //Check for thread-safe state restore() method
    plug->safe_restore = false;
    LilvNode* state_threadSafeRestore = lilv_new_uri(plug_data->lv_world, LV2_STATE__threadSafeRestore);
    if(lilv_plugin_has_feature(plug->plug, state_threadSafeRestore)){
	plug->safe_restore = true;
    }
    lilv_node_free(state_threadSafeRestore);
    
    //initialize the plug urids, they are the same as plug_data, used for convenience
    plug->urids = &(plug_data->urids);
    //init the features
    //log feature
    plug->features.log.handle = plug_data;
    plug->features.log.printf = plug_printf;
    plug->features.log.vprintf = plug_vprintf;
    plug->features.log_feature.URI = LV2_LOG__log;
    plug->features.log_feature.data =  &(plug->features.log);
    //state:threadSafeRestore
    plug->features.safe_restore_feature.URI = LV2_STATE__threadSafeRestore;
    plug->features.safe_restore_feature.data = NULL;
    //map to id feature
    plug->map.handle = plug_data;
    plug->map.map = map_uri;
    plug->features.map_feature.URI = LV2_URID__map;
    plug->features.map_feature.data = &(plug->map);
    //map id to uri feature
    plug->unmap.handle = plug_data;
    plug->unmap.unmap = unmap_uri;
    plug->features.unmap_feature.URI = LV2_URID__unmap;
    plug->features.unmap_feature.data = &(plug->unmap);
    //worker features
    plug->features.sched.schedule_work = jalv_worker_schedule;
    plug->features.sched_feature.URI = LV2_WORKER__schedule;
    plug->features.sched_feature.data = &(plug->features.sched);
    plug->features.ssched.schedule_work = jalv_worker_schedule;
    plug->features.state_sched_feature.URI = LV2_WORKER__schedule;
    plug->features.state_sched_feature.data = &(plug->features.ssched);
    //init options features
    const LV2_Options_Option options[(sizeof(plug->features.options))/
				      (sizeof(plug->features.options[0]))] = {
	{LV2_OPTIONS_INSTANCE, 0, plug_data->urids.param_sampleRate, sizeof(float), plug_data->urids.atom_Float,
	 &(plug_data->sample_rate)},
	
	{LV2_OPTIONS_INSTANCE, 0, plug_data->urids.bufsz_minBlockLength, sizeof(int32_t),
	 plug_data->urids.atom_Int, &(plug_data->block_length)},
	
	{LV2_OPTIONS_INSTANCE, 0, plug_data->urids.bufsz_maxBlockLength, sizeof(int32_t),
	 plug_data->urids.atom_Int, &(plug_data->block_length)},
	
	{LV2_OPTIONS_INSTANCE, 0, plug_data->urids.bufsz_sequenceSize, sizeof(int32_t),
	 plug_data->urids.atom_Int, &(plug_data->midi_buf_size)},
	
	{LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL}	
    };
    memcpy(plug->features.options, options, sizeof(plug->features.options));
    plug->features.options_feature.URI = LV2_OPTIONS__options;
    plug->features.options_feature.data = (void*)plug->features.options;
    //init features that have no data
    const LV2_Feature static_features[] = {{LV2_STATE__loadDefaultState, NULL},
						  {LV2_BUF_SIZE__powerOf2BlockLength, NULL},
						  {LV2_BUF_SIZE__fixedBlockLength, NULL},
						  {LV2_BUF_SIZE__boundedBlockLength, NULL}
    };
    //build the features list to pass to plugins
    const LV2_Feature* const features[] = {&(plug->features.map_feature),
	&(plug->features.unmap_feature), &(plug->features.sched_feature),
	&(plug->features.state_sched_feature),
	&(plug->features.log_feature), &(plug->features.safe_restore_feature),
	&(plug->features.options_feature),
	&static_features[0], &static_features[1],
	&static_features[2], &static_features[3], NULL};
    
    plug->feature_list = (const LV2_Feature**)calloc(1, sizeof(features));
    if(!plug->feature_list){
	plug_remove_plug(plug_data, plug->id);
	return_val = -1;
	goto done;
    }
    memcpy(plug->feature_list, features, sizeof(features));

    //init atom forge
    lv2_atom_forge_init(&(plug->forge), &(plug->map));
    
    //instantiate and activate the plugin
    plug->plug_instance = lilv_plugin_instantiate(plugin, plug_data->sample_rate, plug->feature_list);
    
    //Create workers if necessary
    if(lilv_plugin_has_extension_data(plug->plug, plug_data->nodes.work_interface)){
	plug->worker = jalv_worker_new(&(plug->work_lock), true);
	plug->features.sched.handle = plug->worker;
	if(plug->safe_restore){
	    plug->state_worker = jalv_worker_new(&(plug->work_lock), false);
	    plug->features.ssched.handle = plug->state_worker;
	}
    }
    //somethings need to get instance
    plug->features.ext_data.data_access = lilv_instance_get_descriptor(plug->plug_instance)->extension_data;
    const LV2_Worker_Interface* worker_iface =
	(const LV2_Worker_Interface*)lilv_instance_get_extension_data(plug->plug_instance,
								      LV2_WORKER__interface);
    jalv_worker_start(plug->worker, worker_iface, plug->plug_instance->lv2_handle);
    jalv_worker_start(plug->state_worker, worker_iface, plug->plug_instance->lv2_handle);
       
    //add the plugin to the plugin array
    return_val = plug_add_plug_to_array(plug_data, plug, id);
    if(return_val == -1){
	plug_remove_plug(plug_data, plug->id);
	goto done;
    }
    plug->id = return_val;
    plug_data->total_plugs += 1;
    if(return_val > plug_data->max_id){
	plug_data->max_id = return_val;
    }
 
    //create the ports on audio_client from the sys_ports
    plug_activate_backend_ports(plug_data, plug);

    //--------------------------------------------------
    //create controls if they are properties and not ports
    plug_create_properties(plug_data, plug, true);
    plug_create_properties(plug_data, plug, false);    
    //--------------------------------------------------

    plug->midi_cont = app_jack_init_midi_cont(MIDI_CONT_SIZE);
    if(!plug->midi_cont){
	plug_remove_plug(plug_data, plug->id);
	return -1;
    }
    
    //activate the plugin instance
    lilv_instance_activate(plug->plug_instance);
    
done:
    return return_val;
}

int plug_load_preset(PLUG_INFO* plug_data, unsigned int plug_id, const char* preset_name){
    if(!plug_data)return -1;
    if(!plug_data->plugins)return -1;
    if(!preset_name)return -1;
    int return_val = 0;
    PLUG_PLUG* plug = (PLUG_PLUG*)plug_data->plugins[plug_id];
    if(!plug)return -1;
    if(plug->preset){
	const LilvNode* old_preset = lilv_state_get_uri(plug->preset);
	if(old_preset){
	    lilv_world_unload_resource(plug_data->lv_world, old_preset);
	}
	lilv_state_free(plug->preset);
	plug->preset = NULL;
    }
    LilvNode* new_preset = lilv_new_uri(plug_data->lv_world, preset_name);
    int load_err = lilv_world_load_resource(plug_data->lv_world, new_preset);
    if(load_err<0){
	if(new_preset)free(new_preset);
	return -1;
    }
    return_val = load_err;
    plug->preset = lilv_state_new_from_world(plug_data->lv_world, &(plug->map), new_preset);

    //TODO we could check here if the plugin allows safe_restore, if yes we would not need to pause
    //the rt process. Though since the process is stopped on app_data level, we would need to split this into
    //several steps - on app_data ask if the plugin supports safe_restore if yes, no need to pause the
    //process, if no we pause the process.
    const LV2_Feature* state_features[9] = {
	&(plug->features.map_feature),
	&(plug->features.unmap_feature),
	&(plug->features.make_path_feature),
	&(plug->features.state_sched_feature),
	&(plug->features.safe_restore_feature),
	&(plug->features.log_feature),
	&(plug->features.options_feature),
	NULL};

    //TODO here depending if the plug has safe_restore or does not we send different set port values functions
    //one sets the values directly (if there is no safe_restore, and the rt process is paused), the other uses
    //circle buffers and is the general function to set values on the plugin
    //TODO for some reason plugin preset loading crashes when sending features to the livl_state_restore
    //TODO need to find out which feature is not loaded correctly and crashes
    lilv_state_restore(plug->preset, plug->plug_instance, plug_set_value_direct, plug, 0, NULL);
    
    if(new_preset)lilv_node_free(new_preset);
    plug->request_update = true;
    return return_val;
}

static void plug_set_value_direct(const char* port_symbol,
				  void* data,
				  const void* value,
				  uint32_t size,
				  uint32_t type){
    PLUG_PLUG* plug = (PLUG_PLUG*) data;
    if(!plug) return;
    PLUG_PORT* port = plug_find_port_by_name(plug, port_symbol);
    if(!port)return;
    float fvalue = 0.0f;
    if(type == plug->forge.Float){
	fvalue = *(const float*)value;
    }
    else if(type == plug->forge.Double){
	fvalue = *(const double*)value;
    }
    else if(type == plug->forge.Int){
	fvalue = *(const int32_t*)value;
    }
    else if(type == plug->forge.Long){
	fvalue = *(const int64_t*)value;
    }
    else{
	return;
    }

    port->control = fvalue;
}

static PLUG_PORT* plug_find_port_by_name(PLUG_PLUG* plug, const char* name){
    if(!plug)return NULL;
    if(!name)return NULL;
    for(uint32_t i = 0; i < plug->num_ports; ++i){
	PLUG_PORT* const cur_port = &(plug->ports[i]);
	if(!cur_port)continue;
	const LilvNode* port_name = lilv_port_get_symbol(plug->plug, cur_port->lilv_port);
	if(strcmp(lilv_node_as_string(port_name), name) == 0){
	    return cur_port;
	}
    }
    return NULL;
}

//TODO does nothing right now
static void plug_create_properties(PLUG_INFO* plug_data, PLUG_PLUG* plug, bool writable){
    const LilvPlugin* plugin = plug->plug;
    LilvWorld* world = plug_data->lv_world;
    LilvNode* patch_writable = lilv_new_uri(world, LV2_PATCH__writable);
    LilvNode* patch_readable = lilv_new_uri(world,  LV2_PATCH__readable);

    LilvNodes* properties = lilv_world_find_nodes(world, lilv_plugin_get_uri(plugin),
						  writable ? patch_writable: patch_readable, NULL);
    LILV_FOREACH(nodes, p, properties){
	const LilvNode* property = lilv_nodes_get(properties, p);
	log_append_logfile("property %s writable %d\n", lilv_node_as_string(property), writable);
	log_append_logfile("default %g\n", lilv_node_as_float(lilv_world_get(world, property, plug_data->nodes.lv2_default, NULL)));
	log_append_logfile("label %s\n", lilv_node_as_string(lilv_world_get(world, property, plug_data->nodes.rdfs_label, NULL)));
       
    }
    lilv_nodes_free(properties);
    lilv_node_free(patch_readable);
    lilv_node_free(patch_writable);
}

void plug_set_samplerate(PLUG_INFO* plug_data, float new_sample_rate){
    if(!plug_data)goto finish;
    plug_data->sample_rate = new_sample_rate;
finish:
}

void plug_set_block_length(PLUG_INFO* plug_data, uint32_t new_block_length){
    if(!plug_data)goto finish;
    plug_data->block_length = new_block_length;
finish:    
}

void plug_activate_backend_ports(PLUG_INFO* plug_data, PLUG_PLUG* plug){
    if(!plug_data)goto finish;
    if(!plug)goto finish;
    if(!plug_data->audio_backend)goto finish;
    const LV2_URID atom_Chunk = plug->map.map(plug->map.handle,
						   lilv_node_as_string(plug_data->nodes.atom_Chunk));
    const LV2_URID atom_Sequence = plug->map.map(plug->map.handle,
						      lilv_node_as_string(plug_data->nodes.atom_Sequence));    
    //--------------------------------------------------
    //initialize the ports
    //TODO now ignores the latency port
    plug->num_ports = lilv_plugin_get_num_ports(plug->plug);
    plug->ports = (PLUG_PORT*)calloc(plug->num_ports, sizeof(PLUG_PORT));
    if(plug->ports==NULL){
	plug_remove_plug(plug_data, plug->id);
	goto finish;
    }
    float* default_values = (float*)calloc(plug->num_ports, sizeof(float));
    if(!default_values){
	plug_remove_plug(plug_data, plug->id);
	goto finish;
    }    
    lilv_plugin_get_port_ranges_float(plug->plug, NULL, NULL, default_values);
   
    for(uint32_t i = 0; i< plug->num_ports; i++){
	PLUG_PORT* const cur_port = &(plug->ports[i]);
	cur_port->lilv_port = lilv_plugin_get_port_by_index(plug->plug, i);
	cur_port->sys_port = NULL;
	cur_port->evbuf = NULL;
	cur_port->buf_size = 0;
	cur_port->index = i;
	cur_port->control = 0.0f;
	cur_port->flow = FLOW_UNKNOWN;
	cur_port->type = TYPE_UNKNOWN;
	cur_port->port_type_urid = 0;
	const LilvNode* sym = lilv_port_get_symbol(plug->plug, cur_port->lilv_port);
	const unsigned int optional = lilv_port_has_property(plug->plug, cur_port->lilv_port,
							     plug_data->nodes.lv2_connectionOptional);
	//build full port name if this is not a control port, so that the port name is unique on
	//the audio backend
	//--------------------------------------------------
	const LilvNode* plug_name_node = lilv_plugin_get_uri(plug->plug);
	const char* plug_uri_name = lilv_node_as_string(plug_name_node);
	char* plug_name = str_return_file_from_path(plug_uri_name);
	if(!plug_name){
	    plug_name = (char*)malloc(sizeof(char)*(strlen(plug_uri_name)+1));
	    strcpy(plug_name, plug_uri_name);
	}
	char* full_port_name = (char*)malloc(sizeof(char)*(strlen(plug_name)+4));
	if(full_port_name){
	    sprintf(full_port_name,"%.2d-", plug->id);
	    strcat(full_port_name, plug_name);
	    strtok(full_port_name, " ");
	    char* temp_string = realloc(full_port_name, sizeof(char)*(strlen(full_port_name)+
								      strlen(lilv_node_as_string(sym))+2));
	    if(temp_string){
		full_port_name = temp_string;
		strcat(full_port_name, "|");
		strcat(full_port_name, lilv_node_as_string(sym));
	    }
	}
	if(plug_name)free(plug_name);
	//--------------------------------------------------
	unsigned int port_io = 0x2;	
	//set if the port is input or output
	if(lilv_port_is_a(plug->plug, cur_port->lilv_port, plug_data->nodes.lv2_InputPort)){
	    cur_port->flow = FLOW_INPUT;
	    port_io = 0x1;	    
	}
	else if(lilv_port_is_a(plug->plug, cur_port->lilv_port, plug_data->nodes.lv2_OutputPort)){
	    cur_port->flow = FLOW_OUTPUT;
	}
	else if(!optional){
	    plug_remove_plug(plug_data, plug->id);
	    goto finish;
	}	
	//Set port types
	//control port
	if(lilv_port_is_a(plug->plug, cur_port->lilv_port, plug_data->nodes.lv2_ControlPort)){
	    cur_port->type = TYPE_CONTROL;
	    cur_port->control = isnan(default_values[i]) ? 0.0f : default_values[i];
	    //connect the port to its value
	    lilv_instance_connect_port(plug->plug_instance, i, &(cur_port->control));
	    //TODO here we should add a control struct that holds min and max values and etc. for the gui
	    LilvNode* port_name_node = lilv_port_get_name(plug->plug, cur_port->lilv_port);
	    if(port_name_node){
		log_append_logfile("control %s\n", lilv_node_as_string(port_name_node));
		lilv_node_free(port_name_node);
	    }
	}
	else if (lilv_port_is_a(plug->plug, cur_port->lilv_port, plug_data->nodes.lv2_AudioPort)){
	    cur_port->type = TYPE_AUDIO;
	    cur_port->sys_port = app_jack_create_port_on_client(plug_data->audio_backend, 0, port_io, full_port_name);
	}
	else if(lilv_port_is_a(plug->plug, cur_port->lilv_port, plug_data->nodes.lv2_CVPort)){
	    cur_port->type = TYPE_CV;
	    cur_port->sys_port = app_jack_create_port_on_client(plug_data->audio_backend, 0, port_io, full_port_name);
	}
	else if(lilv_port_is_a(plug->plug, cur_port->lilv_port, plug_data->nodes.atom_AtomPort)){
	    cur_port->type = TYPE_EVENT;
	    if(lilv_port_supports_event(plug->plug, cur_port->lilv_port, plug_data->nodes.midi_MidiEvent)) {
		cur_port->port_type_urid = plug_data->urids.midi_MidiEvent;
		cur_port->sys_port = app_jack_create_port_on_client(plug_data->audio_backend, 1, port_io, full_port_name);
	    }
	    //ready the evbuf for the events
	    //--------------------
	    free(cur_port->evbuf);
	    const size_t size = cur_port->buf_size ? cur_port->buf_size : plug_data->midi_buf_size;
	    cur_port->evbuf = plug_evbuf_new(size, atom_Chunk, atom_Sequence);
	    lilv_instance_connect_port(plug->plug_instance, i, plug_evbuf_get_buffer(cur_port->evbuf));
	    unsigned int is_input = 0;
	    if(cur_port->flow == FLOW_INPUT)is_input = 1;
	    plug_evbuf_reset(cur_port->evbuf, is_input);
	    //--------------------	    
	}
	//if not optional but we dont know the type we cant load this plugin
	else if(!optional){
	    plug_remove_plug(plug_data, plug->id);
	    goto finish;	    
	}
	//if the type is unknown connect to null
	if(cur_port->type == FLOW_UNKNOWN || cur_port->type == TYPE_UNKNOWN){
	    lilv_instance_connect_port(plug->plug_instance, i, NULL);
	}	
	//set the port buffer size
	LilvNode* min_size = lilv_port_get(plug->plug, cur_port->lilv_port, plug_data->nodes.rsz_minimumSize);
	if(min_size && lilv_node_is_int(min_size)){
	    cur_port->buf_size = lilv_node_as_int(min_size);
	    plug_data->buffer_size = MAX(plug_data->buffer_size, cur_port->buf_size * N_BUFFER_CYCLES);
	}
	lilv_node_free(min_size);
	if(full_port_name)free(full_port_name);
    }	
    //find the controling port index
    const LilvPort* control_input = lilv_plugin_get_port_by_designation(plug->plug, plug_data->nodes.lv2_InputPort,
									plug_data->nodes.lv2_control);
    if(control_input){
	const uint32_t index = lilv_port_get_index(plug->plug, control_input);
	if(plug->ports[index].type == TYPE_EVENT){
	    plug->control_in = index;
	}
    }
    free(default_values);


finish:
}

void** plug_return_sys_ports(PLUG_INFO* plug_data, unsigned int plug_id, unsigned int* number_ports){
    if(!plug_data)return NULL;
    if(plug_id>plug_data->max_id)return NULL;
    PLUG_PLUG* cur_plug = plug_data->plugins[plug_id];
    if(!cur_plug)return NULL;
    int port_num = cur_plug->num_ports;
    if(port_num<=0)return NULL;
    if(number_ports)*number_ports = port_num;
    void** ret_sys_ports = (void**)malloc(sizeof(void*) * port_num);
    if(!ret_sys_ports)return NULL;
    for(int i = 0; i<port_num; i++){
	ret_sys_ports[i] = NULL;
	PLUG_PORT cur_port = cur_plug->ports[i];
	void* cur_sys_port = cur_port.sys_port;
	if(cur_sys_port){
	    ret_sys_ports[i] = cur_sys_port;
	}
    }
    return ret_sys_ports;
}

void plug_process_data_rt(PLUG_INFO* plug_data, unsigned int nframes){
    if(!plug_data)return;    
    if(!plug_data->plugins)return;    
    if(plug_data->max_id<0)return;
    for(int id = 0; id < plug_data->max_id+1; id++){
	PLUG_PLUG* plug = plug_data->plugins[id];
	if(!plug)continue;
	if(!plug->ports)continue;
	//----------------------------------------------------------------------------------------------------
	//first connect the ports for processing
	for(uint32_t i = 0; i< plug->num_ports; i++){
	    PLUG_PORT* const cur_port = &(plug->ports[i]);
	    //if there is a sys_port get the buffer from it
	    void* a_buffer = NULL;
	    if(cur_port->sys_port)
		a_buffer = app_jack_get_buffer_rt(cur_port->sys_port, nframes);
	    if(cur_port->type == TYPE_AUDIO){
		lilv_instance_connect_port(plug->plug_instance, i, a_buffer);
	    }
	    if(cur_port->type == TYPE_CV){
		lilv_instance_connect_port(plug->plug_instance, i, a_buffer);	    
	    }
	    if(cur_port->type == TYPE_CONTROL && cur_port->flow == FLOW_INPUT){
		//TODO update control ports from gui (update the parameter values)
	    }
	    if(cur_port->type == TYPE_EVENT && cur_port->flow == FLOW_INPUT){
		//clean the evbuf
		plug_evbuf_reset(cur_port->evbuf, 1);
		PLUG_EVBUF_ITERATOR iter_buf = plug_evbuf_begin(cur_port->evbuf);
		if(plug->request_update){
		    const LV2_Atom_Object get = {
			{sizeof(LV2_Atom_Object_Body), plug_data->urids.atom_Object},
			{0, plug_data->urids.patch_Get}};
		    plug_evbuf_write(&iter_buf, 0, 0, get.atom.type, get.atom.size, LV2_ATOM_BODY_CONST(&get));
		}
		//TODO atom sequence ports should communicate with gui but for now only midi ports will
		//be implemented
		if(a_buffer){
		    app_jack_midi_cont_reset(plug->midi_cont);
		    app_jack_return_notes_vels_rt(a_buffer, plug->midi_cont);
		    //go through the returned audio backend midi event and write to the evbuf
		    for(uint32_t i = 0; i<plug->midi_cont->num_events; i++){
			const unsigned char midi_msg[3] = {plug->midi_cont->types[i], plug->midi_cont->note_pitches[i], plug->midi_cont->vel_trig[i]};
			int write = plug_evbuf_write(&iter_buf, plug->midi_cont->nframe_nums[i], 0, cur_port->port_type_urid,
						     plug->midi_cont->buf_size[i], midi_msg);
		    }
		}
	    }
	    if(cur_port->type == TYPE_EVENT && cur_port->flow == FLOW_OUTPUT){
		plug_evbuf_reset(cur_port->evbuf, 0);
	    }
	    plug->request_update = false;
	}
	//----------------------------------------------------------------------------------------------------

	//----------------------------------------------------------------------------------------------------
	//now run the plugin for nframes
	plug_run_rt(plug, nframes);

	//----------------------------------------------------------------------------------------------------
	//now update the output ports and any output controls (ui)
	for(uint32_t i = 0; i< plug->num_ports; i++){
	    PLUG_PORT* const cur_port = &(plug->ports[i]);
	    if(cur_port->flow!=FLOW_OUTPUT)continue;

	    if(cur_port->type == TYPE_AUDIO){

	    }
	    if(cur_port->type == TYPE_CV){
   
	    }
	    //TODO send plugin values to ui, but dont do it in realtime speed, might need a frequency when to send
	    if(cur_port->type == TYPE_CONTROL){
	    
	    }
	    if(cur_port->type == TYPE_EVENT){
		void* buf = NULL;	    
		if(cur_port->sys_port){

		    //clean the buffer
		    buf = app_jack_get_buffer_rt(cur_port->sys_port, nframes);
		    app_jack_midi_clear_buffer_rt(buf);
		}
		//write from sys_port out to the audio client midi out
		for(PLUG_EVBUF_ITERATOR i = plug_evbuf_begin(cur_port->evbuf); plug_evbuf_is_valid(i);
		    i = plug_evbuf_next(i)){
		    uint32_t frames = 0;
		    uint32_t subframes = 0;
		    LV2_URID type = 0;
		    uint32_t size = 0;
		    void* data = NULL;

		    plug_evbuf_get(i, &frames, &subframes, &type, &size, &data);
		    if(buf && type == plug->urids->midi_MidiEvent){
			app_jack_midi_events_write_rt(buf, frames, data, size);
		    }
		}
	    }
	}	
    }
}

static void plug_run_rt(PLUG_PLUG* plug, unsigned int nframes){
    if(!plug)return;
    if(!plug->plug_instance)return;
	
    LilvInstance* plug_instance = plug->plug_instance;
    lilv_instance_run(plug_instance, nframes);

    //Process workers for the plugin
    LV2_Handle handle = lilv_instance_get_handle(plug->plug_instance);
    jalv_worker_emit_responses(plug->worker, handle);
    jalv_worker_emit_responses(plug->state_worker, handle);
    jalv_worker_end_run(plug->worker);
}

static int plug_add_plug_to_array(PLUG_INFO* plug_data, PLUG_PLUG* plug, int in_id){
    int return_val = -1;
    if(in_id!=-1){
	if(in_id>=MAX_INSTANCES)goto finish;
	if(plug_data->plugins[in_id] !=NULL){
	    plug_remove_plug(plug_data, in_id);
	}
	plug_data->plugins[in_id] = plug;
	return_val = in_id;
	goto finish;
    }
    else{
	if(plug_data->total_plugs >= MAX_INSTANCES)goto finish;
	for(int i = 0; i<plug_data->max_id+2; i++){
	    if(plug_data->plugins[i] == NULL){
		plug_data->plugins[i] = plug;
		return_val = i;
		goto finish;
	    }
	}
    }
    
finish:
    return return_val;    
    
}
int  plug_remove_plug(PLUG_INFO* plug_data, const int id){
    if(!plug_data)return -1;
    if(!plug_data->audio_backend)return -1;
    int return_val = 0;       
    PLUG_PLUG* cur_plug = plug_data->plugins[id];
    if(!cur_plug){
	return_val = -1;
	goto done;
    }
    //Terminate the worker
    if(cur_plug->worker){
	jalv_worker_exit(cur_plug->worker);
	jalv_worker_free(cur_plug->worker);	
    }
    if(cur_plug->state_worker){
	jalv_worker_exit(cur_plug->state_worker);
	jalv_worker_free(cur_plug->state_worker);
    }
    zix_sem_destroy(&(cur_plug->work_lock));
    //clean the ports
    for(uint32_t i = 0; i< cur_plug->num_ports; i++){
	PLUG_PORT* const cur_port = &(cur_plug->ports[i]);
	if(cur_port->sys_port)
	    app_jack_unregister_port(plug_data->audio_backend, cur_port->sys_port);
	if(cur_port)
	    if(cur_port->evbuf)free(cur_port->evbuf);
    }
    if(cur_plug->ports)free(cur_plug->ports);
    cur_plug->ports = NULL;  
    //free the features_list
    if(cur_plug->feature_list)free(cur_plug->feature_list);
    //remove preset
    if(cur_plug->preset){
	const LilvNode* old_preset = lilv_state_get_uri(cur_plug->preset);
	if(old_preset){
	    lilv_world_unload_resource(plug_data->lv_world, old_preset);
	}
	lilv_state_free(cur_plug->preset);
    }
    //free instance
    if(cur_plug->plug_instance){
	LilvInstance* cur_instance = cur_plug->plug_instance;
	lilv_instance_deactivate(cur_instance);
	lilv_instance_free(cur_instance);
	cur_plug->plug_instance = NULL;		
    }
    //clean the midi container
    if(cur_plug->midi_cont){
	app_jack_clean_midi_cont(cur_plug->midi_cont);
	free(cur_plug->midi_cont);
    }
    
    cur_plug->plug = NULL;    
    free(cur_plug);
    plug_data->plugins[id] = NULL;
    //find what the biggest id is now
    if(plug_data->max_id == id){
	int max_id = -1;
	for(unsigned int i = 0; i<plug_data->max_id+1; i++){
	    PLUG_PLUG* this_plug = plug_data->plugins[i];
	    if(!this_plug)continue;
	    if(this_plug->id >= max_id){
		max_id = this_plug->id;
	    }
	}
	plug_data->max_id = max_id;
    }
    if(plug_data->total_plugs>0)plug_data->total_plugs -= 1;
done:
    return return_val;
}
void plug_clean_memory(PLUG_INFO* plug_data){
    if(!plug_data)goto finish;
    for(int i = 0; i< plug_data->max_id+1; i++){
	plug_remove_plug(plug_data, i);
    }
    if(plug_data->symap)symap_free(plug_data->symap);
    zix_sem_destroy(&(plug_data->symap_lock));    
    for (LilvNode** n = (LilvNode**)&plug_data->nodes; *n; ++n) {
	lilv_node_free(*n);
    }    
    if(plug_data->lv_world)lilv_world_free(plug_data->lv_world);
    free(plug_data);
finish:
}
