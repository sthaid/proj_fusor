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

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <pthread.h>

#include "util_cam.h"
#include "util_misc.h"

//
// notes:
//
// v4l2-ctl is useful to determine webcam capabilities
//  - yum install v4l-utils
//  - v4l2-ctl --help-all
//  - v4l2-ctl --list-formats-ext
//
// sample code:
//  - https://gist.github.com/Circuitsoft/1126411  print capabilities

//
// defines
//

#define WC_VIDEO   "/dev/video"   // base name
#define MAX_BUFMAP 32  

//
// typedefs
//

typedef struct {
    void  * addr;
    int32_t length;
} bufmap_t;

//
// variables
//

static int32_t   cam_fd = -1;
static bufmap_t  bufmap[MAX_BUFMAP];

//
// prototypes
//

static void cam_exit_handler(void);

// -----------------  API  ---------------------------------------------------------

int32_t cam_init(int32_t width, int32_t height, int32_t frames_per_sec)
{
    struct v4l2_capability     cap;
    struct v4l2_cropcap        cropcap;
    struct v4l2_crop           crop;
    struct v4l2_format         format;
    struct v4l2_streamparm     streamparm;
    struct v4l2_requestbuffers reqbuf;
    struct v4l2_buffer         buffer;
    enum   v4l2_buf_type       buf_type;
    int32_t                    i;

    // print args
    INFO("width=%d height=%d\n", width, height);

    // open webcam, try devices /dev/video0 to 1
    for (i = 0; i < 2; i++) {
        char devpath[100];
        sprintf(devpath, "%s%d", WC_VIDEO, i);
        cam_fd = open(devpath, O_RDWR|O_NONBLOCK);
        if (cam_fd < 0) {
            WARN("open failed %s %s\n",  devpath, strerror(errno));
            continue;
        }
        break;
    }
    if (cam_fd < 0) {
        ERROR("open failed\n");
        goto error;
    }

    // get and verify capability
    if (ioctl(cam_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        ERROR("ioctl VIDIOC_QUERYCAP %s\n", strerror(errno));
        goto error;
    }
    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        ERROR("no cap V4L2_CAP_VIDEO_CAPTURE\n");
        goto error;
    }
    if ((cap.capabilities & V4L2_CAP_STREAMING) == 0) {
        ERROR("no cap V4L2_CAP_STREAMING\n");
        goto error;
    }

    // get VIDEO_CAPTURE format type, and
    // set pixel format to (MJPEG,width,height)
    bzero(&format, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_G_FMT, &format) < 0) {
        ERROR("ioctl VIDIOC_G_FMT %s\n", strerror(errno));
        goto error;
    }
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.width  =  width;
    format.fmt.pix.height =  height;
    if (ioctl(cam_fd, VIDIOC_S_FMT, &format) < 0) {
        ERROR("ioctl VIDIOC_S_FMT %s\n", strerror(errno));
        goto error;
    }

    // get crop capabilities
    bzero(&cropcap,sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_CROPCAP, &cropcap) < 0) {
        ERROR("ioctl VIDIOC_CROPCAP, %s\n", strerror(errno));
        goto error;
    }

    // set crop to default 
    bzero(&crop, sizeof(crop));
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    crop.c = cropcap.defrect;
    if (ioctl(cam_fd, VIDIOC_S_CROP, &crop) < 0) {
        if (errno == EINVAL || errno == ENOTTY) {
            DEBUG("crop not supported\n");
        } else {
            ERROR("ioctl VIDIOC_S_CROP, %s\n", strerror(errno));
            goto error;
        }
    }

    // set frames per sec
    bzero(&streamparm, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator   = 1;
    streamparm.parm.capture.timeperframe.denominator = frames_per_sec;
    if (ioctl(cam_fd, VIDIOC_S_PARM, &streamparm) < 0) {
        ERROR("ioctl VIDIOC_S_PARM, %s\n", strerror(errno));
        goto error;
    }

    // request memory mapped buffers
    bzero(&reqbuf, sizeof(reqbuf));
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = MAX_BUFMAP;
    if (ioctl (cam_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        ERROR("ioctl VIDIOC_REQBUFS %s\n", strerror(errno));
        goto error;
    }

    // verify we got all the buffers requested
    if (reqbuf.count != MAX_BUFMAP) {
        ERROR("got wrong number of frames, requested %d, actual %d\n",
              MAX_BUFMAP, reqbuf.count);
        goto error;
    }

    // memory map each of the buffers
    for (i = 0; i < MAX_BUFMAP; i++) {
        bzero(&buffer,sizeof(struct v4l2_buffer));
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;
        if (ioctl (cam_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            ERROR("ioctl VIDIOC_QUERYBUF index=%d %s\n", i, strerror(errno));
            goto error;
        }
        bufmap[i].addr = mmap(NULL, buffer.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,           
                              cam_fd, buffer.m.offset);
        bufmap[i].length = buffer.length;

        if (bufmap[i].addr == MAP_FAILED) {
            ERROR("mmap failed, %s\n", strerror(errno));
            goto error;
        }
    }

    // give the buffers to driver
   for (i = 0; i < MAX_BUFMAP; i++) {
        bzero(&buffer,sizeof(struct v4l2_buffer));
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;
        if (ioctl(cam_fd, VIDIOC_QBUF, &buffer) < 0) {
            ERROR("ioctl VIDIOC_QBUF index=%d %s\n", i, strerror(errno));
            goto error;
        }
    }

    // enable capture
    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMON, &buf_type) < 0) {
        ERROR("ioctl VIDIOC_STREAMON %s\n", strerror(errno));
        goto error;
    }

    // register exit handler
    atexit(cam_exit_handler);

    // return success
    INFO("success\n");
    return 0;

error:
    // error
    if (cam_fd > 0) {
        close(cam_fd);
        cam_fd = -1;
    }
    return -1;
}

