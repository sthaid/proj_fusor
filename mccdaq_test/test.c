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

#include "util_mccdaq.h"
#include "util_misc.h"

#define MAX_DATA_STORE (10*500000)   // 10 secs of samples

static uint16_t data_store[MAX_DATA_STORE];
static uint32_t max_data_store;
static bool sigint;
static bool error;

static void sigint_handler(int sig);
static char * plot_line(uint16_t value);
static int32_t mccdaq_callback(uint16_t * data, int32_t max_data, bool error_flag);

// --------------------------------------------------------------

int32_t main(int argc, char ** argv)
{
    int32_t          ret, plot_start;
    size_t           len;
    bool             done;
    char             cmd_line[100], last_cmd_line[100], cmd_code;
    struct sigaction action;

    // init vars
    done = false;
    plot_start = 0;
    last_cmd_line[0] = '\0';

    // register signal handler for SIGINT
    bzero(&action, sizeof(action));
    action.sa_handler = sigint_handler;
    sigaction(SIGINT, &action, NULL);

    // init
    INFO("calling mccdaq_init\n");
    ret = mccdaq_init();
    if (ret != 0) {
        FATAL("mccdaq_init ret %d\n", ret);
    }

    // loop, get and process command
    // - g : get
    // - p : plot
    // - e,q : exit
    while (!done) {
        // get cmd_line, and
        // remove newline char at end
        if (sigint) break;  //XXX
        fputs("> ", stdout);
        fgets(cmd_line, sizeof(cmd_line), stdin);
        if (sigint) break;  //XXX
        len = strlen(cmd_line);
        if (len > 0) {
            cmd_line[len-1] = '\0';
        }

        // if cmd_line is zero len, then use last_cmd_line
        if (strlen(cmd_line) == 0) {
            strcpy(cmd_line, last_cmd_line);
        }

        // save cmd_line in last_cmd_line
        strcpy(last_cmd_line, cmd_line);

        // scan the cmd_line
        cmd_code = ' ';
        sscanf(cmd_line, "%c", &cmd_code);

        // process the cmd_code
        printf("XXX got cmd_line '%s' cmd_code='%c'\n", cmd_line, cmd_code);
        switch (cmd_code) {
        case 'g':
            // get data
            bzero(data_store, MAX_DATA_STORE*sizeof(uint16_t));
            max_data_store = 0;
            mccdaq_start(mccdaq_callback);
            while (max_data_store < MAX_DATA_STORE && !error) {
                usleep(10000);
                if (sigint) break;
            }
            // XXX may want to stop this
            plot_start = 0;
            break;
        case 'p': {
            // plot data
            int32_t i, plot_end;
            printf("PLOT\n");
            plot_end = plot_start + 49;  //XXX
            if (plot_end >= max_data_store) {
                plot_end = max_data_store-1;
            }
            for (i = plot_start; i <= plot_end; i++) {
                uint16_t value = data_store[i];
                printf("%8d: %5d: %s\n", i, value-2048, plot_line(value));
            }
            plot_start = plot_end + 1;
            break; }
        case 'e': case 'q':
            done = true;
            break;
        case ' ':
            break;
        default:
            printf("invalid cmd '%c'\n", cmd_code);
            break;
        }
    }
            
    // done
    return 0;
}
#if 0
    // XXX
    // XXX check for bogus values like 0 or -1
    // XXX plot
    // XXX commands    Get, Plot [<x>], Scan for discontinuity
    // XXX capture ctrl c for orderly exit
    int32_t i;
    FILE * fp = fopen("temp.txt", "w");
    if (fp == NULL) {
        ERROR("failed open temp.txt, %s\n", strerror(errno));
    } else {
        for (i = 0; i < max_data_store; i++) {
            fprintf(fp, "%6d:  %4d\n", i, data_store[i]-2048);
        }
        fclose(fp);
    }

    // XXX tbd
    //INFO("XXX tbd\n");
    //pause();

    // XXX print chart to file
#endif

static void sigint_handler(int sig)
{
    sigint = true;
}

static char * plot_line(uint16_t value)
{
    static char str[106];
    int32_t     idx, i;

    // value                : range 0 - 4095
    // idx = value/40 + 1   : range  1 - 103
    // plot_line:           :  0  1-51  52  53-103  104
    //                      :  |        ctr           |

    memset(str, ' ', sizeof(str));
    str[0]   = '|';
    str[104] = '|';
    str[105] = '\0';

    if (value > 4095) {
        WARN("value %d out of range\n", value);
        return str;
    }

    idx = value/40 + 1;
    i = 52;
    while (true) {
        str[i] = '*';
        if (i == idx) break;
        i += (idx > 52 ? 1 : -1);
    }

    return str;
}

static int32_t mccdaq_callback(uint16_t * data, int32_t max_data, bool error_flag)
{
    uint64_t        intvl, curr_us;
    int32_t         space_avail, copy_count;

    static uint64_t start_us, count
    
    DEBUG("max_data=%d eror_flag=%d\n", max_data, error_flag);

    if (error_flag) {
        ERROR("error occurred\n");
        error = error_flag;
        return 1;
    }

    curr_us = microsec_timer();
    intvl   = curr_us - start_us;
    count  += max_data;
    if (start_us == 0 || intvl >= 1000000) { 
        if (start_us != 0) {
            INFO("rate = %"PRId64" samples/sec\n", count * 1000000 / intvl);
        }
        start_us = curr_us;
        count = 0;
    }

    // XXX temp
    int32_t i;
    static int32_t call_count;
    call_count++;
    for (i = 0; i < max_data; i++) {
        if (data[i] > (2048+300)) {
            printf("%d: data[%d] = %d\n",
                call_count, i, data[i]-2048);
        }
    }

    return 0;

    if (max_data_store >= MAX_DATA_STORE) {
        FATAL("invalid max_data_store %d\n", max_data_store);
    }

    space_avail = MAX_DATA_STORE - max_data_store;
    copy_count = (space_avail <= max_data ? space_avail : max_data);
    memcpy(data_store+max_data_store, data, copy_count*2);
    max_data_store += copy_count;

    if (max_data_store == MAX_DATA_STORE) {
        INFO("data store is full, max_data_store=%d\n", max_data_store);
        return 1;
    }

    return 0;
}
