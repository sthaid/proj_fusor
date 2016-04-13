#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_mixer.h>
#include <png.h>

#include "util_sdl.h"
#include "util_sdl_button_sound.h"
#include "util_misc.h"

//
// defines
//

#define MAX_FONT 2

#define EVENT_INIT \
    do { \
        bzero(sdl_event_reg_tbl, sizeof(sdl_event_reg_tbl)); \
        sdl_event_max = 0; \
    } while (0)

//
// typedefs
//

typedef struct {
    TTF_Font * font;
    int32_t    char_width;
    int32_t    char_height;
} sdl_font_t;

typedef struct {
    SDL_Rect pos;
    int32_t type;
} sdl_event_reg_t;

//
// variables
//

static SDL_Window     * sdl_window;
static SDL_Renderer   * sdl_renderer;
static int32_t          sdl_win_width;
static int32_t          sdl_win_height;
static bool             sdl_win_minimized;

static Mix_Chunk      * sdl_button_sound;

static sdl_font_t       sdl_font[MAX_FONT];

static sdl_event_reg_t  sdl_event_reg_tbl[SDL_EVENT_MAX];
static int32_t          sdl_event_max;

static uint32_t         sdl_color_to_rgba[] = {
                            //    red           green          blue    alpha
                               (127 << 24) | (  0 << 16) | (255 << 8) | 255,     // PURPLE
                               (  0 << 24) | (  0 << 16) | (255 << 8) | 255,     // BLUE
                               (  0 << 24) | (255 << 16) | (255 << 8) | 255,     // LIGHT_BLUE
                               (  0 << 24) | (255 << 16) | (  0 << 8) | 255,     // GREEN
                               (255 << 24) | (255 << 16) | (  0 << 8) | 255,     // YELLOW
                               (255 << 24) | (128 << 16) | (  0 << 8) | 255,     // ORANGE
                               (255 << 24) | (  0 << 16) | (  0 << 8) | 255,     // RED         
                               (224 << 24) | (224 << 16) | (224 << 8) | 255,     // GRAY        
                               (255 << 24) | (105 << 16) | (180 << 8) | 255,     // PINK 
                               (255 << 24) | (255 << 16) | (255 << 8) | 255,     // WHITE       
                               (  0 << 24) | (  0 << 16) | (  0 << 8) | 255,     // BLACK       
                                        };

//
// prototypes
//

static void play_event_sound(void);
static void print_screen(void);

// -----------------  SDL INIT & CLOSE  --------------------------------- 

void sdl_init(uint32_t w, uint32_t h)
{
    #define SDL_FLAGS SDL_WINDOW_RESIZABLE

    char  * font0_path, * font1_path;
    int32_t font0_ptsize, font1_ptsize;

    // initialize Simple DirectMedia Layer  (SDL)
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) < 0) {
        FATAL("SDL_Init failed\n");
    }

    // create SDL Window and Renderer
    if (SDL_CreateWindowAndRenderer(w, h, SDL_FLAGS, &sdl_window, &sdl_renderer) != 0) {
        FATAL("SDL_CreateWindowAndRenderer failed\n");
    }
    SDL_GetWindowSize(sdl_window, &sdl_win_width, &sdl_win_height);
    INFO("sdl_win_width=%d sdl_win_height=%d\n", sdl_win_width, sdl_win_height);

    // init button_sound
    if (Mix_OpenAudio( 22050, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        FATAL("Mix_OpenAudio failed\n");
    }
    sdl_button_sound = Mix_QuickLoad_WAV(button_sound_wav);
    if (sdl_button_sound == NULL) {
        FATAL("Mix_QuickLoadWAV failed\n");
    }
    Mix_VolumeChunk(sdl_button_sound,MIX_MAX_VOLUME/2);

    // initialize True Type Font
    if (TTF_Init() < 0) {
        FATAL("TTF_Init failed\n");
    }

    font0_path = "fonts/FreeMonoBold.ttf";         // normal 
    font0_ptsize = sdl_win_height / 18 - 1;
    font1_path = "fonts/FreeMonoBold.ttf";         // extra large, for keyboard
    font1_ptsize = sdl_win_height / 13 - 1;

    sdl_font[0].font = TTF_OpenFont(font0_path, font0_ptsize);
    if (sdl_font[0].font == NULL) {
        FATAL("failed TTF_OpenFont %s\n", font0_path);
    }
    TTF_SizeText(sdl_font[0].font, "X", &sdl_font[0].char_width, &sdl_font[0].char_height);
    INFO("font0 psize=%d width=%d height=%d\n", 
         font0_ptsize, sdl_font[0].char_width, sdl_font[0].char_height);

    sdl_font[1].font = TTF_OpenFont(font1_path, font1_ptsize);
    if (sdl_font[1].font == NULL) {
        FATAL("failed TTF_OpenFont %s\n", font1_path);
    }
    TTF_SizeText(sdl_font[1].font, "X", &sdl_font[1].char_width, &sdl_font[1].char_height);
    INFO("font1 psize=%d width=%d height=%d\n", 
         font1_ptsize, sdl_font[1].char_width, sdl_font[1].char_height);
}

