#pragma once
#include <sndfile.h>
//loads a wav file to memory
//props - the properties that will be returned by the sndfile library, it will contain samplerate, channels etc.
//frame_buffer_s - the size of the single read of the buffer, will keep it around half a second
//path - the path to the file to load to the memory
//load_buffer the pointer to the array of the sample buffer, this will hold the address to the memory where the
//loaded buffer is
int load_wav_mem(SF_INFO* props,
                 const int frame_buffer_s,
                 const char *path,
                 float **load_buffer);
