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

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"
#include "util_sdl.h"
#include "util_jpeg_decode.h"
#include "util_cam.h"
#include "util_misc.h"
#include "about.h"

//
// defines
//

#define VERSION_STR  "1.0"

#define MODE_STR(m) ((m) == LIVE ? "LIVE" : (m) == PLAYBACK ? "PLAYBACK" : "TEST")

#define MAGIC_FILE 0x1122334455667788

#define MAX_FILE_DATA_PART1   (6*3600)  // 6 hours
#define MAX_DATA_PART2_LENGTH 1000000

#define FILE_DATA_PART2_OFFSET \
   ((sizeof(file_hdr_t) +  \
     sizeof(struct data_part1_s) * MAX_FILE_DATA_PART1 + \
     0x1000) & ~0xfffL)

#define FONT0_HEIGHT (sdl_font_char_height(0))
#define FONT0_WIDTH  (sdl_font_char_width(0))
#define FONT1_HEIGHT (sdl_font_char_height(1))
#define FONT1_WIDTH  (sdl_font_char_width(1))

#define DEFAULT_WIN_WIDTH         1920
#define DEFAULT_WIN_HEIGHT        1000

//#define JPEG_BUFF_SAMPLE_CREATE_ENABLE
#define JPEG_BUFF_SAMPLE_FILENAME "support/jpeg_buff_sample.bin"

#define REDUCE(val, tbl) \
    do { \
        int32_t i; \
        for (i = 1; i < sizeof(tbl)/sizeof(tbl[0]); i++) { \
            if (val <= tbl[i]) { \
                val = tbl[i-1]; \
                break; \
            } \
        } \
    } while (0)

#define INCREASE(val, tbl) \
    do { \
        int32_t i; \
        for (i = sizeof(tbl)/sizeof(tbl[0])-2; i >= 0; i--) { \
            if (val >= tbl[i]) { \
                val = tbl[i+1]; \
                break; \
            } \
        } \
    } while (0)

#define DEFAULT_IMAGE_X                     "320"
#define DEFAULT_IMAGE_Y                     "240"
#define DEFAULT_IMAGE_SIZE                  "300"
#define DEFAULT_NEUTRON_PHT_MV              "100"
#define DEFAULT_NEUTRON_SCALE_CPM           "100"
#define DEFAULT_SUMMARY_GRAPH_TIME_SPAN_SEC "600"
#define DEFAULT_FILE_IDX_PLAYBACK_INIT      "0"
#define DEFAULT_ADC_DATA_GRAPH_SELECT       "0"
#define DEFAULT_ADC_DATA_GRAPH_MAX_Y_MV     "10000"

#define CONFIG_IMAGE_X                      (config[0].value)
#define CONFIG_IMAGE_Y                      (config[1].value)
#define CONFIG_IMAGE_SIZE                   (config[2].value)
#define CONFIG_NEUTRON_PHT_MV               (config[3].value)
#define CONFIG_NEUTRON_SCALE_CPM            (config[4].value)
#define CONFIG_SUMMARY_GRAPH_TIME_SPAN_SEC  (config[5].value)
#define CONFIG_FILE_IDX_PLAYBACK_INIT       (config[6].value)
#define CONFIG_ADC_DATA_GRAPH_SELECT        (config[7].value)
#define CONFIG_ADC_DATA_GRAPH_MAX_Y_MV      (config[8].value)

#define UNITS_KV     1
#define UNITS_MA     2
#define UNITS_CPM    3
#define UNITS_D2_MT  4
#define UNITS_N2_MT  5

//
// typedefs
//

enum mode {LIVE, PLAYBACK, TEST};

typedef struct {
    uint64_t magic;
    uint64_t start_time;  // XXX not used
    uint32_t max;
    uint8_t  reserved[4096-20];
} file_hdr_t;

//
// variables
//

static enum mode                mode;
static bool                     initial_mode;
static bool                     lost_connection;
static bool                     file_error;
static bool                     time_error;
static bool                     program_terminating;
static bool                     cam_thread_running;
static bool                     opt_no_cam;
static char                     screenshot_prefix[100];
static struct sockaddr_in       server_sockaddr;

static uint32_t                 win_width;
static uint32_t                 win_height;

static int32_t                  file_fd;
static file_hdr_t             * file_hdr;
static struct data_part1_s    * file_data_part1;
static int32_t                  file_idx_global;

static int32_t                  test_file_secs;

static uint8_t                  jpeg_buff[1000000];
static int32_t                  jpeg_buff_len;
static uint64_t                 jpeg_buff_us;
static pthread_mutex_t          jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;

static char                     config_path[PATH_MAX];
static const int32_t            config_version = 1;
static config_t                 config[] = { { "image_x",                     DEFAULT_IMAGE_X                     },
                                             { "image_y",                     DEFAULT_IMAGE_Y                     },
                                             { "image_size",                  DEFAULT_IMAGE_SIZE                  },
                                             { "neutron_pht_mv",              DEFAULT_NEUTRON_PHT_MV              },
                                             { "neutron_scale_cpm",           DEFAULT_NEUTRON_SCALE_CPM           },
                                             { "summary_graph_time_span_sec", DEFAULT_SUMMARY_GRAPH_TIME_SPAN_SEC },
                                             { "file_idx_playback_init",      DEFAULT_FILE_IDX_PLAYBACK_INIT      },
                                             { "adc_data_graph_select",       DEFAULT_ADC_DATA_GRAPH_SELECT       },
                                             { "adc_data_graph_max_y_mv",     DEFAULT_ADC_DATA_GRAPH_MAX_Y_MV     },
                                             { "",                            ""                     } };
static int32_t                  image_x;
static int32_t                  image_y;
static int32_t                  image_size;
static int32_t                  neutron_pht_mv;
static int32_t                  neutron_scale_cpm;
static int32_t                  summary_graph_time_span_sec;
static int64_t                  file_idx_playback_init;
static int32_t                  adc_data_graph_select;
static int32_t                  adc_data_graph_max_y_mv;

//
// prototypes
//

static int32_t initialize(int32_t argc, char ** argv);
static void usage(void);
static void * get_live_data_thread(void * cx);
static int32_t write_data_to_file(data_t * data);
static void * cam_thread(void * cx);
static int32_t display_handler();
static void draw_camera_image(rect_t * cam_pane, int32_t file_idx);
static void draw_camera_image_control(char key);
static void draw_data_values(rect_t * data_pane, int32_t file_idx);
static void draw_summary_graph(rect_t * graph_pane, int32_t file_idx);
static void draw_summary_graph_control(char key);
static void draw_adc_data_graph(rect_t * graph_pane, int32_t file_idx);
static void draw_adc_data_graph_control(char key);
static void draw_graph_common(rect_t * graph_pane, char * title_str, int32_t x_range_param, int32_t str_col, char * x_info_str, char * y_info_str, float cursor_pos, char * cursor_str, int32_t max_graph, ...);
static int32_t generate_test_file(void);
static char * val2str(float val, int32_t units);
static struct data_part2_s * read_data_part2(int32_t file_idx);
static float neutron_cpm(int32_t file_idx);

// -----------------  MAIN  ----------------------------------------------------------

int32_t main(int32_t argc, char **argv)
{
    int32_t wait_time_ms;

    // initialize
    if (initialize(argc, argv) < 0) {
        ERROR("initialize failed, program terminating\n");
        return 1;
    }

    // run time
    switch (mode) {
    case TEST:
        if (generate_test_file() < 0) {
            ERROR("generate_test_file failed, program terminating\n");
            return 1;
        }
        break;
    case LIVE: case PLAYBACK:
        if (display_handler() < 0) {
            ERROR("display_handler failed, program terminating\n");
            return 1;
        }
        break;
    default:
        FATAL("mode %d not valid\n", mode);
        break;
    }

    // program termination
    program_terminating = true;
    wait_time_ms = 0;
    while (cam_thread_running && wait_time_ms < 5000) {
        usleep(10000);  // 10 ms
        wait_time_ms += 10;
    }

    INFO("terminating normally\n");
    return 0; 
}

// -----------------  INITIALIZE  ----------------------------------------------------

static void atexit_config_write(void);

