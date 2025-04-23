#include "math_funcs.h"
#include <math.h>
#include <stdlib.h>

typedef struct _range_table{
    PARAM_T min_pt; //minimum value in the table
    PARAM_T max_pt;//maximum value in the table
    PARAM_T pt_inc;//the increment that the values will be looked up by
    unsigned int len;//table total length, calculated
    PARAM_T* table;//the table filled with values, growing in pt_inc increments
}MATH_RANGE_TABLE;

MATH_RANGE_TABLE* math_init_range_table(PARAM_T min_pt, PARAM_T max_pt, PARAM_T pt_inc){
    PARAM_T total_len = (max_pt - min_pt) / pt_inc;
    unsigned int len_int = (unsigned int)total_len;
    PARAM_T fracPart = total_len - len_int;
    //this means that with given pt_inc we cant get the max_pt going from min_pt, so max_pt will be increased later
    if(fracPart > 0) len_int += 1;
    //the length of the table has to be 1 bigger
    len_int += 1;
    MATH_RANGE_TABLE* range_table = malloc(sizeof(MATH_RANGE_TABLE));
    if(!range_table)return NULL;
    range_table->min_pt = min_pt;
    range_table->max_pt = max_pt;
    range_table->pt_inc = pt_inc;
    range_table->len = len_int;
    range_table->table = malloc(sizeof(PARAM_T) * len_int);
    if(!range_table->table){
	free(range_table);
	return NULL;
    }
    //just to track the max_pt in case the max_pt cant be reached with pt_inc given, and we have to increase max_pt
    PARAM_T cur_max_pt = min_pt;
    //fill the table with 0.0
    for(unsigned int i = 0; i < len_int; i++){
	range_table->table[i] = 0.0;
	if(i > 0)cur_max_pt += pt_inc;
    }
    range_table->max_pt = cur_max_pt;
    return range_table;
}

PARAM_T math_range_table_convert_value(MATH_RANGE_TABLE* range_table, PARAM_T get_value){
    if(!range_table)return 0.0;
    if(!range_table->table)return 0.0;
    if(range_table->pt_inc <= 0)return 0.0;
    if(range_table->len <= 0) return 0.0;
    //get_value is the value user wants to convert to a value in the table
    //so first calculate what index is this
    PARAM_T index = (get_value - range_table->min_pt) / range_table->pt_inc;
    if(index >= range_table->len) index -= 1.0;
    //if the index is still beyond the table user gave us a huge value, just wrap around the table
    if(index >= range_table->len) index = range_table->len - 1.0;

    return math_get_from_table_lerp(range_table->table, range_table->len, index);
}

PARAM_T math_range_table_get_value(MATH_RANGE_TABLE* range_table, unsigned int index){
    if(!range_table)return 0.0;
    if(!range_table->table)return 0.0;
    if(range_table->pt_inc <= 0)return 0.0;
    if(range_table->len <= 0) return 0.0;
    if(index >= range_table->len) return 0.0;

    return range_table->min_pt + (index * range_table->pt_inc);
}

unsigned int math_range_table_get_len(MATH_RANGE_TABLE* range_table){
    if(!range_table)return 0;
    return range_table->len;
}

int math_range_table_enter_value(MATH_RANGE_TABLE* range_table, unsigned int index, PARAM_T val){
    if(!range_table)return -1;
    if(index >= range_table->len)return -1;
    range_table->table[index] = val;
    return 0;
}

void math_range_table_clean(MATH_RANGE_TABLE* range_table){
    if(!range_table)return;
    if(range_table->table)free(range_table->table);
    free(range_table);
}

//fit old range to a new one
PARAM_T fit_range(PARAM_T i_end, PARAM_T i_start, PARAM_T o_end, PARAM_T o_start, PARAM_T in){
    PARAM_T slope = 1.0*(o_end-o_start)/(i_end-i_start);
    return o_start + slope*(in - i_start);
}