void sdl_close(void)
{
    int32_t i;
    
    Mix_FreeChunk(sdl_button_sound);
    Mix_CloseAudio();

    for (i = 0; i < MAX_FONT; i++) {
        TTF_CloseFont(sdl_font[i].font);
    }
    TTF_Quit();

    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
}

void sdl_get_state(int32_t * win_width, int32_t * win_height, bool * win_minimized)
{
    if (win_width) {
        *win_width = sdl_win_width;
    }
    if (win_height) {
        *win_height = sdl_win_height;
    }
    if (win_minimized) {
        *win_minimized = sdl_win_minimized;
    }
}

// -----------------  DISPLAY INIT AND PRESENT  ------------------------- 

void sdl_display_init(void)
{
    EVENT_INIT;
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(sdl_renderer);
}

void sdl_display_present(void)
{
    SDL_RenderPresent(sdl_renderer);
}

// -----------------  PANE SUPPORT ROUTINES  ---------------------------- 

void sdl_init_pane(rect_t * r, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}

int32_t sdl_pane_cols(rect_t * r, int32_t fid)
{
    return r->w / sdl_font[fid].char_width;
}

int32_t sdl_pane_rows(rect_t * r, int32_t fid)
{
    return r->h / sdl_font[fid].char_height;
}

// -----------------  FONT SUPPORT ROUTINES  ---------------------------- 

int32_t sdl_font_char_width(int32_t fid)
{
    return sdl_font[fid].char_width;
}

int32_t sdl_font_char_height(int32_t fid)
{
    return sdl_font[fid].char_height;
}

// -----------------  EVENT HANDLING  ----------------------------------- 

void sdl_event_register(int32_t event_id, int32_t event_type, rect_t * pos_arg)
{
    SDL_Rect pos;

    pos.x = pos_arg->x;
    pos.y = pos_arg->y;
    pos.w = pos_arg->w;
    pos.h = pos_arg->h;

    sdl_event_reg_tbl[event_id].pos  = pos;
    sdl_event_reg_tbl[event_id].type = event_type;

    if (event_id >= sdl_event_max) {
        sdl_event_max = event_id + 1;
    }
}

