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
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "common.h"
#include "util_dataq.h"
#include "util_mccdaq.h"
#include "util_cam.h"
#include "util_misc.h"

//
// defines
//

#define GAS_ID_D2 0
#define GAS_ID_N2 1

//#define CAM_ENABLE

#define ATOMIC_INCREMENT(x) \
    do { \
        __sync_fetch_and_add(x,1); \
    } while (0)

#define ATOMIC_DECREMENT(x)  \
    do { \
        __sync_fetch_and_sub(x,1); \
    } while (0)

//
// typedefs
//

//
// variables
//

static int32_t         active_thread_count;
static bool            sigint_or_sigterm;

#ifdef CAM_ENABLE
static uint8_t         jpeg_buff[1000000];
static int32_t         jpeg_buff_len;
static uint64_t        jpeg_buff_us;
static pthread_mutex_t jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static pthread_mutex_t neutron_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        neutron_time;
static float           neutron_cps;
static int16_t         neutron_adc_data[MAX_ADC_DATA];
static uint32_t        max_neutron_adc_data;

//
// prototypes
//

static void init(void);
static void server(void);
#ifdef CAM_ENABLE
static void * cam_thread(void * cx);
#endif
static void signal_handler(int sig);
static void * server_thread(void * cx);
static void init_data_struct(data_t * data, time_t time_now);
static float convert_adc_voltage(float adc_volts);
static float convert_adc_current(float adc_volts);
static float convert_adc_pressure(float adc_volts, uint32_t gas_id);
static int32_t mccdaq_callback(uint16_t * data, int32_t max_data);
static void print_plot_str(uint16_t value, uint16_t baseline);

// -----------------  MAIN & TOP LEVEL ROUTINES  -------------------------------------

int32_t main(int32_t argc, char **argv)
{
    int32_t wait_ms;

    // init
    init();

    // runtime
    server();

    // terminate
    for (wait_ms = 0; active_thread_count > 0 && wait_ms < 5000; wait_ms++) {
        usleep(1000);
    }
    if (active_thread_count > 0) {
        ERROR("all threads did not terminate, active_thread_count=%d\n", active_thread_count);
    }
    INFO("terminating\n");
    return 0;
}

static void init(void)
{
    struct rlimit rl;
    struct sigaction action;

    // use line bufferring
    setlinebuf(stdout);

    // allow core dump
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    // print size of data part1 and part2, and validate sizes are multiple of 8
    DEBUG("sizeof data_t=%zd part1=%zd part2=%zd\n",
          sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));
    if ((sizeof(struct data_part1_s) % 8) || (sizeof(struct data_part2_s) % 8)) {
        FATAL("sizeof data_t=%zd part1=%zd part2=%zd\n",
              sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));
    }

    // register signal handler for SIGINT, and SIGTERM
    bzero(&action, sizeof(action));
    action.sa_handler = signal_handler;
    sigaction(SIGINT, &action, NULL);

    bzero(&action, sizeof(action));
    action.sa_handler = signal_handler;
    sigaction(SIGTERM, &action, NULL);

#ifdef CAM_ENABLE
    // init camera
    pthread_t thread;
    if (cam_init(CAM_WIDTH, CAM_HEIGHT, FRAMES_PER_SEC) == 0) {
        if (pthread_create(&thread, NULL, cam_thread, NULL) != 0) {
            FATAL("pthread_create cam_thread, %s\n", strerror(errno));
        }
    }
#endif

    // init dataq device used to acquire chamber voltage, current and pressure readings
    dataq_init(0.5,   // averaging duration in secs
               1200,  // scan rate  (samples per second)
               3,     // number of adc channels
               DATAQ_ADC_CHAN_VOLTAGE,
               DATAQ_ADC_CHAN_CURRENT,
               DATAQ_ADC_CHAN_PRESSURE);

    // init mccdaq device, used to acquire 500000 samples per second from the
    // ludlum 2929 amplifier output
    mccdaq_init();
    mccdaq_start(mccdaq_callback);
}

