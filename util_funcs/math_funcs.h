#pragma once
#include "../structs.h"

//a table where the user can specify the ranges from min to max, not length
//and the length is calculated automatically
typedef struct _range_table MATH_RANGE_TABLE;
//initiate the range table, it will be filled with zeroes first
MATH_RANGE_TABLE* math_init_range_table(PARAM_T min_pt, PARAM_T max_pt, PARAM_T pt_inc);
//given the value, get from the appropriate index in the table the value saved there by the user
PARAM_T math_range_table_convert_value(MATH_RANGE_TABLE* range_table, PARAM_T get_value);
//given the index of the table give the value (get_value in math_range_table_convert_value)
//this is used to fill the table values
PARAM_T math_range_table_get_value(MATH_RANGE_TABLE* range_table, unsigned int index);
unsigned int math_range_table_get_len(MATH_RANGE_TABLE* range_table);
int math_range_table_enter_value(MATH_RANGE_TABLE* range_table, unsigned int index, PARAM_T val);
void math_range_table_clean(MATH_RANGE_TABLE* range_table);

/*various math helper functions, conversions etc.*/
//most of them should operate on double, for more precision
//function to fit one range to another
/* i_end, i_start - old range
 * o_end, o_start  - new range
 * in - the value*/
PARAM_T fit_range(PARAM_T i_end, PARAM_T i_start, PARAM_T o_end, PARAM_T o_start, PARAM_T in);

//convert midi note to frequency
//this is a simply table lookup, no logametric calculations
PARAM_T midi_note_to_freq(MIDI_DATA_T note_in);
//get an increment multiplier for a exponential scale.
//num_items - how many items in the system, if cur_item == num_items will return ~2.0
//if cur_item > num_items will of course extrapolate.
//Ex.: num_items  = 12 and cur_item = how many semitones to get a multiplier for frequency to add the semitones 
PARAM_T exp_range_ratio(PARAM_T num_items, PARAM_T cur_item);
//return the frequency increased or decreased by the semitones, uses freq_calc_freq_ratio function
PARAM_T freq_add_semitones(PARAM_T freq_in, PARAM_T semitones);
//get from a table values with linear interpolation between values if index is with a fraction
PARAM_T math_get_from_table_lerp(PARAM_T* table_in, unsigned int len, PARAM_T index);