sdl_event_t * sdl_poll_event(void)
{
    #define AT_POS(X,Y,pos) (((X) >= (pos).x) && \
                             ((X) < (pos).x + (pos).w) && \
                             ((Y) >= (pos).y) && \
                             ((Y) < (pos).y + (pos).h))

    #define SDL_WINDOWEVENT_STR(x) \
       ((x) == SDL_WINDOWEVENT_SHOWN        ? "SDL_WINDOWEVENT_SHOWN"        : \
        (x) == SDL_WINDOWEVENT_HIDDEN       ? "SDL_WINDOWEVENT_HIDDEN"       : \
        (x) == SDL_WINDOWEVENT_EXPOSED      ? "SDL_WINDOWEVENT_EXPOSED"      : \
        (x) == SDL_WINDOWEVENT_MOVED        ? "SDL_WINDOWEVENT_MOVED"        : \
        (x) == SDL_WINDOWEVENT_RESIZED      ? "SDL_WINDOWEVENT_RESIZED"      : \
        (x) == SDL_WINDOWEVENT_SIZE_CHANGED ? "SDL_WINDOWEVENT_SIZE_CHANGED" : \
        (x) == SDL_WINDOWEVENT_MINIMIZED    ? "SDL_WINDOWEVENT_MINIMIZED"    : \
        (x) == SDL_WINDOWEVENT_MAXIMIZED    ? "SDL_WINDOWEVENT_MAXIMIZED"    : \
        (x) == SDL_WINDOWEVENT_RESTORED     ? "SDL_WINDOWEVENT_RESTORED"     : \
        (x) == SDL_WINDOWEVENT_ENTER        ? "SDL_WINDOWEVENT_ENTER"        : \
        (x) == SDL_WINDOWEVENT_LEAVE        ? "SDL_WINDOWEVENT_LEAVE"        : \
        (x) == SDL_WINDOWEVENT_FOCUS_GAINED ? "SDL_WINDOWEVENT_FOCUS_GAINED" : \
        (x) == SDL_WINDOWEVENT_FOCUS_LOST   ? "SDL_WINDOWEVENT_FOCUS_LOST"   : \
        (x) == SDL_WINDOWEVENT_CLOSE        ? "SDL_WINDOWEVENT_CLOSE"        : \
                                              "????")

    #define MOUSE_BUTTON_STATE_NONE     0
    #define MOUSE_BUTTON_STATE_DOWN     1
    #define MOUSE_BUTTON_STATE_MOTION   2

    #define MOUSE_BUTTON_STATE_RESET \
        do { \
            mouse_button_state = MOUSE_BUTTON_STATE_NONE; \
            mouse_button_motion_event = SDL_EVENT_NONE; \
            mouse_button_x = 0; \
            mouse_button_y = 0; \
        } while (0)

    SDL_Event ev;
    int32_t i;

    static sdl_event_t event;
    static int32_t     mouse_button_state; 
    static int32_t     mouse_button_motion_event;
    static int32_t     mouse_button_x;
    static int32_t     mouse_button_y;

    bzero(&event, sizeof(event));
    event.event = SDL_EVENT_NONE;

    while (true) {
        // get the next event, break out of loop if no event
        if (SDL_PollEvent(&ev) == 0) {
            break;
        }

        // process the SDL event, this code
        // - sets event and sdl_quit
        // - updates sdl_win_width, sdl_win_height, sdl_win_minimized
        switch (ev.type) {
        case SDL_MOUSEBUTTONDOWN: {
            DEBUG("MOUSE DOWN which=%d button=%s state=%s x=%d y=%d\n",
                   ev.button.which,
                   (ev.button.button == SDL_BUTTON_LEFT   ? "LEFT" :
                    ev.button.button == SDL_BUTTON_MIDDLE ? "MIDDLE" :
                    ev.button.button == SDL_BUTTON_RIGHT  ? "RIGHT" :
                                                            "???"),
                   (ev.button.state == SDL_PRESSED  ? "PRESSED" :
                    ev.button.state == SDL_RELEASED ? "RELEASED" :
                                                      "???"),
                   ev.button.x,
                   ev.button.y);

            // if not the left button then get out
            if (ev.button.button != SDL_BUTTON_LEFT) {
                break;
            }

            // reset mouse_button_state
            MOUSE_BUTTON_STATE_RESET;

            // check for text or mouse-click event
            for (i = 0; i < sdl_event_max; i++) {
                if ((sdl_event_reg_tbl[i].type == SDL_EVENT_TYPE_TEXT ||
                     sdl_event_reg_tbl[i].type == SDL_EVENT_TYPE_MOUSE_CLICK) &&
                    AT_POS(ev.button.x, ev.button.y, sdl_event_reg_tbl[i].pos)) 
                {
                    event.event = i;
                    break;
                }
            }
            if (event.event != SDL_EVENT_NONE) {
                play_event_sound();
                break;
            }

            // it is not a text event, so set MOUSE_BUTTON_STATE_DOWN
            mouse_button_state = MOUSE_BUTTON_STATE_DOWN;
            mouse_button_x = ev.button.x;
            mouse_button_y = ev.button.y;
            break; }

        case SDL_MOUSEBUTTONUP: {
            DEBUG("MOUSE UP which=%d button=%s state=%s x=%d y=%d\n",
                   ev.button.which,
                   (ev.button.button == SDL_BUTTON_LEFT   ? "LEFT" :
                    ev.button.button == SDL_BUTTON_MIDDLE ? "MIDDLE" :
                    ev.button.button == SDL_BUTTON_RIGHT  ? "RIGHT" :
                                                            "???"),
                   (ev.button.state == SDL_PRESSED  ? "PRESSED" :
                    ev.button.state == SDL_RELEASED ? "RELEASED" :
                                                      "???"),
                   ev.button.x,
                   ev.button.y);

            // if not the left button then get out
            if (ev.button.button != SDL_BUTTON_LEFT) {
                break;
            }

            // reset mouse_button_state,
            // this is where MOUSE_BUTTON_STATE_MOTION state is exitted
            MOUSE_BUTTON_STATE_RESET;
            break; }

        case SDL_MOUSEMOTION: {
            // if MOUSE_BUTTON_STATE_NONE then get out
            if (mouse_button_state == MOUSE_BUTTON_STATE_NONE) {
                break;
            }

            // if in MOUSE_BUTTON_STATE_DOWN then check for mouse motion event; and if so
            // then set state to MOUSE_BUTTON_STATE_MOTION
            if (mouse_button_state == MOUSE_BUTTON_STATE_DOWN) {
                for (i = 0; i < sdl_event_max; i++) {
                    if (sdl_event_reg_tbl[i].type == SDL_EVENT_TYPE_MOUSE_MOTION &&
                        AT_POS(ev.motion.x, ev.motion.y, sdl_event_reg_tbl[i].pos)) 
                    {
                        mouse_button_state = MOUSE_BUTTON_STATE_MOTION;
                        mouse_button_motion_event = i;
                        break;
                    }
                }
            }

            // if did not find mouse_motion_event and not already in MOUSE_BUTTON_STATE_MOTION then 
            // reset mouse_button_state, and get out
            if (mouse_button_state != MOUSE_BUTTON_STATE_MOTION) {
                MOUSE_BUTTON_STATE_RESET;
                break;
            }

            // get all additional pending mouse motion events, and sum the motion
            event.event = mouse_button_motion_event;
            event.mouse_motion.delta_x = 0;
            event.mouse_motion.delta_y = 0;
            do {
                event.mouse_motion.delta_x += ev.motion.x - mouse_button_x;
                event.mouse_motion.delta_y += ev.motion.y - mouse_button_y;
                mouse_button_x = ev.motion.x;
                mouse_button_y = ev.motion.y;
            } while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION) == 1);
            break; }

        case SDL_MOUSEWHEEL: {
            int32_t mouse_x, mouse_y;

            // check if mouse wheel event is registered for the location of the mouse
            SDL_GetMouseState(&mouse_x, &mouse_y);
            for (i = 0; i < sdl_event_max; i++) {
                if (sdl_event_reg_tbl[i].type == SDL_EVENT_TYPE_MOUSE_WHEEL &&
                    AT_POS(mouse_x, mouse_y, sdl_event_reg_tbl[i].pos)) 
                {
                    break;
                }
            }

            // if did not find a registered mouse wheel event then get out
            if (i == sdl_event_max) {
                break;
            }

            // set return event
            event.event = i;
            event.mouse_wheel.delta_x = ev.wheel.x;
            event.mouse_wheel.delta_y = ev.wheel.y;
            break; }

        case SDL_KEYDOWN: {
            int32_t  key = ev.key.keysym.sym;
            bool     shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
            bool     ctrl = (ev.key.keysym.mod & KMOD_CTRL) != 0;
            int32_t  possible_event = -1;

            if (ctrl) {
                if (key == 'p') {
                    print_screen();
                    play_event_sound();
                }
                break;
            }

            if (key < 128) {
                possible_event = key;
            } else if (key == SDLK_HOME) {
                possible_event = SDL_EVENT_KEY_HOME;
            } else if (key == SDLK_END) {
                possible_event = SDL_EVENT_KEY_END;
            } else if (key == SDLK_PAGEUP) {
                possible_event= SDL_EVENT_KEY_PGUP;
            } else if (key == SDLK_PAGEDOWN) {
                possible_event = SDL_EVENT_KEY_PGDN;
            }

            if (possible_event != -1) {
                if ((possible_event >= 'a' && possible_event <= 'z') &&
                    (sdl_event_reg_tbl[possible_event].pos.w ||
                     sdl_event_reg_tbl[toupper(possible_event)].pos.w))
                {
                    event.event = !shift ? possible_event : toupper(possible_event);
                    play_event_sound();
                } else if (sdl_event_reg_tbl[possible_event].pos.w) {
                    event.event = possible_event;
                    play_event_sound();
                }
            }
            break; }

       case SDL_WINDOWEVENT: {
            DEBUG("got event SDL_WINOWEVENT - %s\n", SDL_WINDOWEVENT_STR(ev.window.event));
            switch (ev.window.event)  {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                sdl_win_width = ev.window.data1;
                sdl_win_height = ev.window.data2;
                event.event = SDL_EVENT_WIN_SIZE_CHANGE;
                break;
            case SDL_WINDOWEVENT_MINIMIZED:
                sdl_win_minimized = true;
                event.event = SDL_EVENT_WIN_MINIMIZED;
                break;
            case SDL_WINDOWEVENT_RESTORED:
                sdl_win_minimized = false;
                event.event = SDL_EVENT_WIN_RESTORED;
                break;
            }
            break; }

        case SDL_QUIT: {
            DEBUG("got event SDL_QUIT\n");
            event.event = SDL_EVENT_QUIT;
            play_event_sound();
            break; }

        default: {
            DEBUG("got event %d - not supported\n", ev.type);
            break; }
        }

        // break if event is set
        if (event.event != SDL_EVENT_NONE) {
            break; 
        }
    }

    return &event;
}

