/**
 * @file    app_comm_log_port.c
 * @brief   日志端口输出层实现
 *
 * 位于日志核心与传输层之间，负责按编译开关将格式化后的日志行分发给
 * UART（通过 xTransportSend）和/或网络（通过 lAppCommLogNetworkWrite）。
 *
 * 当前调用链（新架构，2026-07-23）：
 *   LOG 任务 (app_comm_log.c)
 *     → s_xBackend.pxWrite = prvPortWrite  ← 本文件注册
 *       → xTransportSend(&s_xLogUartChannel, pucData, usLength, timeout)
 *           → transport_uart.c 内部根据 hdmatx 自动选择 DMA 或轮询发送
 *       → lAppCommLogNetworkWrite()  ← __weak 占位，未来 HTTP 覆盖
 *
 * 多路输出成功判定原则：
 * - 任一已开启输出成功 → 本条日志总结果为成功
 * - 所有输出均失败 → 记录总失败
 * - 后端失败禁止递归提交日志（防止死循环）
 *
 * 切换日志串口：
 * - 只需修改本文件第 11 行的 s_pxLogUart 指向的 UART 句柄（如 &huart3）
 * - 传输层内部会自动根据 CubeMX 配置选择 DMA 或轮询
 * - 无需修改日志核心、格式化和队列
 */

#include "app_comm_log_port.h"

#include <string.h>

#include "app_crash_diag.h"
#include "app_crash_diag_config.h"
#include "stm32f4xx_ll_usart.h"
#include "task.h"
#include "transport.h"
#include "transport_uart.h"
#include "usart.h"

/**
 * @name UART 日志输出通道
 *
 * 日志专用的 Transport 通道，使用 "log_uart" 作为唯一名称。
 * 通过 xTransportSend() 统一发送，不直接调用 HAL UART 函数。
 *
 * UART 发送模式由传输层根据 huart->hdmatx 自动决定：
 * - 有 DMA → memcpy 到 SRAM 缓冲区 → HAL_UART_Transmit_DMA() → 信号量等待
 * - 无 DMA → HAL_UART_Transmit() 轮询（阻塞，CPU 搬运）
 *
 * @{
 */
#if (APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U)
static UART_HandleTypeDef * const s_pxLogUart = &huart1;  /**< 日志串口号配置：修改此指针即可切换日志串口
                                                                可选值：&huart1 ~ &huart6 */
static TransportChannel_t s_xLogUartChannel;               /**< 传输层日志 UART 通道句柄 */
static TransportUartContext_t s_xLogUartContext;           /**< 传输层 UART 上下文（含 DMA 中转区和信号量） */
#endif
/** @} */

/** @brief 日志各输出端口独立计数（Keil Watch 可监视） */
AppCommLogPortStatus_t g_xAppCommLogPortStatus;

/* ------------------- 静态函数前置声明 ------------------- */
static int32_t prvPortWrite(void *pvContext, const uint8_t *pucData,
	uint16_t usLength);

/**
 * @brief 注册日志输出路由
 *
 * 在 LOG 任务启动时调用一次。执行以下操作：
 * 1. 零化各端口统计计数器
 * 2. 根据编译开关记录哪个端口已启用
 * 3. 如果 UART 输出开启 → 创建 TransportUart 通道（名称为 "log_uart"）
 *    通道配置为仅发送（ucReceiveEnabled=0），然后打开通道
 * 4. 如果 UART 通道创建/打开失败 → 记录错误并返回 BACKEND
 * 5. 将 prvPortWrite 注册为日志核心的后端写入函数
 *
 * @return APP_COMM_LOG_RESULT_OK 注册成功
 *         APP_COMM_LOG_RESULT_BACKEND UART 通道创建或打开失败
 */
