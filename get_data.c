// XXX review FATAL, should instead drop the connection
// XXX make static routines
/*
main
- init dataq
- init mccdaq
- listen and accept connections

server thread
  while true
    gather data and send
    wait for next second
  endwhile

The data is:
  - magic
  - part1
    - hv info
    - current
    - pressure for both d2 and n2
    - scaler counts for each channnel, average and moving average
  - part2
    - diagnostic voltage values for a 1000? sample interval that can be plotted
      ( hv, current, pressure, scaler amp out)
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
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "util_dataq.h"
#include "util_cam.h"
#include "util_misc.h"

// XXX common.h here
#define CAM_WIDTH   960
#define CAM_HEIGHT  720

#define ADC_CHAN_VOLTAGE           1  // XXX names
#define ADC_CHAN_CURRENT           2
#define ADC_CHAN_CHAMBER_PRESSURE  3

#define PORT 9001

#define MAX_DETECTOR_CHAN    4
#define MAX_ADC_DIAG_CHAN    4
#define MAX_ADC_DIAG_VALUE   1000

#define DATA_MAGIC  0xaabbccdd

#define IS_ERROR(x) ((int32_t)(x) >= ERROR_FIRST && (int32_t)(x) <= ERROR_LAST)
#define ERROR_FIRST                   1000000
#define ERROR_PRESSURE_SENSOR_FAULTY  1000000
#define ERROR_OVER_PRESSURE           1000001
#define ERROR_NO_VALUE                1000002
#define ERROR_LAST                    1000002
#define ERROR_TEXT(x) \
    ((int32_t)(x) == ERROR_PRESSURE_SENSOR_FAULTY ? "FAULTY" : \
     (int32_t)(x) == ERROR_OVER_PRESSURE          ? "OVPRES" : \
     (int32_t)(x) == ERROR_NO_VALUE               ? "NOVAL"    \
                                                  : "????")


typedef struct {
    uint32_t magic;
    struct data_part1_s {
        float voltage_mean_kv;
        float voltage_min_kv;
        float voltage_max_kv;
        float current_ma;
        float chamber_pressure_d2_mtorr;
        float chamber_pressure_n2_mtorr;
        float average_cpm[MAX_DETECTOR_CHAN];
        float moving_average_cpm[MAX_DETECTOR_CHAN];
    } part1;
    struct data_part2_s {
        int16_t adc_diag[MAX_ADC_DIAG_CHAN][MAX_ADC_DIAG_VALUE];
        uint32_t cam_buff_len;
        uint8_t cam_buff[0];
    } part2;
} data_t;

// XXX


//
// defines
//

//
// typedefs
//

//
// variables
//

//
// prototypes
//

static void init(void);
static void server(void);
static void * server_thread(void * cx);
static int32_t init_data_struct(data_t * data);
static float convert_adc_voltage(float adc_volts);
static float convert_adc_current(float adc_volts);
static float convert_adc_chamber_pressure(float adc_volts, uint32_t gas_id);

// -----------------  MAIN  ----------------------------------------------------------

int32_t main(int32_t argc, char **argv)
{
    init();
    server();
    return 0;
}

static void init(void)
{
    // XXX
    return;
    cam_init(CAM_WIDTH, CAM_HEIGHT);
    dataq_init(0.5, 4,   // XXX set the rate too?
               ADC_CHAN_VOLTAGE,
               ADC_CHAN_CURRENT,
               ADC_CHAN_CHAMBER_PRESSURE);
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
    ret = pthread_attr_init(&attr);
    if (ret == -1) {
        FATAL("pthread_attr_init, %s\n", strerror(errno));
    }
    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (ret == -1) {
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
        ret = pthread_create(&thread, &attr, server_thread, (void*)(uintptr_t)sockfd);
        if (ret == -1) {
            FATAL("pthread_create server_thread, %s\n", strerror(errno));
        }
    }
}

// -----------------  SERVER_THREAD  -------------------------------------------------

static void * server_thread(void * cx)
{
    int32_t   sockfd = (uintptr_t)sockfd;   // XXX uintptr
    time_t    time_now, time_last;
    uint8_t * jpeg_buff_ptr;
    uint32_t  jpeg_buff_len;
    ssize_t   len;
    data_t    data;

    INFO("accepted connection, sockfd=%d\n", sockfd);

    time_last = time(NULL);

    while (true) {
        // get camera image
        cam_get_buff(&jpeg_buff_ptr, &jpeg_buff_len);

        // if time now is same as time last then discard the image
        time_now = time(NULL);
        if (time_now == time_last) {
            cam_put_buff(jpeg_buff_ptr);
            continue;
        }

        // sanity check time_now, should be time_last+1
        if (time_now < time_last) {
            FATAL("time has gone backwards\n");
        }
        if (time_now != time_last+1) {
            WARN("time_now - time_last = %ld\n", time_now-time_last);
        }

        // init data struct
        // XXX check ret
        init_data_struct(&data);

        // send data struct
        // XXX does this send all
        len = send(sockfd, &data, sizeof(data), 0);
        if (len != sizeof(data)) {
            FATAL("send failed, len=%zd, %s\n", len, strerror(errno));
        }
    }

    return NULL;
}

// -----------------  INIT_DATA_STRUCT  ----------------------------------------------

static int32_t init_data_struct(data_t * data)
{
    int16_t mean_mv, min_mv, max_mv;
    int32_t ret;

    // zero and set magic
    bzero(data, sizeof(data_t));
    data->magic = DATA_MAGIC;

    // data part1 voltage
    ret = dataq_get_adc(ADC_CHAN_VOLTAGE, NULL, &mean_mv, NULL, &min_mv, &max_mv,
                        0, NULL, NULL);
    if (ret != 0) {
        return -1;
    }
    data->part1.voltage_min_kv  = convert_adc_voltage(min_mv/1000.);
    data->part1.voltage_max_kv  = convert_adc_voltage(max_mv/1000.);
    data->part1.voltage_mean_kv = convert_adc_voltage(mean_mv/1000.);

    // data part1 current
    ret = dataq_get_adc(ADC_CHAN_CURRENT, NULL, &mean_mv, NULL, NULL, NULL,
                        0, NULL, NULL);
    if (ret != 0) {
        return -1;
    }
    data->part1.current_ma = convert_adc_current(mean_mv/1000.);

    // return success
    return 0;
}

#if 0
            // read ADC_CHAN_CHAMBER_PRESSURE and convert to mTorr
            ret = dataq_get_adc(ADC_CHAN_CHAMBER_PRESSURE, NULL, NULL, NULL, NULL, &max_mv,
                                0, NULL, NULL);
            if (ret != 0) {
                break;
            }
            data->gas_id = gas_get_id();
            data->chamber_pressure_mtorr = convert_adc_chamber_pressure(max_mv/1000., data->gas_id);


        OK data.part1.voltage_mean_kv;
        OK data.part1.voltage_min_kv;
        OK data.part1.voltage_max_kv;
        OK data.part1.current_ma;
        data.part1.chamber_pressure_d2_mtorr;
        data.part1.chamber_pressure_n2_mtorr;
        data.part1.average_cpm[MAX_DETECTOR_CHAN];
        data.part1.moving_average_cpm[MAX_DETECTOR_CHAN];
/*
typedef struct {
    uint32_t magic;
    struct data_part1_s {
        float voltage_mean_kv;
        float voltage_min_kv;
        float voltage_max_kv;
        float current_ma;
        float chamber_pressure_d2_mtorr;
        float chamber_pressure_n2_mtorr;
        float average_cpm[MAX_DETECTOR_CHAN];
        float moving_average_cpm[MAX_DETECTOR_CHAN];
    } part1;
    struct data_part2_s {
        int16_t adc_diag[MAX_ADC_DIAG_CHAN][MAX_ADC_DIAG_VALUE];
        uint32_t cam_buff_len;
        uint8_t cam_buff[0];
    } part2;
} data_t;
*/
#endif


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

// -----------------  CONVERT ADC CHAMBER PRESSURE GAUGE  ----------------------------

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

static float convert_adc_chamber_pressure(float adc_volts, uint32_t gas_id)
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

