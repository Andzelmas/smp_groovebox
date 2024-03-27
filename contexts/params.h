#pragma once
#include "../structs.h"
#include "../util_funcs/math_funcs.h"
//struct that holds values that user wants interpolated over time
typedef struct _params_interp_val PRM_INTERP_VAL;
//struct that holds the parameters
typedef struct _params_container PRM_CONTAIN;
//parameter
typedef struct _params_param PRM_PARAM;
//init the interpoalting val
PRM_INTERP_VAL* params_init_interpolated_val(SAMPLE_T max_range, unsigned int total_samples);
//interpolate the value and get the cur val
SAMPLE_T params_interp_val_get_value(PRM_INTERP_VAL* intrp_val, SAMPLE_T new_val);
//initializes the parameter container the parameter value arrays (for min val, names etc) have to be the same size
PRM_CONTAIN* params_init_param_container(unsigned int num_of_params, char** param_names, float* param_vals,
					 float* param_mins, float* param_maxs, float* param_incs, unsigned char* val_types);
//add a curve table for exponential, logarithmic etc. parameters
//should be added in the initialization stage, while the rt thread is not launched, since will add to rt and ui parameters
int param_add_curve_table(PRM_CONTAIN* param_container, int val_id, MATH_RANGE_TABLE* table);
//set the parameter value. param_op is what to do with parameter, check types.h the paramOperType enum
int param_set_value(PRM_CONTAIN* param_container, int val_id, SAMPLE_T set_to, unsigned char param_op,
		    unsigned int rt_params);
//get the parameter value. What type of value is returned to val_type, check appReturntype enum in types.h for the list
//if returned value is -1 and val_type is 0, something went wrong
//if interp == 1 the value will be interpolated slowly on each param_get_value call (the speed is INTERP_SAMPLES in params.c)
//if curved == 1 get the value from the curve_table if there is one
SAMPLE_T param_get_value(PRM_CONTAIN* param_container, int val_id, unsigned char* val_type,
			 unsigned int curved, unsigned int interp, unsigned int rt_params);
//set the strings for paramter, the type must be String_Return_type
int param_set_param_strings(PRM_CONTAIN* param_container, int val_id, char** strings);
//get the parameter string, from the current parameter value, the values must go from >= 0 in positive direction
const char* param_get_param_string(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params);
//check if the parameter is just changed - returns 1 if this parameters value was not retrieved with param_get_value
int param_get_if_changed(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params);
//check if any of the parameters have changed in the parameter set. If at least one parameter has a just_changed
//1 the for loop will break and return 1
int param_get_if_any_changed(PRM_CONTAIN* param_container, unsigned int rt_params);
//get the parameter name
const char* param_get_name(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params);
//return how many parameters are there on the param container
unsigned int param_return_num_params(PRM_CONTAIN* param_container);
//cleans the parameter containerx
void param_clean_param_container(PRM_CONTAIN* param_container);
