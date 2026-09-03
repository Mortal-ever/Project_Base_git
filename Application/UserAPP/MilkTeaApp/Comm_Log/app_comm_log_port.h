/**
 * @file    app_comm_log_port.h
 * @brief   日志端口输出层
 *
 * 位于日志核心与传输层之间，负责：
 * - 根据编译开关分发格式化后的日志行到 UART 和/或网络输出
 * - 对每个输出独立统计成功/失败计数
 * - 提供启动前的早期轮询 UART 输出
 * - 预留未来 HTTP 网络日志接口的弱实现占位
 *
 * 架构位置：
 *   日志核心 (app_comm_log.c)
 *     → s_xBackend.pxWrite = prvPortWrite  ← 本层注册
 *       → xTransportSend() (UART 通道)       ← 通过传输层统一发送
 *       → lAppCommLogNetworkWrite() (网络)   ← 弱实现，未来 HTTP 覆盖
 *
 * 多路输出成功判定原则：
 * - 任一已开启输出成功 → 本条日志总结果为成功
 * - 所有已开启输出均失败 → 记录为后端失败，更新计数器
 * - 一个输出失败不影响另一个输出
 * - 后端失败禁止递归提交日志（否则会死循环）
 */

#ifndef APP_COMM_LOG_PORT_H
#define APP_COMM_LOG_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "app_comm_log.h"
#include "transport.h"

/**
 * @brief 按输出端口的独立统计信息
 *
 * 每个输出端口拥有独立的成功/失败计数和最后一次错误码。
 * 通过 vAppCommLogPortGetStatus() 获取线程安全的快照。
 * Keil Watch 可直接监视全局变量 g_xAppCommLogPortStatus。
 */
typedef struct {
    uint32_t ulUartSentCount;        /**< UART 端口成功发送的日志条数 */
    uint32_t ulUartFailureCount;     /**< UART 端口发送失败的日志条数 */
    uint32_t ulNetworkSentCount;     /**< 网络端口成功发送的日志条数（当前始终为 0） */
    uint32_t ulNetworkFailureCount;  /**< 网络端口发送失败的日志条数 */
    int32_t lLastUartError;          /**< 最后一次 UART 发送错误码 */
    int32_t lLastNetworkError;       /**< 最后一次网络发送错误码 */
    uint8_t ucUartEnabled;           /**< UART 输出开关是否已启用（编译期宏的镜像） */
    uint8_t ucNetworkEnabled;        /**< 网络输出开关是否已启用（编译期宏的镜像） */
} AppCommLogPortStatus_t;

/** @brief Keil Watch 可直接监视的全局状态变量 */
extern AppCommLogPortStatus_t g_xAppCommLogPortStatus;

/**
 * @brief 注册日志输出路由
 *
 * 初始化 UART 传输通道（如果 APP_COMM_LOG_UART_OUTPUT_ENABLE=1），
 * 创建 TransportChannel、打开通道，然后将 prvPortWrite 注册为日志后端的
 * 写入函数。后续 LOG 任务调用 s_xBackend.pxWrite() 即进入本层的分发逻辑。
 *
 * 调用时机：LOG 任务启动时调用一次，不可在 ISR 中调用。
 *
 * @return APP_COMM_LOG_RESULT_OK 注册成功
 *         APP_COMM_LOG_RESULT_BACKEND UART 通道创建或打开失败
 */
AppCommLogResult_e xAppCommLogPortRegister(void);

/**
 * @brief 调度器启动前的早期日志输出
 *
 * 在 FreeRTOS 调度器运行之前，通过 xTransportSend() 经传输层
 * 输出启动探针文本（如 "BOOT USART1 READY"、任务创建结果）。
 *
 * 此时 DMA 信号量和网络未就绪，传输层内部自动使用 HAL_UART_Transmit()
 * 轮询发送，不依赖信号量或 DMA 完成中断。
 *
 * @param pucData    待发送的格式化文本（可能位于 CCM）
 * @param usLength   数据长度
 * @return 0 成功，负值为传输层错误码
 */
int32_t lAppCommLogPortEarlyWrite(const uint8_t *pucData,
    uint16_t usLength);

/* Receives Debug command bytes from the full-duplex log UART. */
TransportResult_e xAppCommLogPortRead(uint8_t *pucData,
	uint16_t usMaxLength, uint16_t *pusReceivedLength,
	uint32_t ulTimeoutMs);

/* Writes one complete Debug response line through the log UART. */
int32_t lAppCommLogPortDebugWrite(const uint8_t *pucData,
	uint16_t usLength, uint32_t ulTimeoutMs);

/*
 * Writes through the registered UART by polling hardware directly.
 * This fatal-path API does not use DMA, interrupts, locks, or RTOS services.
 */
int32_t lAppCommLogPortCrashWrite(const uint8_t *pucData,
	uint16_t usLength);

/**
 * @brief 获取端口层状态快照（线程安全）
 *
 * 在临界区内完整复制 g_xAppCommLogPortStatus 到调用者提供的缓冲区。
 * 业务代码和未来 HTTP 状态接口应调用此函数，而不是直接读取全局变量。
 *
 * @param pxStatus [出] 接收状态快照的缓冲区，为 NULL 时静默返回
 */
void vAppCommLogPortGetStatus(AppCommLogPortStatus_t *pxStatus);

/**
 * @brief 网络日志输出接口（弱实现占位）
 *
 * 当前为 __weak 空实现，始终返回 APP_COMM_LOG_RESULT_NOT_READY。
 * 未来 HTTP 模块提供同名强实现后，链接器自动使用 HTTP 版本，
 * 无需修改日志核心或端口层。
 *
 * 强实现必须遵守：
 * - 返回 0 表示成功，非 0 表示失败
 * - 函数返回前完成发送或复制数据到自有的缓冲区
 * - 禁止保存 pucData 指针供函数返回后使用
 * - 禁止调用任何日志提交 API（防递归）
 * - 必须有界超时，不能无限阻塞 LOG 任务
 *
 * @param pucData    待发送数据（可能位于 CCM，不可被 DMA/网络栈长期引用）
 * @param usLength   数据长度
 * @return 0 成功，非 0 失败
 */
int32_t lAppCommLogNetworkWrite(const uint8_t *pucData,
    uint16_t usLength);

#ifdef __cplusplus
}
#endif

#endif /* APP_COMM_LOG_PORT_H */
