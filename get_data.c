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

#include "common.h"
#include "util_dataq.h"
#include "util_cam.h"
#include "util_misc.h"

#include <libusb/pmd.h>
#include <libusb/usb-20X.h>

//
// defines
//

#define GAS_ID_D2 0
#define GAS_ID_N2 1

//
// typedefs
//

//
// variables
//

uint8_t         jpeg_buff[1000000];
int32_t         jpeg_buff_len;
uint64_t        jpeg_buff_us;
pthread_mutex_t jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;


//
// prototypes
//

static void init(void);
static void server(void);
void * cam_thread(void * cx);
static void * server_thread(void * cx);
static void init_data_struct(data_t * data, time_t time_now);
static float convert_adc_voltage(float adc_volts);
static float convert_adc_current(float adc_volts);
static float convert_adc_pressure(float adc_volts, uint32_t gas_id);

// -----------------  MAIN & TOP LEVEL ROUTINES  -------------------------------------

int32_t main(int32_t argc, char **argv)
{
    init();
    server();
    return 0;
}

static void init(void)
{
    pthread_t thread;
    struct rlimit rl;
    int32_t ret;

    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    INFO("sizeof data_t=%zd part1=%zd part2=%zd\n",
         sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));

    if (cam_init(CAM_WIDTH, CAM_HEIGHT, FRAMES_PER_SEC) == 0) {
        if (pthread_create(&thread, NULL, cam_thread, NULL) != 0) {
            FATAL("pthread_create cam_thread, %s\n", strerror(errno));
        }
    }

    dataq_init(0.5,   // averaging duration in secs
               1200,  // scan rate  (samples per second)
               3,     // number of adc channels
               ADC_CHAN_VOLTAGE,
               ADC_CHAN_CURRENT,
               ADC_CHAN_PRESSURE);

    libusb_device_handle *mcc_udev_204 = NULL;  // XXX global
    ret = libusb_init(NULL);
    if (ret != LIBUSB_SUCCESS) {
        FATAL("libusb_init ret %d\n", ret);
    }
    mcc_udev_204 = usb_device_find_USB_MCC(USB204_PID, NULL);
    if (mcc_udev_204) {
        INFO("MCC-USB-204 found\n");
    } else {
        WARN("MCC-USB-204 not found\n");
    }

    // XXX temp
    exit(1);
}

static void server(void)
{
    struct sockaddr_in server_address;
    int32_t            listen_sockfd;
    int32_t            ret;
    pthread_t          thread;
    pthread_attr_t     attr;

    // create socket
    listen_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sockfd == -1) {
        FATAL("socket, %s\n", strerror(errno));
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
            FATAL("accept, %s\n", strerror(errno));
        }

        // create thread
        if (pthread_create(&thread, &attr, server_thread, (void*)(uintptr_t)sockfd) != 0) {
            FATAL("pthread_create server_thread, %s\n", strerror(errno));
        }
    }
}

void * cam_thread(void * cx) 
{
    int32_t   ret;
    uint8_t * ptr;
    uint32_t  len;

    while (true) {
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
}

// -----------------  SERVER_THREAD  -------------------------------------------------

static void * server_thread(void * cx)
{
    int32_t   sockfd = (uintptr_t)cx;
    time_t    time_now, time_last;
    ssize_t   len;
    data_t  * data;

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

    // terminate connection with client, and 
    // terminate thread
    close(sockfd);
    return NULL;
}

// -----------------  INIT_DATA_STRUCT  ----------------------------------------------

static void init_data_struct(data_t * data, time_t time_now)
{
    int16_t mean_mv, min_mv, max_mv;
    int32_t ret;
    bool    ret1, ret2, ret3;

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

    // data part1 voltage_min_kv, voltage_max_kv, and voltage_mean_kv
    ret = dataq_get_adc(ADC_CHAN_VOLTAGE, NULL, &mean_mv, NULL, &min_mv, &max_mv);
    if (ret == 0) {
        data->part1.voltage_min_kv  = convert_adc_voltage(min_mv/1000.);
        data->part1.voltage_max_kv  = convert_adc_voltage(max_mv/1000.);
        data->part1.voltage_mean_kv = convert_adc_voltage(mean_mv/1000.);
    } else {
        data->part1.voltage_min_kv  = ERROR_NO_VALUE;
        data->part1.voltage_max_kv  = ERROR_NO_VALUE;
        data->part1.voltage_mean_kv = ERROR_NO_VALUE;
    }

    // data part1 current_ma
    ret = dataq_get_adc(ADC_CHAN_CURRENT, NULL, &mean_mv, NULL, NULL, NULL);
    if (ret == 0) {
        data->part1.current_ma = convert_adc_current(mean_mv/1000.);
    } else {
        data->part1.current_ma = ERROR_NO_VALUE;
    }

    // data part1 pressure_xx_mtorr
    ret = dataq_get_adc(ADC_CHAN_PRESSURE, NULL, NULL, NULL, NULL, &max_mv);
    if (ret == 0) {
        data->part1.pressure_d2_mtorr = convert_adc_pressure(max_mv/1000., GAS_ID_D2);
        data->part1.pressure_n2_mtorr = convert_adc_pressure(max_mv/1000., GAS_ID_N2);
    } else {
        data->part1.pressure_d2_mtorr = ERROR_NO_VALUE;
        data->part1.pressure_n2_mtorr = ERROR_NO_VALUE;
    }

    // data part1 average_cpm XXX
    // data part1 moving_average_cpm XXX

    //
    // data part2
    //

    // data part2: magic
    data->part2.magic = MAGIC_DATA_PART2;

    // data part2: adc_samples
    ret1 = dataq_get_adc_samples(ADC_CHAN_VOLTAGE, 
                                 data->part2.voltage_adc_samples_mv,
                                 MAX_ADC_SAMPLES);
    ret2 = dataq_get_adc_samples(ADC_CHAN_CURRENT, 
                                 data->part2.current_adc_samples_mv,
                                 MAX_ADC_SAMPLES);
    ret3 = dataq_get_adc_samples(ADC_CHAN_PRESSURE, 
                                 data->part2.pressure_adc_samples_mv,
                                 MAX_ADC_SAMPLES);

    // data part2: jpeg_buff
    pthread_mutex_lock(&jpeg_mutex);
    if (microsec_timer() - jpeg_buff_us < 1000000) {
        memcpy(data->part2.jpeg_buff, jpeg_buff, jpeg_buff_len);
        data->part2.jpeg_buff_len = jpeg_buff_len;
    }
    pthread_mutex_unlock(&jpeg_mutex);

    //
    // data part1 (continued)
    //

    data->part1.data_part2_offset                        = 0;   // for display pgm
    data->part1.data_part2_length                        = sizeof(struct data_part2_s) + data->part2.jpeg_buff_len;
    data->part1.data_part2_jpeg_buff_valid               = (data->part2.jpeg_buff_len != 0);
    data->part1.data_part2_voltage_adc_samples_mv_valid  = (ret1 == 0);
    data->part1.data_part2_current_adc_samples_mv_valid  = (ret2 == 0);
    data->part1.data_part2_pressure_adc_samples_mv_valid = (ret3 == 0);
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

    return adc_volts * (1E9 / 94.34E3 / 1000.);    // kV
}

static float convert_adc_current(float adc_volts)
{
    // My fusor's current measurement resistor is 100 Ohm.

    // I = Vadc / 100            (amps)
    //
    // I = Vadc / 100 * 1000     (milli-amps)

    return adc_volts * 10.;    // mA
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
    { "D2",
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

