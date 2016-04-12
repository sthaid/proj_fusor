#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <pthread.h>



#include "util_misc.h"

// XXX try printcap from https://gist.github.com/Circuitsoft/1126411
// XXX try new camera
// XXX clean this up


// XXX v4l2-ctl
// yum install v4l-utils
//    v4l2-ctl --help-all
//    v4l2-ctl --list-formats-ext



#define STATUS_ERR_WEBCAM_FAILURE            117
#define STATUS_INFO_OK                       0



// webcam base device name
#define WC_VIDEO "/dev/video"

#define MAX_BUFMAP             32  
#define FRAMES_PER_SEC         10

#define MAX_RESOLUTION    3
#define WIDTH_HIGH_RES    640
#define HEIGHT_HIGH_RES   480
#define WIDTH_MED_RES     320
#define HEIGHT_MED_RES    240
#define WIDTH_LOW_RES     160
#define HEIGHT_LOW_RES    120
#define VALID_WIDTH_AND_HEIGHT(w,h) \
                          (((w) == WIDTH_HIGH_RES && (h) == HEIGHT_HIGH_RES) || \
                           ((w) == WIDTH_MED_RES && (h) == HEIGHT_MED_RES)   || \
                           ((w) == WIDTH_LOW_RES && (h) == HEIGHT_LOW_RES))
#define WIDTH(res)        ((res) == '0' ? WIDTH_LOW_RES  : (res) == '1' ? WIDTH_MED_RES  : WIDTH_HIGH_RES)
#define HEIGHT(res)       ((res) == '0' ? HEIGHT_LOW_RES : (res) == '1' ? HEIGHT_MED_RES : HEIGHT_HIGH_RES)


typedef struct {
    void  * addr;
    int     length;
} bufmap_t;


int                      cam_fd = -1;
bufmap_t                 bufmap[MAX_BUFMAP];
uint32_t cam_status;
#define MS 1000

char resolution = '2';

void * cam_thread(void * cx);

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
    int                        i;
    bool                       first_try = true;
    pthread_t thread_id;

    INFO("starting, resolution=%c\n", resolution);

