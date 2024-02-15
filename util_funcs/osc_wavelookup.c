/*
Code adapted (and copied) from www.earlevel.com wavetable oscillator series
Created by Nigel Redmon on 5/15/12
EarLevel Engineering: earlevel.com
Copyright 2012 Nigel Redmon
*/  

#include <math.h>
#include <stdlib.h>

#include "osc_wavelookup.h"
#include "../types.h"
#include "../util_funcs/log_funcs.h"
#include "math_funcs.h"
//maximum number of tables on the OSC_OBJ table per wave shape
#define WAVETABLE_SLOTS 32

//how much to oversample the wavetables
#define OVERSAMPLE 2
//what is the base frequency from which to build the tables
#define BASEFREQUENCY 20

typedef struct _wavetable{
    SAMPLE_T topFreq;
    int waveTableLen;
    SAMPLE_T* waveTable;
}OSC_WAVETABLE;

typedef struct _osc_wavetable_obj{
    SAMPLE_T sampleRate;

    int numWaveTables;
    OSC_WAVETABLE waveTables[WAVETABLE_SLOTS];
}OSC_OBJ;


static void defineSaw(int len, int numHarmonics, SAMPLE_T* ar, SAMPLE_T* ai){
    if(numHarmonics > (len >> 1)) numHarmonics = (len >> 1);
    //zeroe the nums
    for (int i = 0; i < len; i++){
	ai[i] = 0;
	ar[i] = 0;
    }

    for (int i = 1, j = len - 1; i <= numHarmonics; i++, j--){
	SAMPLE_T temp = -1.0 / i;
	ar[i] = -temp;
	ar[j] = temp;
    }
}

static void defineSqr(int len, int numHarmonics, SAMPLE_T* ar, SAMPLE_T* ai){
    if(numHarmonics > (len >> 1)) numHarmonics = (len >> 1);
    //zeroe the nums
    for (int i = 0; i < len; i++){
	ai[i] = 0;
	ar[i] = 0;
    }

    for (int i = 1, j = len - 1; i <= numHarmonics; i++, j--){
	SAMPLE_T temp = i & 0x01 ? 1.0 / i : 0.0;
	ar[i] = -temp;
	ar[j] = temp;
    }
}

static void defineTriang(int len, int numHarmonics, SAMPLE_T* ar, SAMPLE_T* ai){
    if(numHarmonics > (len >> 1)) numHarmonics = (len >> 1);
    //zeroe the nums
    for (int i = 0; i < len; i++){
	ai[i] = 0;
	ar[i] = 0;
    }

    SAMPLE_T sign = 1;
    for (int i = 1, j = len - 1; i <= numHarmonics; i++, j--){
	SAMPLE_T temp = i & 0x01 ? 1.0 / (i * i) * (sign = -sign) : 0.0;
	ar[i] = -temp;
	ar[j] = temp;
    }
}

/*
 in-place complex fft
 
 After Cooley, Lewis, and Welch; from Rabiner & Gold (1975)
 
 program adapted from FORTRAN 
 by K. Steiglitz  (ken@princeton.edu)
 Computer Science Dept. 
 Princeton University 08544
*/
static void fft(int N, SAMPLE_T* ar, SAMPLE_T* ai){
    int i, j, k, L;            /* indexes */
    int M, TEMP, LE, LE1, ip;  /* M = log N */
    int NV2, NM1;
    SAMPLE_T t;               /* temp */
    SAMPLE_T Ur, Ui, Wr, Wi, Tr, Ti;
    SAMPLE_T Ur_old;
    
    // if ((N > 1) && !(N & (N - 1)))   // make sure we have a power of 2
    
    NV2 = N >> 1;
    NM1 = N - 1;
    TEMP = N; /* get M = log N */
    M = 0;
    while (TEMP >>= 1) ++M;
    
    /* shuffle */
    j = 1;
    for (i = 1; i <= NM1; i++) {
        if(i<j) {             /* swap a[i] and a[j] */
            t = ar[j-1];     
            ar[j-1] = ar[i-1];
            ar[i-1] = t;
            t = ai[j-1];
            ai[j-1] = ai[i-1];
            ai[i-1] = t;
        }
        
        k = NV2;             /* bit-reversed counter */
        while(k < j) {
            j -= k;
            k /= 2;
        }
        
        j += k;
    }
    
    LE = 1.;
    for (L = 1; L <= M; L++) {            // stage L
        LE1 = LE;                         // (LE1 = LE/2) 
        LE *= 2;                          // (LE = 2^L)
        Ur = 1.0;
        Ui = 0.; 
        Wr = cos(M_PI/(float)LE1);
        Wi = -sin(M_PI/(float)LE1); // Cooley, Lewis, and Welch have "+" here
        for (j = 1; j <= LE1; j++) {
            for (i = j; i <= N; i += LE) { // butterfly
                ip = i+LE1;
                Tr = ar[ip-1] * Ur - ai[ip-1] * Ui;
                Ti = ar[ip-1] * Ui + ai[ip-1] * Ur;
                ar[ip-1] = ar[i-1] - Tr;
                ai[ip-1] = ai[i-1] - Ti;
                ar[i-1]  = ar[i-1] + Tr;
                ai[i-1]  = ai[i-1] + Ti;
            }
            Ur_old = Ur;
            Ur = Ur_old * Wr - Ui * Wi;
            Ui = Ur_old * Wi + Ui * Wr;
        }
    }
}

