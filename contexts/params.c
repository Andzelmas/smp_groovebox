#include "params.h"
#include "../types.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../util_funcs/log_funcs.h"
//the max length for param names
#define MAX_PARAM_NAME_LENGTH 100
//default speed per samples to interpolate the parameters when requested
#define INTERP_SAMPLES 400

typedef struct _params_interp_val{
    SAMPLE_T cur_inc; //how much to increment the value each sample cur_inc = max_range / total_samples. So smaller ranges will be interpolated faster
    SAMPLE_T from_val; //from what value we are interpolating
    SAMPLE_T to_val; //to what value interpolating
    SAMPLE_T cur_val; //cur value that will be returned to the user
    SAMPLE_T new_val; //new value that is given from the user, while the interpolation is not finished ignore this value
    int dir_mult; //to what direction to go, if from_val > to_val we have to cur_inc *= -1;
}PRM_INTERP_VAL;

typedef struct _params_param{
    //value of the parameter
    float val;
    float min_val;
    float max_val;
    float def_val; //default value
    //how much to increase or decrease the parameter
    float inc_am;
    //the type of the parameter value, used for ui display purposes
    //check the appReturnType in the types.h
    unsigned char val_type;
    //if this is 1 the parameter was just changed, this will change to 0 when get_value will be invoked
    unsigned int just_changed;
    char* name;
    //sometimes we might want to get an interpolated version of the parameter, so it does not change so quickly,
    //for example to avoid a click when changing amplitude of a synth oscillator
    PRM_INTERP_VAL* interp_val;
    //for exponential parameters this is where the table for the exp or any other curve should be
    //this table needs to be normalized (range 0..1) and malloced outside of this context and added here
    //with the param_add_curve_table
    MATH_RANGE_TABLE* curve_table;
    //for parameters that contain strings to display for user
    //max_val - min_val should be a whole number because this will be how many strings are in the string array
    char** param_strings;
}PRM_PARAM;

typedef struct _params_container{
    //the parameters arrays
    //rt_params should be touched only by the rt thread, and the ui_params only by the simple,
    //usually the ui thread
    PRM_PARAM** rt_params;
    PRM_PARAM** ui_params;
    //how many parameters are there
    unsigned int num_of_params;
}PRM_CONTAIN;

PRM_INTERP_VAL* params_init_interpolated_val(SAMPLE_T max_range, unsigned int total_samples){
    if(total_samples <= 0)return NULL;
    if(max_range <= 0)return NULL;
    PRM_INTERP_VAL* intrp_val = malloc(sizeof(PRM_INTERP_VAL));
    if(!intrp_val)return NULL;
    
    intrp_val->cur_inc = (SAMPLE_T)(max_range / (SAMPLE_T)total_samples);
    intrp_val->cur_val = 0.0;
    intrp_val->from_val = 0.0;
    intrp_val->to_val = 0.0;
    intrp_val->new_val = 0.0;
    intrp_val->dir_mult = 1;
    return intrp_val;
}

