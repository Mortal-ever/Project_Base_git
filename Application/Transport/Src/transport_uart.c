/**
 * @file      transport_uart.c
 * @brief     基于 STM32 HAL UART / RS485 的 Transport 后端。
 * @author    WHong
 * @date      2026-07-28
 *
 * @details
 * UART 后端通过 static const TransportOps_t 绑定 open/close/send/receive/control 等操作，
 * pvContext 指向调用者拥有的 TransportUartContext_t。Context 内使用 FreeRTOS 静态
 * Mutex、Semaphore 和 StreamBuffer 管理任务态发送与中断态接收，不依赖动态分配。
 *
 * 发送路径根据调度器状态和 UART DMA 配置选择轮询或 DMA；半双工/RS485 场景在发送前后
 * 统一切换收发方向。接收使用 HAL 单字节中断反复重挂，并通过 UART 专用注册表把全局
 * HAL callback 路由回所属 Transport 实例。
 *
 * 若 Channel 注册了 Event Callback，RX 字节直接进入事件回调而不再写入同步 StreamBuffer；
 * 因此事件模式与同步 xTransportReceive() 是不同的数据消费路径。
 */

#include "transport_uart.h"

#include <string.h>

#include "task.h"

/** @brief 用于把全局 HAL UART 回调路由到具体 Transport 实例的 Channel 表。 */
static TransportChannel_t *s_apxUartChannels[TRANSPORT_UART_MAX_CHANNELS];

/** @brief UART 回调路由表中的有效 Channel 数量。 */
static uint8_t s_ucUartChannelCount;

static TransportResult_e prvOpen(void *pvContext);

static TransportResult_e prvClose(void *pvContext);

static TransportResult_e prvSend(void *pvContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, uint32_t ulTimeoutMs);

static TransportResult_e prvSendBeforeScheduler(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, uint32_t ulTimeoutMs);

static TransportResult_e prvSendRuntime(TransportUartContext_t *pxContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs);

static TransportResult_e prvTransmitRuntime(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, uint16_t *pusSentLen, TickType_t xTimeoutTicks);

static TransportResult_e prvTransmitDmaChunk(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, TickType_t xTimeoutTicks);

static TransportResult_e prvReceive(void *pvContext, uint8_t *pucData,
	uint16_t usMaxLen, uint16_t *pusReceivedLen, uint32_t ulTimeoutMs);

static TransportResult_e prvControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument);

static TransportState_e prvGetState(void *pvContext);

static int32_t prvGetNativeError(void *pvContext);

static TransportChannel_t *prvFindByUart(UART_HandleTypeDef *pxUart);

static TickType_t prvMsToTicks(uint32_t ulTimeoutMs);

static TickType_t prvGetRemainingTicks(TickType_t xStart,
	TickType_t xTimeout);

static uint32_t prvTicksToMs(TickType_t xTicks);

static HAL_StatusTypeDef prvSetDirection(TransportUartContext_t *pxContext,
	uint8_t ucTransmit);

/** @brief HAL UART 后端注册给公共 Transport 层的操作表。 */
static const TransportOps_t s_xUartOps = {
	prvOpen,
	prvClose,
	prvSend,
	prvReceive,
	prvControl,
	prvGetState,
	prvGetNativeError
};

/*-----------------------------------------------------------*/

