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
#include <limits.h>

#include "util_sdl.h"
#include "util_misc.h"

//
// defines
//

#define SDL_EVENT_SHIFT           (SDL_EVENT_USER_START+0)
#define SDL_EVENT_MOUSE_MOTION    (SDL_EVENT_USER_START+1)
#define SDL_EVENT_MOUSE_WHEEL     (SDL_EVENT_USER_START+2)
#define SDL_EVENT_FIELD_SELECT    (SDL_EVENT_USER_START+3)    // through +9
#define SDL_EVENT_LIST_CHOICE     (SDL_EVENT_USER_START+10)   // through +49

// -----------------  PREDEFINED DISPLAYS  ----------------------------- 

void sdl_display_get_string(int32_t count, ...)
{
    char        * prompt_str[10]; 
    char        * curr_str[10];
    char        * ret_str[10];

    va_list       ap;
    rect_t        keybdpane_full, keybdpane; 
    char          str[200];
    int32_t       i;
    sdl_event_t * event;
    uint32_t      win_width, win_height;

    int32_t       field_select;
    bool          shift;

    // this supports up to 4 fields
    if (count > 4) {
        ERROR("count %d too big, max 4\n", count);
    }

    // transfer variable arg list to array
    va_start(ap, count);
    for (i = 0; i < count; i++) {
        prompt_str[i] = va_arg(ap, char*);
        curr_str[i] = va_arg(ap, char*);
        ret_str[i] = va_arg(ap, char*);
    }
    va_end(ap);

    // init ret_str to curr_str
    for (i = 0; i < count; i++) {
        strcpy(ret_str[i], curr_str[i]);
    }

    // other init
    field_select = 0;
    shift = false;
    
    // loop until ENTER or ESC event received
    while (true) {
        // short sleep
        usleep(5000);

        // init
        sdl_get_state(&win_width, &win_height, NULL);
        sdl_init_pane(&keybdpane_full, &keybdpane, 0, 0, win_width, win_height);
        sdl_display_init();

        // draw keyboard
        static char * row_chars_unshift[4] = { "1234567890_",
                                               "qwertyuiop",
                                               "asdfghjkl",
                                               "zxcvbnm,." };
        static char * row_chars_shift[4]   = { "1234567890_",
                                               "QWERTYUIOP",
                                               "ASDFGHJKL",
                                               "ZXCVBNM,." };
        char ** row_chars;
        int32_t r, c, i, j;

        row_chars = (!shift ? row_chars_unshift : row_chars_shift);
        r = sdl_pane_rows(&keybdpane,1) / 2 - 10;
        if (r < 0) {
            r = 0;
        }
        c = (sdl_pane_cols(&keybdpane,1) - 33) / 2;

        for (i = 0; i < 4; i++) {
            for (j = 0; row_chars[i][j] != '\0'; j++) {
                char s[2];
                s[0] = row_chars[i][j];
                s[1] = '\0';
                sdl_render_text_with_event(&keybdpane, r+2*i, c+3*j, 1, s, LIGHT_BLUE, BLACK, s[0]);
            }
        }
        sdl_render_text_with_event(&keybdpane, r+6, c+27, 1, "SPACE",  LIGHT_BLUE, BLACK, ' ');
        sdl_render_text_with_event(&keybdpane, r+8, c+0,  1, "SHIFT",  LIGHT_BLUE, BLACK, SDL_EVENT_SHIFT);
        sdl_render_text_with_event(&keybdpane, r+8, c+8,  1, "BS",     LIGHT_BLUE, BLACK, SDL_EVENT_KEY_BS);
        sdl_render_text_with_event(&keybdpane, r+8, c+13, 1, "TAB",    LIGHT_BLUE, BLACK, SDL_EVENT_KEY_TAB);
        sdl_render_text_with_event(&keybdpane, r+8, c+19, 1, "ESC",    LIGHT_BLUE, BLACK, SDL_EVENT_KEY_ESC);
        sdl_render_text_with_event(&keybdpane, r+8, c+25, 1, "ENTER",  LIGHT_BLUE, BLACK, SDL_EVENT_KEY_ENTER);
        sdl_event_register(SDL_EVENT_KEY_SHIFT,   SDL_EVENT_TYPE_KEY, NULL);
        sdl_event_register(SDL_EVENT_KEY_UNSHIFT, SDL_EVENT_TYPE_KEY, NULL);

        // draw prompts
        for (i = 0; i < count; i++) {
            sprintf(str, "%s: %s", prompt_str[i], ret_str[i]);
            if ((i == field_select) && ((microsec_timer() % 1000000) > 500000) ) {
                strcat(str, "_");
            }
            sdl_render_text_with_event(&keybdpane, 
                                       r + 10 + 2 * (i % 2),   // row
                                       i / 2 * sdl_pane_cols(&keybdpane,1) / 2,   // col
                                       1,  // font_id
                                       str, 
                                       LIGHT_BLUE, BLACK, 
                                       SDL_EVENT_FIELD_SELECT+i);
        }

        // present the display
        sdl_display_present();

        // handle events 
        while (true)  {
            event = sdl_poll_event();
            if (event->event == SDL_EVENT_NONE || event->event == SDL_EVENT_QUIT) {
                break;
            } else if (event->event == SDL_EVENT_SHIFT) {
                shift = !shift;
                break;
            } else if (event->event == SDL_EVENT_KEY_SHIFT) {
                shift = true;
                break;
            } else if (event->event == SDL_EVENT_KEY_UNSHIFT) {
                shift = false;
                break;
            } else if (event->event == SDL_EVENT_KEY_BS) {
                int32_t len = strlen(ret_str[field_select]);
                if (len > 0) {
                    ret_str[field_select][len-1] = '\0';
                }
            } else if (event->event == SDL_EVENT_KEY_TAB) {
                field_select = (field_select + 1) % count;
            } else if (event->event == SDL_EVENT_KEY_ENTER) {
                break;
            } else if (event->event == SDL_EVENT_KEY_ESC) {
                if (strcmp(ret_str[field_select], curr_str[field_select])) {
                    strcpy(ret_str[field_select], curr_str[field_select]);
                } else {
                    for (i = 0; i < count; i++) {
                        strcpy(ret_str[i], curr_str[i]);
                    }
                    break;
                }
            } else if (event->event >= SDL_EVENT_FIELD_SELECT && event->event < SDL_EVENT_FIELD_SELECT+count) {
                field_select = event->event - SDL_EVENT_FIELD_SELECT;
            } else if (event->event >= 0x20 && event->event <= 0x7e) {
                char s[2];
                s[0] = event->event;
                s[1] = '\0';
                strcat(ret_str[field_select], s);
            }
        }
        if (event->event == SDL_EVENT_KEY_ENTER || event->event == SDL_EVENT_QUIT) {
            break;
        }
    }
}

