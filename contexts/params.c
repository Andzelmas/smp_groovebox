#include "params.h"
#include "../types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "../util_funcs/log_funcs.h"
#include "../util_funcs/ring_buffer.h"
//default speed per samples to interpolate the parameters when requested
#define INTERP_SAMPLES 400

typedef struct _params_interp_val{
    PARAM_T cur_inc; //how much to increment the value 
    PARAM_T from_val; //from what value we are interpolating
    PARAM_T to_val; //to what value interpolating
    PARAM_T cur_val; //cur value that will be returned to the user
    PARAM_T new_val; //new value that is given from the user, while the interpolation is not finished ignore this value
    int dir_mult; //to what direction to go, if from_val > to_val we have to cur_inc *= -1;
}PRM_INTERP_VAL;

typedef struct _params_param{
    //value of the parameter
    PARAM_T val;
    PARAM_T min_val;
    PARAM_T max_val;
    PARAM_T def_val; //default value
    //how much to increase or decrease the parameter
    PARAM_T inc_am;
    //the type of the parameter value, used for ui display purposes
    //check the appReturnType in the types.h
    unsigned char val_type;
    //if this is 1 the parameter was just changed, this will change to 0 when get_value will be invoked
    unsigned int just_changed;
    //if the name of parameter just changed this will be 1, and will become a 0 if param_name_get_if_changed is called
    unsigned int name_just_changed;
    char name[MAX_PARAM_NAME_LENGTH];
    //sometimes we might want to get an interpolated version of the parameter, so it does not change so quickly,
    //for example to avoid a click when changing amplitude of a synth oscillator
    PRM_INTERP_VAL* interp_val;
    //for exponential parameters this is where the table for the exp or any other curve should be
    //this table needs to be normalized (range 0..1) and malloced outside of this context and added here
    //with the param_add_curve_table
    MATH_RANGE_TABLE* curve_table;
    //for parameters that contain strings to display for user
    unsigned int param_strings_num; // how many strings there are for this parameter
    char** param_strings; //the string array
    //user data for convenience (for example clap plugins has a void* cookie for faster loading of params from events)
    void* user_data;
    //is the parameter hidden
    uint16_t is_hidden;
}PRM_PARAM;

typedef struct _params_container{
    //the parameters arrays
    //rt_params should be touched only by the rt thread, and the ui_params only by the simple,
    //usually the ui thread
    PRM_PARAM* rt_params;
    PRM_PARAM* ui_params;
    //how many parameters are there
    unsigned int num_of_params_ui;
    unsigned int num_of_params_rt;
    //ring buffers for parameter manipulation/communication
    RING_BUFFER* param_rt_to_ui;
    RING_BUFFER* param_ui_to_rt;
    PRM_USER_DATA user_data;
}PRM_CONTAIN;

PRM_INTERP_VAL* params_init_interpolated_val(PARAM_T max_range, unsigned int total_samples){
    if(total_samples <= 0)return NULL;
    if(max_range <= 0)return NULL;
    PRM_INTERP_VAL* intrp_val = malloc(sizeof(PRM_INTERP_VAL));
    if(!intrp_val)return NULL;
    
    intrp_val->cur_inc = (PARAM_T)(max_range / (PARAM_T)total_samples);
    intrp_val->cur_val = 0.0;
    intrp_val->from_val = 0.0;
    intrp_val->to_val = 0.0;
    intrp_val->new_val = 0.0;
    intrp_val->dir_mult = 1;
    return intrp_val;
}

