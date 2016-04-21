// 04/20/16 03:44:48.414 ERROR jpeg_decode_output_message_override: Premature end of JPEG file
// 04/20/16 03:44:48.466 ERROR jpeg_decode_output_message_override: Premature end of JPEG file
// 04/20/16 04:54:52.639 FATAL cam_put_buff: ioctl VIDIOC_QBUF index=24 Invalid argument

// 04/19/16 08:14:48.530 ERROR jpeg_decode_output_message_override: Premature end of JPEG file
// 04/19/16 08:14:48.592 ERROR jpeg_decode_output_message_override: Premature end of JPEG file
// 04/19/16 08:14:48.621 FATAL cam_put_buff: ioctl VIDIOC_QBUF index=9 Invalid argument

// XXX make static vars and procs
// XXX is data_t this same size on fedora and rpi
// XXX measure the timing and cpu utilization

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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "util_sdl.h"
#include "util_jpeg_decode.h"
#include "util_dataq.h"
#include "util_cam.h"
#include "util_misc.h"

#define VERSION_STR  "1.0"
#include "about.h"

//
// defines
//

#define WIN_WIDTH   1920
#define WIN_HEIGHT  1080

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

#define MAX_GRAPH_SCALE (sizeof(graph_scale)/sizeof(graph_scale[0]))

//
// typedefs
//

enum mode {LIVE_MODE, PLAYBACK_MODE};

typedef struct {
    uint64_t     magic;
    uint64_t     time_us;

    bool         data_valid;
    float        voltage_rms_kv;
    float        voltage_min_kv;
    float        voltage_max_kv;
    float        current_ma;
    float        pressure_mtorr;

    bool         jpeg_valid;
    uint32_t     jpeg_buff_len;
    uint8_t    * jpeg_buff_ptr;
    off_t        jpeg_buff_offset;
} data_t;

typedef struct {
    int32_t span;   // must be multiple of 60
    int32_t delta;
} graph_scale_t;

//
// variables
//

enum mode     mode;
bool          no_cam;
bool          no_dataq;

int32_t       fd;
void        * fd_mmap_addr;

data_t      * history;
time_t        history_start_time_sec;  //xxx review
time_t        history_end_time_sec;    //xxx review
time_t        cursor_time_sec;         //xxx review

int32_t       graph_scale_idx;
graph_scale_t graph_scale[] = {
                    { 60,      1 },    // 1 minute
                    { 600,     2 },    // 10 minutes
                    { 3600,   12 },    // 1 hour
                    { 36000, 120 },    // 10 hours
                                        };

//
// prototypes
//