void sdl_display_text(char * text)
{
    rect_t         dstrect;
    texture_t    * texture = NULL;
    rect_t         disp_pane_full, disp_pane;
    sdl_event_t  * event;
    char         * p;
    int32_t        i;
    int32_t        lines_per_display;
    bool           done = false;
    int32_t        max_texture = 0;
    int32_t        max_texture_alloced = 0;
    int32_t        text_y = 0;
    int32_t        pixels_per_row = sdl_font_char_height(0);
    uint32_t       win_width, win_height;

    // create a texture for each line of text
    //
    // note - this could be improved by doing just the first portion initially,
    //        and then finish up in the loop below
    for (p = text; *p; ) {
        char  * newline;
        char    line[200];

        newline = strchr(p, '\n');
        if (newline) {
            memcpy(line, p, newline-p);
            line[newline-p] = '\0';
            p = newline + 1;
        } else {
            strcpy(line, p);
            p += strlen(line);
        }

        if (max_texture+1 > max_texture_alloced) {
            max_texture_alloced += 1000;
            texture = realloc(texture, max_texture_alloced*sizeof(void*));
        }

        texture[max_texture++] = sdl_create_text_texture(WHITE, BLACK, 0, line);
    }

    // loop until done
    while (!done) {
        // short delay
        usleep(5000);

        // init 
        sdl_get_state(&win_width, &win_height, NULL);
        sdl_init_pane(&disp_pane_full, &disp_pane, 0, 0, win_width, win_height);
        sdl_display_init();
        lines_per_display = win_height / pixels_per_row;

        // sanitize text_y, this is the location of the text that is displayed
        // at the top of the display
        if (text_y > pixels_per_row * (max_texture - lines_per_display + 2)) {
            text_y = pixels_per_row * (max_texture - lines_per_display + 2);
        }
        if (text_y < 0) {
            text_y = 0;
        } 

        // display the text
        for (i = 0; ; i++) {
            int32_t w,h;

            if (i == max_texture) {
                break;
            }

            sdl_query_texture(texture[i], &w, &h);
            dstrect.x = 0;
            dstrect.y = i*pixels_per_row - text_y;
            dstrect.w = w;
            dstrect.h = h;

            if (dstrect.y + dstrect.h < 0) {
                continue;
            }
            if (dstrect.y >= (signed)win_height) {
                break;
            }

            sdl_render_texture(texture[i], &dstrect);
        }

        // display controls 
        if (max_texture > lines_per_display) {
            sdl_render_text(&disp_pane, 0, -7, 0, "(HOME)", LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane, 2, -7, 0, "(END)",  LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane, 4, -7, 0, "(PGUP)", LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane, 6, -7, 0, "(PGDN)", LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane, 8, -7, 0, "(UP^)",  LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane,10, -7, 0, "(DNv)",  LIGHT_BLUE, BLACK);
            sdl_event_register(SDL_EVENT_KEY_HOME, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_END, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_PGUP, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_PGDN, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_UP_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_DOWN_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_TYPE_MOUSE_MOTION, &disp_pane);
            sdl_event_register(SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_TYPE_MOUSE_WHEEL, &disp_pane);
        }
        sdl_render_text(&disp_pane, -1, -7, 0, "(ESC)", LIGHT_BLUE, BLACK);
        sdl_event_register(SDL_EVENT_KEY_ESC, SDL_EVENT_TYPE_KEY, NULL);

        // present the display
        sdl_display_present();

        // check for event
        event = sdl_poll_event();
        switch (event->event) {
        case SDL_EVENT_MOUSE_MOTION:
            text_y -= event->mouse_motion.delta_y;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            text_y -= event->mouse_wheel.delta_y * 2 * pixels_per_row;
            break;
        case SDL_EVENT_KEY_HOME:
            text_y = 0;
            break;
        case SDL_EVENT_KEY_END:
            text_y = INT_MAX;
            break;
        case SDL_EVENT_KEY_PGUP:
            text_y -= (lines_per_display - 2) * pixels_per_row;
            break;
        case SDL_EVENT_KEY_PGDN:
            text_y += (lines_per_display - 2) * pixels_per_row;
            break;
        case SDL_EVENT_KEY_UP_ARROW:
            text_y -= pixels_per_row;
            break;
        case SDL_EVENT_KEY_DOWN_ARROW:
            text_y += pixels_per_row;
            break;
        case SDL_EVENT_KEY_ESC:
        case SDL_EVENT_QUIT: 
            done = true;
        }
    }

    // free allocations
    for (i = 0; i < max_texture; i++) {
        sdl_destroy_texture(texture[i]);
    }
}