PARAM_T params_interp_val_get_value(PRM_INTERP_VAL* intrp_val, PARAM_T new_val){
    if(!intrp_val)return new_val;
    intrp_val->new_val = new_val;
    
    if(intrp_val->cur_inc <= 0)return new_val;
    
    intrp_val->cur_val += (intrp_val->cur_inc * intrp_val->dir_mult);
    
    if(intrp_val->cur_val >= intrp_val->to_val && intrp_val->dir_mult > 0){
	intrp_val->from_val = intrp_val->to_val;
	intrp_val->cur_val = intrp_val->to_val;
	intrp_val->to_val = intrp_val->new_val;
	
	intrp_val->dir_mult = 1;
	if(intrp_val->from_val > intrp_val->to_val) intrp_val->dir_mult = -1;
    }

    if(intrp_val->cur_val <= intrp_val->to_val && intrp_val->dir_mult < 0){
	intrp_val->from_val = intrp_val->to_val;
	intrp_val->cur_val = intrp_val->to_val;
	intrp_val->to_val = intrp_val->new_val;
	
	intrp_val->dir_mult = 1;
	if(intrp_val->from_val > intrp_val->to_val) intrp_val->dir_mult = -1;
    }
    
    return intrp_val->cur_val;
}

PRM_CONTAIN* params_init_param_container(unsigned int num_of_params, char** param_names, PARAM_T* param_vals,
					 PARAM_T* param_mins, PARAM_T* param_maxs, PARAM_T* param_incs, unsigned char* val_types, void** user_data_per_param){
    if(num_of_params<=0) return NULL;
    if(!param_names || !param_vals || !param_mins || !param_maxs || !param_incs || !val_types) return NULL;
    PRM_CONTAIN* param_container = (PRM_CONTAIN*)malloc(sizeof(PRM_CONTAIN));
    if(!param_container)return NULL;
    param_container->param_rt_to_ui = NULL;
    param_container->param_ui_to_rt = NULL;
    param_container->num_of_params_rt = 0;
    param_container->num_of_params_ui = 0;
    param_container->rt_params = NULL;
    param_container->ui_params = NULL;
    (&param_container->user_data)->user_data = NULL;
    (&param_container->user_data)->val_to_string = NULL;
    
    param_container->param_rt_to_ui = ring_buffer_init(sizeof(PARAM_RING_DATA_BIT), MAX_PARAM_RING_BUFFER_ARRAY_SIZE);
    param_container->param_ui_to_rt = ring_buffer_init(sizeof(PARAM_RING_DATA_BIT), MAX_PARAM_RING_BUFFER_ARRAY_SIZE);
    if(!param_container->param_rt_to_ui || !param_container->param_ui_to_rt){
	param_clean_param_container(param_container);
	return NULL;
    }
    
    param_container->num_of_params_rt = num_of_params;
    param_container->rt_params = calloc(num_of_params, sizeof(PRM_PARAM));
    param_container->num_of_params_ui = num_of_params;
    param_container->ui_params = calloc(num_of_params, sizeof(PRM_PARAM));
    if(!param_container->rt_params || !param_container->ui_params){
	param_clean_param_container(param_container);
	return NULL;
    }
    
    for(int i = 0; i< num_of_params; i++){
	PRM_PARAM* rt_params = &(param_container->rt_params[i]);
	PRM_PARAM* ui_params = &(param_container->ui_params[i]);
	
	rt_params->interp_val = params_init_interpolated_val(fabs(param_maxs[i] - param_mins[i]) , INTERP_SAMPLES);
	if(!rt_params->interp_val)continue;	
	ui_params->interp_val = params_init_interpolated_val(fabs(param_maxs[i] - param_mins[i]) , INTERP_SAMPLES);
	if(!ui_params->interp_val){
	    free(rt_params->interp_val);
	    continue;
	}	
	rt_params->just_changed = 0;
	ui_params->just_changed = 0;
		
	rt_params->val = param_vals[i];
	ui_params->val = param_vals[i];
	
	rt_params->min_val = param_mins[i];
	ui_params->min_val = param_mins[i];
	
	rt_params->max_val = param_maxs[i];
	ui_params->max_val = param_maxs[i];

	rt_params->def_val = param_vals[i];
	ui_params->def_val = param_vals[i];
	
	rt_params->inc_am = param_incs[i];
	ui_params->inc_am = param_incs[i];
	
	rt_params->val_type = val_types[i];
	ui_params->val_type = val_types[i];

	//to make the parameter hidden send a param_set_value with Operation_Hidden and set_to 0 or 1
	rt_params->is_hidden = 0;
	ui_params->is_hidden = 0;
	
	const char* param_name = param_names[i];
	snprintf(rt_params->name, MAX_PARAM_NAME_LENGTH, "%s", param_name);
	snprintf(ui_params->name, MAX_PARAM_NAME_LENGTH, "%s", param_name);
	rt_params->name_just_changed = 1;
	ui_params->name_just_changed = 1;

	if(user_data_per_param){
	    rt_params->user_data = user_data_per_param[i];
	    ui_params->user_data = user_data_per_param[i];
	}
    }

    return param_container;
}
void params_add_user_data(PRM_CONTAIN* param_container, PRM_USER_DATA user_data){
    if(!param_container)return;
    param_container->user_data = user_data;
}
int param_add_curve_table(PRM_CONTAIN* param_container, int val_id, MATH_RANGE_TABLE* table){
    if(!param_container)return -1;
    
    if(val_id >= param_container->num_of_params_ui)return -1;
    PRM_PARAM* param_array_rt = param_container->rt_params;
    PRM_PARAM* param_array_ui = param_container->ui_params;
    if(!param_array_rt || !param_array_ui)return -1;

    PRM_PARAM* cur_param_rt = &(param_array_rt[val_id]);
    PRM_PARAM* cur_param_ui = &(param_array_ui[val_id]);
    if(!cur_param_rt || !cur_param_ui)return -1;
    
    cur_param_rt->curve_table = table;
    cur_param_ui->curve_table = table;

    return 0;
}

