#pragma once

/**
 * @file Audio.h
 * @brief 音量、静音和提示音命令。
 */

#include <viaim_connect/Types.h>

/**
 * @addtogroup viaim_commands
 * @details 音频控制规则：SET_VOLUME 仅接受 SET 且至少选择一个音量字段；QUERY_VOLUME
 *          仅接受 QUERY 且 present_fields 为 0；MUTE 和 PROMPT_TONE 接受 QUERY（无
 *          字段）或 SET（必须包含 ENABLED）。响应和主动事件由 SDK 解码后交付。
 * @{
 */

enum {
    VIAIM_CONNECT_COMMAND_SET_VOLUME = 0x000D, /**< 可发送 SET；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_VOLUME = 0x000E, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_MUTE = 0x0035, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_PROMPT_TONE = 0x0042 /**< 可发送 SET/QUERY；响应可接收。 */
};

/** @brief PC 和蓝牙音量负载。 */
typedef struct ViaimConnectVolumeData {
    uint8_t pc; /**< PC 音量协议值；SDK 接受 uint8_t 全宽，设备定义具体语义。 */
    uint8_t bluetooth; /**< 蓝牙音量协议值；SDK 接受 uint8_t 全宽，设备定义具体语义。 */
} ViaimConnectVolumeData;

enum {
    VIAIM_CONNECT_VOLUME_FIELD_PC = UINT64_C(1) << 0, /**< payload.volume.pc 有效。 */
    VIAIM_CONNECT_VOLUME_FIELD_BLUETOOTH = UINT64_C(1) << 1 /**< payload.volume.bluetooth 有效。 */
};

/** @} */
