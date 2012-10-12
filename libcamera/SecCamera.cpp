/*
 * Copyright 2008, The Android Open Source Project
 * Copyright 2010, Samsung Electronics Co. LTD
 * Copyright 2011, Havlena Petr <havlenapetr@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
************************************
* Filename: SecCamera.cpp
* Author:   Sachin P. Kamat
* Purpose:  This file interacts with the Camera and JPEG drivers.
*************************************
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "SecCamera"

#include <utils/Log.h>

#include <string.h>
#include <stdlib.h>
#include <sys/poll.h>
#include "SecCamera.h"
#include "cutils/properties.h"

using namespace android;

#define CHECK(return_value)                                          \
    if (return_value < 0) {                                          \
        ALOGE("%s::%d fail. errno: %s, m_camera_id = %d\n",          \
             __func__, __LINE__, strerror(errno), m_camera_id);      \
        return -1;                                                   \
    }

#define CHECK_FD(fd)                                                 \
    if (fd <= 0) {                                                   \
        ALOGE("%s::%d bad file descriptor, m_camera_id = %d\n",      \
             __func__, __LINE__, m_camera_id);                       \
        return -1;                                                   \
    }

#define SET_VALUE_IF(fd, what, value)                                \
    CHECK_FD(fd);                                                    \
    if (value != -1) {                                               \
        int ret = fimc_v4l2_s_ctrl(fd, what, value);                 \
        CHECK(ret);                                                  \
    }

#define ALIGN_TO_32B(x)   ((((x) + (1 <<  5) - 1) >>  5) <<  5)
#define ALIGN_TO_128B(x)  ((((x) + (1 <<  7) - 1) >>  7) <<  7)
#define ALIGN_TO_8KB(x)   ((((x) + (1 << 13) - 1) >> 13) << 13)

namespace android {

// ======================================================================
// Camera controls

#if defined(LOG_NDEBUG) && LOG_NDEBUG == 0
static unsigned long measure_time(struct timeval *start, struct timeval *stop)
{
    unsigned long sec, usec, time;

    sec = stop->tv_sec - start->tv_sec;

    if (stop->tv_usec >= start->tv_usec) {
        usec = stop->tv_usec - start->tv_usec;
    } else {
        usec = stop->tv_usec + 1000000 - start->tv_usec;
        sec--;
    }

    time = (sec * 1000000) + usec;

    return time;
}
#endif

static int get_pixel_depth(unsigned int fmt)
{
    int depth = 0;

    switch (fmt) {
    case V4L2_PIX_FMT_NV12:
        depth = 12;
        break;
    case V4L2_PIX_FMT_NV12T:
        depth = 12;
        break;
    case V4L2_PIX_FMT_NV21:
        depth = 12;
        break;
    case V4L2_PIX_FMT_YUV420:
        depth = 12;
        break;

    case V4L2_PIX_FMT_RGB565:
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_YVYU:
    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_VYUY:
    case V4L2_PIX_FMT_NV16:
    case V4L2_PIX_FMT_NV61:
    case V4L2_PIX_FMT_YUV422P:
        depth = 16;
        break;

    case V4L2_PIX_FMT_RGB32:
        depth = 32;
        break;
    }

    return depth;
}

static int fimc_poll(struct pollfd *events)
{
    int ret;

    /* 10 second delay is because sensor can take a long time
     * to do auto focus and capture in dark settings
     */
    ret = poll(events, 1, 10000);
    if (ret < 0) {
        ALOGE("ERR(%s):poll error\n", __func__);
        return ret;
    }

    if (ret == 0) {
        ALOGE("ERR(%s):No data in 10 secs..\n", __func__);
        return ret;
    }

    return ret;
}

int SecCamera::previewPoll(bool preview)
{
    int ret;

    if (preview) {
#ifdef ENABLE_ESD_PREVIEW_CHECK
        int status = 0;

        if (!(++m_esd_check_count % 60)) {
            status = getCameraSensorESDStatus();
            m_esd_check_count = 0;
            if (status) {
               ALOGE("ERR(%s) ESD status(%d)", __func__, status);
               return status;
            }
        }
#endif

        ret = poll(&m_events_c, 1, 1000);
    } else {
        ret = poll(&m_events_c2, 1, 1000);
    }

    if (ret < 0) {
        ALOGE("ERR(%s):poll error\n", __func__);
        return ret;
    }

    if (ret == 0) {
        ALOGE("ERR(%s):No data in 1 secs.. Camera Device Reset \n", __func__);
        return ret;
    }

    return ret;
}

static int fimc_v4l2_querycap(int fp)
{
    struct v4l2_capability cap;
    int ret = 0;

    ret = ioctl(fp, VIDIOC_QUERYCAP, &cap);

    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_QUERYCAP failed\n", __func__);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        ALOGE("ERR(%s):no capture devices\n", __func__);
        return -1;
    }

    return ret;
}

static const __u8* fimc_v4l2_enuminput(int fp, int index)
{
    static struct v4l2_input input;

    input.index = index;
    if (ioctl(fp, VIDIOC_ENUMINPUT, &input) != 0) {
        ALOGE("ERR(%s):No matching index found\n", __func__);
        return NULL;
    }
    ALOGI("Name of input channel[%d] is %s\n", input.index, input.name);

    return input.name;
}


static int fimc_v4l2_s_input(int fp, int index)
{
    struct v4l2_input input;
    int ret;

    input.index = index;

    ret = ioctl(fp, VIDIOC_S_INPUT, &input);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_S_INPUT failed\n", __func__);
        return ret;
    }

    return ret;
}

static int fimc_v4l2_s_fmt(int fp, int width, int height, unsigned int fmt, int flag_capture)
{
    struct v4l2_format v4l2_fmt;
    struct v4l2_pix_format pixfmt;
    int ret;

    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    memset(&pixfmt, 0, sizeof(pixfmt));

    pixfmt.width = width;
    pixfmt.height = height;
    pixfmt.pixelformat = fmt;

    pixfmt.sizeimage = (width * height * get_pixel_depth(fmt)) / 8;

    pixfmt.field = V4L2_FIELD_NONE;

    v4l2_fmt.fmt.pix = pixfmt;

    /* Set up for capture */
    ret = ioctl(fp, VIDIOC_S_FMT, &v4l2_fmt);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_S_FMT failed\n", __func__);
        return -1;
    }

    return 0;
}

static int fimc_v4l2_s_fmt_cap(int fp, int width, int height, unsigned int fmt)
{
    struct v4l2_format v4l2_fmt;
    struct v4l2_pix_format pixfmt;
    int ret;

    memset(&pixfmt, 0, sizeof(pixfmt));

    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    pixfmt.width = width;
    pixfmt.height = height;
    pixfmt.pixelformat = fmt;
    if (fmt == V4L2_PIX_FMT_JPEG) {
        pixfmt.colorspace = V4L2_COLORSPACE_JPEG;
    }

    pixfmt.sizeimage = (width * height * get_pixel_depth(fmt)) / 8;

    v4l2_fmt.fmt.pix = pixfmt;

    //ALOGE("ori_w %d, ori_h %d, w %d, h %d\n", width, height, v4l2_fmt.fmt.pix.width, v4l2_fmt.fmt.pix.height);

    /* Set up for capture */
    ret = ioctl(fp, VIDIOC_S_FMT, &v4l2_fmt);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_S_FMT failed\n", __func__);
        return ret;
    }

    return ret;
}

static int fimc_v4l2_enum_fmt(int fp, unsigned int fmt)
{
    struct v4l2_fmtdesc fmtdesc;
    int found = 0;

    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmtdesc.index = 0;

    while (ioctl(fp, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        if (fmtdesc.pixelformat == fmt) {
            ALOGV("passed fmt = %#x found pixel format[%d]: %s\n", fmt, fmtdesc.index, fmtdesc.description);
            found = 1;
            break;
        }

        fmtdesc.index++;
    }

    if (!found) {
        ALOGE("unsupported pixel format\n");
        return -1;
    }

    return 0;
}

static int fimc_v4l2_reqbufs(int fp, enum v4l2_buf_type type, int nr_bufs)
{
    struct v4l2_requestbuffers req;
    int ret;

    req.count = nr_bufs;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(fp, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_REQBUFS failed\n", __func__);
        return -1;
    }

    return req.count;
}

static int fimc_v4l2_querybuf(int fp, struct fimc_buffer *buffer, enum v4l2_buf_type type)
{
    struct v4l2_buffer v4l2_buf;
    int ret;

    ALOGI("%s :", __func__);

    v4l2_buf.type = type;
    v4l2_buf.memory = V4L2_MEMORY_MMAP;
    v4l2_buf.index = buffer->index;

    ret = ioctl(fp , VIDIOC_QUERYBUF, &v4l2_buf);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_QUERYBUF failed\n", __func__);
        return -1;
    }

    if ((buffer->start = (char *)mmap(0, v4l2_buf.length,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         fp, v4l2_buf.m.offset)) < 0) {
         ALOGE("%s %d] mmap() failed\n",__func__, __LINE__);
         return -1;
    }
    buffer->length = v4l2_buf.length;

    ALOGI("%s: buffer->start = %p buffer->length = %d buffer->index = %d",
         __func__, buffer->start, buffer->length, buffer->index);

    return 0;
}

static int fimc_v4l2_streamon(int fp)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int ret;

    ret = ioctl(fp, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_STREAMON failed\n", __func__);
        return ret;
    }

    return ret;
}

static int fimc_v4l2_streamoff(int fp)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int ret;

    ALOGV("%s :", __func__);
    ret = ioctl(fp, VIDIOC_STREAMOFF, &type);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_STREAMOFF failed\n", __func__);
        return ret;
    }

    return ret;
}

static int fimc_v4l2_qbuf(int fp, int index)
{
    struct v4l2_buffer v4l2_buf;
    int ret;

    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_buf.memory = V4L2_MEMORY_MMAP;
    v4l2_buf.index = index;

    ret = ioctl(fp, VIDIOC_QBUF, &v4l2_buf);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_QBUF failed\n", __func__);
        return ret;
    }

    return 0;
}

static int fimc_v4l2_dqbuf(int fp)
{
    struct v4l2_buffer v4l2_buf;
    int ret;

    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_buf.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(fp, VIDIOC_DQBUF, &v4l2_buf);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_DQBUF failed, dropped frame\n", __func__);
        return ret;
    }

    return v4l2_buf.index;
}

static int fimc_v4l2_g_ctrl(int fp, unsigned int id)
{
    struct v4l2_control ctrl;
    int ret;

    ctrl.id = id;

    ret = ioctl(fp, VIDIOC_G_CTRL, &ctrl);
    if (ret < 0) {
        ALOGE("ERR(%s): VIDIOC_G_CTRL(id = 0x%x (%d)) failed, ret = %d\n",
             __func__, id, id-V4L2_CID_PRIVATE_BASE, ret);
        return ret;
    }

    return ctrl.value;
}

static int fimc_v4l2_s_ctrl(int fp, unsigned int id, unsigned int value)
{
    struct v4l2_control ctrl;
    int ret;

    ctrl.id = id;
    ctrl.value = value;

    ret = ioctl(fp, VIDIOC_S_CTRL, &ctrl);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_S_CTRL(id = %#x (%d), value = %d) failed ret = %d\n",
             __func__, id, id-V4L2_CID_PRIVATE_BASE, value, ret);

        return ret;
    }

    return ctrl.value;
}

