#pragma once

/**
 * @file Types.h
 * @brief viaim_connect_sdk 公共基础类型、返回码及连接选项。
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @defgroup viaim_sdk_lifecycle SDK 基础与实例生命周期
 * @brief 创建、配置和销毁相互独立的 SDK 实例。
 * @details
 * **核心函数、调用时机和调用效果**
 *
 * | 函数 | 调用时机 | 调用效果 |
 * | --- | --- | --- |
 * | viaim_connect_create() | 任何其他实例 API 之前 | 加载释义配置、初始化后端并返回独占句柄 |
 * | viaim_connect_destroy() | 所有业务结束后 | 同步关闭摄像头和连接并释放实例 |
 *
 * **前置条件：** 调用方使用对应初始化宏，并保持所有保留字段为零。
 *
 * **调用效果：** 每次成功 create 都产生一个拥有独立配置、连接和回调的实例。
 *
 * **生命周期：** create 成功得到的句柄必须且只能交给 destroy 一次。
 *
 * **线程与回调：** 普通查询可并发调用；destroy 不能从 SDK 或摄像头回调线程调用。
 *
 * 完整顺序参见 @ref api_lifecycle "实例完整生命周期"。
 * @{
 */

/** @brief 当前公共 C ABI 主版本号；主版本变化表示存在 ABI 不兼容修改。 */
#define VIAIM_CONNECT_ABI_VERSION_MAJOR 4u
/** @brief 当前公共 C ABI 次版本号；同一主版本内只追加兼容能力。 */
#define VIAIM_CONNECT_ABI_VERSION_MINOR 0u
/** @brief 当前公共 C ABI 修订版本号；表示不改变 ABI 的修订。 */
#define VIAIM_CONNECT_ABI_VERSION_PATCH 0u
/** @brief 编码为 0xMMMMmmpp 的当前公共 C ABI 版本，可用于编译期兼容判断。 */
#define VIAIM_CONNECT_ABI_VERSION 0x00040000u

/**
 * @brief 不透明 SDK 实例句柄；每个实例最多管理一台 Connect 设备。
 * @details 句柄由 viaim_connect_create() 创建、由 viaim_connect_destroy() 独占销毁，
 *          调用方不得复制其底层对象或在销毁后继续使用指针值。
 */
typedef struct ViaimConnectSdk ViaimConnectSdk;

/**
 * @brief 只读 UTF-8 字符串视图。
 *
 * data 指向的内容不保证以 '\0' 结尾；size 的单位是字节而不是 Unicode 字符数。
 * 当 size 为 0 时 data 可以为 NULL；size 非零时 data 必须非 NULL。该类型本身不拥有
 * 内存，具体有效期由产生或消费该视图的 API 说明。
 */
typedef struct ViaimConnectStringView {
    const char* data; /**< UTF-8 数据首地址；size 为 0 时允许为 NULL。 */
    size_t size;      /**< UTF-8 数据长度，单位为字节，不包含终止符。 */
} ViaimConnectStringView;

/**
 * @brief 不拥有内存的只读字节视图。
 * @details size 为字节数；size 为 0 时 data 可以为 NULL，size 非零时 data 必须
 *          非 NULL。作为请求输入时由发送 API 在返回前复制；作为回调输出时只在
 *          对应回调期间有效。
 */
typedef struct ViaimConnectBytesView {
    const uint8_t* data; /**< 数据首地址；size 为 0 时允许为 NULL。 */
    size_t size;         /**< 数据长度，单位为字节。 */
} ViaimConnectBytesView;

/** @brief 携带一个 UTF-8 字符串视图的通用命令负载。 */
typedef struct ViaimConnectStringData {
    ViaimConnectStringView value; /**< 命令携带的 UTF-8 字符串。 */
} ViaimConnectStringData;

enum {
    VIAIM_CONNECT_STRING_FIELD_VALUE = UINT64_C(1) << 0 /**< value 字段有效。 */
};

/** @brief 携带一个协议布尔值的通用命令负载。 */
typedef struct ViaimConnectSwitchData {
    uint8_t enabled; /**< 开关值：0 表示关闭，1 表示开启。 */
} ViaimConnectSwitchData;

enum {
    VIAIM_CONNECT_SWITCH_FIELD_ENABLED = UINT64_C(1) << 0 /**< enabled 字段有效。 */
};

/** @brief 携带一个协议时长值的通用命令负载。 */
typedef struct ViaimConnectDurationData {
    uint8_t value; /**< 协议定义的时长值；具体单位由对应命令定义。 */
} ViaimConnectDurationData;