static void server(void)
{
    struct sockaddr_in server_address;
    int32_t            listen_sockfd;
    int32_t            ret;
    pthread_t          thread;
    pthread_attr_t     attr;
    int32_t            optval;

    // create socket
    listen_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sockfd == -1) {
        FATAL("socket, %s\n", strerror(errno));
    }

    // set reuseaddr
    optval = 1;
    ret = setsockopt(listen_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, 4);
    if (ret == -1) {
        FATAL("SO_REUSEADDR, %s\n", strerror(errno));
    }

    // bind socket
    bzero(&server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(PORT);
    ret = bind(listen_sockfd,
               (struct sockaddr *)&server_address,
               sizeof(server_address));
    if (ret == -1) {
        FATAL("bind, %s\n", strerror(errno));
    }

    // listen 
    ret = listen(listen_sockfd, 50);
    if (ret == -1) {
        FATAL("listen, %s\n", strerror(errno));
    }

    // init thread attributes to make thread detached
    if (pthread_attr_init(&attr) != 0) {
        FATAL("pthread_attr_init, %s\n", strerror(errno));
    }
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        FATAL("pthread_attr_setdetachstate, %s\n", strerror(errno));
    }

    // loop, accepting connection, and create thread to service the client
    INFO("server: accepting connections\n");
    while (1) {
        int                sockfd;
        socklen_t          len;
        struct sockaddr_in address;

        // accept connection
        len = sizeof(address);
        sockfd = accept(listen_sockfd, (struct sockaddr *) &address, &len);
        if (sockfd == -1) {
            if (sigint_or_sigterm) {
                break;
            }
            FATAL("accept, %s\n", strerror(errno));
        }

        // create thread
        if (pthread_create(&thread, &attr, server_thread, (void*)(uintptr_t)sockfd) != 0) {
            FATAL("pthread_create server_thread, %s\n", strerror(errno));
        }
    }
}

#ifdef CAM_ENABLE
static void * cam_thread(void * cx) 
{
    int32_t   ret;
    uint8_t * ptr;
    uint32_t  len;

    ATOMIC_INCREMENT(&active_thread_count);

    while (true) {
        // if sigint_or_sigterm then exit thread
        if (sigint_or_sigterm) {
            break;
        }

        // get cam buff
        ret = cam_get_buff(&ptr, &len);
        if (ret != 0) {
            usleep(100000);
            continue;
        }

        // copy buff to global
        pthread_mutex_lock(&jpeg_mutex);
        memcpy(jpeg_buff, ptr, len);
        jpeg_buff_len = len;
        jpeg_buff_us = microsec_timer();
        pthread_mutex_unlock(&jpeg_mutex);

        // put buff
        cam_put_buff(ptr);
    }

    ATOMIC_DECREMENT(&active_thread_count);

    return NULL;
}
#endif

static void signal_handler(int sig)
{
    sigint_or_sigterm = true;
}

// -----------------  SERVER_THREAD  -------------------------------------------------

static void * server_thread(void * cx)
{
    int32_t   sockfd = (uintptr_t)cx;
    time_t    time_now, time_last;
    ssize_t   len;
    data_t  * data;

    ATOMIC_INCREMENT(&active_thread_count);

    INFO("accepted connection, sockfd=%d\n", sockfd);

    time_last = time(NULL);
    data = malloc(sizeof(data_t) + 1000000);

    while (true) {
        // wait for time_now to change (should be an increase by 1 second from time_last)
        while (true) {
            time_now = time(NULL);
            if (time_now != time_last) {
                break;
            }
            usleep(1000);  // 1 ms
            if (sigint_or_sigterm) {
                goto exit_thread;
            }
        }

        // sanity check time_now, should be time_last+1
        if (time_now < time_last) {
            ERROR("terminating connection - time has gone backwards\n");
            break;
        }
        if (time_now != time_last+1) {
            WARN("time_now - time_last = %ld\n", time_now-time_last);
        }

        // init data struct
        init_data_struct(data, time_now);

        // send data struct
        len = send(sockfd, data, sizeof(data_t)+data->part2.jpeg_buff_len, MSG_NOSIGNAL);
        if (len != sizeof(data_t)+data->part2.jpeg_buff_len) {
            if (len == -1 && (errno == ECONNRESET || errno == EPIPE)) {
                INFO("terminating connection\n");
            } else {
                ERROR("terminating connection - send failed, len=%zd, %s, \n", len, strerror(errno));
            }
            break;
        }

        // save time_last
        time_last = time_now;
    }

exit_thread:
    // terminate connection with client, and 
    // terminate thread
    close(sockfd);
    ATOMIC_DECREMENT(&active_thread_count);
    return NULL;
}