static int fimc_v4l2_s_ext_ctrl(int fp, unsigned int id, void *value)
{
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control ctrl;
    int ret;

    ctrl.id = id;
    ctrl.string = (char *) value;

    ctrls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
    ctrls.count = 1;
    ctrls.controls = &ctrl;

    ret = ioctl(fp, VIDIOC_S_EXT_CTRLS, &ctrls);
    if (ret < 0)
        ALOGE("ERR(%s):VIDIOC_S_EXT_CTRLS failed\n", __func__);

    return ret;
}

static int fimc_v4l2_g_parm(int fp, struct v4l2_streamparm *streamparm)
{
    int ret;

    streamparm->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(fp, VIDIOC_G_PARM, streamparm);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_G_PARM failed\n", __func__);
        return -1;
    }

    ALOGV("%s : timeperframe: numerator %d, denominator %d\n", __func__,
            streamparm->parm.capture.timeperframe.numerator,
            streamparm->parm.capture.timeperframe.denominator);

    return 0;
}

static int fimc_v4l2_s_parm(int fp, struct v4l2_streamparm *streamparm)
{
    int ret;

    streamparm->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(fp, VIDIOC_S_PARM, streamparm);
    if (ret < 0) {
        ALOGE("ERR(%s):VIDIOC_S_PARM failed\n", __func__);
        return ret;
    }

    return 0;
}

// ======================================================================
// Constructor & Destructor

SecCamera::SecCamera() :
            m_flag_init(0),
            m_camera_id(CAMERA_ID_BACK),
            m_preview_v4lformat(V4L2_PIX_FMT_NV21),
            m_preview_width      (0),
            m_preview_height     (0),
            m_preview_max_width  (MAX_BACK_CAMERA_PREVIEW_WIDTH),
            m_preview_max_height (MAX_BACK_CAMERA_PREVIEW_HEIGHT),
            m_snapshot_v4lformat(-1),
            m_snapshot_width      (0),
            m_snapshot_height     (0),
            m_snapshot_max_width  (MAX_BACK_CAMERA_SNAPSHOT_WIDTH),
            m_snapshot_max_height (MAX_BACK_CAMERA_SNAPSHOT_HEIGHT),
            m_angle(-1),
            m_anti_banding(-1),
            m_wdr(-1),
            m_anti_shake(-1),
            m_zoom_level(-1),
            m_object_tracking(-1),
            m_smart_auto(-1),
            m_beauty_shot(-1),
            m_vintage_mode(-1),
            m_face_detect(-1),
            m_gps_latitude(-1),
            m_gps_longitude(-1),
            m_gps_altitude(-1),
            m_gps_timestamp(-1),
            m_vtmode(0),
            m_sensor_mode(-1),
            m_shot_mode(-1),
            m_exif_orientation(-1),
            m_blur_level(-1),
            m_chk_dataline(-1),
            m_video_gamma(-1),
            m_slow_ae(-1),
            m_camera_af_flag(-1),
            m_flag_camera_start(0),
            m_jpeg_thumbnail_width (0),
            m_jpeg_thumbnail_height(0),
            m_jpeg_quality(100),
            m_capture_bufs(NULL),
            m_capture_bufs_size(0),
            m_capture_burst(false)
#ifdef ENABLE_ESD_PREVIEW_CHECK
            ,
            m_esd_check_count(0)
#endif // ENABLE_ESD_PREVIEW_CHECK
{
    m_params = (struct sec_cam_parm*)&m_streamparm.parm.raw_data;
    struct v4l2_captureparm capture;
    m_params->capture.timeperframe.numerator = 1;
    m_params->capture.timeperframe.denominator = 0;
    m_params->contrast = -1;
    m_params->effects = -1;
    m_params->brightness = -1;
    m_params->flash_mode = -1;
    m_params->focus_mode = -1;
    m_params->iso = -1;
    m_params->metering = -1;
    m_params->saturation = -1;
    m_params->scene_mode = -1;
    m_params->sharpness = -1;
    m_params->white_balance = -1;

    ALOGV("%s :", __func__);
}

int SecCamera::flagCreate(void) const
{
    ALOGV("%s : : %d", __func__, m_flag_init);
    return m_flag_init;
}

SecCamera::~SecCamera()
{
    ALOGV("%s :", __func__);
}

int SecCamera::initCamera(int index)
{
    ALOGV("%s :", __func__);
    int ret = 0;

    if (!m_flag_init) {
        /* Arun C
         * Reset the lense position only during camera starts; don't do
         * reset between shot to shot
         */
        m_camera_af_flag = -1;

        m_cam_fd_temp = -1;
        m_cam_fd2_temp = -1;

        m_cam_fd = open(CAMERA_DEV_NAME, O_RDWR);
        if (m_cam_fd < 0) {
            ALOGE("ERR(%s):Cannot open %s (error : %s)\n", __func__, CAMERA_DEV_NAME, strerror(errno));
            return -1;
        }

        ALOGV("initCamera: m_cam_fd(%d), m_jpeg_fd(%d)", m_cam_fd, m_jpeg_fd);

        ret = fimc_v4l2_querycap(m_cam_fd);
        CHECK(ret);
        if (!fimc_v4l2_enuminput(m_cam_fd, index))
            return -1;
        ret = fimc_v4l2_s_input(m_cam_fd, index);
        CHECK(ret);

        m_cam_fd2 = open(CAMERA_DEV_NAME2, O_RDWR);
        if (m_cam_fd2 < 0) {
            ALOGE("ERR(%s):Cannot open %s (error : %s)\n", __func__, CAMERA_DEV_NAME2, strerror(errno));
            return -1;
        }

        ALOGV("initCamera: m_cam_fd2(%d)", m_cam_fd2);

        ret = fimc_v4l2_querycap(m_cam_fd2);
        CHECK(ret);
        if (!fimc_v4l2_enuminput(m_cam_fd2, index))
            return -1;
        ret = fimc_v4l2_s_input(m_cam_fd2, index);
        CHECK(ret);

        m_camera_id = index;

        switch (m_camera_id) {
        case CAMERA_ID_FRONT:
            m_preview_max_width   = MAX_FRONT_CAMERA_PREVIEW_WIDTH;
            m_preview_max_height  = MAX_FRONT_CAMERA_PREVIEW_HEIGHT;
            m_snapshot_max_width  = MAX_FRONT_CAMERA_SNAPSHOT_WIDTH;
            m_snapshot_max_height = MAX_FRONT_CAMERA_SNAPSHOT_HEIGHT;
            break;

        case CAMERA_ID_BACK:
            m_preview_max_width   = MAX_BACK_CAMERA_PREVIEW_WIDTH;
            m_preview_max_height  = MAX_BACK_CAMERA_PREVIEW_HEIGHT;
            m_snapshot_max_width  = MAX_BACK_CAMERA_SNAPSHOT_WIDTH;
            m_snapshot_max_height = MAX_BACK_CAMERA_SNAPSHOT_HEIGHT;
            break;
        }

        setExifFixedAttribute();

        m_flag_init = 1;
    }
    return 0;
}

void SecCamera::resetCamera()
{
    ALOGV("%s :", __func__);
    deinitCamera();
    initCamera(m_camera_id);
}

void SecCamera::deinitCamera()
{
    ALOGV("%s :", __func__);

    if (m_flag_init) {

        stopRecord();

        /* close m_cam_fd after stopRecord() because stopRecord()
         * uses m_cam_fd to change frame rate
         */
        ALOGV("deinitCamera: m_cam_fd(%d)", m_cam_fd);
        if (m_cam_fd > -1) {
            close(m_cam_fd);
            m_cam_fd = -1;
        }

        ALOGV("deinitCamera: m_cam_fd2(%d)", m_cam_fd2);
        if (m_cam_fd2 > -1) {
            close(m_cam_fd2);
            m_cam_fd2 = -1;
        }

        if (m_cam_fd_temp != -1) {
            close(m_cam_fd_temp);
            m_cam_fd_temp = -1;
        }

        if (m_cam_fd2_temp != -1) {
            close(m_cam_fd2_temp);
            m_cam_fd2_temp = -1;
        }

        m_flag_init = 0;
    }
}


int SecCamera::getCameraFd(void)
{
    return m_cam_fd;
}

// ======================================================================
// Preview

int SecCamera::startStream(void)
{
    int ret;

    if (m_camera_id == CAMERA_ID_BACK) {
        ret = fimc_v4l2_s_parm(m_cam_fd, &m_streamparm);
        CHECK(ret);

        // set all stream params manually, because ce147 driver doesn't handle
        // this in fimc_v4l2_s_parm call
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_EFFECT, m_params->effects);
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_ISO, m_params->iso);
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_METERING, m_params->metering);
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_SCENE_MODE, m_params->scene_mode);
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_WHITE_BALANCE, m_params->white_balance);
    }

    ret = fimc_v4l2_streamon(m_cam_fd);
    CHECK(ret);

    if (m_camera_id == CAMERA_ID_FRONT) {
        ret = fimc_v4l2_s_parm(m_cam_fd, &m_streamparm);
        CHECK(ret);

        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_VGA_BLUR, m_blur_level);
    }

    SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_BRIGHTNESS, m_params->brightness);

    if (m_camera_id == CAMERA_ID_BACK) {
        // these params must be set after streamon
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_ZOOM, m_zoom_level);
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_CONTRAST, m_params->contrast);
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_FOCUS_MODE, m_params->focus_mode);
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_SATURATION, m_params->saturation);
        SET_VALUE_IF(m_cam_fd, V4L2_CID_CAMERA_SHARPNESS, m_params->sharpness);
    }

    return 0;
}

int SecCamera::stopStream()
{
    if (m_params->flash_mode == FLASH_MODE_TORCH)
        setFlashMode(FLASH_MODE_OFF);

    int ret = fimc_v4l2_streamoff(m_cam_fd);
    CHECK(ret);

    return 0;
}

int SecCamera::startPreview(void)
{
    v4l2_streamparm streamparm;
    struct sec_cam_parm *parms;
    parms = (struct sec_cam_parm*)&streamparm.parm.raw_data;
    ALOGV("%s :", __func__);

    // aleady started
    if (m_flag_camera_start > 0) {
        ALOGE("ERR(%s):Preview was already started\n", __func__);
        return 0;
    }

    CHECK_FD(m_cam_fd);

    memset(&m_events_c, 0, sizeof(m_events_c));
    m_events_c.fd = m_cam_fd;
    m_events_c.events = POLLIN | POLLERR;

    /* enum_fmt, s_fmt sample */
    int ret = fimc_v4l2_enum_fmt(m_cam_fd,m_preview_v4lformat);
    CHECK(ret);

    if (m_camera_id == CAMERA_ID_BACK)
        ret = fimc_v4l2_s_fmt(m_cam_fd, m_preview_width, m_preview_height, m_preview_v4lformat, 0);
    else
        ret = fimc_v4l2_s_fmt(m_cam_fd, m_preview_height, m_preview_width, m_preview_v4lformat, 0);
    CHECK(ret);

    ret = fimc_v4l2_reqbufs(m_cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, MAX_BUFFERS);
    CHECK(ret);

    ALOGV("%s : m_preview_width: %d m_preview_height: %d m_angle: %d\n",
            __func__, m_preview_width, m_preview_height, m_angle);

    ret = fimc_v4l2_s_ctrl(m_cam_fd,
                           V4L2_CID_CAMERA_CHECK_DATALINE, m_chk_dataline);
    CHECK(ret);

    if (m_camera_id == CAMERA_ID_FRONT) {
        ret = fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_VT_MODE, m_vtmode);
        CHECK(ret);
    }

    /* start with all buffers in queue */
    for (int i = 0; i < MAX_BUFFERS; i++) {
        ret = fimc_v4l2_qbuf(m_cam_fd, i);
        CHECK(ret);
    }

    ret = startStream();
    CHECK(ret);

    m_flag_camera_start = 1;

    // It is a delay for a new frame, not to show the previous bigger ugly picture frame.
    ret = fimc_poll(&m_events_c);
    CHECK(ret);
    ret = fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_RETURN_FOCUS, 0);
    CHECK(ret);

    ALOGV("%s: got the first frame of the preview\n", __func__);

    return 0;
}

