#pragma once

/**
 * @file Recording.h
 * @brief Connect 设备录音命令快捷接口。
 */

#include <viaim_connect/Types.h>

/**
 * @defgroup viaim_recording_commands 录音命令快捷接口
 * @brief 把功能模式和闪录状态作为一个连续批次排入设备命令队列。
 * @details 四个接口要求 sdk 有效且连接状态为
 *          VIAIM_CONNECT_CONNECTION_CONNECTED。每次调用先排入
 *          VIAIM_CONNECT_COMMAND_FUNCTION_MODE，再排入
 *          VIAIM_CONNECT_COMMAND_FLASH_STATUS；同一实例上的其他发送不会插入这两条命令之间。
 *
 * VIAIM_CONNECT_OK 只表示完整的两条命令已进入本地发送队列，不表示设备已开始或停止录音。
 * 设备仍分别返回两条响应，调用方必须在 ViaimConnectEventCallback 中按 command.id
 * 分别检查 protocol_result。接口可从普通业务线程或 SDK 事件回调线程调用。
 *
 * 这些接口只控制 Connect 设备，不会打开或关闭 PC 摄像头，不会筛选 media_packet/audio_pcm
 * 回调，也不会创建、写入或关闭录音文件。文件合成、落盘和摄像头联动由调用方负责。
 * @{
 */

