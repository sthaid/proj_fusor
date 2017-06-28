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

#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>
#include <netdb.h>

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

    // log to stdout 
    printf("%s %s %s: %s\n",
           time2str(time_str, get_real_time_us(), false, true, true),
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

char * time2str(char * str, int64_t us, bool gmt, bool display_ms, bool display_date) 
{
    struct tm tm;
    time_t secs;
    int32_t cnt;
    char * s = str;

    secs = us / 1000000;

    if (gmt) {
        gmtime_r(&secs, &tm);
    } else {
        localtime_r(&secs, &tm);
    }

    if (display_date) {
        cnt = sprintf(s, "%2.2d/%2.2d/%2.2d ",
                         tm.tm_mon+1, tm.tm_mday, tm.tm_year%100);
        s += cnt;
    }

    cnt = sprintf(s, "%2.2d:%2.2d:%2.2d",
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
    s += cnt;

    if (display_ms) {
        cnt = sprintf(s, ".%3.3"PRId64, (us % 1000000) / 1000);
        s += cnt;
    }

    if (gmt) {
        strcpy(s, " GMT");
    }

    return str;
}

// -----------------  CONFIG READ / WRITE  -------------------------------

int config_read(char * config_path, config_t * config, int config_version)
{
    FILE * fp;
    int    i, version=0;
    char * name;
    char * value;
    char * saveptr;
    char   s[100] = "";

    // open config_file and verify version, 
    // if this fails then write the config file with default values
    if ((fp = fopen(config_path, "re")) == NULL ||
        fgets(s, sizeof(s), fp) == NULL ||
        sscanf(s, "VERSION %d", &version) != 1 ||
        version != config_version)
    {
        if (fp != NULL) {
            fclose(fp);
        }
        INFO("creating default config file %s, version=%d\n", config_path, config_version);
        return config_write(config_path, config, config_version);
    }

    // read config entries
    while (fgets(s, sizeof(s), fp) != NULL) {
        name = strtok_r(s, " \n", &saveptr);
        if (name == NULL || name[0] == '#') {
            continue;
        }

        value = strtok_r(NULL, " \n", &saveptr);
        if (value == NULL) {
            value = "";
        }

        for (i = 0; config[i].name[0]; i++) {
            if (strcmp(name, config[i].name) == 0) {
                strcpy(config[i].value, value);
                break;
            }
        }
    }

    // close
    fclose(fp);
    return 0;
}

int config_write(char * config_path, config_t * config, int config_version)
{
    FILE * fp;
    int    i;

    // open
    fp = fopen(config_path, "we");  // mode: truncate-or-create, close-on-exec
    if (fp == NULL) {
        ERROR("failed to write config file %s, %s\n", config_path, strerror(errno));
        return -1;
    }

    // write version
    fprintf(fp, "VERSION %d\n", config_version);

    // write name/value pairs
    for (i = 0; config[i].name[0]; i++) {
        fprintf(fp, "%-20s %s\n", config[i].name, config[i].value);
    }

    // close
    fclose(fp);
    return 0;
}

// -----------------  NETWORKING  ----------------------------------------

int getsockaddr(char * node, int port, struct sockaddr_in * ret_addr)
{
    struct addrinfo   hints;
    struct addrinfo * result;
    char              port_str[20];
    int               ret;

    sprintf(port_str, "%d", port);

    bzero(&hints, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
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

char * sock_addr_to_str(char * s, int slen, struct sockaddr * addr)
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

int do_recv(int sockfd, void * recv_buff, size_t len)
{
    int ret;
    size_t len_remaining = len;

    while (len_remaining) {
        ret = recv(sockfd, recv_buff, len_remaining, MSG_WAITALL);
        if (ret <= 0) {
            if (ret == 0) {
                errno = ENODATA;
            }
            return -1;
        }

        len_remaining -= ret;
        recv_buff += ret;
    }

    return len;
}

int do_send(int sockfd, void * send_buff, size_t len)
{
    int ret;
    size_t len_remaining = len;

    while (len_remaining) {
        ret = send(sockfd, send_buff, len_remaining, MSG_NOSIGNAL);
        if (ret <= 0) {
            if (ret == 0) {
                errno = ENODATA;
            }
            return -1;
        }

        len_remaining -= ret;
        send_buff += ret;
    }

    return len;
}