int SecCamera::stopPreview(void)
{
    ALOGV("%s :", __func__);

    if (m_flag_camera_start == 0) {
        ALOGW("%s: doing nothing because m_flag_camera_start is zero", __func__);
        return 0;
    }

    CHECK_FD(m_cam_fd);

    int ret = stopStream();
    CHECK(ret);

    m_flag_camera_start = 0;

    return ret;
}

//Recording
int SecCamera::startRecord(void)
{
    int ret, i;

    ALOGV("%s :", __func__);

    // aleady started
    if (m_flag_record_start > 0) {
        ALOGE("ERR(%s):Preview was already started\n", __func__);
        return 0;
    }

    CHECK_FD(m_cam_fd2);

    /* enum_fmt, s_fmt sample */
    ret = fimc_v4l2_enum_fmt(m_cam_fd2, V4L2_PIX_FMT_NV12T);
    CHECK(ret);

    ALOGI("%s: m_recording_width = %d, m_recording_height = %d\n",
         __func__, m_recording_width, m_recording_height);

    if(m_camera_id == CAMERA_ID_BACK)
        ret = fimc_v4l2_s_fmt(m_cam_fd2, m_recording_width,
                              m_recording_height, V4L2_PIX_FMT_NV12T, 0);
    else
        ret = fimc_v4l2_s_fmt(m_cam_fd2, m_recording_height,
                              m_recording_width, V4L2_PIX_FMT_NV12T, 0);
    CHECK(ret);

    ret = fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FRAME_RATE,
                            m_params->capture.timeperframe.denominator);
    CHECK(ret);

    ret = fimc_v4l2_reqbufs(m_cam_fd2, V4L2_BUF_TYPE_VIDEO_CAPTURE, MAX_BUFFERS);
    CHECK(ret);

    /* start with all buffers in queue */
    for (i = 0; i < MAX_BUFFERS; i++) {
        ret = fimc_v4l2_qbuf(m_cam_fd2, i);
        CHECK(ret);
    }

    ret = fimc_v4l2_streamon(m_cam_fd2);
    CHECK(ret);

    // Get and throw away the first frame since it is often garbled.
    memset(&m_events_c2, 0, sizeof(m_events_c2));
    m_events_c2.fd = m_cam_fd2;
    m_events_c2.events = POLLIN | POLLERR;
    ret = fimc_poll(&m_events_c2);
    CHECK(ret);

    m_flag_record_start = 1;

    return 0;
}

int SecCamera::stopRecord(void)
{
    int ret;

    ALOGV("%s :", __func__);

    if (m_flag_record_start == 0) {
        ALOGW("%s: doing nothing because m_flag_record_start is zero", __func__);
        return 0;
    }

    CHECK_FD(m_cam_fd2);

    m_flag_record_start = 0;

    ret = fimc_v4l2_streamoff(m_cam_fd2);
    CHECK(ret);

    ret = fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FRAME_RATE,
                            FRAME_RATE_AUTO);
    CHECK(ret);

    return 0;
}

unsigned int SecCamera::getRecPhyAddrY(int index)
{
    unsigned int addr_y;

    CHECK_FD(m_cam_fd2);

    addr_y = fimc_v4l2_s_ctrl(m_cam_fd2, V4L2_CID_PADDR_Y, index);
    CHECK((int)addr_y);
    return addr_y;
}

unsigned int SecCamera::getRecPhyAddrC(int index)
{
    unsigned int addr_c;

    CHECK_FD(m_cam_fd2);

    addr_c = fimc_v4l2_s_ctrl(m_cam_fd2, V4L2_CID_PADDR_CBCR, index);
    CHECK((int)addr_c);
    return addr_c;
}

unsigned int SecCamera::getPhyAddrY(int index)
{
    unsigned int addr_y;

    CHECK_FD(m_cam_fd);

    addr_y = fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_PADDR_Y, index);
    CHECK((int)addr_y);
    return addr_y;
}

unsigned int SecCamera::getPhyAddrC(int index)
{
    unsigned int addr_c;

    CHECK_FD(m_cam_fd);

    addr_c = fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_PADDR_CBCR, index);
    CHECK((int)addr_c);
    return addr_c;
}

int SecCamera::pausePreview()
{
    return fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_STREAM_PAUSE, 0);
}

int SecCamera::resumePreview()
{
    return fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_STREAM_PAUSE, 1);
}

int SecCamera::getPreview()
{
    int index;
    int ret;

    CHECK_FD(m_cam_fd);

    if (m_flag_camera_start == 0 || previewPoll(true) == 0) {
        ALOGE("ERR(%s):Start Camera Device Reset \n", __func__);
        /* GAUDI Project([arun.c@samsung.com]) 2010.05.20. [Implemented ESD code] */
        /*
         * When there is no data for more than 1 second from the camera we inform
         * the FIMC driver by calling fimc_v4l2_s_input() with a special value = 1000
         * FIMC driver identify that there is something wrong with the camera
         * and it restarts the sensor.
         */
        stopPreview();
        /* Reset Only Camera Device */
        ret = fimc_v4l2_querycap(m_cam_fd);
        CHECK(ret);
        if (fimc_v4l2_enuminput(m_cam_fd, m_camera_id))
            return -1;
        ret = fimc_v4l2_s_input(m_cam_fd, 1000);
        CHECK(ret);
        ret = startPreview();
        if (ret < 0) {
            ALOGE("ERR(%s): startPreview() return %d\n", __func__, ret);
            return 0;
        }
    }

    index = fimc_v4l2_dqbuf(m_cam_fd);
    if (!(0 <= index && index < MAX_BUFFERS)) {
        ALOGE("ERR(%s):wrong index = %d\n", __func__, index);
        return -1;
    }

    ret = fimc_v4l2_qbuf(m_cam_fd, index);
    CHECK(ret);

    return index;
}

int SecCamera::getRecordFrame()
{
    if (m_flag_record_start == 0) {
        ALOGE("%s: m_flag_record_start is 0", __func__);
        return -1;
    }

    CHECK_FD(m_cam_fd2);

    previewPoll(false);
    return fimc_v4l2_dqbuf(m_cam_fd2);
}

int SecCamera::releaseRecordFrame(int index)
{
    if (!m_flag_record_start) {
        /* this can happen when recording frames are returned after
         * the recording is stopped at the driver level.  we don't
         * need to return the buffers in this case and we've seen
         * cases where fimc could crash if we called qbuf and it
         * wasn't expecting it.
         */
        ALOGI("%s: recording not in progress, ignoring", __func__);
        return 0;
    }

    CHECK_FD(m_cam_fd2);

    return fimc_v4l2_qbuf(m_cam_fd2, index);
}

int SecCamera::setPreviewSize(int width, int height, int pixel_format)
{
    ALOGV("%s(width(%d), height(%d), format(%d))", __func__, width, height, pixel_format);

    int v4lpixelformat = pixel_format;

#if defined(LOG_NDEBUG) && LOG_NDEBUG == 0
    if (v4lpixelformat == V4L2_PIX_FMT_YUV420)
        ALOGV("PreviewFormat:V4L2_PIX_FMT_YUV420");
    else if (v4lpixelformat == V4L2_PIX_FMT_NV12)
        ALOGV("PreviewFormat:V4L2_PIX_FMT_NV12");
    else if (v4lpixelformat == V4L2_PIX_FMT_NV12T)
        ALOGV("PreviewFormat:V4L2_PIX_FMT_NV12T");
    else if (v4lpixelformat == V4L2_PIX_FMT_NV21)
        ALOGV("PreviewFormat:V4L2_PIX_FMT_NV21");
    else if (v4lpixelformat == V4L2_PIX_FMT_YUV422P)
        ALOGV("PreviewFormat:V4L2_PIX_FMT_YUV422P");
    else if (v4lpixelformat == V4L2_PIX_FMT_YUYV)
        ALOGV("PreviewFormat:V4L2_PIX_FMT_YUYV");
    else if (v4lpixelformat == V4L2_PIX_FMT_RGB565)
        ALOGV("PreviewFormat:V4L2_PIX_FMT_RGB565");
    else
        ALOGV("PreviewFormat:UnknownFormat");
#endif
    m_preview_width  = width;
    m_preview_height = height;
    m_preview_v4lformat = v4lpixelformat;

    return 0;
}

void SecCamera::getPreviewSize(int *width, int *height, int *frame_size)
{
    *width  = m_preview_width;
    *height = m_preview_height;
    *frame_size = m_frameSize(m_preview_v4lformat, m_preview_width, m_preview_height);
}

void SecCamera::getPreviewMaxSize(int *width, int *height)
{
    *width  = m_preview_max_width;
    *height = m_preview_max_height;
}

int SecCamera::getPreviewPixelFormat(void)
{
    return m_preview_v4lformat;
}


// ======================================================================
// Snapshot
int SecCamera::beginSnapshot(bool burst)
{
    ALOGV("%s :", __func__);

    int nframes, ret = 0;

    CHECK_FD(m_cam_fd);

    LOG_TIME_DEFINE(0)
    LOG_TIME_DEFINE(1)

    if (m_flag_camera_start > 0) {
        LOG_TIME_START(0)
        ALOGW("WARN(%s):Camera was in preview, should have been stopped\n", __func__);
        stopPreview();
        LOG_TIME_END(0)
    }

    memset(&m_events_c, 0, sizeof(m_events_c));
    m_events_c.fd = m_cam_fd;
    m_events_c.events = POLLIN | POLLERR;

    LOG_TIME_START(1) // prepare

    ret = fimc_v4l2_enum_fmt(m_cam_fd, m_snapshot_v4lformat);
    CHECK(ret);

    ret = fimc_v4l2_s_fmt_cap(m_cam_fd, m_snapshot_height, m_snapshot_width, m_snapshot_v4lformat);
    CHECK(ret);

    nframes = 1;
    ret = fimc_v4l2_reqbufs(m_cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, nframes);
    CHECK(ret);

    m_capture_bufs = (fimc_buffer *) malloc(sizeof(fimc_buffer) * nframes);
    m_capture_bufs_size = nframes;
    m_capture_burst = burst;
    for(int i = 0; i < m_capture_bufs_size; i++) {
        m_capture_bufs[i].index = i;
        ret = fimc_v4l2_querybuf(m_cam_fd, &m_capture_bufs[i], V4L2_BUF_TYPE_VIDEO_CAPTURE);
        CHECK(ret);
        ret = fimc_v4l2_qbuf(m_cam_fd, i);
        CHECK(ret);
    }

    ret = fimc_v4l2_streamon(m_cam_fd);
    CHECK(ret);
    LOG_TIME_END(1)

    return 0;
}

int SecCamera::endSnapshot(void)
{
    ALOGV("%s :", __func__);

    int ret;

    CHECK_FD(m_cam_fd);

    LOG_TIME_DEFINE(0)
    LOG_TIME_DEFINE(1)

    LOG_TIME_START(0)
    if(m_capture_bufs) {
        for(int i = 0; i < m_capture_bufs_size; i++) {
            if (m_capture_bufs[i].start) {
                munmap(m_capture_bufs[i].start, m_capture_bufs[i].length);
                ALOGV("munmap():virt. addr %p size = %d\n",
                        m_capture_bufs[i].start, m_capture_bufs[i].length);
            }
        }
        free(m_capture_bufs);
        m_capture_bufs = NULL;
    }
    LOG_TIME_END(0)

    LOG_TIME_START(1)
    ret = fimc_v4l2_streamoff(m_cam_fd);
    CHECK(ret);
    LOG_TIME_END(1)

    return ret;
}

