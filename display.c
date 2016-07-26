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
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

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

#define MAX_FILE_DATA_PART1   86400   // 1 day
#define MAX_DATA_PART2_LENGTH 1000000
#define MAX_GRAPH             8

#define FILE_DATA_PART2_OFFSET \
   ((sizeof(file_hdr_t) +  \
     sizeof(struct data_part1_s) * MAX_FILE_DATA_PART1 + \
     0x1000) & ~0xfffL)

#define FONT0_HEIGHT (sdl_font_char_height(0))
#define FONT0_WIDTH  (sdl_font_char_width(0))
#define FONT1_HEIGHT (sdl_font_char_height(1))
#define FONT1_WIDTH  (sdl_font_char_width(1))

#define DEFAULT_WIN_WIDTH   1920
#define DEFAULT_WIN_HEIGHT  1000

//#define JPEG_BUFF_SAMPLE_CREATE_ENABLE
#define JPEG_BUFF_SAMPLE_FILENAME "jpeg_buff_sample.bin"

#define NEUTRON_CPM(_file_idx) (file_data_part1[_file_idx].he3.cpm_10_sec[0])

//
// typedefs
//

enum mode {LIVE, PLAYBACK, TEST};

typedef struct {
    uint64_t magic;
    uint64_t start_time;
    uint32_t max;
    uint8_t  reserved[4096-20];
} file_hdr_t;

//
// variables
//

static enum mode                mode;
static bool                     initially_live_mode;
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

static int32_t                  graph_x_origin;
static int32_t                  graph_x_range;
static int32_t                  graph_y_origin;
static int32_t                  graph_y_range;
static rect_t                   graph_pane_global;
static int32_t                  graph_select;
static int32_t                  graph_x_time_span_sec;
static int32_t                  graph_y_max_mv;
static float                    graph_y_max_cpm;

static int32_t                  test_file_secs;

static uint8_t                  jpeg_buff[1000000];
static int32_t                  jpeg_buff_len;
static uint64_t                 jpeg_buff_us;
static pthread_mutex_t          jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;

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
static void draw_data_values(rect_t * data_pane, int32_t file_idx);
static void draw_graph_init(rect_t * graph_pane);
static void draw_graph_scale_select(char key);
static void draw_graph0(int32_t file_idx);
static void draw_graph1(int32_t file_idx);
static void draw_graph2(int32_t file_idx);
static void draw_graph34567(int32_t file_idx);
static void draw_graph_common(char * title_str, char * x_info_str, char * y_info_str, float cursor_pos, char * cursor_str, int32_t max_graph, ...);
static int32_t generate_test_file(void);
static char * val2str(float val);
static char * val2str2(float val, char * trailer_str);
static struct data_part2_s * read_data_part2(int32_t file_idx);
static int getsockaddr(char * node, int port, int socktype, int protcol, struct sockaddr_in * ret_addr);
static char * sock_addr_to_str(char * s, int slen, struct sockaddr * addr);

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

static int32_t initialize(int32_t argc, char ** argv)
{
    struct rlimit      rl;
    pthread_t          thread;
    int32_t            ret, len;
    char               filename[100];
    char               servername[100];
    char               s[100];
    int32_t            wait_ms;

    // use line bufferring
    setlinebuf(stdout);

    // init core dumps
    // note - requires fs.suid_dumpable=1  in /etc/sysctl.conf if this is a suid pgm
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    // check size of data struct on 64bit linux and rpi
    DEBUG("sizeof data_t=%zd part1=%zd part2=%zd\n",
           sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));

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
    file_fd = open(filename, O_RDWR);
    if (file_fd < 0) {
        ERROR("failed to open %s, %s\n", filename, strerror(errno));
        return -1;
    }
    file_hdr = mmap(NULL,  // addr
                    sizeof(file_hdr_t),
                    PROT_READ|PROT_WRITE,
                    MAP_SHARED,
                    file_fd,
                    0);   // offset
    if (file_hdr == MAP_FAILED) {
        ERROR("failed to map file_hdr %s, %s\n", filename, strerror(errno));
        return -1;
    }
    file_data_part1 = mmap(NULL,  // addr
                           sizeof(struct data_part1_s) * MAX_FILE_DATA_PART1,
                           PROT_READ|PROT_WRITE,
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
        ret =  getsockaddr(servername, PORT, SOCK_STREAM, 0, &server_sockaddr);
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
        if (file_data_part1[0].magic != MAGIC_DATA_PART1) {
            ERROR("no data in file %s (0x%"PRIx64"\n", filename, file_data_part1[0].magic);
            return -1;
        }
        file_idx_global = 0;
    }

    // set initially_live_mode flag
    initially_live_mode = (mode == LIVE);

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

// -----------------  GET LIVE DATA THREAD  ------------------------------------------