static int32_t initialize(int32_t argc, char ** argv)
{
    struct rlimit      rl;
    pthread_t          thread;
    int32_t            ret, len;
    char               filename[100];
    char               servername[100];
    char               s[100];
    int32_t            wait_ms;
    const char       * config_dir;

    // use line bufferring
    setlinebuf(stdout);

    // init core dumps
    // note - requires fs.suid_dumpable=1  in /etc/sysctl.conf if this is a suid pgm
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    // print size of data part1 and part2, and validate sizes are multiple of 8
    DEBUG("sizeof data_t=%zd part1=%zd part2=%zd\n",
          sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));
    if ((sizeof(struct data_part1_s) % 8) || (sizeof(struct data_part2_s) % 8)) {
        FATAL("sizeof data_t=%zd part1=%zd part2=%zd\n",
              sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));
    }

    // init globals that are not 0
    mode = LIVE;
    file_idx_global = -1;
    strcpy(servername, "rpi_data");
    win_width = DEFAULT_WIN_WIDTH;
    win_height = DEFAULT_WIN_HEIGHT;

    // init locals
    strcpy(filename, "");
 
    // parse options
    // -h          : help
    // -v          : version
    // -g WxH      : window width and height, default 1920x1080
    // -s name     : server name
    // -p filename : playback file
    // -x          : don't capture cam data in live mode
    // -t secs     : generate test data file, secs long
    while (true) {
        char opt_char = getopt(argc, argv, "hvg:s:p:xt:");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'h':
            usage();
            exit(0);
        case 'v':
            INFO("Version %s\n", VERSION_STR);
            exit(0);
        case 'g':
            if (sscanf(optarg, "%dx%d", &win_width, &win_height) != 2) {
                ERROR("invalid '-g %s'\n", optarg);
                exit(1);
            }
            break;
        case 's':
            strcpy(servername, optarg);
            break;
        case 'p':
            mode = PLAYBACK;
            strcpy(filename, optarg);
            break;
        case 'x':
            opt_no_cam = true;
            break;  
        case 't':
            mode = TEST;
            if (sscanf(optarg, "%d", &test_file_secs) != 1 || test_file_secs < 1 || test_file_secs > MAX_FILE_DATA_PART1) {
                ERROR("test_file_secs '%s' is invalid\n",optarg);
                return -1;
            }
            break;
        default:
            return -1;
        }
    }

    // read config file, and 
    // initialize exit handler to write config file
    config_dir = getenv("HOME");
    if (config_dir == NULL) {
        FATAL("env var HOME not set\n");
    }
    sprintf(config_path, "%s/.fusorrc", config_dir);
    if (config_read(config_path, config, config_version) < 0) {
        FATAL("config_read failed for %s\n", config_path);
    }
    if (sscanf(CONFIG_IMAGE_X, "%d", &image_x) != 1 ||
        sscanf(CONFIG_IMAGE_Y, "%d", &image_y) != 1 ||
        sscanf(CONFIG_IMAGE_SIZE, "%d", &image_size) != 1 ||
        sscanf(CONFIG_NEUTRON_PHT_MV, "%d", &neutron_pht_mv) != 1 ||
        sscanf(CONFIG_NEUTRON_SCALE_CPM, "%d", &neutron_scale_cpm) != 1 ||
        sscanf(CONFIG_SUMMARY_GRAPH_TIME_SPAN_SEC, "%d", &summary_graph_time_span_sec) != 1 ||
        sscanf(CONFIG_FILE_IDX_PLAYBACK_INIT, "%ld", &file_idx_playback_init) != 1 ||
        sscanf(CONFIG_ADC_DATA_GRAPH_SELECT, "%d", &adc_data_graph_select) != 1 ||
        sscanf(CONFIG_ADC_DATA_GRAPH_MAX_Y_MV, "%d", &adc_data_graph_max_y_mv) != 1) 
    {
        FATAL("invalid config value, not a number\n");
    }
    atexit(atexit_config_write);

    // if mode is live or test then 
    //   if filename was provided then 
    //     use the provided filename
    //   else if live mode then
    //     generate live mode filename
    //   else
    //     generate test mode filename
    //   endif
    // endif
    if (mode == LIVE || mode == TEST) {
        if (argc > optind) {
            strcpy(filename, argv[optind]);
        } else if (mode == LIVE) {
            time_t t = time(NULL);
            struct tm * tm = localtime(&t);
            sprintf(filename, "fusor_%2.2d%2.2d%2.2d_%2.2d%2.2d%2.2d.dat",
                    tm->tm_year-100, tm->tm_mon+1, tm->tm_mday,
                    tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else {  // mode is TEST
            sprintf(filename, "fusor_test_%d_secs.dat", test_file_secs);
        }
    }

    // print mode and filename
    INFO("mode            = %s\n", MODE_STR(mode));
    INFO("filename        = %s\n", filename);
    if (mode == TEST) {
        INFO("test_file_secs  = %d\n", test_file_secs);
    }

    // validate filename extension is '.dat', and
    // save screenshot prefix
    len = strlen(filename);
    if (len < 5 || strcmp(&filename[len-4], ".dat") != 0) {
        ERROR("filename must have '.dat. extension\n");
        return -1;
    }
    memcpy(screenshot_prefix, filename, len-4);

    // if mode is live or test then 
    //   verify filename does not exist
    //   create and init the file  
    // endif
    if (mode == LIVE || mode == TEST) {
        // verify filename does not exist
        struct stat stat_buf;
        if (stat(filename, &stat_buf) == 0) {
            ERROR("file %s already exists\n", filename);
            return -1;
        }

        // create and init the file  
        file_hdr_t hdr;
        int32_t fd;
        fd = open(filename, O_CREAT|O_EXCL|O_RDWR, 0666);
        if (fd < 0) {
            ERROR("failed to create %s, %s\n", filename, strerror(errno));
            return -1;
        }
        bzero(&hdr, sizeof(hdr));
        hdr.magic      = MAGIC_FILE;
        hdr.start_time = 0;
        hdr.max        = 0;
        if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            ERROR("failed to init %s, %s\n", filename, strerror(errno));
            return -1;
        }
        if (ftruncate(fd,  FILE_DATA_PART2_OFFSET) < 0) {
            ERROR("ftuncate failed on %s, %s\n", filename, strerror(errno));
            return -1;
        }
        close(fd);
    }

    // if in playback mode then
    //   verify filename exists
    // endif
    if (mode == PLAYBACK) {
        struct stat stat_buf;
        if (stat(filename, &stat_buf) == -1) {
            ERROR("file %s does not exist, %s\n", filename, strerror(errno));
            return -1;
        }
    }
    
    // open and map filename
    file_fd = open(filename, mode == PLAYBACK ? O_RDONLY : O_RDWR);
    if (file_fd < 0) {
        ERROR("failed to open %s, %s\n", filename, strerror(errno));
        return -1;
    }
    file_hdr = mmap(NULL,  // addr
                    sizeof(file_hdr_t),
                    mode == PLAYBACK ? PROT_READ : PROT_READ|PROT_WRITE,
                    MAP_SHARED,
                    file_fd,
                    0);   // offset
    if (file_hdr == MAP_FAILED) {
        ERROR("failed to map file_hdr %s, %s\n", filename, strerror(errno));
        return -1;
    }
    file_data_part1 = mmap(NULL,  // addr
                           sizeof(struct data_part1_s) * MAX_FILE_DATA_PART1,
                           mode == PLAYBACK ? PROT_READ : PROT_READ|PROT_WRITE,
                           MAP_SHARED,
                           file_fd,
                           sizeof(file_hdr_t));   // offset
    if (file_data_part1 == MAP_FAILED) {
        ERROR("failed to map file_data_part1 %s, %s\n", filename, strerror(errno));
        return -1;
    }

    // verify file header
    if (file_hdr->magic != MAGIC_FILE ||
        file_hdr->max > MAX_FILE_DATA_PART1)
    {
        ERROR("invalid file %s, magic=0x%"PRIx64" max=%d\n", 
              filename, file_hdr->magic, file_hdr->max);
        return -1;
    }

    // if in live mode then
    //   get server address
    //   create thread to acquire data from server
    //   wait for first data to be received from server
    //   cam_init
    // endif
    if (mode == LIVE) {
        // get address of server, 
        // print servername, and serveraddr
        ret =  getsockaddr(servername, PORT, &server_sockaddr);
        if (ret < 0) {
            ERROR("failed to get address of %s\n", servername);
            return -1;
        }
        INFO("servername      = %s\n", servername);
        INFO("serveraddr      = %s\n", 
             sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&server_sockaddr));

        // create get_live_data_thread        
        if (pthread_create(&thread, NULL, get_live_data_thread, NULL)) {
            ERROR("pthread_create get_live_data_thread, %s\n", strerror(errno));
            return -1;
        }

        // wait for get_live_data_thread to get first data, tout 5 secs
        wait_ms = 0;
        while (file_idx_global == -1) {
            wait_ms += 10;
            usleep(10000);
            if (wait_ms >= 5000) {
                ERROR("failed to receive data from server\n");
                unlink(filename);
                return -1;
            }
        }

        // cam_init
        if (!opt_no_cam) {
            if (cam_init(CAM_WIDTH, CAM_HEIGHT, FRAMES_PER_SEC) == 0) {
                if (pthread_create(&thread, NULL, cam_thread, NULL) != 0) {
                    FATAL("pthread_create cam_thread, %s\n", strerror(errno));
                }
            }
        }
    }

    // if in playback mode then
    //   verify the first entry of the file_data_part1 is valid, and
    //   set file_idx_global
    // endif
    if (mode == PLAYBACK) {
        // verify data_part1 magic
        if (file_data_part1[0].magic != MAGIC_DATA_PART1) {
            ERROR("no data in file %s (0x%"PRIx64"\n", filename, file_data_part1[0].magic);
            return -1;
        }

        // try to init file_idx_global to location from prior invocation of display
        if (file_idx_playback_init != 0 &&
            file_hdr->max >= 1 &&
            file_idx_playback_init >= file_data_part1[0].time &&
            file_idx_playback_init <= file_data_part1[file_hdr->max-1].time)
        {
            file_idx_global = file_idx_playback_init - file_data_part1[0].time;
        } else {
            file_idx_global = 0;
        }

        // sanity check file_idx_global
        if (file_idx_global < 0 || file_idx_global >= file_hdr->max) {
            ERROR("file_idx_global=%d is out of range, file_hdr->max=%d, "
                  "setting file_idx_global to zero\n",
                  file_idx_global, file_hdr->max);
            file_idx_global = 0;
        }
    }

    // set initial_mode
    initial_mode = mode;

    // return success
    return 0;
}

static void usage(void)
{
    printf("\n"
           "usage: display [options]\n"
           "\n"
           "   where options include:\n"
           "       -h          : help\n"
           "       -v          : version\n"
           "       -g WxH      : window width and height, default 1920x1080\n"
           "       -s name     : server name\n"
           "       -p filename : playback file\n"
           "       -x          : don't capture cam data in live mode\n"
           "       -t secs     : generate test data file, secs long\n" 
           "\n"
                    );
}