static void play_event_sound(void)   
{
    Mix_PlayChannel(-1, sdl_button_sound, 0);
}

static void print_screen(void) 
{
    int32_t   (*pixels)[4096] = NULL;
    FILE       *fp = NULL;
    SDL_Rect    rect;
    int32_t    *row_pointers[4096];
    char        file_name[1000];
    png_structp png_ptr;
    png_infop   info_ptr;
    png_byte    color_type, bit_depth;
    int32_t     y, ret;
    time_t      t;
    struct tm   tm;

    //
    // allocate and read the pixels
    //

    pixels = calloc(1, sdl_win_height*sizeof(pixels[0]));
    if (pixels == NULL) {
        ERROR("allocate pixels failed\n");
        goto done;
    }

    rect.x = 0;
    rect.y = 0;
    rect.w = sdl_win_width;
    rect.h = sdl_win_height;   
    ret = SDL_RenderReadPixels(sdl_renderer, &rect, SDL_PIXELFORMAT_ABGR8888, pixels, sizeof(pixels[0]));
    if (ret < 0) {
        ERROR("SDL_RenderReadPixels, %s\n", SDL_GetError());
        goto done;
    }

    //
    // creeate the file_name using localtime
    //

    t = time(NULL);
    localtime_r(&t, &tm);
    sprintf(file_name, "entropy_%2.2d%2.2d%2.2d_%2.2d%2.2d%2.2d.png",
            tm.tm_year - 100, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);

    //
    // write pixels to file_name ...
    //

    // init
    color_type = PNG_COLOR_TYPE_RGB_ALPHA;
    bit_depth = 8;
    for (y = 0; y < sdl_win_height; y++) {
        row_pointers[y] = pixels[y];
    }

    // create file 
    fp = fopen(file_name, "wb");
    if (!fp) {
        ERROR("fopen %s\n", file_name);
        goto done;  
    }

    // initialize stuff 
    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ERROR("png_create_write_struct\n");
        goto done;  
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ERROR("png_create_info_struct\n");
        goto done;  
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ERROR("init_io failed\n");
        goto done;  
    }
    png_init_io(png_ptr, fp);

    // write header 
    if (setjmp(png_jmpbuf(png_ptr))) {
        ERROR("writing header\n");
        goto done;  
    }
    png_set_IHDR(png_ptr, info_ptr, sdl_win_width, sdl_win_height,
                 bit_depth, color_type, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png_ptr, info_ptr);

    // write bytes 
    if (setjmp(png_jmpbuf(png_ptr))) {
        ERROR("writing bytes\n");
        goto done;  
    }
    png_write_image(png_ptr, (void*)row_pointers);

    // end write 
    if (setjmp(png_jmpbuf(png_ptr))) {
        ERROR("end of write\n");
        goto done;  
    }
    png_write_end(png_ptr, NULL);

