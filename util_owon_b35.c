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
// the meter reading in the meter[] array. The owon_b35_get_value() 
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
    int32_t  desired_value_type;
    uint64_t reading_count;
    uint64_t reading_count_last;
    uint64_t value_time_us;
    double   value;
} meter_t;

//
// varialbles
//

meter_t meter[MAX_METER];

//
// prototypes
//

static void * meter_thread(void *cx);
static void * monitor_thread(void *cx);

// -----------------  API  ---------------------------------------------------------

int32_t owon_b35_init(int32_t cnt, ...) // id,addr,desired_value_type,name  
{
    int32_t i;
    va_list ap;
    pthread_t thread;

    // init meter table entry and create thread for all the meters 
    // specified by caller
    va_start(ap, cnt);
    for (i = 0; i < cnt; i++) {
        int32_t id                 = va_arg(ap, int32_t);
        char  * bluetooth_addr     = va_arg(ap, char*);
        int32_t desired_value_type = va_arg(ap, int32_t);
        char  * meter_name         = va_arg(ap, char*);

        if (id >= MAX_METER) {
            FATAL("id %d too large\n", id);
        }

        INFO("id=%d addr=%s desired_value_type=%s name=%s\n",
             id, bluetooth_addr, OWON_B35_VALUE_TYPE_STR(desired_value_type), meter_name);

        meter_t *m = &meter[id];
        strcpy(m->bluetooth_addr, bluetooth_addr);
        m->desired_value_type = desired_value_type;
        strcpy(m->meter_name, meter_name);

        pthread_create(&thread, NULL, meter_thread, m);
    }
    va_end(ap);

    // create the monitor thread, which will look for stalled
    // meter_threads, and kill the associated gatttool
    pthread_create(&thread, NULL, monitor_thread, NULL);

    // return success
    return 0;
}

double owon_b35_get_value(int id)
{
    meter_t *m = &meter[id];  

    if (id >= MAX_METER) {
        FATAL("id %d too large\n", id);
    }

    if (microsec_timer() - m->value_time_us > 2000000) {
        return ERROR_NO_VALUE;
    } else {
        return m->value;
    }
}

// -----------------  PRIVATE  -----------------------------------------------------

static void * meter_thread(void *cx)
{
    meter_t *m = cx;
    FILE    *fp;
    char     cmd[1000], str[1000];
    int32_t  cnt, value_type;
    uint32_t d[14];
    double   value;
    uint64_t time_now_us, time_since_last_gatttool_start_us;

    static pthread_mutex_t gatttool_start_mutex = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t last_gatttool_start_us;

    // this code decodes only the following:
    // - DC voltage
    // - DC current
    // other meter features, such as resistance, temperature, delta-mode, hold-mode
    // are not supported

restart:
    // start gatttool, but don't start concurrently
    pthread_mutex_lock(&gatttool_start_mutex);
    time_now_us = microsec_timer();
    time_since_last_gatttool_start_us = time_now_us - last_gatttool_start_us;
    if (time_since_last_gatttool_start_us < 5000000) {
        usleep(5000000 - time_since_last_gatttool_start_us);
    }
    sprintf(cmd, "gatttool -b %s --char-read --handle 0x2d --listen", m->bluetooth_addr);
    DEBUG("RUNNING '%s'\n", cmd);
    fp = popen(cmd,"r");
    if (fp == NULL) {
        FATAL("failed popen '%s'\n", cmd);
    }
    last_gatttool_start_us = time_now_us;
    pthread_mutex_unlock(&gatttool_start_mutex);

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

        // keep track of the number of readings received; 
        // for use by the monitor_thread
        m->reading_count++;

        // publish, but only if value_type is what is desired
        if (value_type == m->desired_value_type) {
            m->value = value;
            m->value_time_us = microsec_timer();
            __sync_synchronize();
        }
        DEBUG("%s : %.3f %s\n", m->bluetooth_addr, m->value, OWON_B35_VALUE_TYPE_STR(value_type));
    }

    // close the fp and restart from the top
    fclose(fp);
    goto restart;
}

static void * monitor_thread(void * cx)
{
    int32_t i;
    char cmd[200];

    // every 10 secs check the meter_threads to verify they have
    // received data from the meter; if they haven't then kill the
    // gatttool; the meter_thread will then restart the gatttool

    while (true) {
        sleep(10);
        for (i = 0; i < MAX_METER; i++) {
            meter_t *m = &meter[i];

            if (m->bluetooth_addr[0] == '\0') {
                continue;
            }

            if (m->reading_count > m->reading_count_last) {
                m->reading_count_last = m->reading_count;
                continue;
            }

            sprintf(cmd, "pkill -f \"^gatttool -b %s\"",  m->bluetooth_addr);
            ERROR("killing gatttool for %s '%s'\n", m->meter_name, cmd);
            system(cmd);
        }
    }

    return NULL;
}

#ifdef UNIT_TEST
// -----------------  UNIT TEST  ---------------------------------------------------

// build for unit test:
//
// gcc -Wall -O2 -DUNIT_TEST -pthread -o ut util_owon_b35.c util_misc.c

static float get_fusor_voltage_kv(void);
static float get_fusor_current_ma(void);

int main(int argc, char **argv)
{
    double ma, kv;
    char ma_str[100], kv_str[100];
    int kv_noval_cnt=0, ma_noval_cnt=0;

    owon_b35_init(
        2,
        OWON_B35_FUSOR_VOLTAGE_METER_ID, OWON_B35_FUSOR_VOLTAGE_METER_ADDR,
              OWON_B35_VALUE_TYPE_DC_MICROAMP, "voltage",
        OWON_B35_FUSOR_CURRENT_METER_ID, OWON_B35_FUSOR_CURRENT_METER_ADDR,
              OWON_B35_VALUE_TYPE_DC_MILLIAMP, "current"
                        );

    while (true) {
        kv = get_fusor_voltage_kv();
        if (kv != ERROR_NO_VALUE) {
            sprintf(kv_str, "%10.1f", kv);
        } else {
            sprintf(kv_str, "%10s", "NO_VAL");
            kv_noval_cnt++;
        }

        ma = get_fusor_current_ma();
        if (ma != ERROR_NO_VALUE) {
            sprintf(ma_str, "%10.1f", ma);
        } else {
            sprintf(ma_str, "%10s", "NO_VAL");
            ma_noval_cnt++;
        }

        INFO("%s KV    %s MA  -- kv_noval_cnt=%d  ma_noval_cnt=%d\n", 
             kv_str, ma_str, kv_noval_cnt, ma_noval_cnt);

        usleep(500000);
    }
}

static float get_fusor_voltage_kv(void)
{
    double ua, kv;

    // for example:
    //   I = E / R = 1000 v / 10^9 ohm = 10^-6 amps
    // therefore 1uA meter reading means 1kv fusor voltage

    ua = owon_b35_get_value(OWON_B35_FUSOR_VOLTAGE_METER_ID);
    if (ua == ERROR_NO_VALUE) {
        return ERROR_NO_VALUE;
    }

    kv = ua;
    return kv;
}

static float get_fusor_current_ma(void)
{
    double ma;

    ma = owon_b35_get_value(OWON_B35_FUSOR_CURRENT_METER_ID);
    if (ma == ERROR_NO_VALUE) {
        return ERROR_NO_VALUE;
    }

    return ma;
}

#endif
