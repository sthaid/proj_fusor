#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
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

#define WC_VIDEO          "/dev/video"   // base name
#define MAX_BUFMAP        32  
#define FRAMES_PER_SEC    10
#define WIDTH             640
#define HEIGHT            480

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

void cam_init(void)
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
        FATAL("open failed\n");
    }

    // get and verify capability
    if (ioctl(cam_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        FATAL("ioctl VIDIOC_QUERYCAP %s\n", strerror(errno));
    }
    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        FATAL("no cap V4L2_CAP_VIDEO_CAPTURE\n");
    }
    if ((cap.capabilities & V4L2_CAP_STREAMING) == 0) {
        FATAL("no cap V4L2_CAP_STREAMING\n");
    }

    // get VIDEO_CAPTURE format type, and
    // set pixel format to (MJPEG,width,height)
    bzero(&format, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_G_FMT, &format) < 0) {
        FATAL("ioctl VIDIOC_G_FMT %s\n", strerror(errno));
    }
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.width  =  WIDTH;
    format.fmt.pix.height =  HEIGHT;
    if (ioctl(cam_fd, VIDIOC_S_FMT, &format) < 0) {
        FATAL("ioctl VIDIOC_S_FMT %s\n", strerror(errno));
    }

    // get crop capabilities
    bzero(&cropcap,sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_CROPCAP, &cropcap) < 0) {
        FATAL("ioctl VIDIOC_CROPCAP, %s\n", strerror(errno));
    }

    // set crop to default 
    bzero(&crop, sizeof(crop));
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    crop.c = cropcap.defrect;
    if (ioctl(cam_fd, VIDIOC_S_CROP, &crop) < 0) {
        if (errno == EINVAL || errno == ENOTTY) {
            DEBUG("crop not supported\n");
        } else {
            FATAL("ioctl VIDIOC_S_CROP, %s\n", strerror(errno));
        }
    }

    // set frames per sec
    bzero(&streamparm, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator   = 1;
    streamparm.parm.capture.timeperframe.denominator = FRAMES_PER_SEC;
    if (ioctl(cam_fd, VIDIOC_S_PARM, &streamparm) < 0) {
        FATAL("ioctl VIDIOC_S_PARM, %s\n", strerror(errno));
    }

    // request memory mapped buffers
    bzero(&reqbuf, sizeof(reqbuf));
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = MAX_BUFMAP;
    if (ioctl (cam_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        FATAL("ioctl VIDIOC_REQBUFS %s\n", strerror(errno));
    }

    // verify we got all the buffers requested
    if (reqbuf.count != MAX_BUFMAP) {
        FATAL("got wrong number of frames, requested %d, actual %d\n",
              MAX_BUFMAP, reqbuf.count);
    }

    // memory map each of the buffers
    for (i = 0; i < MAX_BUFMAP; i++) {
        bzero(&buffer,sizeof(struct v4l2_buffer));
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;
        if (ioctl (cam_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            FATAL("ioctl VIDIOC_QUERYBUF index=%d %s\n", i, strerror(errno));
        }
        bufmap[i].addr = mmap(NULL, buffer.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,           
                              cam_fd, buffer.m.offset);
        bufmap[i].length = buffer.length;

        if (bufmap[i].addr == MAP_FAILED) {
            FATAL("mmap failed, %s\n", strerror(errno));
        }
    }

    // give the buffers to driver
   for (i = 0; i < MAX_BUFMAP; i++) {
        bzero(&buffer,sizeof(struct v4l2_buffer));
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;
        if (ioctl(cam_fd, VIDIOC_QBUF, &buffer) < 0) {
            FATAL("ioctl VIDIOC_QBUF index=%d %s\n", i, strerror(errno));
        }
    }

    // enable capture
    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMON, &buf_type) < 0) {
        FATAL("ioctl VIDIOC_STREAMON %s\n", strerror(errno));
    }

    // register exit handler
    atexit(cam_exit_handler);

    // return success
    INFO("success\n");
}

static void cam_exit_handler(void) 
{
    enum v4l2_buf_type buf_type;

    printf("XXX CAM EXIT \n");
    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMOFF, &buf_type) < 0) {
        WARN("ioctl VIDIOC_STREAMOFF %s\n", strerror(errno));
    }

    // XXX is this needed usleep(100000);
    close(cam_fd);
    printf("XXX CAM DONE \n");
}

void cam_get_buff(uint8_t **buff, uint32_t *len, uint32_t * buff_id)
{
    int64_t duration = 0;

    static int32_t max_buffer_avail;
    static struct v4l2_buffer buffer_avail[MAX_BUFMAP];
 
try_again:
    // dequeue buffers until no more available
    while (true) {
        struct v4l2_buffer buffer;
        bzero(&buffer, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cam_fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                break;
            } else {
                FATAL("ioctl VIDIOC_DQBUF failed, %s\n", strerror(errno));
            }
        }

        if (max_buffer_avail >= MAX_BUFMAP) {
            FATAL("max_buffer_avail = %d\n", max_buffer_avail);
        }
        buffer_avail[max_buffer_avail++] = buffer;
    }

    // if no buffers are available then delay and try again
    if (max_buffer_avail == 0) {
        if (duration > 2000000) {
            FATAL("cam not responding\n");
        }
        usleep(1000);
        duration += 1000;
        goto try_again;
    }
  
    // if this routine is now holding more than 3 then
    // discard all but the newest
    if (max_buffer_avail > 3) {
        int32_t i;
        for (i = 0; i < max_buffer_avail-1; i++) {
            WARN("discarding, index=%d\n", buffer_avail[i].index);
            cam_put_buff(buffer_avail[i].index);
        }

        buffer_avail[0] = buffer_avail[max_buffer_avail-1];
        max_buffer_avail = 1;
    }

    // return the oldest in buffer_avail
    *buff = (uint8_t*)bufmap[buffer_avail[0].index].addr;
    *len =  buffer_avail[0].bytesused;
    *buff_id = buffer_avail[0].index;

    // shift the remaining, slot 0 needs to be the oldest
    max_buffer_avail--;
    memmove(&buffer_avail[0], &buffer_avail[1], max_buffer_avail*sizeof(void*));
}

void cam_put_buff(uint32_t buff_id)
{
    struct v4l2_buffer buffer;

    // give the buffer back to the driver
    bzero(&buffer,sizeof(struct v4l2_buffer));
    buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index  = buff_id;
    if (ioctl(cam_fd, VIDIOC_QBUF, &buffer) < 0) {
        FATAL("ioctl VIDIOC_QBUF index=%d %s\n", buff_id, strerror(errno));
    }
}
