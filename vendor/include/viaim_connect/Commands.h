#pragma once

/**
 * @file Commands.h
 * @brief 统一命令信封、业务域负载和事件释义 API。
 */

#include <viaim_connect/Types.h>
#include <viaim_connect/commands/Audio.h>
#include <viaim_connect/commands/Camera.h>
#include <viaim_connect/commands/Device.h>
#include <viaim_connect/commands/Interaction.h>
#include <viaim_connect/commands/System.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @defgroup viaim_commands 业务命令与事件释义
 * @brief 构造并发送强类型命令，接收事件并生成调用方可读释义。
 * @details
 * **核心函数、调用时机和调用效果**
 *
 * | 函数 | 调用时机 | 调用效果 |
 * | --- | --- | --- |
 * | viaim_connect_send() | 连接状态为 CONNECTED 时 | 校验并复制请求，将编码结果排入传输队列 |
 * | viaim_connect_describe_event() | 事件回调期间 | 按实例创建时冻结的配置生成事件名称和含义 |
 * | viaim_connect_describe_event_attribute() | 事件回调期间 | 解释指定的零基原始 TLV 属性 |
 *
 * **前置条件：** 请求和输出结构使用初始化宏；命令 ID、operation、present_fields 和
 * payload 成员必须相互匹配。
 *
 * **调用效果：** send 的 OK 仅表示 SDK 已接受请求；设备业务结果由事件中的
 * protocol_result 报告。
 *
 * **生命周期：** send 在返回前复制输入视图；事件和释义输出是借用数据，按函数页面
 * 规定的回调或线程局部有效期使用。
 *
 * **线程与回调：** send 可从事件回调调用；释义函数是只读调用，但需要在下一次释义
 * 调用或回调返回前复制要保留的文本。
 *
 * 完整顺序参见 @ref api_commands "发送命令与处理事件"。
 * @{
 */

/** @brief Protocol.h 中定义的有序原始 TLV 属性。 */
typedef struct ViaimConnectRawAttribute ViaimConnectRawAttribute;

/**
 * @brief 请求或事件携带的通用操作类型。
 * @details 调用方发送 SET、QUERY 或 EXECUTE；SDK 接收设备数据后填入 RESPONSE 或
 *          REPORT。每个命令 ID 支持的发送操作见 commands 子目录中的对应头文件说明。
 */
typedef int32_t ViaimConnectOperation;

enum {
    VIAIM_CONNECT_OPERATION_SET = 0,      /**< 设置一个或多个属性。 */
    VIAIM_CONNECT_OPERATION_QUERY = 1,    /**< 查询一个或多个属性。 */
    VIAIM_CONNECT_OPERATION_EXECUTE = 2,  /**< 执行一次性动作。 */
    VIAIM_CONNECT_OPERATION_RESPONSE = 3, /**< 设备对请求的响应。 */
    VIAIM_CONNECT_OPERATION_REPORT = 4    /**< 设备主动上报。 */
};

/** @brief 收到的业务命令实体解析状态；无论状态如何，raw_attributes 都保留原始信息。 */
typedef int32_t ViaimConnectDecodeStatus;

enum {
    VIAIM_CONNECT_DECODE_OK = 0, /**< 所有收到的属性均已完整解析。 */
    VIAIM_CONNECT_DECODE_OK_WITH_UNKNOWN = 1, /**< 已知属性解析成功，同时存在未知属性。 */
    VIAIM_CONNECT_DECODE_PARTIAL = 2, /**< 部分已知属性因长度、重复或结构问题未能解析。 */
    VIAIM_CONNECT_DECODE_RAW_ONLY = 3 /**< 未知命令，仅提供 raw_attributes。 */
};

enum {
    VIAIM_CONNECT_COMMAND_AUDIO = 0x0900, /**< 音频媒体帧标识，不用于 viaim_connect_send()。 */
    VIAIM_CONNECT_COMMAND_VIDEO = 0x0901  /**< 视频媒体帧标识，不用于 viaim_connect_send()。 */
};

/** @brief ViaimConnectCommandPayload 的固定 ABI 容量，单位字节；调用方不得修改。 */
#define VIAIM_CONNECT_COMMAND_PAYLOAD_CAPACITY 128u

/**
 * @brief 由命令 ID 决定有效成员的定长业务负载。
 *
 * 调用方根据 ViaimConnectCommand::id 访问对应命名成员，不能读取其他成员。
 * reserved 固定联合体 ABI 大小；新增命名成员不得超过
 * VIAIM_CONNECT_COMMAND_PAYLOAD_CAPACITY。
 */
typedef union ViaimConnectCommandPayload {
    ViaimConnectMacData mac; /**< MAC 地址负载。 */
    ViaimConnectKeyData key; /**< 按键上报负载。 */
    ViaimConnectBvctData bvct; /**< 电池、电压、电流和温度负载。 */
    ViaimConnectSoftwareVersionData software_version; /**< 软件版本负载。 */
    ViaimConnectVolumeData volume; /**< 音量负载。 */
    ViaimConnectStringData bluetooth_name; /**< 蓝牙名称：发送时须为 1..24 字节有效 UTF-8。 */
    ViaimConnectHardwareVersionData hardware_version; /**< 硬件版本负载。 */
    ViaimConnectStringData serial_number; /**< 设备序列号负载。 */
    ViaimConnectUtcData utc; /**< UTC 时间负载。 */
    ViaimConnectFunctionModeData function_mode; /**< 功能模式负载。 */
    ViaimConnectMeetingSceneData meeting_scene; /**< 会议场景负载。 */
    ViaimConnectViewAngleData view_angle; /**< 视角动作负载。 */
    ViaimConnectResolutionData resolution; /**< 视频分辨率负载。 */
    ViaimConnectCameraData camera; /**< 相机参数负载。 */
    ViaimConnectPictureData picture; /**< 画面参数负载。 */
    ViaimConnectSwitchData wakeup; /**< 语音唤醒开关负载。 */
    ViaimConnectStorageData storage; /**< 存储容量负载。 */
    ViaimConnectOtaData ota; /**< OTA 状态负载。 */
    ViaimConnectGestureData gesture; /**< 手势识别负载。 */
    ViaimConnectSwitchData mute; /**< 静音开关负载。 */
    ViaimConnectSwitchData touchpad_light; /**< 触控板灯开关负载。 */
    ViaimConnectVoiceprintRegistrationData voiceprint_registration; /**< 声纹注册负载。 */
    ViaimConnectVoiceprintDeleteData voiceprint_delete; /**< 声纹删除负载。 */
    ViaimConnectVoiceprintListData voiceprint_list; /**< 声纹列表负载。 */
    ViaimConnectLensCoverData lens_cover; /**< 镜头盖状态负载。 */
    ViaimConnectWakeupLanguageData wakeup_language; /**< 唤醒语言负载。 */
    ViaimConnectBatteryStatusData battery_status; /**< 电池状态上报负载。 */
    ViaimConnectSwitchData prompt_tone; /**< 提示音开关负载。 */
    ViaimConnectSwitchData ring_light; /**< 环形灯开关负载。 */
    ViaimConnectHandshakeData handshake; /**< 握手状态快照负载。 */
    ViaimConnectSwitchData voiceprint_switch; /**< 声纹开关负载。 */
    ViaimConnectScheduledShutdownData scheduled_shutdown; /**< 定时关机负载。 */
    ViaimConnectFlashData flash; /**< 闪录状态上报负载。 */
    ViaimConnectHeartbeatData heartbeat; /**< 心跳负载。 */
    ViaimConnectCameraResetData camera_reset; /**< 相机复位负载。 */
    ViaimConnectDurationData flash_duration; /**< 闪录时长负载。 */
    ViaimConnectDurationData flash_silent_exit; /**< 闪录静默退出时长负载。 */
    ViaimConnectSystemPowerData system_power; /**< 系统电源动作负载。 */
    uint8_t reserved[VIAIM_CONNECT_COMMAND_PAYLOAD_CAPACITY]; /**< 固定 ABI 容量的保留存储。 */
} ViaimConnectCommandPayload;

#if !defined(DOXYGEN_SHOULD_SKIP_THIS)
#if defined(__cplusplus)
#  define VIAIM_CONNECT_STATIC_ASSERT(condition, message) \
      static_assert(condition, message)
#else
#  define VIAIM_CONNECT_JOIN_TOKENS_IMPL(left, right) left##right
#  define VIAIM_CONNECT_JOIN_TOKENS(left, right) \
      VIAIM_CONNECT_JOIN_TOKENS_IMPL(left, right)
#  define VIAIM_CONNECT_STATIC_ASSERT(condition, message) \
      typedef char VIAIM_CONNECT_JOIN_TOKENS(             \
          viaim_connect_static_assert_at_line_, __LINE__)[(condition) ? 1 : -1]
#endif

#define VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(type)                              \
    VIAIM_CONNECT_STATIC_ASSERT(                                             \
        sizeof(type) <= VIAIM_CONNECT_COMMAND_PAYLOAD_CAPACITY,              \
        #type " exceeds ViaimConnectCommandPayload capacity")

VIAIM_CONNECT_STATIC_ASSERT(
    sizeof(ViaimConnectCommandPayload) ==
        VIAIM_CONNECT_COMMAND_PAYLOAD_CAPACITY,
    "ViaimConnectCommandPayload ABI size changed");
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectMacData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectKeyData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectBvctData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectSoftwareVersionData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectVolumeData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectStringData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectHardwareVersionData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectUtcData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectFunctionModeData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectMeetingSceneData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectViewAngleData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectResolutionData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectCameraData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectPictureData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectSwitchData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectStorageData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectOtaData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectGestureData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectVoiceprintRegistrationData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectVoiceprintDeleteData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectVoiceprintListData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectLensCoverData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectWakeupLanguageData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectBatteryStatusData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectHandshakeData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectScheduledShutdownData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectFlashData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectHeartbeatData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectCameraResetData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectDurationData);
VIAIM_CONNECT_ASSERT_PAYLOAD_FITS(ViaimConnectSystemPowerData);

#undef VIAIM_CONNECT_ASSERT_PAYLOAD_FITS
#undef VIAIM_CONNECT_STATIC_ASSERT
#if !defined(__cplusplus)
#  undef VIAIM_CONNECT_JOIN_TOKENS
#  undef VIAIM_CONNECT_JOIN_TOKENS_IMPL
#endif
#endif

/**
 * @brief 请求和事件共用的命令实体。
 *
 * 使用 VIAIM_CONNECT_COMMAND_INIT 初始化。present_fields 中置位的字段才有效；
 * 位定义位于对应的 commands/ 业务域头文件中。
 */
typedef struct ViaimConnectCommand {
    uint32_t struct_size; /**< 当前结构体大小，即 sizeof(ViaimConnectCommand)。 */
    uint16_t id; /**< VIAIM_CONNECT_COMMAND_* 命令 ID。 */
    ViaimConnectOperation operation; /**< 命令操作；支持范围由具体命令定义。 */
    uint64_t present_fields; /**< payload 中有效字段的 VIAIM_CONNECT_*_FIELD_* 位集合。 */
    ViaimConnectCommandPayload payload; /**< 与 id 对应的命名负载成员。 */
    uint64_t reserved[4]; /**< 保留字段，必须全部为 0。 */
} ViaimConnectCommand;

/** @brief 发送给 Connect 的强类型业务请求。 */
typedef struct ViaimConnectRequest {
    uint32_t struct_size; /**< 当前结构体大小，即 sizeof(ViaimConnectRequest)。 */
    ViaimConnectCommand command; /**< 要发送的命令实体。 */
    uint64_t reserved[4]; /**< 保留字段，必须全部为 0。 */
} ViaimConnectRequest;

/**
 * @brief 从 Connect 收到的强类型业务事件。
 *
 * event、raw_attributes 及其嵌套视图均只在当前事件回调期间有效。raw_attributes 按
 * 设备帧中的到达顺序保存全部 TLV，包括未知属性和重复属性；需要保留时在回调内复制。
 */
typedef struct ViaimConnectEvent {
    uint32_t struct_size; /**< SDK 写入的结构体大小。 */
    uint8_t protocol_result; /**< 设备返回的业务结果；0 表示设备接受或成功。 */
    ViaimConnectDecodeStatus decode_status; /**< SDK 对业务负载的解析状态。 */
    ViaimConnectCommand command; /**< 已解析的命令实体。 */
    const ViaimConnectRawAttribute* raw_attributes; /**< 原始 TLV 属性数组，仅在回调期间有效。 */
    size_t raw_attribute_count; /**< raw_attributes 中的元素数量。 */
    uint64_t reserved[4]; /**< 保留字段。 */
} ViaimConnectEvent;

/**
 * @brief 已知事件的配置化 UTF-8 释义。
 *
 * name 借用实例冻结的只读释义配置，实例销毁后失效；meaning 使用线程局部复用
 * 缓冲区，同一线程下一次事件释义调用时失效。两者均不保证 NUL 结尾；若要在当前
 * 回调之后使用，应按对应 size 立即复制。
 */
typedef struct ViaimConnectDescription {
    uint32_t struct_size; /**< 调用方提供的输出结构体容量。 */
    const char* name; /**< UTF-8 命令名称，不保证以 '\0' 结尾。 */
    size_t name_size; /**< name 长度，单位为字节。 */
    const char* meaning; /**< UTF-8 事件释义，不保证以 '\0' 结尾。 */
    size_t meaning_size; /**< meaning 长度，单位为字节。 */
    uint64_t reserved[4]; /**< 保留字段。 */
} ViaimConnectDescription;

/**
 * @brief 单个事件属性的借用数据和 UTF-8 释义。
 *
 * raw_value 借用事件内存，只在当前事件回调期间有效。meaning 使用线程
 * 局部复用缓冲区，在同一线程下一次属性释义调用或当前回调返回后失效。
 */
typedef struct ViaimConnectAttributeDescription {
    uint32_t struct_size; /**< 调用方提供的输出结构体容量。 */
    uint8_t attribute_id; /**< 被释义的原始 TLV 属性 ID。 */
    ViaimConnectBytesView raw_value; /**< 被释义属性的原始 value 视图。 */
    ViaimConnectStringView meaning; /**< 格式化后的 UTF-8 属性含义。 */
    uint64_t reserved[4]; /**< 保留字段。 */
} ViaimConnectAttributeDescription;

/**
 * @brief ViaimConnectCommand 默认初始化值。
 * @details 设置结构版本、QUERY 默认操作、空字段集合及零保留区；通常由
 *          VIAIM_CONNECT_REQUEST_INIT 间接使用，再覆盖 id、operation、
 *          present_fields 和匹配的 payload 成员。
 */
#define VIAIM_CONNECT_COMMAND_INIT                                           \
    {                                                                     \
        sizeof(ViaimConnectCommand), 0, VIAIM_CONNECT_OPERATION_QUERY, 0,       \
            {0}, {0}                                                      \
    }

/**
 * @brief ViaimConnectRequest 默认初始化值。
 * @details 构造可传给 viaim_connect_send() 的空请求；发送前必须填写受支持的命令。
 */
#define VIAIM_CONNECT_REQUEST_INIT                                           \
    {                                                                     \
        sizeof(ViaimConnectRequest), VIAIM_CONNECT_COMMAND_INIT, {0}            \
    }

/** @brief ViaimConnectEvent 默认初始化值，供测试或调用方自建兼容缓冲区使用。 */
#define VIAIM_CONNECT_EVENT_INIT                                             \
    {                                                                     \
        sizeof(ViaimConnectEvent), 0, VIAIM_CONNECT_DECODE_OK,                  \
            VIAIM_CONNECT_COMMAND_INIT, NULL, 0, {0}                         \
    }

/** @brief 传给 viaim_connect_describe_event() 的输出缓冲区初始化值。 */
#define VIAIM_CONNECT_DESCRIPTION_INIT                                       \
    {                                                                     \
        sizeof(ViaimConnectDescription), NULL, 0, NULL, 0, {0}               \
    }

/** @brief 传给 viaim_connect_describe_event_attribute() 的输出缓冲区初始化值。 */
#define VIAIM_CONNECT_ATTRIBUTE_DESCRIPTION_INIT                             \
    {                                                                     \
        sizeof(ViaimConnectAttributeDescription), 0, {NULL, 0},              \
            {NULL, 0}, {0}                                                \
    }

/**
 * @brief 校验、编码并通过指定 SDK 实例异步发送一个业务请求。
 *
 * id、operation、present_fields 与命名 payload 成员必须符合对应业务域头文件的规则。
 * SDK 在返回前完成编码并复制所有字符串/字节视图。OK 表示帧已被活动会话接受，不代表
 * Connect 已执行业务；设备结果随后通过 ViaimConnectEvent::protocol_result 报告。
 *
 * @param[in] sdk 有效 SDK 实例。
 * @param[in] request 使用 VIAIM_CONNECT_REQUEST_INIT 初始化并填写的请求。
 * @pre 连接状态为 VIAIM_CONNECT_CONNECTION_CONNECTED，且 request 的结构版本、保留
 *      字段及命令组合有效。
 * @post 返回后调用方可立即修改或释放 request 及其引用的可变长度缓冲区。
 * @retval VIAIM_CONNECT_OK 请求已编码并交给传输队列。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk/request 为空、命令不支持，或
 *         id、operation、present_fields、payload/视图组合无效。
 * @retval VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION request 或内嵌 command 结构版本不兼容。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 未 open、会话正在停止或当前不能接受发送。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 编码或 C ABI 边界捕获到内部异常。
 * @par 生命周期
 * 输入只借用到本函数返回；设备响应以独立的借用 ViaimConnectEvent 交付。
 * @par 线程与回调
 * 可从事件回调或其他线程调用；函数不等待设备响应。
 * @par 示例
 * @code{.c}
 * ViaimConnectRequest request = VIAIM_CONNECT_REQUEST_INIT;
 * request.command.id = VIAIM_CONNECT_COMMAND_SET_VOLUME;
 * request.command.operation = VIAIM_CONNECT_OPERATION_SET;
 * request.command.present_fields = VIAIM_CONNECT_VOLUME_FIELD_PC;
 * request.command.payload.volume.pc = 50;
 * ViaimConnectResult queued = viaim_connect_send(sdk, &request);
 * // queued == OK 只表示已排队；在事件回调中另查 protocol_result。
 * @endcode
 * @see VIAIM_CONNECT_REQUEST_INIT
 * @see ViaimConnectEventCallback
 * @ingroup viaim_commands
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_send(
    ViaimConnectSdk* sdk,
    const ViaimConnectRequest* request);

/**
 * @brief 使用实例创建时冻结的配置生成整个事件的 UTF-8 名称和含义。
 * @param[in] sdk 产生 event 的有效 SDK 实例。
 * @param[in] event 当前事件回调中的非空业务事件。
 * @param[out] description 使用 VIAIM_CONNECT_DESCRIPTION_INIT 初始化的输出结构。结构
 *                         版本兼容时，本函数先将其重置为空，再校验 event。
 * @pre event 及其 raw_attributes/字节视图仍处于有效期内。
 * @post 成功时 name 借用 sdk 的只读配置，meaning 借用当前线程的复用缓冲区；均不保证
 *       NUL 结尾。失败时兼容的输出结构保持为空。
 * @retval VIAIM_CONNECT_OK 找到规则并生成释义。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk、event、description 或原始属性视图无效。
 * @retval VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION event、command 或输出结构版本不兼容。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 实例没有可用的冻结释义注册表。
 * @retval VIAIM_CONNECT_ERROR_NOT_FOUND 配置中没有该方向/命令/结果的释义规则。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 格式化或 C ABI 边界捕获到内部异常。
 * @par 生命周期
 * name 最迟在 sdk 销毁时失效；meaning 在同一线程下一次 describe_event 调用时失效。
 * 为跨回调保留文本，应在当前回调中按 name_size/meaning_size 复制。
 * @par 线程与回调
 * 只读调用，可在事件回调中使用；不同线程拥有各自的 meaning 缓冲区。
 * @par 示例
 * @code{.c}
 * static void __cdecl describe_in_callback(
 *     const ViaimConnectEvent* event, void* user_data) {
 *     ViaimConnectSdk* sdk = (ViaimConnectSdk*)user_data;
 *     ViaimConnectDescription text = VIAIM_CONNECT_DESCRIPTION_INIT;
 *     if (viaim_connect_describe_event(sdk, event, &text) == VIAIM_CONNECT_OK) {
 *         // 在下一次释义调用前复制 text.meaning[0..meaning_size)。
 *     }
 * }
 * @endcode
 * @see VIAIM_CONNECT_DESCRIPTION_INIT
 * @see viaim_connect_describe_event_attribute()
 * @ingroup viaim_commands
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_describe_event(
    ViaimConnectSdk* sdk,
    const ViaimConnectEvent* event,
    ViaimConnectDescription* description);

/**
 * @brief 解释事件中按到达顺序排列的一项原始 TLV 属性。
 * @param[in] sdk 产生 event 的有效 SDK 实例。
 * @param[in] event 当前事件回调中的非空业务事件。
 * @param[in] attribute_index raw_attributes 中从 0 开始的索引；重复属性各占一个索引。
 * @param[out] out_description 使用 VIAIM_CONNECT_ATTRIBUTE_DESCRIPTION_INIT 初始化的
 *                             输出结构；版本兼容时先被重置为空。
 * @pre attribute_index 小于 event->raw_attribute_count，且事件及视图仍在回调有效期内。
 * @post 成功时 raw_value 借用 event 内存，meaning 借用当前线程的复用缓冲区；失败时
 *       兼容的输出结构保持为空。
 * @retval VIAIM_CONNECT_OK 找到规则并生成属性释义。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk/event/out_description、索引或视图无效。
 * @retval VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION event、command 或输出结构版本不兼容。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 实例没有可用的冻结释义注册表。
 * @retval VIAIM_CONNECT_ERROR_NOT_FOUND 配置中没有该命令/属性/值的释义规则。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 格式化或 C ABI 边界捕获到内部异常。
 * @par 生命周期
 * raw_value 在当前事件回调返回时失效；meaning 在同一线程下一次属性释义调用时失效。
 * @par 线程与回调
 * 只读调用，可在事件回调中逐项遍历；跨线程使用前必须复制返回的字节和文本。
 * @par 示例
 * @code{.c}
 * for (size_t i = 0; i < event->raw_attribute_count; ++i) {
 *     ViaimConnectAttributeDescription item =
 *         VIAIM_CONNECT_ATTRIBUTE_DESCRIPTION_INIT;
 *     if (viaim_connect_describe_event_attribute(sdk, event, i, &item) ==
 *         VIAIM_CONNECT_OK) {
 *         // 立即复制 item.raw_value 和 item.meaning；下一次调用会复用 meaning。
 *     }
 * }
 * @endcode
 * @see VIAIM_CONNECT_ATTRIBUTE_DESCRIPTION_INIT
 * @see viaim_connect_describe_event()
 * @ingroup viaim_commands
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_describe_event_attribute(
    ViaimConnectSdk* sdk,
    const ViaimConnectEvent* event,
    size_t attribute_index,
    ViaimConnectAttributeDescription* out_description);

/** @} */
