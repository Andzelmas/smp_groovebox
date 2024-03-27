#pragma once
//enum for context types
enum appContextTypes{
    Context_type_Sampler = 0x01,
    Context_type_Plugins = 0x02,
    Context_type_Trk = 0x03,
    Context_type_Synth = 0x04
};

//enum for value return types
enum appReturnType{
    Uchar_type = 0x01,
    Int_type = 0x02,
    Float_type = 0x03,
    DB_Return_Type = 0x04, // returned value should be displayed as db, so converted to log scale
    String_Return_Type = 0x05, //returned value is a string, will need to use a param function to return it
    Curve_Float_Return_Type = 0x06 //float that should be presented to the user as a special curve, from curve table on the parameter
};

//enum for operations on parameters types
enum paramOperType{
    Operation_Nothing = 0x00,
    Operation_Decrease = 0x01,
    Operation_Increase = 0x02,
    Operation_SetValue = 0x03,
    Operation_DefValue = 0x04 //set the value of the parameter to the default value
};

//wavetables
enum waveTablesType{
    SIN_WAVETABLE, 
    TRIANGLE_WAVETABLE,
    SAW_WAVETABLE,
    SQUARE_WAVETABLE
};

//parameter and ring buffer realtime values
enum paramRealtimeType{
    UI_PARAM_E = 0,
    RT_PARAM_E = 1,
    UI_TO_RT_RING_E = 0,
    RT_TO_UI_RING_E = 1
};
//stream flow directions
/*
enum FlowType{
    FLOW_INPUT = 0x1,
    FLOW_OUTPUT = 0x2
};
*/
enum FlowType{
    FLOW_UNKNOWN,
    FLOW_INPUT,
    FLOW_OUTPUT
};
//the port type for the audio client
enum PortType{
    TYPE_UNKNOWN,
    TYPE_CONTROL,
    TYPE_AUDIO,
    TYPE_EVENT,
    TYPE_MIDI,
    TYPE_CV
};    