static void param_set_value_directly(PRM_PARAM* cur_param, PARAM_T set_to, const char* in_string, unsigned char param_op){
    if(!cur_param)return;
    PARAM_T prev_value = cur_param->val;

    switch(param_op){
    case Operation_Decrease:
	cur_param->val -= set_to * cur_param->inc_am;
	break;
    case Operation_Increase:
	cur_param->val += set_to * cur_param->inc_am;
	break;
    case Operation_SetValue:
	cur_param->val = set_to;
	break;
    case Operation_DefValue:
	cur_param->val = cur_param->def_val;
	break;
    case Operation_SetIncr:
	cur_param->inc_am = set_to;
	break;
    case Operation_SetDefValue:
	cur_param->def_val = set_to;
	break;
    case Operation_ChangeName:
	if(in_string){
	    snprintf(cur_param->name, MAX_PARAM_NAME_LENGTH, "%s", in_string);
	    cur_param->name_just_changed = 1;
	}
	break;
    case Operation_ToggleHidden:
	if(set_to <= 0)cur_param->is_hidden = 0;
	if(set_to >= 1)cur_param->is_hidden = 1;
	break;
    default:
	cur_param->val = cur_param->val;
    }

    if(cur_param->val < cur_param->min_val)cur_param->val = cur_param->min_val;
    if(cur_param->val > cur_param->max_val)cur_param->val = cur_param->max_val;
    //if the value changed mark this param as changed
    if(prev_value != cur_param->val){
	cur_param->just_changed = 1;
    }
}

void param_msgs_process(PRM_CONTAIN* param_container, unsigned int rt_params){
    if(!param_container)return;

    PRM_PARAM* prm_array = NULL;
    RING_BUFFER* ring_buffer = NULL;
    if(!rt_params){
	prm_array = param_container->ui_params;
	ring_buffer = param_container->param_rt_to_ui;
    }
    if(rt_params){
	prm_array = param_container->rt_params;
	ring_buffer = param_container->param_ui_to_rt;
    }
    if(!prm_array || !ring_buffer)return;

    unsigned int cur_items = ring_buffer_return_items(ring_buffer);
    for(unsigned int i = 0; i < cur_items; i++){
	PARAM_RING_DATA_BIT cur_bit;
	int read_buffer = ring_buffer_read(ring_buffer, &cur_bit, sizeof(cur_bit));
	if(read_buffer <= 0)continue;
	param_set_value_directly(&(prm_array[cur_bit.param_id]), cur_bit.param_value, cur_bit.param_string, cur_bit.param_op);
    }    
    
}

