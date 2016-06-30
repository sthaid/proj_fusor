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

#define MODE_STR(m) ((m) == LIVE ? "LIVE" : "PLAYBACK")

#define MAGIC_FILE 0x1122334455667788

#define MAX_FILE_DATA_PART1   100000
#define MAX_DATA_PART2_LENGTH 1000000

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

//
// typedefs
//

typedef struct {
    uint64_t magic;
    uint64_t start_time;
    uint32_t max;
    uint8_t  reserved[4096-20];
} file_hdr_t;

enum mode {LIVE, PLAYBACK};

//
// variables
//

static enum mode             initial_mode;
static enum mode             current_mode;
static uint32_t              win_width = DEFAULT_WIN_WIDTH;
static uint32_t              win_height = DEFAULT_WIN_HEIGHT;

static int32_t               file_fd;
static file_hdr_t          * file_hdr;
static struct data_part1_s * file_data_part1;
static int32_t               file_idx_global;

//
// prototypes
//

static int32_t initialize(int32_t argc, char ** argv);
static void usage(void);
static void * client_thread(void * cx);
static void display_handler();
static void draw_camera_image(rect_t * cam_pane, int32_t file_idx);
static void draw_data_values(rect_t * data_pane, int32_t file_idx);
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

    display_handler();

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

    // init globals 
    initial_mode = LIVE;
    file_idx_global = -1;
    strcpy(servername, "rpi_data");
    strcpy(filename, "");
 
    // parse options
    // -h          : help
    // -v          : version
    // -g WxH      : window width and height, default 1920x1080
    // -s name     : server name
    // -p filename : playback file
    while (true) {
        char opt_char = getopt(argc, argv, "hvg:s:p:");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'h':
            usage();
            exit(0);
        case 'v':
            printf("Version %s\n", VERSION_STR);
            exit(0);
        case 'g':
            if (sscanf(optarg, "%dx%d", &win_width, &win_height) != 2) {
                printf("invalid '-g %s'\n", optarg);
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
        default:
            return -1;
        }
    }

    // if in live mode then
    //   if filename was provided then 
    //     use the provided filename
    //   else
    //     generate the filename
    //   endif
    // endif
    if (initial_mode == LIVE) {
        if (argc > optind) {
            strcpy(filename, argv[optind]);
        } else {
            time_t t = time(NULL);
            struct tm * tm = localtime(&t);
            sprintf(filename, "fusor_%2.2d%2.2d%2.2d_%2.2d%2.2d%2.2d.dat",
                    tm->tm_mon+1, tm->tm_mday, tm->tm_year-100,
                    tm->tm_hour, tm->tm_min, tm->tm_sec);
        }
    }

    // print mode and filename
    INFO("mode       = %s\n", MODE_STR(initial_mode));
    INFO("filename   = %s\n", filename);

    // if in live mode then
    //   verify filename does not exist
    //   create and init the file  
    // endif
    if (initial_mode == LIVE) {
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
        file_hdr->max >= MAX_FILE_DATA_PART1)
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
        INFO("servername = %s\n", servername);

        // get address of server
        ret =  getsockaddr(servername, PORT, SOCK_STREAM, 0, &sockaddr);
        if (ret < 0) {
            ERROR("failed to get address of %s\n", servername);
            return -1;
        }
 
        // print serveraddr
        INFO("serveraddr = %s\n", 
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

        // YYY keep cache copy of data part2 
    }

    // YYY set error indicator here 
    // YYY how to handle timeouts in receiving data 
    
    return NULL;
}

// -----------------  DISPLAY HANDLER - MAIN  ----------------------------------------

static void display_handler(void)
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
    int32_t       file_idx_last;
    int32_t       file_max_last;

    // initializae 
    quit = false;
    file_idx_last = -1;
    file_max_last = -1;

    sdl_init(win_width, win_height);

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
        // get the file_idx, and verify
        file_idx = file_idx_global;
        if (file_idx < 0 ||
            file_idx >= file_hdr->max ||
            file_data_part1[file_idx].magic != MAGIC_DATA_PART1) 
        {
            FATAL("invalid file_idx %d, max =%d\n",
                  file_idx, file_hdr->max);
        }

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
        // YYY tbd

        // register for events   
        // YYY other graph events
        sdl_event_register(SDL_EVENT_KEY_ESC, SDL_EVENT_TYPE_KEY, NULL);             // quit (esc)
        sdl_event_register('?', SDL_EVENT_TYPE_KEY, NULL);                           // help
        sdl_event_register('s', SDL_EVENT_TYPE_KEY, NULL);                           // graph select
        sdl_event_register(SDL_EVENT_KEY_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);      // cursor
        sdl_event_register(SDL_EVENT_KEY_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_CTRL_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_CTRL_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL); 
        sdl_event_register(SDL_EVENT_KEY_ALT_LEFT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_ALT_RIGHT_ARROW, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_HOME, SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_END, SDL_EVENT_TYPE_KEY, NULL);

        // present the display
        sdl_display_present();

        // loop until
        // - at least one event has been processed AND current event is none, OR
        // - file index has changed
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
                // YYY
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
            default:
                event_processed_count--;
                break;
            }

            // test if should break out of this loop
            if ((event_processed_count > 0 && event->event == SDL_EVENT_NONE) ||
                (file_idx != file_idx_last) ||
                (file_hdr->max != file_max_last) ||
                (quit))
            {
                file_idx_last = file_idx;
                file_max_last = file_hdr->max;
                break;
            }

            // delay 10 ms
            usleep(10000);
        }
    }

    INFO("terminating\n");
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
        INFO("YYY return cached, file_idx=%d\n", file_idx);
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
    INFO("YYY return new read data, file_idx=%d\n", file_idx);
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

