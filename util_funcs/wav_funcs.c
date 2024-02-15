#include <stdio.h>
#include <stdlib.h>

//my own libraries
#include "wav_funcs.h"
#include "log_funcs.h"
int load_wav_mem(SF_INFO* props,
                 const int frame_buffer_s,
                 const char *path,
                 float **load_buffer){
        int return_var = 0;

        //read file
	SNDFILE *ifd = NULL;
	//set the format to zero before reading so to not get any undefined behaviours
	props->format = 0;
        ifd = sf_open(path, SFM_READ, props);
        if(!ifd){
	    int err = sf_error(ifd);
	    log_append_logfile("Failed to open file %s\n",sf_error_number(err));
	    return_var = -2;
	    goto cleanup;
        }

        int one_sample_size = props->channels*sizeof(float);
        float *currentBuf = (float*)malloc(one_sample_size*frame_buffer_s);
        if(!currentBuf){
            log_append_logfile("Failed to alloc the memory for currentBuf\n");
            return_var = -3;
            goto cleanup;
        }
        
        int framesread = 0;

        int place_bytes = 0;
        float* temp_buf = currentBuf;
        int curSize = one_sample_size*frame_buffer_s;
        
        while(1){ 
            framesread = sf_readf_float(ifd, currentBuf+place_bytes, frame_buffer_s);
            if(framesread<0){
		log_append_logfile("Could not read samples from file\n");
		return_var = -4;
		free(currentBuf);
		goto cleanup;
            }
            if(framesread==0)break;
            
            //resize the temp_buf and if its not null resize the currentBuf
            //so temp_buf is like a safety measure
            curSize += framesread*one_sample_size;
            temp_buf = realloc(currentBuf, curSize);
            if(!temp_buf){
                log_append_logfile("Failed to increase memory for the wav reader\n");
                free(currentBuf);
                return_var = -5;
                goto cleanup;
            }
            currentBuf = temp_buf;
            
            //how many members to move the pointer
            place_bytes+=framesread*props->channels;
        }
        /*since we read to know how many frames we've read
         * when the head reads less than frame_buffer_s frames we will have 
         * too much memory allocated, the place_bytes will have the correct
         * amount we just need to multiply by the sizeof(float)  */
        if(place_bytes>0)
            temp_buf = realloc(currentBuf,(place_bytes)*sizeof(float)+1);
        if(!temp_buf){
            log_append_logfile("Failed to realloc the currentBuf memory after reading the file\n");
            free(currentBuf);
            return_var = -6;
            goto cleanup;
        }
        currentBuf = temp_buf;
        //now we send the currentBuf accumulated file to our load_buffer
        if(currentBuf){
            //first free the sample buffer that is already there, so no memory leaks are created
            if(*load_buffer){
                free(*load_buffer);
                *load_buffer = NULL;
            }
            *load_buffer = currentBuf;
        }
        
        //if place_bytes is > 0 it will hold the size of the sample array
        if(place_bytes>0)return_var = place_bytes;
        
        cleanup: 
        if(ifd)sf_close(ifd);
        
        return return_var;
}


