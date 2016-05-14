// XXX xxx
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

#include <time.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "util_sdl.h"
#include "util_jpeg_decode.h"
#include "util_dataq.h"
#include "util_cam.h"
#include "util_misc.h"

#include "about.h"

//
// defines
//

#define VERSION_STR  "1.0"

#define DEFAULT_WIN_WIDTH   1920
#define DEFAULT_WIN_HEIGHT  1080

#define CAM_WIDTH   960
#define CAM_HEIGHT  720

#define ADC_CHAN_VOLTAGE   1
#define ADC_CHAN_CURRENT   2
#define ADC_CHAN_PRESSURE  3

#define MAX_HISTORY  100000

#define FONT0_HEIGHT (sdl_font_char_height(0))
#define FONT0_WIDTH  (sdl_font_char_width(0))

#define MODE_STR(m) ((m) == LIVE_MODE ? "LIVE" : "PLAYBACK")

#define MAGIC 0x1122334455667788

#define ASSERT_IN_ANY_MODE() \
    do { \
        if (mode != LIVE_MODE && mode != PLAYBACK_MODE) { \
            FATAL("mode must be LIVE_MODE or PLAYBACK_MODE\n"); \
        } \
    } while (0)
#define ASSERT_IN_LIVE_MODE() \
    do { \
        if (mode != LIVE_MODE) { \
            FATAL("mode must be LIVE_MODE\n"); \
        } \
    } while (0)
#define ASSERT_IN_PLAYBACK_MODE() \
    do { \
        if (mode != PLAYBACK_MODE) { \
            FATAL("mode must be PLACKBACK_MODE\n"); \
        } \
    } while (0)

#define IS_ERROR(x) ((int32_t)(x) >= ERROR_FIRST && (int32_t)(x) <= ERROR_LAST)
#define ERROR_FIRST                   1000000
#define ERROR_PRESSURE_SENSOR_FAULTY  1000000
#define ERROR_OVER_PRESSURE           1000001
#define ERROR_LAST                    1000001
#define ERROR_TEXT(x) \
    ((int32_t)(x) == ERROR_PRESSURE_SENSOR_FAULTY ? "FAULTY" : \
     (int32_t)(x) == ERROR_OVER_PRESSURE          ? "OVPRES"  \
                                                  : "????")

//
// typedefs
//

enum mode {LIVE_MODE, PLAYBACK_MODE};

// this struct is designed to be same size on 32bit and 64bit systems, so 
// that the fusor data file is portable
typedef struct {
    uint64_t     magic;
    uint64_t     time_us;

    bool         data_valid;
    uint8_t      gas_id;
    uint8_t      reserved1[2];
    float        voltage_rms_kv;
    float        voltage_min_kv;
    float        voltage_max_kv;
    float        current_ma;
    float        pressure_mtorr;
    float        reserved2[4];

    bool         jpeg_valid;
    uint8_t      reserved3[3];
    uint32_t     jpeg_buff_len;
    uint8_t    * jpeg_buff_ptr;  //XXX combine
    uint64_t     jpeg_buff_offset;

    bool         voltage_history_valid;
    uint8_t      reserved4[3];
    uint32_t     voltage_history_buff_len;
    float      * voltage_history_buff_ptr;  //XXX combine
    uint64_t     voltage_history_buff_offset;
} data_t;

//
// variables
//

static enum mode     mode;
static bool          no_cam;
static bool          no_dataq;
static uint32_t      win_width = DEFAULT_WIN_WIDTH;
static uint32_t      win_height = DEFAULT_WIN_HEIGHT;

static int32_t       fd;
static void        * fd_mmap_addr;

static data_t      * history;
static time_t        history_start_time_sec;
static time_t        history_end_time_sec;
static time_t        cursor_time_sec;

//
// prototypes
//

