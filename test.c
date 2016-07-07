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

#include "util_mccdaq.h"
#include "util_misc.h"

#define MAX_DATA_STORE (10*500000)   // 10 secs of samples

static uint16_t data_store[MAX_DATA_STORE];
static uint32_t max_data_store;

void mccdaq_callback(uint16_t * data, int32_t max_data);

// --------------------------------------------------------------

int32_t main(int argc, char ** argv)
{
    int32_t ret;

    ret = mccdaq_init(mccdaq_callback);
    if (ret != 0) {
        FATAL("mccdaq_init ret %d\n", ret);
    }

    INFO("WAITING FOR DATA STORE TO FILL\n");
    while (max_data_store < MAX_DATA_STORE) {
        usleep(10000);
    }

    INFO("DATA STORE IS FILL, num samples = %d\n", max_data_store);
    pause();

    return 0;
}

void mccdaq_callback(uint16_t * data, int32_t max_data)
{
    uint64_t        intvl, curr_us;
    int32_t         num_avail, num_copy;

    static uint64_t start_us, count
    
    DEBUG("max_data %d\n", max_data);

    if (max_data_store == MAX_DATA_STORE) {
        // XXX shouldnt need this code if stoped
        return;
    }

    curr_us = microsec_timer();
    intvl   = curr_us - start_us;
    count  += max_data;
    if (start_us == 0 || intvl >= 1000000) { 
        if (start_us != 0) {
            INFO("rate = %"PRId64" samples/sec\n", count * 1000000 / intvl);
        }
        start_us = curr_us;
        count = 0;
    }

    num_avail = MAX_DATA_STORE - max_data_store;
    num_copy = (num_avail <= max_data ? num_avail : max_data);
    memcpy(data_store, data, num_copy*2);
    max_data_store += num_copy;

    if (max_data_store == MAX_DATA_STORE) {
        INFO("data store is full, max_data_store=%d\n", max_data_store);
        // XXX should stop here
    }
}