/**
 * @brief  创建并注册一个 HAL UART / RS485 Transport 通道。
 *
 * @details
 * 调用者提供 Channel、Context、名称和 UART 配置存储。本函数先检查同一 HAL UART 句柄
 * 是否已经被其它 Transport 通道占用，再清零 Channel/Context、按值复制配置，并使用
 * FreeRTOS 静态对象存储创建发送互斥量、发送完成信号量和接收 StreamBuffer。
 *
 * Channel 绑定静态 s_xUartOps 操作表，pvContext 指回当前 UART Context，随后注册到公共
 * Transport 管理器和 UART 回调路由表。Channel/Context 本体仍由调用者拥有。
 *
 * @param[out] pxChannel  调用者提供的 TransportChannel_t。
 * @param[out] pxContext  调用者提供的 UART Transport Context。
 * @param[in]  pcName     通道名称；注册期间必须持续有效。
 * @param[in]  pxConfig   UART/RS485 配置，函数内部按值复制。
 *
 * @retval TRANSPORT_RESULT_OK 创建并注册成功。
 * @retval TRANSPORT_RESULT_INVALID_ARG 参数无效。
 * @retval TRANSPORT_RESULT_BUSY 同一 HAL UART 已被注册。
 * @retval TRANSPORT_RESULT_NO_RESOURCE 注册表或静态 RTOS 对象创建失败。
 */

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
	/* 配置按值保存在 Context 中，同时保留指回公共 Channel 的实例关系。 */
	pxContext->xConfig = *pxConfig;
	pxContext->pxChannel = pxChannel;
	/* RTOS 对象全部使用 Context 内嵌静态存储，不依赖 heap。 */
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

	/* Channel 绑定共享 UART 操作表，pvContext 区分具体 UART 实例。 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  打开 UART Transport，并按配置启动单字节中断接收。
 *
 * @details
 * 已打开时幂等返回。若未启用接收，则仅标记通道已打开；启用接收时通过
 * HAL_UART_Receive_IT() 把 ucRxByte 作为单字节接收槽，后续由 HAL Rx 完成回调负责
 * 投递字节并重新挂起下一次接收。
 *
 * @param[in,out] pvContext TransportUartContext_t 上下文。
 *
 * @retval TRANSPORT_RESULT_OK 打开成功。
 * @retval TRANSPORT_RESULT_BUSY HAL UART 忙。
 * @retval 其他 TransportResult_e 参数或 HAL 错误。
 */
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

/**
 * @brief  关闭 UART Transport 并停止当前接收流程。
 *
 * @details
 * 若接收已启用且当前未暂停，则调用 HAL_UART_AbortReceive() 终止接收；成功后把
 * ucIsOpen 清零并把 ucRxPaused 置位。该函数不销毁 Context 内的静态 RTOS 对象，也不
 * 注销 Channel，只关闭当前 UART 使用状态。
 *
 * @param[in,out] pvContext TransportUartContext_t 上下文。
 *
 * @retval TransportResult_e 关闭结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  根据当前执行环境选择 UART 发送实现。
 *
 * @details
 * 该函数是 s_xUartOps 的统一发送入口。禁止从 ISR 调用；调度器尚未启动时走
 * prvSendBeforeScheduler() 的同步 HAL 轮询路径，调度器运行后走 prvSendRuntime()，
 * 由互斥量、总时间预算以及可选 DMA 机制完成线程环境发送。
 *
 * @param[in,out] pvContext    UART Context。
 * @param[in]     pucData      待发送数据。
 * @param[in]     usDataLen    发送长度。
 * @param[out]    pusSentLen   实际发送长度。
 * @param[in]     ulTimeoutMs  发送总超时。
 *
 * @retval TransportResult_e 发送结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  在 FreeRTOS 调度器启动前使用 HAL 阻塞发送一整段 UART 数据。
 *
 * @details
 * 此时任务、信号量和 DMA 完成等待机制不能按运行期方式使用，因此直接切换 RS485/半双工
 * 方向后调用 HAL_UART_Transmit()。无论发送结果如何，函数结束前都会尝试恢复接收方向。
 *
 * @param[in,out] pxContext    UART Context。
 * @param[in]     pucData      待发送数据。
 * @param[in]     usDataLen    数据长度。
 * @param[out]    pusSentLen   实际发送长度。
 * @param[in]     ulTimeoutMs  HAL 阻塞发送超时。
 *
 * @retval TransportResult_e 发送结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  在任务上下文中串行化 UART 发送，并统一管理半双工收发切换。
 *
 * @details
 * 从函数入口建立总 Tick 预算，并用 xTxMutex 防止多个任务并发发送。若为半双工或配置了
 * RS485 方向脚，会先暂停当前中断接收，再切到发送方向；真正的数据发送交给
 * prvTransmitRuntime()。结束时恢复接收方向、必要时重新挂起 HAL_UART_Receive_IT()，
 * 更新公共 Channel 状态并释放互斥量。
 *
 * @param[in,out] pxContext    UART Context。
 * @param[in]     pucData      待发送数据。
 * @param[in]     usDataLen    数据长度。
 * @param[out]    pusSentLen   实际发送长度。
 * @param[in]     ulTimeoutMs  整个运行期发送的总超时。
 *
 * @retval TransportResult_e 发送结果。
 */
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

	/* 从入口建立一次总预算，互斥等待、发送和恢复动作共同消耗这笔时间。 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  根据 UART 是否配置 TX DMA 选择运行期实际发送路径。
 *
 * @details
 * 未配置有效 hdmatx 时使用 HAL_UART_Transmit() 阻塞发送；配置了 DMA 时，则把长报文按
 * TRANSPORT_UART_TX_BUFFER_SIZE 分块，并在同一个总 Tick 预算下依次调用
 * prvTransmitDmaChunk()。每完成一个分块才推进 pusSentLen，避免把尚未完成的数据计为成功。
 *
 * @param[in,out] pxContext      UART Context。
 * @param[in]     pucData        待发送数据。
 * @param[in]     usDataLen      总长度。
 * @param[out]    pusSentLen     实际累计发送长度。
 * @param[in]     xTimeoutTicks  自进入发送阶段起的总 Tick 预算。
 *
 * @retval TransportResult_e 发送结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  通过 Context 内嵌暂存区发送一个 DMA 分块并等待完成事件。
 *
 * @details
 * 先清空旧的 xTxDone 信号量，再把调用者数据复制到 aucTxStorage，使 DMA 生命周期不依赖
 * 外部临时缓冲区。启动 HAL_UART_Transmit_DMA() 后阻塞等待 TX 完成回调释放信号量。
 * 若等待超时，则中止当前发送并清理可能残留的完成信号。
 *
 * @param[in,out] pxContext      UART Context。
 * @param[in]     pucData        当前分块数据。
 * @param[in]     usDataLen      当前分块长度。
 * @param[in]     xTimeoutTicks  当前分块最多可使用的剩余 Tick 预算。
 *
 * @retval TransportResult_e DMA 发送结果。
 */
