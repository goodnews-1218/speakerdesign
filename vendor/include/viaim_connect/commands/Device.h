#pragma once

/**
 * @file Device.h
 * @brief 设备标识、电池、版本、时间、存储和握手命令。
 */

#include <viaim_connect/Types.h>

/**
 * @addtogroup viaim_commands
 * @details 设备信息命令规则：QUERY_* 仅接受 QUERY 且不带字段；SET_UTC 仅接受 SET
 *          和 seconds；HANDSHAKE 仅接受 EXECUTE 且不带字段。BATTERY_REPORT 只由设备
 *          主动上报，不能发送。响应中的 present_fields 指明实际存在的属性，字节/字符串
 *          视图只在当前事件回调期间有效。
 * @{
 */

enum {
    VIAIM_CONNECT_COMMAND_QUERY_MAC = 0x0007, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_BVCT = 0x000B, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_SOFTWARE_VERSION = 0x000C, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_HARDWARE_VERSION = 0x0019, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_SERIAL_NUMBER = 0x001B, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_SET_UTC = 0x001E, /**< 可发送 SET；响应可接收。 */
    VIAIM_CONNECT_COMMAND_QUERY_STORAGE = 0x0030, /**< 可发送 QUERY；响应可接收。 */
    VIAIM_CONNECT_COMMAND_BATTERY_REPORT = 0x0041, /**< 仅接收设备 REPORT；不可发送。 */
    VIAIM_CONNECT_COMMAND_HANDSHAKE = 0x0044 /**< 可发送 EXECUTE；响应可接收。 */
};

/** @brief 设备 MAC 地址负载。 */
typedef struct ViaimConnectMacData {
    uint8_t bytes[6]; /**< 按设备协议顺序排列的 6 字节 MAC 地址。 */
} ViaimConnectMacData;

enum {
    VIAIM_CONNECT_MAC_FIELD_VALUE = UINT64_C(1) << 0 /**< payload.mac.bytes 有效。 */
};

/** @brief 电池、电压、电流和温度（BVCT）负载。 */
typedef struct ViaimConnectBvctData {
    uint8_t battery_percent; /**< 剩余电量百分比，范围 0..100。 */
    uint16_t voltage_mv; /**< 电池电压，单位为毫伏。 */
    int16_t current_ma; /**< 电池电流，单位为毫安，可为负值。 */
    uint16_t temperature_raw; /**< 设备协议中的原始温度值。 */
} ViaimConnectBvctData;

enum {
    VIAIM_CONNECT_BVCT_FIELD_BATTERY_PERCENT = UINT64_C(1) << 0, /**< battery_percent 有效。 */
    VIAIM_CONNECT_BVCT_FIELD_VOLTAGE_MV = UINT64_C(1) << 1, /**< voltage_mv 有效。 */
    VIAIM_CONNECT_BVCT_FIELD_CURRENT_MA = UINT64_C(1) << 2, /**< current_ma 有效。 */
    VIAIM_CONNECT_BVCT_FIELD_TEMPERATURE_RAW = UINT64_C(1) << 3 /**< temperature_raw 有效。 */
};

/**
 * @brief 各设备模块的软件版本负载。
 *
 * 所有字节视图均借用事件内存，只在当前事件回调期间有效。
 */
typedef struct ViaimConnectSoftwareVersionData {
    ViaimConnectBytesView device; /**< 主设备软件版本字节。 */
    ViaimConnectBytesView bluetooth; /**< 蓝牙模块软件版本字节。 */
    ViaimConnectBytesView wakeup_chip; /**< 唤醒芯片软件版本字节。 */
    ViaimConnectBytesView pd; /**< USB Power Delivery 模块软件版本字节。 */
} ViaimConnectSoftwareVersionData;

enum {
    VIAIM_CONNECT_SOFTWARE_VERSION_FIELD_DEVICE = UINT64_C(1) << 0, /**< software_version.device 有效。 */
    VIAIM_CONNECT_SOFTWARE_VERSION_FIELD_BLUETOOTH = UINT64_C(1) << 1, /**< software_version.bluetooth 有效。 */
    VIAIM_CONNECT_SOFTWARE_VERSION_FIELD_WAKEUP_CHIP = UINT64_C(1) << 2, /**< software_version.wakeup_chip 有效。 */
    VIAIM_CONNECT_SOFTWARE_VERSION_FIELD_PD = UINT64_C(1) << 3 /**< software_version.pd 有效。 */
};

/**
 * @brief 设备硬件版本负载。
 *
 * 字符串视图借用事件内存，只在当前事件回调期间有效。
 */
typedef struct ViaimConnectHardwareVersionData {
    ViaimConnectStringView bluetooth_board; /**< 蓝牙板硬件版本 UTF-8 文本。 */
    ViaimConnectStringView application_board; /**< 应用板硬件版本 UTF-8 文本。 */
} ViaimConnectHardwareVersionData;

