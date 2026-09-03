/**
  * @file      transport.c
  * @brief     Implement the backend-neutral Transport dispatcher.
  * @author    WHong
  * @date      2026-07-28
  */

#include "transport.h"

#include <string.h>

#include "task.h"

/** @brief Registry of caller-owned channels in registration order. */
static TransportChannel_t *s_apxChannels[TRANSPORT_MAX_CHANNELS];
/** @brief Number of valid channel pointers in s_apxChannels. */
static uint8_t s_ucChannelCount;

/**
  * @brief  读取一个 Transport 通道后端的最新原生错误。
  * @param[in] pxChannel 已注册的 Transport 通道。
  * @retval 后端原生错误值；通道或后端无效时返回 0。
  */
static int32_t prvGetNativeError(TransportChannel_t *pxChannel);
/**
  * @brief  调用后端接收一次但不重复记录公共诊断信息。
  * @param[in,out] pxChannel 目标 Transport 通道。
  * @param[out] pucData 接收数据缓冲区。
  * @param[in] usMaxLen 缓冲区容量，单位为字节。
  * @param[out] pusReceivedLen 实际接收字节数。
  * @param[in] ulTimeoutMs 接收总超时时间，单位为毫秒。
  * @retval TransportResult_e 后端接收结果。
  */
static TransportResult_e prvReceiveOnce(TransportChannel_t *pxChannel,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs);
/**
  * @brief  将毫秒转换为至少一个 FreeRTOS Tick。
  * @param[in] ulTimeoutMs 超时时间，单位为毫秒。
  * @retval 向上取整后的 Tick 数。
  */
static TickType_t prvMsToTicks(uint32_t ulTimeoutMs);
/**
  * @brief  将 FreeRTOS Tick 向上取整转换为毫秒。
  * @param[in] xTicks 待转换的 Tick 数。
  * @retval 向上取整后的毫秒数。
  */
static uint32_t prvTicksToMsCeil(TickType_t xTicks);
/**
  * @brief  更新一次逻辑操作的计数器、状态和故障详情。
  * @param[in,out] pxChannel 被操作的 Transport 通道。
  * @param[in] xOperation 操作阶段。
  * @param[in] xResult 规范化操作结果。
  * @param[in] usRequestedLength 请求的字节数或接收容量。
  * @param[in] usTransferredLength 实际完成的字节数。
  */
static void prvRecordOperation(TransportChannel_t *pxChannel,
	TransportOperation_e xOperation, TransportResult_e xResult,
	uint16_t usRequestedLength, uint16_t usTransferredLength);

/* Resets only the channel registry; backend contexts remain caller-owned. */
/*-----------------------------------------------------------*/
void vTransportManagerInit(void)
{
	uint8_t ucIndex;

	taskENTER_CRITICAL();
	for (ucIndex = 0U; ucIndex < TRANSPORT_MAX_CHANNELS; ucIndex++) {
		s_apxChannels[ucIndex] = NULL; /* 清空对应的通道对象。 */
	}
	s_ucChannelCount = 0U;
	taskEXIT_CRITICAL();
}

