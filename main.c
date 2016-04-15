#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>

#include "util_sdl.h"
#include "util_jpeg_decode.h"
#include "util_dataq.h"
#include "util_cam.h"
#include "util_misc.h"

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

    dataq_init(0.5, 4, 4,3,2,1);
    cam_init();
    sdl_init(1280, 800);

    texture_t texture;
    texture = sdl_create_yuy2_texture(640,480);

    bool done = false;
    while (!done) {
        uint8_t * jpeg_buff;
        uint32_t jpeg_buff_len;
        uint8_t * pixel_buff;
        uint32_t pixel_buff_width, pixel_buff_height;
        uint32_t buff_id;
        
        rect_t pane; 

        uint64_t start;
        int32_t ret;

        //static int count;
        //char str[44];

        sdl_event_t * event;

        cam_get_buff(&jpeg_buff, &jpeg_buff_len, &buff_id);
        printf("GOT ID %d\n", buff_id);
        //printf("jpeg_buff=%p  jpeg_buff_len=%d\n", jpeg_buff, jpeg_buff_len);

        // XXX time this
        start = microsec_timer();
        ret = jpeg_decode(0,  // cxid
                          JPEG_DECODE_MODE_YUY2,      
                          jpeg_buff, jpeg_buff_len,
                          &pixel_buff, &pixel_buff_width, &pixel_buff_height);
        if (ret < 0) {
            printf("jpeg decode failed\n");
        }
        printf("DURATION jpeg_decode = %"PRId64" us\n", (microsec_timer()-start));
        printf("decode ret=%d  w=%d  h=%d\n",
               ret, pixel_buff_width, pixel_buff_height);
        // XXX assert pixel w and h are correct

        //start = microsec_timer();
        cam_put_buff(buff_id);
        //printf("DURATION put_buff = %ld us\n", (microsec_timer()-start));

        sdl_display_init();

        sdl_init_pane(&pane, 0, 0, 640, 480);
        
        start = microsec_timer();
        sdl_update_yuy2_texture(texture, pixel_buff);
        printf("DURATION update_yuy2 = %"PRId64" us\n", (microsec_timer()-start));

        start = microsec_timer();
        sdl_render_texture(texture, &pane);
        printf("DURATION render_texture = %"PRId64" us\n", (microsec_timer()-start));

        start = microsec_timer();
        sdl_display_present();
        printf("DURATION display_present = %"PRId64" us\n", (microsec_timer()-start));

        sdl_event_register('q', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('Q', SDL_EVENT_TYPE_KEY, NULL);

        event = sdl_poll_event();
        switch (event->event) {
        case 'q':
            printf("Got q\n");
            done = true;
            break;
        case 'Q':
            printf("Got Q\n");
            done = true;
            break;
        case SDL_EVENT_QUIT:
            done = true;
            break;
        default:
            break;
        }

        free(pixel_buff);
    }

    printf("TERMINATING\n");
    exit(1);

#if 0
    sdl_init(1280, 800);

    static char * choices[] = {
                "Expanding Gas in a Container",
                "Gravity Simulation",
                "Our Expanding Universe",
                "Random Walk",
                "About",
                        };

    int32_t selection = -1;
    sdl_display_choose_from_list("-- Choose A Simulation --",
                                 choices, sizeof(choices)/sizeof(choices[0]),
                                 &selection);
    printf("selection %d\n", selection);
    sdl_close();
#endif

    dataq_init(0.5, 4, 4,3,2,1);

    while (true) pause();
    //sleep(100);

    INFO("TERMINATING\n");

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

