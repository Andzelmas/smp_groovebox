#pragma once

// given an array_in of indices values, remove the index rem_val value
// so if array is [0, 1, 2, 3, 4] and rem_val is 3, array_in will be [0, 1, 2, 4, 5]
void arr_u_idx_array_member_remove(unsigned int *array_in, unsigned int array_count, unsigned int rem_val);

// given an array_in of indices values, add the index ins_val
// if array is [0, 1, 4, 5, 5] and ins_val is 3, array_in will be [0, 1, 3, 4, 5]
void arr_u_idx_array_member_insert(unsigned int *array_in, unsigned int array_count, unsigned int ins_val);
