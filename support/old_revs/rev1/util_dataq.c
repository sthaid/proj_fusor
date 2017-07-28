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

#include <termios.h>
#include <fcntl.h>
#include <pthread.h>
#include <math.h>

#include "util_dataq.h"
#include "util_misc.h"

//#define DATAQ_SIMULATOR
#ifndef DATAQ_SIMULATOR

//
// defines
//

//#define ENABLE_TEST_THREAD

#define DATAQ_DEVICE   "/dev/serial/by-id/usb-0683_1490-if00"
#define STTY_SETTINGS  "4:0:14b2:0:3:1c:7f:15:1:0:1:0:11:13:1a:0:12:f:17:16:0:0:0:0:0:0:0:0:0:0:0:0:0:0:0:0"
#define MAX_RESP       100
#define MAX_ADC_CHAN   9            // channels 1 .. 8

//
// typedefs
//

typedef struct {
    int16_t * val;   // millivolts
    int32_t sum;
    int64_t sum_squares;
    int32_t idx;
} adc_t;

//
// variables
//

static int      fd;
static adc_t    adc[MAX_ADC_CHAN];
static int64_t  scan_count;
static bool     scan_okay;
static int32_t  scan_hz;
static int32_t  max_val;
static int32_t  max_slist_idx; 
static int32_t  slist_idx_to_adc_chan[8];
static bool     exitting;

//
// prototypes
//

static void dataq_exit_handler(void);
static void dataq_issue_cmd(char * cmd, char * resp);
static void * dataq_recv_data_thread(void * cx);
static void dataq_process_adc_raw(int32_t slist_idx, int32_t new_val);
static void * dataq_monitor_thread(void * cx);
#ifdef ENABLE_TEST_THREAD
static void * dataq_test_thread(void * cx);
#endif

// -----------------  DATAQ API ROUTINES  -----------------------------------------------

