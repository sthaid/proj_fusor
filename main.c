// XXX retest the predefined displays
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

//
// defines
//

#define VERSION_STR  "1.0"

#define WIN_WIDTH   1920
#define WIN_HEIGHT  1080

#define CAM_WIDTH   960
#define CAM_HEIGHT  720

#define MAX_ADC_CHAN       8
#define MAX_ADC_CHAN_LIST  3
#define ADC_CHAN_VOLTAGE   1
#define ADC_CHAN_CURRENT   2
#define ADC_CHAN_PRESSURE  3

#define MAX_HISTORY  100000

#define FONT0_HEIGHT (sdl_font_char_height(0))

#define MODE_STR(m) ((m) == LIVE_MODE ? "LIVE" : "PLAYBACK")

#define MAGIC 0x1122334455667788

//
// typedefs
//

enum mode {LIVE_MODE, PLAYBACK_MODE};

typedef struct {
    uint64_t     magic;
    uint64_t     time_us;

    bool         adc_valid;
    adc_value_t  adc_value[MAX_ADC_CHAN];

    bool         jpeg_valid;
    uint32_t     jpeg_buff_len;
    uint8_t    * jpeg_buff_ptr;
    off_t        jpeg_buff_offset;
} data_t;

//
// variables
//

enum mode  mode;
bool       no_cam;
bool       no_dataq;

int32_t    fd;
void     * fd_mmap_addr;

data_t    * history;
int32_t     max_history;  // XXX is this needed, and verify it is correctly updated
time_t      history_start_time_sec;  //XXX review
time_t      history_end_time_sec;  //XXX review

int64_t     cursor_time_sec;  //XXX review

int32_t     adc_chan_list[MAX_ADC_CHAN_LIST] = { ADC_CHAN_VOLTAGE, ADC_CHAN_CURRENT, ADC_CHAN_PRESSURE };


//
// prototypes
//

