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

// XXX - TODO
// - the extra packet ?
// - pass cal tbl to callback ?
// - ctrl c and atexit

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
#include <libusb/pmd.h>
#include <libusb/usb-20X.h>

#include "util_mccdaq.h"
#include "util_misc.h"

//
// defines
//

#define CHANNEL    0
#define FREQUENCY  499999         // samples per second
#define MAX_DATA   (20*500000)    // 20 secs of data

#define STATE_CHANGE(new_state) \
    do { \
        INFO("state is now %s\n", STATE_STRING(new_state)); \
        g_state = (new_state); \
    } while (0)
#define STATE_STRING(x) \
   ((x) == NOT_INITIALIZED  ? "NOT_INITIALIZED" : \
    (x) == STOPPED          ? "STOPPED"         : \
    (x) == RUNNING          ? "RUNNING"         : \
    (x) == STOPPING         ? "STOPPING"          \
                            : "ERROR")

//
// typedefs
//

enum state { NOT_INITIALIZED, STOPPED, RUNNING, STOPPING, ERROR };

//
// variables
//

static libusb_device_handle * g_udev;
static float                  g_cal_tbl[NCHAN_USB20X][2];
static uint16_t             * g_data;
static uint64_t               g_produced;
static mccdaq_callback_t      g_cb;
static enum state             g_state;
static bool                   g_producer_thread_running;
static bool                   g_consumer_thread_running;

//
// protoytpes
//

static void mccdaq_exit(void);
static void * mccdaq_producer_thread(void * cx);
static void * mccdaq_consumer_thread(void * cx);

// -----------------  PUBLIC ROUTINES  ----------------------------------

int32_t mccdaq_init(void)
{
    int32_t   ret;
    int32_t   usb_max_packet_size;

    // init usb library
    ret = libusb_init(NULL);
    if (ret != LIBUSB_SUCCESS) {
        FATAL("libusb_init ret %d\n", ret);
    }

    // find the MCC-USB-204 usb device
    g_udev = usb_device_find_USB_MCC(USB204_PID, NULL);
    if (!g_udev) {
        ERROR("MCC-USB-204 not found\n");
        return -1;
    }

    // print the usb packet size, should be 64 
    usb_max_packet_size = usb_get_max_packet_size(g_udev,0);
    INFO("usb_max_packet_size = %d\n", usb_max_packet_size);
    if (usb_max_packet_size != 64) {
        FATAL("usb_max_packet_size = %d\n", usb_max_packet_size);
    }

    // get the calibration date, and print
    struct tm calDate;
    usbCalDate_USB20X(g_udev, &calDate);
    INFO("MFG Calibration date = %s\n", asctime(&calDate));

    // get the calibration table, and print channel 0 values
    int32_t idx = 0;
    usbBuildGainTable_USB20X(g_udev, g_cal_tbl);
    INFO("Calibration Table %d: Slope=%f  Offset=%f\n", 
         idx, g_cal_tbl[idx][0], g_cal_tbl[idx][1]);

    // allocate memory for producer
    g_data = calloc(MAX_DATA, sizeof(uint16_t));
    if (g_data == NULL) {
        FATAL("calloc size %zd", MAX_DATA*sizeof(uint16_t));
    }
    g_produced = 0;

    // set state to stopped
    STATE_CHANGE(STOPPED);

    // register exit handler
    atexit(mccdaq_exit);

    // return success
    INFO("success\n");
    return 0;
}

int32_t  mccdaq_start(mccdaq_callback_t cb)
{
    pthread_t thread;

    // if not initialized then return error
    if (g_state == NOT_INITIALIZED) {
        ERROR("not initialized\n");
        return -1;
    }

    // if already running then return error
    if (g_state == RUNNING) {
        ERROR("alreday running\n");
        return -1;
    }

    // if stopping or in error state then wait for threads to stop
    if (g_state == STOPPING || g_state == ERROR) {
        while (g_producer_thread_running || g_consumer_thread_running) {
            usleep(1000);
        }
        STATE_CHANGE(STOPPED);
    }

    // sanity check
    if (g_state != STOPPED) {
        FATAL("state should be STOPPED, but is %d\n", g_state);
    }

    // clear data
    memset(g_data, -1, MAX_DATA*sizeof(uint16_t));
    g_produced = 0;

    // store callback
    g_cb = cb;

    // set state
    STATE_CHANGE(RUNNING);

    // create threads
    if (pthread_create(&thread, NULL, mccdaq_producer_thread, NULL) != 0) {
        FATAL("pthread_create mccdaq_producer_thread, %s\n", strerror(errno));
    }
    if (pthread_create(&thread, NULL, mccdaq_consumer_thread, NULL) != 0) {
        FATAL("pthread_create mccdaq_consumer_thread, %s\n", strerror(errno));
    }

    // return success
    return 0;
}

int32_t mccdaq_stop(void)
{
    // if not initialized then return error
    if (g_state == NOT_INITIALIZED) {
        ERROR("not initialized\n");
        return -1;
    }

    // set state to STOPPING
    STATE_CHANGE(STOPPING);

    // wait for threads to be not running
    while (g_producer_thread_running || g_consumer_thread_running) {
        usleep(1000);
    }

    // set state to STOPPED
    STATE_CHANGE(STOPPED);

    // return success
    return 0;
}

