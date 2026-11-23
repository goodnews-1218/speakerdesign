#pragma once

/**
 * @file Interaction.h
 * @brief 唤醒、手势和声纹交互命令。
 */

#include <viaim_connect/Types.h>

/**
 * @addtogroup viaim_commands
 * @details 交互命令规则：WAKEUP、GESTURE、VOICEPRINT_SWITCH 接受 SET/QUERY；
 *          VOICEPRINT_REGISTRATION 和 VOICEPRINT_DELETE 接受 EXECUTE；
 *          QUERY_VOICEPRINTS 接受 QUERY；WAKEUP_LANGUAGE 接受 SET。发送字符串视图时
 *          SDK 在 send 返回前复制；事件中的名称数组只在当前回调期间有效。
 * @{
 */

enum {
    VIAIM_CONNECT_COMMAND_WAKEUP = 0x002D, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_GESTURE = 0x0034, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_VOICEPRINT_REGISTRATION = 0x0038, /**< 可发送 EXECUTE；响应/结果可接收。 */
    VIAIM_CONNECT_COMMAND_VOICEPRINT_DELETE = 0x0039, /**< 可发送 EXECUTE；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_VOICEPRINTS = 0x003A, /**< 可发送 QUERY；分段响应可接收。 */
    VIAIM_CONNECT_COMMAND_WAKEUP_LANGUAGE = 0x003F, /**< 可发送 SET；响应可接收。 */
    VIAIM_CONNECT_COMMAND_VOICEPRINT_SWITCH = 0x0045 /**< 可发送 SET/QUERY；响应可接收。 */
};

/** @brief 手势识别模式和开关负载。 */
typedef struct ViaimConnectGestureData {
    uint8_t mode; /**< 单模式设置时的手势模式编号。 */
    uint8_t enabled; /**< mode 对应的开关：0 关闭，1 开启。 */
    uint8_t mode_1_enabled; /**< 手势模式 1 开关：0 关闭，1 开启。 */
    uint8_t mode_2_enabled; /**< 手势模式 2 开关：0 关闭，1 开启。 */
    uint8_t mode_3_enabled; /**< 手势模式 3 开关：0 关闭，1 开启。 */
    uint8_t mode_4_enabled; /**< 手势模式 4 开关：0 关闭，1 开启。 */
} ViaimConnectGestureData;

enum {
    VIAIM_CONNECT_GESTURE_FIELD_MODE = UINT64_C(1) << 0, /**< gesture.mode 有效。 */
    VIAIM_CONNECT_GESTURE_FIELD_ENABLED = UINT64_C(1) << 1, /**< gesture.enabled 有效。 */
    VIAIM_CONNECT_GESTURE_FIELD_MODE_1 = UINT64_C(1) << 2, /**< mode_1_enabled 有效。 */
    VIAIM_CONNECT_GESTURE_FIELD_MODE_2 = UINT64_C(1) << 3, /**< mode_2_enabled 有效。 */
    VIAIM_CONNECT_GESTURE_FIELD_MODE_3 = UINT64_C(1) << 4, /**< mode_3_enabled 有效。 */
    VIAIM_CONNECT_GESTURE_FIELD_MODE_4 = UINT64_C(1) << 5 /**< mode_4_enabled 有效。 */
};

/** @brief 声纹注册动作类型。 */
typedef int32_t ViaimConnectVoiceprintRegistrationAction;
enum {
    VIAIM_CONNECT_VOICEPRINT_REGISTRATION_START = 1, /**< 开始注册。 */
    VIAIM_CONNECT_VOICEPRINT_REGISTRATION_STOP = 2 /**< 停止注册。 */
};

/**
 * @brief 声纹注册请求或结果负载。
 *
 * name 在发送请求时由 SDK 于函数返回前复制；在事件中仅于当前回调期间有效。
 */
typedef struct ViaimConnectVoiceprintRegistrationData {
    ViaimConnectVoiceprintRegistrationAction action; /**< 注册动作。 */
    ViaimConnectStringView name; /**< 声纹名称 UTF-8 视图。 */
    uint8_t result; /**< 设备返回的声纹注册结果值。 */
} ViaimConnectVoiceprintRegistrationData;

