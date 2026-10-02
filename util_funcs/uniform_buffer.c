#include "uniform_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _ub_event{
    unsigned char* event_list; //the user data - a chunk of memory allocated for user items, that can be different sizes
    uint32_t total_size; //total size of the event_list in bytes
    uint32_t total_items; //total number of items in the event_list that can be filled
    uint32_t current_size; //end of the last item from the event_list start
    uint32_t items; //how many items there are currently;
    uint32_t* item_addr; //offset of each item from the event_list start, UB_ALIGN aligned
    uint32_t* item_sizes; //size of each element in the event_list, without the alignment padding
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

void* ub_push_reserve(UB_EVENT* ub_ev, uint32_t size){
    if(!ub_ev)return NULL;
    if(size == 0)return NULL;
    if(ub_ev->items >= ub_ev->total_items)return NULL;

    //64 bit so neither the rounding nor the sum can wrap
    uint64_t offset = ((uint64_t)ub_ev->current_size + UB_ALIGN - 1) & ~(uint64_t)(UB_ALIGN - 1);
    if(offset + size > ub_ev->total_size)return NULL;

    uint32_t idx = ub_ev->items;
    ub_ev->item_addr[idx] = (uint32_t)offset;
    ub_ev->item_sizes[idx] = size;
    ub_ev->current_size = (uint32_t)offset + size;
    ub_ev->items += 1;
    return ub_ev->event_list + offset;
}

int ub_push(UB_EVENT* ub_ev, const void* const source, uint32_t source_size){
    if(!source)return -1;
    void* item = ub_push_reserve(ub_ev, source_size);
    if(!item)return -1;
    memcpy(item, source, source_size);
    return 0;
}

uint32_t ub_item_get_size(UB_EVENT* ub_ev, uint32_t idx){
    if(!ub_ev)return 0;
    if(idx >= ub_ev->items)return 0;

    return ub_ev->item_sizes[idx];
}

uint32_t ub_size(UB_EVENT* ub_ev){
    if(!ub_ev)return 0;
    return ub_ev->items;
}

void* ub_item_get(UB_EVENT* ub_ev, uint32_t idx){
    if(!ub_ev)return NULL;
    if(idx >= ub_ev->items)return NULL;

    return ub_ev->event_list + ub_ev->item_addr[idx];
}

void ub_list_reset(UB_EVENT* ub_ev){
    if(!ub_ev)return;

    ub_ev->current_size = 0;
    ub_ev->items = 0;
}
