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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include "util_sdl.h"
#include "util_jpeg_decode.h"
#include "util_dataq.h"
#include "util_cam.h"
#include "util_misc.h"

//
// defines
//

//#define FILE_MAGIC      0x1234cdef
//#define RECORD_MAGIC_1  0x11111111
//#define RECORD_MAGIC_2  0x22222222
#define CAM_WIDTH       960
#define CAM_HEIGHT      720
#define VERSION_STR     "1.0"

#define ADC_CHAN_VOLTAGE   1
#define ADC_CHAN_CURRENT   2
#define ADC_CHAN_PRESSURE  3

//
// typedefs
//

#if 0
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
#endif

typedef struct {
    uint64_t  real_time_us;

    bool      pixel_buff_valid;
    uint8_t * pixel_buff;
    uint32_t  pixel_buff_width;
    uint32_t  pixel_buff_height;

    bool      data_valid;
    double    voltage_rms_kv;
    double    voltage_min_kv;
    double    voltage_max_kv;
    double    current_ma;
    double    pressure_mtorr;
} data_t;

enum mode {LIVE_MODE, PLAYBACK_MODE};

//
// variables
//

//history_t * history;
//int32_t     history_idx_first;
//int32_t     history_idx_last;
static bool       no_cam;
static bool       no_dataq;
static enum mode  mode;
static int32_t    fd;

//
// prototypes
//

void initialize(int32_t argc, char ** argv);
void usage(void);
char * bool2str(bool b);
void display_handler();
void get_data(data_t ** data);
void free_data(data_t * data);
//void * data_file_thread(void * cx);

// -----------------  MAIN  ----------------------------------------------------------

int32_t main(int32_t argc, char **argv)
{
    initialize(argc, argv);
    display_handler();
    return 0;
}

// -----------------  INITIALIZE  ----------------------------------------------------

void initialize(int32_t argc, char ** argv)
{
    char filename[100];

    // parse options
    // -n cam   : no camera, applies only in live mode
    // -n dataq : no data acquisition, applies only in live mode
    // -v       : version
    // -h       : help
    while (true) {
        char opt_char = getopt(argc, argv, "n:hv");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'n':
            if (strcmp(optarg, "cam") == 0) {
                no_cam = true;
            } else if (strcmp(optarg, "dataq") == 0) {
                no_dataq = true;
            } else {
                printf("invalid '-n %s'\n", optarg);
                exit(1);  
            }
            break;
        case 'h':
            usage();
            exit(0);  
        case 'v':
            printf("Version %s\n", VERSION_STR);
            exit(0);  
        default:
            exit(1);  
        }
    }

    // determine if in LIVE_MODE or PLAYBACK mode, and
    // the filename to be used
    if (argc == optind) {
        sprintf(filename, "fusor_xxx.dat"); // XXX
        mode = LIVE_MODE;
    } else {
        strcpy(filename, argv[optind]);
        mode = PLAYBACK_MODE;
    }

    // print args
    INFO("starting in %s mode\n",
         (mode == LIVE_MODE ? "LIVE" : "PLAYBACK"));
    INFO("  filename  = %s\n", filename);
    INFO("  no_cam    = %s\n", bool2str(no_cam));
    INFO("  no_dataq  = %s\n", bool2str(no_dataq));

    // if live mode
    //   create file for saving a recording of this run,
    //   initialize data acquisition,
    //   initialize camera
    // else  (playback mode)
    //   open the file containing the recording to be examined
    // endif
    if (mode == LIVE_MODE) {
        fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (fd < 0) {
            FATAL("failed to create %s, %s\n", filename, strerror(errno));
        }
        if (!no_dataq) {
            dataq_init(0.5, 3,
                       ADC_CHAN_VOLTAGE,
                       ADC_CHAN_CURRENT,
                       ADC_CHAN_PRESSURE);
        }
        if (!no_cam) {
            cam_init(CAM_WIDTH, CAM_HEIGHT);
        }
    } else {
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            FATAL("failed to open %s, %s\n", filename, strerror(errno));
        }
    }
}