enum {
    VIAIM_CONNECT_VOICEPRINT_REGISTRATION_FIELD_ACTION = UINT64_C(1) << 0, /**< registration.action 有效。 */
    VIAIM_CONNECT_VOICEPRINT_REGISTRATION_FIELD_NAME = UINT64_C(1) << 1, /**< registration.name 有效。 */
    VIAIM_CONNECT_VOICEPRINT_REGISTRATION_FIELD_RESULT = UINT64_C(1) << 2 /**< registration.result 有效。 */
};

/** @brief 声纹删除动作类型。 */
typedef int32_t ViaimConnectVoiceprintDeleteAction;
enum {
    VIAIM_CONNECT_VOICEPRINT_DELETE_ALL = 0, /**< 删除全部声纹。 */
    VIAIM_CONNECT_VOICEPRINT_DELETE_ONE = 1 /**< 按名称删除一个声纹。 */
};

/**
 * @brief 声纹删除请求负载。
 *
 * name 仅在 VIAIM_CONNECT_VOICEPRINT_DELETE_ONE 时使用，并由 SDK 在发送函数
 * 返回前复制。
 */
typedef struct ViaimConnectVoiceprintDeleteData {
    ViaimConnectVoiceprintDeleteAction action; /**< 删除动作。 */
    ViaimConnectStringView name; /**< 要删除的声纹名称 UTF-8 视图。 */
} ViaimConnectVoiceprintDeleteData;

enum {
    VIAIM_CONNECT_VOICEPRINT_DELETE_FIELD_ACTION = UINT64_C(1) << 0, /**< voiceprint_delete.action 有效。 */
    VIAIM_CONNECT_VOICEPRINT_DELETE_FIELD_NAME = UINT64_C(1) << 1 /**< voiceprint_delete.name 有效。 */
};

/** @brief 分帧声纹列表中的内容类型。 */
typedef int32_t ViaimConnectVoiceprintListPart;
enum {
    VIAIM_CONNECT_VOICEPRINT_LIST_COUNT = 1, /**< 当前事件提供声明总数。 */
    VIAIM_CONNECT_VOICEPRINT_LIST_ITEM = 2 /**< 当前事件提供名称项。 */
};

/**
 * @brief 一帧声纹列表响应。
 *
 * SDK 不跨事件聚合声纹列表。names 及每个字符串视图仅在当前事件回调期间
 * 有效，调用方需要自行按需复制和聚合。
 */
typedef struct ViaimConnectVoiceprintListData {
    ViaimConnectVoiceprintListPart part; /**< 当前事件携带的列表内容类型。 */
    uint8_t declared_count; /**< 设备声明的声纹名称总数。 */
    const ViaimConnectStringView* names; /**< 当前事件中的名称视图数组。 */
    size_t name_count; /**< names 数组的元素数量。 */
} ViaimConnectVoiceprintListData;

enum {
    VIAIM_CONNECT_VOICEPRINT_LIST_FIELD_PART = UINT64_C(1) << 0, /**< voiceprint_list.part 有效。 */
    VIAIM_CONNECT_VOICEPRINT_LIST_FIELD_DECLARED_COUNT = UINT64_C(1) << 1, /**< declared_count 有效。 */
    VIAIM_CONNECT_VOICEPRINT_LIST_FIELD_NAMES = UINT64_C(1) << 2 /**< names/name_count 有效。 */
};

/** @brief 语音唤醒语言负载。 */
typedef struct ViaimConnectWakeupLanguageData {
    uint8_t language; /**< 语言协议值；SDK 接受 uint8_t 全宽。 */
} ViaimConnectWakeupLanguageData;

enum {
    VIAIM_CONNECT_WAKEUP_LANGUAGE_FIELD_LANGUAGE = UINT64_C(1) << 0 /**< payload.wakeup_language.language 有效。 */
};

/** @} */
