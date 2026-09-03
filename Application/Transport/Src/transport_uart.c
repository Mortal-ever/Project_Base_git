/**
  * @file      transport_uart.c
  * @brief     Implement HAL UART and RS485 Transport operations.
  * @author    WHong
  * @date      2026-07-28
  */

#include "transport_uart.h"

#include <string.h>

#include "task.h"

/** @brief Channels routed by the global HAL UART callbacks. */
static TransportChannel_t *s_apxUartChannels[TRANSPORT_UART_MAX_CHANNELS];
/** @brief Number of valid channel pointers in s_apxUartChannels. */
static uint8_t s_ucUartChannelCount;

/**
  * @brief  打开已配置的 UART 通道并启动接收中断。
  * @param[in,out] pvContext TransportUartContext_t 上下文。
  * @retval TransportResult_e 打开结果。
  */
static TransportResult_e prvOpen(void *pvContext);
/**
  * @brief  关闭 UART 通道并终止活动 HAL 操作。
  * @param[in,out] pvContext TransportUartContext_t 上下文。
  * @retval TransportResult_e 关闭结果。
  */
static TransportResult_e prvClose(void *pvContext);
/**
  * @brief  根据调度器状态选择启动前或运行期 UART 发送路径。
  * @param[in,out] pvContext UART Transport 上下文。
  * @param[in] pucData 待发送数据。
  * @param[in] usDataLen 待发送字节数。
  * @param[out] pusSentLen 实际发送字节数。
  * @param[in] ulTimeoutMs 发送总超时时间，单位为毫秒。
  * @retval TransportResult_e 发送结果。
  */
static TransportResult_e prvSend(void *pvContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, uint32_t ulTimeoutMs);
/**
  * @brief  在 FreeRTOS 调度器启动前使用有界轮询发送 UART 数据。
  * @param[in,out] pxContext UART Transport 上下文。
  * @param[in] pucData 待发送数据。
  * @param[in] usDataLen 待发送字节数。
  * @param[out] pusSentLen 实际发送字节数。
  * @param[in] ulTimeoutMs 发送总超时时间，单位为毫秒。
  * @retval TransportResult_e 发送结果。
  */
static TransportResult_e prvSendBeforeScheduler(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, uint32_t ulTimeoutMs);
/**
  * @brief  在任务上下文中串行化发送并使用一个总超时。
  * @param[in,out] pxContext UART Transport 上下文。
  * @param[in] pucData 待发送数据。
  * @param[in] usDataLen 待发送字节数。
  * @param[out] pusSentLen 实际发送字节数。
  * @param[in] ulTimeoutMs 发送总超时时间，单位为毫秒。
  * @retval TransportResult_e 发送结果。
  */
static TransportResult_e prvSendRuntime(TransportUartContext_t *pxContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs);
/**
  * @brief  选择运行期 DMA 暂存发送或轮询发送路径。
  * @param[in,out] pxContext UART Transport 上下文。
  * @param[in] pucData 待发送数据。
  * @param[in] usDataLen 待发送字节数。
  * @param[out] pusSentLen 实际发送字节数。
  * @param[in] xTimeoutTicks 发送总 Tick 预算。
  * @retval TransportResult_e 发送结果。
  */
static TransportResult_e prvTransmitRuntime(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, TickType_t xTimeoutTicks);
/**
  * @brief  复制一段数据到 DMA 可访问暂存区并发送。
  * @param[in,out] pxContext UART Transport 上下文及 DMA 暂存区。
  * @param[in] pucData 待发送数据。
  * @param[in] usDataLen 本次暂存区块字节数。
  * @param[in] xTimeoutTicks 本次 DMA 操作的 Tick 预算。
  * @retval TransportResult_e DMA 发送结果。
  */
static TransportResult_e prvTransmitDmaChunk(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, TickType_t xTimeoutTicks);
/**
  * @brief  从中断写入的静态 StreamBuffer 接收字节。
  * @param[in,out] pvContext UART Transport 上下文。
  * @param[out] pucData 接收缓冲区。
  * @param[in] usMaxLen 接收缓冲区容量。
  * @param[out] pusReceivedLen 实际接收字节数。
  * @param[in] ulTimeoutMs 接收总超时时间，单位为毫秒。
  * @retval TransportResult_e 接收结果。
  */
static TransportResult_e prvReceive(void *pvContext, uint8_t *pucData,
	uint16_t usMaxLen, uint16_t *pusReceivedLen, uint32_t ulTimeoutMs);