SAMPLE_T params_interp_val_get_value(PRM_INTERP_VAL* intrp_val, SAMPLE_T new_val){
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

PRM_CONTAIN* params_init_param_container(unsigned int num_of_params, char** param_names, float* param_vals,
					 float* param_mins, float* param_maxs, float* param_incs, unsigned char* val_types){
    if(num_of_params<=0) return NULL;
    if(!param_names || !param_vals || !param_mins || !param_maxs || !param_incs || !val_types) return NULL;
    PRM_CONTAIN* param_container = (PRM_CONTAIN*)malloc(sizeof(PRM_CONTAIN));
    if(!param_container)return NULL;
    param_container->num_of_params = num_of_params;
    param_container->rt_params = (PRM_PARAM**)malloc(sizeof(PRM_PARAM*) * num_of_params);
    if(!param_container->rt_params){
	if(param_container)free(param_container);
	return NULL;
    }
    param_container->ui_params = (PRM_PARAM**)malloc(sizeof(PRM_PARAM*) * num_of_params);
    if(!param_container->ui_params){
	if(param_container)free(param_container);
	if(param_container->rt_params)free(param_container->rt_params);
	return NULL;
    }
    for(int i = 0; i< num_of_params; i++){
	param_container->rt_params[i] = (PRM_PARAM*)malloc(sizeof(PRM_PARAM));
	if(!param_container->rt_params[i])continue;
	param_container->ui_params[i] = (PRM_PARAM*)malloc(sizeof(PRM_PARAM));
	if(!param_container->ui_params[i]){
	    if(param_container->rt_params[i])free(param_container->rt_params[i]);
	    continue;
	}

	param_container->rt_params[i]->interp_val = params_init_interpolated_val(fabs(param_maxs[i] - param_mins[i]) , INTERP_SAMPLES);
	if(!param_container->rt_params[i]->interp_val){
	    if(param_container->rt_params[i])free(param_container->rt_params[i]);
	    if(param_container->ui_params[i])free(param_container->ui_params[i]);
	    continue;
	}	
	param_container->ui_params[i]->interp_val = params_init_interpolated_val(fabs(param_maxs[i] - param_mins[i]) , INTERP_SAMPLES);
	if(!param_container->ui_params[i]->interp_val){
	    if(param_container->rt_params[i]->interp_val)free(param_container->rt_params[i]->interp_val);
	    if(param_container->rt_params[i])free(param_container->rt_params[i]);
	    if(param_container->ui_params[i])free(param_container->ui_params[i]);
	    continue;
	}
	param_container->rt_params[i]->param_strings = NULL;
	param_container->ui_params[i]->param_strings = NULL;
	if((val_types[i] & 0xff) == String_Return_Type){
	    unsigned int len = (param_maxs[i] - param_mins[i]) + 1;
	    if(len > 0){
		param_container->rt_params[i]->param_strings = malloc(sizeof(char*) * len);
		param_container->ui_params[i]->param_strings = malloc(sizeof(char*) * len);
	    }
	}

	param_container->rt_params[i]->curve_table = NULL;
	param_container->ui_params[i]->curve_table = NULL;
	
	param_container->rt_params[i]->just_changed = 0;
	param_container->ui_params[i]->just_changed = 0;
		
	param_container->rt_params[i]->val = param_vals[i];
	param_container->ui_params[i]->val = param_vals[i];
	
	param_container->rt_params[i]->min_val = param_mins[i];
	param_container->ui_params[i]->min_val = param_mins[i];
	
	param_container->rt_params[i]->max_val = param_maxs[i];
	param_container->ui_params[i]->max_val = param_maxs[i];

	param_container->rt_params[i]->def_val = param_vals[i];
	param_container->ui_params[i]->def_val = param_vals[i];
	
	param_container->rt_params[i]->inc_am = param_incs[i];
	param_container->ui_params[i]->inc_am = param_incs[i];
	
	param_container->rt_params[i]->val_type = val_types[i];
	param_container->ui_params[i]->val_type = val_types[i];
	
	const char* param_name = param_names[i];
	param_container->rt_params[i]->name = (char*)malloc(sizeof(char) * (strlen(param_name)+1));
	if(!param_container->rt_params[i]->name)continue;
	param_container->ui_params[i]->name = (char*)malloc(sizeof(char) * (strlen(param_name)+1));
	if(!param_container->ui_params[i]->name){
	    if(param_container->rt_params[i]->name)free(param_container->rt_params[i]->name);
	    continue;
	}
	strcpy(param_container->rt_params[i]->name, param_name);
	strcpy(param_container->ui_params[i]->name, param_name);
    }

    return param_container;
}

int param_add_curve_table(PRM_CONTAIN* param_container, int val_id, MATH_RANGE_TABLE* table){
    if(!param_container)return -1;
    
    if(val_id >= param_container->num_of_params)return -1;
    PRM_PARAM** param_array_rt = param_container->rt_params;
    PRM_PARAM** param_array_ui = param_container->ui_params;
    if(!param_array_rt || !param_array_ui)return -1;

    PRM_PARAM* cur_param_rt = param_array_rt[val_id];
    PRM_PARAM* cur_param_ui = param_array_ui[val_id];
    if(!cur_param_rt || !cur_param_ui)return -1;
    
    cur_param_rt->curve_table = table;
    cur_param_ui->curve_table = table;

    return 0;
}

int param_set_value(PRM_CONTAIN* param_container, int val_id, SAMPLE_T set_to, unsigned char param_op,
		    unsigned int rt_params){
    if(!param_container)return -1;
    
    if(val_id >= param_container->num_of_params)return -1;
    PRM_PARAM** param_array = param_container->rt_params;
    //if we dont want the rt_params
    if(rt_params == 0)param_array = param_container->ui_params;

    PRM_PARAM* cur_param = param_array[val_id];
    SAMPLE_T prev_value = cur_param->val;

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
    default:
	cur_param->val = cur_param->val;
    }

    if(cur_param->val < cur_param->min_val)cur_param->val = cur_param->min_val;
    if(cur_param->val > cur_param->max_val)cur_param->val = cur_param->max_val;
    //if the value changed mark this param as changed
    if(prev_value != cur_param->val){
	cur_param->just_changed = 1;
    }
    return 0;    
}