// -----------------  INIT_DATA_STRUCT  ----------------------------------------------

static void init_data_struct(data_t * data, time_t time_now)
{
    int16_t mean_mv, max_mv;
    int32_t ret, wait_ms;
    bool    ret1, ret2, ret3, ret4;

    // 
    // zero data struct 
    //

    bzero(data, sizeof(data_t));

    //
    // data part1
    //

    // data part1 magic & time
    data->part1.magic = MAGIC_DATA_PART1;
    data->part1.time  = time_now;

    // data part1 voltage_min_kv, voltage_max_kv, and voltage_kv
    ret = dataq_get_adc(DATAQ_ADC_CHAN_VOLTAGE, NULL, &mean_mv, NULL, NULL, NULL);         
    if (ret == 0) {
        data->part1.voltage_kv = convert_adc_voltage(mean_mv/1000.);
    } else {
        data->part1.voltage_kv = ERROR_NO_VALUE;
    }

    // data part1 current_ma
    ret = dataq_get_adc(DATAQ_ADC_CHAN_CURRENT, NULL, &mean_mv, NULL, NULL, NULL);
    if (ret == 0) {
        data->part1.current_ma = convert_adc_current(mean_mv/1000.);
    } else {
        data->part1.current_ma = ERROR_NO_VALUE;
    }

    // data part1 pressure_mtorr
    ret = dataq_get_adc(DATAQ_ADC_CHAN_PRESSURE, NULL, NULL, NULL, NULL, &max_mv);
    if (ret == 0) {
        data->part1.pressure_mtorr = convert_adc_pressure(max_mv/1000., GAS_ID_D2);
    } else {
        data->part1.pressure_mtorr = ERROR_NO_VALUE;
    }

    // wait for up to 250 ms for neutron data to be available for time_now;  
    // if neutron data avail then copy it into data part1 and part2
    for (wait_ms = 0; neutron_time != time_now && wait_ms < 250; wait_ms++) {
        usleep(1000);
    }
    pthread_mutex_lock(&neutron_mutex);
    if (neutron_time == time_now) {
        data->part1.neutron_cps = neutron_cps;
    } else {
        data->part1.neutron_cps = ERROR_NO_VALUE;
    }
    pthread_mutex_unlock(&neutron_mutex);

    //
    // data part2
    //

    // data part2: magic
    data->part2.magic = MAGIC_DATA_PART2;

    // data part2: adc_data
    ret1 = dataq_get_adc_data(DATAQ_ADC_CHAN_VOLTAGE, 
                              data->part2.voltage_adc_data,
                              MAX_ADC_DATA);
    ret2 = dataq_get_adc_data(DATAQ_ADC_CHAN_CURRENT, 
                              data->part2.current_adc_data,
                              MAX_ADC_DATA);
    ret3 = dataq_get_adc_data(DATAQ_ADC_CHAN_PRESSURE, 
                              data->part2.pressure_adc_data,
                              MAX_ADC_DATA);
    pthread_mutex_lock(&neutron_mutex);
    ret4 = (neutron_time == time_now ? 0 : -1);
    if (ret4 == 0) {
        memcpy(data->part2.neutron_adc_data, neutron_adc_data, max_neutron_adc_data*sizeof(int16_t));
    }
    data->part2.max_neutron_adc_data = max_neutron_adc_data;
    pthread_mutex_unlock(&neutron_mutex);

#ifdef CAM_ENABLE
    // data part2: jpeg_buff
    pthread_mutex_lock(&jpeg_mutex);
    if (microsec_timer() - jpeg_buff_us < 1000000) {
        memcpy(data->part2.jpeg_buff, jpeg_buff, jpeg_buff_len);
        data->part2.jpeg_buff_len = jpeg_buff_len;
    }
    pthread_mutex_unlock(&jpeg_mutex);
#endif

    //
    // data part1 (continued)
    //

    data->part1.data_part2_offset                  = 0;   // for display pgm
    data->part1.data_part2_length                  = sizeof(struct data_part2_s) + data->part2.jpeg_buff_len;
    data->part1.data_part2_jpeg_buff_valid         = (data->part2.jpeg_buff_len != 0);
    data->part1.data_part2_voltage_adc_data_valid  = (ret1 == 0);
    data->part1.data_part2_current_adc_data_valid  = (ret2 == 0);
    data->part1.data_part2_pressure_adc_data_valid = (ret3 == 0);
    data->part1.data_part2_neutron_adc_data_valid  = (ret4 == 0);
}