void  sdl_display_choose_from_list(char * title_str, char ** choice, int32_t max_choice, int32_t * selection)
{
    texture_t      title_texture = NULL;
    texture_t    * texture = NULL;
    rect_t         disp_pane_full, disp_pane;
    sdl_event_t  * event;
    int32_t        i;
    int32_t        lines_per_display;
    int32_t        text_y = 0;
    bool           done = false;
    int32_t        pixels_per_row = sdl_font_char_height(0) + 2;
    uint32_t       win_width, win_height;

    // preset return
    *selection = -1;

    // create texture for title
    title_texture =  sdl_create_text_texture(WHITE, BLACK, 0, title_str);

    // create textures for each choice string
    texture = calloc(max_choice, sizeof(void*));
    if (texture == NULL) {
        ERROR("calloc texture, max_choice %d\n", max_choice);
        goto done;
    }
    for (i = 0; i < max_choice; i++) {
        texture[i] = sdl_create_text_texture(LIGHT_BLUE, BLACK, 0, choice[i]);
    }

    // loop until selection made, or aborted
    while (!done) {
        // short delay
        usleep(5000);

        // init 
        sdl_get_state(&win_width, &win_height, NULL);
        sdl_init_pane(&disp_pane_full, &disp_pane, 0, 0, win_width, win_height);
        sdl_display_init();
        lines_per_display = (win_height - 2*pixels_per_row) / pixels_per_row;

        // sanitize text_y, this is the location of the text that is displayed
        // at the top of the display
        if (text_y > pixels_per_row * (2*(max_choice+1)-1 - lines_per_display+2)) {
            text_y = pixels_per_row * (2*(max_choice+1)-1 - lines_per_display+2);
        }
        if (text_y < 0) {
            text_y = 0;
        }

        // display the title and choices, and register for choices events
        for (i = 0; ; i++) {
            int32_t w,h;
            rect_t dstrect;
            texture_t t;

            if (i == max_choice+1) {
                break;
            }

            t = (i == 0 ? title_texture : texture[i-1]);

            sdl_query_texture(t, &w, &h);
            dstrect.x = 0;
            dstrect.y = i * 2 * pixels_per_row - text_y;
            dstrect.w = w;
            dstrect.h = h;

            if (dstrect.y + dstrect.h < 0) {
                continue;
            }
            if (dstrect.y >= win_height) {
                break;
            }

            sdl_render_texture(t, &dstrect);

            if (i >= 1) {
                dstrect.x -= sdl_font_char_width(0) / 2;
                dstrect.y -= sdl_font_char_height(0) / 2;
                dstrect.w += sdl_font_char_width(0);
                dstrect.h += sdl_font_char_height(0);

                sdl_event_register(SDL_EVENT_LIST_CHOICE+i-1, SDL_EVENT_TYPE_TEXT, &dstrect);
            }
        }

        // display controls 
        if (2*(max_choice+1)-1 > lines_per_display) {
            sdl_render_text(&disp_pane, 0, -7, 0, "(HOME)", LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane, 2, -7, 0, "(END)",  LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane, 4, -7, 0, "(PGUP)", LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane, 6, -7, 0, "(PGDN)", LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane, 8, -7, 0, "(UP^)",  LIGHT_BLUE, BLACK);
            sdl_render_text(&disp_pane,10, -7, 0, "(DNv)",  LIGHT_BLUE, BLACK);
            sdl_event_register(SDL_EVENT_KEY_HOME, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_END, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_PGUP, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_PGDN, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_UP_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_KEY_DOWN_ARROW, SDL_EVENT_TYPE_KEY, NULL);
            sdl_event_register(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_TYPE_MOUSE_MOTION, &disp_pane);
            sdl_event_register(SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_TYPE_MOUSE_WHEEL, &disp_pane);
        }
        sdl_render_text(&disp_pane, -1, -7, 0, "(ESC)", LIGHT_BLUE, BLACK);
        sdl_event_register(SDL_EVENT_KEY_ESC, SDL_EVENT_TYPE_KEY, NULL);

        // present the display
        sdl_display_present();

        // check for event
        event = sdl_poll_event();
        switch (event->event) {
        case SDL_EVENT_LIST_CHOICE ... SDL_EVENT_LIST_CHOICE+39:
            *selection = event->event - SDL_EVENT_LIST_CHOICE;
            done = true;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            text_y -= event->mouse_motion.delta_y;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            text_y -= event->mouse_wheel.delta_y * 2 * pixels_per_row;
            break;
        case SDL_EVENT_KEY_HOME:
            text_y = 0;
            break;
        case SDL_EVENT_KEY_END:
            text_y = INT_MAX;
            break;
        case SDL_EVENT_KEY_PGUP:
            text_y -= (lines_per_display - 2) * pixels_per_row;
            break;
        case SDL_EVENT_KEY_PGDN:
            text_y += (lines_per_display - 2) * pixels_per_row;
            break;
        case SDL_EVENT_KEY_UP_ARROW:
            text_y -= pixels_per_row;
            break;
        case SDL_EVENT_KEY_DOWN_ARROW:
            text_y += pixels_per_row;
            break;
        case SDL_EVENT_KEY_ESC:
        case SDL_EVENT_QUIT: 
            done = true;
        }
    }

done:
    // free allocations
    sdl_destroy_texture(title_texture);
    for (i = 0; i < max_choice; i++) {
        sdl_destroy_texture(texture[i]);
    }
    free(texture);
}

