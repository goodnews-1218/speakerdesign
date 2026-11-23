#pragma once

/**
 * @file ViaimConnect.h
 * @brief viaim_connect_sdk 实例生命周期和设备连接 API。
 */

#include <viaim_connect/Protocol.h>
#include <viaim_connect/Recording.h>

/**
 * @brief 创建一个相互独立的 SDK 实例。
 *
 * 这是所有实例级 API 的第一步。函数同步读取并校验消息释义配置、发现并加载插件、
 * 初始化 HID 后端；全部成功后才发布句柄。每个实例拥有独立的连接、回调和运行设置。
 *
 * @param[in] options 创建选项。允许为 NULL，此时使用
 *                    VIAIM_CONNECT_CREATE_OPTIONS_INIT 的默认配置。非空时必须通过初始化宏
 *                    初始化；description_config_path 在函数返回前完成读取。
 * @param[out] out_sdk 接收新实例句柄的非空指针。进入函数后立即写为 NULL，仅在返回
 *                     VIAIM_CONNECT_OK 时得到有效句柄。
 * @post 成功时返回的句柄处于未打开状态；失败时不泄漏部分初始化资源，*out_sdk 为 NULL。
 * @retval VIAIM_CONNECT_OK 实例创建成功。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT 输出指针、保留字段或配置路径参数无效。
 * @retval VIAIM_CONNECT_ERROR_NOT_FOUND 默认或指定的释义配置文件不存在。
 * @retval VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION 配置、插件或公共结构版本不兼容。
 * @retval VIAIM_CONNECT_ERROR_IO 无法定位 SDK、读取配置或初始化底层后端。
 * @retval VIAIM_CONNECT_ERROR_PLUGIN 插件发现、加载或初始化失败。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 未捕获的内部异常被 C ABI 边界转换。
 * @par 生命周期
 * 成功返回的句柄由调用方独占，最终必须传给 viaim_connect_destroy() 一次。
 * @par 线程与回调
 * 创建期间不会触发该实例的用户回调。
 * @par 示例
 * @code{.c}
 * ViaimConnectSdk* sdk = NULL;
 * ViaimConnectCreateOptions options = VIAIM_CONNECT_CREATE_OPTIONS_INIT;
 * options.description_config_path = "config/viaim_connect_descriptions.json";
 * ViaimConnectResult result = viaim_connect_create(&options, &sdk);
 * if (result != VIAIM_CONNECT_OK) {
 *     return result;
 * }
 * // 后续步骤失败时也必须清理已创建的 sdk。
 * result = viaim_connect_set_log_level(sdk, VIAIM_CONNECT_LOG_INFO);
 * if (result != VIAIM_CONNECT_OK) {
 *     (void)viaim_connect_destroy(sdk);
 *     return result;
 * }
 * @endcode
 * @see VIAIM_CONNECT_CREATE_OPTIONS_INIT
 * @see viaim_connect_destroy()
 * @ingroup viaim_sdk_lifecycle
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_create(
    const ViaimConnectCreateOptions* options,
    ViaimConnectSdk** out_sdk);

/**
 * @brief 同步停止全部活动并销毁 SDK 实例。
 * @param[in] sdk 由 viaim_connect_create() 返回且尚未销毁的句柄。
 * @pre 不能从该实例的 SDK 回调或摄像头回调线程调用。
 * @post 除回调线程重入被拒绝外，本函数会先关闭摄像头和设备连接，再释放句柄；即使
 *       关闭阶段返回非 OK，句柄也已被消费并失效。函数返回后不再产生该实例的回调。
 * @retval VIAIM_CONNECT_OK 资源已全部停止并释放。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 为空或不是有效句柄。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 从该实例的回调线程调用；此时句柄仍有效。
 * @retval VIAIM_CONNECT_ERROR_IO 关闭设备会话时发生 I/O 错误；句柄仍已销毁。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 停止摄像头或内部清理失败；句柄仍已销毁。
 * @par 生命周期
 * 一旦本函数开始消费句柄，调用方不得重试 destroy 或继续访问该指针。
 * @par 线程与回调
 * 应由应用控制线程调用；需要从回调触发退出时，只向控制线程投递请求。
 * @see viaim_connect_create()
 * @see viaim_connect_close()
 * @ingroup viaim_sdk_lifecycle
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_destroy(ViaimConnectSdk* sdk);

/**
 * @brief 立即设置指定实例的控制台日志级别。
 * @param[in] sdk 有效 SDK 实例。
 * @param[in] level 一个 VIAIM_CONNECT_LOG_* 枚举值；OFF 关闭控制台日志。
 * @pre sdk 尚未销毁。
 * @post 成功时新级别立即用于随后产生的日志；不会启动或停止设备连接。
 * @retval VIAIM_CONNECT_OK 设置已应用。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 或 level 无效。
 * @par 线程与回调
 * 可在连接前后或用户回调中调用；不改变回调线程模型。
 * @see ViaimConnectLogLevel
 * @ingroup viaim_connection
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_set_log_level(
    ViaimConnectSdk* sdk,
    ViaimConnectLogLevel level);

/**
 * @brief 立即设置指定实例输出原始收发 HEX 的通道。
 * @param[in] sdk 有效 SDK 实例。
 * @param[in] flags VIAIM_CONNECT_RAW_LOG_* 的按位组合；NONE(0) 全部关闭。
 * @pre sdk 尚未销毁。
 * @post 成功时设置立即影响随后收发的数据；不会建立连接。
 * @retval VIAIM_CONNECT_OK 设置已应用。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 无效或 flags 含未知位。
 * @par 线程与回调
 * 可在连接前后或用户回调中调用。
 * @see ViaimConnectRawLogFlags
 * @ingroup viaim_connection
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_set_raw_log_flags(
    ViaimConnectSdk* sdk,
    ViaimConnectRawLogFlags flags);

/**
 * @brief 立即设置指定实例的心跳日志和事件可见性。
 * @param[in] sdk 有效 SDK 实例。
 * @param[in] flags VIAIM_CONNECT_HEARTBEAT_* 的按位组合；NONE(0) 隐藏心跳。
 * @pre sdk 尚未销毁。
 * @post 成功时设置立即生效；SDK 始终继续自动回复心跳，也不会因此建立连接。
 * @retval VIAIM_CONNECT_OK 设置已应用。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 无效或 flags 含未知位。
 * @par 线程与回调
 * 可在连接前后或用户回调中调用。
 * @see ViaimConnectHeartbeatFlags
 * @ingroup viaim_connection
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_set_heartbeat_flags(
    ViaimConnectSdk* sdk,
    ViaimConnectHeartbeatFlags flags);

/**
 * @brief 复制连接配置和回调，并启动指定实例的设备连接流程。
 *
 * 匹配不到设备时仍可返回 OK，实例进入 RECONNECTING 并持续重试。匹配到多台物理设备
 * 时返回 AMBIGUOUS_DEVICE 且不保留连接会话。OK 仅表示连接流程已启动，并不保证此刻
 * 已经 CONNECTED；业务应等待 connection_state 回调。
 *
 * @param[in] sdk 有效且尚未打开的 SDK 实例。
 * @param[in] options 连接选项；NULL 等价于 VIAIM_CONNECT_OPEN_OPTIONS_INIT。非空结构
 *                    及其保留字段在返回前校验并复制。
 * @param[in] callbacks 回调集合；NULL 等价于 VIAIM_CONNECT_CALLBACKS_INIT。函数复制
 *                      结构，但不拥有 callbacks->user_data 指向的对象。
 * @pre 同一实例未处于 open 或已打开状态。
 * @post 返回 OK 时连接会话保持活动，并通过回调报告后续连接、重连和错误；失败时不
 *       留下部分会话。
 * @retval VIAIM_CONNECT_OK 连接流程已经启动。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk、延迟范围、超时或保留字段无效。
 * @retval VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION options 或 callbacks 结构版本不兼容。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 实例已打开或正在执行另一次 open。
 * @retval VIAIM_CONNECT_ERROR_AMBIGUOUS_DEVICE 同时发现多台匹配的 Connect 设备。
 * @retval VIAIM_CONNECT_ERROR_IO 传输后端启动失败。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 无法创建连接会话或发生内部异常。
 * @par 生命周期
 * callbacks->user_data 由调用方拥有，必须保持有效直到 viaim_connect_close() 返回。
 * @par 线程与回调
 * open 返回前可能启动分发线程；应用只在 CONNECTED 通知后开始发送业务命令。
 * @par 示例
 * @code{.c}
 * static void __cdecl state_changed(
 *     ViaimConnectConnectionState state, void* user_data) {
 *     (void)user_data;
 *     if (state == VIAIM_CONNECT_CONNECTION_CONNECTED) {
 *         // 通知业务线程：连接现在可用。
 *     }
 * }
 * static void __cdecl async_error(
 *     ViaimConnectResult error, const char* message, void* user_data) {
 *     (void)error; (void)message; (void)user_data;
 * }
 * ViaimConnectOpenOptions options = VIAIM_CONNECT_OPEN_OPTIONS_INIT;
 * ViaimConnectCallbacks callbacks = VIAIM_CONNECT_CALLBACKS_INIT;
 * callbacks.connection_state = state_changed;
 * callbacks.error = async_error;
 * ViaimConnectResult result = viaim_connect_open(sdk, &options, &callbacks);
 * // result == OK 后继续事件循环，不把它当作 CONNECTED。
 * @endcode
 * @see VIAIM_CONNECT_OPEN_OPTIONS_INIT
 * @see VIAIM_CONNECT_CALLBACKS_INIT
 * @see viaim_connect_close()
 * @ingroup viaim_connection
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_open(
    ViaimConnectSdk* sdk,
    const ViaimConnectOpenOptions* options,
    const ViaimConnectCallbacks* callbacks);

/**
 * @brief 同步关闭摄像头和设备连接，并排空该实例的用户回调。
 * @param[in] sdk 有效 SDK 实例。
 * @pre 不能从该实例的 SDK 回调或摄像头回调线程调用。
 * @post 成功时摄像头先被关闭，连接身份和会话随后清除，且不再产生新用户回调。
 *       实例保持有效，可再次调用 viaim_connect_open()。未打开时调用也成功。
 * @retval VIAIM_CONNECT_OK 已关闭或原本就处于关闭状态。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 无效。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 从回调线程调用，或底层会话拒绝关闭。
 * @retval VIAIM_CONNECT_ERROR_IO 关闭设备传输时发生 I/O 错误。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 停止摄像头或内部清理失败。
 * @par 生命周期
 * 本函数不销毁 sdk；完成全部业务后仍须调用 viaim_connect_destroy()。
 * @par 线程与回调
 * 若另一个线程正在 open，本函数等待 open 结束后再关闭。应由应用控制线程调用。
 * @see viaim_connect_open()
 * @see viaim_connect_destroy()
 * @see viaim_connect_camera_close()
 * @ingroup viaim_connection
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_close(ViaimConnectSdk* sdk);

/**
 * @brief 同步读取指定实例当前的连接状态快照。
 * @param[in] sdk 有效 SDK 实例。
 * @param[out] state 接收 ViaimConnectConnectionState 的非空指针：CLOSED 表示无活动
 *                   会话，CONNECTING 表示首次连接，CONNECTED 表示可发送命令，
 *                   RECONNECTING 表示断线后正在重试。
 * @pre sdk 和 state 均非空且 sdk 尚未销毁。
 * @post 成功时写入调用瞬间的状态；返回后其他线程可能立即改变实际状态。
 * @retval VIAIM_CONNECT_OK 已写入状态。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 或 state 无效。
 * @par 线程与回调
 * 这是只读快照查询，可从用户回调或其他线程调用。
 * @see ViaimConnectConnectionStateCallback
 * @ingroup viaim_connection
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_get_connection_state(
    ViaimConnectSdk* sdk,
    ViaimConnectConnectionState* state);

/**
 * @defgroup viaim_camera 摄像头采集
 * @brief 枚举与当前 Connect 身份对应的摄像头，并交付固定单平面 BGR24 帧。
 * @details
 * **核心函数、调用时机和调用效果**
 *
 * | 函数 | 调用时机 | 调用效果 |
 * | --- | --- | --- |
 * | viaim_connect_camera_enumerate() | open 已建立设备身份后 | 返回调用方拥有的不可变设备/能力快照 |
 * | viaim_connect_camera_enumeration_get_device_info() | 快照有效期间 | 返回借用的设备名和能力数量 |
 * | viaim_connect_camera_enumeration_get_capability() | 选择模式时 | 返回一项 SDK 可输出的 BGR24 宽、高和整数 fps 范围 |
 * | viaim_connect_camera_enumeration_destroy() | 不再查询或高级 open 已复制选择后 | 释放快照及其借用视图 |
 * | viaim_connect_camera_open() | 摄像头为 CLOSED 时 | 自动选择 1920x1080、30 fps 并同步打开 |
 * | viaim_connect_camera_open_with_options() | 需要自选模式且摄像头为 CLOSED 时 | 校验并复制指定模式后同步打开 |
 * | viaim_connect_camera_get_state() | 需要状态快照时 | 返回调用瞬间的摄像头状态 |
 * | viaim_connect_camera_close() | 停止采集或关闭 SDK 前 | 同步停流并排空本次流的摄像头回调 |
 *
 * **前置条件：** SDK 已通过 open 建立 Connect 身份；输出和选项结构使用初始化宏；
 * 摄像头回调已随 ViaimConnectCallbacks 传给 open。
 *
 * **调用效果：** SDK 使用内部的 vendored OpenCV 和 `CAP_DSHOW`。Windows 通过
 * DirectShow `IAMStreamConfig` 读取 `driver-advertised Windows modes`，枚举阶段不打开
 * 摄像头流或读取帧。驱动公布的 MJPEG、YUY2、NV12 模式只有在 OpenCV 4.12 DirectShow
 * 路径支持转换时才形成离散 BGR24 能力；`H.264-only modes are omitted`。
 * 调用方只填写 width、height 和 `uint32_t fps`，并从同一枚举快照选择三者。
 *
 * SDK 为每个公开模式保存 `private native FOURCC`，调用方不设置或看到输入 FOURCC。
 * 正式打开会精确校验生效的宽、高、fps、FOURCC 和首个 `CV_8UC3` 帧；底层不能精确
 * 交付时明确失败，不静默回退。对于 OpenCV 交付得比请求更快的输入，SDK 在唯一采集
 * 线程内对公开回调做节奏控制（允许调度容差）。SDK 不输出 NV12/MJPEG。公共输出始终是
 * VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24、`plane_count == 1`，且
 * `strides[0] >= width * 3`、`planes[0].size == strides[0] * height`；这里的数据大小
 * data_size 即 planes[0].size。第二平面固定满足 `planes[1].data == NULL`、
 * `planes[1].size == 0`、`strides[1] == 0`。
 *
 * **生命周期：** 枚举快照必须且只能销毁一次；帧和 plane 数据只在回调期间有效。
 *
 * **线程与回调：** 帧回调运行在唯一采集线程；采集循环产生的状态也在该线程发布，
 * OPENING/CLOSED 等同步状态可由调用线程发布。没有监控线程，也没有 mailbox/dispatcher。
 * frame 和 plane 属于借用数据；借用数据仅在当前回调有效，保留时必须在回调内同步复制。
 * 回调不得阻塞或重入 enumerate、open、close、destroy；
 * `close() 等待采集线程及当前回调结束`，成功返回后不会再产生该流的回调。
 *
 * 完整顺序参见 @ref api_camera "摄像头 BGR24 采集"。
 * @{
 */