static void initialize(int32_t argc, char ** argv);
static void usage(void);
static char * bool2str(bool b);
static void display_handler();
static void draw_camera_image(data_t * data, rect_t * cam_pane, texture_t cam_texture);
static void draw_data_values(data_t *data, rect_t * data_pane);
static char * val2str(char * str, float val, char * trailer_str);
static void draw_graph1(rect_t * graph_pane);
static void graph1_scale_select(int32_t event);
static void draw_graph2(rect_t * graph_pane);
static data_t *  get_data(void);
static void free_data(data_t * data);
static bool record_data(data_t * data);
static float convert_adc_voltage(float adc_volts);
static float convert_adc_current(float adc_volts);
static float convert_adc_pressure(float adc_volts, uint32_t gas_id);
static uint32_t gas_get_id(void);
static void gas_select(void);
static char * gas_get_name(uint32_t gas_id);

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
    // -g WxH   : window width and height, default 1920x1080
    // -n cam   : no camera, applies only in live mode
    // -n dataq : no data acquisition, applies only in live mode
    // -v       : version
    // -h       : help
    while (true) {
        char opt_char = getopt(argc, argv, "g:n:hv");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'g':
            if (sscanf(optarg, "%dx%d", &win_width, &win_height) != 2) {
                printf("invalid '-g %s'\n", optarg);
                exit(1);
            }
            break;
        case 'n': // xxx error if not in live mode
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
        time_t t;
        struct tm * tm;
        t = time(NULL);
        tm = localtime(&t);
        sprintf(filename, "fusor_%2.2d%2.2d%2.2d_%2.2d%2.2d%2.2d.dat",
                tm->tm_mon+1, tm->tm_mday, tm->tm_year-100,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        mode = LIVE_MODE;
    } else {
        strcpy(filename, argv[optind]);
        mode = PLAYBACK_MODE;
    }

    // print args
    INFO("starting in %s mode\n",
         (mode == LIVE_MODE ? "LIVE" : "PLAYBACK"));
    INFO("  filename  = %s\n", filename);
    INFO("  WxH       = %dx%d\n", win_width, win_height);
    INFO("  no_cam    = %s\n", bool2str(no_cam));
    INFO("  no_dataq  = %s\n", bool2str(no_dataq));

    // if live mode
    //   initialize data acquisition,
    //   initialize camera
    // endif
    if (mode == LIVE_MODE) {
        if (!no_dataq) {
            dataq_init(0.5, 3, ADC_CHAN_VOLTAGE, ADC_CHAN_CURRENT, ADC_CHAN_PRESSURE);
        }
        if (!no_cam) {
            cam_init(CAM_WIDTH, CAM_HEIGHT);
        }
    }

    // if live mode
    //   create file for saving a recording of this run,
    //   allocate history
    //   init history vars
    // else  (playback mode)
    //   open the file containing the recording to be examined
    //   mmap the file's history array
    //   init history vars
    // endif
    if (mode == LIVE_MODE) {
        fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (fd < 0) {
            FATAL("failed to create %s, %s\n", filename, strerror(errno));
        }

        history = calloc(MAX_HISTORY, sizeof(data_t));
        if (history == NULL) {
            FATAL("failed to allocate history\n");
        }

        history_start_time_sec = get_real_time_us() / 1000000; 
        history_end_time_sec = history_start_time_sec - 1;
        cursor_time_sec = history_end_time_sec;
    } else {
        char    start_time_str[MAX_TIME_STR];
        char    end_time_str[MAX_TIME_STR];
        int32_t i, max_history=0;

        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            FATAL("failed to open %s, %s\n", filename, strerror(errno));
        }

        fd_mmap_addr = mmap(NULL,  // addr
                            MAX_HISTORY * sizeof(data_t),
                            PROT_READ, 
                            MAP_PRIVATE,    
                            fd, 
                            0);   // offset
        if (fd_mmap_addr == MAP_FAILED) {
            FATAL("mmap failed, %s\n", strerror(errno));
        }
        history = fd_mmap_addr;

        for (i = 0; i < MAX_HISTORY; i++) {
            if (history[i].magic != MAGIC && history[i].magic != 0) {
                FATAL("file %s contains bad magic, history[%d].magic = 0x%"PRIx64"\n",
                      filename, i, history[i].magic);
            }
            if (history[i].magic == MAGIC) {
                max_history = i + 1;
            }
        }
        if (max_history == 0) {
            FATAL("file %s contains no history\n", filename);
        }

        history_start_time_sec = history[0].time_us / 1000000;
        history_end_time_sec = history_start_time_sec + max_history - 1;
        cursor_time_sec = history_start_time_sec;

        time2str(start_time_str, history_start_time_sec*(uint64_t)1000000, false, false, true);
        time2str(end_time_str, history_end_time_sec*(uint64_t)1000000, false, false, true);
        INFO("history range is %s to %s, max_history=%d\n", start_time_str, end_time_str, max_history);
    }
}

