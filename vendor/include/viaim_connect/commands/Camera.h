#pragma once

/**
 * @file Camera.h
 * @brief 相机场景、视角、分辨率、画质和镜头控制命令。
 */

#include <viaim_connect/Types.h>

/**
 * @addtogroup viaim_commands
 * @details 相机业务命令（区别于 MJPEG 采集 API）的操作规则：MEETING_SCENE 接受
 *          SET/QUERY；VIEW_ANGLE、CAMERA_RESET 接受 EXECUTE；成对的 SET_/QUERY_
 *          命令分别只接受 SET/QUERY；QUERY_LENS_COVER 只接受 QUERY。QUERY 的
 *          present_fields 必须为 0，SET/EXECUTE 必须只设置对应负载的字段位。
 * @{
 */

enum {
    VIAIM_CONNECT_COMMAND_MEETING_SCENE = 0x0023, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_VIEW_ANGLE = 0x0024, /**< 可发送 EXECUTE；响应可接收。 */
    VIAIM_CONNECT_COMMAND_SET_RESOLUTION = 0x0027, /**< 可发送 SET；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_RESOLUTION = 0x0028, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_SET_CAMERA = 0x0029, /**< 可发送 SET；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_CAMERA = 0x002A, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_SET_PICTURE = 0x002B, /**< 可发送 SET；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_PICTURE = 0x002C, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_LENS_COVER = 0x003E, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_CAMERA_RESET = 0x004B /**< 可发送 EXECUTE；响应可接收。 */
};

/** @brief 会议场景负载。 */
typedef struct ViaimConnectMeetingSceneData {
    uint8_t scene; /**< 会议场景协议值；SDK 接受 uint8_t 全宽。 */
} ViaimConnectMeetingSceneData;

enum {
    VIAIM_CONNECT_MEETING_SCENE_FIELD_SCENE = UINT64_C(1) << 0 /**< payload.meeting_scene.scene 有效。 */
};

/** @brief 一次性视角调节动作负载。 */
typedef struct ViaimConnectViewAngleData {
    uint8_t action; /**< 视角动作协议值；SDK 接受 uint8_t 全宽。 */
} ViaimConnectViewAngleData;

enum {
    VIAIM_CONNECT_VIEW_ANGLE_FIELD_ACTION = UINT64_C(1) << 0 /**< payload.view_angle.action 有效。 */
};

/** @brief 视频分辨率和帧率负载。 */
typedef struct ViaimConnectResolutionData {
    uint8_t resolution; /**< 设备协议中的分辨率档位，不是像素宽度。 */
    uint8_t frame_rate; /**< 设备协议中的帧率档位，不是直接的 fps 数值。 */
} ViaimConnectResolutionData;

enum {
    VIAIM_CONNECT_RESOLUTION_FIELD_RESOLUTION = UINT64_C(1) << 0, /**< payload.resolution.resolution 有效。 */
    VIAIM_CONNECT_RESOLUTION_FIELD_FRAME_RATE = UINT64_C(1) << 1 /**< payload.resolution.frame_rate 有效。 */
};

/** @brief 相机曝光、白平衡和画质参数负载。 */
typedef struct ViaimConnectCameraData {
    uint8_t automatic_exposure; /**< 自动曝光开关：0 关闭，1 开启。 */
    uint8_t exposure; /**< 手动曝光档位。 */
    uint8_t automatic_white_balance; /**< 自动白平衡开关：0 关闭，1 开启。 */
    uint16_t white_balance; /**< 手动白平衡值。 */
    uint8_t contrast; /**< 对比度值。 */
    uint8_t saturation; /**< 饱和度值。 */
    uint8_t sharpness; /**< 锐度值。 */
    uint8_t hdr_enabled; /**< HDR 开关：0 关闭，1 开启。 */
    uint8_t anti_flicker_enabled; /**< 防闪烁开关：0 关闭，1 开启。 */
    uint8_t brightness; /**< 亮度值。 */
} ViaimConnectCameraData;

enum {
    VIAIM_CONNECT_CAMERA_FIELD_AUTOMATIC_EXPOSURE = UINT64_C(1) << 0, /**< automatic_exposure 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_EXPOSURE = UINT64_C(1) << 1, /**< exposure 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_AUTOMATIC_WHITE_BALANCE = UINT64_C(1) << 2, /**< automatic_white_balance 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_WHITE_BALANCE = UINT64_C(1) << 3, /**< white_balance 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_CONTRAST = UINT64_C(1) << 4, /**< contrast 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_SATURATION = UINT64_C(1) << 5, /**< saturation 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_SHARPNESS = UINT64_C(1) << 6, /**< sharpness 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_HDR = UINT64_C(1) << 7, /**< hdr_enabled 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_ANTI_FLICKER = UINT64_C(1) << 8, /**< anti_flicker_enabled 有效。 */
    VIAIM_CONNECT_CAMERA_FIELD_BRIGHTNESS = UINT64_C(1) << 9 /**< brightness 有效。 */
};

/** @brief 画面变换与自动校正参数负载。 */
typedef struct ViaimConnectPictureData {
    uint8_t horizontal_mirror; /**< 水平镜像开关：0 关闭，1 开启。 */
    uint8_t vertical_mirror; /**< 垂直镜像开关：0 关闭，1 开启。 */
    uint8_t smart_adjustment; /**< 智能调节开关：0 关闭，1 开启。 */
    uint8_t horizontal_correction; /**< 水平校正开关：0 关闭，1 开启。 */
    uint8_t low_resolution; /**< 低分辨率模式开关：0 关闭，1 开启。 */
    uint8_t reset; /**< 画面参数复位请求值。 */
} ViaimConnectPictureData;

enum {
    VIAIM_CONNECT_PICTURE_FIELD_HORIZONTAL_MIRROR = UINT64_C(1) << 0, /**< horizontal_mirror 有效。 */
    VIAIM_CONNECT_PICTURE_FIELD_VERTICAL_MIRROR = UINT64_C(1) << 1, /**< vertical_mirror 有效。 */
    VIAIM_CONNECT_PICTURE_FIELD_SMART_ADJUSTMENT = UINT64_C(1) << 2, /**< smart_adjustment 有效。 */
    VIAIM_CONNECT_PICTURE_FIELD_HORIZONTAL_CORRECTION = UINT64_C(1) << 3, /**< horizontal_correction 有效。 */
    VIAIM_CONNECT_PICTURE_FIELD_LOW_RESOLUTION = UINT64_C(1) << 4, /**< low_resolution 有效。 */
    VIAIM_CONNECT_PICTURE_FIELD_RESET = UINT64_C(1) << 5 /**< reset 有效。 */
};

/** @brief 镜头盖状态负载。 */
typedef struct ViaimConnectLensCoverData {
    uint8_t open; /**< 镜头盖状态：0 关闭，1 打开。 */
} ViaimConnectLensCoverData;

enum {
    VIAIM_CONNECT_LENS_COVER_FIELD_OPEN = UINT64_C(1) << 0 /**< payload.lens_cover.open 有效。 */
};

/** @brief 相机参数复位负载。 */
typedef struct ViaimConnectCameraResetData {
    uint8_t target; /**< 要复位的参数组协议值；SDK 接受 uint8_t 全宽。 */
} ViaimConnectCameraResetData;

enum {
    VIAIM_CONNECT_CAMERA_RESET_FIELD_TARGET = UINT64_C(1) << 0 /**< payload.camera_reset.target 有效。 */
};

/** @} */