done:
    //
    // clean up and return
    //
    if (fp != NULL) {
        fclose(fp);
    }
    free(pixels);
}

// -----------------  RENDER TEXT WITH EVENT HANDLING ------------------- 

void sdl_render_text_font0(rect_t * pane, int32_t row, int32_t col, char * str, int32_t event)
{
    sdl_render_text_ex(pane, row, col, str, event, sdl_pane_cols(pane,0)-col, false, 0);
}

void sdl_render_text_font1(rect_t * pane, int32_t row, int32_t col, char * str, int32_t event)
{
    sdl_render_text_ex(pane, row, col, str, event, sdl_pane_cols(pane,1)-col, false, 1);
}

void sdl_render_text_ex(rect_t * pane, int32_t row, int32_t col, char * str, int32_t event, 
        int32_t field_cols, bool center, int32_t font_id)
{
    SDL_Surface    * surface = NULL;
    SDL_Texture    * texture = NULL;
    SDL_Color        fg_color;
    SDL_Rect         pos;
    char             s[500];
    int32_t          slen;

    static SDL_Color fg_color_normal = {255,255,255,255}; 
    static SDL_Color fg_color_event  = {0,255,255,255}; 
    static SDL_Color bg_color        = {0,0,0,255}; 

    // if zero length string then nothing to do
    if (str[0] == '\0') {
        return;
    }

    // if row or col are less than zero then adjust 
    if (row < 0) {
        row += sdl_pane_rows(pane,font_id);
    }
    if (col < 0) {
        col += sdl_pane_cols(pane,font_id);
    }

    // if field_cols is zero then set to huge value (unlimiitted)
    if (field_cols == 0) {
        field_cols = 999;
    }

    // verify row, col, and field_cols
    if (row < 0 || row >= sdl_pane_rows(pane,font_id) || 
        col < 0 || col >= sdl_pane_cols(pane,font_id) ||
        field_cols <= 0) 
    {
        return;
    }

    // reduce field_cols if necessary to stay in pane
    if (field_cols > sdl_pane_cols(pane,font_id) - col) {
         field_cols = sdl_pane_cols(pane,font_id) - col;
    }

    // make a copy of the str arg, and shorten if necessary
    strcpy(s, str);
    slen = strlen(s);
    if (slen > field_cols) {
        s[field_cols] = '\0';
        slen = field_cols;
    }

    // if centered then adjust col
    if (center) {
        col += (field_cols - slen) / 2;
    }

    // render the text to a surface
    fg_color = (event != SDL_EVENT_NONE ? fg_color_event : fg_color_normal); 
    surface = TTF_RenderText_Shaded(sdl_font[font_id].font, s, fg_color, bg_color);
    if (surface == NULL) { 
        FATAL("TTF_RenderText_Shaded returned NULL\n");
    } 

    // determine the display location
    pos.x = pane->x + col * sdl_font[font_id].char_width;
    pos.y = pane->y + row * sdl_font[font_id].char_height;
    pos.w = surface->w;
    pos.h = surface->h;

    // create texture from the surface, and 
    // render the texture
    texture = SDL_CreateTextureFromSurface(sdl_renderer, surface); 
    SDL_RenderCopy(sdl_renderer, texture, NULL, &pos); 

    // clean up
    if (surface) {
        SDL_FreeSurface(surface); 
        surface = NULL;
    }
    if (texture) {
        SDL_DestroyTexture(texture); 
        texture = NULL;
    }

    // if there is a event then save the location for the event handler
    if (event != SDL_EVENT_NONE) {
        pos.x -= sdl_font[font_id].char_width/2;
        pos.y -= sdl_font[font_id].char_height/2;
        pos.w += sdl_font[font_id].char_width;
        pos.h += sdl_font[font_id].char_height;

        rect_t pos2;
        pos2.x = pos.x;
        pos2.y = pos.y;
        pos2.w = pos.w;
        pos2.h = pos.h;

        sdl_event_register(event, SDL_EVENT_TYPE_TEXT, &pos2);
    }
}