/**
 * @brief 不透明、不可变的摄像头枚举快照。
 * @details 快照绑定创建它的 SDK 实例身份和当时的 Connect VID/PID，拥有设备名与能力
 *          数据；必须由 viaim_connect_camera_enumeration_destroy() 销毁且只能销毁一次。
 */
typedef struct ViaimConnectCameraEnumeration ViaimConnectCameraEnumeration;

/** @brief 摄像头公共输出像素格式类型；不表示驱动原生输入 subtype。 */
typedef int32_t ViaimConnectCameraPixelFormat;

enum {
    /** @brief 未选择或未知的摄像头像素格式。 */
    VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_UNKNOWN = 0,
    /** @brief 单平面交错 BGR 24 位像素数据，字节三元组顺序为 B、G、R。 */
    VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24 = 1
};

/**
 * @brief 枚举快照内的 Connect 摄像头信息。
 * @details name 是不保证 NUL 结尾的 UTF-8 借用视图，在所属 enumeration 销毁时失效。
 */
typedef struct ViaimConnectCameraDeviceInfo {
    uint32_t struct_size; /**< 调用方容量；使用 VIAIM_CONNECT_CAMERA_DEVICE_INFO_INIT。 */
    ViaimConnectStringView name; /**< 借用的 UTF-8 设备名，快照销毁时失效。 */
    uint32_t capability_count; /**< 可按零基索引查询的能力数量。 */
    uint64_t reserved[4]; /**< ABI 保留输出；调用方忽略。 */
} ViaimConnectCameraDeviceInfo;

