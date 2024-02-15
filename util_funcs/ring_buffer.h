#pragma once
//the ring buffer sturct that holds the void data, the atomic count etc.
typedef struct _ring_fifo_buffer RING_BUFFER;
//initiate (malloc) the ring_buffer and return it, on fail returns NULL.
//data_array_size.
RING_BUFFER* ring_buffer_init(unsigned int single_data_size, unsigned int data_array_size);
//read single item from the ring_buffer data array, its important that the destination object is the same
//size as the single_data_size given to the ring_buffer_init function, this is why the user has to send
//the destination data size in the dest_size var
int ring_buffer_read(RING_BUFFER* ring_buf, void* const dest, unsigned int dest_size);
//write single item to the ring_buffer data array, its important that the source object is the same
//size as the single_data_size given to the ring_buffer_init function, this is why the user has to send
//the source data size in the source_size var
int ring_buffer_write(RING_BUFFER* ring_buf, const void* const source, unsigned int source_size);
//return the number how many items are in the ring_buffer data array at the moment
unsigned int ring_buffer_return_items(RING_BUFFER* ring_buf);
//clean the ring buffer, free its memory etc.
void ring_buffer_clean(RING_BUFFER* ring_buffer);