SAMPLE_T param_get_value(PRM_CONTAIN* param_container, int val_id, unsigned char* val_type,
			 unsigned int curved, unsigned int interp, unsigned int rt_params){
    if(!param_container)return -1;
    
    if(val_id >= param_container->num_of_params)return -1;
    PRM_PARAM** param_array = param_container->rt_params;
    //if we dont want the rt_params
    if(rt_params == 0)param_array = param_container->ui_params;

    PRM_PARAM* cur_param = param_array[val_id];
    *val_type = cur_param->val_type;
    //when returning the value we mark this param as no longer just_changed
    cur_param->just_changed = 0;

    SAMPLE_T ret_val = cur_param->val;
    //check if this parameter is of exponential or other curve nature
    if(curved == 1){
	if(cur_param->curve_table){
	    //if there is a curve table
	    SAMPLE_T val_min = cur_param->min_val;
	    SAMPLE_T val_max = cur_param->max_val;
	    //first make the param range 0..1
	    SAMPLE_T val_norm = fit_range(val_max, val_min, 1.0, 0.0, ret_val);
	    //now get what this value is in the table
	    SAMPLE_T val_curve = math_range_table_convert_value(cur_param->curve_table, val_norm);
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

int param_set_param_strings(PRM_CONTAIN* param_container, int val_id, char** strings){
    if(!param_container)return -1;
    if(!strings)return -1;
    if(val_id >= param_container->num_of_params)return -1;
    PRM_PARAM** param_array_rt = param_container->rt_params;
    PRM_PARAM** param_array_ui = param_container->ui_params;

    PRM_PARAM* cur_param_rt = param_array_rt[val_id];
    PRM_PARAM* cur_param_ui = param_array_ui[val_id];
    unsigned char val_type = cur_param_ui->val_type;
    if((val_type & 0xff) != String_Return_Type)return -1;
    if(!cur_param_rt->param_strings || !cur_param_ui->param_strings)return -1;

    unsigned int len = abs(cur_param_rt->max_val - cur_param_rt->min_val) + 1;
    if(len <= 0)return -1;
    
    for(int i = 0; i < len; i++){
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
    if(val_id >= param_container->num_of_params)return NULL;
    PRM_PARAM** param_array = param_container->rt_params;
    if(rt_params == 0) param_array = param_container->ui_params;

    PRM_PARAM* cur_param = param_array[val_id];
    int cur_val = cur_param->val;
    unsigned int len = abs(cur_param->max_val - cur_param->min_val) + 1;
    if(len <= 0 || cur_val >= len || cur_val < 0)return NULL;
        
    return cur_param->param_strings[cur_val];
}

int param_get_if_changed(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return -1;
    
    if(val_id >= param_container->num_of_params)return -1;
    PRM_PARAM** param_array = param_container->rt_params;
    //if we dont want the rt_params
    if(rt_params == 0)param_array = param_container->ui_params;

    PRM_PARAM* cur_param = param_array[val_id];
    if(!cur_param)return -1;
    return cur_param->just_changed;
}

int param_get_if_any_changed(PRM_CONTAIN* param_container, unsigned int rt_params){
    if(!param_container)return -1;
    int params_changed = 0;
    unsigned int num_params = param_return_num_params(param_container);
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

const char* param_get_name(PRM_CONTAIN* param_container, int val_id, unsigned int rt_params){
    if(!param_container)return NULL;

    if(val_id >= param_container->num_of_params)return NULL;
    PRM_PARAM** param_array = param_container->rt_params;
    //if we dont want the rt_params
    if(rt_params == 0)param_array = param_container->ui_params;

    PRM_PARAM* cur_param = param_array[val_id];
    const char* cur_name = cur_param->name;
    return cur_name;
}

int param_find_name(PRM_CONTAIN* param_container, const char* param_name, unsigned int rt_params){
    if(!param_container)return -1;
    if(!param_name)return -1;
    PRM_PARAM** param_array = param_container->rt_params;
    //if we dont want the rt_params
    if(rt_params == 0)param_array = param_container->ui_params;
    for(int i = 0; i < param_container->num_of_params; i++){
	const char* cur_name = param_array[i]->name;
	if(strcmp(param_name, cur_name)==0){
	    return i;
	}
    }
    return -1;
}

unsigned int param_return_num_params(PRM_CONTAIN* param_container){
    if(!param_container)return 0;
    return param_container->num_of_params;    
}

static void param_clean_param(PRM_PARAM* param){
    if(!param)return;
    if(param->interp_val)free(param->interp_val);
    if(param->name)free(param->name);
    if(param->param_strings){
	unsigned int len = (param->max_val - param->min_val) + 1;
	if(len > 0){
	    for(int j = 0; j < len; j++){
		if(param->param_strings[j])free(param->param_strings[j]);
	    }
	}
	free(param->param_strings);
    }
}

void param_clean_param_container(PRM_CONTAIN* param_container){
    if(!param_container)return;
    for(int i = 0; i < param_container->num_of_params; i++){
	if(param_container->rt_params[i]){
	    param_clean_param(param_container->rt_params[i]);
	    free(param_container->rt_params[i]);
	}
	
	if(param_container->ui_params[i]){
	    param_clean_param(param_container->ui_params[i]);
	    free(param_container->ui_params[i]);
	}	
    }
    
    if(param_container->rt_params)free(param_container->rt_params);
    if(param_container->ui_params)free(param_container->ui_params);
    
    free(param_container);
}