// -----------------  RENDER RECTANGLES & LINES  ------------------------ 

void sdl_render_rect(rect_t * rect_arg, int32_t line_width, int32_t color)
{
    SDL_Rect rect;
    int32_t i;
    uint8_t r, g, b, a;
    uint32_t rgba;

    rect.x = rect_arg->x;
    rect.y = rect_arg->y;
    rect.w = rect_arg->w;
    rect.h = rect_arg->h;

    rgba = sdl_color_to_rgba[color];

    r = (rgba >> 24) & 0xff;
    g = (rgba >> 16) & 0xff;
    b = (rgba >>  8) & 0xff;
    a = (rgba      ) & 0xff;

    SDL_SetRenderDrawColor(sdl_renderer, r, g, b, a);

    for (i = 0; i < line_width; i++) {
        SDL_RenderDrawRect(sdl_renderer, &rect);
        if (rect.w < 2 || rect.h < 2) {
            break;
        }
        rect.x += 1;
        rect.y += 1;
        rect.w -= 2;
        rect.h -= 2;
    }
}

// -----------------  RENDER USING TEXTURES  ---------------------------- 

texture_t sdl_create_yuy2_texture(int32_t w, int32_t h)
{
    SDL_Texture * texture;

    texture = SDL_CreateTexture(sdl_renderer,
                                SDL_PIXELFORMAT_YUY2,
                                SDL_TEXTUREACCESS_STREAMING,
                                w, h);
    if (texture == NULL) {
        ERROR("failed to allocate texture\n");
        return NULL;
    }

    return (texture_t)texture;
}

