#if 0
TODO
- check camera angle
- compile both pgms on linux and rpi
- check size of data_t and compare
- Graph Cycling in live mode
  - main graph,  add neutron counts
  - detector moving average
  - detector average
  - diag: the current voltage graph , and add the neutron detector voltage
- Maybe can have a generic plot routine,
  - this will keep all the plots consistent
  - and make the coding or additon of new plots easier
- Some Details:
  - When in Playback display RECORDED-DATA in RED, else LIVE-DATA in GREEN
  - D2 vs N2 select - changes which fields is displayed and which field is shown on the main graph
- msync to sync
- XXX use int for data
- XXX use name display.dat instead of fusor.dat
#endif

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
#include "util_misc.h"
#include "about.h"

//
// defines
//

#define VERSION_STR  "1.0"

#define MODE_STR(m) ((m) == LIVE ? "LIVE" : (m) == PLAYBACK ? "PLAYBACK" : "TEST")

#define MAGIC_FILE 0x1122334455667788   // XXX magics could incorporte sizeof data1/2

#define MAX_FILE_DATA_PART1   100000  // XXX maybe make this 1 day
#define MAX_DATA_PART2_LENGTH 1000000
#define MAX_GRAPH             1
#define MAX_GRAPH_POINTS      100000

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

// #define JPEG_BUFF_SAMPLE_CREATE_ENABLE
#define JPEG_BUFF_SAMPLE_FILENAME "jpeg_buff_sample.bin"

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

typedef struct {
    char    title[100];
    int32_t color;
    int32_t max_points;
    point_t points[MAX_GRAPH_POINTS];
} graph_t;

//
// variables
//

static enum mode             initial_mode;
static enum mode             current_mode;

static uint32_t              win_width;
static uint32_t              win_height;

static int32_t               file_fd;
static file_hdr_t          * file_hdr;
static struct data_part1_s * file_data_part1;
static int32_t               file_idx_global;

static int32_t               graph_x_origin;
static int32_t               graph_x_range;
static int32_t               graph_y_origin;
static int32_t               graph_y_range;
static rect_t                graph_pane_global;
static int32_t               graph_select;
static int32_t               graph_scale_idx[MAX_GRAPH];

static int32_t               test_file_secs;

//
// prototypes
//

static int32_t initialize(int32_t argc, char ** argv);
static void usage(void);
static void * client_thread(void * cx);
static int32_t display_handler();
static void draw_camera_image(rect_t * cam_pane, int32_t file_idx);
static void draw_data_values(rect_t * data_pane, int32_t file_idx);
static void draw_graph_init(rect_t * graph_pane);
static void draw_graph0(int32_t file_idx);
static void draw_graph_common( char * info_str, int32_t cursor_x, char * cursor_str, 
    int32_t max_graph, ...);
static int32_t generate_test_file(void);
static char * val2str(char * str, float val);
static struct data_part2_s * read_data_part2(int32_t file_idx);
static int getsockaddr(char * node, int port, int socktype, int protcol, struct sockaddr_in * ret_addr);
static char * sock_addr_to_str(char * s, int slen, struct sockaddr * addr);

// -----------------  MAIN  ----------------------------------------------------------

int32_t main(int32_t argc, char **argv)
{
    if (initialize(argc, argv) < 0) {
        ERROR("initialize failed, program terminating\n");
        return 1;
    }

    switch (current_mode) {
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
        FATAL("mode %d not valid\n", current_mode);
        break;
    }

    INFO("terminating normally\n");
    return 0; 
}

// -----------------  INITIALIZE  ----------------------------------------------------