/*
 * Set Jpeg quality & exif info and get JPEG data from camera ISP
 */
int SecCamera::getJpeg(unsigned int *phyaddr, unsigned char** jpeg_buf,
                       unsigned int *jpeg_size)
{
    ALOGV("%s :", __func__);

    int index, ret = 0;

    CHECK_FD(m_cam_fd);

    LOG_TIME_DEFINE(0)
    LOG_TIME_DEFINE(1)

    LOG_TIME_START(0)
    //Date time
    time_t rawtime;
    time(&rawtime);
    struct tm *timeinfo = localtime(&rawtime);

    ret = fimc_v4l2_s_ext_ctrl(m_cam_fd, V4L2_CID_CAMERA_EXIF_TIME_INFO, timeinfo);
    CHECK(ret);

    ret = fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAM_CAPTURE, 0);
    CHECK(ret);

    ret = fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_CAPTURE, 0);
    CHECK(ret);

    // capture
    ret = fimc_poll(&m_events_c);
    CHECK(ret);
    index = fimc_v4l2_dqbuf(m_cam_fd);
    if (!(0 <= index && index < m_capture_bufs_size)) {
        ALOGE("ERR(%s):wrong index = %d\n", __func__, index);
        return -1;
    }
    if(m_capture_burst) {
        ret = fimc_v4l2_qbuf(m_cam_fd, index);
        CHECK(ret);
    } else {
        fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_STREAM_PAUSE, 0);
    }
    LOG_TIME_END(0)

    LOG_TIME_START(1)
    *jpeg_size = fimc_v4l2_g_ctrl(m_cam_fd, V4L2_CID_CAM_JPEG_MAIN_SIZE);
    CHECK(*jpeg_size);

    int main_offset = fimc_v4l2_g_ctrl(m_cam_fd, V4L2_CID_CAM_JPEG_MAIN_OFFSET);
    CHECK(main_offset);
    m_postview_offset = fimc_v4l2_g_ctrl(m_cam_fd, V4L2_CID_CAM_JPEG_POSTVIEW_OFFSET);
    CHECK(m_postview_offset);

    ALOGI("Snapshot dqueued buffer=%d snapshot_width=%d snapshot_height=%d, size=%d, main_offset=%d",
            index, m_snapshot_width, m_snapshot_height, *jpeg_size, main_offset);

    *jpeg_buf = (unsigned char*)(m_capture_bufs[index].start) + main_offset;
    *phyaddr = getPhyAddrY(index) + m_postview_offset;
    LOG_TIME_END(1)

    LOG_CAMERA("getJpeg intervals: capture(%lu), poll_jpeg(%lu)  us",
                    LOG_TIME(0), LOG_TIME(1));

    return 0;
}

int SecCamera::getExif(unsigned char *pExifDst, unsigned char *pThumbSrc)
{
    JpegEncoder jpgEnc;

    ALOGV("%s : m_jpeg_thumbnail_width = %d, height = %d",
         __func__, m_jpeg_thumbnail_width, m_jpeg_thumbnail_height);
    if ((m_jpeg_thumbnail_width > 0) && (m_jpeg_thumbnail_height > 0)) {
        int inFormat = JPG_MODESEL_YCBCR;
        int outFormat = JPG_422;
        switch (m_snapshot_v4lformat) {
        case V4L2_PIX_FMT_NV12:
        case V4L2_PIX_FMT_NV21:
        case V4L2_PIX_FMT_NV12T:
        case V4L2_PIX_FMT_YUV420:
            outFormat = JPG_420;
            break;
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_UYVY:
        case V4L2_PIX_FMT_YUV422P:
            outFormat = JPG_422;
            break;
        }

        if (jpgEnc.setConfig(JPEG_SET_ENCODE_IN_FORMAT, inFormat) != JPG_SUCCESS)
            return -1;

        if (jpgEnc.setConfig(JPEG_SET_SAMPING_MODE, outFormat) != JPG_SUCCESS)
            return -1;

        if (jpgEnc.setConfig(JPEG_SET_ENCODE_QUALITY, JPG_QUALITY_LEVEL_2) != JPG_SUCCESS)
            return -1;

        int thumbWidth, thumbHeight, thumbSrcSize;
        getThumbnailConfig(&thumbWidth, &thumbHeight, &thumbSrcSize);
        if (jpgEnc.setConfig(JPEG_SET_ENCODE_WIDTH, thumbWidth) != JPG_SUCCESS)
            return -1;

        if (jpgEnc.setConfig(JPEG_SET_ENCODE_HEIGHT, thumbHeight) != JPG_SUCCESS)
            return -1;

        char *pInBuf = (char *)jpgEnc.getInBuf(thumbSrcSize);
        if (pInBuf == NULL)
            return -1;
        memcpy(pInBuf, pThumbSrc, thumbSrcSize);

        unsigned int thumbSize;

        jpgEnc.encode(&thumbSize, NULL);

        ALOGV("%s : enableThumb set to true", __func__);
        //TODO: findout why this causes memory corruption,
        //      probably we make bad thumbnail MemoryHeapBase
        //mExifInfo.enableThumb = true;
    } else {
        ALOGV("%s : enableThumb set to false", __func__);
        mExifInfo.enableThumb = false;
    }

    unsigned int exifSize;

    setExifChangedAttribute();

    ALOGV("%s: calling jpgEnc.makeExif, mExifInfo.width set to %d, height to %d\n",
         __func__, mExifInfo.width, mExifInfo.height);

    jpgEnc.makeExif(pExifDst, &mExifInfo, &exifSize, true);

    return exifSize;
}

void SecCamera::getPostViewConfig(int *width, int *height, int *size)
{
    if (m_preview_width == 1024) {
        *width = BACK_CAMERA_POSTVIEW_WIDE_WIDTH;
        *height = BACK_CAMERA_POSTVIEW_HEIGHT;
        *size = BACK_CAMERA_POSTVIEW_WIDE_WIDTH * BACK_CAMERA_POSTVIEW_HEIGHT * BACK_CAMERA_POSTVIEW_BPP / 8;
    } else {
        *width = BACK_CAMERA_POSTVIEW_WIDTH;
        *height = BACK_CAMERA_POSTVIEW_HEIGHT;
        *size = BACK_CAMERA_POSTVIEW_WIDTH * BACK_CAMERA_POSTVIEW_HEIGHT * BACK_CAMERA_POSTVIEW_BPP / 8;
    }
    ALOGV("[5B] m_preview_width : %d, mPostViewWidth = %d mPostViewHeight = %d mPostViewSize = %d",
            m_preview_width, *width, *height, *size);
}

void SecCamera::getThumbnailConfig(int *width, int *height, int *size)
{
    if (m_camera_id == CAMERA_ID_BACK) {
        *width  = BACK_CAMERA_THUMBNAIL_WIDTH;
        *height = BACK_CAMERA_THUMBNAIL_HEIGHT;
        *size   = BACK_CAMERA_THUMBNAIL_WIDTH * BACK_CAMERA_THUMBNAIL_HEIGHT
                    * BACK_CAMERA_THUMBNAIL_BPP / 8;
    } else {
        *width  = FRONT_CAMERA_THUMBNAIL_WIDTH;
        *height = FRONT_CAMERA_THUMBNAIL_HEIGHT;
        *size   = FRONT_CAMERA_THUMBNAIL_WIDTH * FRONT_CAMERA_THUMBNAIL_HEIGHT
                    * FRONT_CAMERA_THUMBNAIL_BPP / 8;
    }
}

int SecCamera::getPostViewOffset(void)
{
    return m_postview_offset;
}

int SecCamera::getJpeg(unsigned char *yuv_buf, unsigned char *jpeg_buf,
                       unsigned int *jpeg_size)
{
    ALOGV("%s :", __func__);

    int index;
    unsigned char *addr;
    int ret = 0;

    CHECK_FD(m_cam_fd);

    LOG_TIME_DEFINE(0)
    LOG_TIME_DEFINE(1)
    LOG_TIME_DEFINE(2)

    LOG_TIME_START(0) // capture
    fimc_poll(&m_events_c);
    index = fimc_v4l2_dqbuf(m_cam_fd);
    if (!(0 <= index && index < m_capture_bufs_size)) {
        ALOGE("ERR(%s):wrong index = %d\n", __func__, index);
        return -1;
    }
    fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_STREAM_PAUSE, 0);
    ALOGV("\nsnapshot dequeued buffer = %d snapshot_width = %d snapshot_height = %d\n\n",
            index, m_snapshot_width, m_snapshot_height);
    LOG_TIME_END(0)

    LOG_TIME_START(1)
    ALOGV("%s : calling memcpy from m_capture_bufs", __func__);
    memcpy(yuv_buf, (unsigned char*)m_capture_bufs[index].start,
            m_snapshot_width * m_snapshot_height * 2);
    LOG_TIME_END(1)

    LOG_TIME_START(2)
    /* JPEG encoding */
    JpegEncoder jpgEnc;
    int inFormat = JPG_MODESEL_YCBCR;
    int outFormat = JPG_422;

    switch (m_snapshot_v4lformat) {
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV21:
    case V4L2_PIX_FMT_NV12T:
    case V4L2_PIX_FMT_YUV420:
        outFormat = JPG_420;
        break;
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_YUV422P:
    default:
        outFormat = JPG_422;
        break;
    }

    if (jpgEnc.setConfig(JPEG_SET_ENCODE_IN_FORMAT, inFormat) != JPG_SUCCESS)
        ALOGE("[JPEG_SET_ENCODE_IN_FORMAT] Error\n");

    if (jpgEnc.setConfig(JPEG_SET_SAMPING_MODE, outFormat) != JPG_SUCCESS)
        ALOGE("[JPEG_SET_SAMPING_MODE] Error\n");

    image_quality_type_t jpegQuality;
    if (m_jpeg_quality >= 90)
        jpegQuality = JPG_QUALITY_LEVEL_1;
    else if (m_jpeg_quality >= 80)
        jpegQuality = JPG_QUALITY_LEVEL_2;
    else if (m_jpeg_quality >= 70)
        jpegQuality = JPG_QUALITY_LEVEL_3;
    else
        jpegQuality = JPG_QUALITY_LEVEL_4;

    if (jpgEnc.setConfig(JPEG_SET_ENCODE_QUALITY, jpegQuality) != JPG_SUCCESS)
        ALOGE("[JPEG_SET_ENCODE_QUALITY] Error\n");
    if (jpgEnc.setConfig(JPEG_SET_ENCODE_WIDTH, m_snapshot_width) != JPG_SUCCESS)
        ALOGE("[JPEG_SET_ENCODE_WIDTH] Error\n");

    if (jpgEnc.setConfig(JPEG_SET_ENCODE_HEIGHT, m_snapshot_height) != JPG_SUCCESS)
        ALOGE("[JPEG_SET_ENCODE_HEIGHT] Error\n");

    unsigned int snapshot_size = m_snapshot_width * m_snapshot_height * 2;
    unsigned char *pInBuf = (unsigned char *)jpgEnc.getInBuf(snapshot_size);

    if (pInBuf == NULL) {
        ALOGE("JPEG input buffer is NULL!!\n");
        return -1;
    }
    memcpy(pInBuf, yuv_buf, snapshot_size);

    setExifChangedAttribute();
    jpgEnc.encode(jpeg_size, &mExifInfo);
    LOG_TIME_END(2)

    uint64_t outbuf_size;
    unsigned char *pOutBuf = (unsigned char *)jpgEnc.getOutBuf(&outbuf_size);

    if (pOutBuf == NULL) {
        ALOGE("JPEG output buffer is NULL!!\n");
        return -1;
    }

    memcpy(jpeg_buf, pOutBuf, outbuf_size);

    LOG_CAMERA("getJpeg intervals: capture(%lu), memcpy(%lu), yuv2Jpeg(%lu)  us",
                    LOG_TIME(0), LOG_TIME(1), LOG_TIME(2));

    return 0;
}