void dataq_init(float averaging_duration_sec, int32_t max_adc_chan, ...)
{
    char      cmd_str[100];
    char      resp[MAX_RESP];
    int32_t   best_srate_arg, i, cnt, len, total_len, duration, ret;
    char      adc_channels_str[100];
    char    * p;
    char      stop_buff[10000];
    pthread_t thread_id;
    va_list   ap;

    // scan valist for adc channel numbers
    max_slist_idx = max_adc_chan;
    va_start(ap, max_adc_chan);
    for (i = 0; i < max_slist_idx; i++) {
        slist_idx_to_adc_chan[i] = va_arg(ap, int32_t);
    }
    va_end(ap);

    // validate adc channel numbers
    for (i = 0; i < max_slist_idx; i++) {
        int32_t adc_chan = slist_idx_to_adc_chan[i];
        if (adc_chan < 1 || adc_chan >= MAX_ADC_CHAN) {
            FATAL("adc_chan %d is invalid\n", adc_chan);
        }
    }

    // debug print args
    p = adc_channels_str;
    for (i = 0; i < max_slist_idx; i++) {
        cnt = sprintf(p, "%d ",  slist_idx_to_adc_chan[i]);
        p += cnt;
    }
    INFO("averaging_duration_sec=%4.2f channels=%s\n",
         averaging_duration_sec, adc_channels_str);

    // setup serial port
    // - LATER perhaps use termios tcsetattr instead
    // - I first ran this program on Fedora20, and had no problem
    //   communicating to the dataq. When first run on RaspberryPi the
    //   comm to dataq failed. To resolve this I captured the stty settings
    //   on Fedora (stty -g) and apply them here.
    sprintf(cmd_str, "stty -F %s %s\n", DATAQ_DEVICE, STTY_SETTINGS);
    system(cmd_str);

    // open the dataq virtual com port
    fd = open(DATAQ_DEVICE, O_RDWR);
    if (fd < 0) {
        FATAL("failed to open %s, %s\n", DATAQ_DEVICE, strerror(errno));
    }

    // cleanup from prior run:
    // - issue 'stop' scanning command
    // - set non blocking
    // - read until get the response to the stop command
    // - clear non blocking
    len = write(fd, "stop\r", 5);
    if (len != 5) {
        FATAL("failed to write stop cmd, %s\n", strerror(errno));
    }

    ret = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (ret < 0) {
        FATAL("failed to set non blocking, %s\n", strerror(errno));
    }

    total_len = 0;
    duration = 0;
    while (true) {
        usleep(100000);
        len = read(fd, stop_buff+total_len, sizeof(stop_buff)-total_len);
        if (len > 0) {
            total_len += len;
            if (total_len >= 5 && strcmp(&stop_buff[total_len-5], "stop\r") == 0) {
                break;
            }
        }
        duration += 100000;
        if (duration >= 1000000) {
            FATAL("did not receive response to stop scanning cmd");
        }
    }        

    ret = fcntl(fd, F_SETFL, 0);
    if (ret < 0) {
        FATAL("failed to clear non blocking, %s\n", strerror(errno));
    }

    // issue 'info 0' command, 
    // this is just a dataq communication sanity check
    dataq_issue_cmd("info 0", resp);

    // configure scanning of the desired adc channels
    dataq_issue_cmd("asc", resp);
    for (i = 0; i < max_slist_idx; i++) {
        sprintf(cmd_str, "slist %d x%4.4x", i, slist_idx_to_adc_chan[i]-1);
        dataq_issue_cmd(cmd_str, resp);
    }

    // set the scan rate; 
    // use the max rate supported by the dataq, which is
    //   - best_srate_arg = 75 * num_enabled_scan_list_elements
    //   - scan_hz = 750000 / best_srate_cmd_arg
    best_srate_arg = 75 * max_slist_idx;
    sprintf(cmd_str, "srate x%4.4x", best_srate_arg);
    dataq_issue_cmd(cmd_str, resp);

    // determine scan_hz, which is used by the monitor_thread
    scan_hz = 750000 / best_srate_arg;

    // determine the number of adc values that are needed for the averaging_duration,
    // and allocate zeroed memory for them
    max_val = scan_hz * averaging_duration_sec;
    for (i = 0; i < max_slist_idx; i++) {
        int32_t adc_chan = slist_idx_to_adc_chan[i];
        adc[adc_chan].val = calloc(max_val, sizeof(int16_t));
        if (adc[adc_chan].val == NULL) {
            FATAL("alloc adc[%d].val failed, max_val=%d\n", adc_chan, max_val);
        }
    }
    
    // print
    INFO("scan_hz=%d max_val=%d\n", scan_hz, max_val);

    // configure binary output
    dataq_issue_cmd("bin", resp);

    // start scan
    write(fd, "start\r", 6);

    // stop scanning atexit
    atexit(dataq_exit_handler);

    // create threads 
    // - receive data from the adc
    // - monitor health of the dataq_recv_data_thread
    // - optional test code
    pthread_create(&thread_id, NULL, dataq_recv_data_thread, NULL);
    pthread_create(&thread_id, NULL, dataq_monitor_thread, NULL);
#ifdef ENABLE_TEST_THREAD
    pthread_create(&thread_id, NULL, dataq_test_thread, NULL);
#endif

    // delay to allow adc data to be available
    usleep(averaging_duration_sec*1000000 + 250000);

    // print success
    INFO("success\n");
}