PARAM_T midi_note_to_freq(MIDI_DATA_T note_in){
    PARAM_T ret_val = 440;
    switch(note_in){
    case 0:
	ret_val = 8.1757989156;
	break;
    case 1:
	ret_val = 8.6619572180;
	break;
    case 2:
	ret_val = 9.1770239974;
	break;
    case 3:
	ret_val = 9.7227182413;
	break;
    case 4:
	ret_val = 10.3008611535;
	break;
    case 5:
	ret_val = 10.9133822323;
	break;
    case 6:
	ret_val = 11.5623257097;
	break;
    case 7:
	ret_val = 12.2498573744;
	break;
    case 8:
	ret_val = 12.9782717994;
	break;
    case 9:
	ret_val = 13.7500000000;
	break;
    case 10:
	ret_val = 14.5676175474;
	break;
    case 11:
	ret_val = 15.4338531643;
	break;
    case 12:
	ret_val = 16.3515978313;
	break;
    case 13:
	ret_val = 17.3239144361;
	break;
    case 14:
	ret_val = 18.3540479948;
	break;
    case 15:
	ret_val = 19.4454364826;
	break;
    case 16:
	ret_val = 20.6017223071;
	break;
    case 17:
	ret_val = 21.8267644646;
	break;
    case 18:
	ret_val = 23.1246514195;
	break;
    case 19:
	ret_val = 24.4997147489;
	break;
    case 20:
	ret_val = 25.9565435987;
	break;
    case 21:
	ret_val = 27.5000000000;
	break;
    case 22:
	ret_val = 29.1352350949;
	break;
    case 23:
	ret_val = 30.8677063285;
	break;
    case 24:
	ret_val = 32.7031956626;
	break;
    case 25:
	ret_val = 34.6478288721;
	break;
    case 26:
	ret_val = 36.7080959897;
	break;
    case 27:
	ret_val = 38.8908729653;
	break;
    case 28:
	ret_val = 41.2034446141;
	break;
    case 29:
	ret_val = 43.6535289291;
	break;
    case 30:
	ret_val = 46.2493028390;
	break;
    case 31:
	ret_val = 48.9994294977;
	break;
    case 32:
	ret_val = 51.9130871975;
	break;
    case 33:
	ret_val = 55.0000000000;
	break;
    case 34:
	ret_val = 58.2704701898;
	break;
    case 35:
	ret_val = 61.7354126570;
	break;
    case 36:
	ret_val = 65.4063913251;
	break;
    case 37:
	ret_val = 69.2956577442;
	break;
    case 38:
	ret_val = 73.4161919794;
	break;
    case 39:
	ret_val = 77.7817459305;
	break;
    case 40:
	ret_val = 82.4068892282;
	break;
    case 41:
	ret_val = 87.3070578583;
	break;
    case 42:
	ret_val = 92.4986056779;
	break;
    case 43:
	ret_val = 97.9988589954;
	break;
    case 44:
	ret_val = 103.8261743950;
	break;
    case 45:
	ret_val = 110.0000000000;
	break;
    case 46:
	ret_val = 116.5409403795;
	break;
    case 47:
	ret_val = 123.4708253140;
	break;
    case 48:
	ret_val = 130.8127826503;
	break;
    case 49:
	ret_val = 138.5913154884;
	break;
    case 50:
	ret_val = 146.8323839587;
	break;
    case 51:
	ret_val = 155.5634918610;
	break;
    case 52:
	ret_val = 164.8137784564;
	break;
    case 53:
	ret_val = 174.6141157165;
	break;
    case 54:
	ret_val = 184.9972113558;
	break;
    case 55:
	ret_val = 195.9977179909;
	break;
    case 56:
	ret_val = 207.6523487900;
	break;
    case 57:
	ret_val = 220.0000000000;
	break;
    case 58:
	ret_val = 233.0818807590;
	break;
    case 59:
	ret_val = 246.9416506281;
	break;
    case 60:
	ret_val = 261.6255653006;
	break;
    case 61:
	ret_val = 277.1826309769;
	break;
    case 62:
	ret_val = 293.6647679174;
	break;
    case 63:
	ret_val = 311.1269837221;
	break;
    case 64:
	ret_val = 329.6275569129;
	break;
    case 65:
	ret_val = 349.2282314330;
	break;
    case 66:
	ret_val = 369.9944227116;
	break;
    case 67:
	ret_val = 391.9954359817;
	break;
    case 68:
	ret_val = 415.3046975799;
	break;
    case 69:
	ret_val = 440.0000000000;
	break;
    case 70:
	ret_val = 466.1637615181;
	break;
    case 71:
	ret_val = 493.8833012561;
	break;
    case 72:
	ret_val = 523.2511306012;
	break;
    case 73:
	ret_val = 554.3652619537;
	break;
    case 74:
	ret_val = 587.3295358348;
	break;
    case 75:
	ret_val = 622.2539674442;
	break;
    case 76:
	ret_val = 659.2551138257;
	break;
    case 77:
	ret_val = 698.4564628660;
	break;
    case 78:
	ret_val = 739.9888454233;
	break;
    case 79:
	ret_val = 783.9908719635;
	break;
    case 80:
	ret_val = 830.6093951599;
	break;
    case 81:
	ret_val = 880.0000000000;
	break;
    case 82:
	ret_val = 932.3275230362;
	break;
    case 83:
	ret_val = 987.7666025122;
	break;
    case 84:
	ret_val = 1046.5022612024;
	break;
    case 85:
	ret_val = 1108.7305239075;
	break;
    case 86:
	ret_val = 1174.6590716696;
	break;
    case 87:
	ret_val = 1244.5079348883;
	break;
    case 88:
	ret_val = 1318.5102276515;
	break;
    case 89:
	ret_val = 1396.9129257320;
	break;
    case 90:
	ret_val = 1479.9776908465;
	break;
    case 91:
	ret_val = 1567.9817439270;
	break;
    case 92:
	ret_val = 1661.2187903198;
	break;
    case 93:
	ret_val = 1760.0000000000;
	break;
    case 94:
	ret_val = 1864.6550460724;
	break;
    case 95:
	ret_val = 1975.5332050245;
	break;
    case 96:
	ret_val = 2093.0045224048;
	break;
    case 97:
	ret_val = 2217.4610478150;
	break;
    case 98:
	ret_val = 2349.3181433393;
	break;
    case 99:
	ret_val = 2489.0158697766;
	break;
    case 100:
	ret_val = 2637.0204553030;
	break;
    case 101:
	ret_val = 2793.8258514640;
	break;
    case 102:
	ret_val = 2959.9553816931;
	break;
    case 103:
	ret_val = 3135.9634878540;
	break;
    case 104:
	ret_val = 3322.4375806396;
	break;
    case 105:
	ret_val = 3520.0000000000;
	break;
    case 106:
	ret_val = 3729.3100921447;
	break;
    case 107:
	ret_val = 3951.0664100490;
	break;
    case 108:
	ret_val = 4186.0090448096;
	break;
    case 109:
	ret_val = 4434.9220956300;
	break;
    case 110:
	ret_val = 4698.6362866785;
	break;
    case 111:
	ret_val = 4978.0317395533;
	break;
    case 112:
	ret_val = 5274.0409106059;
	break;
    case 113:
	ret_val = 5587.6517029281;
	break;
    case 114:
	ret_val = 5919.9107633862;
	break;
    case 115:
	ret_val = 6271.92697571;
	break;
    case 116:
	ret_val = 6644.8751612791;
	break;
    case 117:
	ret_val = 7040.0000000000;
	break;
    case 118:
	ret_val = 7458.6201842894;
	break;
    case 119:
	ret_val = 7902.1328200980;
	break;
    case 120:
	ret_val = 8372.0180896192;
	break;
    case 121:
	ret_val = 8869.8441912599;
	break;
    case 122:
	ret_val = 9397.2725733570;
	break;
    case 123:
	ret_val = 9956.0634791066;
	break;
    case 124:
	ret_val = 10548.0818212118;
	break;
    case 125:
	ret_val = 11175.3034058561;
	break;
    case 126:
	ret_val = 11839.8215267723;
	break;
    case 127:
	ret_val = 12543.8539514160;
	break;
    default:
	ret_val = 440;
    }
    return ret_val;
}

PARAM_T exp_range_ratio(PARAM_T num_items, PARAM_T cur_item){
    if(cur_item == 0)return 1;
    return pow(pow(2, 1.0/num_items), cur_item);
}

PARAM_T freq_add_semitones(PARAM_T freq_in, PARAM_T semitones){
    PARAM_T ret_freq = freq_in;
    if(semitones == 0)return freq_in;
    PARAM_T freq_ratio = exp_range_ratio(12.0, semitones);
    ret_freq = (PARAM_T)(freq_in * freq_ratio);
    return ret_freq;
}

PARAM_T math_get_from_table_lerp(PARAM_T* table_in, unsigned int len, PARAM_T index){
    unsigned int int_idx = index;
    if(int_idx >= len) int_idx = len - 1;
    
    PARAM_T fracPart = index - int_idx;
    
    PARAM_T samp0 = table_in[int_idx];
    if(++int_idx >= len) int_idx = 0;
    PARAM_T samp1 = table_in[int_idx];
    PARAM_T samp = samp0 + (samp1 - samp0) * fracPart;

    return samp;
}