enum {
    VIAIM_CONNECT_HARDWARE_VERSION_FIELD_BLUETOOTH_BOARD =
        UINT64_C(1) << 0, /**< hardware_version.bluetooth_board 有效。 */
    VIAIM_CONNECT_HARDWARE_VERSION_FIELD_APPLICATION_BOARD =
        UINT64_C(1) << 1 /**< hardware_version.application_board 有效。 */
};

/** @brief UTC 时间负载。 */
typedef struct ViaimConnectUtcData {
    uint32_t seconds; /**< 设备协议使用的 UTC 秒数。 */
} ViaimConnectUtcData;

enum {
    VIAIM_CONNECT_UTC_FIELD_SECONDS = UINT64_C(1) << 0 /**< payload.utc.seconds 有效。 */
};

/** @brief 设备存储容量负载。 */
typedef struct ViaimConnectStorageData {
    uint64_t available_bytes; /**< 当前可用空间，单位为字节。 */
    uint64_t total_bytes; /**< 总空间，单位为字节。 */
} ViaimConnectStorageData;

enum {
    VIAIM_CONNECT_STORAGE_FIELD_AVAILABLE_BYTES = UINT64_C(1) << 0, /**< available_bytes 有效。 */
    VIAIM_CONNECT_STORAGE_FIELD_TOTAL_BYTES = UINT64_C(1) << 1 /**< total_bytes 有效。 */
};

/** @brief 设备主动上报的电池状态。 */
typedef struct ViaimConnectBatteryStatusData {
    uint8_t charging; /**< 充电状态：0 否，1 是。 */
    uint8_t low_power; /**< 低电量状态：0 否，1 是。 */
    uint8_t battery_percent; /**< 剩余电量百分比，范围 0..100。 */
    uint8_t discharging; /**< 放电状态：0 否，1 是。 */
} ViaimConnectBatteryStatusData;

enum {
    VIAIM_CONNECT_BATTERY_STATUS_FIELD_CHARGING = UINT64_C(1) << 0, /**< charging 有效。 */
    VIAIM_CONNECT_BATTERY_STATUS_FIELD_LOW_POWER = UINT64_C(1) << 1, /**< low_power 有效。 */
    VIAIM_CONNECT_BATTERY_STATUS_FIELD_PERCENT = UINT64_C(1) << 2, /**< battery_percent 有效。 */
    VIAIM_CONNECT_BATTERY_STATUS_FIELD_DISCHARGING = UINT64_C(1) << 3 /**< discharging 有效。 */
};

/**
 * @brief 建立连接时返回的设备状态快照。
 *
 * serial_number 借用事件内存，只在当前事件回调期间有效。
 */
typedef struct ViaimConnectHandshakeData {
    uint8_t system_status; /**< 设备系统状态值。 */
    uint8_t voiceprint_status; /**< 声纹模块状态值。 */
    uint8_t lens_state; /**< 镜头盖状态值。 */
    uint8_t pc_volume; /**< PC 音量值。 */
    uint8_t battery_percent; /**< 剩余电量百分比，范围 0..100。 */
    uint8_t charging; /**< 充电状态：0 否，1 是。 */
    uint8_t low_power; /**< 低电量状态：0 否，1 是。 */
    uint8_t wakeup_enabled; /**< 唤醒开关：0 关闭，1 开启。 */
    uint8_t voiceprint_enabled; /**< 声纹开关：0 关闭，1 开启。 */
    uint64_t remaining_space; /**< 设备剩余空间，单位为字节。 */
    uint8_t version[4]; /**< 设备协议定义的 4 字节版本值。 */
    ViaimConnectStringView serial_number; /**< 设备序列号 UTF-8 视图。 */
} ViaimConnectHandshakeData;

enum {
    VIAIM_CONNECT_HANDSHAKE_FIELD_SYSTEM_STATUS = UINT64_C(1) << 0, /**< system_status 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_VOICEPRINT_STATUS = UINT64_C(1) << 1, /**< voiceprint_status 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_LENS_STATE = UINT64_C(1) << 2, /**< lens_state 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_PC_VOLUME = UINT64_C(1) << 3, /**< pc_volume 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_BATTERY_PERCENT = UINT64_C(1) << 4, /**< battery_percent 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_CHARGING = UINT64_C(1) << 5, /**< charging 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_LOW_POWER = UINT64_C(1) << 6, /**< low_power 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_WAKEUP = UINT64_C(1) << 7, /**< wakeup_enabled 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_VOICEPRINT = UINT64_C(1) << 8, /**< voiceprint_enabled 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_REMAINING_SPACE = UINT64_C(1) << 9, /**< remaining_space 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_VERSION = UINT64_C(1) << 10, /**< version 有效。 */
    VIAIM_CONNECT_HANDSHAKE_FIELD_SERIAL_NUMBER = UINT64_C(1) << 11 /**< serial_number 有效。 */
};

/** @} */
