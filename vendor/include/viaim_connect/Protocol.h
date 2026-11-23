#pragma once

/**
 * @file Protocol.h
 * @brief 业务事件、媒体分片和回调定义。
 */

#include <viaim_connect/Commands.h>

/**
 * @defgroup viaim_connection 连接、日志与回调
 * @brief 配置日志、启动或停止设备连接，并接收异步数据。
 * @details
 * **核心函数、调用时机和调用效果**
 *
 * | 函数 | 调用时机 | 调用效果 |
 * | --- | --- | --- |
 * | viaim_connect_set_log_level() | create 后任意时刻 | 立即调整该实例的控制台日志级别 |
 * | viaim_connect_set_raw_log_flags() | create 后任意时刻 | 立即调整原始业务/媒体数据日志 |
 * | viaim_connect_set_heartbeat_flags() | create 后任意时刻 | 调整心跳日志和事件可见性，不影响自动应答 |
 * | viaim_connect_open() | 实例尚未打开时 | 复制选项和回调并启动异步连接流程 |
 * | viaim_connect_close() | 停止业务或销毁前 | 同步关闭摄像头和连接并排空回调 |
 * | viaim_connect_get_connection_state() | 需要状态快照时 | 返回调用瞬间的连接状态 |
 *
 * **前置条件：** 句柄有效；输入结构由对应初始化宏初始化且保留字段为零。
 *
 * **调用效果：** open 成功表示连接会话已启动，不等于已经连接；以状态回调为准。
 *
 * **生命周期：** 回调结构在 open 内复制，user_data 指向的对象须存活到 close 返回。
 *
 * **线程与回调：** 连接、事件和音频回调在连接分发线程串行执行；摄像头回调在
 * 独立的摄像头分发线程串行执行。close 不能从这两个回调线程调用，成功返回后不再
 * 产生新回调。
 *
 * 完整顺序参见 @ref api_lifecycle "实例完整生命周期" 和
 * @ref api_callbacks "回调与数据有效期"。
 * @{
 */

/** @brief HID 协议通道类型；枚举值与 HID Report ID 一致。 */
typedef int32_t ViaimConnectChannel;

enum {
    VIAIM_CONNECT_CHANNEL_BUSINESS = 0x06, /**< 业务命令通道。 */
    VIAIM_CONNECT_CHANNEL_AUDIO = 0x07,    /**< 音频媒体通道。 */
    VIAIM_CONNECT_CHANNEL_VIDEO = 0x08     /**< 视频媒体通道。 */
};

/** @brief ViaimConnectMediaPacket::codec 携带的已知音频编码类型。 */
typedef int32_t ViaimConnectAudioCodec;

enum {
    VIAIM_CONNECT_AUDIO_CODEC_OPUS = 0, /**< Opus 压缩音频。 */
    VIAIM_CONNECT_AUDIO_CODEC_PCM = 1,  /**< 未压缩 PCM 音频。 */
    VIAIM_CONNECT_AUDIO_CODEC_OGG = 2   /**< Ogg 容器数据。 */
};

/** @brief 媒体分片和 PCM 帧携带的已知音频来源。 */
typedef int32_t ViaimConnectAudioSource;

enum {
    VIAIM_CONNECT_AUDIO_SOURCE_SPEAKER = 0, /**< 扬声器侧音频。 */
    VIAIM_CONNECT_AUDIO_SOURCE_MICROPHONE = 1, /**< 麦克风侧音频。 */
    VIAIM_CONNECT_AUDIO_SOURCE_UNSPECIFIED = 2 /**< 协议未指定来源。 */
};

/** @brief SDK 输出的 PCM 样本表示方式。 */
typedef int32_t ViaimConnectPcmSampleFormat;

enum {
    VIAIM_CONNECT_PCM_S16_LE = 1 /**< 16 位有符号小端整数。 */
};

/** @brief 摄像头流状态类型；状态变化由 ViaimConnectCameraStateCallback 报告。 */
typedef int32_t ViaimConnectCameraState;

enum {
    VIAIM_CONNECT_CAMERA_CLOSED = 0,       /**< 没有打开的摄像头流。 */
    VIAIM_CONNECT_CAMERA_OPENING = 1,      /**< 正在同步打开所选模式。 */
    VIAIM_CONNECT_CAMERA_STREAMING = 2,    /**< 已打开并可交付 BGR24 帧。 */
    VIAIM_CONNECT_CAMERA_DISCONNECTED = 3, /**< 摄像头断开；不会自动重开。 */
    VIAIM_CONNECT_CAMERA_ERROR = 4         /**< 采集因错误停止；需 close 后重开。 */
};

