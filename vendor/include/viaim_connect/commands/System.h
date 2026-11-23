#pragma once

/**
 * @file System.h
 * @brief 按键、蓝牙名称、系统模式、OTA、灯光、心跳和电源命令。
 */

#include <viaim_connect/Types.h>

/**
 * @addtogroup viaim_commands
 * @details 系统命令规则：KEY_REPORT、OTA_REPORT、HEARTBEAT 只接收 REPORT；
 *          RESTORE_FACTORY、START_OTA 接受无字段 EXECUTE；SET/QUERY 名称命令按名称
 *          分离；FUNCTION_MODE、灯光、SCHEDULED_SHUTDOWN、FLASH_STATUS、两个时长命令
 *          接受 SET/QUERY；SYSTEM_POWER 接受 EXECUTE。查询不携带字段。
 * @{
 */

enum {
    VIAIM_CONNECT_COMMAND_KEY_REPORT = 0x000A, /**< 仅接收设备 REPORT；不可发送。 */
    VIAIM_CONNECT_COMMAND_SET_BLUETOOTH_NAME = 0x0012, /**< 可发送 SET；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_BLUETOOTH_NAME = 0x0013, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_FUNCTION_MODE = 0x0021, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_RESTORE_FACTORY = 0x002F, /**< 可发送无字段 EXECUTE；响应可接收。 */
    VIAIM_CONNECT_COMMAND_START_OTA = 0x0031, /**< 可发送无字段 EXECUTE；响应可接收。 */
    VIAIM_CONNECT_COMMAND_OTA_REPORT = 0x0032, /**< 仅接收设备 REPORT；不可发送。 */
    VIAIM_CONNECT_COMMAND_TOUCHPAD_LIGHT = 0x0036, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_RING_LIGHT = 0x0043, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_SCHEDULED_SHUTDOWN = 0x0046, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_FLASH_STATUS = 0x0047, /**< 可发送 SET/QUERY，也可接收状态事件。 */
    VIAIM_CONNECT_COMMAND_HEARTBEAT = 0x004A, /**< 仅接收设备 REPORT；SDK 自动回复。 */
    VIAIM_CONNECT_COMMAND_FLASH_DURATION = 0x004C, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_FLASH_SILENT_EXIT = 0x004D, /**< 可发送 SET/QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_SYSTEM_POWER = 0x0050 /**< 可发送 EXECUTE；响应可接收。 */
};

/** @brief 按键上报负载。 */
typedef struct ViaimConnectKeyData {
    uint8_t key; /**< 按键编号。 */
    uint8_t action; /**< 按键动作值。 */
} ViaimConnectKeyData;

enum {
    VIAIM_CONNECT_KEY_FIELD_KEY = UINT64_C(1) << 0, /**< payload.key.key 有效。 */
    VIAIM_CONNECT_KEY_FIELD_ACTION = UINT64_C(1) << 1 /**< payload.key.action 有效。 */
};

/** @brief 设备功能模式负载。 */
typedef struct ViaimConnectFunctionModeData {
    uint8_t mode; /**< 功能模式值；发送 SET 时 SDK 接受 0..6。 */
} ViaimConnectFunctionModeData;

enum {
    VIAIM_CONNECT_FUNCTION_MODE_FIELD_MODE = UINT64_C(1) << 0 /**< payload.function_mode.mode 有效。 */
};

/** @brief OTA 状态上报负载。 */
typedef struct ViaimConnectOtaData {
    uint8_t progress; /**< OTA 进度百分比；正常设备值为 0..100。 */
    uint8_t result; /**< OTA 结果值。 */
    uint8_t failure_reason; /**< OTA 失败原因值。 */
} ViaimConnectOtaData;

enum {
    VIAIM_CONNECT_OTA_FIELD_PROGRESS = UINT64_C(1) << 0, /**< ota.progress 有效。 */
    VIAIM_CONNECT_OTA_FIELD_RESULT = UINT64_C(1) << 1, /**< ota.result 有效。 */
    VIAIM_CONNECT_OTA_FIELD_FAILURE_REASON = UINT64_C(1) << 2 /**< ota.failure_reason 有效。 */
};

/** @brief 定时关机负载。 */
typedef struct ViaimConnectScheduledShutdownData {
    uint8_t minutes; /**< 距离关机的分钟数；0 的含义由设备协议定义。 */
} ViaimConnectScheduledShutdownData;

enum {
    VIAIM_CONNECT_SCHEDULED_SHUTDOWN_FIELD_MINUTES = UINT64_C(1) << 0 /**< scheduled_shutdown.minutes 有效，单位分钟。 */
};

/** @brief 闪录状态上报负载。 */
typedef struct ViaimConnectFlashData {
    uint8_t source_attribute; /**< 触发本次报告的原始 TLV 属性 ID。 */
    uint8_t status; /**< 闪录状态；发送 SET 时仅接受 0 或 1。 */
    uint32_t timestamp; /**< 设备协议原始时间戳；SDK 不换算单位。 */
    uint8_t function_mode; /**< 报告中的设备功能模式。 */
} ViaimConnectFlashData;

enum {
    VIAIM_CONNECT_FLASH_FIELD_STATUS = UINT64_C(1) << 0, /**< flash.status 有效。 */
    VIAIM_CONNECT_FLASH_FIELD_TIMESTAMP = UINT64_C(1) << 1, /**< flash.timestamp 有效。 */
    VIAIM_CONNECT_FLASH_FIELD_FUNCTION_MODE = UINT64_C(1) << 2 /**< flash.function_mode 有效。 */
};

/** @brief 心跳负载。 */
typedef struct ViaimConnectHeartbeatData {
    uint8_t attribute_id; /**< 心跳原始 TLV 属性 ID。 */
    uint32_t sequence; /**< 心跳序号。 */
    uint8_t interval; /**< 设备协议中的心跳间隔值。 */
} ViaimConnectHeartbeatData;

enum {
    VIAIM_CONNECT_HEARTBEAT_FIELD_ATTRIBUTE_ID = UINT64_C(1) << 0, /**< heartbeat.attribute_id 有效。 */
    VIAIM_CONNECT_HEARTBEAT_FIELD_SEQUENCE = UINT64_C(1) << 1, /**< heartbeat.sequence 有效。 */
    VIAIM_CONNECT_HEARTBEAT_FIELD_INTERVAL = UINT64_C(1) << 2 /**< heartbeat.interval 有效。 */
};

/** @brief 系统电源动作负载。 */
typedef struct ViaimConnectSystemPowerData {
    uint8_t action; /**< 电源动作值；SDK 接受 0..2。 */
} ViaimConnectSystemPowerData;

enum {
    VIAIM_CONNECT_SYSTEM_POWER_FIELD_ACTION = UINT64_C(1) << 0 /**< payload.system_power.action 有效。 */
};

/** @} */