int param_set_value(PRM_CONTAIN* param_container, int val_id, PARAM_T set_to, const char* set_string_to, unsigned char param_op, unsigned int rt_params){
    if(!param_container)return -1;
    if(isnan(set_to))return -1;
    PRM_PARAM* param_array = NULL;
    RING_BUFFER* ring_buffer = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
	ring_buffer = param_container->param_ui_to_rt;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
	ring_buffer = param_container->param_rt_to_ui;
    }
    if(val_id >= num_of_params || !param_array || !ring_buffer)return -1;

    PRM_PARAM* cur_param = &(param_array[val_id]);
    param_set_value_directly(cur_param, set_to, set_string_to, param_op);
    //only send the change to the other thread if the parameter actually changed its value
    if(param_get_if_changed(param_container, val_id, rt_params) == 1){
	PARAM_RING_DATA_BIT send_bit;
	send_bit.param_id = val_id;
	send_bit.param_op = param_op;
	snprintf(send_bit.param_string, MAX_PARAM_NAME_LENGTH, "%s", set_string_to);
	send_bit.param_value = set_to;
	ring_buffer_write(ring_buffer, &send_bit, sizeof(send_bit));
    }
    return 0;    
}
void* param_user_data_return(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return NULL;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }
    if(val_id >= num_of_params)return NULL;

    PRM_PARAM cur_param = param_array[val_id];
    
    return cur_param.user_data;
}
PARAM_T param_get_increment(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return -1;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }
    if(val_id >= num_of_params)return -1;

    PRM_PARAM cur_param = param_array[val_id];

    PARAM_T ret_increment = cur_param.inc_am;
    
    return ret_increment;
}

static unsigned char param_get_val_type(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return 0;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }
    if(val_id >= num_of_params)return 0;
    if(!param_array)return 0;
    
    PRM_PARAM* cur_param = &(param_array[val_id]);
    return cur_param->val_type;
}

PARAM_T param_get_value(PRM_CONTAIN* param_container, int val_id, unsigned int curved, unsigned int interp, unsigned int rt_params){
    if(!param_container)return -1;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }
    if(val_id >= num_of_params)return -1;
    if(!param_array)return -1;
    PRM_PARAM* cur_param = &(param_array[val_id]);
    //when returning the value we mark this param as no longer just_changed
    cur_param->just_changed = 0;

    PARAM_T ret_val = cur_param->val;
    //check if this parameter is of exponential or other curve nature
    if(curved == 1){
	if(cur_param->curve_table){
	    //if there is a curve table
	    PARAM_T val_min = cur_param->min_val;
	    PARAM_T val_max = cur_param->max_val;
	    //first make the param range 0..1
	    PARAM_T val_norm = fit_range(val_max, val_min, 1.0, 0.0, ret_val);
	    //now get what this value is in the table
	    PARAM_T val_curve = math_range_table_convert_value(cur_param->curve_table, val_norm);
	    //and return to the original range
	    ret_val = fit_range(1.0, 0.0, val_max, val_min, val_curve);
	}
    }
    
    //if user wants to interpolate the value and not return it right away
    if(interp == 1){
	if(cur_param->interp_val){
	    ret_val = params_interp_val_get_value(cur_param->interp_val, ret_val);
	}
    }
    return ret_val;
}

