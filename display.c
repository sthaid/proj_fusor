    // this program requires CAM_WIDTH to be >= CAM_HEIGHT; 
    // the reason being that a square texture is created with dimension
    // of CAM_HEIGHT x CAM_HEIGHT, and this texture is updated with the
    // pixels centered around CAM_WIDTH/2
    if (CAM_WIDTH < CAM_HEIGHT) {
        FATAL("CAM_WIDTH must be >= CAM_HEIGHT\n");
    }



#if 0
Same general screen layout
- experiment with smaller camera image, and larger graph area

Connect camera to this computer

when in live mode
- init: connect to dataq device
- thread to read from the device and write to disk
   last thing this thread does is update file header to commit, and maybe fflush
- In live mode, when data arrives from data-acq
    - put it in the file, along with most recent cam
    - update max cursor time, maybe this is in the file too,
         I guess the file needs a header
    - if tracking end of data then
      . update cursor time

Recording,
- file hdr
   - magic
   - time of first entry
   - time of last entry
- recorded data toc entry  (this is memmapped)
  . time  (used to sanity check program)  OPTIONAL
  . part1
  . offset and length of cam data
  . offset and length of part2
- additional
  . cam
  . part2

Graph Cycling in live mode
- main graph,  add neutron counts
- detector moving average
- detector average
- diag: the current voltage graph , and add the neutron detector voltage

Display is updated:
- whenever a key is pressed OR cursor time changes
- display data is always acquired by:
   - reading directly from memory mapped recording, to get current
      and values to plot  OR a routine
   - calling a routine to get the cam data based on cursortime
   - calling a routine to get the part2 data based on cursortime

Maybe can have a generic plot routine,
- this will keep all the plots consistent
- and make the coding or additon of new plots easier

Some Details:
- When in Playback display RECORDED-DATA in RED, else LIVE-DATA in GREEN
- D2 vs N2 select - changes which fields is displayed and which field is shown on the main graph

DONT NEED
- live mode vs playback mode distinction
- live mode history buff
- the current get data routine
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

//
// defines
//

// this program requires CAM_WIDTH to be >= CAM_HEIGHT; 
// the reason being that a square texture is created with dimension
// of CAM_HEIGHT x CAM_HEIGHT, and this texture is updated with the
// pixels centered around CAM_WIDTH/2
#if CAM_WIDTH < CAM_HEIGHT
  #error CAM_WIDTH must be >= CAN_HEIGHT
#endif

#define MAGIC_FILE 0x1122334455667788

#define MAX_FILE_DATA_PART1 100000

//
// typedefs
//

typedef struct {
    uint64_t magic
    uint64_t start_time;
    uint32_t max;
    uint8_t  reserved[4096-20];
} file_hdr_t;

//
// variables
//

file_hdr_t          * file_hdr;
struct data_part1_s * file_data_part1;

//
// prototypes
//

static void initialize(int32_t argc, char ** argv);
static void usage(void);
static void display_handler();
static void draw_camera_image(data_t * data, rect_t * cam_pane, texture_t cam_texture);
static void draw_data_values(data_t *data, rect_t * data_pane);
static char * val2str(char * str, float val, char * trailer_str);

// -----------------  MAIN  ----------------------------------------------------------

static int32_t main(int32_t argc, char **argv)
{
    if (initialize(argc, argv) < 0) {
        ERROR("initialize failed, program terminating\n");
        return 1;
    }

    display_handler();

    return 0;
}

// -----------------  INITIALIZE  ----------------------------------------------------