/**
  * @brief  执行 UART 接收、清空或波特率控制命令。
  * @param[in,out] pvContext UART Transport 上下文。
  * @param[in] xCommand 控制命令。
  * @param[in,out] pvArgument 控制参数，按命令类型解释。
  * @retval TransportResult_e 控制结果。
  */
static TransportResult_e prvControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument);
/**
  * @brief  读取 UART 后端生命周期状态。
  * @param[in] pvContext UART Transport 上下文。
  * @retval 当前 TransportState_e 状态。
  */
static TransportState_e prvGetState(void *pvContext);
/**
  * @brief  读取最新 HAL 状态或错误码。
  * @param[in] pvContext UART Transport 上下文。
  * @retval HAL 原生状态或错误码。
  */
static int32_t prvGetNativeError(void *pvContext);
/**
  * @brief  按 HAL UART 句柄查找已注册的 Transport 通道。
  * @param[in] pxUart HAL UART 句柄。
  * @retval 匹配的通道指针；找不到时返回 NULL。
  */
static TransportChannel_t *prvFindByUart(UART_HandleTypeDef *pxUart);
/**
  * @brief  将毫秒转换为有界的 FreeRTOS Tick 数。
  * @param[in] ulTimeoutMs 超时时间，单位为毫秒。
  * @retval 向上取整后的 Tick 数。
  */
static TickType_t prvMsToTicks(uint32_t ulTimeoutMs);
/**
  * @brief  计算经过时间后剩余的 Tick 预算。
  * @param[in] xStart 操作开始 Tick。
  * @param[in] xTimeout 原始总 Tick 预算。
  * @retval 剩余 Tick 数；超时后返回 0。
  */
static TickType_t prvGetRemainingTicks(TickType_t xStart,
	TickType_t xTimeout);
/**
  * @brief  将 FreeRTOS Tick 转换为有界 HAL 毫秒超时。
  * @param[in] xTicks 待转换的 Tick 数。
  * @retval HAL 可接受的毫秒超时值。
  */
static uint32_t prvTicksToMs(TickType_t xTicks);
/**
  * @brief  设置可选 RS485 收发方向控制。
  * @param[in,out] pxContext UART Transport 上下文。
  * @param[in] ucTransmit 非零切换发送方向，零切换接收方向。
  * @retval HAL_OK 方向控制成功或未配置方向脚。
  * @retval 其他 HAL_StatusTypeDef GPIO 写入失败。
  */
static HAL_StatusTypeDef prvSetDirection(TransportUartContext_t *pxContext,
	uint8_t ucTransmit);

/** @brief HAL UART operation table registered with generic channels. */
static const TransportOps_t s_xUartOps = {
	prvOpen,
	prvClose,
	prvSend,
	prvReceive,
	prvControl,
	prvGetState,
	prvGetNativeError
};