texture_t sdl_create_filled_circle_texture(int32_t radius, int32_t color)
{
    int32_t width = 2 * radius + 1;
    int32_t x = radius;
    int32_t y = 0;
    int32_t radiusError = 1-x;
    int32_t pixels[width][width];
    SDL_Texture * texture;
    uint32_t rgba = sdl_color_to_rgba[color];

    #define DRAWLINE(Y, XS, XE, V) \
        do { \
            int32_t i; \
            for (i = XS; i <= XE; i++) { \
                pixels[Y][i] = (V); \
            } \
        } while (0)

    // initialize pixels
    bzero(pixels,sizeof(pixels));
    while(x >= y) {
        DRAWLINE(y+radius, -x+radius, x+radius, rgba);
        DRAWLINE(x+radius, -y+radius, y+radius, rgba);
        DRAWLINE(-y+radius, -x+radius, x+radius, rgba);
        DRAWLINE(-x+radius, -y+radius, y+radius, rgba);
        y++;
        if (radiusError<0) {
            radiusError += 2 * y + 1;
        } else {
            x--;
            radiusError += 2 * (y - x) + 1;
        }
    }

    // create the texture and copy the pixels to the texture
    texture = SDL_CreateTexture(sdl_renderer,
                                SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_STATIC,
                                width, width);
    if (texture == NULL) {
        ERROR("failed to allocate texture\n");
        return NULL;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(texture, NULL, pixels, width*4);

    // return texture
    return (texture_t)texture;
}

texture_t sdl_create_text_texture(int32_t fg_color, int32_t bg_color, int32_t font_id, char * str)
{
    SDL_Surface * surface;
    SDL_Texture * texture;
    uint32_t      fg_rgba;
    uint32_t      bg_rgba;
    SDL_Color     fg_sdl_color;
    SDL_Color     bg_sdl_color;

#if 0 // XXX
    printf("fg=%d bg=%d fid=%d str='%s'\n",
        fg_color, bg_color, font_id, str);
#endif
    if (str[0] == '\0') {
        return NULL;
    }

    fg_rgba = sdl_color_to_rgba[fg_color];
    fg_sdl_color.r = (fg_rgba >> 24) & 0xff;
    fg_sdl_color.g = (fg_rgba >> 16) & 0xff;
    fg_sdl_color.b = (fg_rgba >>  8) & 0xff;
    fg_sdl_color.a = (fg_rgba >>  0) & 0xff;

    bg_rgba = sdl_color_to_rgba[bg_color];
    bg_sdl_color.r = (bg_rgba >> 24) & 0xff;
    bg_sdl_color.g = (bg_rgba >> 16) & 0xff;
    bg_sdl_color.b = (bg_rgba >>  8) & 0xff;
    bg_sdl_color.a = (bg_rgba >>  0) & 0xff;

    surface = TTF_RenderText_Shaded(sdl_font[font_id].font, str, fg_sdl_color, bg_sdl_color);
    if (surface == NULL) {
        ERROR("failed to allocate surface\n");
        return NULL;
    }
    texture = SDL_CreateTextureFromSurface(sdl_renderer, surface);
    if (texture == NULL) {
        ERROR("failed to allocate texture\n");
        SDL_FreeSurface(surface);
        return NULL;
    }
    SDL_FreeSurface(surface);

    return (texture_t)texture;
}

void sdl_update_yuy2_texture(texture_t texture, uint8_t * pixels) 
{
    int32_t width, height;

    // XXX time this - measured0 or 1
    //uint64_t start = microsec_timer();
    sdl_query_texture(texture, &width, &height);
    //printf("XXX DURATION query_texture = %ld us\n", (microsec_timer()-start));


    SDL_UpdateTexture((SDL_Texture*)texture,
                      NULL,            // update entire texture
                      pixels,          // pixels
                      width*2);        // pitch
}

void sdl_query_texture(texture_t texture, int32_t * width, int32_t * height)
{
    if (texture == NULL) {
        *width = 0;
        *height = 0;
        return;
    }

    SDL_QueryTexture((SDL_Texture *)texture, NULL, NULL, width, height);
}

void sdl_render_texture(texture_t texture, rect_t * dstrect_arg)
{
    SDL_Rect dstrect;

    if (texture == NULL) {
        return;
    }

    dstrect.x = dstrect_arg->x;
    dstrect.y = dstrect_arg->y;
    dstrect.w = dstrect_arg->w;
    dstrect.h = dstrect_arg->h;

#if 0 // XXX
    printf("RENDER %p %d %d %d %d \n", texture, 
        dstrect.x,
        dstrect.y,
        dstrect.w,
        dstrect.h);
#endif

    SDL_RenderCopy(sdl_renderer, texture, NULL, &dstrect);
}

void sdl_destroy_texture(texture_t texture)
{
    if (texture) {
        SDL_DestroyTexture((SDL_Texture *)texture);
    }
}