void initialize(int32_t argc, char ** argv);
void usage(void);
char * bool2str(bool b);
void display_handler();
void draw_camera_image(data_t * data, rect_t * cam_pane, texture_t cam_texture);
void draw_data_values(data_t *data, rect_t * data_pane);
void draw_graph(rect_t * graph_pane);
data_t *  get_data(void);
void free_data(data_t * data);

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

    // XXX check is same size on fedora 64bit
    printf("XXX sizeof datat %d\n", sizeof(data_t));  // XXX = 96

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
        cursor_time_sec = history_end_time_sec;  // XXX realyy -1?
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
    rect_t        data_pane_full, data_pane;
    rect_t        graph_pane_full, graph_pane;
    texture_t     cam_texture;
    char          str[100];
    struct tm   * tm;
    time_t        t;

    // xxx verify cam width >= height, and explain

    // initializae 
    done = false;

    sdl_init(WIN_WIDTH, WIN_HEIGHT);
    cam_texture = sdl_create_yuy2_texture(CAM_HEIGHT,CAM_HEIGHT);

    sdl_init_pane(&title_pane_full, &title_pane, 
                  0, 0, 
                  WIN_WIDTH, FONT0_HEIGHT+4);
    sdl_init_pane(&cam_pane_full, &cam_pane, 
                  0, FONT0_HEIGHT+2, 
                  CAM_HEIGHT+4, CAM_HEIGHT+4); 
    sdl_init_pane(&data_pane_full, &data_pane, 
                  CAM_HEIGHT+2, FONT0_HEIGHT+2, 
                  WIN_WIDTH-(CAM_HEIGHT+2), CAM_HEIGHT+4); 
    sdl_init_pane(&graph_pane_full, &graph_pane, 
                  0, FONT0_HEIGHT+CAM_HEIGHT+4,
                  WIN_WIDTH, WIN_HEIGHT-(FONT0_HEIGHT+CAM_HEIGHT+4));

    // loop until done
    while (!done) {
        // get data to be displayed
        data = get_data();

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
        draw_graph(&graph_pane);

        // register for events
        sdl_event_register(SDL_EVENT_KEY_ESC, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('?', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('+', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('=', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('-', SDL_EVENT_TYPE_KEY, NULL);

        // present the display
        sdl_display_present();

        // process events
        event = sdl_poll_event();
        switch (event->event) {
        case SDL_EVENT_QUIT: 
        case SDL_EVENT_KEY_ESC: 
            done = true;
            break;
        case '?':  
            sdl_display_text(about);
            break;
        case '+':
        case '=':
            printf("XXX graph plus\n");
            if (graph_scale_idx < MAX_GRAPH_SCALE-1) {
                graph_scale_idx++;
            }
            break;
        case '-':
            printf("XXX graph minus\n");
            if (graph_scale_idx > 0) {
                graph_scale_idx--;
            }
            break;
        default:
            break;
        }

        // free the data
        free_data(data);
    }

    INFO("terminating\n");
}

void draw_camera_image(data_t * data, rect_t * cam_pane, texture_t cam_texture)
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

void draw_data_values(data_t *data, rect_t * data_pane)
{
    char str[100];

    if (!data->data_valid) {
        return;
    }
        
    sprintf(str, "%4.1f kV rms", data->voltage_rms_kv);
    sdl_render_text(data_pane, 0, 0, 1, str, WHITE, BLACK);
    sprintf(str, "%4.1f kV min", data->voltage_min_kv);
    sdl_render_text(data_pane, 1, 0, 1, str, WHITE, BLACK);
    sprintf(str, "%4.1f kV max", data->voltage_max_kv);
    sdl_render_text(data_pane, 2, 0, 1, str, WHITE, BLACK);
    sprintf(str, "%4.1f mA", data->current_ma);
    sdl_render_text(data_pane, 3, 0, 1, str, WHITE, BLACK);
    sprintf(str, "%4.1f mTorr", data->pressure_mtorr);
    sdl_render_text(data_pane, 4, 0, 1, str, WHITE, BLACK);
}

void draw_graph(rect_t * graph_pane)
{
    #define MAX_GRAPH_CONFIG (sizeof(graph_config)/sizeof(graph_config[0]))
    static struct graph_config {
        char   * name;
        float    max_value;
        int32_t  color;
        off_t    field_offset;
    } graph_config[] = {
        { "kV : 30 MAX", 30.0, RED, offsetof(data_t, voltage_rms_kv) },
                            };

    time_t  graph_start_time_sec, graph_end_time_sec;
    int32_t X_origin, X_pixels, Y_origin, Y_pixels, T_delta, T_span;
    float   X_pixels_per_sec;
    int32_t i;

    // sanitize scale_idx
    while (graph_scale_idx < 0) {
        graph_scale_idx += MAX_GRAPH_SCALE;
    }
    while (graph_scale_idx >= MAX_GRAPH_SCALE) {
        graph_scale_idx -= MAX_GRAPH_SCALE;
    }

    // init
    T_span = graph_scale[graph_scale_idx].span;
    T_delta = graph_scale[graph_scale_idx].delta;
    X_origin = 10;
    X_pixels = 1200;
    X_pixels_per_sec = (float)X_pixels / T_span;
    Y_origin = graph_pane->h - FONT0_HEIGHT - 4;
    Y_pixels = graph_pane->h - FONT0_HEIGHT - 10;

    // determine graph_start_sec and graph_end_sec
    if (mode == LIVE_MODE) {
        graph_start_time_sec = history_start_time_sec;
        graph_end_time_sec = graph_start_time_sec + (T_span - 1);
        if (cursor_time_sec > graph_end_time_sec) {
            graph_end_time_sec = cursor_time_sec;
            graph_start_time_sec = graph_end_time_sec - (T_span - 1);
        }
    } else {
        // XXX tbd
        graph_start_time_sec = history_start_time_sec;
        graph_end_time_sec = graph_start_time_sec + T_span - 1;
    }

    // fill white
    rect_t rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = graph_pane->w;
    rect.h = graph_pane->h;
    sdl_render_fill_rect(graph_pane, &rect, WHITE);

    // draw the graphs
    for (i = 0; i < MAX_GRAPH_CONFIG; i++) {
        struct graph_config * gc;
        int32_t max_points, idx;
        point_t points[1000];
        time_t  t;
        float   X, X_delta, Y_scale;

        gc = &graph_config[i];
        X = X_origin + X_pixels - 1;
        X_delta = X_pixels_per_sec * T_delta;
        Y_scale  = Y_pixels / gc->max_value;
        max_points = 0;

        for (t = graph_end_time_sec; t >= graph_start_time_sec; t -= T_delta) {
            idx = t - history_start_time_sec;
            if ((idx >= 0 && idx < MAX_HISTORY) && history[idx].data_valid) {
                float value  = *(float*)((void*)&history[idx] + gc->field_offset);
                if (value < 0) {
                    value = 0;
                } else if (value > gc->max_value) {
                    value = gc->max_value;
                }
                if (max_points >= (sizeof(points)/sizeof(points[0]))) {
                    FATAL("max_points=%d is too big\n", max_points);
                }
                points[max_points].x = X;
                points[max_points].y = Y_origin - value * Y_scale;
                max_points++;
                // XXX if full then reset points
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
                       (graph_end_time_sec - cursor_time_sec) * X_pixels_per_sec;
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
    str_col = (X_pixels + X_origin) / FONT0_WIDTH + 9;
    sdl_render_text(graph_pane, 
                    -1, str_col,
                    0, str, BLACK, WHITE);

    // draw graph names 
    for (i = 0; i < MAX_GRAPH_CONFIG; i++) {
        sdl_render_text(graph_pane, 
                        i, str_col,
                        0, graph_config[i].name, graph_config[i].color, WHITE);
    }
}

// -----------------  GET AND FREE DATA  ---------------------------------------------

data_t * get_data(void)
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
        if (!no_cam) {
            cam_get_buff(&data->jpeg_buff_ptr, &data->jpeg_buff_len);
            data->jpeg_valid = true;
        }

        // get adc data from the dataq device, and convert to voltage, current, and pressure
        if (!no_dataq) do {
            float rms, mean, min, max;
            int32_t ret1, ret2, ret3;

            ret1 = dataq_get_adc(ADC_CHAN_VOLTAGE, &rms, NULL, NULL, &min, &max);
            ret2 = dataq_get_adc(ADC_CHAN_CURRENT, NULL, &mean, NULL, NULL, NULL);
            ret3 = dataq_get_adc(ADC_CHAN_PRESSURE, NULL, &mean, NULL, NULL, NULL);
            data->data_valid = (ret1 == 0 && ret2 == 0 && ret3 == 0);

            if (!data->data_valid) {
                break;
            }

            data->voltage_rms_kv = (float)(time(NULL) - history_start_time_sec) / 10;  // XXX tbd

            data->voltage_min_kv = 10;
            data->voltage_max_kv = 10;
            data->current_ma = 10;
            data->pressure_mtorr = 10;
        } while (0);

        // if data time is 1 second beyond the end of current saved history then
        //   store data in history, and
        //   write to file
        // endif
        time_t t = data->time_us / 1000000;
        if (t > history_end_time_sec) {
            data_t data2;
            int32_t len;
            static off_t jpeg_buff_offset = MAX_HISTORY * sizeof(data_t);

            // determine the index into history buffer, indexed by seconds
            int32_t idx = t - history_start_time_sec;
            if (idx < 0 || idx >= MAX_HISTORY) {
                FATAL("invalid history idx = %d\n", idx);
            }
            //printf("XXX ADDING HISTORY %d\n", idx);

            // save the data in history, and
            // update the graph cursor_time
            history_end_time_sec = t;
            history[idx] = *data;
            cursor_time_sec = history_end_time_sec;

            // write to history file
            data2 = history[idx];
            if (data2.jpeg_valid) {
                data2.jpeg_buff_offset = jpeg_buff_offset;
                data2.jpeg_buff_ptr = NULL;
                jpeg_buff_offset += data2.jpeg_buff_len;

                len = pwrite(fd, history[idx].jpeg_buff_ptr, data2.jpeg_buff_len, data2.jpeg_buff_offset);
                // printf("XXX write %d\n", len);
                if (len !=  data2.jpeg_buff_len) {
                    FATAL("failed write jpeg to file, len=%d, %s\n", len, strerror(errno));
                }
            } else {
                data2.jpeg_buff_offset = 0;
                data2.jpeg_buff_ptr = NULL;
            }

            len = pwrite(fd, &data2, sizeof(data2), idx * sizeof(data2));
            // printf("XXX write %d\n", len);
            if (len != sizeof(data2)) {
                FATAL("failed write data2 to file, len=%d, %s\n", len, strerror(errno));
            }
        }
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

void free_data(data_t * data) 
{
    if (mode == LIVE_MODE) {
        if (data->jpeg_valid) {
            cam_put_buff(data->jpeg_buff_ptr);
        }
    } else {
        if (data->jpeg_valid) {
            free(data->jpeg_buff_ptr);
        }
    }

    free(data);
}