// -----------------  CONVERT ADC HV VOLTAGE & CURRENT  ------------------------------

// These routines convert the voltage read from the dataq adc channels to
// the value which will be displayed. 
//
// For example assume that the HV voltage divider is 10000 to 1, thus an adc 
// voltage reading of 2 V means the HV is 20000 Volts. The HV is displayed in
// kV, so the value returned would be 20.

static float convert_adc_voltage(float adc_volts)
{
    #define TUNE_KV   1.43

    // My fusor's voltage divider is made up of a 1G Ohm resistor, and
    // a 100K Ohm resistor. In parallel with the 100K Ohm resistor are
    // the panel meter and the dataq adc input, which have resistances of
    // 10M Ohm and 2M Ohm respectively. So, use 94.34K instead of 100K
    // in the conversion calculation.

    // I = Vhv / (1G + 94.34K) 
    //
    // I = Vhv / 1G                           (approximately)
    //
    // Vadc = (Vhv / 1G) * 94.34K
    //
    // Vhv = Vadc * (1G / 94.34K)             (volts)
    //
    // Vhv = Vadc * (1G / 94.34K) / 1000      (killo-volts)

    return adc_volts * (1E9 / 94.34E3 / 1000.) + TUNE_KV;    // kV
}

static float convert_adc_current(float adc_volts)
{
    #define TUNE_MA   0.14

    // My fusor's current measurement resistor is 100 Ohm.

    // I = Vadc / 100            (amps)
    //
    // I = Vadc / 100 * 1000     (milli-amps)

    return adc_volts * 10. + TUNE_MA;    // mA
}

// -----------------  CONVERT ADC PRESSURE GAUGE  ----------------------------

// Notes:
// - Refer to http://www.lesker.com/newweb/gauges/pdf/manuals/275iusermanual.pdf
//   section 7.2
// - The gas_tbl below is generated from the table in Section 7.2 of 
//   275iusermanual.pdf. The devel_tools/kjl_275i_log_linear_tbl program
//   converted the table to C code.

// --- defines ---

#define MAX_GAS_TBL (sizeof(gas_tbl)/sizeof(gas_tbl[0]))

// --- typedefs ---

typedef struct {
    char * name;
    struct {
        float pressure;
        float voltage;
    } interp_tbl[50];
} gas_t;

// --- variables ---