int32_t dataq_get_adc(int32_t adc_chan,
                      int16_t * rms_mv,
                      int16_t * mean_mv, int16_t * sdev_mv,
                      int16_t * min_mv, int16_t * max_mv,
                      float history_secs, int16_t ** history_mv, int32_t * max_history_mv)
{
    int32_t i;
    adc_t * x;

    // validate adc_chan
    if (adc_chan < 1 || adc_chan >= MAX_ADC_CHAN || adc[adc_chan].val == NULL) {
        ERROR("adc_chan %d is not valid\n", adc_chan);
        return -1;
    }
    x = &adc[adc_chan];

    // if dataq scan is not working then return error
    if (!scan_okay) {
        ERROR("adc data not available\n");
        return -1;
    }

    // calculate rms 
    if (rms_mv) {
        *rms_mv = sqrtf((float)x->sum_squares / max_val);
    }

    // calculate mean voltage
    if (mean_mv) {
        *mean_mv = x->sum / max_val;
    }

    // calculate standad deviation voltage
    if (sdev_mv) {
        float u = (float)x->sum / max_val;
        *sdev_mv = sqrtf(((float)x->sum_squares / max_val) - (u * u));
    }

    // calculate min and max voltages
    if (min_mv || max_mv) {
        int16_t min = +32767;
        int16_t max = -32767;
        for (i = 0; i < max_val; i++) {
            if (x->val[i] < min) {
                min = x->val[i];
            }
            if (x->val[i] > max) {
                max = x->val[i];
            }
        }
        if (min_mv) {
            *min_mv = min;
        }
        if (max_mv) {
            *max_mv = max;
        }
    }

    // return history
    if (history_mv && max_history_mv) {
        int32_t max, idx, i;
        int16_t * buff;

        // preset returns
        *history_mv = NULL;
        *max_history_mv = 0;

        // determine number of history values to return based on requested history_secs
        max = history_secs * scan_hz;
        if (max >= max_val-10 || max <= 0) {
            ERROR("history_secs %f invalid, max=%d max_val=%d scan_hz=%d\n",
                  history_secs, max, max_val, scan_hz);
            return -1;
        }

        // allocate history buffer
        buff = malloc(max*sizeof(int16_t));
        if (buff == NULL) {
            FATAL("malloc failed, max=%d\n", max);
        }

        // copy values to history buff
        idx = (x->idx-1) - max + 1;
        if (idx < 0) {
            idx += max_val;
        }
        for (i = 0; i < max; i++) {
            buff[i] = x->val[idx++];
            if (idx >= max_val) {
                idx = 0;
            }
        }

        // set returns
        *history_mv = buff;
        *max_history_mv = max;
    }

    // return success
    return 0;
}

// -----------------  PRIVATE ROUTINES  -------------------------------------------------

static void dataq_exit_handler()
{
    int32_t len;

    // let the threads know program is exitting
    exitting = true;
    
    // at program exit, stop the dataq
    len = write(fd, "stop\r", 5);
    if (len != 5) {
        ERROR("issuing stop cmd\n");
    }

    // give some time to allow the stop cmd to be sent
    usleep(100000);

    // close
    close(fd);
}

static void dataq_issue_cmd(char * cmd, char * resp)
{
    char cmd2[100];
    int32_t len, ret, total_len, i;
    char *p;
    bool resp_recvd;

    // terminate command with <cr>
    strcpy(cmd2, cmd);
    strcat(cmd2, "\r");

    // write command
    len = write(fd, cmd2, strlen(cmd2));
    if (len != strlen(cmd2)) {
        FATAL("failed write cmd '%s', %s\n", cmd, strerror(errno));
    }

    // set non blocking
    ret = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (ret < 0) {
        FATAL("failed to set non blocking, %s\n", strerror(errno));
    }

    // read response, must terminate with <cr>, with 1 sec tout
    p = resp;
    total_len = 0;
    resp_recvd = false;
    for (i = 0; i < 200; i++) {
        len = read(fd, p, MAX_RESP-total_len);
        if (len > 0) {
            total_len += len;
            p += len;
            if (resp[total_len-1] == '\r') {
                resp[total_len-1] = '\0';   // remove <cr>, and null term
                total_len--;
                resp_recvd = true;
                break;
            }
        } else if (len == -1 && errno == EWOULDBLOCK) {
            // nothing to do here
        } else if (len == 0) {
            // nothing to do here
        } else {
            FATAL("failed read, len=%d, %s\n", len, strerror(errno));
        }
        usleep(5000);
    }

    // clear non blocking
    ret = fcntl(fd, F_SETFL, 0);
    if (ret < 0) {
        FATAL("failed to clear non blocking, %s\n", strerror(errno));
    }

    // check for failure to read response
    if (!resp_recvd) {
        FATAL("response to cmd '%s' was not received\n", cmd);
    }

    // check that response received was correct, it should match the cmd
    if (strncmp(cmd, resp, strlen(cmd)) != 0) {
        FATAL("response to cmd '%s', resp '%s', is incorrect\n", cmd, resp);
    }

    // debug print
    DEBUG("okay: cmd='%s', resp='%s'\n", cmd, resp);
}