// -----------------  MCCDAQ EXIT HANDLER  D-----------------------------

static void mccdaq_exit(void)
{
    // stop, and call cleanup 
    mccdaq_stop();
    cleanup_USB20X(g_udev);
}

// -----------------  MCCDAQ PRODUCER THREAD-----------------------------

static void * mccdaq_producer_thread(void * cx) 
{
    #define OPTIONS     0
    #define MAX_LENGTH  20000
    #define TOUT_MS     2000

    int32_t    ret, status;
    int32_t    length_avail, length, transferred_bytes;
    uint16_t * data = g_data;

    g_producer_thread_running = true;

    // start the analog input scan
    usbAInScanStart_USB20X(g_udev, 0, FREQUENCY, 1<<CHANNEL, OPTIONS, 0, 0);

    // loop, performing usb buld transfers
    while (true) {
        // if state is STOPPING or ERROR then
        //   exit thread
        // endif
        if (g_state == STOPPING || g_state == ERROR) {
            break;
        }

        // determine number of bytes to request in call to libusb_bulk_transfer;
        // this is normally MAX_LENGTH, but will be less when the data pointer nears
        // the end of the g_data buffer
        length_avail = (g_data + MAX_DATA - data) * sizeof(uint16_t);
        length = (length_avail >= MAX_LENGTH ? MAX_LENGTH : length_avail);

        // transfer analog data from mcc usb 204 device to data buffer
        ret = libusb_bulk_transfer(g_udev, 
                                   LIBUSB_ENDPOINT_IN|1, 
                                   (uint8_t*)data,
                                   length, 
                                   &transferred_bytes, 
                                   TOUT_MS);
        status = usbStatus_USB20X(g_udev);
        DEBUG("ret=%d length=%d transferred_byts=%d status=%d\n", 
              ret, length, transferred_bytes, status);

        //XXX INFO("dataidx %zd   value %d\n",
             //XXX data-g_data, data[0]);

        // if error has occurred that can be corrected then
        //   restart the analog input scan
        // else if an uncorrectable error occurred then
        //   set state to ERROR and get out
        // endif
        if ((ret == LIBUSB_ERROR_PIPE) || !(status & AIN_SCAN_RUNNING)) {
            WARN("restarting, ret==%d status=0x%x\n", ret, status);
            libusb_clear_halt(g_udev, LIBUSB_ENDPOINT_IN|1);
            usbAInScanStart_USB20X(g_udev, 0, FREQUENCY, 1<<CHANNEL, OPTIONS, 0, 0);
        } else if (ret != 0 || transferred_bytes == 0) {
            ERROR("ret=%d transferred_bytes=%d\n", ret, transferred_bytes);
            STATE_CHANGE(ERROR);
            break;
        }

        // make data available to consumer thread
        g_produced += transferred_bytes / 2;

        // update data pointer to prepare for next call to libusb_bulk_transfer
        data += transferred_bytes / 2;
        if (data > g_data + MAX_DATA) {
            FATAL("data out of range, %zd\n", data-g_data);
        }
        if (data == g_data + MAX_DATA) {
            INFO("resetting data to begining of circular buffer\n");
            data = g_data;
        }
    }

    // stop the scan
    usbAInScanStop_USB20X(g_udev);
    usbAInScanClearFIFO_USB20X(g_udev);

    g_producer_thread_running = false;
    return NULL;
}

// -----------------  MCCDAQ CONSUMER THREAD-----------------------------

static void * mccdaq_consumer_thread(void * cx) 
{
    int64_t    consumed = 0;
    int64_t    produced;
    int64_t    count, max_count;
    uint16_t * data;

    g_consumer_thread_running = true;

    while (true) {
        // if state is STOPPING or ERROR then
        //   if error then  
        //     call calback with error flag set
        //   endif
        //   exit thread
        // endif
        if (g_state == STOPPING || g_state == ERROR) {
            if (g_state == ERROR) {
                g_cb(NULL, 0, true);
            }
            break;
        }

        // if no data then delay 
        produced = g_produced;
        if (produced == consumed) {
            usleep(1000);
            continue;
        }

        // if too far behind then discard data
        if (produced - consumed > 500000) {
            INFO("falling behind, discarding %"PRId64" samples\n", produced-consumed);
            consumed = produced;
            continue;
        }

        // call callback to process the data;
        // if callback requests stop then enter stopping state and exit thread
        count = produced - consumed;
        data = g_data + (consumed % MAX_DATA);
        //XXX INFO("CONSUMER dataidx %zd\n", data-g_data);
        max_count = g_data + MAX_DATA - data;
        if (count <= max_count) {
            if (g_cb(data, count, false)) {
                STATE_CHANGE(STOPPING);
                break;
            }
        } else {
            if (g_cb(data, max_count, false) || g_cb(g_data, count-max_count, false)) {
                STATE_CHANGE(STOPPING);
                break;
            }
        }

        // increase the amount consumed
        consumed += count;
    }

    g_consumer_thread_running = false;

    return NULL;
}