gas_t gas_tbl[] = { 
    { "D2", // TORRS       VOLTS
      { {     0.00001,     0.000 },
        {     0.00002,     0.301 },
        {     0.00005,     0.699 },
        {     0.0001,      1.000 },
        {     0.0002,      1.301 },
        {     0.0005,      1.699 },
        {     0.0010,      2.114 },
        {     0.0020,      2.380 },
        {     0.0050,      2.778 },
        {     0.0100,      3.083 },
        {     0.0200,      3.386 },
        {     0.0500,      3.778 },
        {     0.1000,      4.083 },
        {     0.2000,      4.398 },
        {     0.5000,      4.837 },
        {     1.0000,      5.190 },
        {     2.0000,      5.616 },
        {     5.0000,      7.391 }, } },
    { "N2",
      { {     0.00001,     0.000 },
        {     0.00002,     0.301 },
        {     0.00005,     0.699 },
        {     0.0001,      1.000 },
        {     0.0002,      1.301 },
        {     0.0005,      1.699 },
        {     0.0010,      2.000 },
        {     0.0020,      2.301 },
        {     0.0050,      2.699 },
        {     0.0100,      3.000 },
        {     0.0200,      3.301 },
        {     0.0500,      3.699 },
        {     0.1000,      4.000 },
        {     0.2000,      4.301 },
        {     0.5000,      4.699 },
        {     1.0000,      5.000 },
        {     2.0000,      5.301 },
        {     5.0000,      5.699 },
        {    10.0000,      6.000 },
        {    20.0000,      6.301 },
        {    50.0000,      6.699 },
        {   100.0000,      7.000 },
        {   200.0000,      7.301 },
        {   300.0000,      7.477 },
        {   400.0000,      7.602 },
        {   500.0000,      7.699 },
        {   600.0000,      7.778 },
        {   700.0000,      7.845 },
        {   760.0000,      7.881 },
        {   800.0000,      7.903 },
        {   900.0000,      7.954 },
        {  1000.0000,      8.000 }, } },
                                            };

// --- code ---

static float convert_adc_pressure(float adc_volts, uint32_t gas_id)
{
    gas_t * gas = &gas_tbl[gas_id];
    int32_t i = 0;

    if (adc_volts < 0.01) {
        return ERROR_PRESSURE_SENSOR_FAULTY;
    }

    while (true) {
        if (gas->interp_tbl[i+1].voltage == 0) {
            return ERROR_OVER_PRESSURE;
        }

        if (adc_volts >= gas->interp_tbl[i].voltage &&
            adc_volts <= gas->interp_tbl[i+1].voltage)
        {
            float p0 = gas->interp_tbl[i].pressure;
            float p1 = gas->interp_tbl[i+1].pressure;
            float v0 = gas->interp_tbl[i].voltage;
            float v1 = gas->interp_tbl[i+1].voltage;
            float torr =  p0 + (p1 - p0) * (adc_volts - v0) / (v1 - v0);
            return torr * 1000.0;
        }
        i++;
    }
}    

// -----------------  MCCDAQ CALLBACK - NEUTRON DETECTOR PULSES  ---------------------