static int addWaveTable(OSC_OBJ* osc, int len, SAMPLE_T* waveTableIn, SAMPLE_T topFreq){
    if(osc->numWaveTables < WAVETABLE_SLOTS){
	SAMPLE_T* waveTable = malloc(sizeof(SAMPLE_T) * len);
	if(!waveTable)return -1;
	osc->waveTables[osc->numWaveTables].waveTableLen = len;
	osc->waveTables[osc->numWaveTables].topFreq = topFreq;

	for(long i = 0; i < len; i++){
	    waveTable[i] = waveTableIn[i];
	}
	osc->waveTables[osc->numWaveTables].waveTable = waveTable;

	++osc->numWaveTables;
	return 0;
    }

    return osc->numWaveTables;
}

static SAMPLE_T makeWaveTable(OSC_OBJ* osc, int len, SAMPLE_T* ar, SAMPLE_T* ai, SAMPLE_T scale, SAMPLE_T topFreq){
    fft(len, ar, ai);
    if(scale == 0.0){
	SAMPLE_T max = 0;
	for(int i = 0; i < len; i++){
	    SAMPLE_T temp = fabs((double)ai[i]);
	    if(max < temp) max = temp;
	}
	if(max > 0)scale = 1.0 / max * .999;
    }

    //normalize
    SAMPLE_T* wave = malloc(sizeof(SAMPLE_T) * len);
    if(wave){
	for(int i = 0; i < len; i++){
	    wave[i] = ai[i] * scale;
	}
	int addedWave = 0;
	addedWave = addWaveTable(osc, len, wave, topFreq);
	if(addedWave > 0)scale = 0.0;
	if(addedWave < 0)scale = -1.0;
	
	free(wave);
    }
    return scale;
}

OSC_OBJ* osc_init_osc_wavetable(int table_type, SAMPLE_T esr){
    OSC_OBJ* osc = (OSC_OBJ*)malloc(sizeof(OSC_OBJ));
    if(!osc)return NULL;
    osc->sampleRate = esr;
    osc->numWaveTables = 0;

    for(int idx = 0; idx < WAVETABLE_SLOTS; idx++){
	osc->waveTables[idx].topFreq = 0;
	osc->waveTables[idx].waveTableLen = 0;
	osc->waveTables[idx].waveTable = NULL;
    }
    
    //calc number of harmonics/partials
    int maxHarms = osc->sampleRate / (3.0 * BASEFREQUENCY) + 0.5;
    if(table_type == SIN_WAVETABLE) maxHarms = 1;
    
    //we need power of two
    unsigned int v = maxHarms;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    if(v < 256) v = 256;
    int tableLen = v * 2 * OVERSAMPLE;
    SAMPLE_T* ar = malloc(sizeof(SAMPLE_T) * tableLen);
    if(!ar){
	osc_clean_osc_wavetable(osc);
	return NULL;
    }    
    SAMPLE_T* ai = malloc(sizeof(SAMPLE_T) * tableLen);
    if(!ai){
	if(ar)free(ar);
	osc_clean_osc_wavetable(osc);
	return NULL;
    }

    SAMPLE_T topFreq = BASEFREQUENCY * 2.0 / osc->sampleRate;
    SAMPLE_T scale = 0.0; 
    for(; maxHarms >= 1; maxHarms >>=1){
	if(table_type == SIN_WAVETABLE || table_type == SAW_WAVETABLE) {
	    defineSaw(tableLen, maxHarms, ar, ai);
	}
	if(table_type == TRIANGLE_WAVETABLE){
	    defineTriang(tableLen, maxHarms, ar, ai);
	}
	if(table_type == SQUARE_WAVETABLE){
	    defineSqr(tableLen, maxHarms, ar, ai);
	}
	
	scale = makeWaveTable(osc, tableLen, ar, ai, scale, topFreq);
	if(scale < 0){
	    osc_clean_osc_wavetable(osc);
	    return NULL;
	}
	topFreq *= 2;
    }

    if(ar)free(ar);
    if(ai)free(ai);
    
    return osc;
}

void osc_updatePhase(OSC_OBJ* osc, SAMPLE_T* phasor, SAMPLE_T freq){
    if(!osc)return;

    SAMPLE_T phaseInc = freq / osc->sampleRate;
    
    *phasor += phaseInc;
    if(*phasor >= 1.0) *phasor -= 1.0;
    if(*phasor >= 1.0) *phasor = 0.0;
}

SAMPLE_T osc_getOutput(OSC_OBJ* osc, SAMPLE_T phasor, SAMPLE_T freq, int with_phaseOfs, SAMPLE_T phaseOfs){
    if(!osc)return 0.0;

    SAMPLE_T phaseInc = freq / osc->sampleRate;
    int waveTableIdx = 0;
    while((phaseInc >= osc->waveTables[waveTableIdx].topFreq) && (waveTableIdx < (osc->numWaveTables -1))){
	++waveTableIdx;
    }
    OSC_WAVETABLE* waveTable = &(osc->waveTables[waveTableIdx]);
    if(!waveTable)return 0.0;
    
    SAMPLE_T temp = phasor * waveTable->waveTableLen;
    SAMPLE_T samp = math_get_from_table_lerp(waveTable->waveTable, waveTable->waveTableLen, temp);
    if(with_phaseOfs == 0)return samp;

    SAMPLE_T offsetPhasor = phasor + phaseOfs;
    if(offsetPhasor >= 1.0) offsetPhasor -= 1.0;
    if(offsetPhasor >= 1.0) offsetPhasor = 0.0;
    
    temp = offsetPhasor * waveTable->waveTableLen;
    SAMPLE_T samp_ofs = math_get_from_table_lerp(waveTable->waveTable, waveTable->waveTableLen, temp);
    return samp - samp_ofs;
    
}

void osc_clean_osc_wavetable(OSC_OBJ* osc){
    if(!osc)return;
    for(int idx = 0; idx < WAVETABLE_SLOTS; idx++){
	if(osc->waveTables[idx].waveTable)free(osc->waveTables[idx].waveTable);
    }
    free(osc);
    
}