static TransportResult_e prvTransmitDmaChunk(
	TransportUartContext_t *pxContext, const uint8_t *pucData,
	uint16_t usDataLen, TickType_t xTimeoutTicks)
{
	HAL_StatusTypeDef xHalResult;
	HAL_StatusTypeDef xAbortResult;

	while (xSemaphoreTake(pxContext->xTxDone, 0U) == pdTRUE) {
	}
	/* 先复制到 Context 长期存在的 DMA 暂存区，避免异步 DMA 借用临时调用者缓冲区。 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  从 ISR 填充的 StreamBuffer 中读取 UART 字节。
 *
 * @details
 * 该同步接收路径要求 UART 已打开且配置允许接收。xStreamBufferReceive() 最多等待
 * ulTimeoutMs 对应 Tick；获得至少一个字节即返回 OK，否则返回 TIMEOUT。它只执行一次
 * StreamBuffer 读取，不保证填满 usMaxLen，固定长度累计由公共 xTransportReceiveExact()
 * 负责。
 *
 * @param[in,out] pvContext       UART Context。
 * @param[out]    pucData         接收缓冲区。
 * @param[in]     usMaxLen        最大读取字节数。
 * @param[out]    pusReceivedLen  实际读取字节数。
 * @param[in]     ulTimeoutMs     本次等待超时。
 *
 * @retval TransportResult_e 接收结果。
 */
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

/**
 * @brief  执行 UART Transport 的接收控制与波特率查询。
 *
 * @details
 * RX_PAUSE 终止当前 HAL 接收并置暂停状态；RX_RESUME 重新挂起单字节中断接收；
 * RX_FLUSH 清空 StreamBuffer；GET_BAUD_RATE 把当前 HAL UART Init.BaudRate 写到调用者
 * 提供的 uint32_t。其它命令返回 NOT_SUPPORTED。
 *
 * @param[in,out] pvContext   UART Context。
 * @param[in]     xCommand    控制命令。
 * @param[in,out] pvArgument  命令参数；GET_BAUD_RATE 时必须指向 uint32_t。
 *
 * @retval TransportResult_e 控制结果。
 */
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

/**
 * @brief  读取 UART 后端对应 Channel 的当前公共状态。
 *
 * @param[in] pvContext UART Context。
 *
 * @retval TransportState_e 当前状态；Context 或 Channel 无效时返回 UNINITIALIZED。
 */
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

/**
 * @brief  返回 UART Context 最近一次 HAL 原生状态/错误值。
 *
 * @param[in] pvContext UART Context。
 *
 * @retval 最近原生值；Context 无效时返回 HAL_ERROR。
 */
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

/**
 * @brief  按 HAL UART 句柄查找回调所属的 TransportChannel。
 *
 * @details
 * HAL 的 UART 完成/错误回调只提供 UART_HandleTypeDef，因此需要遍历 UART 专用注册表，
 * 找到其 Context 中 xConfig.pxUart 与传入句柄相同的 Channel，再恢复具体 Transport 实例。
 *
 * @param[in] pxUart HAL UART 句柄。
 *
 * @retval 非 NULL 匹配通道。
 * @retval NULL 未注册该 UART。
 */
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

/**
 * @brief  将毫秒超时转换为 FreeRTOS Tick。
 *
 * @details
 * 0 ms 保持为 0；正毫秒值若被 pdMS_TO_TICKS() 向下转换为 0，则提升为 1 Tick，
 * 避免正超时意外退化为非阻塞等待。
 *
 * @param[in] ulTimeoutMs 毫秒值。
 *
 * @retval 对应 Tick 数。
 */
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

/**
 * @brief  根据开始 Tick 和总预算计算当前剩余 Tick。
 *
 * @details
 * 使用无符号 Tick 差值计算经过时间；若已经达到或超过总预算则返回 0，否则返回剩余值。
 *
 * @param[in] xStart   操作开始 Tick。
 * @param[in] xTimeout 总 Tick 预算。
 *
 * @retval 剩余 Tick 数。
 */
static TickType_t prvGetRemainingTicks(TickType_t xStart,
	TickType_t xTimeout)
{
	TickType_t xElapsed;

	xElapsed = xTaskGetTickCount() - xStart;
	return (xElapsed >= xTimeout) ? 0U : (xTimeout - xElapsed);
}

/*-----------------------------------------------------------*/

/**
 * @brief  将 FreeRTOS Tick 转换为 HAL 使用的毫秒超时。
 *
 * @details
 * 使用 portTICK_PERIOD_MS 进行换算；若非零 Tick 因平台换算结果得到 0 ms，则提升为 1 ms。
 *
 * @param[in] xTicks Tick 数。
 *
 * @retval 毫秒超时。
 */
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

/**
 * @brief  切换 UART 半双工模式和可选 RS485 DE/RE 方向引脚。
 *
 * @details
 * 若启用 HAL 半双工模式，则根据 ucTransmit 选择发送器或接收器；若配置了外部方向 GPIO，
 * 同时按 xTxEnableLevel 设置发送有效电平或其反相接收电平。GPIO 写入本身无返回值，
 * 本函数最终返回的是 HAL 半双工切换结果。
 *
 * @param[in,out] pxContext   UART Context。
 * @param[in]     ucTransmit  非零表示发送方向，0 表示接收方向。
 *
 * @retval HAL_StatusTypeDef 半双工切换结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  把 HAL UART DMA/中断发送完成事件路由给所属 Transport 实例。
 *
 * @details
 * 通过 UART 句柄找到 Channel；若该 UART 不属于 Transport，则交给 weak 的 unclaimed
 * 回调。属于当前发送事务时清除 ucTxActive / ucTxError，释放 xTxDone 信号量，并向
 * Transport 事件层发布 TX_COMPLETE，最后按 FreeRTOS 规则请求必要的 ISR 退出切换。
 *
 * @param[in] pxUart HAL 回调提供的 UART 句柄。
 *
 * @warning 运行于中断上下文，不得执行阻塞操作。
 */

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

/*-----------------------------------------------------------*/

/**
 * @brief  处理一个 UART 接收字节，并重新挂起下一次单字节中断接收。
 *
 * @details
 * 找到所属 Channel 后，如果注册了 Transport Event Callback，则当前字节直接作为
 * RX_DATA 事件交给回调；否则写入同步接收使用的 xRxStream。StreamBuffer 满时增加丢弃计数
 * 并发布 RX_OVERFLOW。只要接收未暂停，就再次调用 HAL_UART_Receive_IT() 接收下一字节。
 *
 * 需要特别注意：存在 Event Callback 时，字节走事件路径而不会再写入 xRxStream，
 * 因而事件回调与同步 StreamBuffer 接收是两条互斥的数据消费路径。
 *
 * @param[in] pxUart HAL 回调提供的 UART 句柄。
 *
 * @warning 运行于中断上下文，不得执行阻塞操作。
 */

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
	/* 注册事件回调后 RX 字节直接交给事件路径，不再同时写入同步 StreamBuffer。 */
	if (pxChannel->pxEventCallback != NULL) {
		vTransportNotifyEventFromISR(pxChannel, TRANSPORT_EVENT_RX_DATA,
			&pxContext->ucRxByte, 1U, &xHigherPriorityTaskWoken);
	} else if (xStreamBufferSendFromISR(pxContext->xRxStream,
		&pxContext->ucRxByte, 1U, &xHigherPriorityTaskWoken) != 1U) {
		pxContext->ulRxDropCount++;
		vTransportNotifyEventFromISR(pxChannel, TRANSPORT_EVENT_RX_OVERFLOW,
			NULL, 0U, &xHigherPriorityTaskWoken);
	}

	/* 单字节接收完成后立即重挂下一字节，形成持续中断接收链。 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  记录 HAL UART 错误，并唤醒可能正在等待发送完成的任务。
 *
 * @details
 * 错误发生后保存 HAL_UART_GetError()、把 Channel 状态置 ERROR，并在存在活动 TX 时设置
 * ucTxError 后释放 xTxDone，使发送任务能够及时退出。随后发布 Transport ERROR 事件；
 * 若接收仍应运行，则发起异步 AbortReceive，完成后由 AbortReceiveCpltCallback 尝试重挂接收。
 *
 * @param[in] pxUart HAL 回调提供的 UART 句柄。
 *
 * @warning 运行于中断上下文，不得执行阻塞操作。
 */

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

/*-----------------------------------------------------------*/

/**
 * @brief  在异步接收中止完成后按当前状态尝试恢复单字节接收。
 *
 * @details
 * 仅当接收功能启用、通道仍处于打开状态且没有被显式暂停时重新调用
 * HAL_UART_Receive_IT()；重挂成功后把公共 Channel 状态恢复为 OPEN。
 *
 * @param[in] pxUart HAL 回调提供的 UART 句柄。
 *
 * @warning 运行于中断上下文。
 */

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

/**
 * @brief  处理未被 Transport 注册表认领的 HAL UART 发送完成回调。
 *
 * @details
 * 默认 weak 实现不做任何处理，应用层可提供同名强符号接管其它非 Transport UART 的回调。
 *
 * @param[in] pxUart 未认领的 UART 句柄。
 */

__weak void vTransportUartUnclaimedTxCallback(UART_HandleTypeDef *pxUart)
{
	(void)pxUart;
}

/*-----------------------------------------------------------*/

/**
 * @brief  处理未被 Transport 注册表认领的 HAL UART 接收完成回调。
 *
 * @details
 * 默认 weak 实现为空，允许应用层通过强符号扩展其它 UART 的 HAL 回调处理。
 *
 * @param[in] pxUart 未认领的 UART 句柄。
 */

__weak void vTransportUartUnclaimedRxCallback(UART_HandleTypeDef *pxUart)
{
	(void)pxUart;
}

/*-----------------------------------------------------------*/

/**
 * @brief  处理未被 Transport 注册表认领的 HAL UART 错误回调。
 *
 * @details
 * 默认 weak 实现为空，允许应用层为其它 UART 提供独立错误处理。
 *
 * @param[in] pxUart 未认领的 UART 句柄。
 */

__weak void vTransportUartUnclaimedErrorCallback(UART_HandleTypeDef *pxUart)
{
	(void)pxUart;
}