static void atexit_config_write(void)
{
    sprintf(CONFIG_IMAGE_X, "%d", image_x);
    sprintf(CONFIG_IMAGE_Y, "%d", image_y);
    sprintf(CONFIG_IMAGE_SIZE, "%d", image_size);
    sprintf(CONFIG_NEUTRON_PHT_MV, "%d", neutron_pht_mv);
    sprintf(CONFIG_NEUTRON_SCALE_CPM, "%d", neutron_scale_cpm);
    sprintf(CONFIG_SUMMARY_GRAPH_TIME_SPAN_SEC, "%d", summary_graph_time_span_sec);
    if (initial_mode == PLAYBACK) {
        sprintf(CONFIG_FILE_IDX_PLAYBACK_INIT, "%ld", file_idx_global+file_data_part1[0].time);
    } else {
        sprintf(CONFIG_FILE_IDX_PLAYBACK_INIT, "%d", 0);
    }
    sprintf(CONFIG_ADC_DATA_GRAPH_SELECT, "%d", adc_data_graph_select);
    sprintf(CONFIG_ADC_DATA_GRAPH_MAX_Y_MV, "%d", adc_data_graph_max_y_mv);
    config_write(config_path, config, config_version);
}

// -----------------  GET LIVE DATA THREAD  ------------------------------------------

static void * get_live_data_thread(void * cx)
{
    int32_t               sfd, len;
    struct timeval        rcvto;
    data_t              * data;
    struct data_part1_s * dp1;
    struct data_part2_s * dp2;
    uint64_t              last_data_time_written_to_file;
    uint64_t              t, time_now, time_delta;
    data_t                data_novalue;

    // init data_novalue
    bzero(&data_novalue, sizeof(data_novalue));

    dp1                                           = &data_novalue.part1;
    dp1->magic                                    = MAGIC_DATA_PART1;
    dp1->time                                     = 0;
    dp1->voltage_kv                               = ERROR_NO_VALUE;
    dp1->current_ma                               = ERROR_NO_VALUE;
    dp1->d2_pressure_mtorr                        = ERROR_NO_VALUE;
    dp1->n2_pressure_mtorr                        = ERROR_NO_VALUE;
    dp1->max_neutron_pulse                        = 0;
    dp1->data_part2_offset                        = 0;
    dp1->data_part2_length                        = sizeof(struct data_part2_s);
    dp1->data_part2_jpeg_buff_len                 = 0;
    dp1->data_part2_voltage_adc_data_valid        = false;
    dp1->data_part2_current_adc_data_valid        = false;
    dp1->data_part2_pressure_adc_data_valid       = false;

    dp2                                           = &data_novalue.part2;
    dp2->magic                                    = MAGIC_DATA_PART2;

    // init others
    sfd = -1;
    data = calloc(1, sizeof(struct data_part1_s) + MAX_DATA_PART2_LENGTH);
    if (data == NULL) {
        FATAL("calloc\n");
    }
    dp1 = &data->part1;
    dp2 = &data->part2;
    last_data_time_written_to_file = 0;

try_to_connect_again:
    // create socket 
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) {
        FATAL("create socket, %s\n", strerror(errno));
    }

    // if failed to connect to the server then
    //   goto connection_failed
    // endif
    if (connect(sfd, (struct sockaddr *)&server_sockaddr, sizeof(server_sockaddr)) < 0) {
        char s[100];
        ERROR("connect to %s, %s\n", 
              sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&server_sockaddr),
              strerror(errno));
        goto connection_failed;
    }

    // set recv timeout to 5 seconds
    rcvto.tv_sec  = 5;
    rcvto.tv_usec = 0;
    if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcvto, sizeof(rcvto)) < 0) {
        FATAL("setsockopt SO_RCVTIMEO, %s\n",strerror(errno));
    }

    // loop getting data
    while (true) {
        // read data part1 from server, and
        // verify data part1 magic, and length
        len = do_recv(sfd, dp1, sizeof(struct data_part1_s));
        if (len != sizeof(struct data_part1_s)) {
            ERROR("recv dp1 len=%d exp=%zd, %s\n",
                  len, sizeof(struct data_part1_s), strerror(errno));
            goto connection_failed;
        }
        if (dp1->magic != MAGIC_DATA_PART1) {
            ERROR("recv dp1 bad magic 0x%"PRIx64"\n", 
                  dp1->magic);
            goto connection_failed;
        }
        if (dp1->data_part2_length > MAX_DATA_PART2_LENGTH) {
            ERROR("data_part2_length %d is too big\n", 
                  dp1->data_part2_length);
            goto connection_failed;
        }

        // read data part2 from server,
        // verify magic
        len = do_recv(sfd, dp2, dp1->data_part2_length);
        if (len != dp1->data_part2_length) {
            ERROR("recv dp2 len=%d exp=%d, %s\n",
                  len, dp1->data_part2_length, strerror(errno));
            goto connection_failed;
        }
        if (dp2->magic != MAGIC_DATA_PART2) {
            ERROR("recv dp2 bad magic 0x%"PRIx64"\n", 
                  dp2->magic);
            goto connection_failed;
        }

        // got data part1 and part2 therefore connection is working
        lost_connection = false;

        // if data part2 does not contain camera data then 
        // see if the camera data is being captured by this program, 
        // and add it
        if (dp1->data_part2_jpeg_buff_len == 0) {
            pthread_mutex_lock(&jpeg_mutex);
            if (microsec_timer() - jpeg_buff_us < 1000000) {
                memcpy(dp2->jpeg_buff, jpeg_buff, jpeg_buff_len);
                dp1->data_part2_jpeg_buff_len = jpeg_buff_len;
                dp1->data_part2_length = sizeof(struct data_part2_s) + jpeg_buff_len;
            }
            pthread_mutex_unlock(&jpeg_mutex);
        }

        // if opt_no_cam then disacard camera data
        if (opt_no_cam) {
            dp1->data_part2_jpeg_buff_len = 0;
            dp1->data_part2_length = sizeof(struct data_part2_s);
        }

        // verify the time of received data is close to the time 
        // on the computer running this display program
        time_now = time(NULL);
        time_delta = (time_now > data->part1.time 
                      ? time_now - data->part1.time
                      : data->part1.time - time_now);
        if (time_delta > 5) {
            ERROR("server time delta = %"PRId64"\n", time_delta);
            goto time_error;
        }

        // if this is the first write then
        //    write the data to file
        // else if data time <= last_data_time_written_to_file
        //    discard
        // else 
        //    write 'no-value' data if needed
        //    write data to file
        // endif
        if (last_data_time_written_to_file == 0) {
            // write data to file
            if (write_data_to_file(data) < 0) {
                goto file_error;
            }
            last_data_time_written_to_file = dp1->time;
        } else if (dp1->time <= last_data_time_written_to_file) {
            // discard
            WARN("discarding received data, time %"PRId64" <= %"PRId64"\n",
                 dp1->time, last_data_time_written_to_file);
        } else {
            // if there is a time gap then
            // write no-value data to file to fill the gap
            for (t = last_data_time_written_to_file+1; t < dp1->time; t++) {
                WARN("writing no-value data to file for time %"PRId64"\n", t);
                data_novalue.part1.time = t;
                if (write_data_to_file(&data_novalue) < 0) {
                    goto file_error;
                }
                last_data_time_written_to_file = t;
            }

            // write data to file
            if (write_data_to_file(data) < 0) {
                goto file_error;
            }
            last_data_time_written_to_file = dp1->time;
        }

        // if live mode then update file_idx_global
        if (mode == LIVE) {
            file_idx_global = file_hdr->max - 1;
            __sync_synchronize();
        }

#ifdef JPEG_BUFF_SAMPLE_CREATE_ENABLE
        // write a sample jpeg buffer to jpeg_buff_sample file
        static bool sample_written = false;
        if (!sample_written && dp1->data_part2_jpeg_buff_len != 0) {
            int32_t fd = open(JPEG_BUFF_SAMPLE_FILENAME, O_CREAT|O_TRUNC|O_RDWR, 0666);
            if (fd < 0) {
                ERROR("open %s, %s\n", JPEG_BUFF_SAMPLE_FILENAME, strerror(errno));
            } else {
                int32_t len = write(fd, dp2->jpeg_buff, dp2->jpeg_buff_len);
                if (len != dp2->jpeg_buff_len) {
                    ERROR("write %s len exp=%d act=%d, %s\n",
                        JPEG_BUFF_SAMPLE_FILENAME, dp2->jpeg_buff_len, len, strerror(errno));
                }
                close(fd);
            }
            sample_written = true;
        }
#endif
    }

connection_failed:
    // handle connectio failed error ...

    // set lost_connection flag,
    // close socket
    ERROR("connection_failed - attempting to reestablish connection\n");
    lost_connection = true;
    if (sfd != -1) {
        close(sfd);
        sfd = -1;
    }

    // write no-value data to file to fill the gap
    if (last_data_time_written_to_file != 0) {
        time_now = time(NULL);
        for (t = last_data_time_written_to_file+1; t < time_now; t++) {
            WARN("writing no-value data to file for time %"PRId64"\n", t);
            data_novalue.part1.time = t;
            if (write_data_to_file(&data_novalue) < 0) {
                goto file_error;
            }
            last_data_time_written_to_file = t;
        }
    }

    // if live mode then update file_idx_global
    if (mode == LIVE) {
        file_idx_global = file_hdr->max - 1;
        __sync_synchronize();
    }

    // sleep 1 sec, 
    // and try to connect again
    sleep(1);
    goto try_to_connect_again;

file_error:
    // handle file error ...

    // this program can not continue receiving data because the program 
    // relies on being able to read data from the file, so set error flags
    // and exit this thread
    ERROR("file error - get_live_data_thread terminating\n");
    lost_connection = true;
    file_error = true;
    if (sfd != -1) {
        close(sfd);
        sfd = -1;
    }
    return NULL;

