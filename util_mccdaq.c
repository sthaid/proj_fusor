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

//
// typedefs
//

//
// variables
//

static libusb_device_handle * g_udev;
static float                  g_cal_tbl[NCHAN_USB20X][2];
static uint16_t             * g_data;
static uint64_t               g_produced;
static mccdaq_callback_t      g_cb;

//
// protoytpes
//

static void * mccdaq_producer_thread(void * cx);
static void * mccdaq_consumer_thread(void * cx);

// -----------------  MCCDAQ INIT  --------------------------------------

int32_t mccdaq_init(mccdaq_callback_t cb)
{
    int32_t   ret;
    int32_t   usb_max_packet_size;
    pthread_t thread;

    // store callback
    g_cb = cb;

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
    g_produced = 0;

    // create threads
    if (pthread_create(&thread, NULL, mccdaq_producer_thread, NULL) != 0) {
        FATAL("pthread_create mccdaq_producer_thread, %s\n", strerror(errno));
    }
    if (pthread_create(&thread, NULL, mccdaq_consumer_thread, NULL) != 0) {
        FATAL("pthread_create mccdaq_consumer_thread, %s\n", strerror(errno));
    }

    // return success
    INFO("success\n");
    return 0;
}

// -----------------  MCCDAQ PRODUCER THREAD-----------------------------

// XXX the extra packet ?

static void * mccdaq_producer_thread(void * cx) 
{
    #define OPTIONS     0
    #define MAX_LENGTH  20000
    #define TOUT_MS     2000

    int32_t    ret, status;
    int32_t    length_avail, length, transferred_bytes;
    uint16_t * data = g_data;

    usbAInScanStart_USB20X(g_udev, 0, FREQUENCY, 1<<CHANNEL, OPTIONS, 0, 0);
    while (true) {
        length_avail = (g_data + MAX_DATA - data) * sizeof(uint16_t);
        length = (length_avail >= MAX_LENGTH ? MAX_LENGTH : length_avail);

        ret = libusb_bulk_transfer(g_udev, 
                                   LIBUSB_ENDPOINT_IN|1, 
                                   (uint8_t*)data,
                                   length, 
                                   &transferred_bytes, 
                                   TOUT_MS);
        status = usbStatus_USB20X(g_udev);
        DEBUG("ret=%d length=%d transferred_byts=%d status=%d\n", 
              ret, length, transferred_bytes, status);

        if ((ret == LIBUSB_ERROR_PIPE) || !(status & AIN_SCAN_RUNNING)) {  // XXX other ret
            WARN("restarting, ret==%d status=0x%x\n", ret, status);
            libusb_clear_halt(g_udev, LIBUSB_ENDPOINT_IN|1);
            usbAInScanStart_USB20X(g_udev, 0, FREQUENCY, 1<<CHANNEL, OPTIONS, 0, 0);
        }

        g_produced += transferred_bytes / 2;
        data       += transferred_bytes / 2;
        if (data > g_data + MAX_DATA) {
            FATAL("data out of range, %zd\n", data-g_data);
        }
        if (data == g_data + MAX_DATA) {
            INFO("resetting data to begining of circular buffer\n");
            data = g_data;
        }
    }

    return NULL;
}

// -----------------  MCCDAQ CONSUMER THREAD-----------------------------

static void * mccdaq_consumer_thread(void * cx) 
{
    int64_t    consumed = 0;
    int64_t    produced;
    int64_t    count, max_count;
    uint16_t * data;

    while (true) {
        // wait for data
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

        // call callback to process the data
        count = produced - consumed;
        data = g_data + (consumed % MAX_DATA);
        max_count = g_data + MAX_DATA - data;
        if (count <= max_count) {
            g_cb(data, count);
        } else {
            g_cb(data, max_count);
            g_cb(g_data, count-max_count);
        }
        consumed += count;
    }

    return NULL;
}
