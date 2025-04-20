#pragma once
#include <stdint.h>

//struct that holds the user buffer with info such as total_size number of items, etc.
typedef struct _ub_event UB_EVENT;

//clean the memory of the ub_ev struct
void ub_clean(UB_EVENT* ub_ev);
//init the UB_EVENT and return address
UB_EVENT* ub_init(uint32_t total_size, uint32_t num_of_items);
//push an item into the ub_ev->event_list
int ub_push(UB_EVENT* ub_ev, const void* const source, uint32_t source_size);
//get the size of the idx item in the ub_ev->event_list, on error will return 0
uint32_t ub_item_get_size(UB_EVENT* ub_ev, uint32_t idx);
//get the item count in the ub_ev->event_list
uint32_t ub_size(UB_EVENT* ub_ev);
//get the idx item address as void* from ub_ev->event_list
void* ub_item_get(UB_EVENT* ub_ev, uint32_t idx);
//empty the ub_ev->event_list and other arrays, ub_ev->items and ub_ev->current_size becomes 0
//does not free the memory, just a reset before writing to and empty ub_ev->event_list 
void ub_list_reset(UB_EVENT* ub_ev);