time_error:
    // handle time error ...

    // this program requires the time on the server to be 
    // reasonably well synced with the time on the computer running 
    // the display program; if not then exit this thread
    ERROR("time error - get_live_data_thread terminating\n");
    lost_connection = true;
    time_error = true;
    if (sfd != -1) {
        close(sfd);
        sfd = -1;
    }
    return NULL;
}

static int32_t write_data_to_file(data_t * data)
{
    int32_t         len;

    static uint64_t last_time;
    static off_t    data_part2_offset = FILE_DATA_PART2_OFFSET;

    // if file is full then return error
    if (file_hdr->max >= MAX_FILE_DATA_PART1) {
        ERROR("file is full\n");
        return -1;
    }

    // verify file is being written with increasing timestamp
    if (last_time != 0 && data->part1.time != last_time+1) {
        FATAL("data time out of sequence, %"PRId64" should be %"PRId64"\n",
              data->part1.time, last_time+1);
    }
    last_time = data->part1.time;

    // save file data_part2_offset in data part1
    data->part1.data_part2_offset = data_part2_offset;

    // write data_part1 to file (file_data_part1 is memory mapped to the file)              
    file_data_part1[file_hdr->max] = data->part1;

    // write data_part2 to file
    len = pwrite(file_fd, &data->part2, data->part1.data_part2_length, data_part2_offset);
    if (len != data->part1.data_part2_length) {
        ERROR("write data_part2 len=%d exp=%d, %s\n",
              len, data->part1.data_part2_length, strerror(errno));
        return -1;
    }
    data_part2_offset += data->part1.data_part2_length;

    // update the file_hdr (also memory mapped)
    file_hdr->max++;
    __sync_synchronize();

    // return success
    return 0;
}

// -----------------  CAM THREAD  ----------------------------------------------------

static void * cam_thread(void * cx)
{
    int32_t   ret;
    uint8_t * ptr;
    uint32_t  len;

    INFO("starting\n");
    cam_thread_running = true;

    while (true) {
        // if program terminating then exit this thread
        if (program_terminating) {
            break;
        }

        // get cam buff
        ret = cam_get_buff(&ptr, &len);
        if (ret != 0) {
            usleep(100000);
            continue;
        }

        // copy buff to global
        pthread_mutex_lock(&jpeg_mutex);
        memcpy(jpeg_buff, ptr, len);
        jpeg_buff_len = len;
        jpeg_buff_us = microsec_timer();
        pthread_mutex_unlock(&jpeg_mutex);

        // put buff
        cam_put_buff(ptr);
    }

    INFO("exitting\n");
    cam_thread_running = false;
    return NULL;
}

// -----------------  DISPLAY HANDLER - MAIN  ----------------------------------------