int SecCamera::setSnapshotSize(int width, int height)
{
    ALOGV("%s(width(%d), height(%d))", __func__, width, height);

    m_snapshot_width  = width;
    m_snapshot_height = height;

    return 0;
}

void SecCamera::getSnapshotSize(int *width, int *height, int *frame_size)
{
    *width  = m_snapshot_width;
    *height = m_snapshot_height;

    int frame = 0;

    frame = m_frameSize(m_snapshot_v4lformat, m_snapshot_width, m_snapshot_height);

    // set it big.
    if (frame == 0)
        frame = m_snapshot_width * m_snapshot_height * BPP;

    *frame_size = frame;
}

void SecCamera::getSnapshotMaxSize(int *width, int *height)
{
    switch (m_camera_id) {
    case CAMERA_ID_FRONT:
        m_snapshot_max_width  = MAX_FRONT_CAMERA_SNAPSHOT_WIDTH;
        m_snapshot_max_height = MAX_FRONT_CAMERA_SNAPSHOT_HEIGHT;
        break;

    default:
    case CAMERA_ID_BACK:
        m_snapshot_max_width  = MAX_BACK_CAMERA_SNAPSHOT_WIDTH;
        m_snapshot_max_height = MAX_BACK_CAMERA_SNAPSHOT_HEIGHT;
        break;
    }

    *width  = m_snapshot_max_width;
    *height = m_snapshot_max_height;
}

int SecCamera::setSnapshotPixelFormat(int pixel_format)
{
    int v4lpixelformat= pixel_format;

    if (m_snapshot_v4lformat != v4lpixelformat) {
        m_snapshot_v4lformat = v4lpixelformat;
    }

    if (m_snapshot_v4lformat == V4L2_PIX_FMT_YUV420)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_YUV420", __func__);
    else if (m_snapshot_v4lformat == V4L2_PIX_FMT_NV12)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_NV12", __func__);
    else if (m_snapshot_v4lformat == V4L2_PIX_FMT_NV12T)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_NV12T", __func__);
    else if (m_snapshot_v4lformat == V4L2_PIX_FMT_NV21)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_NV21", __func__);
    else if (m_snapshot_v4lformat == V4L2_PIX_FMT_YUV422P)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_YUV422P", __func__);
    else if (m_snapshot_v4lformat == V4L2_PIX_FMT_YUYV)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_YUYV", __func__);
    else if (m_snapshot_v4lformat == V4L2_PIX_FMT_UYVY)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_UYVY", __func__);
    else if (m_snapshot_v4lformat == V4L2_PIX_FMT_RGB565)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_RGB565", __func__);
    else if (m_snapshot_v4lformat == V4L2_PIX_FMT_JPEG)
        ALOGV("%s : SnapshotFormat:V4L2_PIX_FMT_JPEG", __func__);
    else
        ALOGE("SnapshotFormat:UnknownFormat");

    return 0;
}

int SecCamera::getSnapshotPixelFormat(void)
{
    return m_snapshot_v4lformat;
}

// ======================================================================
// Settings

int SecCamera::getCameraId(void)
{
    return m_camera_id;
}

// -----------------------------------

int SecCamera::setAutofocus(void)
{
    ALOGV("%s :", __func__);

    CHECK_FD(m_cam_fd);

    if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_SET_AUTO_FOCUS, AUTO_FOCUS_ON) < 0) {
            ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_SET_AUTO_FOCUS", __func__);
        return -1;
    }

    return 0;
}

int SecCamera::getAutoFocusResult(void)
{
    int af_result, count, ret;

    ALOGV("%s :", __func__);

    CHECK_FD(m_cam_fd);

    for (count = 0; count < FIRST_AF_SEARCH_COUNT; count++) {
        ret = fimc_v4l2_g_ctrl(m_cam_fd, V4L2_CID_CAMERA_AUTO_FOCUS_RESULT_FIRST);
        if (ret != AF_PROGRESS)
            break;
        usleep(AF_DELAY);
    }

    if ((count >= FIRST_AF_SEARCH_COUNT) || (ret != AF_SUCCESS)) {
        ALOGV("%s : 1st AF timed out, failed, or was canceled", __func__);
        af_result = 0;
        goto finish_auto_focus;
    }

    af_result = 1;
    ALOGV("%s : AF was successful, returning %d", __func__, af_result);

finish_auto_focus:
    if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FINISH_AUTO_FOCUS, 0) < 0) {
        ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_FINISH_AUTO_FOCUS", __func__);
        return -1;
    }
    return af_result;
}

int SecCamera::cancelAutofocus(void)
{
    ALOGV("%s :", __func__);

    CHECK_FD(m_cam_fd);

    if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_SET_AUTO_FOCUS, AUTO_FOCUS_OFF) < 0) {
        ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_SET_AUTO_FOCUS", __func__);
        return -1;
    }

    return 0;
}

// -----------------------------------

int SecCamera::zoomIn(void)
{
    ALOGV("%s :", __func__);
    return 0;
}

int SecCamera::zoomOut(void)
{
    ALOGV("%s :", __func__);
    return 0;
}

// -----------------------------------

int SecCamera::setRotate(int angle)
{
    ALOGV("%s(angle(%d))", __func__, angle);

    if (m_angle != angle) {
        switch (angle) {
        case -360:
        case    0:
        case  360:
            m_angle = 0;
            break;

        case -270:
        case   90:
            m_angle = 90;
            break;

        case -180:
        case  180:
            m_angle = 180;
            break;

        case  -90:
        case  270:
            m_angle = 270;
            break;

        default:
            ALOGE("ERR(%s):Invalid angle(%d)", __func__, angle);
            return -1;
        }

        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_ROTATION, angle) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_ROTATION", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getRotate(void)
{
    ALOGV("%s : angle(%d)", __func__, m_angle);
    return m_angle;
}

int SecCamera::setFrameRate(int frame_rate)
{
    ALOGV("%s(FrameRate(%d))", __func__, frame_rate);

    if (frame_rate < FRAME_RATE_AUTO || FRAME_RATE_MAX < frame_rate )
        ALOGE("ERR(%s):Invalid frame_rate(%d)", __func__, frame_rate);

    if (m_params->capture.timeperframe.denominator != (unsigned)frame_rate) {
        m_params->capture.timeperframe.denominator = frame_rate;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FRAME_RATE, frame_rate) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_FRAME_RATE", __func__);
                return -1;
            }
        }
    }

    return 0;
}

// -----------------------------------

int SecCamera::setVerticalMirror(void)
{
    ALOGV("%s :", __func__);

    CHECK_FD(m_cam_fd);

    if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_VFLIP, 0) < 0) {
        ALOGE("ERR(%s):Fail on V4L2_CID_VFLIP", __func__);
        return -1;
    }

    return 0;
}

int SecCamera::setHorizontalMirror(void)
{
    ALOGV("%s :", __func__);

    CHECK_FD(m_cam_fd);

    if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_HFLIP, 0) < 0) {
        ALOGE("ERR(%s):Fail on V4L2_CID_HFLIP", __func__);
        return -1;
    }

    return 0;
}

// -----------------------------------

int SecCamera::setWhiteBalance(int white_balance)
{
    ALOGV("%s(white_balance(%d))", __func__, white_balance);

    if (white_balance <= WHITE_BALANCE_BASE || WHITE_BALANCE_MAX <= white_balance) {
        ALOGE("ERR(%s):Invalid white_balance(%d)", __func__, white_balance);
        return -1;
    }

    if (m_params->white_balance != white_balance) {
        m_params->white_balance = white_balance;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_WHITE_BALANCE, white_balance) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_WHITE_BALANCE", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getWhiteBalance(void)
{
    ALOGV("%s : white_balance(%d)", __func__, m_params->white_balance);
    return m_params->white_balance;
}

// -----------------------------------

int SecCamera::setBrightness(int brightness)
{
    ALOGV("%s(brightness(%d))", __func__, brightness);

    brightness += EV_DEFAULT;

    if (brightness < EV_MINUS_4 || EV_PLUS_4 < brightness) {
        ALOGE("ERR(%s):Invalid brightness(%d)", __func__, brightness);
        return -1;
    }

    if (m_params->brightness != brightness) {
        m_params->brightness = brightness;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_BRIGHTNESS, brightness) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_BRIGHTNESS", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getBrightness(void)
{
    ALOGV("%s : brightness(%d)", __func__, m_params->brightness);
    return m_params->brightness;
}

// -----------------------------------

int SecCamera::setImageEffect(int image_effect)
{
    ALOGV("%s(image_effect(%d))", __func__, image_effect);

    if (image_effect <= IMAGE_EFFECT_BASE || IMAGE_EFFECT_MAX <= image_effect) {
        ALOGE("ERR(%s):Invalid image_effect(%d)", __func__, image_effect);
        return -1;
    }

    if (m_params->effects != image_effect) {
        m_params->effects = image_effect;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_EFFECT, image_effect) < 0) {
                 ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_EFFECT", __func__);
                 return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getImageEffect(void)
{
    ALOGV("%s : image_effect(%d)", __func__, m_params->effects);
    return m_params->effects;
}

// ======================================================================
int SecCamera::setAntiBanding(int anti_banding)
{
    ALOGV("%s(anti_banding(%d))", __func__, anti_banding);

    if (anti_banding < ANTI_BANDING_AUTO || ANTI_BANDING_OFF < anti_banding) {
        ALOGE("ERR(%s):Invalid anti_banding (%d)", __func__, anti_banding);
        return -1;
    }

    if (m_anti_banding != anti_banding) {
        m_anti_banding = anti_banding;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_ANTI_BANDING, anti_banding) < 0) {
                 ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_ANTI_BANDING", __func__);
                 return -1;
            }
        }
    }

    return 0;
}