void sdl_display_error(char * err_str0, char * err_str1, char * err_str2)
{
    rect_t        disp_pane_full, disp_pane;
    sdl_event_t * event;
    int32_t       row, col;
    bool          done = false;
    uint32_t      win_width, win_height;

    // loop until done
    while (!done) {
        // short delay
        usleep(5000);

        // init 
        sdl_get_state(&win_width, &win_height, NULL);
        sdl_init_pane(&disp_pane_full, &disp_pane, 0, 0, win_width, win_height);
        sdl_display_init();

        // display the error strings
        row = sdl_pane_rows(&disp_pane,0) / 2 - 2;
        if (row < 0) {
            row = 0;
        }
        col = 0;
        sdl_render_text(&disp_pane, row,   col, 0, err_str0, LIGHT_BLUE, BLACK);
        sdl_render_text(&disp_pane, row+1, col, 0, err_str1, LIGHT_BLUE, BLACK);
        sdl_render_text(&disp_pane, row+2, col, 0, err_str2, LIGHT_BLUE, BLACK);

        // display controls 
        sdl_render_text(&disp_pane, -1, -7, 0, "(ESC)", LIGHT_BLUE, BLACK);
        sdl_event_register(SDL_EVENT_KEY_ESC, SDL_EVENT_TYPE_KEY, NULL);

        // present the display
        sdl_display_present();

        // check for event
        event = sdl_poll_event();
        switch (event->event) {
        case SDL_EVENT_KEY_ESC:
        case SDL_EVENT_QUIT: 
            done = true;
        }
    }
}