static void * dataq_recv_data_thread(void * cx)
{
    uint8_t buff[1000];
    uint8_t *p;
    int32_t len, buff_len, slist_idx; 

    // init
    bzero(buff, sizeof(buff));
    buff_len = 0;

    // verify start response received
    len = read(fd, buff, 6);
    if (len != 6 || memcmp(buff, "start\r", 6) != 0) {
        FATAL("failed receive response to start, len=%d, %s\n", len, strerror(errno));
    }

    // loop, read and process adc data
    while (true) {
        // read adc data from dataq device into buff
        len = read(fd, buff+buff_len, sizeof(buff)-buff_len);
        if (len < 0) {
            FATAL("failed to read adc data, %s\n", strerror(errno));
        }
        buff_len += len;

        // if program is exitting then terminate this thread
        if (exitting) {
            return NULL;
        }

        // while there is enough data read for all sensors being scanned,
        // and to validate sync then extract adc values
        while (buff_len >= max_slist_idx*2+1) {
            // if not sychronized then abort for now
            // LATER improve this to resync, if needed
            if (((buff[0] & 1) != 0) || ((buff[max_slist_idx*2] & 1) != 0)) {
                FATAL("not synced\n");
            }

            // extract adc values from buff
            p = buff;
            for (slist_idx = 0; slist_idx < max_slist_idx; slist_idx++) {
                int32_t new_val;
    
                new_val = ((p[1] & 0xfe) << 4) | (p[0] >> 3);
                new_val ^= 0x800;
                if (new_val & 0x800) {
                    new_val |= 0xfffff000;
                }

                dataq_process_adc_raw(slist_idx, new_val);

                p += 2;
            }

            // shift buff, and adjust buff_len
            memmove(buff, buff+(max_slist_idx*2), buff_len-(max_slist_idx*2));
            buff_len -= (max_slist_idx*2);

            // bump up scan_count, which is used by the dataq_monitor_thread to
            // determine if scanning is working 
            scan_count++;
        }
    }

    return NULL;
}

static void dataq_process_adc_raw(int32_t slist_idx, int32_t new_val)
{
    int32_t new_mv, old_mv;
    int32_t adc_chan = slist_idx_to_adc_chan[slist_idx];
    adc_t * x = &adc[adc_chan];
    
    // convert new_val from raw to new_mv
    // note: a raw value of 2048 is equivalent to 10 v or 10000 mv
    new_mv = new_val * 10000 / 2048;

    // save adc value in circular buffer
    old_mv = x->val[x->idx];
    x->val[x->idx] = new_mv;
    x->idx = (x->idx + 1) % max_val;

    // update the sum of saved values, and
    // the sum^2 of saved values
    x->sum += (new_mv - old_mv);
    x->sum_squares += (new_mv*new_mv - old_mv*old_mv);
}

static void * dataq_monitor_thread(void * cx)
{
    uint64_t last_scan_count;
    uint64_t curr_scan_count;
    uint64_t delta_scan_count;
    int32_t  min_scan_hz;
    int32_t  max_scan_hz;

    last_scan_count = scan_count;
    min_scan_hz = scan_hz - scan_hz / 10;
    max_scan_hz = scan_hz + scan_hz / 10;
    DEBUG("min max scan hz %d %d\n", min_scan_hz, max_scan_hz);

    // loop forever
    while (true) {
        // sleep 1 second
        sleep(1);

        // if program is exitting then terminate this thread
        if (exitting) {
            return NULL;
        }

        // determine the amount scan_count has changed during the 1 sec sleep
        curr_scan_count = scan_count;
        delta_scan_count = curr_scan_count - last_scan_count;
        last_scan_count = curr_scan_count;

        // set scan_okay flag to true if delta_scan_count is within expected range,
        // and set to false otherwise
        if (delta_scan_count > min_scan_hz && delta_scan_count < max_scan_hz) {
            if (!scan_okay) {
                INFO("dataq adc scan okay, delta_scan_count=%"PRId64"\n", delta_scan_count);
            }
            scan_okay = true;
        } else {
            if (scan_okay) {
                ERROR("dataq adc scan failure, delta_scan_count=%"PRId64"\n", delta_scan_count);
            }
            scan_okay = false;
        }
    }

    return NULL;
}        
        