static int32_t initialize(int32_t argc, char ** argv)
{
    struct rlimit      rl;
    pthread_t          thread;
    int32_t            ret;
    char               filename[100];
    char               servername[100];
    struct sockaddr_in sockaddr;
    char               s[100];
    int32_t            wait_ms;

    // init core dumps
    // note - requires fs.suid_dumpable=1  in /etc/sysctl.conf if this is a suid pgm
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    // check size of data struct on 64bit linux and rpi
    INFO("sizeof data_t=%zd part1=%zd part2=%zd\n",
         sizeof(data_t), sizeof(struct data_part1_s), sizeof(struct data_part2_s));

    // init globals that are not 0
    initial_mode = LIVE;
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
    // -t secs     : generate test data file, secs long
    while (true) {
        char opt_char = getopt(argc, argv, "hvg:s:p:t:");
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
            initial_mode = PLAYBACK;
            strcpy(filename, optarg);
            break;
        case 't':
            initial_mode = TEST;
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
    if (initial_mode == LIVE || initial_mode == TEST) {
        if (argc > optind) {
            strcpy(filename, argv[optind]);
        } else if (initial_mode == LIVE) {
            time_t t = time(NULL);
            struct tm * tm = localtime(&t);
            sprintf(filename, "fusor_%2.2d%2.2d%2.2d_%2.2d%2.2d%2.2d.dat",
                    tm->tm_mon+1, tm->tm_mday, tm->tm_year-100,
                    tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else {  // initial_mode is TEST
            sprintf(filename, "fusor_test_%d_secs.dat", test_file_secs);
        }
    }

    // print mode and filename
    INFO("mode            = %s\n", MODE_STR(initial_mode));
    INFO("filename        = %s\n", filename);
    if (initial_mode == TEST) {
        INFO("test_file_secs  = %d\n", test_file_secs);
    }

    // if mode is live or test then 
    //   verify filename does not exist
    //   create and init the file  
    // endif
    if (initial_mode == LIVE || initial_mode == TEST) {
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
    if (initial_mode == PLAYBACK) {
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
    //   connect to server
    //   create thread to acquire data from server
    //   wait for first data to be received from server
    // endif
    if (initial_mode == LIVE) {
        int32_t sfd;

        // print servername
        INFO("servername      = %s\n", servername);

        // get address of server
        ret =  getsockaddr(servername, PORT, SOCK_STREAM, 0, &sockaddr);
        if (ret < 0) {
            ERROR("failed to get address of %s\n", servername);
            return -1;
        }
 
        // print serveraddr
        INFO("serveraddr      = %s\n", 
             sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&sockaddr));

        // create socket
        sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == -1) {
            ERROR("create socket, %s\n", strerror(errno));
            return -1;
        }

        // connect to the server
        if (connect(sfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0) {
            char s[100];
            ERROR("connect to %s, %s\n", 
                  sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&sockaddr),
                  strerror(errno));
            return -1;
        }

        // create client_thread        
        if (pthread_create(&thread, NULL, client_thread, (void*)(uintptr_t)sfd) != 0) {
            ERROR("pthread_create client_thread, %s\n", strerror(errno));
            return -1;
        }

        // wait for client_thread to get first data, tout 5 secs
        wait_ms = 0;
        while (file_idx_global == -1) {
            wait_ms += 10;
            usleep(10000);
            if (wait_ms >= 5000) {
                ERROR("failed to receive data from server\n");
                return -1;
            }
        }
    }

    // if in playback mode then
    //   verify the first entry of the file_data_part1 is valid, and
    //   set file_idx_global
    // endif
    if (initial_mode == PLAYBACK) {
        if (file_data_part1[0].magic != MAGIC_DATA_PART1) {
            ERROR("no data in file %s (0x%"PRIx64"\n", filename, file_data_part1[0].magic);
            return -1;
        }
        file_idx_global = 0;
    }

    // set current_mode to initial_mode
    current_mode = initial_mode;

    // return success
    return 0;
}

static void usage(void)
{
    // YYY tbd
}

// -----------------  CLIENT THREAD  -------------------------------------------------

static void * client_thread(void * cx)
{
    int32_t               sfd;
    int32_t               len;
    off_t                 offset;
    uint64_t              last_time;
    struct data_part1_s   data_part1;
    struct data_part2_s * data_part2;

    sfd    = (uintptr_t)cx;
    offset = FILE_DATA_PART2_OFFSET;
    last_time = -1;
    data_part2 = calloc(1, MAX_DATA_PART2_LENGTH);
    if (data_part2 == NULL) {
        FATAL("calloc\n");
    }

    while (true) {
        // if file is full then terminate thread
        if (file_idx_global >= MAX_FILE_DATA_PART1) {
            ERROR("file is full\n");
            break;
        }

        // read data part1 from server, and
        // verify data part1 magic, and length
        len = recv(sfd, &data_part1, sizeof(data_part1), MSG_WAITALL);
        if (len != sizeof(data_part1)) {
            ERROR("recv data_part1 len=%d exp=%zd, %s\n",
                  len, sizeof(data_part1), strerror(errno));
            break;
        }
        if (data_part1.magic != MAGIC_DATA_PART1) {
            ERROR("recv data_part1 bad magic 0x%"PRIx64"\n", 
                  data_part1.magic);
            break;
        }
        if (data_part1.data_part2_length > MAX_DATA_PART2_LENGTH) {
            ERROR("data_part2_length %d is too big\n", 
                  data_part1.data_part2_length);
            break;
        }

        // read data part2 from server,
        // verify magic
        len = recv(sfd, data_part2, data_part1.data_part2_length, MSG_WAITALL);
        if (len != data_part1.data_part2_length) {
            ERROR("recv data_part2 len=%d exp=%d, %s\n",
                  len, data_part1.data_part2_length, strerror(errno));
            break;
        }
        if (data_part2->magic != MAGIC_DATA_PART2) {
            ERROR("recv data_part2 bad magic 0x%"PRIx64"\n", 
                  data_part2->magic);
            break;
        }

        // check for time increasing by other than 1 second;
        // if so, print warning
        if (last_time != -1 && data_part1.time != last_time + 1) {
            WARN("time increased by %"PRId64"\n", data_part1.time - last_time);
        }
        last_time = data_part1.time;

        // save file offset in data_part1
        data_part1.data_part2_offset = offset;

        // write data to file
        file_data_part1[file_hdr->max] = data_part1;
        len = pwrite(file_fd, data_part2, data_part1.data_part2_length, offset);
        if (len != data_part1.data_part2_length) {
            ERROR("write data_part2 len=%d exp=%d, %s\n",
                  len, data_part1.data_part2_length, strerror(errno));
            break;
        }
        offset += data_part1.data_part2_length;

        // update file header, and
        // if live mode then update file_idx_global
        file_hdr->max++;
        if (current_mode == LIVE) {
            file_idx_global = file_hdr->max - 1;
        }
        INFO("XXX max=%d idx_global=%d\n", file_hdr->max, file_idx_global);

        // YYY keep cache copy of data part2 

#ifdef JPEG_BUFF_SAMPLE_CREATE_ENABLE
        // write a sample jpeg buffer to jpeg_buff_sample file
        static bool sample_written = false;
        if (!sample_written) {
            int32_t fd = open(JPEG_BUFF_SAMPLE_FILENAME, O_CREAT|O_TRUNC|O_RDWR, 0666);
            if (fd < 0) {
                ERROR("open %s, %s\n", JPEG_BUFF_SAMPLE_FILENAME, strerror(errno));
            } else {
                int32_t len = write(fd, data_part2->jpeg_buff, data_part2->jpeg_buff_len);
                if (len != data_part2->jpeg_buff_len) {
                    ERROR("write %s len exp=%d act=%d, %s\n",
                        JPEG_BUFF_SAMPLE_FILENAME, data_part2->jpeg_buff_len, len, strerror(errno));
                }
                close(fd);
            }
            sample_written = true;
        }
#endif
    }

    // YYY set error indicator here 
    // YYY how to handle timeouts in receiving data 
    
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

    // initializae 
    quit = false;
    file_max_last = -1;

    sdl_init(win_width, win_height);  // XXX check status here

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
        file_idx = file_idx_global;
        if (file_idx < 0 ||
            file_idx >= file_hdr->max ||
            file_data_part1[file_idx].magic != MAGIC_DATA_PART1) 
        {
            FATAL("invalid file_idx %d, max =%d\n",
                  file_idx, file_hdr->max);
        }
        INFO("XXX file_idx %d\n", file_idx);

        // initialize for display update
        sdl_display_init();

        // draw pane borders   
        sdl_render_pane_border(&title_pane_full, GREEN);
        sdl_render_pane_border(&cam_pane_full,   GREEN);
        sdl_render_pane_border(&data_pane_full,  GREEN);
        sdl_render_pane_border(&graph_pane_full, GREEN);

        // draw title line
        if (current_mode == LIVE) {
            sdl_render_text(&title_pane, 0, 0, 0, "LIVE", GREEN, BLACK);
        } else {
            sdl_render_text(&title_pane, 0, 0, 0, "PLAYBACK", RED, BLACK);
        }
            
        t = file_data_part1[file_idx].time;
        tm = localtime(&t);
        sprintf(str, "%d/%d/%d %2.2d:%2.2d:%2.2d",
                tm->tm_mon+1, tm->tm_mday, tm->tm_year-100,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        sdl_render_text(&title_pane, 0, 10, 0, str, WHITE, BLACK);

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
        default:
            FATAL("graph_select %d out of range\n", graph_select);
        }

        // register for events   
        // YYY other graph events
        sdl_event_register(SDL_EVENT_KEY_ESC, SDL_EVENT_TYPE_KEY, NULL);             // quit (esc)
        sdl_event_register('?', SDL_EVENT_TYPE_KEY, NULL);                           // help
        sdl_event_register('s', SDL_EVENT_TYPE_KEY, NULL);                           // graph select
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

        // present the display
        sdl_display_present();

        // loop until
        // - at least one event has been processed AND current event is none, OR
        // - file index that is currently displayed is not file_idx_global
        // - file max has changed
        // - quit flag is set
        event_processed_count = 0;
        while (true) {
            // get and process event
            event_processed_count++;
            event = sdl_poll_event();
            switch (event->event) {
            case SDL_EVENT_QUIT: case SDL_EVENT_KEY_ESC: 
                quit = true;
                break;
            case '?':  
                sdl_display_text(about);
                break;
            case 's':
                graph_select = 0; // XXX
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
                current_mode = PLAYBACK;
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
                    if (initial_mode == LIVE) {
                        current_mode = LIVE;
                    }
                } else {
                    file_idx_global = x;
                    current_mode = PLAYBACK;
                }
                break; }
            case SDL_EVENT_KEY_HOME:
                file_idx_global = 0;
                current_mode = PLAYBACK;
                break;
            case SDL_EVENT_KEY_END:
                file_idx_global = file_hdr->max - 1;
                if (initial_mode == LIVE) {
                    current_mode = LIVE;
                }
                break;
            case '-': 
                graph_scale_idx[graph_select]--;
                break;
            case '+': case '=':
                graph_scale_idx[graph_select]++;
                break;
            default:
                event_processed_count--;
                break;
            }

            // test if should break out of this loop
            // XXX update comment
            if ((event->event == SDL_EVENT_NONE) &&
                ((event_processed_count > 0) ||
                 (file_idx != file_idx_global) ||
                 (file_hdr->max != file_max_last) ||
                 (quit)))
            {
                INFO("epc=%d  file_idx_global/file_idx=%d %d  max/last= %d %d  \n\n",  
                   event_processed_count, file_idx_global, file_idx, file_hdr->max, file_max_last);
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
    char str[100], s1[100], s2[100], s3[100];

    dp1 = &file_data_part1[file_idx];

    sprintf(str, "KV    %s %s %s",
            val2str(s1, dp1->voltage_mean_kv),
            val2str(s2, dp1->voltage_min_kv),
            val2str(s3, dp1->voltage_max_kv));
    sdl_render_text(data_pane, 0, 0, 1, str, WHITE, BLACK);

    sprintf(str, "MA    %s",  // YYY add min and max
            val2str(s1, dp1->current_ma));
    sdl_render_text(data_pane, 1, 0, 1, str, WHITE, BLACK);

    sprintf(str, "D2 mT %s",
            val2str(s1, dp1->chamber_pressure_d2_mtorr));
    sdl_render_text(data_pane, 2, 0, 1, str, WHITE, BLACK);

    sprintf(str, "N2 mT %s",
            val2str(s1, dp1->chamber_pressure_n2_mtorr));
    sdl_render_text(data_pane, 3, 0, 1, str, WHITE, BLACK);
}

// - - - - - - - - -  DISPLAY HANDLER - DRAW GRAPH  - - - - - - - - - - - - - - - - 

static void draw_graph_init(rect_t * graph_pane)
{
    graph_pane_global = *graph_pane;

    graph_x_origin = 10;
    graph_x_range  = 1200;
    graph_y_origin = graph_pane->h - FONT0_HEIGHT - 4;
    graph_y_range  = graph_pane->h - FONT0_HEIGHT - 10;
}

static void draw_graph0(int32_t file_idx)
{
    int32_t  file_idx_start, file_idx_end, idx;
    uint64_t cursor_time_sec;
    int32_t  cursor_x;
    char     cursor_str[100], info_str[100], str[100];
    int32_t  x_time_span_sec;
    float    x_pixels_per_sec;
    float    x;
    float    y_max;

    // XXX check how long it takes to run this routine for 86400 points
    static int32_t x_time_span_sec_tbl[] = {60, 600, 3600, 86400};
    static graph_t g_kv;

    #define MAX_X_TIME_SPAN_SEC_TBL \
        (sizeof(x_time_span_sec_tbl) / sizeof(x_time_span_sec_tbl[0]))

    // sanitize graph_scale_idx[0]
    if (graph_scale_idx[0] < 0) {
        graph_scale_idx[0] = 0;
    } else if (graph_scale_idx[0] >= MAX_X_TIME_SPAN_SEC_TBL) {
        graph_scale_idx[0] = MAX_X_TIME_SPAN_SEC_TBL - 1;
    }
        
    // init
    x_time_span_sec = x_time_span_sec_tbl[graph_scale_idx[0]];
    x_pixels_per_sec = (float)graph_x_range / x_time_span_sec;
    cursor_time_sec = file_data_part1[file_idx].time;

    // init file_idx_start & file_idx_end
    if (current_mode == LIVE) {
        file_idx_end   = file_idx;
        file_idx_start = file_idx_end - (x_time_span_sec - 1);
    } else {
        file_idx_start = file_idx - x_time_span_sec / 2;
        file_idx_end   = file_idx_start + x_time_span_sec - 1;
    }

    // init graph_t for:
    // - voltage_mean_kv 
    // - current_ma 
    // - chamber_pressure_d2_mtorr 
    // YYY check that max_points isn't too big
    // YYY args   30.0, "kv", voltage_mean_kv, g_kv, RED
    y_max = 30.0;
    sprintf(g_kv.title, "%s %6s : %.0f MAX",
            val2str(str, file_data_part1[file_idx].voltage_mean_kv),
            "kV",
            y_max);
    g_kv.color = RED;
    g_kv.max_points = 0;
    x = graph_x_origin + graph_x_range - 1;
    for (idx = file_idx_end; idx >= file_idx_start; idx--) {
        if (idx >= 0 && 
            idx < file_hdr->max &&
            !IS_ERROR(file_data_part1[idx].voltage_mean_kv))
        {
            point_t * p = &g_kv.points[g_kv.max_points];
            p->x = x;
            p->y = graph_y_origin - (graph_y_range / y_max) * file_data_part1[idx].voltage_mean_kv;
            g_kv.max_points++;
        }
        x -= x_pixels_per_sec;
    }
    INFO("XXX max_points  %d\n", g_kv.max_points);

    // init info_str
    sprintf(info_str, "X-SPAN %d SEC  (-/+)", x_time_span_sec);

    // XXX also need title_str

    // init cursor position and string
    cursor_x = (graph_x_origin + graph_x_range - 1) -
               (file_idx_end - file_idx) * x_pixels_per_sec;
    time2str(cursor_str, cursor_time_sec*1000000, false, false, false);

    // draw the graph
    draw_graph_common(info_str, cursor_x, cursor_str, 1, &g_kv);
}

static void draw_graph_common( char * info_str, int32_t cursor_x, char * cursor_str, 
    int32_t max_graph, ...)
{
    va_list ap;
    int32_t str_col, i;
    rect_t rect;

    va_start(ap, max_graph);

    // init
    str_col = (graph_x_range + graph_x_origin) / FONT0_WIDTH + 6;

    // fill white
    rect.x = 0;
    rect.y = 0;
    rect.w = graph_pane_global.w;
    rect.h = graph_pane_global.h;
    sdl_render_fill_rect(&graph_pane_global, &rect, WHITE);

    // loop over the graphs
    for (i = 0; i < max_graph; i++) {
        // draw the graph points
        graph_t * g          = va_arg(ap, graph_t *);
        point_t * points     = g->points;
        int32_t   max_points = g->max_points;
        int32_t   color      = g->color;
        char    * title      = g->title;
        while (max_points > 1) {
            int32_t points_to_render = (max_points > 1000 ? 1000 : max_points);
            sdl_render_lines(&graph_pane_global, points, points_to_render, color);
            max_points -= (points_to_render-1);
            points     += (points_to_render-1);
        }

        // draw the graph title
        sdl_render_text(&graph_pane_global, i, str_col, 0, title, color, WHITE);
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
    if (cursor_x >= 0) {
        sdl_render_line(&graph_pane_global,
                        cursor_x, graph_y_origin,
                        cursor_x, graph_y_origin-graph_y_range,
                        PURPLE);
    }
    if (cursor_str != NULL) {
        int32_t cursor_str_col = cursor_x/FONT0_WIDTH - strlen(cursor_str)/2;
        if (cursor_str_col < 0) {
            cursor_str_col = 0;
        }
        sdl_render_text(&graph_pane_global,
                        -1, cursor_str_col,
                        0, cursor_str, PURPLE, WHITE);
    }

    // draw info_str
    if (info_str != NULL) {
        sdl_render_text(&graph_pane_global,
                        -1, str_col,
                        0, info_str, BLACK, WHITE);
    }

    // draw graph select control
    sdl_render_text(&graph_pane_global, 0, -3, 0, "(s)", BLACK, WHITE);

    va_end(ap);
}

#if 0  //XXX del
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
        { "kV    : 30 MAX", 30.0, RED, offsetof(data_t, voltage_mean_kv) },
        { "mA    : 30 MAX", 30.0, GREEN, offsetof(data_t, current_ma) },
        { "mTorr : 30 MAX", 30.0, BLUE, offsetof(data_t, chamber_pressure_mtorr) },
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

    // determine graph1_start_sec and graph1_end_sec
    if (mode == LIVE_MODE) {
        graph1_end_time_sec = cursor_time_sec;
        graph1_start_time_sec = graph1_end_time_sec - (T_span - 1);
    } else {
        graph1_start_time_sec = cursor_time_sec - T_span / 2;
        graph1_end_time_sec = graph1_start_time_sec + T_span - 1;
    }


    // draw the graph1s
    for (i = 0; i < MAX_GRAPH1_CONFIG; i++) {
        #define MAX_POINTS1 1000
        struct graph1_config * gc;
        int32_t max_points, idx;
        point_t points[MAX_POINTS1];
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
                if (max_points == MAX_POINTS1) {
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
        if (history[idx].data_valid) {
            value = *(float*)((void*)&history[idx] + gc->field_offset);
        } else {
            value = ERROR_NO_VALUE;
        }
        val2str(str, value, graph1_config[i].name);

        sdl_render_text(graph_pane, 
                        i, str_col,
                        0, str, graph1_config[i].color, WHITE);
    }
}
#endif

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
        dp1->voltage_mean_kv = 30.0 * idx / test_file_secs;
        dp1->voltage_min_kv = 0;
        dp1->voltage_max_kv = 15.0 * idx / test_file_secs;
        dp1->current_ma = 0;
        dp1->chamber_pressure_d2_mtorr = 10;
        dp1->chamber_pressure_n2_mtorr = 20;
        for (i = 0; i < MAX_DETECTOR_CHAN; i++) {
            dp1->average_cpm[i] = ERROR_NO_VALUE;
            dp1->moving_average_cpm[i] = ERROR_NO_VALUE;
        }
        dp1->data_part2_length = sizeof(struct data_part2_s) + jpeg_buff_len;
        dp1->data_part2_jpeg_buff_valid = (jpeg_buff_len != 0);
        dp1->data_part2_offset = dp2_offset;

        // data part2
        dp2->magic = MAGIC_DATA_PART2;
        memcpy(dp2->jpeg_buff, jpeg_buff, jpeg_buff_len);
        dp2->jpeg_buff_len = jpeg_buff_len;
        // XXX more dp2 fields

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

static char * val2str(char * str, float val)
{
    if (IS_ERROR(val)) {
        sprintf(str, "%-6s", ERROR_TEXT(val));
    } else if (val < 1000.0) {
        sprintf(str, "%-6.2f", val);
    } else {
        sprintf(str, "%-6.0f", val);
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

    // YYY also cache the live data

    // initial allocate 
    if (last_read_data_part2 == NULL) {
        last_read_data_part2 = calloc(1,MAX_DATA_PART2_LENGTH);
        if (last_read_data_part2 == NULL) {
            FATAL("calloc");
        }
    }

    // if file_idx is same as last read then return data_part2 from last read
    if (file_idx == last_read_file_idx) {
        // INFO("YYY return cached, file_idx=%d\n", file_idx);
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
    // INFO("YYY return new read data, file_idx=%d\n", file_idx);
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

