/*
Copyright (c) 2016 Steven Haid

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>

#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "util_mccdaq.h"
#include "util_misc.h"

//
// defines
//

#define MAX_CHANNEL             8
#define PULSE_THRESHOLD_NOT_SET 65535

//
// typedefs
//

enum mode { IDLE, INFO, PLOT, PULSEMON };

//
// variables
//

static enum mode mode;
static int32_t   mode_arg;
static bool      monitor_okay;
static uint64_t  total_samples_rcvd; 
static bool      sigint;
static int32_t   pulse_count[MAX_CHANNEL];
static uint64_t  pulse_time_start;
 
//
// prototypes
//

static void * monitor_thread(void * cx);
static void sigint_handler(int sig);
static int32_t mccdaq_callback(uint16_t * data, int32_t max_data);
static void print_plot_str(uint16_t value, uint16_t pulse_threshold);

// -----------------  MAIN  ------------------------------------------------

int32_t main(int argc, char ** argv)
{
    struct sigaction action;
    int32_t          ret;
    int32_t          ms;
    pthread_t        thread;

    // use line buffering
    setlinebuf(stdout);

    // register signal handler for SIGINT
    bzero(&action, sizeof(action));
    action.sa_handler = sigint_handler;
    sigaction(SIGINT, &action, NULL);

    // mccdaq init, and start
    INFO("calling mccdaq_init\n");
    ret = mccdaq_init();
    if (ret != 0) {
        FATAL("mccdaq_init ret %d\n", ret);
    }
    INFO("calling mccdaq_start\n");
    ret =  mccdaq_start(mccdaq_callback);
    if (ret != 0) {
        FATAL("mccdaq_start ret %d\n", ret);
    }

    // init pulse_time_start
    pulse_time_start = time(NULL);

    // create monitor thread, and 
    // wait for monitor thread to declare okay
    pthread_create(&thread, NULL, monitor_thread, NULL);
    for (ms = 0; ms < 5000 && !monitor_okay; ms++) {
        usleep(1000);
    }
    if (!monitor_okay) {
        FATAL("data not being received from mccdaq\n");
    }

    // loop, get and process command, each of
    // these cmds can be terminated by ctrl c
    // - info:           displays info once per second
    // - plot <samples>: plot raw adc data 
    // - pulsemon:       plot detected pulses
    // - reset:          reset pulse_count
    // - help:           display this 
    // - quit:           terminate program
    char *cmd_line = NULL;
    char *cmd, *arg;
    while (true) {
        // get cmd_line
        free(cmd_line);
        cmd_line = NULL;
        if ((cmd_line = readline("> ")) == NULL) {
            break;
        }
        if (sigint) {
            sigint = false;
            continue;
        }
        if (cmd_line[0] != '\0') {
            add_history(cmd_line);
        }

        // parse cmd_line
        cmd = strtok(cmd_line, " \n");
        if (cmd == NULL) {
            continue;
        }
        arg = strtok(NULL, " \n");

        // process cmd
        if (strcmp(cmd, "info") == 0) {
            mode = INFO;
        } else if (strcmp(cmd, "plot") == 0) {
            int32_t samples = 30;
            if (arg) {
                if (sscanf(arg, "%d", &samples) != 1 || samples <= 0) {
                    ERROR("'%s' is not a positive integer\n", arg);
                    continue;
                }
            }
            mode_arg = samples;
            mode = PLOT;
        } else if (strcmp(cmd, "pulsemon") == 0) {
            mode = PULSEMON;
        } else if (strcmp(cmd, "reset") == 0) {
            pulse_time_start = time(NULL);
            bzero(pulse_count, sizeof(pulse_count));
        } else if (strcmp(cmd, "help") == 0) {
            printf("cmds:\n");
            printf("- info:           displays info once per second\n");
            printf("- plot <samples>: plot raw adc data \n");
            printf("- pulsemon:       plot detected pulses\n");
            printf("- reset:          reset pulse_count\n");
            printf("- help:           display this\n");
            printf("- quit:           terminate program\n");
        } else if (strcmp(cmd, "quit") == 0) {
            break;
        } else {
            ERROR("cmd '%s' not supported\n", cmd);
        }

        // wait for cmd to compete 
        while (mode != IDLE) {
            usleep(1000);
        }
    }

    // terminate
    INFO("terminating\n");
    return 0;
}

void * monitor_thread(void * cx)
{
    uint64_t start, cnt;

    while (true) {
        start = total_samples_rcvd;
        sleep(1);
        cnt = total_samples_rcvd - start;

        monitor_okay = (cnt >= 450000 && cnt <= 510000);
        if (!monitor_okay) {
            ERROR("samples per sec %"PRId64" is out of range\n", cnt);
        }
    }
    return NULL;
}

static void sigint_handler(int sig)
{
    sigint = true;
}

// -----------------  MCCDAQ CALLBACK  -----------------------------------------------

static int32_t mccdaq_callback(uint16_t * d, int32_t max_d)
{
    #define MAX_DATA 1000000

    static uint16_t data[MAX_DATA];
    static int32_t  max_data;
    static int32_t  idx;
    static uint64_t time_last;
    static uint16_t pulse_threshold = PULSE_THRESHOLD_NOT_SET;
    
    DEBUG("max_data=%d\n", max_data);

    // keep track of total samples received
    total_samples_rcvd += max_d;

    // if mode is PLOT then plot the value;
    // mode_arg is the number of samples to plot, when this 
    // becomes zero then set mode to IDLE
    if (mode == PLOT) {
        int32_t i;
        for (i = 0; i < max_d; i++) {
            if (mode_arg <= 0) {
                mode_arg = 0;
                mode = IDLE;
                break;
            }
            print_plot_str(d[i], PULSE_THRESHOLD_NOT_SET);
            mode_arg--;
        }
    }

    // copy received data to array large enough for one second of data
    if (max_data + max_d > MAX_DATA) {
        FATAL("too much data %d+%d > %d\n", max_data, max_d, MAX_DATA);
    }
    memcpy(data+max_data, d, max_d*sizeof(uint16_t));
    max_data += max_d;

    // determine pule threshold ...
    //
    // if there is less than 1000 samples of data available for this second then
    //   return because we want 1000 or more samples to determine the pule_threshold
    // else if pulse_threshold is not set then
    //   determine the minimum he3 adc value for the 1000 samples, and
    //   set pulse_threshold to that baseline value plus 8
    // endif
    if (max_data < 1000) {
        return 0;  
    } else if (pulse_threshold == PULSE_THRESHOLD_NOT_SET) {
        int32_t i;
        int32_t baseline = 1000000000;
        for (i = 0; i < 1000; i++) {
            if (data[i] < baseline) {
                baseline = data[i];
            }
        }
        pulse_threshold = baseline + 8;
        DEBUG("pulse_threshold-2048=%d\n", pulse_threshold-2048);
    }

    // search for pulses in the data
    // - when pulse is found determine 
    //   . pulse_start_idx, pulse_end_idx
    //   . pulse_height
    //   . pulse_chan
    //   . pulse_count[MAX_CHANNEL]
    // - if mode is PULSEMON then 
    //      plot the pulse and print pulse height
    //   endif
    bool    in_a_pulse      = false;
    int32_t pulse_start_idx = -1;
    int32_t pulse_end_idx   = -1;
    while (true) {
        // terminate this loop when 
        // - near the end of data and not processing a pulse OR
        // - at the end of data
        if ((!in_a_pulse && idx >= max_data-10) || 
            (idx == max_data))
        {
            break;
        }

        // print warning if data out of range
        if (data[idx] > 4095) {
            WARN("data[%d] = %u, is out of range\n", idx, data[idx]);
            data[idx] = 2048;
        }

        // determine the pulse_start_idx and pulse_end_idx
        if (data[idx] >= pulse_threshold && !in_a_pulse) {
            in_a_pulse = true;
            pulse_start_idx = idx;
        } else if (data[idx] < pulse_threshold && in_a_pulse) {
            in_a_pulse = false;
            pulse_end_idx = idx - 1;
        }

        // if a pulse has been located ...
        if (pulse_end_idx != -1) {
            int32_t pulse_height, pulse_channel;
            int32_t i;

            // scan from start to end of pulse to determine pulse_height
            pulse_height = -1;
            for (i = pulse_start_idx; i <= pulse_end_idx; i++) {
                if (data[i] - pulse_threshold > pulse_height) {
                    pulse_height = data[i] - pulse_threshold;
                }
            }
                
            // determine pulse_channel from pulse_height
            pulse_channel = pulse_height / ((4096 - pulse_threshold) / MAX_CHANNEL);
            if (pulse_channel >= MAX_CHANNEL) {
                WARN("chan being reduced from %d to %d\n", pulse_channel, MAX_CHANNEL-1);
                pulse_channel = MAX_CHANNEL-1;
            }

            // keep track of pulse_count
            __sync_fetch_and_add(&pulse_count[pulse_channel], 1);

            // if mode is PULSEMON then plot this pulse
            if (mode == PULSEMON) {
                char time_str[MAX_TIME_STR];
                printf("%s: pulse: height=%d  channel=%d  threshold=%d\n", 
                       time2str(time_str, get_real_time_us(), false, true, true),
                       pulse_height, pulse_channel, pulse_threshold);
                for (i = pulse_start_idx; i <= pulse_end_idx; i++) {
                    print_plot_str(data[i], pulse_threshold);
                }
                printf("\n");
            }

            // done with this pulse
            pulse_start_idx = -1;
            pulse_end_idx = -1;
        }

        // move to next data 
        idx++;
    }

    // if time has incremented from last time this code block was run then ...
    uint64_t time_now = time(NULL);
    if (time_now > time_last) {
        int32_t chan;
        // if mode equal INFO then print stats
        if (mode == INFO) {
            int64_t duration_secs = time_now - pulse_time_start;

            printf("time=%d samples=%d  mccdaq_restarts=%d  pulse_threshold-2048=%d\n",
                   (int32_t)duration_secs,
                   max_data,
                   mccdaq_get_restart_count(),
                   pulse_threshold-2048);

            printf("  counts = ");
            for (chan = 0; chan < MAX_CHANNEL; chan++) {
                printf("%8d ", pulse_count[chan]);
            }
            printf("\n");

            printf("  cpm    = ");
            for (chan = 0; chan < MAX_CHANNEL; chan++) {
                printf("%8.2f ", pulse_count[chan] / (duration_secs / 60.0));
            }
            printf("\n");

            printf("\n");
        }

        // reset for next second of data
        time_last = time_now;
        max_data = 0;
        idx = 0;
        pulse_threshold = PULSE_THRESHOLD_NOT_SET;
    }

    // if sigint then set mode to IDLE
    if (sigint) {
        sigint = false;
        mode = IDLE;
        mode_arg = 0;
    }

    // return 0, so scanning continues
    return 0;
}

// -----------------  SUPPORT ROUTINES  -----------------------------------------------

static void print_plot_str(uint16_t value, uint16_t pulse_threshold)
{
    char    str[106];
    int32_t idx, i;

    // value                : range 0 - 4095
    // idx = value/40 + 1   : range  1 - 103
    // plot_str:            :  0  1-51  52  53-103  104
    //                      :  |        ^^           |

    memset(str, ' ', sizeof(str)-1);
    str[0]   = '|';
    str[104] = '|';
    str[105] = '\0';

    if (value > 4095) {
        printf("%5d: value is out of range\n", value-2048);
        return;
    }
    if (pulse_threshold > 4095 && pulse_threshold != PULSE_THRESHOLD_NOT_SET) {
        printf("%5d: pulse_threshold is out of range\n", value-2048);
        return;
    }

    idx = value/40 + 1;
    i = 52;
    while (true) {
        str[i] = '*';
        if (i == idx) break;
        i += (idx > 52 ? 1 : -1);
    }
    str[52] = '|';
    if (pulse_threshold != PULSE_THRESHOLD_NOT_SET) {
        str[pulse_threshold/40+1] = '|';
    }

    printf("%5d: %s\n", value-2048, str);
}