/** @brief 传给 viaim_connect_camera_enumeration_get_device_info() 的输出初始化值。 */
#define VIAIM_CONNECT_CAMERA_DEVICE_INFO_INIT \
    { sizeof(ViaimConnectCameraDeviceInfo), {NULL, 0}, 0, {0} }

/**
 * @brief SDK 可输出的一段 BGR24 摄像头有效模式范围。
 * @details 此能力来自 Windows 驱动通过 IAMStreamConfig 明确公布且 OpenCV DirectShow
 * 路径可转换的原生模式。width/height 必须位于最小/最大值之间并符合对应 step；fps 为
 * 整数帧率并位于公布范围内。原生输入格式不进入公共 ABI。
 */
typedef struct ViaimConnectCameraCapability {
    uint32_t struct_size; /**< 调用方容量；使用 VIAIM_CONNECT_CAMERA_CAPABILITY_INIT。 */
    ViaimConnectCameraPixelFormat pixel_format; /**< 固定为 BGR24。 */
    uint32_t min_width; /**< 最小帧宽，单位像素。 */
    uint32_t max_width; /**< 最大帧宽，单位像素。 */
    uint32_t width_step; /**< 可选帧宽步长，单位像素。 */
    uint32_t min_height; /**< 最小帧高，单位像素。 */
    uint32_t max_height; /**< 最大帧高，单位像素。 */
    uint32_t height_step; /**< 可选帧高步长，单位像素。 */
    uint32_t min_fps; /**< 最低帧率，单位帧每秒。 */
    uint32_t max_fps; /**< 最高帧率，单位帧每秒。 */
    uint64_t reserved[4]; /**< ABI 保留输出；调用方忽略。 */
} ViaimConnectCameraCapability;

