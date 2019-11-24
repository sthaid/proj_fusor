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
#include <sys/resource.h>
#include <sys/socket.h>

#include "common.h"
#include "util_dataq.h"
#include "util_mccdaq.h"
#include "util_owon_b35.h"
#include "util_cam.h"
#include "util_misc.h"

//
// defines
//

//#define CAM_ENABLE
#define DEBUG_PRINT_PULSE_GRAPH
#define DEBUG_PRINT_INFO

#define GAS_ID_D2 0
#define GAS_ID_N2 1

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
static int16_t         neutron_pulse_mv[MAX_NEUTRON_PULSE];  // store pulse height for each pulse, in mv
static int16_t         neutron_adc_pulse_data[MAX_NEUTRON_PULSE][MAX_NEUTRON_ADC_PULSE_DATA];   // mv
static int32_t         max_neutron_pulse;

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
static float get_fusor_voltage_kv(void);
static float get_fusor_current_ma(void);
static float convert_adc_pressure(float adc_volts, int32_t gas_id);
static int32_t mccdaq_callback(uint16_t * data, int32_t max_data);
#ifdef DEBUG_PRINT_PULSE_GRAPH
static void print_plot_str(int32_t value, int32_t baseline);
#endif

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
               1,     // number of adc channels
               DATAQ_ADC_CHAN_PRESSURE);

    // init mccdaq device, used to acquire 500000 samples per second from the
    // ludlum 2929 amplifier output
    mccdaq_init();
    mccdaq_start(mccdaq_callback);

    // init owen_b35, used to acquire fusor voltage and current via bluetooth meter
    owon_b35_init(
        2, 
        OWON_B35_FUSOR_VOLTAGE_METER_ID, OWON_B35_FUSOR_VOLTAGE_METER_ADDR, 
              OWON_B35_VALUE_TYPE_DC_MICROAMP, "voltage",
        OWON_B35_FUSOR_CURRENT_METER_ID, OWON_B35_FUSOR_CURRENT_METER_ADDR, 
              OWON_B35_VALUE_TYPE_DC_MILLIAMP, "current"
                        );
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
        len = do_send(sockfd, data, sizeof(data_t)+data->part1.data_part2_jpeg_buff_len);
        if (len != sizeof(data_t)+data->part1.data_part2_jpeg_buff_len) {
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
    int16_t mean_mv;
    int32_t ret, wait_ms;
    static bool unavail_warn_printed = false;

    // zero data struct;  
    bzero(data, sizeof(data_t));

    // data part1 magic & time, and 
    // data part2: magic
    data->part1.magic = MAGIC_DATA_PART1;
    data->part1.time  = time_now;
    data->part2.magic = MAGIC_DATA_PART2;

    // data part1 voltage_kv, and
    // data part1 current_ma
    data->part1.voltage_kv = get_fusor_voltage_kv();
    data->part1.current_ma = get_fusor_current_ma();

    // if we don't have either of the fusor voltage or current then 
    // print a warning
    if (data->part1.voltage_kv == ERROR_NO_VALUE || data->part1.current_ma == ERROR_NO_VALUE) {
        char voltage_str[100], current_str[100];
        if (data->part1.voltage_kv == ERROR_NO_VALUE) {
            sprintf(voltage_str, "NO_VALUE");
        } else {
            sprintf(voltage_str, "%.1f kV", data->part1.voltage_kv);
        }
        if (data->part1.current_ma == ERROR_NO_VALUE) {
            sprintf(current_str, "NO_VALUE");
        } else {
            sprintf(current_str, "%.1f mA", data->part1.current_ma);
        }
        WARN("fusor voltage or current values not available: VOLTAGE='%s' CURRENT='%s'\n",
             voltage_str, current_str);
        unavail_warn_printed = true;
    } else {
        if (unavail_warn_printed) {
            INFO("fusor voltage and current values are now both available\n");
            unavail_warn_printed = false;
        }
    }

    // data part1 d2_pressure_mtorr and n2_pressure_mtorr
    ret = dataq_get_adc(DATAQ_ADC_CHAN_PRESSURE, NULL, &mean_mv, NULL, NULL, NULL);
    if (ret == 0) {
        data->part1.d2_pressure_mtorr = convert_adc_pressure(mean_mv/1000., GAS_ID_D2);
        data->part1.n2_pressure_mtorr = convert_adc_pressure(mean_mv/1000., GAS_ID_N2);
    } else {
        data->part1.d2_pressure_mtorr = ERROR_NO_VALUE;
        data->part1.n2_pressure_mtorr = ERROR_NO_VALUE;
    }

    // data part2: voltage, current, and pressure adc_data
    data->part1.data_part2_voltage_adc_data_valid  = false;
    data->part1.data_part2_current_adc_data_valid  = false;
    ret = dataq_get_adc_data(DATAQ_ADC_CHAN_PRESSURE, 
                             data->part2.pressure_adc_data,
                             MAX_ADC_DATA);
    data->part1.data_part2_pressure_adc_data_valid = (ret == 0);

    // wait for up to 250 ms for neutron data to be available for time_now;  
    // if neutron data avail then copy it into data part1 and part2
    for (wait_ms = 0; neutron_time != time_now && wait_ms < 250; wait_ms++) {
        usleep(1000);
    }
    pthread_mutex_lock(&neutron_mutex);
    if (neutron_time == time_now) {
        memcpy(data->part1.neutron_pulse_mv, 
               neutron_pulse_mv, 
               max_neutron_pulse*sizeof(neutron_pulse_mv[0]));
        memcpy(data->part2.neutron_adc_pulse_data, 
               neutron_adc_pulse_data, 
               max_neutron_pulse*sizeof(neutron_adc_pulse_data[0]));
        data->part1.max_neutron_pulse = max_neutron_pulse;
    } else {
        data->part1.max_neutron_pulse = 0;
    }
    pthread_mutex_unlock(&neutron_mutex);

#ifdef CAM_ENABLE
    // data part2: jpeg_buff
    pthread_mutex_lock(&jpeg_mutex);
    if (microsec_timer() - jpeg_buff_us < 1000000) {
        memcpy(data->part2.jpeg_buff, jpeg_buff, jpeg_buff_len);
        data->part1.data_part2_jpeg_buff_len = jpeg_buff_len;
    }
    pthread_mutex_unlock(&jpeg_mutex);
#else
    data->part1.data_part2_jpeg_buff_len = 0;
#endif

    // data part1: data_part_offset, and data_part2_length
    data->part1.data_part2_offset  = 0;   // for use by the display pgm
    data->part1.data_part2_length  = sizeof(struct data_part2_s) + data->part1.data_part2_jpeg_buff_len;
}