/*-----------------------------------------------------------*/
AppCommLogResult_e xAppCommLogPortRegister(void)
{
	AppCommLogBackend_t xBackend;
#if (APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U)
	TransportUartConfig_t xUartConfig;
	TransportResult_e xTransportResult;
#endif

	/* 清零各端口统计计数器 */
	memset(&g_xAppCommLogPortStatus, 0,
		sizeof(g_xAppCommLogPortStatus));

	/* 记录编译开关镜像到运行期变量（Keil Watch 可查看） */
	g_xAppCommLogPortStatus.ucUartEnabled =
		(uint8_t)APP_COMM_LOG_UART_OUTPUT_ENABLE;
	g_xAppCommLogPortStatus.ucNetworkEnabled =
		(uint8_t)APP_COMM_LOG_NETWORK_OUTPUT_ENABLE;

#if (APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U)
	/* 配置 UART 传输通道（仅发送，不接收） */
	memset(&xUartConfig, 0, sizeof(xUartConfig));
	xUartConfig.pxUart = s_pxLogUart;
	xUartConfig.ucReceiveEnabled = 1U;

	/* 创建并打开传输层通道 */
	xTransportResult = xTransportUartCreate(&s_xLogUartChannel,
		&s_xLogUartContext, "log_uart", &xUartConfig);
	if (xTransportResult == TRANSPORT_RESULT_OK) {
		xTransportResult = xTransportOpen(&s_xLogUartChannel);
	}

	/* 通道创建或打开失败 → 记录错误并返回 */
	if (xTransportResult != TRANSPORT_RESULT_OK) {
		g_xAppCommLogPortStatus.lLastUartError =
			(int32_t)xTransportResult;
		g_xAppCommLogPortStatus.ulUartFailureCount++;
		return APP_COMM_LOG_RESULT_BACKEND;
	}
#endif

	/* 注册 prvPortWrite 为日志核心的后端写入函数 */
	xBackend.pxWrite = prvPortWrite;
	xBackend.pvContext = NULL;
	return xAppCommLogRegisterBackend(&xBackend);
}

/**
 * @brief 调度器启动前的早期日志输出
 *
 * 在 FreeRTOS 调度器运行之前，通过 xTransportSend() 输出启动探针文本。
 * 传输层内部检测到调度器未运行，自动使用 HAL_UART_Transmit() 轮询发送，
 * 不依赖 DMA 信号量。
 *
 * 典型输出文本示例：
 * - "BOOT USART1 READY"  ← 硬件探针
 * - "Task: LOG CREATED OK"  ← 任务创建结果
 *
 * @param pucData    待发送的格式化日志文本（可能来自 CCM 任务栈）
 * @param usLength   数据长度（字节）
 * @return 0 成功，负值为传输层错误码
 */
/*-----------------------------------------------------------*/
int32_t lAppCommLogPortEarlyWrite(const uint8_t *pucData,
	uint16_t usLength)
{
#if (APP_COMM_LOG_EARLY_UART_ENABLE != 0U)
	return (int32_t)xTransportSend(&s_xLogUartChannel, pucData, usLength,
		APP_COMM_LOG_UART_TIMEOUT_MS);
#else
	(void)pucData;
	(void)usLength;
	return 0;
#endif
}

/*-----------------------------------------------------------*/
TransportResult_e xAppCommLogPortRead(uint8_t *pucData,
	uint16_t usMaxLength, uint16_t *pusReceivedLength,
	uint32_t ulTimeoutMs)
{
#if (APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U)
	return xTransportReceive(&s_xLogUartChannel, pucData, usMaxLength,
		pusReceivedLength, ulTimeoutMs);
#else
	(void)pucData;
	(void)usMaxLength;
	(void)pusReceivedLength;
	(void)ulTimeoutMs;
	return TRANSPORT_RESULT_NOT_SUPPORTED;
#endif
}

/*-----------------------------------------------------------*/
int32_t lAppCommLogPortDebugWrite(const uint8_t *pucData,
	uint16_t usLength, uint32_t ulTimeoutMs)
{
#if (APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U)
	return (int32_t)xTransportSend(&s_xLogUartChannel, pucData,
		usLength, ulTimeoutMs);
#else
	(void)pucData;
	(void)usLength;
	(void)ulTimeoutMs;
	return (int32_t)TRANSPORT_RESULT_NOT_SUPPORTED;
#endif
}