/** @brief 传给 viaim_connect_camera_enumeration_get_capability() 的输出初始化值。 */
#define VIAIM_CONNECT_CAMERA_CAPABILITY_INIT                         \
    {                                                               \
        sizeof(ViaimConnectCameraCapability),                       \
        VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_UNKNOWN,                  \
        0, 0, 0, 0, 0, 0, 0, 0, {0}                               \
    }

/**
 * @brief 从枚举快照中选择并打开一个具体 BGR24 输出模式。
 * @details 调用方只填写 width/height/fps，且三者必须匹配同一快照中的有效 BGR24 能力；
 *          没有输入 FOURCC 字段。结构在
 *          viaim_connect_camera_open_with_options() 返回前复制，reserved
 *          必须全部为 0。
 */
typedef struct ViaimConnectCameraOpenOptions {
    uint32_t struct_size; /**< 结构大小；由初始化宏设置。 */
    uint32_t width; /**< 请求帧宽，单位像素。 */
    uint32_t height; /**< 请求帧高，单位像素。 */
    uint32_t fps; /**< 请求帧率，单位帧每秒。 */
    uint64_t reserved[4]; /**< ABI 保留输入，必须全为 0。 */
} ViaimConnectCameraOpenOptions;

/**
 * @brief ViaimConnectCameraOpenOptions 初始化值。
 * @details 设置结构版本和零保留区；调用方必须再填写宽、高和帧率。
 */