#ifdef ENABLE_TEST_THREAD
static void * dataq_test_thread(void * cx)
{
    adc_value_t adc_value;
    int32_t ret, adc_chan, i;

    // give the recdata_thread 1 second to acquire data
    sleep(1);

    // display voltage info once per second, on all registered channels
    while (true) {
        sleep(1);

        if (exitting) {
            return NULL;
        }

        printf("CHAN      RMS     MEAN      MIN       MAX\n");
        for (i = 0; i < max_slist_idx; i++) {
            adc_chan = slist_idx_to_adc_chan[i];
        
            ret = dataq_get_adc(adc_chan, &adc_value);
            if (ret == 0) {
                printf("%4d %8.3f %8.3f %8.3f %8.3f\n",
                        adc_chan, 
                        VOLTS(adc_value.rms),
                        VOLTS(adc_value.mean),
                        VOLTS(adc_value.min),
                        VOLTS(adc_value.max));
            } else {
                printf("%4d\n", adc_chan);
            }
        }
        printf("\n");
    }
    return NULL;
}
#endif

#else
// -----------------  SIMULATOR  -------------------------------------------------------

#define MAX_ADC_CHAN   9            // channels 1 .. 8

typedef struct {
    int16_t * val;   // millivolts
    int32_t sum;
    int64_t sum_squares;
    int32_t idx;
} adc_t;

static int32_t  max_slist_idx; 
static int32_t  slist_idx_to_adc_chan[8];
static adc_t    adc[MAX_ADC_CHAN];
static int32_t  max_val;
static bool     scan_okay = true;
static int32_t  scan_hz = 1000;

static void * dataq_simulator_thread(void * cx);
static void dataq_process_adc_raw(int32_t slist_idx, int32_t new_val);

void dataq_init(float averaging_duration_sec, int32_t max_adc_chan, ...)
{
    int32_t   i, cnt;
    char      adc_channels_str[100];
    char    * p;
    va_list   ap;
    pthread_t thread_id;

    // scan valist for adc channel numbers
    max_slist_idx = max_adc_chan;
    va_start(ap, max_adc_chan);
    for (i = 0; i < max_slist_idx; i++) {
        slist_idx_to_adc_chan[i] = va_arg(ap, int32_t);
    }
    va_end(ap);

    // validate adc channel numbers
    for (i = 0; i < max_slist_idx; i++) {
        int32_t adc_chan = slist_idx_to_adc_chan[i];
        if (adc_chan < 1 || adc_chan >= MAX_ADC_CHAN) {
            FATAL("adc_chan %d is invalid\n", adc_chan);
        }
    }

    // debug print args
    p = adc_channels_str;
    for (i = 0; i < max_slist_idx; i++) {
        cnt = sprintf(p, "%d ",  slist_idx_to_adc_chan[i]);
        p += cnt;
    }
    INFO("averaging_duration_sec=%4.2f channels=%s\n",
         averaging_duration_sec, adc_channels_str);

    // determine the number of adc values that are needed for the averaging_duration,
    // and allocate zeroed memory for them
    max_val = scan_hz * averaging_duration_sec;
    for (i = 0; i < max_slist_idx; i++) {
        int32_t adc_chan = slist_idx_to_adc_chan[i];
        adc[adc_chan].val = calloc(max_val, sizeof(int16_t));
        if (adc[adc_chan].val == NULL) {
            FATAL("alloc adc[%d].val failed, max_val=%d\n", adc_chan, max_val);
        }
    }
    INFO("scan_hz=%d max_val=%d\n", scan_hz, max_val);

    // create the simulator thread
    pthread_create(&thread_id, NULL, dataq_simulator_thread, NULL);
}