/** @brief 在 ViaimConnect.h 中定义的借用摄像头帧。 */
typedef struct ViaimConnectCameraFrame ViaimConnectCameraFrame;

/**
 * @brief 业务事件中收到的一项原始 TLV 属性。
 *
 * SDK 保留设备帧中的属性顺序和重复项。value 只在当前事件回调期间有效。
 */
struct ViaimConnectRawAttribute {
    uint8_t id;              /**< TLV 属性 ID。 */
    ViaimConnectBytesView value; /**< TLV value 的只读字节视图。 */
};

/**
 * @brief 已解析的音频或视频分片视图。
 *
 * 此结构始终表示设备原始分片。音频完整帧的聚合/解码结果通过
 * ViaimConnectAudioPcmFrameCallback 单独交付。
 * data 只在当前媒体回调期间有效；需要异步处理时必须在回调返回前复制。时间顺序由
 * frame_sequence、packet_index 和 packet_count 表达，所有 size 的单位均为字节。
 */
typedef struct ViaimConnectMediaPacket {
    uint32_t struct_size; /**< 当前结构体大小，即 sizeof(ViaimConnectMediaPacket)。 */
    ViaimConnectChannel channel; /**< 音频或视频 HID 通道。 */
    uint8_t codec;       /**< 设备协议中的媒体编码类型值。 */
    uint8_t source;      /**< 设备协议中的媒体来源值。 */
    uint8_t speaker_id;  /**< 说话人 ID；协议未提供时为 0。 */
    uint32_t frame_sequence; /**< 媒体帧序号。 */
    uint16_t packet_count; /**< 当前媒体帧的分片总数。 */
    uint16_t packet_index; /**< 当前分片序号，取值由设备协议定义。 */
    const uint8_t* data; /**< 分片负载首地址，仅在回调期间有效。 */
    size_t size;         /**< 分片负载长度，单位为字节。 */
} ViaimConnectMediaPacket;

/**
 * @brief SDK 聚合、解码或透传得到的一帧完整 PCM 音频。
 * @details data 只在当前回调期间有效，需要保留时必须在返回前复制。当前输出为
 *          16000 Hz、单声道、16 bit、VIAIM_CONNECT_PCM_S16_LE；size 为总字节数，
 *          sample_count_per_channel 是每声道样本数。
 */
typedef struct ViaimConnectAudioPcmFrame {
    uint32_t struct_size; /**< 结构体大小，即 sizeof(ViaimConnectAudioPcmFrame)。 */
    ViaimConnectAudioSource source; /**< 设备协议报告的音频来源。 */
    uint8_t speaker_id; /**< 说话人 ID；协议未提供时为 0。 */
    uint32_t frame_sequence; /**< 源音频帧序号。 */
    uint32_t sample_rate; /**< 采样率，单位 Hz；当前为 16000。 */
    uint16_t channel_count; /**< 交错声道数；当前为 1。 */
    uint16_t bits_per_sample; /**< 每个样本的位数；当前为 16。 */
    ViaimConnectPcmSampleFormat sample_format; /**< 样本格式；当前为 VIAIM_CONNECT_PCM_S16_LE。 */
    uint32_t sample_count_per_channel; /**< data 中每个声道的样本数。 */
    const uint8_t* data; /**< 借用的 PCM 字节，仅在回调期间有效。 */
    size_t size; /**< data 总长度，单位字节。 */
} ViaimConnectAudioPcmFrame;

/**
 * @brief 连接状态变化回调。
 * @param[in] state 新连接状态；CONNECTED 后才可发送业务命令。
 * @param[in] user_data 原样传入的 ViaimConnectCallbacks::user_data。
 * @details 在该实例的串行回调分发线程触发。状态可能在回调返回后继续变化。
 */
typedef void(__cdecl* ViaimConnectConnectionStateCallback)(
    ViaimConnectConnectionState state,
    void* user_data);

/**
 * @brief 异步错误回调。
 * @param[in] error 对应的 SDK 错误码。
 * @param[in] message 以 NUL 结尾的 UTF-8 错误说明，仅在当前回调期间有效。
 * @param[in] user_data 原样传入的 ViaimConnectCallbacks::user_data。
 * @details 报告 open 返回后发生的连接、传输或媒体处理错误；需要保留 message 时
 *          必须在回调内复制。
 */
typedef void(__cdecl* ViaimConnectErrorCallback)(
    ViaimConnectResult error,
    const char* message,
    void* user_data);

/**
 * @brief 业务事件回调。
 *
 * 包括完整、部分解析和仅原始数据的事件。event 及其嵌套视图只在当前
 * 回调期间有效。
 *
 * @param[in] event 收到的业务事件。
 * @param[in] user_data 原样传入的 ViaimConnectCallbacks::user_data。
 * @details event、raw_attributes、所有字符串和字节视图只在当前回调期间有效。
 */