#define VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT \
    {                                                   \
        sizeof(ViaimConnectCameraOpenOptions),          \
        0, 0, 0, {0}                                    \
    }

/** @brief 摄像头帧方向等附加信息的位标志类型。 */
typedef uint32_t ViaimConnectCameraFrameFlags;

enum {
    VIAIM_CONNECT_CAMERA_FRAME_FLAG_NONE = 0, /**< 无附加方向处理。 */
    VIAIM_CONNECT_CAMERA_FRAME_FLAG_VERTICAL_FLIPPED = 1U << 0 /**< 解码图像需垂直翻转。 */
};

/**
 * @brief 借用的 SDK BGR24 输出帧。
 *
 * `plane_count == 1`；planes[0] 是逐行存储的交错 BGR24，像素字节三元组顺序为 B、G、R。
 * `strides[0] >= width * 3`，`planes[0].size == strides[0] * height`，planes[1]
 * 为空且 strides[1] 为 0。计算乘积时应先提升到 uint64_t 并检查溢出。本结构体和 plane
 * 数据仅在当前 frame callback 内有效，需要保留时必须同步复制。回调直接运行在唯一
 * 采集线程，没有 mailbox/dispatcher；不得阻塞或重入摄像头生命周期函数。
 */
struct ViaimConnectCameraFrame {
    uint32_t struct_size; /**< SDK 写入的结构大小。 */
    ViaimConnectCameraPixelFormat pixel_format; /**< 借用的 BGR24 负载格式。 */
    uint32_t width; /**< 帧宽，单位像素。 */
    uint32_t height; /**< 帧高，单位像素。 */
    uint32_t plane_count; /**< planes/strides 中有效项数量，固定为 1。 */
    ViaimConnectBytesView planes[2]; /**< 借用的平面负载，只在帧回调期间有效。 */
    uint32_t strides[2]; /**< 平面行跨度；strides[0] 为 BGR24 行跨度，strides[1] 为 0。 */
    uint64_t sequence; /**< 当前流内从 0 开始递增的 SDK 帧序号。 */
    int64_t start_time_100ns; /**< 后端帧开始时间戳，单位 100 ns。 */
    int64_t stop_time_100ns; /**< 后端帧结束时间戳，单位 100 ns。 */
    int32_t rotation_degrees; /**< 旋转元数据，单位度。 */
    ViaimConnectCameraFrameFlags flags; /**< VIAIM_CONNECT_CAMERA_FRAME_FLAG_* 位集合。 */
};