//======================================================================
int SecCamera::setSceneMode(int scene_mode)
{
    ALOGV("%s(scene_mode(%d))", __func__, scene_mode);

    if (scene_mode <= SCENE_MODE_BASE || SCENE_MODE_MAX <= scene_mode) {
        ALOGE("ERR(%s):Invalid scene_mode (%d)", __func__, scene_mode);
        return -1;
    }

    if (m_params->scene_mode != scene_mode) {
        m_params->scene_mode = scene_mode;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_SCENE_MODE, scene_mode) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_SCENE_MODE", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getSceneMode(void)
{
    return m_params->scene_mode;
}

//======================================================================

int SecCamera::setFlashMode(int flash_mode)
{
    ALOGV("%s(flash_mode(%d))", __func__, flash_mode);

    if (flash_mode <= FLASH_MODE_BASE || FLASH_MODE_MAX <= flash_mode) {
        ALOGE("ERR(%s):Invalid flash_mode (%d)", __func__, flash_mode);
        return -1;
    }

    if (m_params->flash_mode != flash_mode) {
        m_params->flash_mode = flash_mode;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FLASH_MODE, flash_mode) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_FLASH_MODE", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getFlashMode(void)
{
    return m_params->flash_mode;
}

//======================================================================

int SecCamera::setISO(int iso_value)
{
    ALOGV("%s(iso_value(%d))", __func__, iso_value);
    if (iso_value < ISO_AUTO || ISO_MAX <= iso_value) {
        ALOGE("ERR(%s):Invalid iso_value (%d)", __func__, iso_value);
        return -1;
    }

    if (m_params->iso != iso_value) {
        m_params->iso = iso_value;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_ISO, iso_value) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_ISO", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getISO(void)
{
    return m_params->iso;
}

//======================================================================

int SecCamera::setContrast(int contrast_value)
{
    ALOGV("%s(contrast_value(%d))", __func__, contrast_value);

    if (contrast_value < CONTRAST_MINUS_2 || CONTRAST_MAX <= contrast_value) {
        ALOGE("ERR(%s):Invalid contrast_value (%d)", __func__, contrast_value);
        return -1;
    }

    if (m_params->contrast != contrast_value) {
        m_params->contrast = contrast_value;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_CONTRAST, contrast_value) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_CONTRAST", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getContrast(void)
{
    return m_params->contrast;
}

//======================================================================

int SecCamera::setSaturation(int saturation_value)
{
    ALOGV("%s(saturation_value(%d))", __func__, saturation_value);

    if (saturation_value <SATURATION_MINUS_2 || SATURATION_MAX<= saturation_value) {
        ALOGE("ERR(%s):Invalid saturation_value (%d)", __func__, saturation_value);
        return -1;
    }

    if (m_params->saturation != saturation_value) {
        m_params->saturation = saturation_value;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_SATURATION, saturation_value) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_SATURATION", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getSaturation(void)
{
    return m_params->saturation;
}

//======================================================================

int SecCamera::setSharpness(int sharpness_value)
{
    ALOGV("%s(sharpness_value(%d))", __func__, sharpness_value);

    if (sharpness_value < SHARPNESS_MINUS_2 || SHARPNESS_MAX <= sharpness_value) {
        ALOGE("ERR(%s):Invalid sharpness_value (%d)", __func__, sharpness_value);
        return -1;
    }

    if (m_params->sharpness != sharpness_value) {
        m_params->sharpness = sharpness_value;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_SHARPNESS, sharpness_value) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_SHARPNESS", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getSharpness(void)
{
    return m_params->sharpness;
}

//======================================================================

int SecCamera::setWDR(int wdr_value)
{
    ALOGV("%s(wdr_value(%d))", __func__, wdr_value);

    if (wdr_value < WDR_OFF || WDR_MAX <= wdr_value) {
        ALOGE("ERR(%s):Invalid wdr_value (%d)", __func__, wdr_value);
        return -1;
    }

    if (m_wdr != wdr_value) {
        m_wdr = wdr_value;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_WDR, wdr_value) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_WDR", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getWDR(void)
{
    return m_wdr;
}

//======================================================================

int SecCamera::setAntiShake(int anti_shake)
{
    ALOGV("%s(anti_shake(%d))", __func__, anti_shake);

    if (anti_shake < ANTI_SHAKE_OFF || ANTI_SHAKE_MAX <= anti_shake) {
        ALOGE("ERR(%s):Invalid anti_shake (%d)", __func__, anti_shake);
        return -1;
    }

    if (m_anti_shake != anti_shake) {
        m_anti_shake = anti_shake;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_ANTI_SHAKE, anti_shake) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_ANTI_SHAKE", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getAntiShake(void)
{
    return m_anti_shake;
}

//======================================================================


int SecCamera::setMetering(int metering_value)
{
    ALOGV("%s(metering (%d))", __func__, metering_value);

    if (metering_value <= METERING_BASE || METERING_MAX <= metering_value) {
        ALOGE("ERR(%s):Invalid metering_value (%d)", __func__, metering_value);
        return -1;
    }

    if (m_params->metering != metering_value) {
        m_params->metering = metering_value;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_METERING, metering_value) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_METERING", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getMetering(void)
{
    return m_params->metering;
}

//======================================================================

int SecCamera::setJpegQuality(int jpeg_quality)
{
    ALOGV("%s(jpeg_quality (%d))", __func__, jpeg_quality);

    if (jpeg_quality < JPEG_QUALITY_ECONOMY || JPEG_QUALITY_MAX <= jpeg_quality) {
        ALOGE("ERR(%s):Invalid jpeg_quality (%d)", __func__, jpeg_quality);
        return -1;
    }

    if (m_jpeg_quality != jpeg_quality) {
        m_jpeg_quality = jpeg_quality;
        if (m_flag_camera_start && (m_camera_id == CAMERA_ID_BACK)) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAM_JPEG_QUALITY, jpeg_quality) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAM_JPEG_QUALITY", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getJpegQuality(void)
{
    return m_jpeg_quality;
}

//======================================================================

int SecCamera::setZoom(int zoom_level)
{
    ALOGV("%s(zoom_level (%d))", __func__, zoom_level);

    if (zoom_level < ZOOM_LEVEL_0 || ZOOM_LEVEL_MAX <= zoom_level) {
        ALOGE("ERR(%s):Invalid zoom_level (%d)", __func__, zoom_level);
        return -1;
    }

    if (m_zoom_level != zoom_level) {
        m_zoom_level = zoom_level;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_ZOOM, zoom_level) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_ZOOM", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getZoom(void)
{
    return m_zoom_level;
}

//======================================================================

int SecCamera::setObjectTracking(int object_tracking)
{
    ALOGV("%s(object_tracking (%d))", __func__, object_tracking);

    if (object_tracking < OBJECT_TRACKING_OFF || OBJECT_TRACKING_MAX <= object_tracking) {
        ALOGE("ERR(%s):Invalid object_tracking (%d)", __func__, object_tracking);
        return -1;
    }

    if (m_object_tracking != object_tracking) {
        m_object_tracking = object_tracking;
    }

    return 0;
}

int SecCamera::getObjectTracking(void)
{
    return m_object_tracking;
}

int SecCamera::getObjectTrackingStatus(void)
{
    int obj_status = 0;
    obj_status = fimc_v4l2_g_ctrl(m_cam_fd, V4L2_CID_CAMERA_OBJ_TRACKING_STATUS);
    return obj_status;
}

int SecCamera::setObjectTrackingStartStop(int start_stop)
{
    ALOGV("%s(object_tracking_start_stop (%d))", __func__, start_stop);

    if (m_object_tracking_start_stop != start_stop) {
        m_object_tracking_start_stop = start_stop;
        if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_OBJ_TRACKING_START_STOP, start_stop) < 0) {
            ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_OBJ_TRACKING_START_STOP", __func__);
            return -1;
        }
    }

    return 0;
}

int SecCamera::setTouchAFStartStop(int start_stop)
{
    ALOGV("%s(touch_af_start_stop (%d))", __func__, start_stop);

    if (m_touch_af_start_stop != start_stop && m_flag_camera_start) {
        m_touch_af_start_stop = start_stop;
        if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_TOUCH_AF_START_STOP, start_stop) < 0) {
            ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_TOUCH_AF_START_STOP", __func__);
            return -1;
        }
    }

    return 0;
}

//======================================================================

int SecCamera::setSmartAuto(int smart_auto)
{
    ALOGV("%s(smart_auto (%d))", __func__, smart_auto);

    if (smart_auto < SMART_AUTO_OFF || SMART_AUTO_MAX <= smart_auto) {
        ALOGE("ERR(%s):Invalid smart_auto (%d)", __func__, smart_auto);
        return -1;
    }

    if (m_smart_auto != smart_auto) {
        m_smart_auto = smart_auto;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_SMART_AUTO, smart_auto) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_SMART_AUTO", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getSmartAuto(void)
{
    return m_smart_auto;
}

int SecCamera::getAutosceneStatus(void)
{
    int autoscene_status = -1;

    if (getSmartAuto() == SMART_AUTO_ON) {
        autoscene_status = fimc_v4l2_g_ctrl(m_cam_fd, V4L2_CID_CAMERA_SMART_AUTO_STATUS);

        if ((autoscene_status < SMART_AUTO_STATUS_AUTO) || (autoscene_status > SMART_AUTO_STATUS_MAX)) {
            ALOGE("ERR(%s):Invalid getAutosceneStatus (%d)", __func__, autoscene_status);
            return -1;
        }
    }
    //ALOGV("%s :    autoscene_status (%d)", __func__, autoscene_status);
    return autoscene_status;
}
//======================================================================

int SecCamera::setBeautyShot(int beauty_shot)
{
    ALOGV("%s(beauty_shot (%d))", __func__, beauty_shot);

    if (beauty_shot < BEAUTY_SHOT_OFF || BEAUTY_SHOT_MAX <= beauty_shot) {
        ALOGE("ERR(%s):Invalid beauty_shot (%d)", __func__, beauty_shot);
        return -1;
    }

    if (m_beauty_shot != beauty_shot) {
        m_beauty_shot = beauty_shot;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_BEAUTY_SHOT, beauty_shot) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_BEAUTY_SHOT", __func__);
                return -1;
            }
        }

        setFaceDetect(FACE_DETECTION_ON_BEAUTY);
    }

    return 0;
}

int SecCamera::getBeautyShot(void)
{
    return m_beauty_shot;
}

//======================================================================

int SecCamera::setVintageMode(int vintage_mode)
{
    ALOGV("%s(vintage_mode(%d))", __func__, vintage_mode);

    if (vintage_mode <= VINTAGE_MODE_BASE || VINTAGE_MODE_MAX <= vintage_mode) {
        ALOGE("ERR(%s):Invalid vintage_mode (%d)", __func__, vintage_mode);
        return -1;
    }

    if (m_vintage_mode != vintage_mode) {
        m_vintage_mode = vintage_mode;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_VINTAGE_MODE, vintage_mode) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_VINTAGE_MODE", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getVintageMode(void)
{
    return m_vintage_mode;
}

//======================================================================

int SecCamera::setFocusMode(int focus_mode)
{
    ALOGV("%s(focus_mode(%d))", __func__, focus_mode);

    if (FOCUS_MODE_MAX <= focus_mode) {
        ALOGE("ERR(%s):Invalid focus_mode (%d)", __func__, focus_mode);
        return -1;
    }

    if (m_params->focus_mode != focus_mode) {
        m_params->focus_mode = focus_mode;

        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FOCUS_MODE, focus_mode) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_FOCUS_MODE", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getFocusMode(void)
{
    return m_params->focus_mode;
}

//======================================================================

int SecCamera::setFaceDetect(int face_detect)
{
    ALOGV("%s(face_detect(%d))", __func__, face_detect);

    if (m_face_detect != face_detect) {
        m_face_detect = face_detect;
        if (m_flag_camera_start) {
            if (m_face_detect != FACE_DETECTION_OFF) {
                if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FOCUS_MODE, FOCUS_MODE_AUTO) < 0) {
                    ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_FOCUS_MODin face detecion", __func__);
                    return -1;
                }
            }
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FACE_DETECTION, face_detect) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_FACE_DETECTION", __func__);
                return -1;
            }
        }
    }

    return 0;
}

int SecCamera::getFaceDetect(void)
{
    return m_face_detect;
}

//======================================================================

int SecCamera::setGPSLatitude(const char *gps_latitude)
{
    double conveted_latitude = 0;
    ALOGV("%s(gps_latitude(%s))", __func__, gps_latitude);
    if (gps_latitude == NULL)
        m_gps_latitude = 0;
    else {
        conveted_latitude = atof(gps_latitude);
        m_gps_latitude = (long)(conveted_latitude * 10000 / 1);
    }

    ALOGV("%s(m_gps_latitude(%ld))", __func__, m_gps_latitude);
    return 0;
}

int SecCamera::setGPSLongitude(const char *gps_longitude)
{
    double conveted_longitude = 0;
    ALOGV("%s(gps_longitude(%s))", __func__, gps_longitude);
    if (gps_longitude == NULL)
        m_gps_longitude = 0;
    else {
        conveted_longitude = atof(gps_longitude);
        m_gps_longitude = (long)(conveted_longitude * 10000 / 1);
    }

    ALOGV("%s(m_gps_longitude(%ld))", __func__, m_gps_longitude);
    return 0;
}

int SecCamera::setGPSAltitude(const char *gps_altitude)
{
    double conveted_altitude = 0;
    ALOGV("%s(gps_altitude(%s))", __func__, gps_altitude);
    if (gps_altitude == NULL)
        m_gps_altitude = 0;
    else {
        conveted_altitude = atof(gps_altitude);
        m_gps_altitude = (long)(conveted_altitude * 100 / 1);
    }

    ALOGV("%s(m_gps_altitude(%ld))", __func__, m_gps_altitude);
    return 0;
}

int SecCamera::setGPSTimeStamp(const char *gps_timestamp)
{
    ALOGV("%s(gps_timestamp(%s))", __func__, gps_timestamp);
    if (gps_timestamp == NULL)
        m_gps_timestamp = 0;
    else
        m_gps_timestamp = atol(gps_timestamp);

    ALOGV("%s(m_gps_timestamp(%ld))", __func__, m_gps_timestamp);
    return 0;
}

int SecCamera::setGPSProcessingMethod(const char *gps_processing_method)
{
    ALOGV("%s(gps_processing_method(%s))", __func__, gps_processing_method);
    memset(mExifInfo.gps_processing_method, 0, sizeof(mExifInfo.gps_processing_method));
    if (gps_processing_method != NULL) {
        size_t len = strlen(gps_processing_method);
        if (len > sizeof(mExifInfo.gps_processing_method)) {
            len = sizeof(mExifInfo.gps_processing_method);
        }
        memcpy(mExifInfo.gps_processing_method, gps_processing_method, len);
    }
    return 0;
}

int SecCamera::setFaceDetectLockUnlock(int facedetect_lockunlock)
{
    ALOGV("%s(facedetect_lockunlock(%d))", __func__, facedetect_lockunlock);

    if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_FACEDETECT_LOCKUNLOCK, facedetect_lockunlock) < 0) {
        ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_FACEDETECT_LOCKUNLOCK", __func__);
        return -1;
    }

    return 0;
}

int SecCamera::setObjectPosition(int x, int y)
{
    ALOGV("%s(setObjectPosition(x=%d, y=%d))", __func__, x, y);

    //if (m_preview_width == 640)
    //    x = x - 80;

    if (m_flag_camera_start) {
        if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_OBJECT_POSITION_X, x) < 0) {
            ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_OBJECT_POSITION_X", __func__);
            return -1;
        }
        if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_OBJECT_POSITION_Y, y) < 0) {
            ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_OBJECT_POSITION_Y", __func__);
            return -1;
        }
    }

    return 0;
}