enum {
    VIAIM_CONNECT_DURATION_FIELD_VALUE = UINT64_C(1) << 0 /**< value 字段有效。 */
};

/** @brief 根据当前编译目标导出或导入 SDK 公共符号。 */
#if defined(VIAIM_CONNECT_SDK_BUILD)
#  define VIAIM_CONNECT_API __declspec(dllexport)
#else
#  define VIAIM_CONNECT_API __declspec(dllimport)
#endif

/** @brief 在 C++ 中为公共函数声明 C 链接，在 C 中保持不变。 */
#if defined(__cplusplus)
#  define VIAIM_CONNECT_EXTERN_C extern "C"
#else
#  define VIAIM_CONNECT_EXTERN_C
#endif

/**
 * @brief SDK API 的稳定返回值类型。
 * @details 返回码描述本次本地 API 调用；viaim_connect_send() 成功后的设备业务结果
 *          另由 ViaimConnectEvent::protocol_result 报告。
 */
typedef int32_t ViaimConnectResult;

/** @brief SDK API 返回码；每个函数页面列出该函数实际可能返回的子集。 */
enum {
    VIAIM_CONNECT_OK = 0,                        /**< 操作成功。 */
    VIAIM_CONNECT_ERROR_INVALID_ARGUMENT = 1,   /**< 参数为空、越界或组合非法。 */
    VIAIM_CONNECT_ERROR_INVALID_STATE = 2,      /**< 当前生命周期或线程状态不允许该操作。 */
    VIAIM_CONNECT_ERROR_NOT_FOUND = 3,          /**< 未找到请求的资源或设备。 */
    VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION = 4, /**< ABI 或结构体版本不兼容。 */
    VIAIM_CONNECT_ERROR_IO = 5,                 /**< HID 或其他输入输出操作失败。 */
    VIAIM_CONNECT_ERROR_PLUGIN = 6,             /**< 插件加载、初始化或关闭失败。 */
    VIAIM_CONNECT_ERROR_INTERNAL = 7,           /**< SDK 内部错误。 */
    VIAIM_CONNECT_ERROR_AMBIGUOUS_DEVICE = 8,   /**< 检测到多台匹配设备，无法唯一选择。 */
    VIAIM_CONNECT_ERROR_DEVICE_IN_USE = 9       /**< 摄像头已被其他程序占用。 */
};

/** @brief SDK 控制台日志级别类型。 */
typedef int32_t ViaimConnectLogLevel;

/** @brief SDK 控制台日志级别。 */
enum {
    VIAIM_CONNECT_LOG_OFF = -1,   /**< 关闭 SDK 控制台日志。 */
    VIAIM_CONNECT_LOG_ERROR = 0,  /**< 仅输出错误。 */
    VIAIM_CONNECT_LOG_WARNING = 1,/**< 输出错误和警告。 */
    VIAIM_CONNECT_LOG_INFO = 2,   /**< 输出一般运行信息。 */
    VIAIM_CONNECT_LOG_DEBUG = 3   /**< 输出调试信息。 */
};

/** @brief 原始收发数据日志通道标志类型。 */
typedef uint32_t ViaimConnectRawLogFlags;

/**
 * @brief 原始收发数据日志通道标志。
 * @details 可按位组合 BUSINESS 和 MEDIA；NONE(0) 关闭原始数据日志，未知位会被
 *          viaim_connect_set_raw_log_flags() 拒绝。
 */
enum {
    VIAIM_CONNECT_RAW_LOG_NONE = 0,          /**< 不输出原始收发数据。 */
    VIAIM_CONNECT_RAW_LOG_BUSINESS = 1U << 0,/**< 输出 0x06 业务通道。 */
    VIAIM_CONNECT_RAW_LOG_MEDIA = 1U << 1,   /**< 输出 0x07/0x08 Media 通道。 */
    VIAIM_CONNECT_RAW_LOG_ALL =
        VIAIM_CONNECT_RAW_LOG_BUSINESS | VIAIM_CONNECT_RAW_LOG_MEDIA
};

/** @brief 心跳可观察行为标志类型。 */
typedef uint32_t ViaimConnectHeartbeatFlags;

/**
 * @brief 0x004A 心跳的日志和回调标志。
 *
 * 这些标志不影响 SDK 自动回复心跳。可按位组合两个非零标志；NONE(0) 同时关闭
 * 心跳原始日志和事件回调，未知位会被 viaim_connect_set_heartbeat_flags() 拒绝。
 */