/**
 * @brief 获取端口层状态快照（线程安全）
 *
 * 临界区内完整复制 g_xAppCommLogPortStatus 到调用者缓冲区。
 * 业务代码和未来 HTTP 状态接口应调用此函数，不直接读全局变量。
 *
 * @param pxStatus [出] 接收状态快照的缓冲区，NULL 时静默返回
 */
/*-----------------------------------------------------------*/
/* Polls the registered UART after abandoning its normal DMA state. */
/*-----------------------------------------------------------*/
int32_t lAppCommLogPortCrashWrite(const uint8_t *pucData,
	uint16_t usLength)
{
#if ((APP_CRASH_DIAG_ENABLE != 0U) && \
	(APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U))
	USART_TypeDef *pxInstance;
	uint32_t ulSpin;
	uint16_t usIndex;

	if ((pucData == NULL) || (usLength == 0U) ||
		(s_xLogUartContext.xConfig.pxUart == NULL) ||
		(s_xLogUartContext.xConfig.pxUart->Instance == NULL)) {
		return -1;
	}

	pxInstance = s_xLogUartContext.xConfig.pxUart->Instance;
	if ((LL_USART_IsEnabled(pxInstance) == 0U) ||
		((pxInstance->CR1 & USART_CR1_TE) == 0U)) {
		return -1;
	}

	/* Prevent an active DMA stream from issuing new UART requests. */
	CLEAR_BIT(pxInstance->CR3, USART_CR3_DMAT);
	for (usIndex = 0U; usIndex < usLength; usIndex++) {
		vAppCrashDiagWatchdogRefresh();
		ulSpin = APP_CRASH_UART_SPIN_LIMIT;
		while (LL_USART_IsActiveFlag_TXE(pxInstance) == 0U) {
			ulSpin--;
			if (ulSpin == 0U) {
				return -2;
			}
			if ((ulSpin & 0x3FFFUL) == 0U) {
				vAppCrashDiagWatchdogRefresh();
			}
		}
		LL_USART_TransmitData8(pxInstance, pucData[usIndex]);
	}

	ulSpin = APP_CRASH_UART_SPIN_LIMIT;
	while (LL_USART_IsActiveFlag_TC(pxInstance) == 0U) {
		ulSpin--;
		if (ulSpin == 0U) {
			return -2;
		}
		if ((ulSpin & 0x3FFFUL) == 0U) {
			vAppCrashDiagWatchdogRefresh();
		}
	}
	return 0;
#else
	(void)pucData;
	(void)usLength;
	return -1;
#endif
}