/**
 * @brief 开始通话录音，连续发送 `0x0021 mode=0x02` 和
 *        `0x0047 status=0x01`。
 * @param[in] sdk 由 viaim_connect_create() 返回、尚未销毁且当前处于 CONNECTED
 *                状态的实例。
 *
 * @par 前置条件
 * sdk 必须有效，且 viaim_connect_get_connection_state() 当前返回
 * VIAIM_CONNECT_CONNECTION_CONNECTED。调用方不得同时销毁 sdk。
 *
 * @par 调用效果
 * SDK 先把功能模式设置为 `0x02`，再把闪录状态设置为 `0x01`；只有完整的两条编码结果都可用
 * 且当前会话能够接收批次时才会入队。VIAIM_CONNECT_OK 不等待设备响应，也不创建主机端录音文件。
 *
 * @par 异步结果
 * 设备会分别产生 VIAIM_CONNECT_COMMAND_FUNCTION_MODE 和
 * VIAIM_CONNECT_COMMAND_FLASH_STATUS 响应。两条事件都必须检查
 * ViaimConnectEvent::protocol_result；任意一条失败都表示设备侧流程未完整成功。
 *
 * @par 线程与回调
 * 可从普通业务线程或 ViaimConnectEventCallback 调用。多个线程并发发送时，本调用的两条命令
 * 仍保持相邻；函数返回后调用方可以继续发送其他业务命令。
 *
 * @code{.c}
 * static void __cdecl on_recording_event(
 *     const ViaimConnectEvent* event, void* user_data) {
 *     (void)user_data;
 *     if (event->command.id == VIAIM_CONNECT_COMMAND_FUNCTION_MODE ||
 *         event->command.id == VIAIM_CONNECT_COMMAND_FLASH_STATUS) {
 *         if (event->protocol_result == 0) {
 *             // 当前这一条设备命令执行成功；仍需等待另一条响应。
 *         } else {
 *             // 记录设备业务失败，并按产品策略停止或重试。
 *         }
 *     }
 * }
 *
 * ViaimConnectResult queued =
 *     viaim_connect_start_call_recording(sdk);
 * if (queued != VIAIM_CONNECT_OK) {
 *     // 命令批次没有完整进入本地发送队列。
 * }
 * @endcode
 *
 * @retval VIAIM_CONNECT_OK 两条命令已作为一个连续批次排入本地发送队列。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 为空或不是有效实例。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 当前没有可接收完整批次的 CONNECTED 会话。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 发生未预期的内部异常；错误回调可能提供详情。
 * @see ViaimConnectEventCallback
 * @see viaim_connect_stop_recording()
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_start_call_recording(ViaimConnectSdk* sdk);

/**
 * @brief 开始扬声器录音，连续发送 `0x0021 mode=0x00` 和
 *        `0x0047 status=0x01`。
 * @param[in] sdk 由 viaim_connect_create() 返回、尚未销毁且当前处于 CONNECTED
 *                状态的实例。
 *
 * @par 前置条件
 * sdk 必须有效，且 viaim_connect_get_connection_state() 当前返回
 * VIAIM_CONNECT_CONNECTION_CONNECTED。调用方不得同时销毁 sdk。
 *
 * @par 调用效果
 * SDK 先把功能模式设置为 `0x00`，再把闪录状态设置为 `0x01`；两条命令作为不可插入的连续批次
 * 入队。成功返回不等待设备执行，不打开摄像头，不筛选 PCM，也不创建录音文件。
 *
 * @par 异步结果
 * 分别等待 VIAIM_CONNECT_COMMAND_FUNCTION_MODE 和
 * VIAIM_CONNECT_COMMAND_FLASH_STATUS 响应，并逐条检查
 * ViaimConnectEvent::protocol_result；不能把第一条成功当作整个录音流程成功。
 *
 * @par 线程与回调
 * 可从普通业务线程或 ViaimConnectEventCallback 调用。并发发送不会插入本批次的两条命令之间。
 *
 * @retval VIAIM_CONNECT_OK 两条命令已作为一个连续批次排入本地发送队列。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 为空或不是有效实例。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 当前没有可接收完整批次的 CONNECTED 会话。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 发生未预期的内部异常；错误回调可能提供详情。
 * @see ViaimConnectEventCallback
 * @see viaim_connect_stop_recording()
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_start_speaker_recording(ViaimConnectSdk* sdk);

/**
 * @brief 开始麦克风录音，连续发送 `0x0021 mode=0x01` 和
 *        `0x0047 status=0x01`。
 * @param[in] sdk 由 viaim_connect_create() 返回、尚未销毁且当前处于 CONNECTED
 *                状态的实例。
 *
 * @par 前置条件
 * sdk 必须有效，且 viaim_connect_get_connection_state() 当前返回
 * VIAIM_CONNECT_CONNECTION_CONNECTED。调用方不得同时销毁 sdk。
 *
 * @par 调用效果
 * SDK 先把功能模式设置为 `0x01`，再把闪录状态设置为 `0x01`；两条命令作为不可插入的连续批次
 * 入队。成功返回不等待设备执行，不打开摄像头，不筛选 PCM，也不创建录音文件。
 *
 * @par 异步结果
 * 分别等待 VIAIM_CONNECT_COMMAND_FUNCTION_MODE 和
 * VIAIM_CONNECT_COMMAND_FLASH_STATUS 响应，并逐条检查
 * ViaimConnectEvent::protocol_result；不能把第一条成功当作整个录音流程成功。
 *
 * @par 线程与回调
 * 可从普通业务线程或 ViaimConnectEventCallback 调用。并发发送不会插入本批次的两条命令之间。
 *
 * @retval VIAIM_CONNECT_OK 两条命令已作为一个连续批次排入本地发送队列。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 为空或不是有效实例。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 当前没有可接收完整批次的 CONNECTED 会话。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 发生未预期的内部异常；错误回调可能提供详情。
 * @see ViaimConnectEventCallback
 * @see viaim_connect_stop_recording()
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_start_microphone_recording(ViaimConnectSdk* sdk);

/**
 * @brief 停止录音，连续发送 `0x0021 mode=0x06` 和
 *        `0x0047 status=0x00`。
 * @param[in] sdk 由 viaim_connect_create() 返回、尚未销毁且当前处于 CONNECTED
 *                状态的实例。
 *
 * @par 前置条件
 * sdk 必须有效，且 viaim_connect_get_connection_state() 当前返回
 * VIAIM_CONNECT_CONNECTION_CONNECTED。调用方不得同时销毁 sdk。
 *
 * @par 调用效果
 * SDK 先把功能模式设置为 `0x06`，再把闪录状态设置为 `0x00`；两条命令作为不可插入的连续批次
 * 入队。成功返回不等待设备真正停止，也不会替调用方停止摄像头、关闭媒体回调或关闭录音文件。
 *
 * @par 异步结果
 * 分别等待 VIAIM_CONNECT_COMMAND_FUNCTION_MODE 和
 * VIAIM_CONNECT_COMMAND_FLASH_STATUS 响应，并逐条检查
 * ViaimConnectEvent::protocol_result。调用方应在确认所需设备响应后，再按自己的流程结束媒体落盘。
 *
 * @par 线程与回调
 * 可从普通业务线程或 ViaimConnectEventCallback 调用。并发发送不会插入本批次的两条命令之间；
 * 需要关闭 SDK 时，仍应由非回调线程完成 viaim_connect_close() 和 viaim_connect_destroy()。
 *
 * @retval VIAIM_CONNECT_OK 两条命令已作为一个连续批次排入本地发送队列。
 * @retval VIAIM_CONNECT_ERROR_INVALID_ARGUMENT sdk 为空或不是有效实例。
 * @retval VIAIM_CONNECT_ERROR_INVALID_STATE 当前没有可接收完整批次的 CONNECTED 会话。
 * @retval VIAIM_CONNECT_ERROR_INTERNAL 发生未预期的内部异常；错误回调可能提供详情。
 * @see ViaimConnectEventCallback
 * @see viaim_connect_start_call_recording()
 * @see viaim_connect_start_speaker_recording()
 * @see viaim_connect_start_microphone_recording()
 */
VIAIM_CONNECT_EXTERN_C VIAIM_CONNECT_API ViaimConnectResult __cdecl
viaim_connect_stop_recording(ViaimConnectSdk* sdk);

/** @} */
