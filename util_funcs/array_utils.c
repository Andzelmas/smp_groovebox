#include "array_utils.h"

void arr_u_idx_array_member_remove(unsigned int *array_in, unsigned int array_count, unsigned int rem_val){
    if(!array_in)return;
    if(array_count == 0)return;
    if(rem_val >= array_count)return;

    unsigned int idx = array_count;
    // what idx is the rem_val value in the array_in array
    for(unsigned int i = 0; i < array_count; i++){
        unsigned int cur_val = array_in[i];
        if (cur_val == rem_val) {
            idx = i;
            break;
        }
    }
    if(idx >= array_count)return;

    // remove the idx member (with rem_val) from the array_in
    for(unsigned int i = idx; i < array_count; i++){
        if(i == array_count - 1){
            array_in[i] = array_count;
            continue;
        }
        array_in[i] = array_in[i+1];
    }
}

void arr_u_idx_array_member_insert(unsigned int *array_in, unsigned int array_count, unsigned int ins_val){
    if(!array_in)return;
    if(array_count == 0)return;
    if(ins_val >= array_count)return;

    // check if this ins_val value is not in the array_in already
    unsigned int found = 0;
    for(unsigned int i = 0; i < array_count; i++){
        if(array_in[i] != ins_val)continue;
        found = 1;
    }
    if(found == 1)return;

    unsigned int idx = array_count;
    // find in what idx ins_val needs to be inserted
    for(unsigned int i = 0; i < array_count; i++){
        if(array_in[i] > ins_val){
            idx = i;
            break;
        }
    }
    // now insert the ins_val value into the array_in
    for(unsigned int i = array_count - 1; i > idx; i--){
        array_in[i] = array_in[i - 1];
    }
    array_in[idx] = ins_val;
}
