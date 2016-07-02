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

#ifndef __UTIL_SDL_H__
#define __UTIL_SDL_H__

//
// event support
//

// events
// - no event
#define SDL_EVENT_NONE                 0
// - ascii events 
#define SDL_EVENT_KEY_BS               8
#define SDL_EVENT_KEY_TAB              9
#define SDL_EVENT_KEY_ENTER            13
#define SDL_EVENT_KEY_ESC              27
// - special keys
#define SDL_EVENT_KEY_HOME             130
#define SDL_EVENT_KEY_END              131
#define SDL_EVENT_KEY_PGUP             132
#define SDL_EVENT_KEY_PGDN             133
#define SDL_EVENT_KEY_UP_ARROW         134
#define SDL_EVENT_KEY_DOWN_ARROW       135
#define SDL_EVENT_KEY_LEFT_ARROW       136
#define SDL_EVENT_KEY_RIGHT_ARROW      137
#define SDL_EVENT_KEY_CTRL_LEFT_ARROW  138
#define SDL_EVENT_KEY_CTRL_RIGHT_ARROW 139
#define SDL_EVENT_KEY_ALT_LEFT_ARROW   140
#define SDL_EVENT_KEY_ALT_RIGHT_ARROW  141
#define SDL_EVENT_KEY_SHIFT            142
#define SDL_EVENT_KEY_UNSHIFT          143
// - window
#define SDL_EVENT_WIN_SIZE_CHANGE      145
#define SDL_EVENT_WIN_MINIMIZED        146
#define SDL_EVENT_WIN_RESTORED         147
// - quit
#define SDL_EVENT_QUIT                 150
// - available to be defined by users
#define SDL_EVENT_USER_START           160  
#define SDL_EVENT_USER_END             999  
// - max event
#define SDL_EVENT_MAX                  1000

// event types
#define SDL_EVENT_TYPE_NONE         0
#define SDL_EVENT_TYPE_TEXT         1
#define SDL_EVENT_TYPE_MOUSE_CLICK  2
#define SDL_EVENT_TYPE_MOUSE_MOTION 3
#define SDL_EVENT_TYPE_MOUSE_WHEEL  4
#define SDL_EVENT_TYPE_KEY          5

// event data structure
typedef struct {
    int32_t event;
    union {
        struct {
            int32_t x;
            int32_t y;
        } mouse_click;
        struct {
            int32_t delta_x;
            int32_t delta_y;;
        } mouse_motion;
        struct {
            int32_t delta_x;
            int32_t delta_y;;
        } mouse_wheel;
    };
} sdl_event_t;

//
// available colors
//

#define PURPLE     0 
#define BLUE       1
#define LIGHT_BLUE 2
#define GREEN      3
#define YELLOW     4
#define ORANGE     5
#define RED        6
#define GRAY       7
#define PINK       8
#define WHITE      9
#define BLACK      10

//
// typedefs
//

typedef void * texture_t;

typedef struct {
    int16_t x, y;
    uint16_t w, h;
} rect_t;

typedef struct {
    int32_t x, y;
} point_t;

//
// prototypes
//

// window initialize, and get_state
int32_t sdl_init(uint32_t w, uint32_t h);
void sdl_get_state(int32_t * win_width, int32_t * win_height, bool * win_minimized);

// display init and present
void sdl_display_init(void);
void sdl_display_present(void);

// pane support
void sdl_init_pane(rect_t * pane, rect_t * rect, int16_t x, int16_t y, uint16_t w, uint16_t h);
int32_t sdl_pane_cols(rect_t * rect, int32_t fid);
int32_t sdl_pane_rows(rect_t * rect, int32_t fid);
void sdl_render_pane_border(rect_t * pane_full, int32_t color);

// font support
int32_t sdl_font_char_width(int32_t fid);
int32_t sdl_font_char_height(int32_t fid);

// event support
void sdl_event_register(int32_t event_id, int32_t event_type, rect_t * pos);
sdl_event_t * sdl_poll_event(void);

// render text
void sdl_render_text(rect_t * pane, int32_t row, int32_t col, int32_t font_id, char * str, 
    int32_t fd_color, int32_t bg_color);
void sdl_render_text_with_event(rect_t * pane, int32_t row, int32_t col, int32_t font_id, char * str, 
    int32_t fg_color, int32_t bg_color, int32_t event_id);

// render rectangle and lines
void sdl_render_rect(rect_t * pane, rect_t * rect, int32_t line_width, int32_t color);
void sdl_render_fill_rect(rect_t * pane, rect_t * rect, int32_t color);
void sdl_render_line(rect_t * pane, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color);
void sdl_render_lines(rect_t * pane, point_t * points, int32_t count, int32_t color);

// render using textures
texture_t sdl_create_yuy2_texture(int32_t w, int32_t h);
texture_t sdl_create_filled_circle_texture(int32_t radius, int32_t color);
texture_t sdl_create_text_texture(int32_t fg_color, int32_t bg_color, int32_t font_id, char * str);
void sdl_update_yuy2_texture(texture_t texture, uint8_t * pixels, int32_t pitch);
void sdl_query_texture(texture_t texture, int32_t * width, int32_t * height);
void sdl_render_texture(texture_t texture, rect_t * dstrect);
void sdl_destroy_texture(texture_t texture);

// predefined displays
void sdl_display_get_string(int32_t count, ...);
void sdl_display_text(char * text);
void sdl_display_choose_from_list(char * title_str, char ** choices, int32_t max_choices, int32_t * selection);
void sdl_display_error(char * err_str0, char * err_str1, char * err_str2);

#endif