void usage(void)
{
    printf("XXX usage tbd\n");
}

char * bool2str(bool b)
{
    return b ? "true" : "false";
}

// -----------------  DISPLAY HANDLER  -----------------------------------------------

void display_handler(void)
{
    bool          done;
    data_t      * data;
    sdl_event_t * event;
    rect_t        title_pane_full, title_pane; 
    rect_t        cam_pane_full, cam_pane;
    rect_t        dataq_pane_full, dataq_pane;
    rect_t        graph_pane_full, graph_pane;
    texture_t     cam_texture;
    char          str[100];

    time_t t;
    struct tm * tm;
    char title_str[100];
    int32_t win_width, win_height;
    int32_t font0_height;

    // XXX int32_t font1_height;

    // XXX verify cam width >= height, and explain

    // initializae 
    sdl_init(1280, 800);  // XXX defines
    cam_texture = sdl_create_yuy2_texture(CAM_HEIGHT,CAM_HEIGHT);
    done = false;

    // loop until done
    while (!done) {
        // get data to be displayed
        get_data(&data);

        // init for display update
        sdl_display_init();
        sdl_get_state(&win_width, &win_height, NULL);
        font0_height = sdl_font_char_height(0);
        //font1_height = sdl_font_char_height(1);
        sdl_init_pane(&title_pane_full, &title_pane, 
                      0, 0, 
                      win_width, font0_height+4);
        sdl_init_pane(&cam_pane_full, &cam_pane, 
                      0, font0_height+2, 
                      CAM_HEIGHT+4, CAM_HEIGHT+4); 
        sdl_init_pane(&dataq_pane_full, &dataq_pane, 
                      CAM_HEIGHT+2, font0_height+2, 
                      win_width-(CAM_HEIGHT+2), CAM_HEIGHT+4); 
        sdl_init_pane(&graph_pane_full, &graph_pane, 
                      0, font0_height+CAM_HEIGHT+4,
                      win_width, win_height-(font0_height+CAM_HEIGHT+4));

        // draw fixed elements
        sdl_render_pane_border(&title_pane_full, GREEN);
        sdl_render_pane_border(&cam_pane_full, GREEN);
        sdl_render_pane_border(&dataq_pane_full, GREEN);
        sdl_render_pane_border(&graph_pane_full, GREEN);

        // draw title
        if (mode == LIVE_MODE) {
            t = data->real_time_us / 1000000;
            tm = localtime(&t);
            sprintf(title_str, "LIVE MODE - %d/%d/%d %2.2d:%2.2d:%2.2d",
                    tm->tm_mon+1, tm->tm_mday, tm->tm_year-100,
                    tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else {
            sprintf(title_str, "PLAYBACK MODE - XXX");
        }
        sdl_render_text_ex(&title_pane, 0, 0, title_str, SDL_EVENT_NONE, sdl_pane_cols(&title_pane,0), true, 0);
        
        // draw the camera image
        if (data->pixel_buff_valid) {
            sdl_update_yuy2_texture(cam_texture, 
                                    data->pixel_buff + (CAM_WIDTH - CAM_HEIGHT) / 2,
                                    CAM_WIDTH);
            sdl_render_texture(cam_texture, &cam_pane);
        }

        // draw the sensor values
        if (data->data_valid) {
            sprintf(str, "%4.1f kV rms", data->voltage_rms_kv);
            sdl_render_text_font1(&dataq_pane, 0, 0, str, SDL_EVENT_NONE);

            sprintf(str, "%4.1f kV min", data->voltage_min_kv);
            sdl_render_text_font1(&dataq_pane, 1, 0, str, SDL_EVENT_NONE);

            sprintf(str, "%4.1f kV max", data->voltage_max_kv);
            sdl_render_text_font1(&dataq_pane, 2, 0, str, SDL_EVENT_NONE);

            sprintf(str, "%4.1f mA", data->current_ma);
            sdl_render_text_font1(&dataq_pane, 3, 0, str, SDL_EVENT_NONE);

            sprintf(str, "%4.1f mTorr", data->pressure_mtorr);
            sdl_render_text_font1(&dataq_pane, 4, 0, str, SDL_EVENT_NONE);
        }

        // draw the graph
        rect_t rect;
        rect.x = 0;
        rect.y = 0;
        rect.w = graph_pane.w;
        rect.h = graph_pane.h;
        sdl_render_fill_rect(&graph_pane, &rect, WHITE);

        // present the display
        sdl_display_present();

        // register for key press events
        sdl_event_register('q', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('Q', SDL_EVENT_TYPE_KEY, NULL);

        // process events
        event = sdl_poll_event();
        switch (event->event) {
        case 'q':
        case 'Q':
        case SDL_EVENT_QUIT:
            done = true;
            break;
        default:
            break;
        }

        // free the data
        // XXX tbd recording when in live mode
        free_data(data);
    }

    INFO("terminating\n");
}

// -----------------  GET AND FREE DATA  ---------------------------------------------

void get_data(data_t ** data_arg)
{
    int32_t ret;
    data_t * data;

    // allocate data buffer
    data = calloc(1, sizeof(data_t));
    if (data == NULL) {
        FATAL("alloc data failed\n");
    }

    // check if in LIVE_MODE vs PLAYBACK_MODE
    if (mode == LIVE_MODE) {
        // LIVE_MODE ...

        // get current time
        data->real_time_us = get_real_time_us();

        // get camera data
        if (!no_cam) {
            uint8_t  * jpeg_buff;
            uint32_t   jpeg_buff_len;
            uint32_t   jpeg_buff_id;

            // get a jpeg_buff from the camera
            cam_get_buff(&jpeg_buff, &jpeg_buff_len, &jpeg_buff_id);

            // convert the jpeg_buff to yuy2 format
            // note - on raspberry pi 3 this takes 36 ms for 960x720
            ret = jpeg_decode(0,  // cxid
                            JPEG_DECODE_MODE_YUY2,      
                            jpeg_buff, jpeg_buff_len,
                            &data->pixel_buff, &data->pixel_buff_width, &data->pixel_buff_height);
            if (ret < 0) {
                FATAL("jpeg_decode failed\n");
            }
            if (data->pixel_buff_width != CAM_WIDTH || data->pixel_buff_height != CAM_HEIGHT) {
                FATAL("jpeg_decode wrong dimensions w=%d h=%d\n",
                    data->pixel_buff_width, data->pixel_buff_height);
            }

            // return the jpeg_buff 
            cam_put_buff(jpeg_buff_id);

            // set valid
            data->pixel_buff_valid = true;
        }

        // get dataq data 
        // XXX but get at 2 hz, else reuse last vals ??
        // XXX don't get mean and sdev if we don't care
        if (!no_dataq) {
            double voltage_rms_adc;
            double voltage_min_adc;
            double voltage_max_adc;
            double current_adc;
            double pressure_adc;

            dataq_get_adc(
                ADC_CHAN_VOLTAGE, 
                &voltage_rms_adc, NULL,  NULL, &voltage_min_adc, &voltage_max_adc);
            dataq_get_adc(
                ADC_CHAN_CURRENT,
                 NULL, &current_adc, NULL, NULL, NULL);  
            dataq_get_adc(
                ADC_CHAN_PRESSURE, 
                NULL, &pressure_adc, NULL, NULL, NULL);  

            // XXX the following needs conversion function
            data->voltage_rms_kv = voltage_rms_adc;
            data->voltage_min_kv = voltage_min_adc;
            data->voltage_max_kv = voltage_max_adc;
            data->current_ma     = current_adc;  
            data->pressure_mtorr = pressure_adc;

            data->data_valid = true;  // XXX set based on ret
        }
    } else {
        // PLAYBACK_MODE ...

        FATAL("playback not supported yet\n");
    }

    // return data
    *data_arg = data;
}

void free_data(data_t * data) 
{
    free(data->pixel_buff);
    free(data);
}

#if 0
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
#endif