//======================================================================

int SecCamera::setGamma(int gamma)
{
     ALOGV("%s(gamma(%d))", __func__, gamma);

     if (gamma < GAMMA_OFF || GAMMA_MAX <= gamma) {
         ALOGE("ERR(%s):Invalid gamma (%d)", __func__, gamma);
         return -1;
     }

     if (m_video_gamma != gamma) {
         m_video_gamma = gamma;
         if (m_flag_camera_start) {
             if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_SET_GAMMA, gamma) < 0) {
                 ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_SET_GAMMA", __func__);
                 return -1;
             }
         }
     }

     return 0;
}

//======================================================================

int SecCamera::setSlowAE(int slow_ae)
{
     ALOGV("%s(slow_ae(%d))", __func__, slow_ae);

     if (slow_ae < GAMMA_OFF || GAMMA_MAX <= slow_ae) {
         ALOGE("ERR(%s):Invalid slow_ae (%d)", __func__, slow_ae);
         return -1;
     }

     if (m_slow_ae!= slow_ae) {
         m_slow_ae = slow_ae;
         if (m_flag_camera_start) {
             if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_SET_SLOW_AE, slow_ae) < 0) {
                 ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_SET_SLOW_AE", __func__);
                 return -1;
             }
         }
     }

     return 0;
}

//======================================================================
int SecCamera::setRecordingSize(int width, int height)
{
     ALOGV("%s(width(%d), height(%d))", __func__, width, height);

     m_recording_width  = width;
     m_recording_height = height;

     return 0;
}

//======================================================================

int SecCamera::setExifOrientationInfo(int orientationInfo)
{
     ALOGV("%s(orientationInfo(%d))", __func__, orientationInfo);

     if (orientationInfo < 0) {
         ALOGE("ERR(%s):Invalid orientationInfo (%d)", __func__, orientationInfo);
         return -1;
     }
     m_exif_orientation = orientationInfo;

     return 0;
}

//======================================================================
int SecCamera::setBatchReflection()
{
    if (m_flag_camera_start) {
        if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_BATCH_REFLECTION, 1) < 0) {
             ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_BATCH_REFLECTION", __func__);
             return -1;
        }
    }

    return 0;
}

/*Video call*/
int SecCamera::setVTmode(int vtmode)
{
    ALOGV("%s(vtmode (%d))", __func__, vtmode);

    if (vtmode < VT_MODE_OFF || VT_MODE_MAX <= vtmode) {
        ALOGE("ERR(%s):Invalid vtmode (%d)", __func__, vtmode);
        return -1;
    }

    if (m_vtmode != vtmode) {
        m_vtmode = vtmode;
    }

    return 0;
}

/* Camcorder fix fps */
int SecCamera::setSensorMode(int sensor_mode)
{
    ALOGV("%s(sensor_mode (%d))", __func__, sensor_mode);

    if (sensor_mode < SENSOR_MODE_CAMERA || SENSOR_MODE_MOVIE < sensor_mode) {
        ALOGE("ERR(%s):Invalid sensor mode (%d)", __func__, sensor_mode);
        return -1;
    }

    if (m_sensor_mode != sensor_mode) {
        m_sensor_mode = sensor_mode;
    }

    return 0;
}

/*  Shot mode   */
/*  SINGLE = 0
*   CONTINUOUS = 1
*   PANORAMA = 2
*   SMILE = 3
*   SELF = 6
*/
int SecCamera::setShotMode(int shot_mode)
{
    ALOGV("%s(shot_mode (%d))", __func__, shot_mode);
    if (shot_mode < SHOT_MODE_SINGLE || SHOT_MODE_SELF < shot_mode) {
        ALOGE("ERR(%s):Invalid shot_mode (%d)", __func__, shot_mode);
        return -1;
    }
    m_shot_mode = shot_mode;

    return 0;
}

int SecCamera::getVTmode(void)
{
    return m_vtmode;
}

int SecCamera::setBlur(int blur_level)
{
    ALOGV("%s(level (%d))", __func__, blur_level);

    if (blur_level < BLUR_LEVEL_0 || BLUR_LEVEL_MAX <= blur_level) {
        ALOGE("ERR(%s):Invalid level (%d)", __func__, blur_level);
        return -1;
    }

    if (m_blur_level != blur_level) {
        m_blur_level = blur_level;
        if (m_flag_camera_start) {
            if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_VGA_BLUR, blur_level) < 0) {
                ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_VGA_BLUR", __func__);
                return -1;
            }
        }
    }
    return 0;
}

int SecCamera::getBlur(void)
{
    return m_blur_level;
}

int SecCamera::setDataLineCheck(int chk_dataline)
{
    ALOGV("%s(chk_dataline (%d))", __func__, chk_dataline);

    if (chk_dataline < CHK_DATALINE_OFF || CHK_DATALINE_MAX <= chk_dataline) {
        ALOGE("ERR(%s):Invalid chk_dataline (%d)", __func__, chk_dataline);
        return -1;
    }

    if (m_chk_dataline != chk_dataline) {
        m_chk_dataline = chk_dataline;
    }

    return 0;
}

int SecCamera::getDataLineCheck(void)
{
    return m_chk_dataline;
}

int SecCamera::setDataLineCheckStop(void)
{
    ALOGV("%s", __func__);

    if (m_flag_camera_start) {
        if (fimc_v4l2_s_ctrl(m_cam_fd, V4L2_CID_CAMERA_CHECK_DATALINE_STOP, 1) < 0) {
            ALOGE("ERR(%s):Fail on V4L2_CID_CAMERA_CHECK_DATALINE_STOP", __func__);
            return -1;
        }
    }
    return 0;
}

const __u8* SecCamera::getCameraSensorName(void)
{
    ALOGV("%s", __func__);

    return fimc_v4l2_enuminput(m_cam_fd, getCameraId());
}

#ifdef ENABLE_ESD_PREVIEW_CHECK
int SecCamera::getCameraSensorESDStatus(void)
{
    ALOGV("%s", __func__);

    // 0 : normal operation, 1 : abnormal operation
    int status = fimc_v4l2_g_ctrl(m_cam_fd, V4L2_CID_ESD_INT);

    return status;
}
#endif // ENABLE_ESD_PREVIEW_CHECK

// ======================================================================
// Jpeg

int SecCamera::setJpegThumbnailSize(int width, int height)
{
    ALOGV("%s(width(%d), height(%d))", __func__, width, height);

    m_jpeg_thumbnail_width  = width;
    m_jpeg_thumbnail_height = height;

    return 0;
}

int SecCamera::getJpegThumbnailSize(int *width, int  *height)
{
    if (width)
        *width   = m_jpeg_thumbnail_width;
    if (height)
        *height  = m_jpeg_thumbnail_height;

    return 0;
}

void SecCamera::setExifFixedAttribute()
{
    char property[PROPERTY_VALUE_MAX];

    //2 0th IFD TIFF Tags
    //3 Maker
    property_get("ro.product.brand", property, EXIF_DEF_MAKER);
    strncpy((char *)mExifInfo.maker, property,
                sizeof(mExifInfo.maker) - 1);
    mExifInfo.maker[sizeof(mExifInfo.maker) - 1] = '\0';
    //3 Model
    property_get("ro.product.model", property, EXIF_DEF_MODEL);
    strncpy((char *)mExifInfo.model, property,
                sizeof(mExifInfo.model) - 1);
    mExifInfo.model[sizeof(mExifInfo.model) - 1] = '\0';
    //3 Software
    property_get("ro.build.id", property, EXIF_DEF_SOFTWARE);
    strncpy((char *)mExifInfo.software, property,
                sizeof(mExifInfo.software) - 1);
    mExifInfo.software[sizeof(mExifInfo.software) - 1] = '\0';

    //3 YCbCr Positioning
    mExifInfo.ycbcr_positioning = EXIF_DEF_YCBCR_POSITIONING;

    //2 0th IFD Exif Private Tags
    //3 F Number
    mExifInfo.fnumber.num = EXIF_DEF_FNUMBER_NUM;
    mExifInfo.fnumber.den = EXIF_DEF_FNUMBER_DEN;
    //3 Exposure Program
    mExifInfo.exposure_program = EXIF_DEF_EXPOSURE_PROGRAM;
    //3 Exif Version
    memcpy(mExifInfo.exif_version, EXIF_DEF_EXIF_VERSION, sizeof(mExifInfo.exif_version));
    //3 Aperture
    uint32_t av = APEX_FNUM_TO_APERTURE((double)mExifInfo.fnumber.num/mExifInfo.fnumber.den);
    mExifInfo.aperture.num = av*EXIF_DEF_APEX_DEN;
    mExifInfo.aperture.den = EXIF_DEF_APEX_DEN;
    //3 Maximum lens aperture
    mExifInfo.max_aperture.num = mExifInfo.aperture.num;
    mExifInfo.max_aperture.den = mExifInfo.aperture.den;
    //3 Lens Focal Length
    if (m_camera_id == CAMERA_ID_BACK)
        mExifInfo.focal_length.num = BACK_CAMERA_FOCAL_LENGTH;
    else
        mExifInfo.focal_length.num = FRONT_CAMERA_FOCAL_LENGTH;

    mExifInfo.focal_length.den = EXIF_DEF_FOCAL_LEN_DEN;
    //3 User Comments
    strcpy((char *)mExifInfo.user_comment, EXIF_DEF_USERCOMMENTS);
    //3 Color Space information
    mExifInfo.color_space = EXIF_DEF_COLOR_SPACE;
    //3 Exposure Mode
    mExifInfo.exposure_mode = EXIF_DEF_EXPOSURE_MODE;

    //2 0th IFD GPS Info Tags
    unsigned char gps_version[4] = { 0x02, 0x02, 0x00, 0x00 };
    memcpy(mExifInfo.gps_version_id, gps_version, sizeof(gps_version));

    //2 1th IFD TIFF Tags
    mExifInfo.compression_scheme = EXIF_DEF_COMPRESSION;
    mExifInfo.x_resolution.num = EXIF_DEF_RESOLUTION_NUM;
    mExifInfo.x_resolution.den = EXIF_DEF_RESOLUTION_DEN;
    mExifInfo.y_resolution.num = EXIF_DEF_RESOLUTION_NUM;
    mExifInfo.y_resolution.den = EXIF_DEF_RESOLUTION_DEN;
    mExifInfo.resolution_unit = EXIF_DEF_RESOLUTION_UNIT;
}