static void * get_live_data_thread(void * cx)
{
    int32_t               sfd, len, chan;
    struct timeval        rcvto;
    data_t              * data;
    struct data_part1_s * dp1;
    struct data_part2_s * dp2;
    uint64_t              last_data_time_written_to_file;
    uint64_t              t, time_now, time_delta;
    he3_t                 he3_novalue;
    data_t                data_novalue;

    // init data_novalue
    for (chan = 0; chan < MAX_HE3_CHAN; chan++) {
        he3_novalue.cpm_1_sec[chan] = ERROR_NO_VALUE;
        he3_novalue.cpm_10_sec[chan] = ERROR_NO_VALUE;
        he3_novalue.cpm_60_sec[chan] = ERROR_NO_VALUE;
        he3_novalue.cpm_600_sec[chan] = ERROR_NO_VALUE;
        he3_novalue.cpm_3600_sec[chan] = ERROR_NO_VALUE;
    }

    dp1                                           = &data_novalue.part1;
    dp1->magic                                    = MAGIC_DATA_PART1;
    dp1->time                                     = 0;
    dp1->voltage_mean_kv                          = ERROR_NO_VALUE;
    dp1->voltage_min_kv                           = ERROR_NO_VALUE;
    dp1->voltage_max_kv                           = ERROR_NO_VALUE;
    dp1->current_ma                               = ERROR_NO_VALUE;
    dp1->pressure_d2_mtorr                        = ERROR_NO_VALUE;
    dp1->pressure_n2_mtorr                        = ERROR_NO_VALUE;
    dp1->he3                                      = he3_novalue;
    dp1->data_part2_offset                        = 0;
    dp1->data_part2_length                        = sizeof(struct data_part2_s);
    dp1->data_part2_jpeg_buff_valid               = false;
    dp1->data_part2_voltage_adc_samples_mv_valid  = false;
    dp1->data_part2_current_adc_samples_mv_valid  = false;
    dp1->data_part2_pressure_adc_samples_mv_valid = false;
    dp1->data_part2_he3_adc_samples_mv_valid      = false;

    dp2                                           = &data_novalue.part2;
    dp2->magic                                    = MAGIC_DATA_PART2;
    dp2->jpeg_buff_len                            = 0;

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
        len = recv(sfd, dp1, sizeof(struct data_part1_s), MSG_WAITALL);
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
        len = recv(sfd, dp2, dp1->data_part2_length, MSG_WAITALL);
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
        if (!dp1->data_part2_jpeg_buff_valid) {
            pthread_mutex_lock(&jpeg_mutex);
            if (microsec_timer() - jpeg_buff_us < 1000000) {
                memcpy(dp2->jpeg_buff, jpeg_buff, jpeg_buff_len);
                dp2->jpeg_buff_len = jpeg_buff_len;
                dp1->data_part2_jpeg_buff_valid = true;
                dp1->data_part2_length = sizeof(struct data_part2_s) + jpeg_buff_len;
            }
            pthread_mutex_unlock(&jpeg_mutex);
        }

        // if opt_no_cam then disacard camera data
        if (opt_no_cam) {
            dp2->jpeg_buff_len = 0;
            dp1->data_part2_jpeg_buff_valid = false;
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
        if (!sample_written && dp1->data_part2_jpeg_buff_valid) {
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
    bool          quit;
    sdl_event_t * event;
    rect_t        title_pane_full, title_pane; 
    rect_t        cam_pane_full, cam_pane;
    rect_t        data_pane_full, data_pane;
    rect_t        graph_pane_full, graph_pane;
    char          str[100];
    struct tm   * tm;
    time_t        t;
    int32_t       file_idx;
    int32_t       event_processed_count;
    int32_t       file_max_last;
    int32_t       tmp;
    bool          lost_connection_msg_is_displayed;
    bool          file_error_msg_is_displayed;
    bool          time_error_msg_is_displayed;
    uint64_t      screenshot_time_us;
    bool          screenshot_msg;
    bool          screenshot_msg_is_displayed;

    #define MAX_SCREENSHOT_US 1000000

    // initializae 
    quit = false;
    file_max_last = -1;
    lost_connection_msg_is_displayed = false;
    file_error_msg_is_displayed = false;
    time_error_msg_is_displayed = false;
    screenshot_time_us = 0;
    screenshot_msg = false;
    screenshot_msg_is_displayed = false;

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
                  win_width-(CAM_HEIGHT+2), CAM_HEIGHT+4); 
    sdl_init_pane(&graph_pane_full, &graph_pane, 
                  0, FONT0_HEIGHT+CAM_HEIGHT+4,
                  win_width, win_height-(FONT0_HEIGHT+CAM_HEIGHT+4));

    draw_graph_init(&graph_pane);

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
        sdl_render_pane_border(&graph_pane_full, GREEN);

        // draw title line
        if (mode == LIVE) {
            sdl_render_text(&title_pane, 0, 0, 0, "LIVE", GREEN, BLACK);
        } else {
            sdl_render_text(&title_pane, 0, 0, 0, "PLAYBACK", RED, BLACK);
        }
            
        t = file_data_part1[file_idx].time;
        tm = localtime(&t);
        sprintf(str, "%d/%d/%d %2.2d:%2.2d:%2.2d",
                tm->tm_year-100, tm->tm_mon+1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        sdl_render_text(&title_pane, 0, 10, 0, str, WHITE, BLACK);

        if (lost_connection) {
            sdl_render_text(&title_pane, 0, 35, 0, "LOST_CONN", RED, BLACK);
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
            sdl_render_text(&title_pane, 0, 63, 0, "TIME_ERROR", RED, BLACK);
            time_error_msg_is_displayed = true;
        } else {
            time_error_msg_is_displayed = false;
        }

        if (screenshot_msg) {
            sdl_render_text(&title_pane, 0, 77, 0, "SCREENSHOT", WHITE, BLACK);
            screenshot_msg_is_displayed = true;
        } else {
            screenshot_msg_is_displayed = false;
        }

        sdl_render_text(&title_pane, 0, -5, 0, "(ESC)", WHITE, BLACK);

        sdl_render_text(&title_pane, 0, -11, 0, "(?)", WHITE, BLACK);
        
        // draw the camera image,
        draw_camera_image(&cam_pane, file_idx);

        // draw the data values,
        draw_data_values(&data_pane, file_idx);

        // draw the graph
        switch (graph_select) {
        case 0:
            draw_graph0(file_idx);
            break;
        case 1:
            draw_graph1(file_idx);
            break;
        case 2:
            draw_graph2(file_idx);
            break;
        case 3: case 4: case 5: case 6: case 7:
            draw_graph34567(file_idx);
            break;
        default:
            FATAL("graph_select %d out of range\n", graph_select);
        }

        // register for events   
        sdl_event_register(SDL_EVENT_KEY_ESC, SDL_EVENT_TYPE_KEY, NULL);             // quit (esc)
        sdl_event_register('?', SDL_EVENT_TYPE_KEY, NULL);                           // help
        sdl_event_register('s', SDL_EVENT_TYPE_KEY, NULL);                           // graph select
        sdl_event_register('S', SDL_EVENT_TYPE_KEY, NULL);                           // graph select
        sdl_event_register(SDL_EVENT_KEY_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);      // graph cursor time
        sdl_event_register(SDL_EVENT_KEY_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_CTRL_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_CTRL_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL); 
        sdl_event_register(SDL_EVENT_KEY_ALT_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_ALT_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_HOME, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_END, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('+', SDL_EVENT_TYPE_KEY, NULL);                           // graph scale control
        sdl_event_register('=', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('-', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('1', SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register('2', SDL_EVENT_TYPE_KEY, NULL);

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
            case SDL_EVENT_QUIT: case SDL_EVENT_KEY_ESC: 
                quit = true;
                break;
            case SDL_EVENT_SCREENSHOT_TAKEN:
                screenshot_time_us = microsec_timer();
                break;
            case '?':  
                sdl_display_text(about);
                break;
            case 's':
                tmp = graph_select + 1;
                graph_select = (tmp <= MAX_GRAPH-1 ? tmp : 0);
                break;
            case 'S':
                tmp = graph_select - 1;
                graph_select = (tmp >= 0 ? tmp : MAX_GRAPH-1);
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
                    mode = (initially_live_mode ? LIVE : PLAYBACK);
                } else {
                    file_idx_global = x;
                    mode = PLAYBACK;
                }
                break; }
            case SDL_EVENT_KEY_HOME:
                file_idx_global = 0;
                mode = PLAYBACK;
                break;
            case SDL_EVENT_KEY_END:
                file_idx_global = file_hdr->max - 1;
                mode = (initially_live_mode ? LIVE : PLAYBACK);
                break;
            case '-': case '+': case '=':
            case '1': case '2':
                draw_graph_scale_select(event->event);
                break;
            default:
                event_processed_count--;
                break;
            }

            // determine if screenshot msg should be displayed 
            screenshot_msg = (microsec_timer() - screenshot_time_us) < MAX_SCREENSHOT_US;

            // test if should break out of this loop
            if ((quit) ||
                (lost_connection != lost_connection_msg_is_displayed) ||
                (file_error != file_error_msg_is_displayed) ||
                (time_error != time_error_msg_is_displayed) ||
                (screenshot_msg != screenshot_msg_is_displayed) ||
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

    static texture_t cam_texture = NULL;

    // this program requires CAM_WIDTH to be >= CAM_HEIGHT; 
    // the reason being that a square texture is created with dimension
    // of CAM_HEIGHT x CAM_HEIGHT, and this texture is updated with the
    // pixels centered around CAM_WIDTH/2
    #if CAM_WIDTH < CAM_HEIGHT
        #error CAM_WIDTH must be >= CAN_HEIGHT
    #endif

    // on first call create the cam_texture
    if (cam_texture == NULL) {
        cam_texture = sdl_create_yuy2_texture(CAM_HEIGHT,CAM_HEIGHT);
        if (cam_texture == NULL) {
            FATAL("failed to create cam_texture\n");
        }
    }

    // if no jpeg buff then 
    //   display 'no image'
    //   return
    // endif
    if (!file_data_part1[file_idx].data_part2_jpeg_buff_valid ||
        (data_part2 = read_data_part2(file_idx)) == NULL)
    {
        errstr = "NO IMAGE";
        goto error;
    }
    
    // decode the jpeg buff contained in data_part2
    ret = jpeg_decode(0,  // cxid
                     JPEG_DECODE_MODE_YUY2,      
                     data_part2->jpeg_buff, data_part2->jpeg_buff_len,
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
    sdl_update_yuy2_texture(cam_texture, 
                             pixel_buff + (CAM_WIDTH - CAM_HEIGHT) / 2,
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

// - - - - - - - - -  DISPLAY HANDLER - DRAW DATA VALUES  - - - - - - - - - - - - - - 

static void draw_data_values(rect_t * data_pane, int32_t file_idx)
{
    struct data_part1_s * dp1;
    char str[100];

    dp1 = &file_data_part1[file_idx];

    sprintf(str, "KV    %s %s %s",
            val2str(dp1->voltage_mean_kv),
            val2str(dp1->voltage_min_kv),
            val2str(dp1->voltage_max_kv));
    sdl_render_text(data_pane, 0, 0, 1, str, WHITE, BLACK);

    sprintf(str, "MA    %s",
            val2str(dp1->current_ma));
    sdl_render_text(data_pane, 1, 0, 1, str, WHITE, BLACK);

    sprintf(str, "D2 mT %s",
            val2str(dp1->pressure_d2_mtorr));
    sdl_render_text(data_pane, 2, 0, 1, str, WHITE, BLACK);

    sprintf(str, "N2 mT %s",
            val2str(dp1->pressure_n2_mtorr));
    sdl_render_text(data_pane, 3, 0, 1, str, WHITE, BLACK);

    sprintf(str, "CPM   %s",
            val2str(NEUTRON_CPM(file_idx)));
    sdl_render_text(data_pane, 4, 0, 1, str, WHITE, BLACK);
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW GRAPH  - - - - - - - - - - - - - - - - 

static void draw_graph_init(rect_t * graph_pane)
{
    graph_pane_global = *graph_pane;

    graph_x_origin = 10;
    graph_x_range  = 1200;
    graph_y_origin = graph_pane->h - FONT0_HEIGHT - 4;
    graph_y_range  = graph_pane->h - FONT0_HEIGHT - 4 - FONT0_HEIGHT;

    graph_x_time_span_sec =  1800;             
    graph_y_max_mv        =  10000;
    graph_y_max_cpm       =  1000;
}

static void draw_graph_scale_select(char key)
{
    static int32_t  x_time_span_sec_tbl[] = {60, 600, 1800, 3600, 7200, 21600, 43200, 86400};
    static int32_t  y_max_mv_tbl[] = {100, 1000, 2000, 5000, 10000};
    static float    y_max_cpm_tbl[] = {0.1, 1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0};

    #define REDUCE(val, tbl) \
        do { \
            int32_t i; \
            for (i = 1; i < sizeof(tbl)/sizeof(tbl[0]); i++) { \
                if (val == tbl[i]) { \
                    val = tbl[i-1]; \
                    break; \
                } \
            } \
        } while (0)

    #define INCREASE(val, tbl) \
        do { \
            int32_t i; \
            for (i = sizeof(tbl)/sizeof(tbl[0])-2; i >= 0; i--) { \
                if (val == tbl[i]) { \
                    val = tbl[i+1]; \
                    break; \
                } \
            } \
        } while (0)

    switch (key) {
    case '-': case '+': case '=':
        // maintain graph_x_time_span_sec for graphs 0,3,4,5,6,7
        if (graph_select == 0  || 
            graph_select == 3  || 
            graph_select == 4  || 
            graph_select == 5  || 
            graph_select == 6  || 
            graph_select == 7)
        {
            if (key == '-') {
                REDUCE(graph_x_time_span_sec, x_time_span_sec_tbl);
            } else {
                INCREASE(graph_x_time_span_sec, x_time_span_sec_tbl);
            }
            break;
        }
        break;
    case '1': case '2':
        // maintain graph_y_max_mv for graphs 1,2
        if (graph_select == 1 ||
            graph_select == 2)
        {
            if (key == '1') {
                REDUCE(graph_y_max_mv, y_max_mv_tbl);
            } else {
                INCREASE(graph_y_max_mv, y_max_mv_tbl);
            }
            break;
        }

        // maintain graph_y_max_cpm for graphs 3,4,5,6,7
        if (graph_select == 3  || 
            graph_select == 4  || 
            graph_select == 5  || 
            graph_select == 6  || 
            graph_select == 7)
        {
            if (key == '1') {
                REDUCE(graph_y_max_cpm, y_max_cpm_tbl);
            } else {
                INCREASE(graph_y_max_cpm, y_max_cpm_tbl);
            }
            break;
        }
        break;
    default:
        FATAL("invalid key 0x%x\n", key);
        break;
    }
}

static void draw_graph0(int32_t file_idx)
{
    float    voltage_mean_kv_values[MAX_FILE_DATA_PART1];
    float    current_ma_values[MAX_FILE_DATA_PART1];
    float    pressure_d2_mtorr_values[MAX_FILE_DATA_PART1];
    float    neutron_cpm_values[MAX_FILE_DATA_PART1];
    int32_t  file_idx_start, file_idx_end, max_values, i;
    uint64_t cursor_time_us;
    float    cursor_pos;
    char     x_info_str[100];
    char     y_info_str[100];
    char     cursor_str[100];

    // init x_info_str and y_info_str
    sprintf(x_info_str, "X: %d SEC (-/+)", graph_x_time_span_sec);
    sprintf(y_info_str, "%s", "");

    // init file_idx_start & file_idx_end
    if (mode == LIVE) {
        file_idx_end   = file_idx;
        file_idx_start = file_idx_end - (graph_x_time_span_sec - 1);
    } else {
        file_idx_start = file_idx - graph_x_time_span_sec / 2;
        file_idx_end   = file_idx_start + graph_x_time_span_sec - 1;
    }

    // init cursor position and string
    cursor_time_us = file_data_part1[file_idx].time * 1000000;
    cursor_pos = (float)(file_idx - file_idx_start) / (graph_x_time_span_sec - 1);
    time2str(cursor_str, cursor_time_us, false, false, false);

    // init arrays of the values to graph
    max_values = 0;
    for (i = file_idx_start; i <= file_idx_end; i++) {
        voltage_mean_kv_values[max_values]   = (i >= 0 && i < file_hdr->max)
                                               ? file_data_part1[i].voltage_mean_kv
                                               : ERROR_NO_VALUE;
        current_ma_values[max_values]        = (i >= 0 && i < file_hdr->max)
                                                ? file_data_part1[i].current_ma      
                                                : ERROR_NO_VALUE;
        pressure_d2_mtorr_values[max_values] = (i >= 0 && i < file_hdr->max)
                                                ? file_data_part1[i].pressure_d2_mtorr
                                                : ERROR_NO_VALUE;
        neutron_cpm_values[max_values]       = (i >= 0 && i < file_hdr->max)
                                                ? NEUTRON_CPM(i)
                                                : ERROR_NO_VALUE;
        max_values++;
    }

    // draw the graph
    i = file_idx - file_idx_start;
    draw_graph_common(
        "SUMMARY", x_info_str, y_info_str, cursor_pos, cursor_str, 4, 
        val2str2(voltage_mean_kv_values[i], "KV   "),   RED,    30.0,    max_values, voltage_mean_kv_values,
        val2str2(current_ma_values[i], "MA   "),        GREEN,  30.0,    max_values, current_ma_values,
        val2str2(pressure_d2_mtorr_values[i], "MTORR"), BLUE,   30.0,    max_values, pressure_d2_mtorr_values,
        val2str2(neutron_cpm_values[i], "CPM  "),       PURPLE, 10000.0, max_values, neutron_cpm_values);
}

static void draw_graph1(int32_t file_idx)
{
    struct data_part1_s * dp1;
    struct data_part2_s * dp2;
    float                 voltage_adc_samples_mv_values[MAX_ADC_SAMPLES];
    float                 current_adc_samples_mv_values[MAX_ADC_SAMPLES];
    float                 pressure_adc_samples_mv_values[MAX_ADC_SAMPLES];
    int32_t               i;
    char                  x_info_str[100];
    char                  y_info_str[100];

    // init x_info str and y_info_str
    sprintf(x_info_str, "X: 1 SEC");
    sprintf(y_info_str, "Y: %d MV (1/2)", graph_y_max_mv);

    // init pointer to dp1, and read dp2
    dp1 = &file_data_part1[file_idx];
    dp2 = read_data_part2(file_idx);
    if (dp2 == NULL) {
        ERROR("failed read data part2\n");
        draw_graph_common("ADC SAMPLES", x_info_str, y_info_str, -1, NULL, 0);
        return;
    }

    // init arrays of the values to graph
    for (i = 0; i < MAX_ADC_SAMPLES; i++) {
        voltage_adc_samples_mv_values[i] = dp1->data_part2_voltage_adc_samples_mv_valid 
                                       ? dp2->voltage_adc_samples_mv[i]
                                       : ERROR_NO_VALUE;
        current_adc_samples_mv_values[i] = dp1->data_part2_current_adc_samples_mv_valid 
                                       ? dp2->current_adc_samples_mv[i]
                                       : ERROR_NO_VALUE;
        pressure_adc_samples_mv_values[i] = dp1->data_part2_pressure_adc_samples_mv_valid 
                                       ? dp2->pressure_adc_samples_mv[i]
                                       : ERROR_NO_VALUE;
    }

    // draw the graph
    draw_graph_common(
        "ADC SAMPLES", x_info_str, y_info_str, -1, NULL, 3,
        "VOLTAGE ", RED,   (double)graph_y_max_mv, MAX_ADC_SAMPLES, voltage_adc_samples_mv_values,
        "CURRENT ", GREEN, (double)graph_y_max_mv, MAX_ADC_SAMPLES, current_adc_samples_mv_values,
        "PRESSURE", BLUE,  (double)graph_y_max_mv, MAX_ADC_SAMPLES, pressure_adc_samples_mv_values);
}

static void draw_graph2(int32_t file_idx)
{
    struct data_part1_s * dp1;
    struct data_part2_s * dp2;
    float                 he3_adc_samples_mv_values[MAX_ADC_SAMPLES];
    int32_t               i;
    char                  x_info_str[100];
    char                  y_info_str[100];

    // init x_info str and y_info_str
    sprintf(x_info_str, "X: 2.4 MS");
    sprintf(y_info_str, "Y: %d MV (1/2)", graph_y_max_mv);

    // init pointer to dp1, and read dp2
    dp1 = &file_data_part1[file_idx];
    dp2 = read_data_part2(file_idx);
    if (dp2 == NULL) {
        ERROR("failed read data part2\n");
        draw_graph_common("ADC SAMPLES", x_info_str, y_info_str, -1, NULL, 0);
        return;
    }

    // init arrays of the values to graph
    for (i = 0; i < MAX_ADC_SAMPLES; i++) {
        he3_adc_samples_mv_values[i] = dp1->data_part2_he3_adc_samples_mv_valid 
                                       ? dp2->he3_adc_samples_mv[i]
                                       : ERROR_NO_VALUE;
    }

    // draw the graph
    draw_graph_common(
        "ADC SAMPLES", x_info_str, y_info_str, -1, NULL, 1,
        "HE3 ", PURPLE, (double)graph_y_max_mv, MAX_ADC_SAMPLES, he3_adc_samples_mv_values);
}

static void draw_graph34567(int32_t file_idx)
{
    float    cpm[MAX_HE3_CHAN][MAX_FILE_DATA_PART1];
    int32_t  file_idx_start, file_idx_end, max_values, i, avg_sec;
    uint64_t cursor_time_us;
    float    cursor_pos;
    char     x_info_str[100];
    char     y_info_str[100];
    char     title_str[100];
    char     cursor_str[100];

    // init avg_sec;
    avg_sec = (graph_select == 3 ? 1    : 
               graph_select == 4 ? 10   : 
               graph_select == 5 ? 60   :
               graph_select == 6 ? 600  : 
               graph_select == 7 ? 3600   
                                 : -1);
    if (avg_sec == -1) {
        FATAL("invalid graph_select %d\n", graph_select);
    }

    // init x_info_str, y_info_str, and title_str
    sprintf(x_info_str, "X: %d SEC (-/+)", graph_x_time_span_sec);
    sprintf(y_info_str, "Y: %0.1f CPM (1/2)", graph_y_max_cpm);
    sprintf(title_str, "CPM %d SEC AVG", avg_sec);

    // init file_idx_start & file_idx_end
    if (mode == LIVE) {
        file_idx_end   = file_idx;
        file_idx_start = file_idx_end - (graph_x_time_span_sec - 1);
    } else {
        file_idx_start = file_idx - graph_x_time_span_sec / 2;
        file_idx_end   = file_idx_start + graph_x_time_span_sec - 1;
    }

    // init cursor position and string
    cursor_time_us = file_data_part1[file_idx].time * 1000000;
    cursor_pos = (float)(file_idx - file_idx_start) / (graph_x_time_span_sec - 1);
    time2str(cursor_str, cursor_time_us, false, false, false);

    // init arrays of the values to graph
    max_values = 0;
    for (i = file_idx_start; i <= file_idx_end; i++) {
        int32_t chan;
        float * he3_cpm;

        switch (avg_sec) {
        case 1:
            he3_cpm = file_data_part1[i].he3.cpm_1_sec;
            break;
        case 10:
            he3_cpm = file_data_part1[i].he3.cpm_10_sec;
            break;
        case 60:
            he3_cpm = file_data_part1[i].he3.cpm_60_sec;
            break;
        case 600:
            he3_cpm = file_data_part1[i].he3.cpm_600_sec;
            break;
        case 3600:
            he3_cpm = file_data_part1[i].he3.cpm_3600_sec;
            break;
        default:
            FATAL("avg_sec %d is invalid\n", avg_sec);
            break;
        }
        for (chan = 0; chan < MAX_HE3_CHAN; chan++) {
            cpm[chan][max_values] = (i >= 0 && i < file_hdr->max)
                                    ? he3_cpm[chan]
                                    : ERROR_NO_VALUE;
        }
        max_values++;
    }

    // draw the graph
    i = file_idx - file_idx_start;
    draw_graph_common(
        title_str, x_info_str, y_info_str, cursor_pos, cursor_str, 8,
        val2str2(cpm[0][i], "CPM"), PURPLE,     graph_y_max_cpm,    max_values, cpm[0],
        val2str2(cpm[1][i], "CPM"), BLUE,       graph_y_max_cpm,    max_values, cpm[1],
        val2str2(cpm[2][i], "CPM"), LIGHT_BLUE, graph_y_max_cpm,    max_values, cpm[2],
        val2str2(cpm[3][i], "CPM"), GREEN,      graph_y_max_cpm,    max_values, cpm[3],
        val2str2(cpm[4][i], "CPM"), YELLOW,     graph_y_max_cpm,    max_values, cpm[4],
        val2str2(cpm[5][i], "CPM"), ORANGE,     graph_y_max_cpm,    max_values, cpm[5],
        val2str2(cpm[6][i], "CPM"), PINK,       graph_y_max_cpm,    max_values, cpm[6],
        val2str2(cpm[7][i], "CPM"), RED,        graph_y_max_cpm,    max_values, cpm[7]);
}

static void draw_graph_common(char * title_str, char * x_info_str, char * y_info_str, float cursor_pos, char * cursor_str, int32_t max_graph, ...)
{
    // variable arg list ...
    // - char * graph_title
    // - int32_t graph_color
    // - double  graph_y_max     
    // - int32_t graph_max_values
    // - float * graph_values
    // - ... repeat for next graph ...

    va_list ap;
    rect_t   rect;
    int32_t  i;
    int32_t  gridx;
    int32_t  cursor_x;
    int32_t  title_str_col;
    int32_t  info_str_col;
    int32_t  cursor_str_col;
    int32_t  graph_title_str_col;
    char     graph_title_str[100];

    struct graph_s {
        char    * title;
        int32_t   color;
        float     y_max;
        int32_t   max_values;
        float   * values;
    } graph[20];

    // get the variable args 
    va_start(ap, max_graph);
    for (i = 0; i < max_graph; i++) {
        graph[i].title      = va_arg(ap, char*);
        graph[i].color      = va_arg(ap, int32_t);
        graph[i].y_max      = va_arg(ap, double);
        graph[i].max_values = va_arg(ap, int32_t);
        graph[i].values     = va_arg(ap, float*);
    }
    va_end(ap);

    // fill white
    rect.x = 0;
    rect.y = 0;
    rect.w = graph_pane_global.w;
    rect.h = graph_pane_global.h;
    sdl_render_fill_rect(&graph_pane_global, &rect, WHITE);

    for (gridx = 0; gridx < max_graph; gridx++) {
        #define MAX_POINTS 1000

        struct graph_s * g;
        float            x;
        float            x_pixels_per_val;
        float            y_scale_factor;
        int32_t          y_limit1;
        int32_t          y_limit2;
        int32_t          max_points;

        int32_t          i;
        point_t          points[MAX_POINTS];
        point_t        * p;

        // init for graph[gridx]
        g                 = &graph[gridx];
        x                 = (float)graph_x_origin;
        x_pixels_per_val  = (float)graph_x_range / g->max_values;
        y_scale_factor    = (float)graph_y_range / g->y_max;
        y_limit1          = graph_y_origin - graph_y_range;
        y_limit2          = graph_y_origin + FONT0_HEIGHT;
        max_points        = 0;

        // draw the graph lines
        for (i = 0; i < g->max_values; i++) {
            if (g->values[i] != ERROR_NO_VALUE) {
                p = &points[max_points];
                p->x = x;
                p->y = graph_y_origin - y_scale_factor * g->values[i];
                if (p->y < y_limit1) {
                    p->y = y_limit1;
                }
                if (p->y > y_limit2) {
                    p->y = y_limit2;
                }
                max_points++;
                if (max_points == MAX_POINTS) {
                    sdl_render_lines(&graph_pane_global, points, max_points, g->color);
                    points[0].x = points[max_points-1].x;
                    points[0].y = points[max_points-1].y;
                    max_points = 1;
                }
            } else if (max_points > 0) {
                sdl_render_lines(&graph_pane_global, points, max_points, g->color);
                max_points = 0;
            }

            x += x_pixels_per_val;
        }
        if (max_points) {
            sdl_render_lines(&graph_pane_global, points, max_points, g->color);
        }

        // draw the graph title
        graph_title_str_col = (graph_x_range + graph_x_origin) / FONT0_WIDTH + 6;
        if (g->y_max >= 1) {
            sprintf(graph_title_str, "%s : %.0f MAX", g->title, g->y_max);
        } else {
            sprintf(graph_title_str, "%s : %.2f MAX", g->title, g->y_max);
        }
        sdl_render_text(&graph_pane_global, gridx+1, graph_title_str_col, 0, graph_title_str, g->color, WHITE);
    }

    // draw x axis
    sdl_render_line(&graph_pane_global, 
                    graph_x_origin, graph_y_origin+1, 
                    graph_x_origin+graph_x_range, graph_y_origin+1,
                    BLACK);
    sdl_render_line(&graph_pane_global, 
                    graph_x_origin, graph_y_origin+2, 
                    graph_x_origin+graph_x_range, graph_y_origin+2,
                    BLACK);
    sdl_render_line(&graph_pane_global, 
                    graph_x_origin, graph_y_origin+3, 
                    graph_x_origin+graph_x_range, graph_y_origin+3,
                    BLACK);

    // draw y axis
    sdl_render_line(&graph_pane_global, 
                    graph_x_origin-1, graph_y_origin+3, 
                    graph_x_origin-1, graph_y_origin-graph_y_range,
                    BLACK);
    sdl_render_line(&graph_pane_global, 
                    graph_x_origin-2, graph_y_origin+3, 
                    graph_x_origin-2, graph_y_origin-graph_y_range,
                    BLACK);
    sdl_render_line(&graph_pane_global, 
                    graph_x_origin-3, graph_y_origin+3, 
                    graph_x_origin-3, graph_y_origin-graph_y_range,
                    BLACK);

    // draw cursor, and cursor_str
    if (cursor_pos >= 0) {
        cursor_x = graph_x_origin + cursor_pos * (graph_x_range - 1);
        sdl_render_line(&graph_pane_global,
                        cursor_x, graph_y_origin,
                        cursor_x, graph_y_origin-graph_y_range,
                        PURPLE);
        if (cursor_str != NULL) {
            cursor_str_col = cursor_x/FONT0_WIDTH - strlen(cursor_str)/2;
            sdl_render_text(&graph_pane_global,
                            -1, cursor_str_col,
                            0, cursor_str, PURPLE, WHITE);
        }
    }

    // draw title_str, and info_str
    if (title_str != NULL) {
        title_str_col = (graph_x_origin + graph_x_range/2) / FONT0_WIDTH - strlen(title_str)/2;
        sdl_render_text(&graph_pane_global,
                        0, title_str_col,
                        0, title_str, BLACK, WHITE);
    }
    if (x_info_str != NULL) {
        info_str_col = (graph_x_range + graph_x_origin) / FONT0_WIDTH + 6;
        sdl_render_text(&graph_pane_global,
                        -1, info_str_col,
                        0, x_info_str, BLACK, WHITE);
    }
    if (y_info_str != NULL) {
        info_str_col = (graph_x_range + graph_x_origin) / FONT0_WIDTH + 6;
        sdl_render_text(&graph_pane_global,
                        -2, info_str_col,
                        0, y_info_str, BLACK, WHITE);
    }

    // draw graph select control
    sdl_render_text(&graph_pane_global, 0, -5, 0, "(s/S)", BLACK, WHITE);
}

// -----------------  GENERATE TEST FILE----------------------------------------------

static int32_t generate_test_file(void) 
{
    time_t                t;
    uint8_t               jpeg_buff[200000];
    uint32_t              jpeg_buff_len;
    uint64_t              dp2_offset;
    int32_t               len, idx, i, fd, chan;
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

        dp1->voltage_mean_kv = 30.0 * idx / test_file_secs;
        dp1->voltage_min_kv = 0;
        dp1->voltage_max_kv = 15.0 * idx / test_file_secs;
        dp1->current_ma = 0;
        dp1->pressure_d2_mtorr = 10;
        dp1->pressure_n2_mtorr = 20;

        for (chan = 0; chan < MAX_HE3_CHAN; chan++) {
            dp1->he3.cpm_1_sec[chan] = 1000;
            dp1->he3.cpm_10_sec[chan] = 1200;
        }

        dp1->data_part2_offset = dp2_offset;
        dp1->data_part2_length = sizeof(struct data_part2_s) + jpeg_buff_len;
        dp1->data_part2_jpeg_buff_valid = (jpeg_buff_len != 0);
        dp1->data_part2_voltage_adc_samples_mv_valid = true;
        dp1->data_part2_current_adc_samples_mv_valid = true;
        dp1->data_part2_pressure_adc_samples_mv_valid = true;

        // data part2
        dp2->magic = MAGIC_DATA_PART2;
        for (i = 0; i < MAX_ADC_SAMPLES; i++) {
            dp2->voltage_adc_samples_mv[i]  = 10000 * i / MAX_ADC_SAMPLES;
            dp2->current_adc_samples_mv[i]  =  5000 * i / MAX_ADC_SAMPLES;
            dp2->pressure_adc_samples_mv[i] =  1000 * i / MAX_ADC_SAMPLES;
        }
        dp2->jpeg_buff_len = jpeg_buff_len;
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

static char * val2str(float val)
{
    static char str_tbl[20][100];
    static uint32_t idx;
    char * str;

    str = str_tbl[idx];
    idx = (idx + 1) % 20;

    if (IS_ERROR(val)) {
        sprintf(str, "%-6s", ERROR_TEXT(val));
    } else if (val < 1000.0) {
        sprintf(str, "%-6.2f", val);
    } else {
        sprintf(str, "%-6.0f", val);
    }

    return str;
}

static char * val2str2(float val, char * trailer_str)
{
    static char str_tbl[20][100];
    static uint32_t idx;
    char * str;

    str = str_tbl[idx];
    idx = (idx + 1) % 20;

    if (IS_ERROR(val)) {
        sprintf(str, "%-6s %s", ERROR_TEXT(val), trailer_str);
    } else if (val < 1000.0) {
        sprintf(str, "%-6.2f %s", val, trailer_str);
    } else {
        sprintf(str, "%-6.0f %s", val, trailer_str);
    }

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

static int getsockaddr(char * node, int port, int socktype, int protcol, struct sockaddr_in * ret_addr)
{
    struct addrinfo   hints;
    struct addrinfo * result;
    char              port_str[20];
    int               ret;

    sprintf(port_str, "%d", port);

    bzero(&hints, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags    = AI_NUMERICSERV;

    ret = getaddrinfo(node, port_str, &hints, &result);
    if (ret != 0) {
        ERROR("failed to get address of %s, %s\n", node, gai_strerror(ret));
        return -1;
    }
    if (result->ai_addrlen != sizeof(*ret_addr)) {
        ERROR("getaddrinfo result addrlen=%d, expected=%d\n",
            (int)result->ai_addrlen, (int)sizeof(*ret_addr));
        return -1;
    }

    *ret_addr = *(struct sockaddr_in*)result->ai_addr;
    freeaddrinfo(result);
    return 0;
}

static char * sock_addr_to_str(char * s, int slen, struct sockaddr * addr)
{
    char addr_str[100];
    int port;

    if (addr->sa_family == AF_INET) {
        inet_ntop(AF_INET,
                  &((struct sockaddr_in*)addr)->sin_addr,
                  addr_str, sizeof(addr_str));
        port = ((struct sockaddr_in*)addr)->sin_port;
    } else if (addr->sa_family == AF_INET6) {
        inet_ntop(AF_INET6,
                  &((struct sockaddr_in6*)addr)->sin6_addr,
                 addr_str, sizeof(addr_str));
        port = ((struct sockaddr_in6*)addr)->sin6_port;
    } else {
        snprintf(s,slen,"Invalid AddrFamily %d", addr->sa_family);
        return s;
    }

    snprintf(s,slen,"%s:%d",addr_str,htons(port));
    return s;
}