static void usage(void)
{
    printf("\
NAME\n\
    fusor - display live or recorded camera and analog values from a fusor\n\
\n\
SYNOPSIS\n\
    fusor [OPTIONS] [FILE]\n\
\n\
DESCRIPTION\n\
    If FILE is not supplied then fusor runs in live mode. The camera image is\n\
    is read from the camera and displayed. The analog values are read from the \n\
    dataq device, scaled, and the scaled values are displayed.\n\
\n\
    If FILE is supplied then fusor runs in playback mode. The camera image and\n\
    scaled values are read from the FILE and displayed.\n\
\n\
OPTIONS\n\
    -g WxH   : window width and height, default 1920x1080\n\
\n\
    -n cam   : no camera, applies only in live mode\n\
\n\
    -n dataq : no data acquisition, applies only in live mode\n\
\n\
    -v       : version\n\
\n\
    -h       : help\n\
");
}

static char * bool2str(bool b)
{
    return b ? "true" : "false";
}

// -----------------  DISPLAY HANDLER - MAIN  ----------------------------------------

static void display_handler(void)
{
    bool          quit;
    data_t      * data;
    sdl_event_t * event;
    rect_t        title_pane_full, title_pane; 
    rect_t        cam_pane_full, cam_pane;
    rect_t        data_pane_full, data_pane;
    rect_t        graph_pane_full, graph_pane;
    texture_t     cam_texture;
    char          str[100];
    struct tm   * tm;
    time_t        t;
    bool          data_file_full;
    int32_t       graph_select = 1;

    // this program requires CAM_WIDTH to be >= CAM_HEIGHT; 
    // the reason being that a square texture is created with dimension
    // of CAM_HEIGHT x CAM_HEIGHT, and this texture is updated with the
    // pixels centered around CAM_WIDTH/2
    if (CAM_WIDTH < CAM_HEIGHT) {
        FATAL("CAM_WIDTH must be >= CAM_HEIGHT\n");
    }

    // initializae 
    quit = false;

    sdl_init(win_width, win_height);
    cam_texture = sdl_create_yuy2_texture(CAM_HEIGHT,CAM_HEIGHT);

    sdl_init_pane(&title_pane_full, &title_pane, 
                  0, 0, 
                  win_width, FONT0_HEIGHT+4);
    sdl_init_pane(&cam_pane_full, &cam_pane, 
                  0, FONT0_HEIGHT+2, 
                  CAM_HEIGHT+4, CAM_HEIGHT+4); 
    sdl_init_pane(&data_pane_full, &data_pane, 
                  CAM_HEIGHT+2, FONT0_HEIGHT+2, 
                  win_width-(CAM_HEIGHT+2), CAM_HEIGHT+4); 
    sdl_init_pane(&graph_pane_full, &graph_pane, 
                  0, FONT0_HEIGHT+CAM_HEIGHT+4,
                  win_width, win_height-(FONT0_HEIGHT+CAM_HEIGHT+4));

    // loop until quit
    while (!quit) {
        // get data to be displayed
        data = get_data();

        // record data when in live mode
        if (mode == LIVE_MODE) {
            data_file_full = record_data(data);
            if (data_file_full) {
                INFO("data file is full\n");
                free_data(data);
                quit = true;
                break;
            }
        }

        // initialize for display update
        sdl_display_init();

        // draw pane borders   
        sdl_render_pane_border(&title_pane_full, GREEN);
        sdl_render_pane_border(&cam_pane_full,   GREEN);
        sdl_render_pane_border(&data_pane_full,  GREEN);
        sdl_render_pane_border(&graph_pane_full, GREEN);

        // draw title line
        t = data->time_us / 1000000;
        tm = localtime(&t);
        sprintf(str, "%s MODE - %d/%d/%d %2.2d:%2.2d:%2.2d",
                MODE_STR(mode),
                tm->tm_mon+1, tm->tm_mday, tm->tm_year-100,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        sdl_render_text(&title_pane, 0, 0, 0, str, WHITE, BLACK);
        sdl_render_text(&title_pane, 0, -5, 0, "(ESC)", WHITE, BLACK);
        sdl_render_text(&title_pane, 0, -11, 0, "(?)", WHITE, BLACK);
        
        // draw the camera image,
        // draw the data values,
        // draw the graph
        draw_camera_image(data, &cam_pane, cam_texture);
        draw_data_values(data, &data_pane);
        switch (graph_select) {
        case 1:
            draw_graph1(&graph_pane);
            break;
        case 2:
            draw_graph2(&graph_pane);
            break;
        }

        // register for events   
        // - quit and help
        sdl_event_register(SDL_EVENT_KEY_ESC, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('?', SDL_EVENT_TYPE_KEY, NULL);
        // - gas select
        if (mode == LIVE_MODE) {
            sdl_event_register('g', SDL_EVENT_TYPE_KEY, NULL);
        }
        // - graph select
        sdl_event_register('s', SDL_EVENT_TYPE_KEY, NULL);
        // - graph 1 zoom
        if (graph_select == 1) {
            sdl_event_register('+', SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register('-', SDL_EVENT_TYPE_KEY, NULL);
        }
        // - graph 1 & 2 cursor time
        if (mode == PLAYBACK_MODE) {
            sdl_event_register(SDL_EVENT_KEY_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_CTRL_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_CTRL_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_ALT_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_ALT_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_HOME, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_END, SDL_EVENT_TYPE_KEY, NULL);
        }

        // present the display
        sdl_display_present();

        // process events
        do {
            event = sdl_poll_event();
            switch (event->event) {
            case SDL_EVENT_QUIT: case SDL_EVENT_KEY_ESC: 
                ASSERT_IN_ANY_MODE();
                quit = true;
                break;
            case '?':  
                ASSERT_IN_ANY_MODE();
                sdl_display_text(about);
                break;
            case 'g':
                ASSERT_IN_LIVE_MODE();
                gas_select();
                break;
            case 's':
                ASSERT_IN_ANY_MODE();
                graph_select++;
                if (graph_select > 2) {
                    graph_select = 1;
                }
                break;
            case '-': case '+':
                ASSERT_IN_ANY_MODE();
                graph1_scale_select(event->event);
                break;
            case SDL_EVENT_KEY_LEFT_ARROW:
            case SDL_EVENT_KEY_CTRL_LEFT_ARROW:
            case SDL_EVENT_KEY_ALT_LEFT_ARROW:
                ASSERT_IN_PLAYBACK_MODE();
                cursor_time_sec -= (event->event == SDL_EVENT_KEY_LEFT_ARROW      ? 1 :
                                    event->event == SDL_EVENT_KEY_CTRL_LEFT_ARROW ? 10 
                                                                                  : 60);
                if (cursor_time_sec < history_start_time_sec) {
                    cursor_time_sec = history_start_time_sec;
                }
                break;
            case SDL_EVENT_KEY_RIGHT_ARROW:
            case SDL_EVENT_KEY_CTRL_RIGHT_ARROW:
            case SDL_EVENT_KEY_ALT_RIGHT_ARROW:
                ASSERT_IN_PLAYBACK_MODE();
                cursor_time_sec += (event->event == SDL_EVENT_KEY_RIGHT_ARROW      ? 1 :
                                    event->event == SDL_EVENT_KEY_CTRL_RIGHT_ARROW ? 10 
                                                                                   : 60);
                if (cursor_time_sec > history_end_time_sec) {
                    cursor_time_sec = history_end_time_sec;
                }
                break;
            case SDL_EVENT_KEY_HOME:
                ASSERT_IN_PLAYBACK_MODE();
                cursor_time_sec = history_start_time_sec;
                break;
            case SDL_EVENT_KEY_END:
                ASSERT_IN_PLAYBACK_MODE();
                cursor_time_sec = history_end_time_sec;
                break;
            default:
                break;
            }
        } while (event->event != SDL_EVENT_NONE && !quit);

        // free the data
        free_data(data);
    }

    INFO("terminating\n");
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW CAMERA IMAGE  - - - - - - - - - - - - - - 

static void draw_camera_image(data_t * data, rect_t * cam_pane, texture_t cam_texture)
{
    uint8_t * pixel_buff;
    uint32_t  pixel_buff_width;
    uint32_t  pixel_buff_height;
    int32_t   ret;

    if (!data->jpeg_valid) {
        return;
    }

    ret = jpeg_decode(0,  // cxid
                     JPEG_DECODE_MODE_YUY2,      
                     data->jpeg_buff_ptr, data->jpeg_buff_len,
                     &pixel_buff, &pixel_buff_width, &pixel_buff_height);
    if (ret < 0) {
        FATAL("jpeg_decode failed\n");
    }
    if (pixel_buff_width != CAM_WIDTH || pixel_buff_height != CAM_HEIGHT) {
        FATAL("jpeg_decode wrong dimensions w=%d h=%d\n",
            pixel_buff_width, pixel_buff_height);
    }

    sdl_update_yuy2_texture(cam_texture, 
                             pixel_buff + (CAM_WIDTH - CAM_HEIGHT) / 2,
                             CAM_WIDTH);
    sdl_render_texture(cam_texture, cam_pane);

    free(pixel_buff);
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW DATA VALUES  - - - - - - - - - - - - - - 

static void draw_data_values(data_t *data, rect_t * data_pane)
{
    char str[100];
    char trailer_str[100];

    if (!data->data_valid) {
        return;
    }
        
    val2str(str, data->voltage_rms_kv, "kV rms");
    sdl_render_text(data_pane, 0, 0, 1, str, WHITE, BLACK);

    val2str(str, data->voltage_min_kv, "kV min");
    sdl_render_text(data_pane, 1, 0, 1, str, WHITE, BLACK);

    val2str(str, data->voltage_max_kv, "kV max");
    sdl_render_text(data_pane, 2, 0, 1, str, WHITE, BLACK);

    val2str(str, data->current_ma, "mA");
    sdl_render_text(data_pane, 3, 0, 1, str, WHITE, BLACK);

    if (data->pressure_mtorr < 1000 || IS_ERROR(data->pressure_mtorr)) {
        sprintf(trailer_str, "mTorr %s %s", 
                gas_get_name(data->gas_id),
                mode == LIVE_MODE ? "(g)" : "");
        val2str(str, data->pressure_mtorr, trailer_str);
    } else {
        sprintf(trailer_str, "Torr %s %s", 
                gas_get_name(data->gas_id),
                mode == LIVE_MODE ? "(g)" : "");
        val2str(str, data->pressure_mtorr/1000, trailer_str);
    }
    sdl_render_text(data_pane, 4, 0, 1, str, WHITE, BLACK);
}

static char * val2str(char * str, float val, char * trailer_str)
{
    if (IS_ERROR(val)) {
        sprintf(str, "%-6s %s", ERROR_TEXT(val), trailer_str);
    } else if (val < 1000.0) {
        sprintf(str, "%-6.2f %s", val, trailer_str);
    } else {
        sprintf(str, "%-6.0f %s", val, trailer_str);
    }
    return str;
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW GRAPH 1  - - - - - - - - - - - - - - - 

#define MAX_GRAPH1_SCALE (sizeof(graph1_scale)/sizeof(graph1_scale[0]))

typedef struct {
    int32_t span;   // must be multiple of 60
    int32_t delta;
} graph1_scale_t;

static int32_t       graph1_scale_idx;
static graph1_scale_t graph1_scale[] = {
                            { 60,      1 },    // 1 minute
                            { 600,     2 },    // 10 minutes
                            { 3600,   12 },    // 1 hour
                            { 36000, 120 },    // 10 hours
                                                };


static void draw_graph1(rect_t * graph_pane)
{
    #define MAX_GRAPH1_CONFIG (sizeof(graph1_config)/sizeof(graph1_config[0]))
    static struct graph1_config {
        char   * name;
        float    max_value;
        int32_t  color;
        off_t    field_offset;
    } graph1_config[] = {
        { "kV    : 30 MAX", 30.0, RED, offsetof(data_t, voltage_rms_kv) },
        { "mA    : 30 MAX", 30.0, GREEN, offsetof(data_t, current_ma) },
        { "mTorr : 30 MAX", 30.0, BLUE, offsetof(data_t, pressure_mtorr) },
                            };

    time_t  graph1_start_time_sec, graph1_end_time_sec;
    int32_t X_origin, X_pixels, Y_origin, Y_pixels, T_delta, T_span;
    float   X_pixels_per_sec;
    int32_t i;

    // sanitize scale_idx
    while (graph1_scale_idx < 0) {
        graph1_scale_idx += MAX_GRAPH1_SCALE;
    }
    while (graph1_scale_idx >= MAX_GRAPH1_SCALE) {
        graph1_scale_idx -= MAX_GRAPH1_SCALE;
    }

    // init
    T_span = graph1_scale[graph1_scale_idx].span;
    T_delta = graph1_scale[graph1_scale_idx].delta;
    X_origin = 10;
    X_pixels = 1200;
    X_pixels_per_sec = (float)X_pixels / T_span;
    Y_origin = graph_pane->h - FONT0_HEIGHT - 4;
    Y_pixels = graph_pane->h - FONT0_HEIGHT - 10;

    // determine graph1_start_sec and graph1_end_sec
    if (mode == LIVE_MODE) {
        graph1_end_time_sec = cursor_time_sec;
        graph1_start_time_sec = graph1_end_time_sec - (T_span - 1);
    } else {
        graph1_start_time_sec = cursor_time_sec - T_span / 2;
        graph1_end_time_sec = graph1_start_time_sec + T_span - 1;
    }

    // fill white
    rect_t rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = graph_pane->w;
    rect.h = graph_pane->h;
    sdl_render_fill_rect(graph_pane, &rect, WHITE);

    // draw the graph1s
    for (i = 0; i < MAX_GRAPH1_CONFIG; i++) {
        #define MAX_POINTS 1000
        struct graph1_config * gc;
        int32_t max_points, idx;
        point_t points[MAX_POINTS];
        time_t  t;
        float   X, X_delta, Y_scale;

        gc = &graph1_config[i];
        X = X_origin + X_pixels - 1;
        X_delta = X_pixels_per_sec * T_delta;
        Y_scale  = Y_pixels / gc->max_value;
        max_points = 0;

        for (t = graph1_end_time_sec; t >= graph1_start_time_sec; t -= T_delta) {
            float value;
            idx = t - history_start_time_sec;
            if (idx >= 0 && 
                idx < MAX_HISTORY && 
                history[idx].data_valid &&
                ((value = *(float*)((void*)&history[idx] + gc->field_offset)), !IS_ERROR(value)))
            {
                if (value < 0) {
                    value = 0;
                } else if (value > gc->max_value) {
                    value = gc->max_value;
                }
                points[max_points].x = X;
                points[max_points].y = Y_origin - value * Y_scale;
                max_points++;
                if (max_points == MAX_POINTS) {
                    sdl_render_lines(graph_pane, points, max_points, gc->color);
                    points[0].x = X;
                    points[0].y = Y_origin - value * Y_scale;
                    max_points = 1;
                }
            } else {
                sdl_render_lines(graph_pane, points, max_points, gc->color);
                max_points = 0;
            }
            X -= X_delta;
        }
        sdl_render_lines(graph_pane, points, max_points, gc->color);
    }

    // draw x axis
    sdl_render_line(graph_pane, 
                    X_origin-1, Y_origin+1, 
                    X_origin+X_pixels, Y_origin+1,
                    BLACK);
    sdl_render_line(graph_pane, 
                    X_origin-2, Y_origin+2, 
                    X_origin+X_pixels, Y_origin+2,
                    BLACK);
    sdl_render_line(graph_pane, 
                    X_origin-3, Y_origin+3, 
                    X_origin+X_pixels, Y_origin+3,
                    BLACK);

    // draw y axis
    sdl_render_line(graph_pane, 
                    X_origin-1, Y_origin+1, 
                    X_origin-1, Y_origin-Y_pixels,
                    BLACK);
    sdl_render_line(graph_pane, 
                    X_origin-2, Y_origin+2, 
                    X_origin-2, Y_origin-Y_pixels,
                    BLACK);
    sdl_render_line(graph_pane, 
                    X_origin-3, Y_origin+3, 
                    X_origin-3, Y_origin-Y_pixels,
                    BLACK);

    // draw cursor
    int32_t X_cursor = (X_origin + X_pixels - 1) -
                       (graph1_end_time_sec - cursor_time_sec) * X_pixels_per_sec;
    sdl_render_line(graph_pane, 
                    X_cursor, Y_origin,
                    X_cursor, Y_origin-Y_pixels,
                    PURPLE);
    
    // draw cursor time
    int32_t str_col;
    char    str[100];
    time2str(str, (uint64_t)cursor_time_sec*1000000, false, false, false);
    str_col = X_cursor/FONT0_WIDTH - 4;
    if (str_col < 0) {
        str_col = 0;
    }
    sdl_render_text(graph_pane, 
                    -1, str_col,
                    0, str, PURPLE, WHITE);

    // draw x axis span time
    if (T_span/60 < 60) {
        sprintf(str, "%d MINUTES (+/-)", T_span/60);
    } else {
        sprintf(str, "%d HOURS (+/-)", T_span/3600);
    }
    str_col = (X_pixels + X_origin) / FONT0_WIDTH + 6;
    sdl_render_text(graph_pane, 
                    -1, str_col,
                    0, str, BLACK, WHITE);

    // draw playback mode controls
    if (mode == PLAYBACK_MODE) {
        sdl_render_text(graph_pane, 
                        -2, str_col,
                        0, "CURSOR (</>/CTRL/ALT)", BLACK, WHITE);
    }

    // draw graph1 names, and current values
    for (i = 0; i < MAX_GRAPH1_CONFIG; i++) {
        char str[100];
        float value;
        int32_t idx;
        struct graph1_config * gc = &graph1_config[i];

        idx = cursor_time_sec - history_start_time_sec;
        if (idx < 0 || idx >= MAX_HISTORY) {
            FATAL("idx %d out of range\n", idx);
        }
        value = *(float*)((void*)&history[idx] + gc->field_offset);
        val2str(str, value, graph1_config[i].name);

        sdl_render_text(graph_pane, 
                        i, str_col,
                        0, str, graph1_config[i].color, WHITE);
    }
}

static void graph1_scale_select(int32_t event)
{
    if (event != '+' && event != '-') {
        FATAL("event must be '+' or '-'\n");
    }

    if (event == '+') {
        if (graph1_scale_idx > 0) {
            graph1_scale_idx--;
        }
    } else {
        if (graph1_scale_idx < MAX_GRAPH1_SCALE-1) {
            graph1_scale_idx++;
        }
    }
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW GRAPH 2  - - - - - - - - - - - - - - - 

static void draw_graph2(rect_t * graph_pane)
{
    //XXX add graph2
}

// -----------------  GET AND FREE DATA  ---------------------------------------------

static data_t * get_data(void)
{
    data_t * data;

    // allocate data buffer
    data = calloc(1, sizeof(data_t));
    if (data == NULL) {
        FATAL("alloc data failed\n");
    }

    // processing for either LIVE_MODE or PLAYBACK_MODE
    if (mode == LIVE_MODE) {
        // LIVE_MODE ...

        // init data magic
        data->magic = MAGIC;

        // get current time
        data->time_us = get_real_time_us();

        // get camera data
        // LATER may need a status return from cam_get_buff
        if (!no_cam) {
            cam_get_buff(&data->jpeg_buff_ptr, &data->jpeg_buff_len);
            data->jpeg_valid = true;
        }

        // get adc data from the dataq device, and 
        // convert to the values which will be displayed
        if (!no_dataq) do {
            int32_t rms_mv, mean_mv, min_mv, max_mv;
            int32_t * history_mv;
            int32_t ret, max_history_mv;

            // read ADC_CHAN_VOLTAGE and convert to kV
            ret = dataq_get_adc(ADC_CHAN_VOLTAGE, &rms_mv, NULL, NULL, &min_mv, &max_mv,
                                0, NULL, NULL);
            if (ret != 0) {
                break;
            }
            data->voltage_rms_kv = convert_adc_voltage(rms_mv/1000.);
            data->voltage_min_kv = convert_adc_voltage(min_mv/1000.);
            data->voltage_max_kv = convert_adc_voltage(max_mv/1000.);

            // read ADC_CHAN_CURRENT and convert to mA
            ret = dataq_get_adc(ADC_CHAN_CURRENT, NULL, &mean_mv, NULL, NULL, NULL,
                                0, NULL, NULL);
            if (ret != 0) {
                break;
            }
            data->current_ma = convert_adc_current(mean_mv/1000.);

            // read ADC_CHAN_PRESSURE and convert to mTorr
            ret = dataq_get_adc(ADC_CHAN_PRESSURE, NULL, &mean_mv, NULL, NULL, NULL,
                                0, NULL, NULL);
            if (ret != 0) {
                break;
            }
            mean_mv = 3386;  // XXX temp set value for testing
            data->gas_id = gas_get_id();
            data->pressure_mtorr = convert_adc_pressure(mean_mv/1000., data->gas_id);

            // set data_valid flag
            data->data_valid = true;

            // read ADC_CHAN_VOLTAGE XXX comment
            ret = dataq_get_adc(ADC_CHAN_VOLTAGE, &rms_mv, NULL, NULL, &min_mv, &max_mv,
                                0.25, &history_mv, &max_history_mv);
            if (ret != 0) {
                break;
            }

// XXX free and realloc
            float * history_kv = (float*)history_mv;

            int32_t i;
            //printf("max_history_mv %d\n", max_history_mv);
            for (i = 0; i < max_history_mv; i++) {
                history_kv[i] = convert_adc_voltage(history_mv[i]/1000.);
            }

            // XXX
            // data->voltage_history_buff_len  
            data->voltage_history_buff_ptr = history_kv;
            // data->voltage_history_buff_offset;
            data->voltage_history_valid = true;
        } while (0);
    } else {
        int32_t len, idx;

        // PLAYBACK_MODE ...

        // copy the data from the history array, at cursor_time_sec
        idx = cursor_time_sec - history_start_time_sec;
        if (idx < 0 || idx >= MAX_HISTORY) {
            FATAL("invalid history idx = %d\n", idx);
        }
        *data = history[idx];

        // malloc and read the jpeg
        if (data->jpeg_valid) {
            data->jpeg_buff_ptr = malloc(data->jpeg_buff_len);
            if (data->jpeg_buff_ptr == NULL){
                FATAL("failed malloc jpeg buff, len=%d\n", data->jpeg_buff_len);
            }
            len = pread(fd, data->jpeg_buff_ptr, data->jpeg_buff_len, data->jpeg_buff_offset);
            if (len != data->jpeg_buff_len) {
                FATAL("read jpeg buff, len=%d, %s\n", len, strerror(errno));
            }
        }

        // clear data->jpeg_buff_offset, this is not used by caller
        data->jpeg_buff_offset = 0;
    }

    // return data
    return data;
}

static void free_data(data_t * data) 
{
    if (mode == LIVE_MODE) {
        if (data->jpeg_valid) {
            cam_put_buff(data->jpeg_buff_ptr);
        }
    } else {
        free(data->jpeg_buff_ptr);
        free(data->voltage_history_buff_ptr);
    }

    free(data);
}

// -----------------  RECORD DATA  ---------------------------------------------------

static bool record_data(data_t * data)
{
    static off_t jpeg_buff_offset = MAX_HISTORY * sizeof(data_t);

    // if the caller supplied data's time is not beyond the end of saved history then return
    time_t t = data->time_us / 1000000;
    if (t <= history_end_time_sec) {
        return false;
    }

    // save the data in history
    // - determine the index into history buffer, indexed by seconds
    // - copy the data to the history buffer
    // - update the history_end_time_sec
    // - update the graph cursor_time
    int32_t idx = t - history_start_time_sec;
    DEBUG("adding history at idx %d\n", idx);
    if (idx >= MAX_HISTORY) {
        return true;  // data file is full
    }
    if (idx < 0) {
        FATAL("invalid history idx = %d\n", idx);
    }
    history[idx] = *data;
    history_end_time_sec = t;
    cursor_time_sec = history_end_time_sec; 

    // write the new history entry to the file
    int32_t len;
    data_t data2 = history[idx];
    if (data2.jpeg_valid) {
        data2.jpeg_buff_offset = jpeg_buff_offset;
        data2.jpeg_buff_ptr = NULL;
        jpeg_buff_offset += data2.jpeg_buff_len;

        len = pwrite(fd, history[idx].jpeg_buff_ptr, data2.jpeg_buff_len, data2.jpeg_buff_offset);
        if (len != data2.jpeg_buff_len) {
            FATAL("failed write jpeg to file, ret_len=%d, exp_len=%d, %s\n", 
                  len, data2.jpeg_buff_len, strerror(errno));
        }
    } else {
        data2.jpeg_buff_offset = 0;
        data2.jpeg_buff_ptr = NULL;
    }

    len = pwrite(fd, &data2, sizeof(data2), idx * sizeof(data2));
    if (len != sizeof(data2)) {
        FATAL("failed write data2 to file, ret_len=%d, exp_len=%zd, %s\n", 
              len, sizeof(data2), strerror(errno));
    }

//XXX write voltage

    // return, data file is not full
    return false;
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

// -----------------  CONVERT ADC PRESSURE GAUGE  ------------------------------------

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

static uint32_t gas_live_mode_id;

static uint32_t gas_get_id(void)
{
    ASSERT_IN_LIVE_MODE();
    return gas_live_mode_id;
}

static void gas_select(void) 
{
    ASSERT_IN_LIVE_MODE();
    gas_live_mode_id = (gas_live_mode_id + 1) % MAX_GAS_TBL;
}

static char * gas_get_name(uint32_t gas_id)
{
    return gas_tbl[gas_id].name;
}