enum {
    VIAIM_CONNECT_HEARTBEAT_NONE = 0, /**< 不打印心跳且不触发事件回调。 */
    VIAIM_CONNECT_HEARTBEAT_RAW_LOG = 1U << 0, /**< 允许输出心跳原始 HEX。 */
    VIAIM_CONNECT_HEARTBEAT_EVENT_CALLBACK =
        1U << 1, /**< 允许心跳进入统一业务事件回调。 */
    VIAIM_CONNECT_HEARTBEAT_ALL =
        VIAIM_CONNECT_HEARTBEAT_RAW_LOG |
        VIAIM_CONNECT_HEARTBEAT_EVENT_CALLBACK
};

/**
 * @brief SDK 实例创建选项。
 *
 * 建议使用 VIAIM_CONNECT_CREATE_OPTIONS_INIT 初始化。NULL 选项等价于该宏的默认值。
 * 配置路径是以 NUL 结尾的 UTF-8 字符串，会在 viaim_connect_create() 返回前完成
 * 读取和复制，调用方随后可以释放；所有 reserved 元素必须为 0。
 */
typedef struct ViaimConnectCreateOptions {
    uint32_t struct_size; /**< 当前结构体大小，即 sizeof(ViaimConnectCreateOptions)。 */
    const char* description_config_path; /**< 释义 JSON 的 UTF-8 路径；NULL 使用 SDK DLL 同目录的默认文件。 */
    uint64_t reserved[4]; /**< 保留字段，必须全部为 0。 */
} ViaimConnectCreateOptions;

/**
 * @brief ViaimConnectCreateOptions 默认初始化值。
 * @details 设置正确的 struct_size、默认释义路径和零保留字段；通常只覆盖
 *          description_config_path，然后传给 viaim_connect_create()。
 */
#define VIAIM_CONNECT_CREATE_OPTIONS_INIT \
    { sizeof(ViaimConnectCreateOptions), NULL, {0} }

/** @brief Connect 连接状态类型。 */
typedef int32_t ViaimConnectConnectionState;

/** @brief Connect 连接状态。 */
enum {
    VIAIM_CONNECT_CONNECTION_CLOSED = 0,      /**< 已关闭且不会自动重连。 */
    VIAIM_CONNECT_CONNECTION_CONNECTING = 1,  /**< 正在进行首次连接。 */
    VIAIM_CONNECT_CONNECTION_CONNECTED = 2,   /**< 已连接，可发送命令。 */
    VIAIM_CONNECT_CONNECTION_RECONNECTING = 3 /**< 连接断开，正在退避重连。 */
};

/**
 * @brief 打开 Connect 连接时使用的选项。
 *
 * 建议使用 VIAIM_CONNECT_OPEN_OPTIONS_INIT 初始化，再覆盖需要修改的字段。VID/PID
 * 用于筛选同一台 Connect 设备的 HID 与摄像头身份；重连和读取时间均以毫秒为单位。
 * reconnect_initial_delay_ms 必须非零且不大于 reconnect_max_delay_ms，read_timeout_ms
 * 必须非零，所有 reserved 元素必须为 0。结构会在 viaim_connect_open() 返回前复制。
 */
typedef struct ViaimConnectOpenOptions {
    uint32_t struct_size; /**< 当前结构体大小，即 sizeof(ViaimConnectOpenOptions)。 */
    uint16_t vendor_id;  /**< USB HID Vendor ID。 */
    uint16_t product_id; /**< USB HID Product ID。 */
    uint32_t reconnect_initial_delay_ms; /**< 首次断线重连等待时间，单位毫秒。 */
    uint32_t reconnect_max_delay_ms; /**< 指数退避的最大等待时间，单位毫秒。 */
    uint32_t read_timeout_ms; /**< 单次 HID 后台读取超时，单位毫秒。 */
    uint64_t reserved[4]; /**< 保留字段，必须全部为 0。 */
} ViaimConnectOpenOptions;

/**
 * @brief ViaimConnectOpenOptions 默认初始化值。
 *
 * 默认 VID 为 0x2207、PID 为 0x0017，重连初始等待 100 ms、最大等待
 * 3000 ms，HID 读取超时 50 ms。成功传给 viaim_connect_open() 只会启动异步连接
 * 流程；实际可用状态由 ViaimConnectConnectionStateCallback 报告。
 */
#define VIAIM_CONNECT_OPEN_OPTIONS_INIT                                      \
    {                                                                     \
        sizeof(ViaimConnectOpenOptions), 0x2207, 0x0017, 100, 3000, 50, {0}  \
    }

/** @} */