void initialize(int32_t argc, char ** argv);
void usage(void);
char * bool2str(bool b);
void display_handler();
void draw_camera_image(data_t * data, rect_t * cam_pane, texture_t cam_texture);
void draw_adc_values(data_t *data, rect_t * data_pane);
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
            dataq_init(0.5, adc_chan_list, MAX_ADC_CHAN_LIST);
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

        history_start_time_sec = get_real_time_us() / 1000000;   // XXX init these onfirst
        history_end_time_sec = history_start_time_sec - 1;
        cursor_time_sec = history_end_time_sec;
        max_history = 0;  // XXX is this used
    } else {
        char    start_time_str[MAX_TIME_STR];
        char    end_time_str[MAX_TIME_STR];
        int32_t i;

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

        time2str(start_time_str, history_start_time_sec*(uint64_t)1000000, false, false);
        time2str(end_time_str, history_end_time_sec*(uint64_t)1000000, false, false);
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
    char          title_str[100];
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
        sprintf(title_str, "%s MODE - %d/%d/%d %2.2d:%2.2d:%2.2d",
                MODE_STR(mode),
                tm->tm_mon+1, tm->tm_mday, tm->tm_year-100,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        sdl_render_text(&title_pane, 0, 0, 0, title_str, WHITE, BLACK);
        
        // draw the camera image,
        // draw the data values,
        // draw the graph
        draw_camera_image(data, &cam_pane, cam_texture);
        draw_adc_values(data, &data_pane);
        draw_graph(&graph_pane);

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

void draw_adc_values(data_t *data, rect_t * data_pane)
{
    float voltage_rms_kv;
    float voltage_min_kv;
    float voltage_max_kv;
    float current_ma;
    float pressure_mtorr;
    char  str[100];

    if (!data->adc_valid) {
        return;
    }
        
    // convert the adc values to the values to be displayed  XXX tbd
#if 1
    // XXX simulation
    static float sim_voltage_rms_kv;
    static bool increasing = true;
    voltage_rms_kv = sim_voltage_rms_kv;
    if (increasing) {
        sim_voltage_rms_kv += .1;
        if (sim_voltage_rms_kv >= 30) {
            increasing = false;
        }
    } else {
        sim_voltage_rms_kv -= .1;
        if (sim_voltage_rms_kv <= 0) {
            increasing = true;
        }
    }

    voltage_min_kv = 0;
    voltage_max_kv = 0;
    current_ma = 0;
    pressure_mtorr = 0;
#endif

    sprintf(str, "%4.1f kV rms", voltage_rms_kv);
    sdl_render_text(data_pane, 0, 0, 1, str, WHITE, BLACK);
    sprintf(str, "%4.1f kV min", voltage_min_kv);
    sdl_render_text(data_pane, 1, 0, 1, str, WHITE, BLACK);
    sprintf(str, "%4.1f kV max", voltage_max_kv);
    sdl_render_text(data_pane, 2, 0, 1, str, WHITE, BLACK);
    sprintf(str, "%4.1f mA", current_ma);
    sdl_render_text(data_pane, 3, 0, 1, str, WHITE, BLACK);
    sprintf(str, "%4.1f mTorr", pressure_mtorr);
    sdl_render_text(data_pane, 4, 0, 1, str, WHITE, BLACK);
}

void draw_graph(rect_t * graph_pane)
{
#if 0 // XXX later
    #define MAX_GRAPH_CONFIG (sizeof(graph_config)/sizeof(graph_config[0]))
    static struct graph_config {
        char   * name;
        float    scale;
        int32_t  color;
        off_t    offset;
    } graph_config[] = {
        { "kV   30", 30.0, RED, offsetof(history_t, voltage_rms_kv) },
                            };

    #define MAX_SCALE_CONFIG (sizeof(scale_config)/sizeof(scale_config[0])
    static struct scale_config {
        char  * name;
        int32_t secs;
    } scale_config[] = {
        { "<---  1 MINUTE  --->", 60 }
                            };
    static int32_t scale_config_idx;

    rect_t  rect;
    int32_t x_start, x_end, y_start, y_end;
    int32_t i, j, count;
    struct graph_config * gc;
    point_t points[1000];

    // init
    x_start = 20;
    x_end   = graph_pane->w - sdl_font_char_width(0)*10;
    y_start = graph_pane->h - (sdl_font_char_height(0) + 1);
    y_end   = 20;

    // fill white
    rect.x = 0;
    rect.y = 0;
    rect.w = graph_pane->w;
    rect.h = graph_pane->h;
    sdl_render_fill_rect(graph_pane, &rect, WHITE);

    // draw the axis
    sdl_render_line(graph_pane, x_start, y_start, x_end, y_start, BLACK);
    //sdl_render_line(graph_pane, x_start, y_start-1, x_end, y_start-1, BLACK);

    sdl_render_line(graph_pane, x_start, y_start, x_start, y_end, BLACK);
    //sdl_render_line(graph_pane, x_start+1, y_start, x_start+1, y_end, BLACK);

    // draw names and scales
    for (i = 0; i < MAX_GRAPH_CONFIG; i++) {
        sdl_render_text(graph_pane, i, -10, 0, 
                        graph_config[i].name, 
                        graph_config[i].color, WHITE);
    }

    // draw the horizontal scale
    // XXX make this clickable
    sdl_render_text(graph_pane, -1, 25, 0, 
                    scale_config[scale_config_idx].name, 
                    BLACK, WHITE);

    // draw the graphs
    // XXX optimize
    // XXX draw cursor
    // XXX graph back from the cursor
    for (i = 0; i < MAX_GRAPH_CONFIG; i++) {
        gc = &graph_config[i];
        count = 0;
        for (j = history_start_time; j <= history_end_time; j++) {
            history_t * h = &history[j-history_start_time];
            if (h->valid) {
                //printf("VALID is TRUE\n");
                float  val  = *(float*)((void*)h+gc->offset);
                float  secs = j - history_start_time;
                points[count].x = x_start+1 + (x_end - x_start) * secs / 60;
                points[count].y = y_start-1 + (y_end - y_start) * val / 30;
                count++;
            } else {
                //printf("VALID is FALSE\n");
                sdl_render_lines(graph_pane, points, count, gc->color);
                count = 0;
            }
        }
        if (count) {
            sdl_render_lines(graph_pane, points, count, gc->color);
            count = 0;
        }
    }
#endif
}

// -----------------  GET AND FREE DATA  ---------------------------------------------

data_t *  get_data(void)
{
    data_t * data;

    // allocate data buffer
    data = calloc(1, sizeof(data_t));
    if (data == NULL) {
        FATAL("alloc data failed\n");
    }

    // processing for either LIVE_MODE or PLAYBACK_MODE
    if (mode == LIVE_MODE) {
        int32_t   ret, i, chan;
        data_t    data2;
        time_t    t;
        size_t    len;

        static off_t jpeg_buff_offset = MAX_HISTORY * sizeof(data_t);

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

        // get adc data from the dataq device
        if (!no_dataq) {
            for (i = 0; i < MAX_ADC_CHAN_LIST; i++) {
                chan = adc_chan_list[i];
                ret = dataq_get_adc(chan, &data->adc_value[chan]);
                if (ret != 0) {
                    break;
                }
            }
            data->adc_valid = (i == MAX_ADC_CHAN_LIST);
        }

        // if data time is 1 second beyond the end of current saved history then
        //   store data in history, and
        //   write to file
        // endif
        t = data->time_us / 1000000;
        if (t > history_end_time_sec) {
            // determine the index into history buffer, indexed by seconds
            int32_t idx = t - history_start_time_sec;
            if (idx < 0 || idx >= MAX_HISTORY) {  // XXX or max_history
                FATAL("invalid history idx = %d\n", idx);
            }
            printf("XXX ADDING HISTORY %d\n", idx);

            // save the data in history  XXX probably update cursor here
            history_end_time_sec = t;
            history[idx] = *data;

            // update the graph cursor_time, 
            // in live mode the graph tracks the most recent data stored in history
            cursor_time_sec = history_end_time_sec;

            // write to history file
            data2 = history[idx];
            if (data2.jpeg_valid) {
                data2.jpeg_buff_offset = jpeg_buff_offset;
                data2.jpeg_buff_ptr = NULL;
                jpeg_buff_offset += data2.jpeg_buff_len;

                len = pwrite(fd, history[idx].jpeg_buff_ptr, data2.jpeg_buff_len, data2.jpeg_buff_offset);
                printf("write %d\n", len);
                if (len !=  data2.jpeg_buff_len) {
                    FATAL("failed write jpeg to file, len=%d, %s\n", len, strerror(errno));
                }
            } else {
                data2.jpeg_buff_offset = 0;
                data2.jpeg_buff_ptr = NULL;
            }

            len = pwrite(fd, &data2, sizeof(data2), idx * sizeof(data2));
            printf("write %d\n", len);
            if (len != sizeof(data2)) {
                FATAL("failed write data2 to file, len=%d, %s\n", len, strerror(errno));
            }

            // XXX wirt to history file, max history
        }
    } else {
        int32_t len, idx;

        // PLAYBACK_MODE ...

        // copy the data from the history array, at cursor_time_sec
        idx = cursor_time_sec - history_start_time_sec;
        if (idx < 0 || idx >= MAX_HISTORY) {  // XXX or max_history
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