static int32_t mccdaq_callback(uint16_t * d, int32_t max_d)
{
    #define MAX_DATA 1000000

    #define MAX_PC1SH 15000
    static uint16_t data[MAX_DATA];
    static int32_t  max_data;
    static int32_t  idx;
    static int32_t  baseline;
    static int32_t  pulse_count;
    static int16_t  local_neutron_adc_data[MAX_ADC_DATA];
    static uint32_t local_max_neutron_adc_data;

    #define TUNE_PULSE_THRESHOLD  10

    #define RESET_FOR_NEXT_SEC \
        do { \
            max_data = 0; \
            idx = 0; \
            pulse_count = 0; \
            local_max_neutron_adc_data= 0; \
        } while (0)

    // if max_data too big then 
    //   print an error 
    //   reset 
    // endif
    if (max_data + max_d > MAX_DATA) {
        ERROR("max_data %d or max_d %d are too large\n", max_data, max_d);
        RESET_FOR_NEXT_SEC;
        return 0;
    }

    // copy caller supplied data to static data buffer
    memcpy(data+max_data, d, max_d*sizeof(uint16_t));
    max_data += max_d;

    // if we have too little data just return, 
    // until additional data is received
    if (max_data < 100) {
        return 0;
    }

    // search for pulses in the data
    int32_t pulse_start_idx = -1;
    int32_t pulse_end_idx   = -1;
    while (true) {
        // terminate this loop when 
        // - not in-a-pulse and near the end of data OR
        // - at the end of data
        if ((pulse_start_idx == -1 && idx >= max_data-20) || 
            (idx == max_data))
        {
            break;
        }

        // print warning if data out of range
        if (data[idx] > 4095) {
            WARN("data[%d] = %u, is out of range\n", idx, data[idx]);
            data[idx] = 2048;
        }

        // update baseline ...
        // if data[idx] is close to baseline then
        //   baseline is okay
        // else if data[idx+10] is close to baseline then
        //   baseline is okay
        // else if data[idx] and the preceding 3 data values are almost the same then
        //   set baseline to data[idx]
        // endif
        if (pulse_start_idx == -1) {
            if (data[idx] >= baseline-1 && data[idx] <= baseline+1) {
                ;  // okay
            } else if (idx+10 < max_data && data[idx+10] >= baseline-1 && data[idx+10] <= baseline+1) {
                ;  // okay
            } else if ((idx >= 3) &&
                       (data[idx-1] >= data[idx]-1 && data[idx-1] <= data[idx]+1) &&
                       (data[idx-2] >= data[idx]-1 && data[idx-2] <= data[idx]+1) &&
                       (data[idx-3] >= data[idx]-1 && data[idx-3] <= data[idx]+1))
            {
                baseline = data[idx];
            }
        }

        // if baseline has not yet determined then continue
        if (baseline == 0) {
            idx++;
            continue;
        }

        // determine the pulse_start_idx and pulse_end_idx
        if (data[idx] >= (baseline + TUNE_PULSE_THRESHOLD) && pulse_start_idx == -1) {
            pulse_start_idx = idx;
        } else if (pulse_start_idx != -1) {
            if (data[idx] < (baseline + TUNE_PULSE_THRESHOLD)) {
                pulse_end_idx = idx - 1;
            } else if (idx - pulse_start_idx >= 10) {
                WARN("discarding a possible pulse because it's too long, pulse_start_idx=%d\n",
                     pulse_start_idx);
                pulse_start_idx = -1;
                pulse_end_idx = -1;
            }
        }

        // if a pulse has been located ...
        // - increment pulse_count
        // - save copy of the pulse in local_neutron_adc_data (50 samples with pulse in center)
        // - determine the pulse_height
        // - print the pulse to the log file
        // endif
        if (pulse_end_idx != -1) {
            int32_t pulse_height, i;
            int32_t pulse_start_idx_extended, pulse_end_idx_extended;

            // keep track of pulse_count
            pulse_count++;

            // add the pulse to local_neutron_adc_data, 
            // XXX perhaps put markr on then end
            pulse_start_idx_extended = pulse_start_idx - 23;
            if (pulse_start_idx_extended < 0) {
                pulse_start_idx_extended = 0;
            }
            pulse_end_idx_extended = pulse_start_idx_extended + 49;
            if (pulse_end_idx_extended >= max_data) {
                pulse_end_idx_extended = max_data - 1;
                pulse_start_idx_extended = pulse_end_idx_extended - 49;
            }
            for (i = pulse_start_idx_extended; i <= pulse_end_idx_extended; i++) {
                if (local_max_neutron_adc_data >= MAX_ADC_DATA) {
                    break;
                }
                local_neutron_adc_data[local_max_neutron_adc_data++] = 
                    (data[i] - baseline) * 1000 / 205;    // mv above baseline
            }

            // scan from start to end of pulse to determine pulse_height,
            // where pulse_height is the height above the baseline
            pulse_height = -1;
            for (i = pulse_start_idx; i <= pulse_end_idx; i++) {
                if (data[i] - baseline > pulse_height) {
                    pulse_height = data[i] - baseline;
                }
            }

            // plot the pulse 
            printf("PULSE:  height = %d   baseline = %d\n",
                   pulse_height, baseline-2048);
            pulse_start_idx_extended = pulse_start_idx - 1;
            pulse_end_idx_extended = pulse_end_idx + 4;
            if (pulse_start_idx_extended < 0) {
                pulse_end_idx_extended = 0;
            }
            if (pulse_end_idx_extended >= max_data) {
                pulse_end_idx_extended = max_data-1;
            }
            for (i = pulse_start_idx_extended; i <= pulse_end_idx_extended; i++) {
                print_plot_str(data[i], baseline);
            }
            printf("\n");

            // done with this pulse
            pulse_start_idx = -1;
            pulse_end_idx = -1;
        }

        // move to next data 
        idx++;
    }

    // if time has incremented then
    //   - publish new neutron data
    //   - print to log file
    //   - reset variables for the next second 
    // endif
    uint64_t time_now = time(NULL);
    if (time_now > neutron_time) {    
        char voltage_str[100], current_str[100], pressure_str[100];
        int16_t mean_mv, max_mv;

        // publish new neutron data
        pthread_mutex_lock(&neutron_mutex);
        neutron_time = time_now;
        neutron_cps = pulse_count;
        memcpy(neutron_adc_data, local_neutron_adc_data, local_max_neutron_adc_data*sizeof(int16_t));
        max_neutron_adc_data = local_max_neutron_adc_data;
        pthread_mutex_unlock(&neutron_mutex);

#if 0 // XXX del
        int i;
        for (i = 0; i < max_neutron_adc_data; i++) {
            printf("%d - %d\n", i, neutron_adc_data[i]);
        }
#endif

        // get voltage, current, and pressure values so they can be printed below
        if (dataq_get_adc(DATAQ_ADC_CHAN_VOLTAGE, NULL, &mean_mv, NULL, NULL, NULL) == 0) {  
            sprintf(voltage_str, "%0.1f KV", convert_adc_voltage(mean_mv/1000.));
        } else {
            sprintf(voltage_str, "NO_VALUE");
        }
        if (dataq_get_adc(DATAQ_ADC_CHAN_CURRENT, NULL, &mean_mv, NULL, NULL, NULL) == 0) {
            sprintf(current_str, "%0.1f MA", convert_adc_current(mean_mv/1000.));
        } else {
            sprintf(current_str, "NO_VALUE");
        }
        if (dataq_get_adc(DATAQ_ADC_CHAN_PRESSURE, NULL, NULL, NULL, NULL, &max_mv) == 0) {
            float pressure_mtorr = convert_adc_pressure(max_mv/1000., GAS_ID_D2);
            if (pressure_mtorr < 1000) {
                sprintf(pressure_str, "%0.1f mTorr", pressure_mtorr);
            } else {
                sprintf(pressure_str, "%0.0f Torr", pressure_mtorr/1000);
            }
        } else {
            sprintf(pressure_str, "NO_VALUE");
        }

        // print info, and seperator line,
        // note that the seperator line is intended to mark the begining of the next second
        printf("NEUTRON:  samples=%d   mccdaq_restarts=%d\n",
               max_data, mccdaq_get_restart_count());
        printf("SUMMARY:  neutron = %d /sec   voltage = %s   current = %s   pressure = %s\n",
               pulse_count, voltage_str, current_str, pressure_str);
        printf("\n");
        INFO("=========================================================================\n");
        printf("\n");

        // reset for the next second
        RESET_FOR_NEXT_SEC;
    }

    // return 'continue-scanning' 
    return 0;
}

static void print_plot_str(uint16_t value, uint16_t baseline)
{
    char    str[110];
    int32_t idx, i;

    // value                     : expected range 2048 - 4095
    // idx = (value - 2048) / 20 : range  0 - 102

    if (value > 4095) {
        printf("%5d: value is out of range\n", value-2048);
        return;
    }
    if (baseline > 4095) {
        printf("%5d: baseline is out of range\n", baseline);
        return;
    }

    if (value < 2048) {
        value = 2048;
    }
    if (baseline < 2048) {
        baseline = 2048;
    }

    bzero(str, sizeof(str));

    idx = (value - 2048) / 20;
    for (i = 0; i <= idx; i++) {
        str[i] = '*';
    }

    idx = (baseline - 2048) / 20;
    if (str[idx] == '*') {
        str[idx] = '+';
    } else {
        str[idx] = '|';
        for (i = 0; i < idx; i++) {
            if (str[i] == '\0') {
                str[i] = ' ';
            }
        }
    }

    printf("%5d: %s\n", value-2048, str);
}

