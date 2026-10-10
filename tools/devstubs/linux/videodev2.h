/*
 * devstubs/linux/videodev2.h — 主机端语法检查用的 V4L2 桩。
 *
 * 按 Linux 官方 <linux/videodev2.h> 复刻工程用到的子集（结构体布局与
 * 字段名一致），供 Windows/MinGW 上对 T113 工程做真实类型检查。
 * 不是实现；交叉编译必须使用目标平台真实的 <linux/videodev2.h>。
 */
#ifndef ACCESSTERMINAL_DEVSTUB_LINUX_VIDEODEV2_H
#define ACCESSTERMINAL_DEVSTUB_LINUX_VIDEODEV2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ioctl 请求码 ---- */
#define VIDIOC_QUERYCAP 0x80685600U
#define VIDIOC_S_FMT 0xC0D05605U
#define VIDIOC_REQBUFS 0xC0145608U
#define VIDIOC_QUERYBUF 0xC0585609U
#define VIDIOC_QBUF 0xC058560FU
#define VIDIOC_DQBUF 0xC0585611U
#define VIDIOC_STREAMON 0x40045612U
#define VIDIOC_STREAMOFF 0x40045613U

/* ---- buffer type / memory ---- */
enum v4l2_buf_type {
    V4L2_BUF_TYPE_VIDEO_CAPTURE = 1,
};

enum v4l2_memory {
    V4L2_MEMORY_MMAP = 1,
    V4L2_MEMORY_USERPTR = 2,
};

/* ---- field ---- */
enum v4l2_field {
    V4L2_FIELD_ANY = 0,
    V4L2_FIELD_NONE = 1,
};

/* ---- 像素格式四字符码 ---- */
#define v4l2_fourcc(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')
#define V4L2_PIX_FMT_JPEG v4l2_fourcc('J', 'P', 'E', 'G')
#define V4L2_PIX_FMT_YUYV v4l2_fourcc('Y', 'U', 'Y', 'V')

/* ---- 结构体 ---- */
struct v4l2_pix_format {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t field;
    uint32_t bytesperline;
    uint32_t sizeimage;
    uint32_t colorspace;
    uint32_t priv;
    uint32_t flags;
    uint32_t ycbcr_enc;
    uint32_t quantization;
    uint32_t xfer_func;
};

struct v4l2_format {
    uint32_t type;
    union {
        struct v4l2_pix_format pix;
        char __raw[200];
    } fmt;
};

struct v4l2_requestbuffers {
    uint32_t count;
    uint32_t type;
    uint32_t memory;
    uint32_t capabilities;
    uint32_t reserved[1];
};

struct v4l2_timecode {
    uint32_t type;
    uint32_t flags;
    uint8_t frames;
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t userbits[4];
};

struct v4l2_buffer {
    uint32_t index;
    uint32_t type;
    uint32_t bytesused;
    uint32_t flags;
    uint32_t field;
    struct {
        int32_t tv_sec;
        int32_t tv_usec;
    } timestamp;
    struct v4l2_timecode timecode;
    uint32_t sequence;
    uint32_t memory;
    union {
        uint32_t offset;
        unsigned long userptr;
    } m;
    uint32_t length;
    uint32_t reserved2;
    union {
        int32_t request_fd;
        uint32_t reserved;
    };
};

struct v4l2_capability {
    uint8_t driver[16];
    uint8_t card[32];
    uint8_t bus_info[32];
    uint32_t version;
    uint32_t capabilities;
    uint32_t device_caps;
    uint32_t reserved[3];
};

#ifdef __cplusplus
}
#endif

#endif /* ACCESSTERMINAL_DEVSTUB_LINUX_VIDEODEV2_H */