int param_set_param_strings(PRM_CONTAIN* param_container, int val_id, char** strings, unsigned int num_strings){
    if(!param_container)return -1;
    if(!strings)return -1;
    if(num_strings <= 0)return -1;
    if(val_id >= param_container->num_of_params_ui)return -1;
    PRM_PARAM* param_array_rt = param_container->rt_params;
    PRM_PARAM* param_array_ui = param_container->ui_params;

    PRM_PARAM* cur_param_rt = &(param_array_rt[val_id]);
    PRM_PARAM* cur_param_ui = &(param_array_ui[val_id]);
    unsigned char val_type = cur_param_ui->val_type;
    if((val_type & 0xff) != String_Return_Type)return -1;
    //free the strings if there are labels already on this parameter
    //though the labels should be set once, on the param init
    if(cur_param_rt->param_strings != NULL){
	free(cur_param_rt->param_strings);
	cur_param_rt->param_strings_num = 0;
    }
    if(cur_param_ui->param_strings != NULL){
	free(cur_param_ui->param_strings);
	cur_param_ui->param_strings_num = 0;
    }
    
    cur_param_rt->param_strings = malloc(sizeof(char*)*num_strings);
    if(cur_param_rt->param_strings == NULL)return -1;
    cur_param_ui->param_strings = malloc(sizeof(char*)*num_strings);
    if(cur_param_ui->param_strings == NULL){
	free(cur_param_rt->param_strings);
	cur_param_rt->param_strings = NULL;
	return -1;
    }
    cur_param_rt->param_strings_num = num_strings;
    cur_param_ui->param_strings_num = num_strings;
    for(int i = 0; i < num_strings; i++){
	cur_param_rt->param_strings[i] = NULL;
	cur_param_ui->param_strings[i] = NULL;
	const char* cur_string = strings[i];
	if(!cur_string)continue;
	char* copy_string_rt = malloc(sizeof(char) * (strlen(cur_string)+1));
	if(!copy_string_rt)continue;
	char* copy_string_ui = malloc(sizeof(char) * (strlen(cur_string)+1));
	if(!copy_string_ui){
	    free(copy_string_rt);
	    continue;
	}
	strcpy(copy_string_rt, cur_string);
	strcpy(copy_string_ui, cur_string);
	cur_param_rt->param_strings[i] = copy_string_rt;
	cur_param_ui->param_strings[i] = copy_string_ui;
    }
    
    return 0;
}

const char* param_get_param_string(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return NULL;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }
    if(val_id >= num_of_params)return NULL;

    PRM_PARAM cur_param = param_array[val_id];
    int cur_val = (int)cur_param.val;
    if(cur_val > cur_param.max_val || cur_val < cur_param.min_val)return NULL;
    if(cur_param.param_strings_num <= 0 || cur_val >= cur_param.param_strings_num || cur_val < 0)return NULL;
    
    return cur_param.param_strings[cur_val];
}
unsigned int param_get_value_as_string(PRM_CONTAIN* param_container, int val_id, char* ret_string, uint32_t string_len){
    if(string_len == 0)return 0;
    if(!ret_string)return 0;
    unsigned char val_type = 0;
    val_type = param_get_val_type(param_container, val_id, 0);
    if(val_type == 0)return 0;

    unsigned int curved = 0;
    if(val_type == Curve_Float_Return_Type)curved = 1;

    PARAM_T val = param_get_value(param_container, val_id, curved, 0, 0);

    //if there is a user provided function to convert the parameter value to string, use it
    if(param_container->user_data.user_data && param_container->user_data.val_to_string){
	if(param_container->user_data.val_to_string(param_container->user_data.user_data, val_id, val, ret_string, string_len) == 1)
	    return 1;
    }

    if(val_type == Uchar_type)snprintf(ret_string, string_len, "%02X", (unsigned int)val);
    
    if(val_type == Int_type)snprintf(ret_string, string_len, "%d", (int)val);
    
    if(val_type == Float_type)snprintf(ret_string, string_len, "%g", val);
    
    if(val_type == DB_Return_Type)snprintf(ret_string, string_len, "%0.3gDB", log10((double)val) * 20);
    
    if(val_type == Curve_Float_Return_Type)snprintf(ret_string, string_len, "%g", val);
    
    if(val_type == String_Return_Type){
	const char* param_string = param_get_param_string(param_container, val_id, 0);
	if(!param_string)return 0;
	snprintf(ret_string, string_len, "%s", param_string);
    }
    return 1;
}