try_again:
    // if not first try then delay
    if (!first_try) {
        INFO("sleep and retry\n");
        cam_status = STATUS_ERR_WEBCAM_FAILURE;
        usleep(10000*MS);  // 10 secs
    }
    first_try = false;

    // if already initialized, then perform uninitialize
    if (cam_fd > 0) {
        close(cam_fd);
        for (i = 0; i < MAX_BUFMAP; i++) {
            if (bufmap[i].addr != NULL) {
                munmap(bufmap[i].addr, bufmap[i].length);
                bufmap[i].addr = NULL;
                bufmap[i].length = 0;
            }
        }
        cam_fd = -1;
        bzero(bufmap,sizeof(bufmap));
    }

    // open webcam
    for (i = 0; i < 2; i++) {
        char devpath[100];
        sprintf(devpath, "%s%d", WC_VIDEO, i);
        //cam_fd = open(devpath, O_RDWR|O_CLOEXEC|O_NONBLOCK);
        cam_fd = open(devpath, O_RDWR);
        if (cam_fd < 0) {
            ERROR("open failed %s %s\n",  devpath, strerror(errno));
        } else {
            INFO("open success %s\n", devpath);
            break;
        }
    }
    if (cam_fd < 0) {
        goto try_again;
    }

    // get and verify capability
    if (ioctl(cam_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        ERROR("ioctl VIDIOC_QUERYCAP %s\n", strerror(errno));
        goto try_again;
    }

    // verify capabilities
    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        ERROR("no cap V4L2_CAP_VIDEO_CAPTURE\n");
        goto try_again;
    }
    if ((cap.capabilities & V4L2_CAP_STREAMING) == 0) {
        ERROR("no cap V4L2_CAP_STREAMING\n");
        goto try_again;
    }

    // get VIDEO_CAPTURE format type, and
    // set pixel format to (MJPEG,width,height)
    bzero(&format, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_G_FMT, &format) < 0) {
        ERROR("ioctl VIDIOC_G_FMT %s\n", strerror(errno));
        goto try_again;
    }
    INFO("setting resolution to %dx%d\n", WIDTH(resolution), HEIGHT(resolution));
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.width  =  WIDTH(resolution);
    format.fmt.pix.height =  HEIGHT(resolution);
    if (ioctl(cam_fd, VIDIOC_S_FMT, &format) < 0) {
        ERROR("ioctl VIDIOC_S_FMT %s\n", strerror(errno));
        goto try_again;
    }

    // get crop capabilities
    bzero(&cropcap,sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_CROPCAP, &cropcap) < 0) {
        ERROR("ioctl VIDIOC_CROPCAP, %s\n", strerror(errno));
        goto try_again;
    }

    // set crop to default 
    bzero(&crop, sizeof(crop));
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    crop.c = cropcap.defrect;
    if (ioctl(cam_fd, VIDIOC_S_CROP, &crop) < 0) {
        if (errno == EINVAL || errno == ENOTTY) {
            INFO("crop not supported\n");
        } else {
            ERROR("ioctl VIDIOC_S_CROP, %s\n", strerror(errno));
            goto try_again;
        }
    }

    // set frames per sec
    bzero(&streamparm, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator   = 1;
    streamparm.parm.capture.timeperframe.denominator = FRAMES_PER_SEC;
    if (ioctl(cam_fd, VIDIOC_S_PARM, &streamparm) < 0) {
        ERROR("ioctl VIDIOC_S_PARM, %s\n", strerror(errno));
        goto try_again;
    }

    // request memory mapped buffers
    bzero(&reqbuf, sizeof(reqbuf));
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = MAX_BUFMAP;
    if (ioctl (cam_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        ERROR("ioctl VIDIOC_REQBUFS %s\n", strerror(errno));
        goto try_again;
    }

    // verify we got all the frames requested
    if (reqbuf.count != MAX_BUFMAP) {
        ERROR("got wrong number of frames, requested %d, actual %d\n",
              MAX_BUFMAP, reqbuf.count);
        goto try_again;
    }

    // memory map each of the buffers
    for (i = 0; i < MAX_BUFMAP; i++) {
        bzero(&buffer,sizeof(struct v4l2_buffer));
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;
        if (ioctl (cam_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            ERROR("ioctl VIDIOC_QUERYBUF index=%d %s\n", i, strerror(errno));
            goto try_again;
        }
        bufmap[i].addr = mmap(NULL, buffer.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,           
                              cam_fd, buffer.m.offset);
        bufmap[i].length = buffer.length;

        if (bufmap[i].addr == MAP_FAILED) {
            ERROR("mmap failed, %s\n", strerror(errno));
            goto try_again;
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
            goto try_again;
        }
    }

    // enable capture
    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMON, &buf_type) < 0) {
        ERROR("ioctl VIDIOC_STREAMON %s\n", strerror(errno));
        goto try_again;
    }

    // create cam_thread
    pthread_create(&thread_id, NULL, cam_thread, NULL);

    // return success
    INFO("success\n");
    cam_status = STATUS_INFO_OK;
}

void * cam_thread(void * cx)
{
    struct v4l2_buffer buffer;

    while (true) {

        // read a frame
#if 0
        int count = 0;
        while (true) {
            bzero(&buffer, sizeof(buffer));
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (ioctl(cam_fd, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN && count++ < 50) {
                    usleep(50*MS);
                    continue;
                }
                ERROR("ioctl VIDIOC_DQBUF failed, count=%d, %s\n", count, strerror(errno));
                cam_status = STATUS_ERR_WEBCAM_FAILURE;
                goto cam_failure;
            }
            break;
        }
#endif
        bzero(&buffer, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cam_fd, VIDIOC_DQBUF, &buffer) < 0) {
            ERROR("ioctl VIDIOC_DQBUF failed, %s\n", strerror(errno));
        }

        // if error flag is set then requeue the buffer and continue
        if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
            WARN("V4L2_BUF_FLAG_ERROR is set, flags=0x%x\n", buffer.flags);
            //IOCTL_VIDIOC_QBUF();
            //continue;
        }

        printf("DQBUF OKAY bytes=%d index=%d flags=0x%x addr=%p\n",
               buffer.bytesused,
               buffer.index,
               buffer.flags,
               bufmap[buffer.index].addr);

        buffer.flags = 0;
        if (ioctl(cam_fd, VIDIOC_QBUF, &buffer) < 0) {
            ERROR("ioctl VIDIOC_QBUF %s\n", strerror(errno));  
        } 
    }
}