static int32_t display_handler(void)
{
    #define MAX_PLAYBACK_SPEED 5
    #define SET_PLAYBACK_PAUSED \
        do { \
            playback_speed = 0; \
            playback_advance_us = 0; \
        } while (0)

    bool          quit;
    sdl_event_t * event;
    rect_t        title_pane_full, title_pane; 
    rect_t        cam_pane_full, cam_pane;
    rect_t        data_pane_full, data_pane;
    rect_t        summary_graph_pane_full, summary_graph_pane;
    rect_t        adc_data_graph_pane_full, adc_data_graph_pane;
    char          str[100];
    struct tm   * tm;
    time_t        t;
    int32_t       file_idx;
    int32_t       event_processed_count;
    int32_t       file_max_last;
    bool          lost_connection_msg_is_displayed;
    bool          file_error_msg_is_displayed;
    bool          time_error_msg_is_displayed;
    int32_t       playback_speed;
    uint64_t      playback_advance_us;

    // initializae 
    quit = false;
    file_max_last = -1;
    lost_connection_msg_is_displayed = false;
    file_error_msg_is_displayed = false;
    time_error_msg_is_displayed = false;
    playback_speed = 0;
    playback_advance_us = 0;

    if (sdl_init(win_width, win_height, screenshot_prefix) < 0) {
        ERROR("sdl_init %dx%d failed\n", win_width, win_height);
        return -1;
    }

    sdl_get_state(&win_width, &win_height, NULL);

    sdl_init_pane(&title_pane_full, &title_pane, 
                  0, 0, 
                  win_width, FONT0_HEIGHT+4);
    sdl_init_pane(&cam_pane_full, &cam_pane, 
                  0, FONT0_HEIGHT+2, 
                  CAM_HEIGHT+4, CAM_HEIGHT+4); 
    sdl_init_pane(&data_pane_full, &data_pane, 
                  CAM_HEIGHT+2, FONT0_HEIGHT+2, 
                  win_width-(CAM_HEIGHT+2), 2*FONT1_HEIGHT+4); 
    sdl_init_pane(&summary_graph_pane_full, &summary_graph_pane, 
                  0, FONT0_HEIGHT+CAM_HEIGHT+4,
                  win_width, win_height-(FONT0_HEIGHT+CAM_HEIGHT+4));
    sdl_init_pane(&adc_data_graph_pane_full, &adc_data_graph_pane, 
                  CAM_HEIGHT+2,
                  FONT0_HEIGHT+2*FONT1_HEIGHT+4,
                  win_width - (CAM_HEIGHT+2),
                  CAM_HEIGHT+FONT0_HEIGHT+6 - (FONT0_HEIGHT+2*FONT1_HEIGHT+4));

    // loop until quit
    while (!quit) {
        // get the file_idx, and verify
        __sync_synchronize();
        file_idx = file_idx_global;
        __sync_synchronize();
        if (file_idx < 0 ||
            file_idx >= file_hdr->max ||
            file_data_part1[file_idx].magic != MAGIC_DATA_PART1) 
        {
            FATAL("invalid file_idx %d, max =%d\n",
                  file_idx, file_hdr->max);
        }
        DEBUG("file_idx %d\n", file_idx);

        // initialize for display update
        sdl_display_init();

        // draw pane borders   
        sdl_render_pane_border(&title_pane_full, GREEN);
        sdl_render_pane_border(&cam_pane_full,   GREEN);
        sdl_render_pane_border(&data_pane_full,  GREEN);
        sdl_render_pane_border(&summary_graph_pane_full, GREEN);
        sdl_render_pane_border(&adc_data_graph_pane_full, GREEN);

        // draw title line
        // 
        // 0         1         2         3         4         5         6         7        
        // 0123456789 123456789 123456789 123456789 123456789 123456789 123456789 
        // PLAYBACK_PAUSED  yy/mm/dd hh:mm:ss  LOST_CONN    FILE_ERROR   TIME_ERROR    ...  (?) (SHIFT-ESC)
        // ^                ^                  ^            ^            ^                  ^   ^         ^
        // 0                17                 36           49           62                -15  -11       -1

        if (mode == LIVE) {
            sdl_render_text(&title_pane, 0, 0, 0, "LIVE", GREEN, BLACK);
        } else {
            char playback_mode_str[50];
            if (playback_speed == 0) {
                sprintf(playback_mode_str, "PLAYBACK_PAUSED");
            } else {
                sprintf(playback_mode_str, "PLAYBACK_X%d", playback_speed);
            }
            sdl_render_text(&title_pane, 0, 0, 0, playback_mode_str, RED, BLACK);
        }
            
        t = file_data_part1[file_idx].time;
        tm = localtime(&t);
        sprintf(str, "%d/%d/%d %2.2d:%2.2d:%2.2d",
                tm->tm_year-100, tm->tm_mon+1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        sdl_render_text(&title_pane, 0, 17, 0, str, WHITE, BLACK);

        if (lost_connection) {
            sdl_render_text(&title_pane, 0, 36, 0, "LOST_CONN", RED, BLACK);
            lost_connection_msg_is_displayed = true;
        } else {
            lost_connection_msg_is_displayed = false;
        }

        if (file_error) {
            sdl_render_text(&title_pane, 0, 49, 0, "FILE_ERROR", RED, BLACK);
            file_error_msg_is_displayed = true;
        } else {
            file_error_msg_is_displayed = false;
        }

        if (time_error) {
            sdl_render_text(&title_pane, 0, 62, 0, "TIME_ERROR", RED, BLACK);
            time_error_msg_is_displayed = true;
        } else {
            time_error_msg_is_displayed = false;
        }

        sdl_render_text(&title_pane, 0, -11, 0, "(SHIFT-ESC)", WHITE, BLACK);

        sdl_render_text(&title_pane, 0, -15, 0, "(?)", WHITE, BLACK);
        
        // draw the camera image,
        draw_camera_image(&cam_pane, file_idx);

        // draw the data values,
        draw_data_values(&data_pane, file_idx);

        // draw the summary graph
        draw_summary_graph(&summary_graph_pane, file_idx);

        // draw the adc data graph
        draw_adc_data_graph(&adc_data_graph_pane, file_idx);

        // register for events   
        sdl_event_register(SDL_EVENT_KEY_SHIFT_ESC, SDL_EVENT_TYPE_KEY, NULL);       // quit (shift-esc key)
        sdl_event_register('?', SDL_EVENT_TYPE_KEY, NULL);                           // help
        sdl_event_register(SDL_EVENT_KEY_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);      // summary graph cursor 
        sdl_event_register(SDL_EVENT_KEY_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_CTRL_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_CTRL_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL); 
        sdl_event_register(SDL_EVENT_KEY_ALT_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_ALT_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_HOME, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_END, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('+', SDL_EVENT_TYPE_KEY, NULL);                           // summary graph x scale 
        sdl_event_register('=', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('-', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('s', SDL_EVENT_TYPE_KEY, NULL);                           // adc data graph select 
        sdl_event_register('1', SDL_EVENT_TYPE_KEY, NULL);                           // adc data graph y scale
        sdl_event_register('2', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('a', SDL_EVENT_TYPE_KEY, NULL);                           // camera image position & zoom
        sdl_event_register('d', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('w', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('x', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('z', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('Z', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('r', SDL_EVENT_TYPE_KEY, NULL);                             
        sdl_event_register('3', SDL_EVENT_TYPE_KEY, NULL);                           // adjust neutron pulse height thresh
        sdl_event_register('4', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('5', SDL_EVENT_TYPE_KEY, NULL);                           // adjust neutron summary graph scale
        sdl_event_register('6', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('>', SDL_EVENT_TYPE_KEY, NULL);                           // playback speed control
        sdl_event_register('<', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(',', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('.', SDL_EVENT_TYPE_KEY, NULL);

        // present the display
        sdl_display_present();

        // loop until
        // 1- quit flag is set, OR
        // 2- a message has changed state OR
        // 3- (there is no current event) AND
        //    ((at least one event has been processed) OR
        //     (file index that is currently displayed is not file_idx_global) OR
        //     (file max has changed))
        event_processed_count = 0;
        while (true) {
            // get and process event
            event_processed_count++;
            event = sdl_poll_event();
            switch (event->event) {
            case SDL_EVENT_QUIT: case SDL_EVENT_KEY_SHIFT_ESC: 
                quit = true;
                break;
            case SDL_EVENT_SCREENSHOT_TAKEN: {
                // set screen white for 1 second
                rect_t screen_pane, dummy_pane;
                rect_t rect = {0,0,win_width,win_height};
                sdl_init_pane(&screen_pane, &dummy_pane, 
                            0, 0, 
                            win_width, win_height);
                sdl_display_init();
                sdl_render_fill_rect(&screen_pane, &rect, WHITE);
                sdl_display_present();
                usleep(250000);
                break; }
            case '?':  
                sdl_display_text(about);
                SET_PLAYBACK_PAUSED;
                break;
            case SDL_EVENT_KEY_LEFT_ARROW:
            case SDL_EVENT_KEY_CTRL_LEFT_ARROW:
            case SDL_EVENT_KEY_ALT_LEFT_ARROW: {
                int32_t x = file_idx_global;
                x -= (event->event == SDL_EVENT_KEY_LEFT_ARROW      ? 1 :
                      event->event == SDL_EVENT_KEY_CTRL_LEFT_ARROW ? 10
                                                                    : 60);
                if (x < 0) {
                    x = 0;
                }
                file_idx_global = x;
                mode = PLAYBACK;
                SET_PLAYBACK_PAUSED;
                break; }
            case SDL_EVENT_KEY_RIGHT_ARROW:
            case SDL_EVENT_KEY_CTRL_RIGHT_ARROW:
            case SDL_EVENT_KEY_ALT_RIGHT_ARROW: {
                int32_t x = file_idx_global;
                x += (event->event == SDL_EVENT_KEY_RIGHT_ARROW      ? 1 :
                      event->event == SDL_EVENT_KEY_CTRL_RIGHT_ARROW ? 10
                                                                     : 60);
                if (x >= file_hdr->max) {
                    x = file_hdr->max - 1;
                    file_idx_global = x;
                    mode = initial_mode;
                } else {
                    file_idx_global = x;
                    mode = PLAYBACK;
                }
                SET_PLAYBACK_PAUSED;
                break; }
            case SDL_EVENT_KEY_HOME:
                file_idx_global = 0;
                mode = PLAYBACK;
                SET_PLAYBACK_PAUSED;
                break;
            case SDL_EVENT_KEY_END:
                file_idx_global = file_hdr->max - 1;
                mode = initial_mode;
                SET_PLAYBACK_PAUSED;
                break;
            case '-': case '+': case '=':
                draw_summary_graph_control(event->event);
                break;
            case 's': case '1': case '2':
                draw_adc_data_graph_control(event->event);
                break;
            case 'a': case 'd': case 'w': case 'x': case 'z': case 'Z': case 'r':
                draw_camera_image_control(event->event);
                break;
            case '3': case '4': {
                int32_t delta;
                delta = (neutron_pht_mv < 500  ? 1  :
                         neutron_pht_mv < 1000 ? 5  :
                         neutron_pht_mv < 2000 ? 10 : 
                         neutron_pht_mv < 3000 ? 20 : 
                                                 100);
                if (event->event == '3') {
                    neutron_pht_mv -= delta;
                    if (neutron_pht_mv < 0) {
                        neutron_pht_mv = 0;
                    }
                } else if (event->event == '4') {
                    neutron_pht_mv += delta;
                    if (neutron_pht_mv > 10000) {
                        neutron_pht_mv = 10000;
                    }
                } 
                break; }
            case '5': case '6': {
                int32_t delta;
                delta = (neutron_scale_cpm < 500  ? 1  :
                         neutron_scale_cpm < 1000 ? 5  :
                         neutron_scale_cpm < 2000 ? 10 : 
                         neutron_scale_cpm < 3000 ? 20 : 
                                                    100);
                if (event->event == '5') {
                    neutron_scale_cpm -= delta;
                    if (neutron_scale_cpm < 10) {
                        neutron_scale_cpm = 10;
                    }
                } else if (event->event == '6') {
                    neutron_scale_cpm += delta;
                    if (neutron_scale_cpm > 10000) {
                        neutron_scale_cpm = 10000;
                    }
                } 
                break; }
            case '>': case '.':
                if (mode == PLAYBACK && playback_speed < MAX_PLAYBACK_SPEED) {
                    playback_speed++;
                    playback_advance_us = microsec_timer() + 1000000 / playback_speed;
                }
                break;
            case '<': case ',':
                if (mode == PLAYBACK && playback_speed > 0) {
                    playback_speed--;
                    playback_advance_us = (playback_speed ? microsec_timer() + 1000000 / playback_speed : 0);
                }
                break;
            case SDL_EVENT_WIN_MINIMIZED:
                SET_PLAYBACK_PAUSED;
                break;
            case SDL_EVENT_WIN_SIZE_CHANGE:
            case SDL_EVENT_WIN_RESTORED:
                break;
            default:
                event_processed_count--;
                break;
            }

            // if in playback run mode then
            // check if it is time to increment file_idx_global
            if (mode == PLAYBACK && playback_speed > 0) {
                uint64_t curr_us = microsec_timer();
                if (curr_us > playback_advance_us) {
                    int32_t x = file_idx_global+1;
                    if (x >= file_hdr->max) {
                        file_idx_global = file_hdr->max - 1;
                        mode = initial_mode;
                        SET_PLAYBACK_PAUSED;
                    } else {
                        file_idx_global = x;
                        playback_advance_us = curr_us + 1000000 / playback_speed;
                    }
                }
            }

            // test if should break out of this loop
            if ((quit) ||
                (lost_connection != lost_connection_msg_is_displayed) ||
                (file_error != file_error_msg_is_displayed) ||
                (time_error != time_error_msg_is_displayed) ||
                ((event->event == SDL_EVENT_NONE) &&
                 ((event_processed_count > 0) ||
                  (file_idx != file_idx_global) ||
                  (file_hdr->max != file_max_last))))
            {
                file_max_last = file_hdr->max;
                break;
            }

            // delay 1 ms
            usleep(1000);
        }
    }

    // return success
    return 0;
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW CAMERA IMAGE  - - - - - - - - - - - - - - 

static void draw_camera_image(rect_t * cam_pane, int32_t file_idx)
{
    uint8_t             * pixel_buff;
    uint32_t              pixel_buff_width;
    uint32_t              pixel_buff_height;
    int32_t               ret;
    struct data_part2_s * data_part2;
    char                * errstr;
    uint32_t              skip_rows, skip_cols;

    static texture_t cam_texture = NULL;
    static int32_t image_size_last = 0;

    // if image_size has changed then reallocate cam_texture
    if (cam_texture == NULL || image_size != image_size_last) {
        sdl_destroy_texture(cam_texture);
        cam_texture = sdl_create_yuy2_texture(image_size,image_size);
        if (cam_texture == NULL) {
            FATAL("failed to create cam_texture\n");
        }
        image_size_last = image_size;
    }

    // if no jpeg buff then 
    //   display 'no image'
    //   return
    // endif
    if (file_data_part1[file_idx].data_part2_jpeg_buff_len == 0 ||
        (data_part2 = read_data_part2(file_idx)) == NULL)
    {
        errstr = "NO IMAGE";
        goto error;
    }
    
    // decode the jpeg buff contained in data_part2
    ret = jpeg_decode(0,  // cxid
                     JPEG_DECODE_MODE_YUY2,      
                     data_part2->jpeg_buff, file_data_part1[file_idx].data_part2_jpeg_buff_len,
                     &pixel_buff, &pixel_buff_width, &pixel_buff_height);
    if (ret < 0) {
        ERROR("jpeg_decode ret %d\n", ret);
        errstr = "DECODE";
        goto error;
    }
    if (pixel_buff_width != CAM_WIDTH || pixel_buff_height != CAM_HEIGHT) {
        ERROR("jpeg_decode wrong dimensions w=%d h=%d\n", pixel_buff_width, pixel_buff_height);
        errstr = "SIZE";
        goto error;
    }

    // display the decoded jpeg
    skip_cols = image_x - image_size/2;
    skip_rows = image_y - image_size/2;
    sdl_update_yuy2_texture(
        cam_texture, 
        pixel_buff + (skip_rows * CAM_WIDTH * 2) + (skip_cols * 2),
        CAM_WIDTH);
    sdl_render_texture(cam_texture, cam_pane);
    free(pixel_buff);

    // return
    return;

    // error  
error:
    sdl_render_text(cam_pane, 2, 1, 1, errstr, WHITE, BLACK);
    return;
}

static void draw_camera_image_control(char key)
{
    // adjust image_x. image_y, image_size
    switch (key) {
    case 'a': 
        image_x += 2;
        break;
    case 'd': 
        image_x -= 2;
        break;
    case 'w': 
        image_y += 2;
        break;
    case 'x': 
        image_y -= 2;
        break;
    case 'z': 
        image_size += 4;
        break;
    case 'Z': 
        image_size -= 4;
        break;
    case 'r':
        image_x    = atoi(DEFAULT_IMAGE_X);
        image_y    = atoi(DEFAULT_IMAGE_Y);
        image_size = atoi(DEFAULT_IMAGE_SIZE);
        break;
    default:
        FATAL("invalid key 0x%x\n", key);
        break;
    }

    // sanitize image_x. image_y, image_size
    if (image_size > CAM_HEIGHT) {
        image_size = CAM_HEIGHT;
    }
    if (image_size < 100) {
        image_size = 100;
    }
    if (image_x - image_size/2 < 0) {
        image_x = image_size/2;
    }
    if (image_x + image_size/2 >= CAM_WIDTH) {
        image_x = CAM_WIDTH - image_size/2;
    }
    if (image_y - image_size/2 < 0) {
        image_y = image_size/2;
    }
    if (image_y + image_size/2 >= CAM_HEIGHT) {
        image_y = CAM_HEIGHT - image_size/2;
    }
    DEBUG("sanitized image_x=%d image_y=%d image_size=%d\n", image_x, image_y, image_size);
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW DATA VALUES  - - - - - - - - - - - - - - 

static void draw_data_values(rect_t * data_pane, int32_t file_idx)
{
    struct data_part1_s * dp1;
    char str[200];

    dp1 = &file_data_part1[file_idx];

    sprintf(str, "%s   %s   %s   NPHT=%d MV",
            val2str(dp1->voltage_kv, UNITS_KV),
            val2str(dp1->current_ma, UNITS_MA),
            val2str(neutron_cpm(file_idx), UNITS_CPM),
            neutron_pht_mv);
    sdl_render_text(data_pane, 0, 0, 1, str, WHITE, BLACK);

    sprintf(str, "%s   %s",
            val2str(dp1->d2_pressure_mtorr, UNITS_D2_MT),
            val2str(dp1->n2_pressure_mtorr, UNITS_N2_MT));
    sdl_render_text(data_pane, 1, 0, 1, str, WHITE, BLACK);
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW SUMMARY GRAPH  - - - - - - - - - - - - 

// #define GRAPH_N2_PRESSURE

static void draw_summary_graph(rect_t * graph_pane, int32_t file_idx)
{
    #define MAX_TIME_SPAN  5000

    float    voltage_kv_values[MAX_TIME_SPAN];
    float    current_ma_values[MAX_TIME_SPAN];
    float    d2_pressure_mtorr_values[MAX_TIME_SPAN];
#ifdef GRAPH_N2_PRESSURE
    float    n2_pressure_mtorr_values[MAX_TIME_SPAN];
#endif
    float    neutron_cpm_values[MAX_TIME_SPAN];
    int32_t  file_idx_start, file_idx_end, max_values, i;
    uint64_t cursor_time_us;
    float    cursor_pos;
    char     x_info_str[100];
    char     cursor_str[100];

    // init x_info_str 
    sprintf(x_info_str, "X: %d SEC", summary_graph_time_span_sec);

    // init file_idx_start & file_idx_end
    if (mode == LIVE) {
        file_idx_end   = file_idx;
        file_idx_start = file_idx_end - (summary_graph_time_span_sec - 1);
    } else {
        file_idx_start = file_idx - summary_graph_time_span_sec / 2;
        file_idx_end   = file_idx_start + summary_graph_time_span_sec - 1;
    }

    // init cursor position and string
    cursor_time_us = file_data_part1[file_idx].time * 1000000;
    cursor_pos = (float)(file_idx - file_idx_start) / (summary_graph_time_span_sec - 1);
    time2str(cursor_str, cursor_time_us, false, false, false);

    // init arrays of the values to graph
    max_values = 0;
    for (i = file_idx_start; i <= file_idx_end; i++) {
        voltage_kv_values[max_values]        = (i >= 0 && i < file_hdr->max)
                                               ? file_data_part1[i].voltage_kv
                                               : ERROR_NO_VALUE;
        current_ma_values[max_values]        = (i >= 0 && i < file_hdr->max)
                                                ? file_data_part1[i].current_ma      
                                                : ERROR_NO_VALUE;
        neutron_cpm_values[max_values]       = (i >= 0 && i < file_hdr->max)
                                                ? neutron_cpm(i)
                                                : ERROR_NO_VALUE;
        d2_pressure_mtorr_values[max_values] = (i >= 0 && i < file_hdr->max)
                                                ? file_data_part1[i].d2_pressure_mtorr
                                                : ERROR_NO_VALUE;
#ifdef GRAPH_N2_PRESSURE
        n2_pressure_mtorr_values[max_values] = (i >= 0 && i < file_hdr->max)
                                                ? file_data_part1[i].n2_pressure_mtorr
                                                : ERROR_NO_VALUE;
#endif
        max_values++;
    }

    // draw the graph
    i = file_idx - file_idx_start;
    double ns = neutron_scale_cpm;
#ifdef GRAPH_N2_PRESSURE
    draw_graph_common(
        graph_pane, 
        "SUMMARY", 
        1200,   
        6, 
        x_info_str, NULL, 
        cursor_pos, cursor_str, 
        5,
        val2str(voltage_kv_values[i],UNITS_KV),           RED,             50., max_values, voltage_kv_values,
        val2str(current_ma_values[i],UNITS_MA),           GREEN,           50., max_values, current_ma_values,
        val2str(neutron_cpm_values[i],UNITS_CPM),         PURPLE,           ns, max_values, neutron_cpm_values,
        val2str(d2_pressure_mtorr_values[i],UNITS_D2_MT), BLUE,           100., max_values, d2_pressure_mtorr_values,
        val2str(n2_pressure_mtorr_values[i],UNITS_N2_MT), LIGHT_BLUE, 1000000., max_values, n2_pressure_mtorr_values);
#else
    draw_graph_common(
        graph_pane, 
        "SUMMARY", 
        1200,   
        6, 
        x_info_str, NULL, 
        cursor_pos, cursor_str, 
        4,
        val2str(voltage_kv_values[i],UNITS_KV),           RED,             50., max_values, voltage_kv_values,
        val2str(current_ma_values[i],UNITS_MA),           GREEN,           50., max_values, current_ma_values,
        val2str(neutron_cpm_values[i],UNITS_CPM),         PURPLE,           ns, max_values, neutron_cpm_values,
        val2str(d2_pressure_mtorr_values[i],UNITS_D2_MT), BLUE,           100., max_values, d2_pressure_mtorr_values);
#endif
}

static void draw_summary_graph_control(char key)
{
    // if changing this, also change MAX_TIME_SPAN above
    //                                       1    5   10    20    30    60  minutes
    static uint32_t time_span_sec_tbl[] = { 60, 300, 600, 1200, 1800, 3600 };

    switch (key) {
    case '-': 
        REDUCE(summary_graph_time_span_sec, time_span_sec_tbl);
        break;
    case '+': case '=':
        INCREASE(summary_graph_time_span_sec, time_span_sec_tbl);
        break;
    default:
        FATAL("invalid key 0x%x\n", key);
        break;
    }
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW ADC DATA GRAPH - - - - - - - - - - - - 

static void draw_adc_data_graph(rect_t * graph_pane, int32_t file_idx)
{
    struct data_part1_s * dp1;
    struct data_part2_s * dp2;
    float adc_data[MAX_ADC_DATA];
    int32_t i, j, k, color;
    int32_t sum=0, cnt=0;
    char title_str[100];

    // init pointer to dp1, and read dp2
    dp1 = &file_data_part1[file_idx];
    dp2 = read_data_part2(file_idx);
    if (dp2 == NULL) {
        ERROR("failed read data part2\n");
    }

    // preset adc_data to NO_VALUE
    for (i = 0; i < MAX_ADC_DATA; i++) {
        adc_data[i] = ERROR_NO_VALUE;
    }

    // init array of the values to graph
    switch (adc_data_graph_select) {
    case 0:
        if (dp2) {
            k = 0;
            for (i = 0; i < dp1->max_neutron_pulse; i++) {
                if (dp1->neutron_pulse_mv[i] >= neutron_pht_mv) {
                    // copy pulse data to adc_data array (to be plotted)
                    for (j = 0; j < MAX_NEUTRON_ADC_PULSE_DATA; j++) {
                        adc_data[k++] = dp2->neutron_adc_pulse_data[i][j];
                        if (k == MAX_ADC_DATA) {
                            break;
                        }
                    }
                    if (k == MAX_ADC_DATA) {
                        break;
                    }

                    // insert gap in adc_data array, between pulses
                    for (j = 0; j < MAX_NEUTRON_ADC_PULSE_DATA; j++) {
                        adc_data[k++] = ERROR_NO_VALUE;
                        if (k == MAX_ADC_DATA) {
                            break;
                        }
                    }
                    if (k == MAX_ADC_DATA) {
                        break;
                    }
                }
            }
        }
        sprintf(title_str, "NEUTRON ADC");
        color = PURPLE;
        break;
    case 1:
        if (dp2 && dp1->data_part2_voltage_adc_data_valid) {
            for (i = 0; i < MAX_ADC_DATA; i++) {
                adc_data[i] = dp2->voltage_adc_data[i];
                sum += adc_data[i];
                cnt++;
            }
        }
        sprintf(title_str, "VOLTAGE ADC");
        color = RED;
        break;
    case 2:
        if (dp2 && dp1->data_part2_current_adc_data_valid) {
            for (i = 0; i < MAX_ADC_DATA; i++) {
                adc_data[i] = dp2->current_adc_data[i];
                sum += adc_data[i];
                cnt++;
            }
        }
        sprintf(title_str, "CURRENT ADC");
        color = GREEN;
        break;
    case 3:
        if (dp2 && dp1->data_part2_pressure_adc_data_valid) {
            for (i = 0; i < MAX_ADC_DATA; i++) {
                adc_data[i] = dp2->pressure_adc_data[i];
                sum += adc_data[i];
                cnt++;
            }
        }
        sprintf(title_str, "PRESSURE ADC");
        color = BLUE;
        break;
    default:
        FATAL("invalid adc_data_graph_select = %d\n", adc_data_graph_select);
        break;
    }

    // append Y scale to title_str
    sprintf(title_str+strlen(title_str), " : Y %d MV", adc_data_graph_max_y_mv);

    // append average to title_str  (if applicable)
    if (cnt > 0) {
        sprintf(title_str+strlen(title_str), " : AVG %d MV", sum / cnt);
    }

    // draw the graph
    draw_graph_common(
        graph_pane,        // the pane
        title_str,         // title_str
        1200,              // x_range
        -8,                // str_col
        "1 SECOND", NULL,  // x_info_str, y_info_str
        -1, NULL,          // cursor_pos, cursor_str
        1,                 // max_graph
        NULL, color, (double)adc_data_graph_max_y_mv, MAX_ADC_DATA, adc_data); 
                           // name,color,y_max,max_values,values
}

static void draw_adc_data_graph_control(char key)
{
    static uint32_t  max_y_mv_tbl[] = {100, 200, 500, 1000, 2000, 5000, 10000};

    switch (key) {
    case 's':
        adc_data_graph_select = ((adc_data_graph_select + 1) % 4);
        break;
    case '1':
        REDUCE(adc_data_graph_max_y_mv, max_y_mv_tbl);
        break;
    case '2':
        INCREASE(adc_data_graph_max_y_mv, max_y_mv_tbl);
        break;
    default:
        FATAL("invalid key 0x%x\n", key);
        break;
    }
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW GRAPH COMMON   - - - - - - - - - - - - 

static void draw_graph_common(
    rect_t * graph_pane,   // the pane    
    char * title_str,      // OPTIONAL graph title, displayed on top line centered above x axis
    int32_t x_range_param, // length of the X axis, in pixels
    int32_t str_col,       // controls the loc of the g->name, and x/y_info_str; 0 is at right end of X axis
    char * x_info_str,     // OPTIONAL x axis information
    char * y_info_str,     // OPTIONAL y axis information
    float cursor_pos,      // OPTIONAL cursor position, in X axis pixels;  -1 if not used
    char * cursor_str,     // OPTIONAL string placed under the cursor
    int32_t max_graph,     // number of graphs that follow in the varargs
    ...)
{
    // variable arg list ...
    // - char * name
    // - int32_t color
    // - double  y_max     
    // - int32_t max_values
    // - float * values
    // - ... repeat for next graph ...

    va_list ap;
    rect_t   rect;
    int32_t  i;
    int32_t  gridx;
    int32_t  cursor_x;
    int32_t  title_str_col;
    int32_t  info_str_col;
    int32_t  cursor_str_col;
    int32_t  name_str_col;
    char     name_str[100];
    int32_t  x_origin;
    int32_t  x_range;
    int32_t  y_origin;
    int32_t  y_range;

    struct graph_s {
        char    * name;
        int32_t   color;
        float     y_max;
        int32_t   max_values;
        float   * values;
    } graph[20];

    // get the variable args 
    va_start(ap, max_graph);
    for (i = 0; i < max_graph; i++) {
        graph[i].name       = va_arg(ap, char*);
        graph[i].color      = va_arg(ap, int32_t);
        graph[i].y_max      = va_arg(ap, double);
        graph[i].max_values = va_arg(ap, int32_t);
        graph[i].values     = va_arg(ap, float*);
    }
    va_end(ap);

    x_origin = 10;
    x_range  = x_range_param;
    y_origin = graph_pane->h - FONT0_HEIGHT - 4;
    y_range  = graph_pane->h - FONT0_HEIGHT - 4 - FONT0_HEIGHT;

    // fill white
    rect.x = 0;
    rect.y = 0;
    rect.w = graph_pane->w;
    rect.h = graph_pane->h;
    sdl_render_fill_rect(graph_pane, &rect, WHITE);

    for (gridx = 0; gridx < max_graph; gridx++) {
        #define MAX_POINTS 1000
        #define LINE_WIDTH 2

        struct graph_s * g;
        float            x;
        float            x_pixels_per_val;
        float            y_scale_factor;
        int32_t          y_limit1;
        int32_t          y_limit2;
        int32_t          max_points;

        int32_t          i, lw;
        point_t          points[MAX_POINTS];
        point_t        * p;

        // loop over LINE_WIDTH
        for (lw = 0; lw < LINE_WIDTH; lw++) {
            // init for graph[gridx]
            g                 = &graph[gridx];
            x                 = (float)x_origin;
            x_pixels_per_val  = (float)x_range / g->max_values;
            y_scale_factor    = (float)y_range / g->y_max;
            y_limit1          = y_origin - y_range;
            y_limit2          = y_origin + FONT0_HEIGHT;
            max_points        = 0;

            // draw the graph lines
            for (i = 0; i < g->max_values; i++) {
                if (!IS_ERROR(g->values[i])) {
                    p = &points[max_points];
                    p->x = x;
                    p->y = y_origin - y_scale_factor * g->values[i];
                    if (p->y < y_limit1) {
                        p->y = y_limit1;
                    }
                    if (p->y > y_limit2) {
                        p->y = y_limit2;
                    }
                    p->y += lw;
                    max_points++;
                    if (max_points == MAX_POINTS) {
                        sdl_render_lines(graph_pane, points, max_points, g->color);
                        points[0].x = points[max_points-1].x;
                        points[0].y = points[max_points-1].y;
                        max_points = 1;
                    }
                } else if (max_points > 0) {
                    sdl_render_lines(graph_pane, points, max_points, g->color);
                    max_points = 0;
                }

                x += x_pixels_per_val;
            }
            if (max_points) {
                sdl_render_lines(graph_pane, points, max_points, g->color);
            }
        }

        // draw the graph name
        if (g->name) {
            name_str_col = (x_range + x_origin) / FONT0_WIDTH + str_col;    
            if (g->y_max >= 1) {
                sprintf(name_str, "%-13s  (%.0f MAX)", g->name, g->y_max);
            } else {
                sprintf(name_str, "%-13s  (%.2f MAX)", g->name, g->y_max);
            }
            sdl_render_text(graph_pane, gridx+1, name_str_col, 0, 
                            name_str, g->color, WHITE);
        }
    }

    // draw x axis
    sdl_render_line(graph_pane, 
                    x_origin, y_origin+1, 
                    x_origin+x_range, y_origin+1,
                    BLACK);
    sdl_render_line(graph_pane, 
                    x_origin, y_origin+2, 
                    x_origin+x_range, y_origin+2,
                    BLACK);
    sdl_render_line(graph_pane, 
                    x_origin, y_origin+3, 
                    x_origin+x_range, y_origin+3,
                    BLACK);

    // draw y axis
    sdl_render_line(graph_pane, 
                    x_origin-1, y_origin+3, 
                    x_origin-1, y_origin-y_range,
                    BLACK);
    sdl_render_line(graph_pane, 
                    x_origin-2, y_origin+3, 
                    x_origin-2, y_origin-y_range,
                    BLACK);
    sdl_render_line(graph_pane, 
                    x_origin-3, y_origin+3, 
                    x_origin-3, y_origin-y_range,
                    BLACK);

    // draw cursor, and cursor_str
    if (cursor_pos >= 0) {
        cursor_x = x_origin + cursor_pos * (x_range - 1);
        sdl_render_line(graph_pane,
                        cursor_x-1, y_origin,
                        cursor_x-1, y_origin-y_range,
                        PURPLE);
        sdl_render_line(graph_pane,
                        cursor_x, y_origin,
                        cursor_x, y_origin-y_range,
                        PURPLE);
        if (cursor_str != NULL) {
            cursor_str_col = cursor_x/FONT0_WIDTH - strlen(cursor_str)/2;
            sdl_render_text(graph_pane,
                            -1, cursor_str_col,
                            0, cursor_str, PURPLE, WHITE);
        }
    }

    // draw title_str, and info_str
    if (title_str != NULL) {
        title_str_col = (x_origin + x_range/2) / FONT0_WIDTH - strlen(title_str)/2;
        sdl_render_text(graph_pane,
                        0, title_str_col,
                        0, title_str, BLACK, WHITE);
    }
    if (x_info_str != NULL) {
        info_str_col = (x_range + x_origin) / FONT0_WIDTH + str_col;    
        sdl_render_text(graph_pane,
                        -1, info_str_col,
                        0, x_info_str, BLACK, WHITE);
    }
    if (y_info_str != NULL) {
        info_str_col = (x_range + x_origin) / FONT0_WIDTH + str_col;    
        sdl_render_text(graph_pane,
                        -2, info_str_col,
                        0, y_info_str, BLACK, WHITE);
    }
}

// -----------------  GENERATE TEST FILE----------------------------------------------

static int32_t generate_test_file(void) 
{
    time_t                t;
    uint8_t               jpeg_buff[200000];
    uint32_t              jpeg_buff_len;
    uint64_t              dp2_offset;
    int32_t               len, idx, i, fd;
    struct data_part1_s * dp1;
    struct data_part2_s * dp2;

    INFO("starting ...\n");

    // init
    t = time(NULL);
    dp2_offset = FILE_DATA_PART2_OFFSET;
    dp2 = calloc(1, MAX_DATA_PART2_LENGTH);
    if (dp2 == NULL) {
        FATAL("calloc\n");
    }

    // init jpeg_buff from jpeg_buff_sample.bin file, if it exists
    fd = open(JPEG_BUFF_SAMPLE_FILENAME, O_RDONLY);
    if (fd < 0) {
        WARN("open %s, %s\n", JPEG_BUFF_SAMPLE_FILENAME, strerror(errno));
        jpeg_buff_len = 0;
    } else {
        jpeg_buff_len = read(fd, jpeg_buff, sizeof(jpeg_buff));
        if (jpeg_buff_len <= 0) {
            WARN("read %s len=%d, %s\n", JPEG_BUFF_SAMPLE_FILENAME, jpeg_buff_len, strerror(errno));
            jpeg_buff_len = 0;
        }
        close(fd);
    }

    // file data
    for (idx = 0; idx < test_file_secs; idx++) {
        // data part1
        dp1 = &file_data_part1[idx];
        dp1->magic = MAGIC_DATA_PART1;
        dp1->time  = t + idx;

        dp1->voltage_kv = 30.0 * idx / test_file_secs;
        dp1->current_ma = 0;
        dp1->d2_pressure_mtorr = 10;
        dp1->n2_pressure_mtorr = 13;
        for (i = 0; i < 100; i++) {
            dp1->neutron_pulse_mv[i] = 50 + 5 * i;   // pulse height in mv
        }
        dp1->max_neutron_pulse = 100;

        dp1->data_part2_offset = dp2_offset;
        dp1->data_part2_length = sizeof(struct data_part2_s) + jpeg_buff_len;
        dp1->data_part2_jpeg_buff_len = jpeg_buff_len;
        dp1->data_part2_voltage_adc_data_valid = true;
        dp1->data_part2_current_adc_data_valid = true;
        dp1->data_part2_pressure_adc_data_valid = true;

        // data part2
        dp2->magic = MAGIC_DATA_PART2;
        for (i = 0; i < MAX_ADC_DATA; i++) {
            dp2->voltage_adc_data[i]  = 10000 * i / MAX_ADC_DATA;
            dp2->current_adc_data[i]  =  5000 * i / MAX_ADC_DATA;
            dp2->pressure_adc_data[i] =  1000 * i / MAX_ADC_DATA;
        }
        for (i = 0; i < dp1->max_neutron_pulse; i++) {
            dp2->neutron_adc_pulse_data[i][MAX_NEUTRON_ADC_PULSE_DATA/2+0] = dp1->neutron_pulse_mv[i];
            dp2->neutron_adc_pulse_data[i][MAX_NEUTRON_ADC_PULSE_DATA/2+1] = dp1->neutron_pulse_mv[i] / 2;
            dp2->neutron_adc_pulse_data[i][MAX_NEUTRON_ADC_PULSE_DATA/2+2] = dp1->neutron_pulse_mv[i] / 4;
        }
        memcpy(dp2->jpeg_buff, jpeg_buff, jpeg_buff_len);

        len = pwrite(file_fd, dp2, dp1->data_part2_length, dp2_offset);
        if (len != dp1->data_part2_length) {
            ERROR("write data_part2 len=%d exp=%d, %s\n",
                  len, dp1->data_part2_length, strerror(errno));
            return -1;
        }

        // update dp2_offset
        dp2_offset += dp1->data_part2_length;

        // print progress
        if (idx && (idx % 1000) == 0) {
            INFO("  completed %d\n", idx);
        }
    }

    // file hdr
    file_hdr->max = test_file_secs;

    // return success
    INFO("done\n");
    return 0;
}

// -----------------  SUPPORT  ------------------------------------------------------ 

static char * val2str(float val, int32_t units)
{
    static char str_tbl[20][100];
    static uint32_t idx;
    char *str;
    const char *fmt;
    const char *units_str;

    // determine fmt and units_str from units arg
    switch (units) {
    case UNITS_KV:
        units_str = " KV";
        fmt = "%0.1f";
        break;
    case UNITS_MA:
        units_str = " MA";
        fmt = "%0.1f";
        break;
    case UNITS_CPM:
        units_str = " CPM";
        fmt = "%0.1f";
        break;
    case UNITS_D2_MT:
    case UNITS_N2_MT:
        units_str = (units == UNITS_N2_MT ? " N2-MT" : " D2-MT");
        if (IS_ERROR(val)) {
            fmt = "notused";
        } else if (val < 1000) {
            fmt = "%0.1f";
        } else {
            val /= 1000;
            fmt = (val < 10 ? "%0.2fK" : val < 100 ? "%0.1fK" : "%0.0fK");
        }
        break;
    default:
        FATAL("invalid units arg %d\n", units);
        break;
    }

    // pick a static str to use 
    str = str_tbl[idx];
    idx = (idx + 1) % 20;

    // start the return string with the value
    if (IS_ERROR(val)) {
        sprintf(str, "%s", ERROR_TEXT(val));
    } else {
        sprintf(str, fmt, val);
    }

    // append units_str to the end of str
    strcat(str, units_str);

    // return the str
    return str;
}

struct data_part2_s * read_data_part2(int32_t file_idx)
{
    int32_t  dp2_length;
    off_t    dp2_offset;
    int32_t  len;

    static int32_t               last_read_file_idx = -1;
    static struct data_part2_s * last_read_data_part2 = NULL;

    // initial allocate 
    if (last_read_data_part2 == NULL) {
        last_read_data_part2 = calloc(1,MAX_DATA_PART2_LENGTH);
        if (last_read_data_part2 == NULL) {
            FATAL("calloc");
        }
    }

    // if file_idx is same as last read then return data_part2 from last read
    if (file_idx == last_read_file_idx) {
        DEBUG("return cached, file_idx=%d\n", file_idx);
        return last_read_data_part2;
    }

    // verify data_part2 exists for specified file_idx
    if ((dp2_length = file_data_part1[file_idx].data_part2_length) == 0 ||
        (dp2_offset = file_data_part1[file_idx].data_part2_offset) == 0)
    {
        return NULL;
    }

    // read data_part2
    len = pread(file_fd, last_read_data_part2, dp2_length, dp2_offset);
    if (len != dp2_length) {
        ERROR("read data_part2 len=%d exp=%d, %s\n",
              len, dp2_length, strerror(errno));
        return NULL;
    }

    // verify magic value in data_part2
    if (last_read_data_part2->magic != MAGIC_DATA_PART2) {
        FATAL("invalid data_part2 magic 0x%"PRIx64" at file_idx %d\n", 
              last_read_data_part2->magic, file_idx);
    }

    // remember the file_idx of this read, and
    // return the data_part2
    DEBUG("return new read data, file_idx=%d\n", file_idx);
    last_read_file_idx = file_idx;
    return last_read_data_part2;
}

static float neutron_cpm(int32_t file_idx)
{
    #define AVG_SAMPLES 10

    int32_t file_idx_avg_start;
    int32_t file_idx_avg_end;

    static int32_t neutron_pht_mv_cache = -1;
    static int32_t neutron_cps_cache[MAX_FILE_DATA_PART1];
    static float   neutron_cpm_average_cache[MAX_FILE_DATA_PART1];

    // init the start and end of the range to be averaged
    file_idx_avg_start = file_idx - (AVG_SAMPLES-1);
    file_idx_avg_end   = file_idx;

    // if neutron_pht_mv has changed then clear cached results
    if (neutron_pht_mv != neutron_pht_mv_cache) {
        int32_t i;
        for (i = 0; i < MAX_FILE_DATA_PART1; i++) {
            neutron_cps_cache[i] = -1;
            neutron_cpm_average_cache[i] = -1;
        }
        neutron_pht_mv_cache = neutron_pht_mv;
    }

    // if file_idx out of range then return error
    if (file_idx_avg_start < 0 || file_idx_avg_end >= file_hdr->max) {
        return ERROR_NO_VALUE;
    }

    // if cached average cpm value is not available then
    // compute it
    if (neutron_cpm_average_cache[file_idx] == -1) {
        int32_t sum = 0, n = 0, i, j, cps;

        // loop over range to average
        for (i = file_idx_avg_start; i <= file_idx_avg_end; i++) {
            // if cached cps value is not available then compute it
            // based on current setting of neutron_pht_mv
            if (neutron_cps_cache[i] == -1) {
                struct data_part1_s * dp1 = &file_data_part1[i];

                // sanity check dp1->magic
                if (dp1->magic != MAGIC_DATA_PART1) {
                    ERROR("dp1->magix 0x%"PRIx64" is not valid\n", dp1->magic);
                    return ERROR_NO_VALUE;
                }

                // count the number of pulses which have height greater or
                // equal to the pulse-height-threshold
                cps = 0;
                for (j = 0; j < dp1->max_neutron_pulse; j++) {
                    if (dp1->neutron_pulse_mv[j] >= neutron_pht_mv) {
                        cps++;
                    }
                }

                // save the result in the neutron_cps_cache
                neutron_cps_cache[i] = cps;
            }

            // sum the cached cps values
            sum += neutron_cps_cache[i];
            n++;
        }

        // compute cached average cpm value
        neutron_cpm_average_cache[file_idx] = (float)sum / n * 60;
    }

    // return the cached average cpm value
    return neutron_cpm_average_cache[file_idx];
}
