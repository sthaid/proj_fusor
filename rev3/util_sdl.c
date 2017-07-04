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

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_mixer.h>
#include <png.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

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
    TTF_Font * font_underline;
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

static char             sdl_screenshot_prefix[100];

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
                               (255 << 24) | (105 << 16) | (180 << 8) | 255,     // PINK 
                               (255 << 24) | (  0 << 16) | (  0 << 8) | 255,     // RED         
                               (224 << 24) | (224 << 16) | (224 << 8) | 255,     // GRAY        
                               (255 << 24) | (255 << 16) | (255 << 8) | 255,     // WHITE       
                               (  0 << 24) | (  0 << 16) | (  0 << 8) | 255,     // BLACK       
                                        };

//
// prototypes
//

static void sdl_exit_handler(void);
static void play_event_sound(void);
static void print_screen(void);
static void sdl_set_color(int32_t color); 

// -----------------  SDL INIT & CLOSE  --------------------------------- 

int32_t sdl_init(uint32_t w, uint32_t h, char * screenshot_prefix)
{
    #define SDL_FLAGS SDL_WINDOW_RESIZABLE

    char  * font0_path, * font1_path;
    int32_t font0_ptsize, font1_ptsize;

    // save copy of screenshot_prefix
    strcpy(sdl_screenshot_prefix, screenshot_prefix);

    // display available and current video drivers
    int num, i;
    num = SDL_GetNumVideoDrivers();
    INFO("Available Video Drivers: ");
    for (i = 0; i < num; i++) {
        INFO("   %s\n",  SDL_GetVideoDriver(i));
    }

    // initialize Simple DirectMedia Layer  (SDL)
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) < 0) {
        ERROR("SDL_Init failed\n");
        return -1;
    }

    // create SDL Window and Renderer
    if (SDL_CreateWindowAndRenderer(w, h, SDL_FLAGS, &sdl_window, &sdl_renderer) != 0) {
        ERROR("SDL_CreateWindowAndRenderer failed\n");
        return -1;
    }
    SDL_GetWindowSize(sdl_window, &sdl_win_width, &sdl_win_height);
    INFO("sdl_win_width=%d sdl_win_height=%d\n", sdl_win_width, sdl_win_height);

#if 0
    // init button_sound
    if (Mix_OpenAudio( 22050, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        WARN("Mix_OpenAudio failed\n");
    } else {
        sdl_button_sound = Mix_QuickLoad_WAV(button_sound_wav);
        if (sdl_button_sound == NULL) {
            ERROR("Mix_QuickLoadWAV failed\n");
            return -1;
        }
        Mix_VolumeChunk(sdl_button_sound,MIX_MAX_VOLUME/2);
    }
#endif

    // initialize True Type Font
    if (TTF_Init() < 0) {
        ERROR("TTF_Init failed\n");
        return -1;
    }

    font0_path = "fonts/FreeMonoBold.ttf";         // normal 
    font0_ptsize = sdl_win_height / 30 - 1;
    font1_path = "fonts/FreeMonoBold.ttf";         // large
    font1_ptsize = sdl_win_height / 18 - 1;

    sdl_font[0].font = TTF_OpenFont(font0_path, font0_ptsize);
    if (sdl_font[0].font == NULL) {
        ERROR("failed TTF_OpenFont %s\n", font0_path);
        return -1;
    }
    sdl_font[0].font_underline = TTF_OpenFont(font0_path, font0_ptsize);
    if (sdl_font[0].font == NULL) {
        ERROR("failed TTF_OpenFont %s\n", font0_path);
        return -1;
    }
    TTF_SizeText(sdl_font[0].font, "X", &sdl_font[0].char_width, &sdl_font[0].char_height);
    TTF_SetFontStyle(sdl_font[0].font_underline, TTF_STYLE_UNDERLINE|TTF_STYLE_BOLD);
    INFO("font0 psize=%d width=%d height=%d\n", 
         font0_ptsize, sdl_font[0].char_width, sdl_font[0].char_height);

    sdl_font[1].font = TTF_OpenFont(font1_path, font1_ptsize);
    if (sdl_font[1].font == NULL) {
        ERROR("failed TTF_OpenFont %s\n", font1_path);
        return -1;
    }
    sdl_font[1].font_underline = TTF_OpenFont(font1_path, font1_ptsize);
    if (sdl_font[1].font == NULL) {
        ERROR("failed TTF_OpenFont %s\n", font1_path);
        return -1;
    }
    TTF_SizeText(sdl_font[1].font, "X", &sdl_font[1].char_width, &sdl_font[1].char_height);
    TTF_SetFontStyle(sdl_font[1].font_underline, TTF_STYLE_UNDERLINE|TTF_STYLE_BOLD);
    INFO("font1 psize=%d width=%d height=%d\n", 
         font1_ptsize, sdl_font[1].char_width, sdl_font[1].char_height);

    // register exit handler
    atexit(sdl_exit_handler);

    // return success
    INFO("success\n");
    return 0;
}