/**
 * @brief 枚举当前 Connect 身份对应的摄像头，并创建不可变能力快照。
 * @param[in] sdk 已成功调用 viaim_connect_open()、拥有当前 VID/PID 身份的实例。
 * @param[out] out_enumeration 接收新快照的非空指针；函数进入后立即写为 NULL，仅在
 *                             返回 OK 时得到调用方拥有的有效句柄。
 * @pre 不能从摄像头回调线程调用；SDK 连接会话尚未 close。
 * @post 成功时快照固定设备名和能力，即使之后重新枚举也不会改变；失败时输出为 NULL。
 * @details Windows DirectShow 定位 VID/PID 匹配的设备，并通过 IAMStreamConfig 读取驱动
 *          明确公布的受支持模式。枚举不创建 VideoCapture、不打开流、不读取帧。
 * @retval VIAIM_CONNECT_OK 已创建枚举快照。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 或 out_enumeration 无效。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 未通过 open 建立身份，或从摄像头回调调用。
 * @retval VIAIM_CONNECT_ERROR_NOT_FOUND 未找到匹配摄像头。
 * @retval VIAIM_CONNECT_ERROR_AMBIGUOUS_DEVICE 找到多台匹配摄像头，无法唯一选择。
 * @retval VIAIM_CONNECT_ERROR_IO DirectShow 枚举失败。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 枚举或 C ABI 边界发生内部异常。
 * @par 生命周期
 * 成功快照必须传给 viaim_connect_camera_enumeration_destroy() 一次；其字符串和能力
 * 输出都借用快照内存。
 * @par 线程与回调
 * 这是同步操作，应在控制线程执行；不会触发帧回调。
 * @par 示例
 * @code{.c}
 * ViaimConnectCameraEnumeration* list = NULL;
 * ViaimConnectResult result = viaim_connect_camera_enumerate(sdk, &list);
 * if (result == VIAIM_CONNECT_OK) {
 *     ViaimConnectCameraDeviceInfo info =
 *         VIAIM_CONNECT_CAMERA_DEVICE_INFO_INIT;
 *     result = viaim_connect_camera_enumeration_get_device_info(list, &info);
 *     // info.name 借用 list；需要长期使用时现在复制。
 *     (void)viaim_connect_camera_enumeration_destroy(list);
 * }
 * @endcode
 * @see viaim_connect_camera_enumeration_get_device_info()
 * @see viaim_connect_camera_enumeration_destroy()
 * @ingroup viaim_camera
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_camera_enumerate(
    ViaimConnectSdk* sdk,
    ViaimConnectCameraEnumeration** out_enumeration);

/**
 * @brief 从枚举快照读取设备名和可查询的能力数量。
 * @param[in] enumeration 有效且尚未销毁的枚举快照。
 * @param[out] out_info 使用 VIAIM_CONNECT_CAMERA_DEVICE_INFO_INIT 初始化的输出结构。
 * @pre enumeration 与 out_info 有效，输出 struct_size 至少覆盖公共字段。
 * @post 成功时写入借用名称和 capability_count；失败时不得读取输出字段。
 * @retval VIAIM_CONNECT_OK 已写入设备信息。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT enumeration 或 out_info 无效。
 * @retval VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION out_info 的结构容量过小。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 能力数量无法表示或发生内部异常。
 * @par 生命周期
 * out_info->name 在 enumeration 销毁时失效，不保证 NUL 结尾。
 * @par 线程与回调
 * 快照只读，可从普通线程并发查询；调用方负责避免与 destroy 并发。
 * @see VIAIM_CONNECT_CAMERA_DEVICE_INFO_INIT
 * @see viaim_connect_camera_enumeration_get_capability()
 * @ingroup viaim_camera
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_camera_enumeration_get_device_info(
    const ViaimConnectCameraEnumeration* enumeration,
    ViaimConnectCameraDeviceInfo* out_info);

/**
 * @brief 从枚举快照读取一项 SDK 可输出的 BGR24 宽、高和整数 fps 范围。
 * @param[in] enumeration 有效且尚未销毁的枚举快照。
 * @param[in] index 小于 ViaimConnectCameraDeviceInfo::capability_count 的零基索引。
 * @param[out] out_capability 使用 VIAIM_CONNECT_CAMERA_CAPABILITY_INIT 初始化的输出结构。
 * @pre enumeration、index 和输出结构版本有效。
 * @post 成功时写入有效输出范围且 pixel_format 为
 *       VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24；失败时不得读取输出字段。
 * @retval VIAIM_CONNECT_OK 已写入能力范围。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT enumeration/out_capability 无效或 index 越界。
 * @retval VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION out_capability 的结构容量过小。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 读取或 C ABI 边界发生内部异常。
 * @par 生命周期
 * 输出仅含数值，不借用额外缓冲区；enumeration 仍须按所有权规则销毁。
 * @par 线程与回调
 * 快照只读，可从普通线程并发查询；调用方负责避免与 destroy 并发。
 * @see VIAIM_CONNECT_CAMERA_CAPABILITY_INIT
 * @see viaim_connect_camera_open_with_options()
 * @ingroup viaim_camera
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_camera_enumeration_get_capability(
    const ViaimConnectCameraEnumeration* enumeration,
    uint32_t index,
    ViaimConnectCameraCapability* out_capability);

/**
 * @brief 销毁一个摄像头枚举快照及其拥有的字符串和能力数据。
 * @param[in] enumeration 有效且尚未销毁的枚举句柄。
 * @pre 不能与该快照的 get/open 调用并发；每个成功句柄只能调用本函数一次。
 * @post 返回 OK 后 enumeration 及所有借用视图立即失效。
 * @retval VIAIM_CONNECT_OK 快照已释放。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT enumeration 为 NULL 或不是可验证的有效快照。
 * @par 生命周期
 * 对已经成功销毁的悬空指针再次调用属于无效使用，其结果未定义；不要以返回码作为
 * 双重销毁检测手段。调用方应在成功后把自己的变量置为 NULL。
 * @par 线程与回调
 * 不触发回调；由持有该快照所有权的线程执行。
 * @see viaim_connect_camera_enumerate()
 * @ingroup viaim_camera
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_camera_enumeration_destroy(
    ViaimConnectCameraEnumeration* enumeration);

/**
 * @brief 使用默认 BGR24 输出模式同步打开摄像头采集。
 * @details 自动枚举当前 Connect 摄像头，并选择第一个支持 1920x1080、30 fps BGR24
 *          输出的能力。摄像头回调来自传给
 *          viaim_connect_open() 的 ViaimConnectCallbacks。
 * @param[in] sdk 摄像头状态为 VIAIM_CONNECT_CAMERA_CLOSED 的有效实例。
 * @retval VIAIM_CONNECT_OK 摄像头已打开并进入 STREAMING。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 无效。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 没有当前 Connect 身份、摄像头未 CLOSED，或
 *         从摄像头回调线程调用。
 * @retval VIAIM_CONNECT_ERROR_NOT_FOUND 未找到摄像头或默认模式。
 * @retval VIAIM_CONNECT_ERROR_AMBIGUOUS_DEVICE 后端无法唯一选择目标摄像头。
 * @retval VIAIM_CONNECT_ERROR_IO 枚举、打开或开始采集期间发生 I/O 错误。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 初始化采集或 C ABI 边界发生内部异常。
 * @see viaim_connect_camera_open_with_options()
 * @see viaim_connect_camera_close()
 * @ingroup viaim_camera
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_camera_open(ViaimConnectSdk* sdk);

/**
 * @brief 校验所选 BGR24 输出模式并同步打开摄像头采集后端。
 *
 * enumeration 必须来自同一 SDK 实例和当前 Connect 身份。函数复制 options，
 * 发布 OPENING 状态，通过 OpenCV VideoCapture 的 `CAP_DSHOW` 后端打开快照保存的私有
 * 原生 FOURCC；只有生效 FOURCC、宽、高、fps 精确匹配且首帧是 CV_8UC3 时才返回 OK
 * 并进入 STREAMING，否则明确失败。
 *
 * @param[in] sdk 摄像头状态为 VIAIM_CONNECT_CAMERA_CLOSED 的有效实例。
 * @param[in] enumeration 由同一 sdk、当前 open 会话创建且尚未销毁的快照。
 * @param[in] options 使用 VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT 初始化并填写的具体模式。
 * @pre 不能从摄像头回调线程调用；选择的宽高满足能力步长且整数 fps 位于公布范围。
 * @post 返回 OK 时后端正在采集且结构已复制，调用方可销毁 enumeration；失败发生在启动
 *       采集线程之后时会进入 ERROR 并可能投递状态/SDK 错误通知，随后应调用 camera_close。
 * @retval VIAIM_CONNECT_OK 摄像头已打开并进入 STREAMING。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk、快照、模式、保留字段或身份归属无效。
 * @retval VIAIM_CONNECT_ERROR_INCOMPATIBLE_VERSION options 结构版本不兼容。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 没有当前 Connect 身份、摄像头未 CLOSED，或
 *         从摄像头回调线程调用。
 * @retval VIAIM_CONNECT_ERROR_NOT_FOUND 快照未公布所请求模式，或实际模式与请求不匹配。
 * @retval VIAIM_CONNECT_ERROR_AMBIGUOUS_DEVICE 后端无法唯一选择目标摄像头。
 * @retval VIAIM_CONNECT_ERROR_IO VideoCapture 打开或读取首帧失败，或采集期间发生 I/O 错误。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 初始化采集或 C ABI 边界发生内部异常。
 * @par 生命周期
 * ViaimConnectCallbacks::user_data 由调用方拥有，保持有效到 viaim_connect_close() 返回。
 * frame 数据仅在回调内有效；enumeration 在本函数返回后不再被采集流借用。
 * @par 线程与回调
 * 帧及采集循环产生的状态在唯一采集线程直接串行交付；OPENING 由调用线程同步发布。
 * 回调可能在本函数返回前开始执行。
 * @par 示例
 * @code{.c}
 * ViaimConnectCameraCapability cap = VIAIM_CONNECT_CAMERA_CAPABILITY_INIT;
 * ViaimConnectResult result =
 *     viaim_connect_camera_enumeration_get_capability(list, 0, &cap);
 * if (result == VIAIM_CONNECT_OK) {
 *     ViaimConnectCameraOpenOptions options =
 *         VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT;
 *     options.width = cap.min_width;
 *     options.height = cap.min_height;
 *     options.fps = cap.min_fps;
 *     result = viaim_connect_camera_open_with_options(sdk, list, &options);
 * }
 * // 无论 open 是否成功，最后都销毁 list；成功流已经复制选择。
 * (void)viaim_connect_camera_enumeration_destroy(list);
 * @endcode
 * @see VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT
 * @see viaim_connect_camera_open()
 * @see viaim_connect_camera_close()
 * @ingroup viaim_camera
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_camera_open_with_options(
    ViaimConnectSdk* sdk,
    const ViaimConnectCameraEnumeration* enumeration,
    const ViaimConnectCameraOpenOptions* options);

/**
 * @brief 同步读取当前摄像头流状态快照。
 * @param[in] sdk 有效 SDK 实例。
 * @param[out] out_state 接收一个 ViaimConnectCameraState 值的非空指针。
 * @pre sdk 尚未销毁且 out_state 非空。
 * @post 成功时写入调用瞬间状态；返回后其他线程可能立即改变实际状态。
 * @retval VIAIM_CONNECT_OK 已写入状态。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 或 out_state 无效。
 * @par 生命周期
 * 输出是独立数值，不借用 SDK 内存。
 * @par 线程与回调
 * 只读原子快照，可从摄像头回调或普通线程调用。
 * @see ViaimConnectCameraStateCallback
 * @ingroup viaim_camera
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_camera_get_state(
    ViaimConnectSdk* sdk,
    ViaimConnectCameraState* out_state);

/**
 * @brief 同步停止摄像头采集并等待本次流的采集线程与回调结束。
 * @param[in] sdk 有效 SDK 实例。
 * @pre 不能从摄像头状态或帧回调线程调用。
 * @post 成功时请求停止，在唯一采集线程完成当前回调、释放 OpenCV capture 后投递 CLOSED；
 *       返回后不再产生该次流的新摄像头回调。
 *       viaim_connect_open() 注册的 callbacks 仍保留用于下次 camera open；已 CLOSED 时幂等成功。
 * @retval VIAIM_CONNECT_OK 摄像头已同步关闭或原本已关闭。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 无效。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 从摄像头回调线程调用；流保持当前状态。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 停止采集、等待线程或 C ABI 边界发生内部异常。
 * @par 生命周期
 * 本函数不关闭 Connect HID 会话，也不销毁 sdk；统一 callbacks 的 user_data 仍须保持
 * 有效到 viaim_connect_close() 返回。随后可直接普通打开，或用新的枚举快照高级打开。
 * @par 线程与回调
 * 应从应用控制线程调用。若回调要求停止，只投递控制消息并尽快返回。
 * @see viaim_connect_camera_open()
 * @see viaim_connect_close()
 * @ingroup viaim_camera
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_camera_close(ViaimConnectSdk* sdk);

/** @} */
