/*
Copyright (c) 2019 Steven Haid

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
#include <pthread.h>

#include "common.h"
#include "util_owon_b35.h"
#include "util_misc.h"

// NOTES:
//
// Inspection of the code found here: https://github.com/inflex/owon-b35
// was very helpful in designing this code.
//
// Use 'hcitool lescan' to determine the bluetooth addresses that are
// in supplied in common.h.
//
// This program uses popen to run gatttol. Gatttool acquires the data
// from the OWON B35T meter. The meter_thread decodes the data and writes
// the meter reading in the meter[] array. The owon_b35_get_reading() 
// routine returns the meter reading from the meter[] array as long as the
// reading is recent and in the desired units.
//
// Here is an example of the output from gatttool:
//    gatttool -b 98:84:E3:CD:B8:68 --char-read --handle 0x2d --listen
//    Notification handle = 0x002e value: 2d 30 30 30 30 20 34 31 00 80 40 00 0d 0a 
//    Notification handle = 0x002e value: 2b 30 30 30 30 20 34 31 00 80 40 00 0d 0a 
//
// The meter_thread only decodes voltage and current.

//
// defines
//

#define MAX_METER 4

//
// typedefs
//

typedef struct {
    char     bluetooth_addr[100];
    char     meter_name[100];
    uint64_t reading_time_us;
    int32_t  value_type;
    double   value;
} meter_t;

//
// varialbles
//

meter_t meter[MAX_METER];
pthread_mutex_t owon_b35_mutex = PTHREAD_MUTEX_INITIALIZER;

//
// prototypes
//

static void * meter_thread(void *cx);

// -----------------  GET CURRENT READING  -----------------------------------------

double owon_b35_get_reading(char *bluetooth_addr, int desired_value_type, char *meter_name)
{
    meter_t *m;
    int i;
    pthread_t thread;
    uint64_t time_now_us;

    static uint64_t last_gatttool_start_us;

    // locate meter data struct for the bluetooth_addr specified by caller;
    // if found then return the meter data
    for (i = 0; i < MAX_METER; i++) {
        m = &meter[i];
        if (strcmp(bluetooth_addr, m->bluetooth_addr) == 0) {
            // if the monitor thread hasn't published a meter reading 
            // within the past 2 seconds then return ERROR_NO_VALUE
            if (microsec_timer() - m->reading_time_us > 2000000) {
                return ERROR_NO_VALUE;
            }

            // if the published meter reading is not of the desired_value_type
            // then return ERROR_NO_VALUE
            if (m->value_type != desired_value_type) {
                return ERROR_NO_VALUE;
            }

            // return the published meter reading
            return m->value;
        }
    }

    // the meter thread has not been created, do so ...

    // acquire mutex
    pthread_mutex_lock(&owon_b35_mutex);

    // find a free entry in meter table
    for (i = 0; i < MAX_METER; i++) {
        m = &meter[i];
        if (m->bluetooth_addr[0] == '\0') {
            break;
        }
    }
    if (i == MAX_METER) {
        FATAL("no free meter entries\n");
    }

    // if a gatttool has been started within past 5 sec then 
    // don't start new gatttool now
    time_now_us = microsec_timer();
    if (time_now_us - last_gatttool_start_us < 5000000) {
        pthread_mutex_unlock(&owon_b35_mutex);
        return ERROR_NO_VALUE;
    }

    // init the meter table entry, and create the meter_thread
    memset(m, 0, sizeof(meter_t));
    strcpy(m->bluetooth_addr, bluetooth_addr);
    strcpy(m->meter_name, meter_name);
    DEBUG("gatttool thread created for %s / %s\n", m->bluetooth_addr, m->meter_name);
    pthread_create(&thread, NULL, meter_thread, m);
    last_gatttool_start_us = time_now_us;

    // release mutex
    pthread_mutex_unlock(&owon_b35_mutex);

    // return NO_VALUE
    return ERROR_NO_VALUE;
}

static void * meter_thread(void *cx)
{
    meter_t *m = cx;
    FILE *fp;
    char cmd[1000], str[1000];
    int cnt;
    unsigned int d[14];
    double value;
    int value_type;

    // this code decodes only the following:
    // - DC voltage
    // - DC current
    // other meter features, such as resistance, temperature, delta-mode, hold-mode
    // are not supported

    // start gatttool
    sprintf(cmd, "gatttool -b %s --char-read --handle 0x2d --listen", m->bluetooth_addr);
    fp = popen(cmd,"r");
    if (fp == NULL) {
        FATAL("failed popen '%s'\n", cmd);
    }

    // loop forever, reading bluetooth data from the meter and
    // decoding the data
    while (fgets(str, sizeof(str), fp) != NULL) {
        // convert str from gattool to data values, and
        // validate the correct number of data values are scanned, and that
        // the first data value is the sign char, and that
        // the last 2 data values are cr lf
        cnt = sscanf(str, "Notification handle = 0x002e value: %x %x %x %x %x %x %x %x %x %x %x %x %x %x",
                     &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], 
                     &d[7], &d[8], &d[9], &d[10], &d[11], &d[12], &d[13]);
        if (cnt != 14) {
            DEBUG("%s invalid data from gatttool, cnt=%d\n", m->bluetooth_addr, cnt);
            continue;
        }
        if (d[0] != '+' && d[0] != '-') {
            DEBUG("%s invalid data from gatttool, d[0]=0x%2.2x\n", m->bluetooth_addr, d[0]);
            continue;
        }
        if (d[1] < '0' || d[1] > '9' ||
            d[2] < '0' || d[2] > '9' ||
            d[3] < '0' || d[3] > '9' ||
            d[4] < '0' || d[3] > '9')
        {
            DEBUG("%s invalid data from gatttool, d[1-4]=0x%2.2x 0x%2.2x 0x%2.2x 0x%2.2x\n",
                  m->bluetooth_addr, d[1], d[2], d[3], d[4]);
            continue;
        }
        if (d[12] != '\r' || d[13] != '\n') {
            DEBUG("%s invalid data from gatttool, d[12]=0x%2.2x d[13]=0x%2.2x\n", 
                  m->bluetooth_addr, d[12], d[13]);
            continue;
        }

        // determine value, using the following ...
        //   0:    2b               sign 2b=>'+'  2d=>'-'
        //   1-4:  30 30 30 30      v = 0000 - 9999 
        //   6:    34               30 : v /= 1
        //                          31 : v /= 1000
        //                          32 : v /= 100
        //                          34 : v /= 10
        value = (d[1]-'0')*1000 + (d[2]-'0')*100 + (d[3]-'0')*10 + (d[4]-'0');
        switch (d[6]) {
        case 0x31: value /= 1000; break;
        case 0x32: value /= 100; break;
        case 0x34: value /= 10; break;
        }
        if (d[0] == '-') {
            value = -value;
        }

        // inspect d[7] and d[8], 
        // to ensure DC is selected and no special modes enabled
        //   7:    31               bitmask  0x20  1=Auto  0=Manual
        //                                   0x10  DC
        //                                   0x08  AC
        //                                   0x04  Delta
        //                                   0x02  Hold  
        //   8:    00               bitmask  0x20  MAX
        //                                   0x10  MIN
        //                                   0x04  LowBattery
        if ((d[7] & 0x10) == 0 ||   // error if not DC
            (d[7] & 0x0e) != 0 ||   // error if AC or Delta or Hold
            (d[8] & 0x30) != 0)     // error if MAX or MIN
        {
            DEBUG("d[7]=0x%2.2x d[8]=0x%2.2x\n", d[7], d[8]);
            continue;
        }

        // inspect d[9] and d[10,
        // to determine if the reading is volts or amps and to get the scale
        //   9:    80                see d[10]
        //   10:   40                20 : Ohms : d[9] == 08 Continuity, d[9] == 10 Megohm, d[9] == 20 Kohm
        //                           40 : Amps : d[9] == 80 uA,  d[9] == 40 mA
        //                           80 : Volts: d[9] == 40 mV
        value_type = -1;
        if (d[10] == 0x40) {
            switch (d[9]) {
            case 0x80: value_type = OWON_B35_VALUE_TYPE_DC_MICROAMP; break;
            case 0x40: value_type = OWON_B35_VALUE_TYPE_DC_MILLIAMP; break;
            case 0x00: value_type = OWON_B35_VALUE_TYPE_DC_AMP; break;
            }
        }
        if (d[10] == 0x80) {
            switch (d[9]) {
            case 0x40: value_type = OWON_B35_VALUE_TYPE_DC_MILLIVOLT; break;
            case 0x00: value_type = OWON_B35_VALUE_TYPE_DC_VOLT; break;
            }
        }
        if (value_type == -1) {
            DEBUG("d[9]=0x%2.2x d[10]=0x%2.2x\n", d[9], d[10]);
            continue;
        }

        // publish
        m->value_type = value_type;
        m->value = value;
        m->reading_time_us = microsec_timer();
        __sync_synchronize();
        DEBUG("%s : %.3f %s\n", 
              m->bluetooth_addr,
              m->value, 
              OWON_B35_VALUE_TYPE_STR(m->value_type));
    }

    // meter_thread exitting
    DEBUG("gatttool thread exitting for - %s / %s\n", m->bluetooth_addr, m->meter_name);
    strcpy(m->bluetooth_addr, "");
    return NULL;
}