int32_t dataq_get_adc(int32_t adc_chan,
                      int16_t * rms_mv,
                      int16_t * mean_mv, int16_t * sdev_mv,
                      int16_t * min_mv, int16_t * max_mv,
                      float history_secs, int16_t ** history_mv, int32_t * max_history_mv)
{
    int32_t i;
    adc_t * x;

    // validate adc_chan
    if (adc_chan < 1 || adc_chan >= MAX_ADC_CHAN || adc[adc_chan].val == NULL) {
        ERROR("adc_chan %d is not valid\n", adc_chan);
        return -1;
    }
    x = &adc[adc_chan];

    // if dataq scan is not working then return error
    if (!scan_okay) {
        ERROR("adc data not available\n");
        return -1;
    }

    // calculate rms 
    if (rms_mv) {
        *rms_mv = sqrtf((float)x->sum_squares / max_val);
    }

    // calculate mean voltage
    if (mean_mv) {
        *mean_mv = x->sum / max_val;
    }

    // calculate standad deviation voltage
    if (sdev_mv) {
        float u = (float)x->sum / max_val;
        *sdev_mv = sqrtf(((float)x->sum_squares / max_val) - (u * u));
    }

    // calculate min and max voltages
    if (min_mv || max_mv) {
        int16_t min = +32767;
        int16_t max = -32767;
        for (i = 0; i < max_val; i++) {
            if (x->val[i] < min) {
                min = x->val[i];
            }
            if (x->val[i] > max) {
                max = x->val[i];
            }
        }
        if (min_mv) {
            *min_mv = min;
        }
        if (max_mv) {
            *max_mv = max;
        }
    }

    // return history
    if (history_mv && max_history_mv) {
        int32_t max, idx, i;
        int16_t * buff;

        // preset returns
        *history_mv = NULL;
        *max_history_mv = 0;

        // determine number of history values to return based on requested history_secs
        max = history_secs * scan_hz;
        if (max >= max_val-10 || max <= 0) {
            ERROR("history_secs %f invalid, max=%d max_val=%d scan_hz=%d\n",
                  history_secs, max, max_val, scan_hz);
            return -1;
        }

        // allocate history buffer
        buff = malloc(max*sizeof(int16_t));
        if (buff == NULL) {
            FATAL("malloc failed, max=%d\n", max);
        }

        // copy values to history buff
        idx = (x->idx-1) - max + 1;
        if (idx < 0) {
            idx += max_val;
        }
        for (i = 0; i < max; i++) {
            buff[i] = x->val[idx++];
            if (idx >= max_val) {
                idx = 0;
            }
        }

        // set returns
        *history_mv = buff;
        *max_history_mv = max;
    }

    // return success
    return 0;
}

static void * dataq_simulator_thread(void * cx)
{
    int32_t count = 0;
    int32_t triangle[20] = { -1000, -800, -600, -400, -200, 0, 200, 400, 600, 800, 1000,
                             800, 600, 400, 200, 0, -200, -400, -600, -800 };
    int32_t slist_idx;
    int32_t raw_val;

    while (true) {
        for (slist_idx = 0; slist_idx < max_slist_idx; slist_idx++) {
            raw_val = (slist_idx_to_adc_chan[slist_idx] * triangle[count]) / 5;
            dataq_process_adc_raw(slist_idx, raw_val);
        }

        if (++count == sizeof(triangle)/sizeof(int32_t)) {
            count = 0;
        }

        usleep(1000);
    }

    return NULL;
}

static void dataq_process_adc_raw(int32_t slist_idx, int32_t new_val)
{
    int32_t new_mv, old_mv;
    int32_t adc_chan = slist_idx_to_adc_chan[slist_idx];
    adc_t * x = &adc[adc_chan];
    
    // convert new_val from raw to new_mv
    // note: a raw value of 2048 is equivalent to 10 v or 10000 mv
    new_mv = new_val * 10000 / 2048;

    // save adc value in circular buffer
    old_mv = x->val[x->idx];
    x->val[x->idx] = new_mv;
    x->idx = (x->idx + 1) % max_val;

    // update the sum of saved values, and
    // the sum^2 of saved values
    x->sum += (new_mv - old_mv);
    x->sum_squares += (new_mv*new_mv - old_mv*old_mv);
}
#endif