int param_get_if_changed(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return -1;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }
    if(val_id >= num_of_params)return -1;

    PRM_PARAM cur_param = param_array[val_id];
    return cur_param.just_changed;
}

int param_get_if_any_changed(PRM_CONTAIN* param_container, unsigned int rt_params){
    if(!param_container)return -1;
    int params_changed = 0;
    unsigned int num_params = param_return_num_params(param_container, rt_params);
    if(num_params<=0)return -1;
    for(int i=0; i < num_params; i++){
	int changed = param_get_if_changed(param_container, i, rt_params);
	if(changed == 1){
	    params_changed = 1;
	    break;
	}
    }
    return params_changed;
}

unsigned int param_is_hidden(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return 0;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }
    if(!param_array)return 0;
    if(val_id >= num_of_params)return 0;
    
    PRM_PARAM cur_param = param_array[val_id];
    return cur_param.is_hidden;
}

unsigned int param_name_get_if_changed(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return 0;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }
    if(!param_array)return 0;
    if(val_id >= num_of_params)return 0;
    PRM_PARAM* cur_param = &(param_array[val_id]);
    unsigned int just_changed = cur_param->name_just_changed;
    cur_param->name_just_changed = 0;
    return just_changed;
}

unsigned int param_get_name(PRM_CONTAIN* param_container, int val_id, char* ret_name, uint32_t name_len){
    if(!param_container)return 0;
    PRM_PARAM* param_array = param_container->ui_params;
    int num_of_params = param_container->num_of_params_ui;
    
    if(val_id >= num_of_params)return 0;

    PRM_PARAM* cur_param = &(param_array[val_id]);
    snprintf(ret_name, name_len, "%s", cur_param->name);
    cur_param->name_just_changed = 0;
    return 1;
}

int param_find_name(PRM_CONTAIN* param_container, const char* param_name, unsigned int rt_params){
    if(!param_container)return -1;
    if(!param_name)return -1;
    PRM_PARAM* param_array = NULL;
    int num_of_params = -1;
    if(rt_params == 0){
	param_array = param_container->ui_params;
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	param_array = param_container->rt_params;
	num_of_params = param_container->num_of_params_rt;
    }

    for(int i = 0; i < num_of_params; i++){
	const char* cur_name = param_array[i].name;
	if(strcmp(param_name, cur_name)==0){
	    return i;
	}
    }
    return -1;
}

unsigned int param_return_num_params(PRM_CONTAIN* param_container, unsigned int rt_params){
    if(!param_container)return 0;
    int num_of_params = -1;
    if(rt_params == 0){
	num_of_params = param_container->num_of_params_ui;
    }
    if(rt_params == 1){
	num_of_params = param_container->num_of_params_rt;
    }

    return num_of_params;
}

static void param_clean_param(PRM_PARAM* param){
    if(!param)return;
    if(param->interp_val)free(param->interp_val);
    if(param->param_strings){
	for(int j = 0; j < param->param_strings_num; j++){
	    if(param->param_strings[j])free(param->param_strings[j]);
	}
	free(param->param_strings);
    }
}

void param_clean_param_container(PRM_CONTAIN* param_container){
    if(!param_container)return;
    for(int i = 0; i < param_container->num_of_params_ui; i++){
	param_clean_param(&(param_container->rt_params[i]));
	param_clean_param(&(param_container->ui_params[i]));
    }	
    ring_buffer_clean(param_container->param_rt_to_ui);
    ring_buffer_clean(param_container->param_ui_to_rt);
    if(param_container->rt_params)free(param_container->rt_params);
    if(param_container->ui_params)free(param_container->ui_params);
    
    free(param_container);
}
