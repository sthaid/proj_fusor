#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include <time.h>
#include <sys/time.h>

#include "util_misc.h"

// -----------------  LOGMSG  --------------------------------------------

void logmsg(char *lvl, const char *func, char *fmt, ...) 
{
    va_list ap;
    char    msg[1000];
    int     len;
    char    time_str[MAX_TIME_STR];

    // construct msg
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    // remove terminating newline
    len = strlen(msg);
    if (len > 0 && msg[len-1] == '\n') {
        msg[len-1] = '\0';
        len--;
    }

    // log to stderr
    fprintf(stderr, "%s %s %s: %s\n",
            time2str(time_str, get_real_time_us(), false, true),
            lvl, func, msg);
}

// -----------------  TIME UTILS  -----------------------------------------

uint64_t microsec_timer(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC,&ts);
    return  ((uint64_t)ts.tv_sec * 1000000) + ((uint64_t)ts.tv_nsec / 1000);
}

uint64_t get_real_time_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME,&ts);
    return ((uint64_t)ts.tv_sec * 1000000) + ((uint64_t)ts.tv_nsec / 1000);
}

char * time2str(char * str, int64_t us, bool gmt, bool display_ms) 
{
    struct tm tm;
    time_t secs;
    int32_t cnt;

    secs = us / 1000000;

    if (gmt) {
        gmtime_r(&secs, &tm);
    } else {
        localtime_r(&secs, &tm);
    }

    cnt = sprintf(str, 
                  "%2.2d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d",
                  tm.tm_mon+1, tm.tm_mday, tm.tm_year%100,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    if (display_ms) {
        cnt += sprintf(str+cnt, ".%3.3"PRId64, (us % 1000000) / 1000);
    }

    if (gmt) {
        strcpy(str+cnt, " GMT");
    }

    return str;
}