static void cam_exit_handler(void) 
{
    enum v4l2_buf_type buf_type;

    if (cam_fd == -1) {
        return;
    }

    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMOFF, &buf_type) < 0) {
        WARN("ioctl VIDIOC_STREAMOFF %s\n", strerror(errno));
    }

    close(cam_fd);
}

int32_t cam_get_buff(uint8_t **buff, uint32_t *len)
{
    int64_t duration = 0;

    static int32_t max_buffer_avail;
    static struct v4l2_buffer buffer_avail[MAX_BUFMAP];

    // if not initialized then return error
    if (cam_fd == -1) {
        return -1;
    }
 
try_again:
    // dequeue buffers until no more available
    while (true) {
        struct v4l2_buffer buffer;

        // dequeue camera buffer, 
        // if no buffers available then break out of this loop
        bzero(&buffer, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cam_fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                break;
            } else {
                ERROR("ioctl VIDIOC_DQBUF failed, %s\n", strerror(errno));
                return -1;
            }
        }

        // debug print
        DEBUG("GET: index=%d addr=%p length=%d flags=0x%x\n", 
              buffer.index, bufmap[buffer.index].addr, bufmap[buffer.index].length,  buffer.flags);

        // if error flag is set then requeue the buffer and break out of this loop
        // so that the duration check is made below
        if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
            WARN("V4L2_BUF_FLAG_ERROR is set, index=%d flags=0x%x\n", 
                 buffer.index, buffer.flags);
            cam_put_buff(bufmap[buffer.index].addr);
            break;
        }

        // save buffer at end of buffer_avail array
        if (max_buffer_avail >= MAX_BUFMAP) {
            ERROR("max_buffer_avail = %d\n", max_buffer_avail);
            return -1;
        }
        buffer_avail[max_buffer_avail++] = buffer;
    }

    // if no buffers are available then 
    //   if it has been more than 2 seconds then 
    //     return error
    //   else
    //     delay and try again
    //   endif
    // endif
    if (max_buffer_avail == 0) {
        if (duration > 2000000) {
            ERROR("cam not responding\n");
            return -1;
        } else {
            usleep(1000);
            duration += 1000;
            goto try_again;
        }
    }
  
    // if this routine is now holding more than 3 then
    // discard all but the newest
    if (max_buffer_avail > 3) {
        int32_t i;
        for (i = 0; i < max_buffer_avail-1; i++) {
            WARN("discarding, index=%d\n", buffer_avail[i].index);
            cam_put_buff(bufmap[buffer_avail[i].index].addr);
        }

        buffer_avail[0] = buffer_avail[max_buffer_avail-1];
        max_buffer_avail = 1;
    }

    // return the oldest in buffer_avail
    *buff = (uint8_t*)bufmap[buffer_avail[0].index].addr;
    *len =  buffer_avail[0].bytesused;

    // shift the remaining, slot 0 needs to be the oldest
    max_buffer_avail--;
    memmove(&buffer_avail[0], &buffer_avail[1], max_buffer_avail*sizeof(struct v4l2_buffer));

    // return success
    return 0;
}

void cam_put_buff(uint8_t * buff)   
{
    struct v4l2_buffer buffer;
    int32_t i, buff_idx;

    // if not initialized then return
    if (cam_fd == -1) {
        return;
    }

    // find the buff_idx
    for (i = 0; i < MAX_BUFMAP; i++) {
        if (bufmap[i].addr == buff) {
            break;
        }
    }
    if (i == MAX_BUFMAP) {
        ERROR("invalid buff addr %p\n", buff);
    }
    buff_idx = i;

    // debug print
    DEBUG("PUT: index=%d addr=%p length=%d\n",
           buff_idx, bufmap[buff_idx].addr, bufmap[buff_idx].length);

    // give the buffer back to the driver
    bzero(&buffer,sizeof(struct v4l2_buffer));
    buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.length = bufmap[buff_idx].length;
    buffer.index  = buff_idx;
    if (ioctl(cam_fd, VIDIOC_QBUF, &buffer) < 0) {
        ERROR("ioctl VIDIOC_QBUF index=%d %s\n", buff_idx, strerror(errno));
    }
}
