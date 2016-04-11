#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <termios.h>
#include <fcntl.h>
#include <pthread.h>

#include "util_dataq.h"
#include "util_misc.h"

//
// defines
//

#define ENABLE_TEST_THREAD

#define DATAQ_DEVICE "/dev/serial/by-id/usb-0683_1490-if00"
#define MAX_RESP     100
#define MAX_VAL      2500
#define SCALE        (10.0/2048)  // volts per count

//
// typedefs
//

typedef struct {
    int32_t val[MAX_VAL];
    int32_t sum;
    int32_t idx;
} adc_t;

//
// variables
//

static int      fd;
static adc_t    adc[4];
static int64_t  scan_count;
static bool     scan_okay;

//
// prototypes
//

static void exit_handler(void);
static void issue_cmd(char * cmd, char * resp);
static void * receive_data_thread(void * cx);
static void process_adc_value(int32_t adc_idx, int32_t new_val);
static void * monitor_thread(void * cx);
#ifdef ENABLE_TEST_THREAD
static void * test_thread(void * cx);
#endif

// --------------------------------------------------------------------------------------

// XXX add missing comments

// XXX args
// XXX   channel_list, and
// XXX   interval to average ovr
void dataq_init()
{
    char resp[MAX_RESP];
    pthread_t thread_id;

    // open the dataq virtual com port
    fd = open(DATAQ_DEVICE, O_RDWR);
    if (fd < 0) {
        FATAL("failed to open %s, %s\n", DATAQ_DEVICE, strerror(errno));
    }

    // issue 'stop' scanning command
    // flush data received but not read
    write(fd, "stop\r", 5);
    usleep(100000);
    tcflush(fd, TCIFLUSH);

    // issue 'info 0' command
//XXX check the responses in the routine
    issue_cmd("info 0", resp);
    if (strcmp(resp, "info 0 DATAQ") != 0) {
        FATAL("invalid response rcvd from 'info 0' cmd, resp='%s'\n", resp);
    }

    // configure scanning of the desired adc channels
    issue_cmd("asc", resp);
    issue_cmd("slist 0 x0000", resp);
    issue_cmd("slist 1 x0001", resp);
    issue_cmd("slist 2 x0002", resp);
    issue_cmd("slist 3 x0003", resp);

    // set the scan rate; 
    // use the max rate supported by the dataq, which is
    //    10000 / num_enabled_scan_list_elements
    //XXX make hz global, and use in monitor
    int32_t srate, hz;
    char srate_cmd_str[100];
    srate = 75 * 4;   // XXX 4 i
    hz = 750000 / srate;
    printf("srate = %d  hz = %d\n", srate, hz);  // XXX make DEBUG print
    sprintf(srate_cmd_str, "srate x%4.4x", srate);
    issue_cmd(srate_cmd_str, resp);

    // configure binary output
    issue_cmd("bin", resp);

    // start scan
    write(fd, "start\r", 6);

    // stop scanning atexit
    atexit(exit_handler);

    // create threads 
    // - receive data from the adc
    // - monitor health of the receive_data_thread
    // - optional test code
    pthread_create(&thread_id, NULL, receive_data_thread, NULL);
    pthread_create(&thread_id, NULL, monitor_thread, NULL);
#ifdef ENABLE_TEST_THREAD
    pthread_create(&thread_id, NULL, test_thread, NULL);
#endif
}

static void exit_handler()
{
    int32_t len;

    len = write(fd, "stop\r", 5);
    if (len != 5) {
        ERROR("issuing stop cmd\n");
    }
}

