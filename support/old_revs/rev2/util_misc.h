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

#ifndef __UTIL_MISC_H__
#define __UTIL_MISC_H__

// -----------------  LOGGING  -----------------------------------

//#define ENABLE_LOGGING_AT_DEBUG_LEVEL

#define INFO(fmt, args...) \
    do { \
        logmsg("INFO", __func__, fmt, ## args); \
    } while (0)
#define WARN(fmt, args...) \
    do { \
        logmsg("WARN", __func__, fmt, ## args); \
    } while (0)
#define ERROR(fmt, args...) \
    do { \
        logmsg("ERROR", __func__, fmt, ## args); \
    } while (0)

#ifdef ENABLE_LOGGING_AT_DEBUG_LEVEL
    #define DEBUG(fmt, args...) \
        do { \
            logmsg("DEBUG", __func__, fmt, ## args); \
        } while (0)
#else
    #define DEBUG(fmt, args...) 
#endif

#define FATAL(fmt, args...) \
    do { \
        logmsg("FATAL", __func__, fmt, ## args); \
        exit(1); \
    } while (0)

void logmsg(char * lvl, const char * func, char * fmt, ...) __attribute__ ((format (printf, 3, 4)));

// -----------------  TIME  --------------------------------------

#define MAX_TIME_STR 50

uint64_t microsec_timer(void);
uint64_t get_real_time_us(void);
char * time2str(char * str, int64_t us, bool gmt, bool display_ms, bool display_date);

// -----------------  RANDOM NUMBERS  ----------------------------

int32_t random_triangular(int32_t low, int32_t high);

#endif