void SecCamera::setExifChangedAttribute()
{
    //2 0th IFD TIFF Tags
    //3 Width
    mExifInfo.width = m_snapshot_width;
    //3 Height
    mExifInfo.height = m_snapshot_height;
    //3 Orientation
    switch (m_exif_orientation) {
    case 0:
        mExifInfo.orientation = EXIF_ORIENTATION_UP;
        break;
    case 90:
        mExifInfo.orientation = EXIF_ORIENTATION_90;
        break;
    case 180:
        mExifInfo.orientation = EXIF_ORIENTATION_180;
        break;
    case 270:
        mExifInfo.orientation = EXIF_ORIENTATION_270;
        break;
    default:
        mExifInfo.orientation = EXIF_ORIENTATION_UP;
        break;
    }
    //3 Date time
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime((char *)mExifInfo.date_time, 20, "%Y:%m:%d %H:%M:%S", timeinfo);

    //2 0th IFD Exif Private Tags
    //3 Exposure Time
    int shutterSpeed = fimc_v4l2_g_ctrl(m_cam_fd,
                                            V4L2_CID_CAMERA_GET_SHT_TIME);
    /* TBD - front camera needs to be fixed to support this g_ctrl,
       it current returns a negative err value, so avoid putting
       odd value into exif for now */
    if (shutterSpeed < 0) {
        ALOGE("%s: error %d getting shutterSpeed, camera_id = %d, using 100",
             __func__, shutterSpeed, m_camera_id);
        shutterSpeed = 100;
    }
    mExifInfo.exposure_time.num = 1;
    // x us -> 1/x s */
    mExifInfo.exposure_time.den = (uint32_t)(1000000 / shutterSpeed);

    //3 ISO Speed Rating
    int iso = m_params->iso;
    switch(iso) {
        case ISO_50:
            mExifInfo.iso_speed_rating = 50;
            break;
        case ISO_100:
            mExifInfo.iso_speed_rating = 100;
            break;
        case ISO_200:
            mExifInfo.iso_speed_rating = 200;
            break;
        case ISO_400:
            mExifInfo.iso_speed_rating = 400;
            break;
        case ISO_800:
            mExifInfo.iso_speed_rating = 800;
            break;
        case ISO_1600:
            mExifInfo.iso_speed_rating = 1600;
            break;
        default:
            mExifInfo.iso_speed_rating = 100;
            break;
    }

    uint32_t av, tv, bv, sv, ev;
    av = APEX_FNUM_TO_APERTURE((double)mExifInfo.fnumber.num / mExifInfo.fnumber.den);
    tv = APEX_EXPOSURE_TO_SHUTTER((double)mExifInfo.exposure_time.num / mExifInfo.exposure_time.den);
    sv = APEX_ISO_TO_FILMSENSITIVITY(mExifInfo.iso_speed_rating);
    bv = av + tv - sv;
    ev = av + tv;
    ALOGD("Shutter speed=%d us, iso=%d\n", shutterSpeed, mExifInfo.iso_speed_rating);
    ALOGD("AV=%d, TV=%d, SV=%d\n", av, tv, sv);

    //3 Shutter Speed
    mExifInfo.shutter_speed.num = tv*EXIF_DEF_APEX_DEN;
    mExifInfo.shutter_speed.den = EXIF_DEF_APEX_DEN;
    //3 Brightness
    mExifInfo.brightness.num = bv*EXIF_DEF_APEX_DEN;
    mExifInfo.brightness.den = EXIF_DEF_APEX_DEN;
    //3 Exposure Bias
    if (m_params->scene_mode == SCENE_MODE_BEACH_SNOW) {
        mExifInfo.exposure_bias.num = EXIF_DEF_APEX_DEN;
        mExifInfo.exposure_bias.den = EXIF_DEF_APEX_DEN;
    } else {
        mExifInfo.exposure_bias.num = 0;
        mExifInfo.exposure_bias.den = 0;
    }
    //3 Metering Mode
    switch (m_params->metering) {
    case METERING_SPOT:
        mExifInfo.metering_mode = EXIF_METERING_SPOT;
        break;
    case METERING_MATRIX:
        mExifInfo.metering_mode = EXIF_METERING_AVERAGE;
        break;
    case METERING_CENTER:
        mExifInfo.metering_mode = EXIF_METERING_CENTER;
        break;
    default :
        mExifInfo.metering_mode = EXIF_METERING_AVERAGE;
        break;
    }

    //3 Flash
    /*int flash = fimc_v4l2_g_ctrl(m_cam_fd, V4L2_CID_CAMERA_GET_FLASH_ONOFF);
    if (flash < 0)
        mExifInfo.flash = EXIF_DEF_FLASH;
    else
        mExifInfo.flash = flash;*/
    mExifInfo.flash = EXIF_DEF_FLASH;

    //3 White Balance
    if (m_params->white_balance == WHITE_BALANCE_AUTO)
        mExifInfo.white_balance = EXIF_WB_AUTO;
    else
        mExifInfo.white_balance = EXIF_WB_MANUAL;
    //3 Scene Capture Type
    switch (m_params->scene_mode) {
    case SCENE_MODE_PORTRAIT:
        mExifInfo.scene_capture_type = EXIF_SCENE_PORTRAIT;
        break;
    case SCENE_MODE_LANDSCAPE:
        mExifInfo.scene_capture_type = EXIF_SCENE_LANDSCAPE;
        break;
    case SCENE_MODE_NIGHTSHOT:
        mExifInfo.scene_capture_type = EXIF_SCENE_NIGHT;
        break;
    default:
        mExifInfo.scene_capture_type = EXIF_SCENE_STANDARD;
        break;
    }

    //2 0th IFD GPS Info Tags
    if (m_gps_latitude != 0 && m_gps_longitude != 0) {
        if (m_gps_latitude > 0)
            strcpy((char *)mExifInfo.gps_latitude_ref, "N");
        else
            strcpy((char *)mExifInfo.gps_latitude_ref, "S");

        if (m_gps_longitude > 0)
            strcpy((char *)mExifInfo.gps_longitude_ref, "E");
        else
            strcpy((char *)mExifInfo.gps_longitude_ref, "W");

        if (m_gps_altitude > 0)
            mExifInfo.gps_altitude_ref = 0;
        else
            mExifInfo.gps_altitude_ref = 1;

        double latitude = fabs(m_gps_latitude / 10000.0);
        double longitude = fabs(m_gps_longitude / 10000.0);
        double altitude = fabs(m_gps_altitude / 100.0);

        mExifInfo.gps_latitude[0].num = (uint32_t)latitude;
        mExifInfo.gps_latitude[0].den = 1;
        mExifInfo.gps_latitude[1].num = (uint32_t)((latitude - mExifInfo.gps_latitude[0].num) * 60);
        mExifInfo.gps_latitude[1].den = 1;
        mExifInfo.gps_latitude[2].num = (uint32_t)((((latitude - mExifInfo.gps_latitude[0].num) * 60)
                                        - mExifInfo.gps_latitude[1].num) * 60);
        mExifInfo.gps_latitude[2].den = 1;

        mExifInfo.gps_longitude[0].num = (uint32_t)longitude;
        mExifInfo.gps_longitude[0].den = 1;
        mExifInfo.gps_longitude[1].num = (uint32_t)((longitude - mExifInfo.gps_longitude[0].num) * 60);
        mExifInfo.gps_longitude[1].den = 1;
        mExifInfo.gps_longitude[2].num = (uint32_t)((((longitude - mExifInfo.gps_longitude[0].num) * 60)
                                        - mExifInfo.gps_longitude[1].num) * 60);
        mExifInfo.gps_longitude[2].den = 1;

        mExifInfo.gps_altitude.num = (uint32_t)altitude;
        mExifInfo.gps_altitude.den = 1;

        struct tm tm_data;
        gmtime_r(&m_gps_timestamp, &tm_data);
        mExifInfo.gps_timestamp[0].num = tm_data.tm_hour;
        mExifInfo.gps_timestamp[0].den = 1;
        mExifInfo.gps_timestamp[1].num = tm_data.tm_min;
        mExifInfo.gps_timestamp[1].den = 1;
        mExifInfo.gps_timestamp[2].num = tm_data.tm_sec;
        mExifInfo.gps_timestamp[2].den = 1;
        snprintf((char*)mExifInfo.gps_datestamp, sizeof(mExifInfo.gps_datestamp),
                "%04d:%02d:%02d", tm_data.tm_year + 1900, tm_data.tm_mon + 1, tm_data.tm_mday);

        mExifInfo.enableGps = true;
    } else {
        mExifInfo.enableGps = false;
    }

    //2 1th IFD TIFF Tags
    mExifInfo.widthThumb = m_jpeg_thumbnail_width;
    mExifInfo.heightThumb = m_jpeg_thumbnail_height;
}

// ======================================================================
// Conversions

inline int SecCamera::m_frameSize(int format, int width, int height)
{
    int size = 0;

    switch (format) {
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV21:
        size = (width * height * 3 / 2);
        break;

    case V4L2_PIX_FMT_NV12T:
        size = ALIGN_TO_8KB(ALIGN_TO_128B(width) * ALIGN_TO_32B(height)) +
                            ALIGN_TO_8KB(ALIGN_TO_128B(width) * ALIGN_TO_32B(height / 2));
        break;

    case V4L2_PIX_FMT_YUV422P:
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_JPEG:
        size = (width * height * 2);
        break;

    default :
        ALOGE("ERR(%s):Invalid V4L2 pixel format(%d)\n", __func__, format);
    case V4L2_PIX_FMT_RGB565:
        size = (width * height * BPP);
        break;
    }

    return size;
}

/*
status_t SecCamera::dump(int fd, const Vector<String16> &args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, 255, "dump(%d)\n", fd);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}
*/

double SecCamera::jpeg_ratio = 0.7;
int SecCamera::interleaveDataSize = 0x360000;
int SecCamera::jpegLineLength = 636;

}; // namespace android