/*-----------------------------------------------------------*/
void vAppCommLogPortGetStatus(AppCommLogPortStatus_t *pxStatus)
{
	if (pxStatus == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	*pxStatus = g_xAppCommLogPortStatus;
	taskEXIT_CRITICAL();
}

/**
 * @brief 端口层写入函数（被日志核心的后端接口注册）
 *
 * 接收同一条格式化后的日志文本，对每个编译期已启用的输出独立发送。
 * 每个输出的成功/失败独立计数，互不影响。
 *
 * 执行流程：
 * 1. 参数校验（数据指针和长度）
 * 2. 如果 UART 输出开启 → xTransportSend() 通过传输层发送
 *    - 成功 → ulUartSentCount++
 *    - 失败 → ulUartFailureCount++，记录 lLastUartError
 * 3. 如果网络输出开启 → lAppCommLogNetworkWrite() 发送
 *    - 当前为 __weak 空实现，始终失败
 * 4. 汇总结果：
 *    - 所有已启用输出均无（ucAttempted==0）→ 返回 0
 *    - 任一输出成功 → 返回 0
 *    - 全部输出失败 → 返回最后一次错误码
 *
 * @param pvContext  未使用（后端的 pvContext 为 NULL，实际上下文在 s_xLogUartChannel 中）
 * @param pucData    格式化后的日志文本（可能位于 CCM LOG 任务栈）
 * @param usLength   文本长度
 * @return 0 成功，负值失败
 */
/*-----------------------------------------------------------*/
static int32_t prvPortWrite(void *pvContext, const uint8_t *pucData,
	uint16_t usLength)
{
#if ((APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U) || \
	(APP_COMM_LOG_NETWORK_OUTPUT_ENABLE != 0U))
	int32_t lResult;
#endif
	int32_t lLastError;
	uint8_t ucAttempted;
	uint8_t ucSucceeded;

	(void)pvContext;

	/* 参数校验 */
	if ((pucData == NULL) || (usLength == 0U)) {
		return (int32_t)APP_COMM_LOG_RESULT_INVALID_ARG;
	}

	lLastError = (int32_t)APP_COMM_LOG_RESULT_NOT_READY;
	ucAttempted = 0U;
	ucSucceeded = 0U;

	/* --- UART 输出 --- */
#if (APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U)
	ucAttempted = 1U;

	/* 通过传输层统一发送 */
	lResult = (int32_t)xTransportSend(&s_xLogUartChannel, pucData,
		usLength, APP_COMM_LOG_UART_TIMEOUT_MS);

	/* 更新 UART 端口统计 */
	taskENTER_CRITICAL();
	g_xAppCommLogPortStatus.lLastUartError = lResult;
	if (lResult == 0) {
		g_xAppCommLogPortStatus.ulUartSentCount++;
	} else {
		g_xAppCommLogPortStatus.ulUartFailureCount++;
	}
	taskEXIT_CRITICAL();

	if (lResult == 0) {
		ucSucceeded = 1U;
	} else {
		lLastError = lResult;
	}
#endif

	/* --- 网络输出（弱占位） --- */
#if (APP_COMM_LOG_NETWORK_OUTPUT_ENABLE != 0U)
	ucAttempted = 1U;

	lResult = lAppCommLogNetworkWrite(pucData, usLength);

	/* 更新网络端口统计 */
	taskENTER_CRITICAL();
	g_xAppCommLogPortStatus.lLastNetworkError = lResult;
	if (lResult == 0) {
		g_xAppCommLogPortStatus.ulNetworkSentCount++;
	} else {
		g_xAppCommLogPortStatus.ulNetworkFailureCount++;
	}
	taskEXIT_CRITICAL();

	if (lResult == 0) {
		ucSucceeded = 1U;
	} else {
		lLastError = lResult;
	}
#endif

	/* 汇总结果 */
	if (ucAttempted == 0U) {
		return 0;  /* 所有输出都未启用 */
	}
	return (ucSucceeded != 0U) ? 0 : lLastError;
}

/**
 * @brief 网络日志弱实现（占位）
 *
 * 当前始终返回 APP_COMM_LOG_RESULT_NOT_READY。
 * 未来 HTTP 模块提供同名强实现后，链接器自动使用 HTTP 版本。
 *
 * 强实现必须（摘自文档约束）：
 * - 返回 0 表示已成功消费，非 0 表示失败
 * - 函数返回前完成发送或复制数据到自有缓冲区
 * - 禁止保存 pucData 指针供函数返回后使用
 * - 禁止调用任何日志提交 API（防递归）
 * - 必须有界超时，不能无限阻塞
 *
 * @param pucData    待发送数据（可能位于 CCM，DMA 不可访问）
 * @param usLength   数据长度
 * @return 始终返回 APP_COMM_LOG_RESULT_NOT_READY
 */
/*-----------------------------------------------------------*/
__weak int32_t lAppCommLogNetworkWrite(const uint8_t *pucData,
	uint16_t usLength)
{
	(void)pucData;
	(void)usLength;
	return (int32_t)APP_COMM_LOG_RESULT_NOT_READY;
}