static void sdl_exit_handler(void)
{
    int32_t i;
    
    usleep(1000000);

    if (sdl_button_sound) {
        Mix_FreeChunk(sdl_button_sound);
        Mix_CloseAudio();
    }

    for (i = 0; i < MAX_FONT; i++) {
        TTF_CloseFont(sdl_font[i].font);
    }
    TTF_Quit();

    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
}

void sdl_get_state(uint32_t * win_width, uint32_t * win_height, bool * win_minimized)
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

void sdl_init_pane(rect_t * pane_full, rect_t * pane, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
    pane_full->x = x;
    pane_full->y = y;
    pane_full->w = w;
    pane_full->h = h;

    pane->x = x + 2;
    pane->y = y + 2;
    pane->w = w - 4;
    pane->h = h - 4;
}

int32_t sdl_pane_cols(rect_t * r, int32_t fid)
{
    return r->w / sdl_font[fid].char_width;
}

int32_t sdl_pane_rows(rect_t * r, int32_t fid)
{
    return r->h / sdl_font[fid].char_height;
}

void sdl_render_pane_border(rect_t * pane_full, int32_t color)
{
    rect_t rect;

    rect.x = 0;
    rect.y = 0;
    rect.w = pane_full->w;
    rect.h = pane_full->h;

    sdl_render_rect(pane_full, &rect, 2, color);
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

    if (pos_arg) {
        pos.x = pos_arg->x;
        pos.y = pos_arg->y;
        pos.w = pos_arg->w;
        pos.h = pos_arg->h;
    } else {
        pos.x = 0;
        pos.y = 0;
        pos.w = 0;
        pos.h = 0;
    }

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
            bool     alt = (ev.key.keysym.mod & KMOD_ALT) != 0;
            int32_t  possible_event = -1;

            if (ctrl && key == 'p') {
                print_screen();
                play_event_sound();
                event.event = SDL_EVENT_SCREENSHOT_TAKEN;
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
            } else if (key == SDLK_UP) {
                possible_event = SDL_EVENT_KEY_UP_ARROW;
            } else if (key == SDLK_DOWN) {
                possible_event = SDL_EVENT_KEY_DOWN_ARROW;
            } else if (key == SDLK_LEFT) {
                possible_event = SDL_EVENT_KEY_LEFT_ARROW;
            } else if (key == SDLK_RIGHT) {
                possible_event = SDL_EVENT_KEY_RIGHT_ARROW;
            } else if (key == SDLK_RSHIFT || key == SDLK_LSHIFT) {
                possible_event = SDL_EVENT_KEY_SHIFT;
            }

            if (shift) {
                if (possible_event >= 'a' && possible_event <= 'z') {
                    possible_event = toupper(possible_event);
                } else if (possible_event >= '0' && possible_event <= '9') {
                    possible_event = ")!@#$%^&*("[possible_event-'0'];
                } else if (possible_event == '-') {
                    possible_event = '_';
                } else if (possible_event == '=') {
                    possible_event = '+';
                } else if (possible_event == '/') {
                    possible_event = '?';
                } else if (possible_event == SDL_EVENT_KEY_ESC) {
                    possible_event = SDL_EVENT_KEY_SHIFT_ESC;
                }
            } else if (ctrl) {
                if (possible_event == SDL_EVENT_KEY_LEFT_ARROW) {
                    possible_event = SDL_EVENT_KEY_CTRL_LEFT_ARROW;
                } else if (possible_event == SDL_EVENT_KEY_RIGHT_ARROW) {
                    possible_event = SDL_EVENT_KEY_CTRL_RIGHT_ARROW;
                }
            } else if (alt) {
                if (possible_event == SDL_EVENT_KEY_LEFT_ARROW) {
                    possible_event = SDL_EVENT_KEY_ALT_LEFT_ARROW;
                } else if (possible_event == SDL_EVENT_KEY_RIGHT_ARROW) {
                    possible_event = SDL_EVENT_KEY_ALT_RIGHT_ARROW;
                }
            }

            if ((possible_event != -1) && 
                (sdl_event_reg_tbl[possible_event].type == SDL_EVENT_TYPE_KEY ||
                 sdl_event_reg_tbl[possible_event].type == SDL_EVENT_TYPE_TEXT))
            {
                event.event = possible_event;
                play_event_sound();
            }
            break; }

        case SDL_KEYUP: {
            int32_t  key = ev.key.keysym.sym;
            bool     shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;

            if ((key == SDLK_RSHIFT || key == SDLK_LSHIFT) && !shift) {
                event.event = SDL_EVENT_KEY_UNSHIFT;
                play_event_sound();
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
    if (sdl_button_sound) {
        Mix_PlayChannel(-1, sdl_button_sound, 0);
    }
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
    // creeate the file_name ...
    // if no sdl_screenshot_prefix then
    //   filename is based on localtime
    // else
    //   filename 'prefix_N.png'
    // endif
    //

    if (sdl_screenshot_prefix[0] == '\0') {
        t = time(NULL);
        localtime_r(&t, &tm);
        sprintf(file_name, "%s_screenshot_%2.2d%2.2d%2.2d_%2.2d%2.2d%2.2d.png",
                sdl_screenshot_prefix,
                tm.tm_year-100, tm.tm_mon+1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        bool file_name_found = false;
        struct stat buf;
        int32_t i;
        for (i = 1; i < 1000; i++) {
            sprintf(file_name, "%s_screenshot_%d.png",
                    sdl_screenshot_prefix, i);
            if (stat(file_name,&buf) == -1 && errno == ENOENT) {
                file_name_found = true;
                break;
            }
        }
        if (!file_name_found) {
            ERROR("unable to select file_name for screenshot, prefix=%s\n", 
                  sdl_screenshot_prefix);
            goto done;
        }
    }

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

// -----------------  RENDER TEXT  -------------------------------------- 

void sdl_render_text(rect_t * pane, int32_t row, int32_t col, int32_t font_id, char * str, 
        int32_t fg_color, int32_t bg_color)
{
    sdl_render_text_with_event(pane, row, col, font_id, str, fg_color, bg_color, SDL_EVENT_NONE);
}

void sdl_render_text_with_event(rect_t * pane, int32_t row, int32_t col, int32_t font_id, char * str, 
        int32_t fg_color, int32_t bg_color, int32_t event_id)
{
    SDL_Surface    * surface = NULL;
    SDL_Texture    * texture = NULL;
    SDL_Rect         pos;
    SDL_Color        fg_sdl_color, bg_sdl_color;
    uint32_t         fg_rgba, bg_rgba;

    // init 
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

    // if zero length string then nothing to do
    if (str[0] == '\0') {
        return;
    }

    // render the text to a surface
    if (event_id == SDL_EVENT_NONE) {
        surface = TTF_RenderText_Shaded(sdl_font[font_id].font, str, fg_sdl_color, bg_sdl_color);
    } else {
        surface = TTF_RenderText_Shaded(sdl_font[font_id].font_underline, str, fg_sdl_color, bg_sdl_color);
    }
    if (surface == NULL) { 
        ERROR("TTF_RenderText_Shaded returned NULL\n");
        return;
    }

    // determine the display location
    pos.x = pane->x + col * sdl_font[font_id].char_width;
    if (col < 0) {
        pos.x += pane->w;
    }
    pos.y = pane->y + row * sdl_font[font_id].char_height;
    if (row < 0) {
        pos.y += pane->h;
    }
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
    if (event_id != SDL_EVENT_NONE) {
        rect_t rect;

        rect.x = pos.x - sdl_font[font_id].char_width/2;
        rect.y = pos.y - sdl_font[font_id].char_height/2;
        rect.w = pos.w + sdl_font[font_id].char_width;
        rect.h = pos.h + sdl_font[font_id].char_height;

        sdl_event_register(event_id, SDL_EVENT_TYPE_TEXT, &rect);
    }
}

// -----------------  RENDER RECTANGLES & LINES  ------------------------ 

void sdl_render_rect(rect_t * pane, rect_t * rect_arg, int32_t line_width, int32_t color)
{
    SDL_Rect rect;
    int32_t i;

    sdl_set_color(color);

    rect.x = pane->x + rect_arg->x;
    rect.y = pane->y + rect_arg->y;
    rect.w = rect_arg->w;
    rect.h = rect_arg->h;

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

void sdl_render_fill_rect(rect_t * pane, rect_t * rect_arg, int32_t color)
{
    SDL_Rect rect;

    sdl_set_color(color);

    rect.x = pane->x + rect_arg->x;
    rect.y = pane->y + rect_arg->y;
    rect.w = rect_arg->w;
    rect.h = rect_arg->h;
    SDL_RenderFillRect(sdl_renderer, &rect);
}

void sdl_render_line(rect_t * pane, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color)
{
    sdl_set_color(color);

    x1 += pane->x;
    y1 += pane->y;
    x2 += pane->x;
    y2 += pane->y;

    SDL_RenderDrawLine(sdl_renderer, x1,y1,x2,y2);
}

void sdl_render_lines(rect_t * pane, point_t * points, int32_t count, int32_t color)
{
    int32_t i;

    if (count <= 1) {
        return;
    }

    sdl_set_color(color);

    if (pane->x || pane->y) {
        point_t new_points[count];
        for (i = 0; i < count; i++) {
            new_points[i].x = points[i].x + pane->x;
            new_points[i].y = points[i].y + pane->y;
        }
        SDL_RenderDrawLines(sdl_renderer, (SDL_Point*)new_points, count);
    } else {
        SDL_RenderDrawLines(sdl_renderer, (SDL_Point*)points, count);
    }
}

static void sdl_set_color(int32_t color)
{
    uint8_t r, g, b, a;
    uint32_t rgba;

    rgba = sdl_color_to_rgba[color];
    r = (rgba >> 24) & 0xff;
    g = (rgba >> 16) & 0xff;
    b = (rgba >>  8) & 0xff;
    a = (rgba      ) & 0xff;
    SDL_SetRenderDrawColor(sdl_renderer, r, g, b, a);
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

void sdl_update_yuy2_texture(texture_t texture, uint8_t * pixels, int32_t pitch) 
{
    int32_t width, height;

    sdl_query_texture(texture, &width, &height);

    SDL_UpdateTexture((SDL_Texture*)texture,
                      NULL,            // update entire texture
                      pixels,          // pixels
                      pitch*2);        // pitch
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

    SDL_RenderCopy(sdl_renderer, texture, NULL, &dstrect);
}

void sdl_destroy_texture(texture_t texture)
{
    if (texture) {
        SDL_DestroyTexture((SDL_Texture *)texture);
    }
}
