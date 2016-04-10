#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "util_sdl.h"
#include "util_jpeg_decode.h"
#include "util_misc.h"

#include "about.h" //XXX

//
// defines
//

#define FILE_MAGIC      0x1234cdef
#define RECORD_MAGIC_1  0x11111111
#define RECORD_MAGIC_2  0x22222222

//
// typedefs
//

typedef struct {
    union {
        struct {
            int32_t file_magic;
            struct timeval tv;
        };
        char raw[512];
    };
} file_header_t;

typedef struct {
    int32_t record_magic_1;
    int32_t length;
    int32_t prior_length;
    struct timeval tv;

    int32_t voltage;
    int32_t current;
    int32_t pressure;
    int32_t neutrons;
    int32_t reserved;

    // record ends with the following 3 fields, 
    // the location of the length field is adjusted to be on 4 byte boundary
    // - char jpeg[];
    // - int32_t length
    // - int32_t record_magic_2
} file_record_t;

typedef struct {
    off_t   file_record_offset;
    int32_t voltage;
    int32_t current;
    int32_t pressure;
    int32_t neutrons;
} history_t;

//
// variables
//

history_t * history;
int32_t     history_idx_first;
int32_t     history_idx_last;

//
// prototypes
//

void init();
void display_handler();
void * data_file_thread(void * cx);

// -----------------  MAIN & INIT  ------------------------------------------

int main(int argc, char **argv)
{
    INFO("hello\n");

    sdl_init(1280, 800);
    sdl_display_text(about);
    sdl_close();

    INFO("TERMINATING\n");
    sleep(3);

    return 0;

    // initialize
    init();

    // display handler
    display_handler();

    // exit
    return 0;
}

void init()
{
    // initialize utilities
    //adc_init();
    //camera_init();
    //if (jpeg_decode_init() < 0) {
        // XXX FATAL("xxx");
    //}

    // open data file, if it doesn't exist create it

    // create thread to handle the data file
}

// -----------------  DISPLAY  ----------------------------------------------

void display_handler()
{
    // XXX display_init();

    while (true) {
        // get data to be displayed

        // clear display, and 
        // draw fixed elements

        // draw the jpeg

        // draw the sensor values

        // draw the graph

        // handle events

    }
}

// -----------------  DATA FILE THREAD  ------------------------------------

void * data_file_thread(void * cx)
{
    #define DATA_INTERVAL_MS 1000

    while (true) {
        // if initializing history then
        //   read entry from file until either:
        //    - done initializing, or
        //    - it is time to write the next entry
        // endif

        // sleep until it is time to write next entry

        // create fusor data record

        // write it to the file, and flush file
    }
}