/* Initializes static RTOS objects and registers one HAL UART channel. */
/*-----------------------------------------------------------*/
TransportResult_e xTransportUartCreate(TransportChannel_t *pxChannel,
									   TransportUartContext_t *pxContext,
									   const char *pcName,
									   const TransportUartConfig_t *pxConfig)
{
	uint8_t ucIndex;
	TransportResult_e xResult;

	if ((pxChannel == NULL) || (pxContext == NULL) || (pcName == NULL) ||
		(pxConfig == NULL) || (pxConfig->pxUart == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	for (ucIndex = 0U; ucIndex < s_ucUartChannelCount; ucIndex++) {
		TransportUartContext_t *pxRegisteredContext;

		pxRegisteredContext = (TransportUartContext_t *)
			s_apxUartChannels[ucIndex]->pvContext;
		if (pxRegisteredContext->xConfig.pxUart == pxConfig->pxUart) {
			return TRANSPORT_RESULT_BUSY;
		}
	}

	if (s_ucUartChannelCount >= TRANSPORT_UART_MAX_CHANNELS) {
		return TRANSPORT_RESULT_NO_RESOURCE;
	}

	memset(pxContext, 0, sizeof(*pxContext));
	memset(pxChannel, 0, sizeof(*pxChannel));
	pxContext->xConfig = *pxConfig;
	pxContext->pxChannel = pxChannel;
	pxContext->xTxMutex = xSemaphoreCreateMutexStatic(
		&pxContext->xTxMutexStorage);
	pxContext->xTxDone = xSemaphoreCreateBinaryStatic(
		&pxContext->xTxDoneStorage);
	pxContext->xRxStream = xStreamBufferCreateStatic(
		TRANSPORT_UART_RX_BUFFER_SIZE, 1U, pxContext->aucRxStorage,
		&pxContext->xRxStreamStorage);

	if ((pxContext->xTxMutex == NULL) || (pxContext->xTxDone == NULL) ||
		(pxContext->xRxStream == NULL)) {
		return TRANSPORT_RESULT_NO_RESOURCE;
	}

	pxChannel->pcName = pcName;
	pxChannel->pxOps = &s_xUartOps;
	pxChannel->pvContext = pxContext;
	pxChannel->xState = TRANSPORT_STATE_CLOSED;

	xResult = xTransportRegister(pxChannel);
	if (xResult != TRANSPORT_RESULT_OK) {
		return xResult;
	}

	s_apxUartChannels[s_ucUartChannelCount] = pxChannel;
	s_ucUartChannelCount++;
	return TRANSPORT_RESULT_OK;
}

/* Opens TX and optionally starts interrupt-driven single-byte reception. */
/*-----------------------------------------------------------*/
static TransportResult_e prvOpen(void *pvContext)
{
	TransportUartContext_t *pxContext;
	HAL_StatusTypeDef xHalResult;

	pxContext = (TransportUartContext_t *)pvContext;
	if ((pxContext == NULL) || (pxContext->xConfig.pxUart == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if (pxContext->ucIsOpen != 0U) {
		return TRANSPORT_RESULT_OK;
	}

	pxContext->ucRxPaused = 1U;
	pxContext->ucIsOpen = 1U;
	if (pxContext->xConfig.ucReceiveEnabled == 0U) {
		pxContext->lLastNativeError = (int32_t)HAL_OK;
		return TRANSPORT_RESULT_OK;
	}

	xHalResult = HAL_UART_Receive_IT(pxContext->xConfig.pxUart,
		&pxContext->ucRxByte, 1U);
	pxContext->lLastNativeError = (int32_t)xHalResult;
	if (xHalResult != HAL_OK) {
		pxContext->ucIsOpen = 0U;
		return (xHalResult == HAL_BUSY) ? TRANSPORT_RESULT_BUSY :
			TRANSPORT_RESULT_IO_ERROR;
	}

	pxContext->ucRxPaused = 0U;
	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static TransportResult_e prvClose(void *pvContext)
{
	TransportUartContext_t *pxContext;
	HAL_StatusTypeDef xHalResult;

	pxContext = (TransportUartContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if (pxContext->ucIsOpen == 0U) {
		return TRANSPORT_RESULT_OK;
	}

	if ((pxContext->xConfig.ucReceiveEnabled != 0U) &&
		(pxContext->ucRxPaused == 0U)) {
		xHalResult = HAL_UART_AbortReceive(pxContext->xConfig.pxUart);
	} else {
		xHalResult = HAL_OK;
	}
	pxContext->lLastNativeError = (int32_t)xHalResult;
	if (xHalResult != HAL_OK) {
		return TRANSPORT_RESULT_IO_ERROR;
	}

	pxContext->ucIsOpen = 0U;
	pxContext->ucRxPaused = 1U;
	return TRANSPORT_RESULT_OK;
}

/* Selects one UART TX implementation without exposing mode to callers. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSend(void *pvContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, uint32_t ulTimeoutMs)
{
	TransportUartContext_t *pxContext;
	BaseType_t xSchedulerState;

	pxContext = (TransportUartContext_t *)pvContext;
	if ((pxContext == NULL) || (pucData == NULL) ||
		(pusSentLen == NULL) || (usDataLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	*pusSentLen = 0U;
	if (pxContext->ucIsOpen == 0U) {
		return TRANSPORT_RESULT_NOT_OPEN;
	}
	if (__get_IPSR() != 0U) {
		return TRANSPORT_RESULT_NOT_SUPPORTED;
	}

	xSchedulerState = xTaskGetSchedulerState();
	if (xSchedulerState == taskSCHEDULER_NOT_STARTED) {
		return prvSendBeforeScheduler(pxContext, pucData, usDataLen,
			pusSentLen, ulTimeoutMs);
	}
	if (xSchedulerState != taskSCHEDULER_RUNNING) {
		return TRANSPORT_RESULT_NOT_READY;
	}
	return prvSendRuntime(pxContext, pucData, usDataLen, pusSentLen,
		ulTimeoutMs);
}

/* Uses polling because no task can wait for DMA before the scheduler runs. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSendBeforeScheduler(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, uint32_t ulTimeoutMs)
{
	HAL_StatusTypeDef xHalResult;
	TransportResult_e xResult;

	xHalResult = prvSetDirection(pxContext, 1U);
	if (xHalResult != HAL_OK) {
		pxContext->lLastNativeError = (int32_t)xHalResult;
		return TRANSPORT_RESULT_IO_ERROR;
	}

	xHalResult = HAL_UART_Transmit(pxContext->xConfig.pxUart,
		(uint8_t *)pucData, usDataLen, ulTimeoutMs);
	pxContext->lLastNativeError = (int32_t)xHalResult;
	if (xHalResult == HAL_OK) {
		*pusSentLen = usDataLen;
		xResult = TRANSPORT_RESULT_OK;
	} else if (xHalResult == HAL_TIMEOUT) {
		xResult = TRANSPORT_RESULT_TIMEOUT;
	} else if (xHalResult == HAL_BUSY) {
		xResult = TRANSPORT_RESULT_BUSY;
	} else {
		xResult = TRANSPORT_RESULT_IO_ERROR;
	}
	if (prvSetDirection(pxContext, 0U) != HAL_OK) {
		pxContext->lLastNativeError = (int32_t)HAL_ERROR;
		xResult = TRANSPORT_RESULT_IO_ERROR;
	}
	return xResult;
}

/* Serializes runtime writers and restores RX/RS485 state on every exit. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSendRuntime(TransportUartContext_t *pxContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs)
{
	TickType_t xStart;
	TickType_t xTimeoutTicks;
	TickType_t xRemainingTicks;
	HAL_StatusTypeDef xHalResult;
	TransportResult_e xResult;
	uint8_t ucRxWasPaused;
	uint8_t ucRxStopped;

	xStart = xTaskGetTickCount();
	xTimeoutTicks = prvMsToTicks(ulTimeoutMs);
	if (xSemaphoreTake(pxContext->xTxMutex, xTimeoutTicks) != pdTRUE) {
		return TRANSPORT_RESULT_BUSY;
	}
	xRemainingTicks = prvGetRemainingTicks(xStart, xTimeoutTicks);
	ucRxWasPaused = pxContext->ucRxPaused;
	ucRxStopped = 0U;
	if ((pxContext->xConfig.ucHalfDuplex != 0U) ||
		(pxContext->xConfig.pxDirectionPort != NULL)) {
		if ((pxContext->xConfig.ucReceiveEnabled != 0U) &&
			(ucRxWasPaused == 0U)) {
			pxContext->ucRxPaused = 1U;
			xHalResult = HAL_UART_AbortReceive(pxContext->xConfig.pxUart);
			pxContext->lLastNativeError = (int32_t)xHalResult;
			if (xHalResult != HAL_OK) {
				pxContext->ucRxPaused = ucRxWasPaused;
				xSemaphoreGive(pxContext->xTxMutex);
				return TRANSPORT_RESULT_IO_ERROR;
			}
			ucRxStopped = 1U;
		}
	}

	xHalResult = prvSetDirection(pxContext, 1U);
	pxContext->lLastNativeError = (int32_t)xHalResult;
	if (xHalResult != HAL_OK) {
		(void)prvSetDirection(pxContext, 0U);
		if (ucRxStopped != 0U) {
			xHalResult = HAL_UART_Receive_IT(pxContext->xConfig.pxUart,
				&pxContext->ucRxByte, 1U);
			if (xHalResult == HAL_OK) {
				pxContext->ucRxPaused = 0U;
			}
		}
		xSemaphoreGive(pxContext->xTxMutex);
		return TRANSPORT_RESULT_IO_ERROR;
	}

	pxContext->pxChannel->xState = TRANSPORT_STATE_BUSY;
	xResult = prvTransmitRuntime(pxContext, pucData, usDataLen, pusSentLen,
		xRemainingTicks);

	if (prvSetDirection(pxContext, 0U) != HAL_OK) {
		xResult = TRANSPORT_RESULT_IO_ERROR;
		pxContext->lLastNativeError = (int32_t)HAL_ERROR;
	}

	if (ucRxStopped != 0U) {
		xHalResult = HAL_UART_Receive_IT(pxContext->xConfig.pxUart,
			&pxContext->ucRxByte, 1U);
		pxContext->lLastNativeError = (int32_t)xHalResult;
		if (xHalResult != HAL_OK) {
			xResult = TRANSPORT_RESULT_IO_ERROR;
		} else {
			pxContext->ucRxPaused = 0U;
		}
	}

	pxContext->pxChannel->xState = (xResult == TRANSPORT_RESULT_OK) ?
		TRANSPORT_STATE_OPEN : TRANSPORT_STATE_ERROR;
	xSemaphoreGive(pxContext->xTxMutex);
	return xResult;
}

/* Uses the CubeMX-linked TX DMA handle as the only mode selection source. */
/*-----------------------------------------------------------*/
static TransportResult_e prvTransmitRuntime(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, TickType_t xTimeoutTicks)
{
	HAL_StatusTypeDef xHalResult;
	TransportResult_e xResult;
	TickType_t xStart;
	TickType_t xRemainingTicks;
	uint16_t usChunkLength;
	uint16_t usOffset;

	if ((pxContext->xConfig.pxUart->hdmatx == NULL) ||
		(pxContext->xConfig.pxUart->hdmatx->Instance == NULL)) {
		xHalResult = HAL_UART_Transmit(pxContext->xConfig.pxUart,
			(uint8_t *)pucData, usDataLen, prvTicksToMs(xTimeoutTicks));
		pxContext->lLastNativeError = (int32_t)xHalResult;
		if (xHalResult == HAL_OK) {
			*pusSentLen = usDataLen;
			return TRANSPORT_RESULT_OK;
		}
		if (xHalResult == HAL_TIMEOUT) {
			return TRANSPORT_RESULT_TIMEOUT;
		}
		return (xHalResult == HAL_BUSY) ? TRANSPORT_RESULT_BUSY :
			TRANSPORT_RESULT_IO_ERROR;
	}

	xStart = xTaskGetTickCount();
	usOffset = 0U;
	while (usOffset < usDataLen) {
		usChunkLength = (uint16_t)(usDataLen - usOffset);
		if (usChunkLength > TRANSPORT_UART_TX_BUFFER_SIZE) {
			usChunkLength = TRANSPORT_UART_TX_BUFFER_SIZE;
		}
		xRemainingTicks = prvGetRemainingTicks(xStart, xTimeoutTicks);
		if (xRemainingTicks == 0U) {
			return TRANSPORT_RESULT_TIMEOUT;
		}
		xResult = prvTransmitDmaChunk(pxContext, &pucData[usOffset],
			usChunkLength, xRemainingTicks);
		if (xResult != TRANSPORT_RESULT_OK) {
			return xResult;
		}
		usOffset = (uint16_t)(usOffset + usChunkLength);
		*pusSentLen = usOffset;
	}
	return TRANSPORT_RESULT_OK;
}

/* Sends one DMA-accessible staging-buffer chunk. */
/*-----------------------------------------------------------*/
static TransportResult_e prvTransmitDmaChunk(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, TickType_t xTimeoutTicks)
{
	HAL_StatusTypeDef xHalResult;
	HAL_StatusTypeDef xAbortResult;

	while (xSemaphoreTake(pxContext->xTxDone, 0U) == pdTRUE) {
	}
	memcpy(pxContext->aucTxStorage, pucData, usDataLen);
	pxContext->ucTxError = 0U;
	pxContext->ucTxActive = 1U;
	xHalResult = HAL_UART_Transmit_DMA(pxContext->xConfig.pxUart,
		pxContext->aucTxStorage, usDataLen);
	pxContext->lLastNativeError = (int32_t)xHalResult;
	if (xHalResult != HAL_OK) {
		pxContext->ucTxActive = 0U;
		return (xHalResult == HAL_BUSY) ? TRANSPORT_RESULT_BUSY :
			TRANSPORT_RESULT_IO_ERROR;
	}
	if (xSemaphoreTake(pxContext->xTxDone, xTimeoutTicks) == pdTRUE) {
		if (pxContext->ucTxError != 0U) {
			return TRANSPORT_RESULT_IO_ERROR;
		}
		return TRANSPORT_RESULT_OK;
	}

	pxContext->ucTxActive = 0U;
	xAbortResult = HAL_UART_AbortTransmit(pxContext->xConfig.pxUart);
	while (xSemaphoreTake(pxContext->xTxDone, 0U) == pdTRUE) {
	}
	pxContext->lLastNativeError = (xAbortResult == HAL_OK) ?
		(int32_t)HAL_TIMEOUT : (int32_t)xAbortResult;
	return (xAbortResult == HAL_OK) ? TRANSPORT_RESULT_TIMEOUT :
		TRANSPORT_RESULT_IO_ERROR;
}

/* Reads from the ISR-fed stream buffer with a bounded wait. */
/*-----------------------------------------------------------*/
static TransportResult_e prvReceive(void *pvContext, uint8_t *pucData,
	uint16_t usMaxLen, uint16_t *pusReceivedLen, uint32_t ulTimeoutMs)
{
	TransportUartContext_t *pxContext;
	size_t xReceivedLen;

	pxContext = (TransportUartContext_t *)pvContext;
	if ((pxContext == NULL) || (pucData == NULL) ||
		(pusReceivedLen == NULL) || (usMaxLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if (pxContext->ucIsOpen == 0U) {
		return TRANSPORT_RESULT_NOT_OPEN;
	}
	if (pxContext->xConfig.ucReceiveEnabled == 0U) {
		return TRANSPORT_RESULT_NOT_SUPPORTED;
	}

	xReceivedLen = xStreamBufferReceive(pxContext->xRxStream, pucData,
		(size_t)usMaxLen, prvMsToTicks(ulTimeoutMs));
	*pusReceivedLen = (uint16_t)xReceivedLen;
	pxContext->lLastNativeError = (xReceivedLen > 0U) ?
		(int32_t)HAL_OK : (int32_t)HAL_TIMEOUT;
	return (xReceivedLen > 0U) ? TRANSPORT_RESULT_OK :
		TRANSPORT_RESULT_TIMEOUT;
}

/*-----------------------------------------------------------*/
static TransportResult_e prvControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument)
{
	TransportUartContext_t *pxContext;
	HAL_StatusTypeDef xHalResult;

	pxContext = (TransportUartContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	switch (xCommand) {
	case TRANSPORT_CTRL_RX_PAUSE:
		if (pxContext->xConfig.ucReceiveEnabled == 0U) {
			return TRANSPORT_RESULT_NOT_SUPPORTED;
		}
		if (pxContext->ucRxPaused != 0U) {
			return TRANSPORT_RESULT_OK;
		}
		xHalResult = HAL_UART_AbortReceive(pxContext->xConfig.pxUart);
		pxContext->lLastNativeError = (int32_t)xHalResult;
		if (xHalResult != HAL_OK) {
			return TRANSPORT_RESULT_IO_ERROR;
		}
		pxContext->ucRxPaused = 1U;
		return TRANSPORT_RESULT_OK;

	case TRANSPORT_CTRL_RX_RESUME:
		if (pxContext->xConfig.ucReceiveEnabled == 0U) {
			return TRANSPORT_RESULT_NOT_SUPPORTED;
		}
		if (pxContext->ucRxPaused == 0U) {
			return TRANSPORT_RESULT_OK;
		}
		xHalResult = HAL_UART_Receive_IT(pxContext->xConfig.pxUart,
			&pxContext->ucRxByte, 1U);
		pxContext->lLastNativeError = (int32_t)xHalResult;
		if (xHalResult != HAL_OK) {
			return TRANSPORT_RESULT_IO_ERROR;
		}
		pxContext->ucRxPaused = 0U;
		return TRANSPORT_RESULT_OK;

	case TRANSPORT_CTRL_RX_FLUSH:
		if (pxContext->xConfig.ucReceiveEnabled == 0U) {
			return TRANSPORT_RESULT_NOT_SUPPORTED;
		}
		if (xStreamBufferReset(pxContext->xRxStream) != pdPASS) {
			return TRANSPORT_RESULT_BUSY;
		}
		return TRANSPORT_RESULT_OK;

	case TRANSPORT_CTRL_GET_BAUD_RATE:
		if (pvArgument == NULL) {
			return TRANSPORT_RESULT_INVALID_ARG;
		}
		*((uint32_t *)pvArgument) = pxContext->xConfig.pxUart->Init.BaudRate;
		return TRANSPORT_RESULT_OK;

	default:
		return TRANSPORT_RESULT_NOT_SUPPORTED;
	}
}

/*-----------------------------------------------------------*/
static TransportState_e prvGetState(void *pvContext)
{
	TransportUartContext_t *pxContext;

	pxContext = (TransportUartContext_t *)pvContext;
	if ((pxContext == NULL) || (pxContext->pxChannel == NULL)) {
		return TRANSPORT_STATE_UNINITIALIZED;
	}
	return pxContext->pxChannel->xState;
}

/*-----------------------------------------------------------*/
static int32_t prvGetNativeError(void *pvContext)
{
	TransportUartContext_t *pxContext;

	pxContext = (TransportUartContext_t *)pvContext;
	if (pxContext == NULL) {
		return (int32_t)HAL_ERROR;
	}
	return pxContext->lLastNativeError;
}

/*-----------------------------------------------------------*/
static TransportChannel_t *prvFindByUart(UART_HandleTypeDef *pxUart)
{
	uint8_t ucIndex;

	for (ucIndex = 0U; ucIndex < s_ucUartChannelCount; ucIndex++) {
		TransportUartContext_t *pxContext;

		pxContext = (TransportUartContext_t *)
			s_apxUartChannels[ucIndex]->pvContext;
		if (pxContext->xConfig.pxUart == pxUart) {
			return s_apxUartChannels[ucIndex];
		}
	}
	return NULL;
}

/*-----------------------------------------------------------*/
static TickType_t prvMsToTicks(uint32_t ulTimeoutMs)
{
	TickType_t xTicks;

	if (ulTimeoutMs == 0U) {
		return 0U;
	}
	xTicks = pdMS_TO_TICKS(ulTimeoutMs);
	return (xTicks == 0U) ? 1U : xTicks;
}

/*-----------------------------------------------------------*/
static TickType_t prvGetRemainingTicks(TickType_t xStart,
	TickType_t xTimeout)
{
	TickType_t xElapsed;

	xElapsed = xTaskGetTickCount() - xStart;
	return (xElapsed >= xTimeout) ? 0U : (xTimeout - xElapsed);
}

/*-----------------------------------------------------------*/
static uint32_t prvTicksToMs(TickType_t xTicks)
{
	uint32_t ulMilliseconds;

	ulMilliseconds = (uint32_t)xTicks * (uint32_t)portTICK_PERIOD_MS;
	if ((xTicks != 0U) && (ulMilliseconds == 0U)) {
		ulMilliseconds = 1U;
	}
	return ulMilliseconds;
}

/*-----------------------------------------------------------*/
static HAL_StatusTypeDef prvSetDirection(TransportUartContext_t *pxContext,
	uint8_t ucTransmit)
{
	GPIO_PinState xPinState;
	HAL_StatusTypeDef xHalResult;

	xHalResult = HAL_OK;
	if (pxContext->xConfig.ucHalfDuplex != 0U) {
		xHalResult = (ucTransmit != 0U) ?
			HAL_HalfDuplex_EnableTransmitter(pxContext->xConfig.pxUart) :
			HAL_HalfDuplex_EnableReceiver(pxContext->xConfig.pxUart);
	}

	if (pxContext->xConfig.pxDirectionPort != NULL) {
		xPinState = (ucTransmit != 0U) ?
			pxContext->xConfig.xTxEnableLevel :
			((pxContext->xConfig.xTxEnableLevel == GPIO_PIN_SET) ?
			 GPIO_PIN_RESET : GPIO_PIN_SET);
		HAL_GPIO_WritePin(pxContext->xConfig.pxDirectionPort,
			pxContext->xConfig.usDirectionPin, xPinState);
	}

	return xHalResult;
}

/**
  * @brief Route HAL transmit completion to the owning UART Transport.
  * @param[in] pxUart HAL UART handle supplied by the interrupt path.
  * @warning Runs in interrupt context and must not block.
  */
/*-----------------------------------------------------------*/
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *pxUart)
{
	TransportChannel_t *pxChannel;
	TransportUartContext_t *pxContext;
	BaseType_t xHigherPriorityTaskWoken;

	pxChannel = prvFindByUart(pxUart);
	if (pxChannel == NULL) {
		vTransportUartUnclaimedTxCallback(pxUart);
		return;
	}

	pxContext = (TransportUartContext_t *)pxChannel->pvContext;
	if (pxContext->ucTxActive == 0U) {
		return;
	}
	pxContext->ucTxActive = 0U;
	pxContext->ucTxError = 0U;
	pxContext->lLastNativeError = (int32_t)HAL_OK;
	xHigherPriorityTaskWoken = pdFALSE;
	(void)xSemaphoreGiveFromISR(pxContext->xTxDone,
		&xHigherPriorityTaskWoken);
	vTransportNotifyEventFromISR(pxChannel, TRANSPORT_EVENT_TX_COMPLETE,
		NULL, 0U, &xHigherPriorityTaskWoken);
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
  * @brief Route one received byte and rearm interrupt reception.
  * @param[in] pxUart HAL UART handle supplied by the interrupt path.
  * @warning Runs in interrupt context and must not block.
  */
/*-----------------------------------------------------------*/
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *pxUart)
{
	TransportChannel_t *pxChannel;
	TransportUartContext_t *pxContext;
	BaseType_t xHigherPriorityTaskWoken;
	HAL_StatusTypeDef xHalResult;

	pxChannel = prvFindByUart(pxUart);
	if (pxChannel == NULL) {
		vTransportUartUnclaimedRxCallback(pxUart);
		return;
	}

	pxContext = (TransportUartContext_t *)pxChannel->pvContext;
	if (pxContext->xConfig.ucReceiveEnabled == 0U) {
		return;
	}
	xHigherPriorityTaskWoken = pdFALSE;
	if (pxChannel->pxEventCallback != NULL) {
		vTransportNotifyEventFromISR(pxChannel, TRANSPORT_EVENT_RX_DATA,
			&pxContext->ucRxByte, 1U, &xHigherPriorityTaskWoken);
	} else if (xStreamBufferSendFromISR(pxContext->xRxStream,
		&pxContext->ucRxByte, 1U, &xHigherPriorityTaskWoken) != 1U) {
		pxContext->ulRxDropCount++;
		vTransportNotifyEventFromISR(pxChannel, TRANSPORT_EVENT_RX_OVERFLOW,
			NULL, 0U, &xHigherPriorityTaskWoken);
	}

	if (pxContext->ucRxPaused == 0U) {
		xHalResult = HAL_UART_Receive_IT(pxUart, &pxContext->ucRxByte, 1U);
		if (xHalResult != HAL_OK) {
			pxContext->ulErrorCount++;
			pxContext->lLastNativeError = (int32_t)xHalResult;
			pxChannel->xState = TRANSPORT_STATE_ERROR;
			vTransportNotifyEventFromISR(pxChannel, TRANSPORT_EVENT_ERROR,
				NULL, 0U, &xHigherPriorityTaskWoken);
		}
	}
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
  * @brief Record a HAL UART error and wake any waiting transmit task.
  * @param[in] pxUart HAL UART handle supplied by the interrupt path.
  * @warning Runs in interrupt context and must not block.
  */
/*-----------------------------------------------------------*/
void HAL_UART_ErrorCallback(UART_HandleTypeDef *pxUart)
{
	TransportChannel_t *pxChannel;
	TransportUartContext_t *pxContext;
	BaseType_t xHigherPriorityTaskWoken;

	pxChannel = prvFindByUart(pxUart);
	if (pxChannel == NULL) {
		vTransportUartUnclaimedErrorCallback(pxUart);
		return;
	}

	pxContext = (TransportUartContext_t *)pxChannel->pvContext;
	pxContext->ulErrorCount++;
	pxContext->lLastNativeError =
		(int32_t)HAL_UART_GetError(pxContext->xConfig.pxUart);
	pxChannel->xState = TRANSPORT_STATE_ERROR;
	xHigherPriorityTaskWoken = pdFALSE;
	if (pxContext->ucTxActive != 0U) {
		pxContext->ucTxActive = 0U;
		pxContext->ucTxError = 1U;
		(void)xSemaphoreGiveFromISR(pxContext->xTxDone,
			&xHigherPriorityTaskWoken);
	}
	vTransportNotifyEventFromISR(pxChannel, TRANSPORT_EVENT_ERROR,
		NULL, 0U, &xHigherPriorityTaskWoken);
	if ((pxContext->xConfig.ucReceiveEnabled != 0U) &&
		(pxContext->ucRxPaused == 0U)) {
		(void)HAL_UART_AbortReceive_IT(pxUart);
	}
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
  * @brief Rearm managed receive after an asynchronous HAL abort completes.
  * @param[in] pxUart HAL UART handle supplied by the interrupt path.
  * @warning Runs in interrupt context and must not block.
  */
/*-----------------------------------------------------------*/
void HAL_UART_AbortReceiveCpltCallback(UART_HandleTypeDef *pxUart)
{
	TransportChannel_t *pxChannel;
	TransportUartContext_t *pxContext;

	pxChannel = prvFindByUart(pxUart);
	if (pxChannel == NULL) {
		return;
	}
	pxContext = (TransportUartContext_t *)pxChannel->pvContext;
	if ((pxContext->xConfig.ucReceiveEnabled != 0U) &&
		(pxContext->ucIsOpen != 0U) &&
		(pxContext->ucRxPaused == 0U)) {
		if (HAL_UART_Receive_IT(pxUart, &pxContext->ucRxByte, 1U) == HAL_OK) {
			pxChannel->xState = TRANSPORT_STATE_OPEN;
		}
	}
}

/*-----------------------------------------------------------*/
__weak void vTransportUartUnclaimedTxCallback(UART_HandleTypeDef *pxUart)
{
	(void)pxUart;
}

/*-----------------------------------------------------------*/
__weak void vTransportUartUnclaimedRxCallback(UART_HandleTypeDef *pxUart)
{
	(void)pxUart;
}

/*-----------------------------------------------------------*/
__weak void vTransportUartUnclaimedErrorCallback(UART_HandleTypeDef *pxUart)
{
	(void)pxUart;
}
