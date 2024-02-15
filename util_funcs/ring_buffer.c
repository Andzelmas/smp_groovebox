#include "ring_buffer.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
//atomics library
#include <stdatomic.h>

typedef struct _ring_fifo_buffer{
    //array of void* data
    char* data;
    //size of single data member in the data array
    atomic_uint single_data_size;
    //the size of the data array (number of members of void data)
    atomic_uint data_array_size;
    //the atomic tracker of items, write head will increase this and read head will decrease
    atomic_uint items;
    //the position of the write head
    unsigned int w_pos;
    //the position of the read head
    unsigned int r_pos;
}RING_BUFFER;


RING_BUFFER* ring_buffer_init(unsigned int single_data_size, unsigned int data_array_size){
    RING_BUFFER* ret_ring = NULL;
    ret_ring = (RING_BUFFER*)malloc(sizeof(RING_BUFFER));
    if(ret_ring){
	ret_ring->data = (char*)malloc(single_data_size * data_array_size);

	atomic_init(&ret_ring->single_data_size, (unsigned int)single_data_size);
	atomic_init(&ret_ring->data_array_size, (unsigned int)data_array_size);
	atomic_init(&ret_ring->items, 0U);
	
	ret_ring->w_pos = 0;
	ret_ring->r_pos = 0;
    }
    
    return ret_ring;
}

int ring_buffer_read(RING_BUFFER* ring_buf, void* const dest, unsigned int dest_size){
    unsigned int single_data_size = atomic_load(&ring_buf->single_data_size);
    //the destination data size does not match with the single data size in the data array
    if(single_data_size != dest_size)return -3;
    unsigned int data_array_size = atomic_load(&ring_buf->data_array_size);
    if(single_data_size<=0 || data_array_size<=0)return -1;
    //empty data array
    if(atomic_load(&ring_buf->items)<=0)return 0;
    //overflow, how the hell this happened?
    if(ring_buf->r_pos >= data_array_size)return -2;
    
    memcpy(dest, ring_buf->data + (ring_buf->r_pos * single_data_size * sizeof(char)), single_data_size);
    ring_buf->r_pos += 1;
    if(ring_buf->r_pos == data_array_size){
	ring_buf->r_pos = 0;
    }
    
    atomic_fetch_sub(&ring_buf->items, 1);
    return 1;
}

int ring_buffer_write(RING_BUFFER* ring_buf, const void* const source, unsigned int source_size){
    unsigned int single_data_size = atomic_load(&ring_buf->single_data_size);
    //the source data size does not match with the single data size in the data array
    if(single_data_size != source_size)return -3;    
    unsigned int data_array_size = atomic_load(&ring_buf->data_array_size);
    if(single_data_size<=0 || data_array_size<=0)return -1;
    //full data array, return with exit code 0
    if(atomic_load(&ring_buf->items) >= data_array_size)return 0;
    
    memcpy(ring_buf->data + (ring_buf->w_pos * single_data_size * sizeof(char)), source, single_data_size);
    ring_buf->w_pos += 1;
    if(ring_buf->w_pos == data_array_size){
	ring_buf->w_pos = 0;
    }
    atomic_fetch_add(&ring_buf->items, 1);
    
    return 1;
}

unsigned int ring_buffer_return_items(RING_BUFFER* ring_buf){
    unsigned int items = 0;
    items = atomic_load(&ring_buf->items);
    return items;
}

void ring_buffer_clean(RING_BUFFER* ring_buffer){
    if(ring_buffer->data)free(ring_buffer->data);
    if(ring_buffer)free(ring_buffer);
}