/* Validates and stores one channel; duplicate names are rejected. */
/*-----------------------------------------------------------*/
TransportResult_e xTransportRegister(TransportChannel_t *pxChannel)
{
	uint8_t ucIndex;

	if ((pxChannel == NULL) || (pxChannel->pcName == NULL) ||
		(pxChannel->pxOps == NULL) || (pxChannel->pvContext == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	taskENTER_CRITICAL();
	for (ucIndex = 0U; ucIndex < s_ucChannelCount; ucIndex++) {
		if (strcmp(s_apxChannels[ucIndex]->pcName, pxChannel->pcName) == 0) {
			taskEXIT_CRITICAL();
			return TRANSPORT_RESULT_BUSY;
		}
	}

	if (s_ucChannelCount >= TRANSPORT_MAX_CHANNELS) {
		taskEXIT_CRITICAL();
		return TRANSPORT_RESULT_NO_RESOURCE;
	}

	s_apxChannels[s_ucChannelCount] = pxChannel;
	s_ucChannelCount++;
	pxChannel->xState = TRANSPORT_STATE_CLOSED;
	memset(&pxChannel->xStatus, 0, sizeof(pxChannel->xStatus));
	pxChannel->xStatus.xState = TRANSPORT_STATE_CLOSED;
	taskEXIT_CRITICAL();

	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
TransportChannel_t *pxTransportFind(const char *pcName)
{
	uint8_t ucIndex;

	if (pcName == NULL) {
		return NULL;
	}

	for (ucIndex = 0U; ucIndex < s_ucChannelCount; ucIndex++) {
		if (strcmp(s_apxChannels[ucIndex]->pcName, pcName) == 0) {
			return s_apxChannels[ucIndex];
		}
	}

	return NULL;
}

/* Dispatches backend open and records state, counters, and native failure. */
/*-----------------------------------------------------------*/
TransportResult_e xTransportOpen(TransportChannel_t *pxChannel)
{
	TransportResult_e xResult;

	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xOpen == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	xResult = pxChannel->pxOps->xOpen(pxChannel->pvContext);
	pxChannel->xState = (xResult == TRANSPORT_RESULT_OK) ?
		TRANSPORT_STATE_OPEN : TRANSPORT_STATE_ERROR;
	prvRecordOperation(pxChannel, TRANSPORT_OPERATION_OPEN, xResult, 0U, 0U);
	return xResult;
}

/*-----------------------------------------------------------*/
TransportResult_e xTransportClose(TransportChannel_t *pxChannel)
{
	TransportResult_e xResult;

	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xClose == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	xResult = pxChannel->pxOps->xClose(pxChannel->pvContext);
	if (xResult == TRANSPORT_RESULT_OK) {
		pxChannel->xState = TRANSPORT_STATE_CLOSED;
	}
	prvRecordOperation(pxChannel, TRANSPORT_OPERATION_CLOSE, xResult, 0U,
		0U);
	return xResult;
}

/* Dispatches backend send and records one complete operation result. */
/*-----------------------------------------------------------*/
TransportResult_e xTransportSend(TransportChannel_t *pxChannel,
								 const uint8_t *pucData,
								 uint16_t usDataLen,
								 uint32_t ulTimeoutMs)
{
	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xSend == NULL) || (pucData == NULL) ||
		(usDataLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	{
		TransportResult_e xResult;
		uint16_t usSentLen;

		usSentLen = 0U;
		xResult = pxChannel->pxOps->xSend(pxChannel->pvContext, pucData,
			usDataLen, &usSentLen, ulTimeoutMs);
		if ((xResult == TRANSPORT_RESULT_OK) &&
			(usSentLen != usDataLen)) {
			xResult = TRANSPORT_RESULT_IO_ERROR;
		}
		prvRecordOperation(pxChannel, TRANSPORT_OPERATION_SEND, xResult,
			usDataLen, usSentLen);
		return xResult;
	}
}

/* Dispatches backend receive and records the actual received byte count. */
/*-----------------------------------------------------------*/
TransportResult_e xTransportReceive(TransportChannel_t *pxChannel,
									uint8_t *pucData,
									uint16_t usMaxLen,
									uint16_t *pusReceivedLen,
									uint32_t ulTimeoutMs)
{
	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xReceive == NULL) || (pucData == NULL) ||
		(pusReceivedLen == NULL) || (usMaxLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	{
		TransportResult_e xResult;

		*pusReceivedLen = 0U;
		xResult = prvReceiveOnce(pxChannel, pucData, usMaxLen,
			pusReceivedLen, ulTimeoutMs);
		prvRecordOperation(pxChannel, TRANSPORT_OPERATION_RECEIVE, xResult,
			usMaxLen, *pusReceivedLen);
		return xResult;
	}
}

/*
 * Accumulates fragmented stream data without renewing the total timeout.
 * Intermediate backend timeouts are retried because nonblocking socket
 * backends can report no data before the overall deadline expires.
 */
/*-----------------------------------------------------------*/
TransportResult_e xTransportReceiveExact(TransportChannel_t *pxChannel,
	uint8_t *pucData, uint16_t usExpectedLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs)
{
	TransportResult_e xResult;
	TickType_t xStart;
	TickType_t xBudget;
	TickType_t xElapsed;
	TickType_t xRemaining;
	uint32_t ulRemainingMs;
	uint16_t usOffset;
	uint16_t usReceived;

	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xReceive == NULL) || (pucData == NULL) ||
		(pusReceivedLen == NULL) || (usExpectedLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	*pusReceivedLen = 0U;
	usOffset = 0U;
	xResult = TRANSPORT_RESULT_TIMEOUT;

	if (ulTimeoutMs == 0U) {
		usReceived = 0U;
		xResult = prvReceiveOnce(pxChannel, pucData, usExpectedLen,
			&usReceived, 0U);
		if (usReceived > usExpectedLen) {
			usReceived = 0U;
			xResult = TRANSPORT_RESULT_IO_ERROR;
		} else if (usReceived == usExpectedLen) {
			xResult = TRANSPORT_RESULT_OK;
		} else if (xResult == TRANSPORT_RESULT_OK) {
			xResult = TRANSPORT_RESULT_TIMEOUT;
		} else {
		}
		*pusReceivedLen = usReceived;
		prvRecordOperation(pxChannel, TRANSPORT_OPERATION_RECEIVE, xResult,
			usExpectedLen, usReceived);
		return xResult;
	}

	if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		return TRANSPORT_RESULT_NOT_READY;
	}

	xStart = xTaskGetTickCount();
	xBudget = prvMsToTicks(ulTimeoutMs);

	while (usOffset < usExpectedLen) {
		xElapsed = xTaskGetTickCount() - xStart;
		if (xElapsed >= xBudget) {
			xResult = TRANSPORT_RESULT_TIMEOUT;
			break;
		}

		xRemaining = xBudget - xElapsed;
		ulRemainingMs = prvTicksToMsCeil(xRemaining);
		usReceived = 0U;
		xResult = prvReceiveOnce(pxChannel, &pucData[usOffset],
			(uint16_t)(usExpectedLen - usOffset), &usReceived,
			ulRemainingMs);

		if (usReceived > (uint16_t)(usExpectedLen - usOffset)) {
			xResult = TRANSPORT_RESULT_IO_ERROR;
			break;
		}

		usOffset = (uint16_t)(usOffset + usReceived);
		if (usOffset == usExpectedLen) {
			xResult = TRANSPORT_RESULT_OK;
			break;
		}

		if (xResult == TRANSPORT_RESULT_OK) {
			if (usReceived == 0U) {
				xResult = TRANSPORT_RESULT_IO_ERROR;
				break;
			}
			continue;
		}

		if (xResult == TRANSPORT_RESULT_TIMEOUT) {
			if (usReceived == 0U) {
				xElapsed = xTaskGetTickCount() - xStart;
				if (xElapsed < xBudget) {
					vTaskDelay(1U);
				}
			}
			continue;
		}

		break;
	}

	*pusReceivedLen = usOffset;
	prvRecordOperation(pxChannel, TRANSPORT_OPERATION_RECEIVE, xResult,
		usExpectedLen, usOffset);
	return xResult;
}

/*-----------------------------------------------------------*/
TransportResult_e xTransportControl(TransportChannel_t *pxChannel,
									TransportControl_e xCommand,
									void *pvArgument)
{
	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xControl == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	{
		TransportResult_e xResult;

		xResult = pxChannel->pxOps->xControl(pxChannel->pvContext, xCommand,
			pvArgument);
		prvRecordOperation(pxChannel, TRANSPORT_OPERATION_CONTROL, xResult,
			0U, 0U);
		return xResult;
	}
}

/*-----------------------------------------------------------*/
TransportState_e xTransportGetState(TransportChannel_t *pxChannel)
{
	if ((pxChannel == NULL) || (pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->xGetState == NULL)) {
		return TRANSPORT_STATE_UNINITIALIZED;
	}

	return pxChannel->pxOps->xGetState(pxChannel->pvContext);
}

/*-----------------------------------------------------------*/
TransportResult_e xTransportGetStatus(TransportChannel_t *pxChannel,
	TransportStatus_t *pxStatus)
{
	if ((pxChannel == NULL) || (pxStatus == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	taskENTER_CRITICAL();
	*pxStatus = pxChannel->xStatus;
	taskEXIT_CRITICAL();
	pxStatus->xState = xTransportGetState(pxChannel);
	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
void vTransportSetEventCallback(TransportChannel_t *pxChannel,
								TransportEventCallback_t pxCallback,
								void *pvCallbackContext)
{
	if (pxChannel == NULL) {
		return;
	}

	taskENTER_CRITICAL();
	pxChannel->pvEventContext = pvCallbackContext;
	pxChannel->pxEventCallback = pxCallback;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
void vTransportNotifyEventFromISR(TransportChannel_t *pxChannel,
								  TransportEvent_e xEvent,
								  const uint8_t *pucData,
								  uint16_t usDataLen,
								  BaseType_t *pxHigherPriorityTaskWoken)
{
	TransportEventCallback_t pxCallback;

	if (pxChannel == NULL) {
		return;
	}

	pxCallback = pxChannel->pxEventCallback;
	{
		UBaseType_t uxSavedInterruptStatus;

		uxSavedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
		if (xEvent == TRANSPORT_EVENT_RX_DATA) {
			pxChannel->xStatus.xLastRxTick = xTaskGetTickCountFromISR();
			pxChannel->xStatus.ulRxByteCount += usDataLen;
		} else if ((xEvent == TRANSPORT_EVENT_ERROR) ||
			(xEvent == TRANSPORT_EVENT_RX_OVERFLOW)) {
			pxChannel->xStatus.ulErrorCount++;
		}
		taskEXIT_CRITICAL_FROM_ISR(uxSavedInterruptStatus);
	}
	if (pxCallback != NULL) {
		pxCallback(pxChannel, xEvent, pucData, usDataLen,
			pxHigherPriorityTaskWoken, pxChannel->pvEventContext);
	}
}

/*-----------------------------------------------------------*/
static int32_t prvGetNativeError(TransportChannel_t *pxChannel)
{
	if ((pxChannel->pxOps == NULL) ||
		(pxChannel->pxOps->lGetNativeError == NULL)) {
		return 0;
	}
	return pxChannel->pxOps->lGetNativeError(pxChannel->pvContext);
}

/* Performs one backend receive without recording a logical operation. */
/*-----------------------------------------------------------*/
static TransportResult_e prvReceiveOnce(TransportChannel_t *pxChannel,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs)
{
	return pxChannel->pxOps->xReceive(pxChannel->pvContext, pucData,
		usMaxLen, pusReceivedLen, ulTimeoutMs);
}

/* Converts a positive millisecond timeout to at least one RTOS tick. */
/*-----------------------------------------------------------*/
static TickType_t prvMsToTicks(uint32_t ulTimeoutMs)
{
	TickType_t xTicks;

	xTicks = pdMS_TO_TICKS(ulTimeoutMs);
	return (xTicks == 0U) ? 1U : xTicks;
}

/* Converts remaining ticks to milliseconds without rounding down. */
/*-----------------------------------------------------------*/
static uint32_t prvTicksToMsCeil(TickType_t xTicks)
{
	uint64_t ullMilliseconds;
	uint32_t ulTickRate;

	ulTickRate = (uint32_t)configTICK_RATE_HZ;
	ullMilliseconds = ((uint64_t)xTicks * 1000ULL) +
		(uint64_t)(ulTickRate - 1U);
	ullMilliseconds /= (uint64_t)ulTickRate;
	if (ullMilliseconds > (uint64_t)UINT32_MAX) {
		return UINT32_MAX;
	}
	return (uint32_t)ullMilliseconds;
}

/* Centralizes counters and the last fault after each backend operation. */
/*-----------------------------------------------------------*/
static void prvRecordOperation(TransportChannel_t *pxChannel,
	TransportOperation_e xOperation, TransportResult_e xResult,
	uint16_t usRequestedLength, uint16_t usTransferredLength)
{
	TickType_t xNow;
	int32_t lNativeError;

	xNow = xTaskGetTickCount();
	lNativeError = prvGetNativeError(pxChannel);
	if ((pxChannel->pxOps != NULL) &&
		(pxChannel->pxOps->xGetState != NULL)) {
		pxChannel->xState = pxChannel->pxOps->xGetState(
			pxChannel->pvContext);
	}
	taskENTER_CRITICAL();
	pxChannel->xStatus.xState = pxChannel->xState;
	if (xOperation == TRANSPORT_OPERATION_SEND) {
		pxChannel->xStatus.usLastTxRequestedLength = usRequestedLength;
		pxChannel->xStatus.usLastTxTransferredLength =
			usTransferredLength;
	} else if (xOperation == TRANSPORT_OPERATION_RECEIVE) {
		pxChannel->xStatus.usLastRxCapacity = usRequestedLength;
		pxChannel->xStatus.usLastRxTransferredLength =
			usTransferredLength;
	} else {
	}
	if (xResult == TRANSPORT_RESULT_OK) {
		if (xOperation == TRANSPORT_OPERATION_OPEN) {
			pxChannel->xStatus.xLastOpenTick = xNow;
			pxChannel->xStatus.ulOpenCount++;
		} else if (xOperation == TRANSPORT_OPERATION_SEND) {
			pxChannel->xStatus.xLastTxTick = xNow;
			pxChannel->xStatus.ulTxOperationCount++;
			pxChannel->xStatus.ulTxByteCount += usTransferredLength;
		} else if (xOperation == TRANSPORT_OPERATION_RECEIVE) {
			pxChannel->xStatus.xLastRxTick = xNow;
			pxChannel->xStatus.ulRxOperationCount++;
			pxChannel->xStatus.ulRxByteCount += usTransferredLength;
		} else {
		}
	} else {
		pxChannel->xStatus.xLastFault.xOperation = xOperation;
		pxChannel->xStatus.xLastFault.xResult = xResult;
		pxChannel->xStatus.xLastFault.lNativeError = lNativeError;
		pxChannel->xStatus.xLastFault.xTimestamp = xNow;
		pxChannel->xStatus.xLastFault.usRequestedLength =
			usRequestedLength;
		pxChannel->xStatus.xLastFault.usTransferredLength =
			usTransferredLength;
		if (xResult != TRANSPORT_RESULT_TIMEOUT) {
			pxChannel->xStatus.ulErrorCount++;
		}
	}
	taskEXIT_CRITICAL();
}
