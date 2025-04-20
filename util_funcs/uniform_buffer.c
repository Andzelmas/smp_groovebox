#include "uniform_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _ub_event{
    void* event_list; //the user data - a chunk of memory allocated for user items, that can be different sizes
    uint32_t total_size; //total size of the event_list in bytes
    uint32_t total_items; //total number of items in the event_list that can be filled
    uint32_t current_size; //what is the size of the event_list right now
    uint32_t items; //how many items there are currently;
    //offset of each item from the event_list start (items end address from the start of the event_list)
    uint32_t* item_addr;
    uint32_t* item_sizes; //size of each element in the event_list
}UB_EVENT;

void ub_clean(UB_EVENT* ub_ev){
    if(!ub_ev)return;
    if(ub_ev->event_list)free(ub_ev->event_list);
    if(ub_ev->item_addr)free(ub_ev->item_addr);
    if(ub_ev->item_sizes)free(ub_ev->item_sizes);
    free(ub_ev);
}

UB_EVENT* ub_init(uint32_t total_size, uint32_t num_of_items){
    UB_EVENT* ub_ev = calloc(1, sizeof(UB_EVENT));
    if(!ub_ev)return NULL;
    ub_ev->event_list = calloc(1, total_size);
    if(!ub_ev->event_list){
	ub_clean(ub_ev);
	return NULL;
    }
    ub_ev->total_size = total_size;
    ub_ev->total_items = num_of_items;
    ub_ev->current_size = 0;
    ub_ev->items = 0;
    ub_ev->item_addr = calloc(num_of_items, sizeof(uint32_t));
    if(!ub_ev->item_addr){
	ub_clean(ub_ev);
	return NULL;
    }
    ub_ev->item_sizes = calloc(num_of_items, sizeof(uint32_t));
    if(!ub_ev->item_sizes){
	ub_clean(ub_ev);
	return NULL;
    }
    
    return ub_ev;
}

int ub_push(UB_EVENT* ub_ev, const void* const source, uint32_t source_size){
    if(!ub_ev)return -1;
    if(!source)return -1;
    if((ub_ev->current_size + source_size) > ub_ev->total_size)return -1;
    if((ub_ev->items + 1) > ub_ev->total_items)return -1;
    
    uint32_t last_idx = 0;
    if(ub_ev->items > 0)last_idx = ub_ev->items - 1;

    uint32_t offset = ub_ev->item_addr[last_idx];
    if(offset == 0 && ub_ev->items > 0)return -1;

    uint32_t idx = 0;
    if(ub_ev->items > 0)idx = last_idx + 1;

    memcpy(ub_ev->event_list + offset, source, source_size);
    ub_ev->item_sizes[idx] = source_size;
    ub_ev->current_size += source_size;
    ub_ev->item_addr[idx] = ub_ev->current_size;
    ub_ev->items += 1;
    return 0; 
}

uint32_t ub_item_get_size(UB_EVENT* ub_ev, uint32_t idx){
    if(!ub_ev)return -1;
    if(idx >= ub_ev->items)return -1;

    return ub_ev->item_sizes[idx];
}

uint32_t ub_size(UB_EVENT* ub_ev){
    if(!ub_ev)return 0;
    return ub_ev->items;
}

void* ub_item_get(UB_EVENT* ub_ev, uint32_t idx){
    if(!ub_ev)return NULL;
    if(idx >= ub_ev->items)return NULL;

    uint32_t offset = 0;
    uint32_t item_size = ub_ev->item_sizes[idx];
    if(item_size == 0)return NULL;
    uint32_t item_end_addr = ub_ev->item_addr[idx];
    if(item_end_addr == 0)return NULL;

    offset = item_end_addr - item_size;
    
    return ub_ev->event_list + offset;
}

void ub_list_reset(UB_EVENT* ub_ev){
    if(!ub_ev)return;

    memset(ub_ev->event_list, 0, ub_ev->total_size);
    memset(ub_ev->item_addr, 0, sizeof(uint32_t) * ub_ev->total_items);
    memset(ub_ev->item_sizes, 0, sizeof(uint32_t) * ub_ev->total_items);
    ub_ev->current_size = 0;
    ub_ev->items = 0;
}