static void issue_cmd(char * cmd, char * resp)
{
    char cmd2[100];
    int32_t len, ret, total_len, i;
    char *p;
    bool succ;

    // terminate command with <cr>
    strcpy(cmd2, cmd);
    strcat(cmd2, "\r");

    // write command
    len = write(fd, cmd2, strlen(cmd2));
    if (len != strlen(cmd2)) {
        FATAL("failed write cmd '%s', %s\n", cmd, strerror(errno));
    }

    // read response, non blocking, with 1 sec tout
    ret = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (ret < 0) {
        FATAL("failed to set non blocking, %s\n", strerror(errno));
    }

    // XXX comments
    p = resp;
    total_len = 0;
    succ = false;
    for (i = 0; i < 200; i++) {
        len = read(fd, p, MAX_RESP-total_len);
        if (len > 0) {
            total_len += len;
            p += len;
            if (resp[total_len-1] == '\r') {
                resp[total_len-1] = '\0';   // remove <cr>
                total_len--;
                succ = true;
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

    // XXX comments
    ret = fcntl(fd, F_SETFL, 0);
    if (ret < 0) {
        FATAL("failed to clear non blocking, %s\n", strerror(errno));
    }

    // check for failure to read response
    if (!succ) {
        FATAL("response to cmd '%s' was not received\n", cmd);
    }

    // XXX check that response received was correct

    // debug print
    DEBUG("okay: cmd='%s', resp='%s'\n", cmd, resp);
}

static void * receive_data_thread(void * cx)
{
    uint8_t buff[1000];
    uint8_t *p;
    int32_t len, buff_len, adc_idx; 

    // init
    bzero(buff, sizeof(buff));
    buff_len = 0;

    // verify start response received
    // XXX tbd non blocking
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

        // while there is enough data read for all sensors being scanned,
        // and to validate sync then extract adc values
        while (buff_len >= 9) {
            // if not sychronized then abort for now
            // XXX improve this to resync
            if (((buff[0] & 1) != 0) || ((buff[8] & 1) != 0)) {
                FATAL("not synced\n");
            }

            // extract adc values from buff
            p = buff;
            for (adc_idx = 0; adc_idx < 4; adc_idx++) {  // XXX 4
                int32_t new_val;
    
                new_val = ((p[1] & 0xfe) << 4) | (p[0] >> 3);
                new_val ^= 0x800;
                if (new_val & 0x800) {
                    new_val |= 0xfffff000;
                }

                process_adc_value(adc_idx, new_val);

                p += 2;
            }

            // shift buff, and adjust buff_len
            memmove(buff, buff+8, buff_len-8);
            buff_len -= 8;

            // bump up scan_count, which is used by the monitor_thread to
            // determine if scanning is working 
            scan_count++;
        }
    }

    return NULL;
}

static void process_adc_value(int32_t adc_idx, int32_t new_val)
{
    int32_t old_val;
    adc_t * x = &adc[adc_idx];
    
    old_val = x->val[x->idx];
    x->val[x->idx] = new_val;
    x->idx = (x->idx + 1) % MAX_VAL;

    x->sum += (new_val - old_val);
}

// XXX return sdev too
// XXX pass in the channel number, not adc_idx
int32_t dataq_get_adc(int32_t adc_idx, double * v_avg, double * v_min, double * v_max)
{
    int32_t min, max, i;
    adc_t * x = &adc[adc_idx];

    if (!scan_okay) {
        *v_avg = 999;
        *v_min = 999;
        *v_max = 999;
        return -1;
    }

    *v_avg = (double)x->sum / MAX_VAL * SCALE;

    if (v_min || v_max) {
        min = +10000;
        max = -10000;
        for (i = 0; i < MAX_VAL; i++) {
            if (x->val[i] < min) {
                min = x->val[i];
            }
            if (x->val[i] > max) {
                max = x->val[i];
            }
        }
        if (v_min) {
            *v_min = (double)min * SCALE;
        }
        if (v_max) {
            *v_max = (double)max * SCALE;
        }
    }

    return 0;
}

static void * monitor_thread(void * cx)
{
    uint64_t last_scan_count;
    uint64_t curr_scan_count;
    uint64_t delta_scan_count;

    last_scan_count = scan_count;

    while (true) {
        sleep(1);

        curr_scan_count = scan_count;
        delta_scan_count = curr_scan_count - last_scan_count;
        last_scan_count = curr_scan_count;

        if (delta_scan_count > 2200 && delta_scan_count < 2800) {
            if (!scan_okay) {
                INFO("dataq adc scan okay, delta_scan_count=%ld\n", delta_scan_count);
            }
            scan_okay = true;
        } else {
            if (scan_okay) {
                ERROR("dataq adc scan failure, delta_scan_count=%ld\n", delta_scan_count);
            }
            scan_okay = false;
        }
    }

    return NULL;
}        
        
#ifdef ENABLE_TEST_THREAD
static void * test_thread(void * cx)
{
    double v_avg, v_min, v_max;
    int32_t ret;

    sleep(1);

    // XXX improve this code, dump all channels, , print date/time
    while (true) {
        sleep(1);
        ret = dataq_get_adc(0, &v_avg, &v_min, &v_max);
        if (ret == 0) {
            printf("v_avg=%6.3f  v_min=%6.3f  v_max=%6.3f\n", v_avg, v_min, v_max);
        } else {
            printf("ERROR voltage values not avialable\n");
        }
    }
    return NULL;
}
#endif