// -----------------  GET FUSOR VOLTAGE AND CURRENT  ---------------------------------

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

static float convert_adc_pressure(float adc_volts, int32_t gas_id)
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
    static int16_t  data[MAX_DATA];
    static int32_t  max_data;
    static int32_t  idx;
    static int32_t  baseline;
    static int16_t  local_neutron_adc_pulse_data[MAX_NEUTRON_PULSE][MAX_NEUTRON_ADC_PULSE_DATA];
    static int16_t  local_neutron_pulse_mv[MAX_NEUTRON_PULSE];
    static int32_t  local_max_neutron_pulse;

    #define TUNE_PULSE_THRESHOLD  10

    #define RESET_FOR_NEXT_SEC \
        do { \
            max_data = 0; \
            idx = 0; \
            local_max_neutron_pulse = 0; \
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
    memcpy(data+max_data, d, max_d*sizeof(int16_t));
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
        // - determine the pulse_height, in mv
        // - save the pulse height in local_neutron_pulse_mv[]
        // - save pulse data in local_neutron_adc_pulse_data
        // - print the pulse to the log file
        // endif
        if (pulse_end_idx != -1) {
            int32_t pulse_height, i, k;
            int32_t pulse_start_idx_extended, pulse_end_idx_extended;

            // scan from start to end of pulse to determine pulse_height,
            // where pulse_height is the height above the baseline
            pulse_height = -1;
            for (i = pulse_start_idx; i <= pulse_end_idx; i++) {
                if (data[i] - baseline > pulse_height) {
                    pulse_height = data[i] - baseline;
                }
            }
            pulse_height = pulse_height * 10000 / 2048;  // convert to mv

            // if there is room to store another neutron pulse then
            // - store the pulse height
            // - store the pulse data
            // endif
            if (max_neutron_pulse < MAX_NEUTRON_PULSE) {
                // store pulse height
                local_neutron_pulse_mv[local_max_neutron_pulse] = pulse_height;

                // store pulse data
                pulse_start_idx_extended = pulse_start_idx - (MAX_NEUTRON_ADC_PULSE_DATA/2);
                if (pulse_start_idx_extended < 0) {
                    pulse_start_idx_extended = 0;
                }
                pulse_end_idx_extended = pulse_start_idx_extended + (MAX_NEUTRON_ADC_PULSE_DATA-1);
                if (pulse_end_idx_extended >= max_data) {
                    pulse_end_idx_extended = max_data - 1;
                    pulse_start_idx_extended = pulse_end_idx_extended - (MAX_NEUTRON_ADC_PULSE_DATA-1);
                }
                for (k = 0, i = pulse_start_idx_extended; i <= pulse_end_idx_extended; i++) {
                    local_neutron_adc_pulse_data[local_max_neutron_pulse][k++] =
                        (data[i] - baseline) * 10000 / 2048;    // mv above baseline
                }

                // increment local_max_neutron_pulse
                local_max_neutron_pulse++;
            }

#ifdef DEBUG_PRINT_PULSE_GRAPH
            // plot the pulse 
            pulse_start_idx_extended = pulse_start_idx - 1;
            pulse_end_idx_extended = pulse_end_idx + 4;
            if (pulse_start_idx_extended < 0) {
                pulse_start_idx_extended = 0;
            }
            if (pulse_end_idx_extended >= max_data) {
                pulse_end_idx_extended = max_data-1;
            }
            printf("PULSE:  height_mv = %d   baseline_mv = %d   (%d,%d,%d)\n",
                   pulse_height, (baseline-2048)*10000/2048,
                   pulse_start_idx_extended, pulse_end_idx_extended, max_data);
            for (i = pulse_start_idx_extended; i <= pulse_end_idx_extended; i++) {
                print_plot_str((data[i]-2048)*10000/2048, (baseline-2048)*10000/2048); 
            }
            printf("\n");
#endif

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
        char voltage_str[100], current_str[100], d2_pressure_str[100], n2_pressure_str[100];
        float current_ma, voltage_kv;
        int16_t mean_mv;

        // publish new neutron data
        pthread_mutex_lock(&neutron_mutex);
        neutron_time = time_now;
        memcpy(neutron_pulse_mv, 
               local_neutron_pulse_mv, 
               local_max_neutron_pulse*sizeof(neutron_pulse_mv[0]));
        memcpy(neutron_adc_pulse_data, 
               local_neutron_adc_pulse_data, 
               local_max_neutron_pulse*sizeof(neutron_adc_pulse_data[0]));
        max_neutron_pulse = local_max_neutron_pulse;
        pthread_mutex_unlock(&neutron_mutex);

        // get voltage, current, and pressure values so they can be printed below
        voltage_kv = get_fusor_voltage_kv();
        if (voltage_kv != ERROR_NO_VALUE) {
            sprintf(voltage_str, "%0.1f KV", voltage_kv);
        } else {
            sprintf(voltage_str, "NO_VALUE");
        }
        current_ma = get_fusor_current_ma();
        if (current_ma != ERROR_NO_VALUE) {
            sprintf(current_str, "%0.1f MA", current_ma);
        } else {
            sprintf(current_str, "NO_VALUE");
        }
        if (dataq_get_adc(DATAQ_ADC_CHAN_PRESSURE, NULL, &mean_mv, NULL, NULL, NULL) == 0) {
            float d2_pressure_mtorr = convert_adc_pressure(mean_mv/1000., GAS_ID_D2);
            float n2_pressure_mtorr = convert_adc_pressure(mean_mv/1000., GAS_ID_N2);
            if (IS_ERROR(d2_pressure_mtorr)) {
                sprintf(d2_pressure_str, "%s", ERROR_TEXT(d2_pressure_mtorr));
            } else if (d2_pressure_mtorr < 1000) {
                sprintf(d2_pressure_str, "%0.1f mTorr", d2_pressure_mtorr);
            } else {
                sprintf(d2_pressure_str, "%0.0f Torr", d2_pressure_mtorr/1000);
            }
            if (IS_ERROR(n2_pressure_mtorr)) {
                sprintf(n2_pressure_str, "%s", ERROR_TEXT(n2_pressure_mtorr));
            } else if (n2_pressure_mtorr < 1000) {
                sprintf(n2_pressure_str, "%0.1f mTorr", n2_pressure_mtorr);
            } else {
                sprintf(n2_pressure_str, "%0.0f Torr", n2_pressure_mtorr/1000);
            }
        } else {
            sprintf(d2_pressure_str, "NO_VALUE");
            sprintf(n2_pressure_str, "NO_VALUE");
        }

#ifdef DEBUG_PRINT_INFO
        // print info, and seperator line,
        // note that the seperator line is intended to mark the begining of the next second
        printf("NEUTRON:  samples=%d   mccdaq_restarts=%d   baseline_mv=%d\n",
               max_data, mccdaq_get_restart_count(), (baseline-2048)*10000/2048);
        printf("SUMMARY:  neutron_pulse = %d /sec   voltage = %s   current = %s   d2_pressure = %s   n2_pressure = %s\n",
               local_max_neutron_pulse, voltage_str, current_str, d2_pressure_str, n2_pressure_str);
        printf("\n");
        INFO("=========================================================================\n");
        printf("\n");
#endif

        // reset for the next second
        RESET_FOR_NEXT_SEC;
    }

    // return 'continue-scanning' 
    return 0;
}

#ifdef DEBUG_PRINT_PULSE_GRAPH
static void print_plot_str(int32_t value, int32_t baseline)
{
    char    str[110];
    int32_t idx, i;

    // args are in mv units

    // value               : expected range 0 - 9995 mv
    // baseline            : expected range 0 - 9995 mv
    // idx = value / 100   : range  0 - 99            

    if (value > 9995) {
        printf("%5d: value is out of range\n", value);
        return;
    }
    if (baseline < 0 || baseline > 9995) {
        printf("%5d: baseline is out of range\n", baseline);
        return;
    }

    if (value < 0) {
        value = 0;
    }

    bzero(str, sizeof(str));

    idx = value / 100;
    for (i = 0; i <= idx; i++) {
        str[i] = '*';
    }

    idx = baseline / 100;
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

    printf("%5d: %s\n", value, str);
}
#endif