static void initialize(int32_t argc, char ** argv)
{
    struct rlimit rl;
    bool          connect_to_data_server;
    pthread_t     thread;

    // init core dumps
    // note - requires fs.suid_dumpable=1  in /etc/sysctl.conf
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);

    // parse options
    // -h       : help
    // -v       : version
    // -g WxH   : window width and height, default 1920x1080
    // -s name  : server name
    while (true) {
        char opt_char = getopt(argc, argv, "hvg:s:");
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
        default:
            return -1;
        }
    }

    // if a filename is not provided then 
    //   generate filename
    //   verify filename does not exist
    //   create and init file
    //   set flag to connect to data server
    // else
    //   verify filename exist
    //   clear flag to connect to data server
    // endif
    if (argc == optind) {
        time_t t;
        struct tm * tm;
        file_hdr_t hdr;

        // generate filename
        t = time(NULL);
        tm = localtime(&t);
        sprintf(filename, "fusor_%2.2d%2.2d%2.2d_%2.2d%2.2d%2.2d.dat",
                tm->tm_mon+1, tm->tm_mday, tm->tm_year-100,
                tm->tm_hour, tm->tm_min, tm->tm_sec);

        // verify filename does not exist
        if (stat(filename, &stat_buf) == 0) {
            ERROR("file %s already exists\n", filename);
            return -1;
        }

        // create and init filename
        fd = open(filename, O_CREAT|O_EXCL|O_RDWR, 0666);
        if (fd < 0) {
            ERROR("failed to create %s, %s\n", filename, strerror(errno));
            return -1;
        }
        bzero(&hdr, sizeof(hdr));
        hdr->magic      = MAGIC_FILE;
        hdr->start_time = 0;
        hdr->max        = 0;
        if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            ERROR("failed to init %s, %s\n", filename, strerror(errno));
            return -1;
        }
        close(fd);

        // set flag to connect to data server
        connect_to_data_server = true;
    } else {
        // verify filename exists
        strcpy(filename, argv[optind]);
        if (stat(filename, &stat_buf) == -1) {
            ERROR("file %s does not exist, %s\n", filename, strerror(errno));
            return -1;
        }

        // clear flag to connect to data server
        connect_to_data_server = false;
    }

    // open and map filename
    // YYY msync to sync
    fd = open(filename, O_RDWR);
    if (fd < 0) {
        ERROR("failed to open %s, %s\n", filename, strerror(errno));
        return -1;
    }
    file_hdr = mmap(NULL,  // addr
                   sizeof(file_hdr_t),
                   PROT_READ|PROT_WRITE,
                   MAP_SHARED,
                   fd,
                   0);   // offset
    if (file_hdr == MAP_FAILED) {
        ERROR("failed to map file_hdr %s, %s\n", filename, strerror(errno));
        return -1;
    }
    file_data_part1 = mmap(NULL,  // addr
                           sizeof(file_data_part1) * XXX,
                           PROT_READ|PROT_WRITE,
                           MAP_SHARED,
                           fd,
                           0);   // offset
    if (file_data_part1 == MAP_FAILED) {
        ERROR("failed to map file_data_part1 %s, %s\n", filename, strerror(errno));
        return -1;
    }

    // verify file header
    if (file_hdr->magic != MAGIC_FILE ||
        file_hdr->max >= MAX_FILE_DATA_PART1)
    {
        ERROR("invalid file %s, magic=0x%lx max=%d\n", 
              file_hdr->magic, file_hdr->max);
        return -1;
    }

    // if the filename was not provided then
    //   connect to server
    //   create thread to acquire data from server
    // endif
    if (connect_to_data_server) {
        // get address of server
        ret =  getsockaddr(server, PORT, SOCK_STREAM, 0, &addr);
        if (ret < 0) {
            ERROR("failed to get address of %s\n", server);
            return -1;
        }

        // create socket
        sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == -1) {
            ERROR("create socket, %s\n", strerror(errno));
            return -1;
        }

        // connect to the server
        if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ERROR("connect to %s, %s\n", 
                  sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&addr),
                  strerror(errno));
            return -1;
        }

        // create client_thread        
        if (pthread_create(&thread, NULL, client_thread, (void*)(uintptr_t)sfd) != 0) {
            ERROR("pthread_create client_thread, %s\n", strerror(errno));
            return -1;
        }
    }

    // print values
    // XXX others
    INFO("address of %s is %s\n",
          server, sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&addr));

    // return success
    return 0;
}

static void usage(void)
{
    // XXX tbd
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
        draw_camera_image(data, &cam_pane, cam_texture);

        // draw the data values,
        draw_data_values(data, &data_pane);

        // draw the graph
        // XXX tbd

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
        // XXX other graph events

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
            case 'g':
                // XXX
                break;
            case 's':
                // XXX
                break;
            default:
                event_processed_count--;
                break;
            }

            // test if should break out of this loop
            if ((event_processed_count > 0 && event->event == SDL_EVENT_NONE) ||
                (file_index != file_index_last) ||
                (file_hdr->max != file_max_last) ||
                (quit))
            {
                file_index_last = file_index;
                file_max_last   = file_hdr->max
                break;
            }

            // delay 10 ms
            usleep(10000);
        }
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
                     data->jpeg_buff.ptr, data->jpeg_buff_len,
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
        
    val2str(str, data->voltage_mean_kv, "kV mean");
    sdl_render_text(data_pane, 0, 0, 1, str, WHITE, BLACK);

    val2str(str, data->voltage_min_kv, "kV min");
    sdl_render_text(data_pane, 1, 0, 1, str, WHITE, BLACK);

    val2str(str, data->voltage_max_kv, "kV max");
    sdl_render_text(data_pane, 2, 0, 1, str, WHITE, BLACK);

    val2str(str, data->current_ma, "mA");
    sdl_render_text(data_pane, 3, 0, 1, str, WHITE, BLACK);

    if (data->chamber_pressure_mtorr < 1000 || IS_ERROR(data->chamber_pressure_mtorr)) {
        sprintf(trailer_str, "mTorr CHMBR %s%s", 
                gas_get_name(data->gas_id),
                mode == LIVE_MODE ? "(g)" : "");
        val2str(str, data->chamber_pressure_mtorr, trailer_str);
    } else {
        sprintf(trailer_str, "Torr CHMBR %s%s", 
                gas_get_name(data->gas_id),
                mode == LIVE_MODE ? "(g)" : "");
        val2str(str, data->chamber_pressure_mtorr/1000, trailer_str);
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
