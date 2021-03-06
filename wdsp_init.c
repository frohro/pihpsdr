/* Copyright (C)
* 2015 - John Melton, G0ORX/N6LYT
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/

#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if_arp.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <semaphore.h>
#include <math.h>

#include "alex.h"
#include "new_protocol.h"
#include "channel.h"
#include "discovered.h"
#include "mode.h"
#include "filter.h"
#include "wdsp.h"
#include "radio.h"
#include "vfo.h"
#include "toolbar.h"
#include "wdsp_init.h"
#ifdef FREEDV
#include "freedv.h"
#endif

#define PI 3.1415926535897932F
#define min(x,y) (x<y?x:y)
#define max(x,y) (x<y?y:x)

static int receiver;
static int running=0;

static int buffer_size=BUFFER_SIZE;
static int tx_buffer_size=BUFFER_SIZE;
static int fft_size=4096;
static int dspRate=48000;
static int outputRate=48000;
static int dvOutputRate=8000;

static int micSampleRate=48000;
static int micDspRate=48000;
static int micOutputRate=192000;

static int spectrumWIDTH=800;
static int SPECTRUM_UPDATES_PER_SECOND=10;

static void initAnalyzer(int channel,int buffer_size);

void setRXMode(int rx,int m) {
fprintf(stderr,"SetRXAMode: rx=%d mode=%d\n",rx,m);
  SetRXAMode(rx, m);
}

void setTXMode(int tx,int m) {
fprintf(stderr,"SetTXAMode: tx=%d mode=%d\n",tx,m);
  SetTXAMode(tx, m);
}

void setMode(int m) {
fprintf(stderr,"setMode: mode=%d m=%d\n",mode,m);
int local_mode=m;
#ifdef FREEDV
    if(mode!=modeFREEDV && m==modeFREEDV) {
      local_mode=modeUSB;
      init_freedv();
    } else if(mode==modeFREEDV && m!=modeFREEDV) {
      close_freedv();
    }
#endif
fprintf(stderr,"setMode: %d mode=%d\n",receiver,mode);
    setRXMode(receiver,local_mode);
fprintf(stderr,"setMode: %d mode=%d\n",CHANNEL_TX,mode);
    setTXMode(CHANNEL_TX,local_mode);
    mode=m;
}

int getMode() {
    return mode;
}

void setFilter(int low,int high) {
fprintf(stderr,"setFilter: %d %d\n",low,high);
    if(mode==modeCWL) {
        filterLow=-cwPitch-low;
        filterHigh=-cwPitch+high;
    } else if(mode==modeCWU) {
        filterLow=cwPitch-low;
        filterHigh=cwPitch+high;
    } else {
        filterLow=low;
        filterHigh=high;
    }

    double fl=filterLow+ddsOffset;
    double fh=filterHigh+ddsOffset;

fprintf(stderr,"setFilter: fl=%f fh=%f\n",fl,fh);
    RXANBPSetFreqs(receiver,(double)filterLow,(double)filterHigh);
    SetRXABandpassFreqs(receiver, fl,fh);
    SetRXASNBAOutputBandwidth(receiver, (double)filterLow, (double)filterHigh);

    SetTXABandpassFreqs(CHANNEL_TX, fl,fh);
}

int getFilterLow() {
    return filterLow;
}

int getFilterHigh() {
    return filterHigh;
}

void wdsp_set_offset(long long offset) {
    if(offset==0) {
      SetRXAShiftFreq(receiver, (double)offset);
      SetRXAShiftRun(receiver, 0);
    } else {
      SetRXAShiftFreq(receiver, (double)offset);
      SetRXAShiftRun(receiver, 1);
    }

    setFilter(filterLow,filterHigh);
}

void wdsp_set_input_rate(double rate) {
    SetInputSamplerate(receiver, (int)rate);
}

static void setupRX(int rx) {
    setRXMode(rx,mode);
    SetRXABandpassFreqs(rx, (double)filterLow, (double)filterHigh);
    SetRXAAGCMode(rx, agc);
    SetRXAAGCTop(rx,agc_gain);

    SetRXAAMDSBMode(rx, 0);
    SetRXAShiftRun(rx, 0);

    SetRXAEMNRPosition(rx, nr_agc);
    SetRXAEMNRgainMethod(rx, nr2_gain_method);
    SetRXAEMNRnpeMethod(rx, nr2_npe_method);
    SetRXAEMNRRun(rx, nr2);
    SetRXAEMNRaeRun(rx, nr2_ae);

    SetRXAANRVals(rx, 64, 16, 16e-4, 10e-7); // defaults
    SetRXAANRRun(rx, nr);
    SetRXAANFRun(rx, anf);
    SetRXASNBARun(rx, snb);
}

static void setupTX(int tx) {
    setTXMode(tx,mode);
    SetTXABandpassFreqs(tx, (double)filterLow, (double)filterHigh);
    SetTXABandpassWindow(tx, 1);
    SetTXABandpassRun(tx, 1);

    SetTXACFIRRun(tx, 1);
    SetTXAEQRun(tx, 0);
    SetTXACTCSSRun(tx, 0);
    SetTXAAMSQRun(tx, 0);
    SetTXACompressorRun(tx, 0);
    SetTXAosctrlRun(tx, 0);
    SetTXAPreGenRun(tx, 0);
    SetTXAPostGenRun(tx, 0);

    SetChannelState(tx,1,0);
    SetChannelState(tx,1,0);
}

void wdsp_init(int rx,int pixels,int protocol) {
    int rc;
    receiver=rx;
    spectrumWIDTH=pixels;

    fprintf(stderr,"wdsp_init: %d\n",rx);
   
    if(protocol==ORIGINAL_PROTOCOL) {
        micOutputRate=48000;
    } else {
        micOutputRate=192000;
    }

    while (gtk_events_pending ())
      gtk_main_iteration ();

    fprintf(stderr,"OpenChannel %d buffer_size=%d fft_size=%d sample_rate=%d dspRate=%d outputRate=%d\n",
                rx,
                buffer_size,
                fft_size,
                sample_rate,
                dspRate,
                outputRate);

    OpenChannel(rx,
                buffer_size,
                fft_size,
                sample_rate,
                dspRate,
                outputRate,
                0, // receive
                1, // run
                0.010, 0.025, 0.0, 0.010, 0);


    while (gtk_events_pending ())
      gtk_main_iteration ();

    switch(sample_rate) {
      case 48000:
        tx_buffer_size=BUFFER_SIZE;
        break;
      case 96000:
        tx_buffer_size=BUFFER_SIZE/2;
        break;
      case 192000:
        tx_buffer_size=BUFFER_SIZE/4;
        break;
      case 384000:
        tx_buffer_size=BUFFER_SIZE/8;
        break;
    }
    fprintf(stderr,"OpenChannel %d buffer_size=%d fft_size=%d sample_rate=%d dspRate=%d outputRate=%d\n",
                CHANNEL_TX,
                tx_buffer_size,
                fft_size,
                sample_rate, //micSampleRate,
                micDspRate,
                micOutputRate);

    OpenChannel(CHANNEL_TX,
                buffer_size,
                fft_size,
                sample_rate, //micSampleRate,
                micDspRate,
                micOutputRate,
                1, // transmit
                1, // run
                0.010, 0.025, 0.0, 0.010, 0);

    while (gtk_events_pending ())
      gtk_main_iteration ();

    fprintf(stderr,"XCreateAnalyzer %d\n",rx);
    int success;
    XCreateAnalyzer(rx, &success, 262144, 1, 1, "");
        if (success != 0) {
            fprintf(stderr, "XCreateAnalyzer %d failed: %d\n" ,rx,success);
        }
    initAnalyzer(rx,buffer_size);

    SetDisplayDetectorMode(rx, 0, display_detector_mode);
    SetDisplayAverageMode(rx, 0,  display_average_mode);
    
    calculate_display_average();
    //SetDisplayAvBackmult(rx, 0, display_avb);
    //SetDisplayNumAverage(rx, 0, display_average);

    while (gtk_events_pending ())
      gtk_main_iteration ();

    XCreateAnalyzer(CHANNEL_TX, &success, 262144, 1, 1, "");
        if (success != 0) {
            fprintf(stderr, "XCreateAnalyzer CHANNEL_TX failed: %d\n" ,success);
        }
    initAnalyzer(CHANNEL_TX,tx_buffer_size);

    setupRX(rx);
    setupTX(CHANNEL_TX);

}

static void initAnalyzer(int channel,int buffer_size) {
    int flp[] = {0};
    double KEEP_TIME = 0.1;
    int n_pixout=1;
    int spur_elimination_ffts = 1;
    int data_type = 1;
    int fft_size = 8192;
    int window_type = 4;
    double kaiser_pi = 14.0;
    int overlap = 2048;
    int clip = 0;
    int span_clip_l = 0;
    int span_clip_h = 0;
    int pixels=spectrumWIDTH;
    int stitches = 1;
    int avm = 0;
    double tau = 0.001 * 120.0;
    int MAX_AV_FRAMES = 60;
    int display_average = MAX(2, (int) MIN((double) MAX_AV_FRAMES, (double) SPECTRUM_UPDATES_PER_SECOND * tau));
    double avb = exp(-1.0 / (SPECTRUM_UPDATES_PER_SECOND * tau));
    int calibration_data_set = 0;
    double span_min_freq = 0.0;
    double span_max_freq = 0.0;

    int max_w = fft_size + (int) MIN(KEEP_TIME * (double) SPECTRUM_UPDATES_PER_SECOND, KEEP_TIME * (double) fft_size * (double) SPECTRUM_UPDATES_PER_SECOND);

    fprintf(stderr,"SetAnalyzer channel=%d\n",channel);
    SetAnalyzer(channel,
            n_pixout,
            spur_elimination_ffts, //number of LO frequencies = number of ffts used in elimination
            data_type, //0 for real input data (I only); 1 for complex input data (I & Q)
            flp, //vector with one elt for each LO frequency, 1 if high-side LO, 0 otherwise
            fft_size, //size of the fft, i.e., number of input samples
            buffer_size, //number of samples transferred for each OpenBuffer()/CloseBuffer()
            window_type, //integer specifying which window function to use
            kaiser_pi, //PiAlpha parameter for Kaiser window
            overlap, //number of samples each fft (other than the first) is to re-use from the previous
            clip, //number of fft output bins to be clipped from EACH side of each sub-span
            span_clip_l, //number of bins to clip from low end of entire span
            span_clip_h, //number of bins to clip from high end of entire span
            pixels, //number of pixel values to return.  may be either <= or > number of bins
            stitches, //number of sub-spans to concatenate to form a complete span
/*
            avm, //averaging mode
            display_average, //number of spans to (moving) average for pixel result
            avb, //back multiplier for weighted averaging
*/
            calibration_data_set, //identifier of which set of calibration data to use
            span_min_freq, //frequency at first pixel value8192
            span_max_freq, //frequency at last pixel value
            max_w //max samples to hold in input ring buffers
    );
}