typedef void(__cdecl* ViaimConnectEventCallback)(
    const ViaimConnectEvent* event,
    void* user_data);

/**
 * @brief ViaimConnectCallbacks::audio_opus 使用的原始媒体分片回调。
 * @param[in] packet 收到的一个原始音频或视频协议分片，仅在当前回调期间有效；对于
 *                   Opus 音频，它不是聚合后的完整 Opus 帧。
 * @param[in] user_data 原样传入的 ViaimConnectCallbacks::user_data。
 */
typedef void(__cdecl* ViaimConnectMediaPacketCallback)(
    const ViaimConnectMediaPacket* packet,
    void* user_data);

/**
 * @brief 完整 PCM 音频帧回调。
 * @param[in] frame 非空 PCM 帧视图，仅在当前回调期间有效。
 * @param[in] user_data 原样传入的 ViaimConnectCallbacks::user_data。
 */
typedef void(__cdecl* ViaimConnectAudioPcmFrameCallback)(
    const ViaimConnectAudioPcmFrame* frame,
    void* user_data);

/**
 * @brief 摄像头流状态变化回调。
 * @param[in] state 新状态；DISCONNECTED 后 SDK 不自动重新打开摄像头。
 * @param[in] user_data 原样传入的 ViaimConnectCallbacks::user_data。
 * @details 在摄像头专用分发线程串行触发。
 */
typedef void(__cdecl* ViaimConnectCameraStateCallback)(
    ViaimConnectCameraState state,
    void* user_data);

/**
 * @brief 借用 BGR24 最新帧回调。
 * @param[in] frame 非空帧视图，仅在当前回调期间有效。
 * @param[in] user_data 原样传入的 ViaimConnectCallbacks::user_data。
 * @details 在摄像头专用分发线程串行触发；消费慢时旧帧可能被丢弃。
 */
typedef void(__cdecl* ViaimConnectCameraFrameCallback)(
    const ViaimConnectCameraFrame* frame,
    void* user_data);

/**
 * @brief SDK 实例的用户回调集合。
 *
 * viaim_connect_open() 在返回前复制本结构，每个回调均可为 NULL；NULL 表示不接收
 * 对应通知。连接、事件和音频回调在连接分发线程串行执行；摄像头回调在独立的
 * 摄像头分发线程串行执行。
 *
 * 回调参数中的事件、原始属性、字符串、字节、媒体分片和 PCM 数据只在当前回调期间
 * 有效，跨回调使用必须立即复制。回调中允许调用 viaim_connect_send() 和只读查询；
 * 同步调用 viaim_connect_close() 或 viaim_connect_destroy() 会返回
 * VIAIM_CONNECT_ERROR_INVALID_STATE。user_data 由调用方拥有，并须保持有效直到 close 返回。
 */
typedef struct ViaimConnectCallbacks {
    uint32_t struct_size; /**< 当前结构体大小，即 sizeof(ViaimConnectCallbacks)。 */
    ViaimConnectConnectionStateCallback connection_state; /**< 连接状态变化回调，可为 NULL。 */
    ViaimConnectErrorCallback error; /**< 异步错误回调，可为 NULL。 */
    ViaimConnectEventCallback event; /**< 业务事件回调，可为 NULL。 */
    ViaimConnectMediaPacketCallback audio_opus; /**< 原始媒体协议分片回调，可为 NULL。 */
    void* user_data; /**< SDK 不解释的调用方上下文，原样传给所有回调。 */
    uint64_t reserved[4]; /**< 保留字段，必须全部为 0。 */
    ViaimConnectAudioPcmFrameCallback audio_pcm; /**< 完整 PCM 音频帧回调，可为 NULL。 */
    ViaimConnectCameraStateCallback camera_state; /**< 摄像头状态回调，可为 NULL。 */
    ViaimConnectCameraFrameCallback camera_frame; /**< 摄像头最新帧回调，可为 NULL。 */
} ViaimConnectCallbacks;

/**
 * @brief ViaimConnectCallbacks 默认初始化值。
 * @details 设置正确 struct_size，将全部回调、user_data 和保留字段置零；按需覆盖回调
 *          后传给 viaim_connect_open()，也可以直接向 open 传 NULL 表示不注册回调。
 */
#define VIAIM_CONNECT_CALLBACKS_INIT                                        \
    {                                                                    \
        sizeof(ViaimConnectCallbacks), NULL, NULL, NULL, NULL, NULL, {0},    \
        NULL, NULL, NULL                                                 \
    }

/** @} */
