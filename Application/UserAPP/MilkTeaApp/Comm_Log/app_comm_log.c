/**
 * @file    app_comm_log.c
 * @brief   日志子系统核心实现
 *
 * 日志子系统核心，包含 LOG 任务、结构化事件队列、格式化引擎和
 * 输出策略引擎。
 *
 * 组件：
 * - 32 条环形队列（无动态分配）
 * - 三级输出策略：即时事件 / 状态去重心跳 / 重复错误摘要
 * - 文本映射表：Source × Result → Task/Module/Event/Level/ResultText
 * - lwIP err_t 和 socket errno 到可读文本的转换
 *
 * 流量路径：
 *   业务任务 → vAppCommLogWrite/WriteState/WriteRateLimited
 *     → prvQueueEntry() 入队 → xTaskNotifyGive() 唤醒
 *       → LOG 任务: prvTakePendingEntry() 出队
 *         → prvFormatEntry() 格式化
 *           → s_xBackend.pxWrite() → prvPortWrite() (app_comm_log_port.c)
 *             → xTransportSend() (UART) / lAppCommLogNetworkWrite() (网络)
 */

#include "app_comm_log.h"

#include <string.h>

#include "lwip/err.h"
#include "lwip/errno.h"
#include "task.h"

/** @brief 日志子系统整体运行统计（Keil Watch 可直接监视） */
AppCommLogStatus_t g_xAppCommLogStatus;

#if (APP_COMM_LOG_ENABLE != 0U)

/*
 * 单一拥有者的日志服务，使用固定 RAM 队列，无动态分配。
 * 生产者只写入一条结构化条目，LOG 任务负责格式化和编译期选择的输出路由。
 */
static AppCommLogBackend_t s_xBackend;
static AppCommLogEntry_t s_axPendingEntries[APP_COMM_LOG_QUEUE_LENGTH];
static TaskHandle_t s_xLogTaskHandle;
static uint16_t s_usQueueReadIndex;
static uint16_t s_usQueueWriteIndex;
static uint16_t s_usQueueCount;

typedef struct {
	TickType_t xLastEmitTick;
	int32_t lResult;
	int32_t lNativeError;
	uint8_t ucInitialized;
} AppCommLogStateSlot_t;

typedef struct {
	TickType_t xLastEmitTick;
	int32_t lResult;
	int32_t lNativeError;
	uint32_t ulSuppressedCount;
	uint8_t ucInitialized;
} AppCommLogRepeatSlot_t;

static AppCommLogStateSlot_t
	s_axStateSlots[APP_COMM_SOURCE_COUNT];
static AppCommLogRepeatSlot_t
	s_axRepeatSlots[APP_COMM_SOURCE_COUNT];

static uint8_t prvTakePendingEntry(AppCommLogEntry_t *pxEntry);
static void prvQueueEntry(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError, int32_t lAuxValue,
	AppCommLogEmission_e xEmission, uint32_t ulRepeatCount);
static uint16_t prvFormatEntry(const AppCommLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usCapacity);
static uint16_t prvAppendEmission(const AppCommLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usLength, uint16_t usCapacity);
static uint16_t prvAppendText(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, const char *pcText);
static uint16_t prvAppendUnsigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint32_t ulValue);
static uint16_t prvAppendSigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, int32_t lValue);
static uint16_t prvAppendAuxiliary(const AppCommLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usLength, uint16_t usCapacity);
static const char *prvGetLevelText(const AppCommLogEntry_t *pxEntry);
static const char *prvGetTaskText(AppCommSource_e xSource);
static const char *prvGetModuleText(AppCommSource_e xSource);
static const char *prvGetEventText(AppCommSource_e xSource);
static const char *prvGetResultText(AppCommSource_e xSource,
	int32_t lResult);
static const char *prvGetNativeLabel(AppCommSource_e xSource,
	int32_t lResult);
static uint8_t prvIsLwipNative(AppCommSource_e xSource, int32_t lResult);
static const char *prvGetLwipErrorText(int32_t lNativeError);
static uint8_t prvIsSocketNative(AppCommSource_e xSource,
	int32_t lResult);
static const char *prvGetSocketErrorText(int32_t lNativeError);
#if (APP_COMM_LOG_CCM_FAULT_INJECTION_ENABLE != 0U)
static void prvInjectCcmBoundaryFault(void);
#endif

/**
 * @brief LOG 任务入口函数
 *
 * 日志子系统唯一的输出 I/O 拥有者。
 * 启动时注册输出路由并写一条自身诊断日志，
 * 之后循环从队列取出事件 → 格式化 → 发送到后端。
 * 无待处理事件时通过 ulTaskNotifyTake 挂起等待。
 */
void vAppCommLogTask(void *pvArgument)
{
	AppCommLogEntry_t xEntry;
	AppCommLogBackend_t xBackend;
	uint8_t aucLine[APP_COMM_LOG_LINE_LENGTH];
	uint16_t usLength;
	int32_t lBackendError;

	(void)pvArgument;
	/* 记录本任务句柄，供业务任务通知用 */
	s_xLogTaskHandle = xTaskGetCurrentTaskHandle();
	/* 写一条自身启动诊断 */
	vAppCommLogWrite(APP_COMM_SOURCE_LOG, 0, 0);

	for (;;) {
		if (prvTakePendingEntry(&xEntry) == 0U) {
			(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			continue;
		}

		usLength = prvFormatEntry(&xEntry, aucLine,
			APP_COMM_LOG_LINE_LENGTH);
		taskENTER_CRITICAL();
		xBackend = s_xBackend;
		taskEXIT_CRITICAL();
		if ((xBackend.pxWrite == NULL) || (usLength == 0U)) {
			taskENTER_CRITICAL();
			g_xAppCommLogStatus.ulBackendFailureCount++;
			g_xAppCommLogStatus.lLastBackendError =
				(int32_t)APP_COMM_LOG_RESULT_NOT_READY;
			taskEXIT_CRITICAL();
			continue;
		}

		lBackendError = xBackend.pxWrite(xBackend.pvContext, aucLine,
			usLength);
		taskENTER_CRITICAL();
		if (lBackendError == 0) {
			g_xAppCommLogStatus.ulSentCount++;
		} else {
			g_xAppCommLogStatus.ulBackendFailureCount++;
		}
		g_xAppCommLogStatus.lLastBackendError = lBackendError;
		taskEXIT_CRITICAL();

#if (APP_COMM_LOG_CCM_FAULT_INJECTION_ENABLE != 0U)
		/*
		 * TEMPORARY CCM FAULT INJECTION TEST.
		 *
		 * Trigger only after the LOG task has attempted its own startup
		 * message. Set APP_COMM_LOG_CCM_FAULT_INJECTION_ENABLE to zero or
		 * remove this block and prvInjectCcmBoundaryFault after validation.
		 */
		if (xEntry.xSource == APP_COMM_SOURCE_LOG) {
			prvInjectCcmBoundaryFault();
		}
#endif
	}
}

#if (APP_COMM_LOG_CCM_FAULT_INJECTION_ENABLE != 0U)
/*
 * Writes one word immediately beyond the STM32F407 64 KB CCM boundary.
 * The volatile access and barriers force the invalid CPU write to complete.
 */
/*-----------------------------------------------------------*/
static void prvInjectCcmBoundaryFault(void)
{
	volatile uint32_t *pulInvalidCcmAddress;

	pulInvalidCcmAddress = (volatile uint32_t *)0x10010000UL;
	*pulInvalidCcmAddress = 0x43434D46UL;
	__DSB();
	__ISB();
}
#endif

/**
 * @brief 注册/替换日志输出后端
 *
 * 启动时由端口层调用。也可运行时调用以动态替换后端。
 * 同一时间只有一个有效后端。
 */
AppCommLogResult_e xAppCommLogRegisterBackend(
	const AppCommLogBackend_t *pxBackend)
{
	AppCommLogResult_e xResult;

	if ((pxBackend == NULL) || (pxBackend->pxWrite == NULL)) {
		return APP_COMM_LOG_RESULT_INVALID_ARG;
	}
	taskENTER_CRITICAL();
	xResult = (g_xAppCommLogStatus.ucBackendRegistered != 0U) ?
		APP_COMM_LOG_RESULT_REPLACED : APP_COMM_LOG_RESULT_OK;
	s_xBackend = *pxBackend;
	g_xAppCommLogStatus.ucBackendRegistered = 1U;
	taskEXIT_CRITICAL();
	return xResult;
}

/**
 * @brief 提交即时事件（简化版）
 *
 * 内部调用 vAppCommLogWriteExtended，AuxValue 固定为 0。
 */
void vAppCommLogWrite(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError)
{
	vAppCommLogWriteExtended(xSource, lResult, lNativeError, 0);
}

/**
 * @brief 提交即时事件（扩展版，带辅助诊断数据）
 */
void vAppCommLogWriteExtended(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError, int32_t lAuxValue)
{
	prvQueueEntry(xSource, lResult, lNativeError, lAuxValue,
		APP_COMM_LOG_EMISSION_EVENT, 0U);
}

/**
 * @brief 提交状态快照（带去重和周期心跳）
 *
 * 首次出现或状态变化 → 立即输出（event=CHANGED）；
 * 状态未变化 → 每 5 秒输出一次心跳（event=HEARTBEAT）。
 * 每个 AppCommSource 独立维护状态槽。
 *
 * @return 1 本次实际入队，0 已抑制
 */
uint8_t ucAppCommLogWriteState(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError)
{
	AppCommLogStateSlot_t *pxSlot;
	AppCommLogEmission_e xEmission;
	TickType_t xNow;
	TickType_t xPeriod;
	uint8_t ucEmit;

	if ((uint32_t)xSource >= (uint32_t)APP_COMM_SOURCE_COUNT) {
		vAppCommLogWrite(xSource, lResult, lNativeError);
		return 1U;
	}
	xNow = xTaskGetTickCount();
	xPeriod = pdMS_TO_TICKS(APP_COMM_LOG_HEARTBEAT_PERIOD_MS);
	ucEmit = 0U;
	xEmission = APP_COMM_LOG_EMISSION_STATE_HEARTBEAT;
	taskENTER_CRITICAL();
	pxSlot = &s_axStateSlots[(uint32_t)xSource];
	if ((pxSlot->ucInitialized == 0U) ||
		(pxSlot->lResult != lResult) ||
		(pxSlot->lNativeError != lNativeError)) {
		pxSlot->ucInitialized = 1U;
		pxSlot->lResult = lResult;
		pxSlot->lNativeError = lNativeError;
		pxSlot->xLastEmitTick = xNow;
		g_xAppCommLogStatus.ulStateChangeCount++;
		xEmission = APP_COMM_LOG_EMISSION_STATE_CHANGED;
		ucEmit = 1U;
	} else if ((TickType_t)(xNow - pxSlot->xLastEmitTick) >= xPeriod) {
		pxSlot->xLastEmitTick = xNow;
		g_xAppCommLogStatus.ulHeartbeatCount++;
		ucEmit = 1U;
	}
	taskEXIT_CRITICAL();
	if (ucEmit != 0U) {
		prvQueueEntry(xSource, lResult, lNativeError, 0,
			xEmission, 0U);
	}
	return ucEmit;
}

/**
 * @brief 提交错误日志（带重复抑制）
 *
 * 同源同错误的首次出现立即输出（event=FIRST），后续相同错误抑制并累加计数，
 * 每 5 秒输出一次摘要（event=REPEAT_SUMMARY，含 repeat=N）。
 * 错误类型变化时先输出之前错误的摘要再重置。
 */
void vAppCommLogWriteRateLimited(AppCommSource_e xSource,
	int32_t lResult, int32_t lNativeError)
{
	AppCommLogRepeatSlot_t *pxSlot;
	TickType_t xNow;
	TickType_t xPeriod;
	int32_t lSummaryResult;
	int32_t lSummaryNativeError;
	uint32_t ulSummaryCount;
	uint8_t ucEmitFirst;
	uint8_t ucEmitSummary;

	if ((uint32_t)xSource >= (uint32_t)APP_COMM_SOURCE_COUNT) {
		vAppCommLogWrite(xSource, lResult, lNativeError);
		return;
	}
	xNow = xTaskGetTickCount();
	xPeriod = pdMS_TO_TICKS(APP_COMM_LOG_REPEAT_SUMMARY_PERIOD_MS);
	lSummaryResult = 0;
	lSummaryNativeError = 0;
	ulSummaryCount = 0U;
	ucEmitFirst = 0U;
	ucEmitSummary = 0U;
	taskENTER_CRITICAL();
	pxSlot = &s_axRepeatSlots[(uint32_t)xSource];
	if (pxSlot->ucInitialized == 0U) {
		pxSlot->ucInitialized = 1U;
		pxSlot->lResult = lResult;
		pxSlot->lNativeError = lNativeError;
		pxSlot->xLastEmitTick = xNow;
		ucEmitFirst = 1U;
	} else if ((pxSlot->lResult != lResult) ||
		(pxSlot->lNativeError != lNativeError)) {
		if (pxSlot->ulSuppressedCount != 0U) {
			lSummaryResult = pxSlot->lResult;
			lSummaryNativeError = pxSlot->lNativeError;
			ulSummaryCount = pxSlot->ulSuppressedCount;
			g_xAppCommLogStatus.ulRepeatSummaryCount++;
			ucEmitSummary = 1U;
		}
		pxSlot->lResult = lResult;
		pxSlot->lNativeError = lNativeError;
		pxSlot->ulSuppressedCount = 0U;
		pxSlot->xLastEmitTick = xNow;
		ucEmitFirst = 1U;
	} else {
		pxSlot->ulSuppressedCount++;
		g_xAppCommLogStatus.ulRateLimitedCount++;
		if ((TickType_t)(xNow - pxSlot->xLastEmitTick) >= xPeriod) {
			lSummaryResult = lResult;
			lSummaryNativeError = lNativeError;
			ulSummaryCount = pxSlot->ulSuppressedCount;
			pxSlot->ulSuppressedCount = 0U;
			pxSlot->xLastEmitTick = xNow;
			g_xAppCommLogStatus.ulRepeatSummaryCount++;
			ucEmitSummary = 1U;
		}
	}
	taskEXIT_CRITICAL();
	if (ucEmitSummary != 0U) {
		prvQueueEntry(xSource, lSummaryResult, lSummaryNativeError, 0,
			APP_COMM_LOG_EMISSION_ERROR_SUMMARY, ulSummaryCount);
	}
	if (ucEmitFirst != 0U) {
		prvQueueEntry(xSource, lResult, lNativeError, 0,
			APP_COMM_LOG_EMISSION_ERROR_FIRST, 0U);
	}
}

/**
 * @brief 将结构化事件写入环形队列
 *
 * 临界区内操作。队列满时丢弃并累加 ulDroppedCount。
 * 入队成功后通过 xTaskNotifyGive 唤醒 LOG 任务。
 */
static void prvQueueEntry(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError, int32_t lAuxValue,
	AppCommLogEmission_e xEmission, uint32_t ulRepeatCount)
{
	TaskHandle_t xLogTaskHandle;
	TickType_t xNow;

	xNow = xTaskGetTickCount();
	taskENTER_CRITICAL();
	if (s_usQueueCount < APP_COMM_LOG_QUEUE_LENGTH) {
		s_axPendingEntries[s_usQueueWriteIndex].xTimestamp =
			xNow;
		s_axPendingEntries[s_usQueueWriteIndex].xSource = xSource;
		s_axPendingEntries[s_usQueueWriteIndex].lResult = lResult;
		s_axPendingEntries[s_usQueueWriteIndex].lNativeError =
			lNativeError;
		s_axPendingEntries[s_usQueueWriteIndex].lAuxValue = lAuxValue;
		s_axPendingEntries[s_usQueueWriteIndex].xEmission = xEmission;
		s_axPendingEntries[s_usQueueWriteIndex].ulRepeatCount =
			ulRepeatCount;
		s_usQueueWriteIndex++;
		if (s_usQueueWriteIndex >= APP_COMM_LOG_QUEUE_LENGTH) {
			s_usQueueWriteIndex = 0U;
		}
		s_usQueueCount++;
		g_xAppCommLogStatus.usPendingCount = s_usQueueCount;
		if (s_usQueueCount >
			g_xAppCommLogStatus.usQueueHighWatermark) {
			g_xAppCommLogStatus.usQueueHighWatermark = s_usQueueCount;
		}
	} else {
		g_xAppCommLogStatus.ulDroppedCount++;
	}
	xLogTaskHandle = s_xLogTaskHandle;
	taskEXIT_CRITICAL();
	if (xLogTaskHandle != NULL) {
		xTaskNotifyGive(xLogTaskHandle);
	}
}

/*-----------------------------------------------------------*/
void vAppCommLogGetStatus(AppCommLogStatus_t *pxStatus)
{
	if (pxStatus == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	*pxStatus = g_xAppCommLogStatus;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
static uint8_t prvTakePendingEntry(AppCommLogEntry_t *pxEntry)
{
	uint8_t ucHasEntry;

	taskENTER_CRITICAL();
	ucHasEntry = (s_usQueueCount != 0U) ? 1U : 0U;
	if (ucHasEntry != 0U) {
		*pxEntry = s_axPendingEntries[s_usQueueReadIndex];
		s_usQueueReadIndex++;
		if (s_usQueueReadIndex >= APP_COMM_LOG_QUEUE_LENGTH) {
			s_usQueueReadIndex = 0U;
		}
		s_usQueueCount--;
		g_xAppCommLogStatus.usPendingCount = s_usQueueCount;
	}
	taskEXIT_CRITICAL();
	return ucHasEntry;
}

/**
 * @brief 格式化结构化事件为文本行
 *
 * 输出格式：[Tick][Level][Task][Module] EVENT event=... result=... detail=...\r\n
 * 格式化在 LOG 任务栈（CCM）上执行，后续需由传输层复制到 SRAM 供 DMA 使用。
 */
static uint16_t prvFormatEntry(const AppCommLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usCapacity)
{
	const char *pcResultText;
	const char *pcNativeText;
	uint16_t usLength;

	usLength = 0U;
	usLength = prvAppendText(pucLine, usLength, usCapacity, "[");
	usLength = prvAppendUnsigned(pucLine, usLength, usCapacity,
		(uint32_t)pxEntry->xTimestamp);
	usLength = prvAppendText(pucLine, usLength, usCapacity, "][");
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetLevelText(pxEntry));
	usLength = prvAppendText(pucLine, usLength, usCapacity, "][");
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetTaskText(pxEntry->xSource));
	usLength = prvAppendText(pucLine, usLength, usCapacity, "][");
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetModuleText(pxEntry->xSource));
	usLength = prvAppendText(pucLine, usLength, usCapacity, "] ");
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetEventText(pxEntry->xSource));
	usLength = prvAppendEmission(pxEntry, pucLine, usLength,
		usCapacity);
	usLength = prvAppendText(pucLine, usLength, usCapacity, " result=");
	pcResultText = prvGetResultText(pxEntry->xSource, pxEntry->lResult);
	if (pcResultText != NULL) {
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			pcResultText);
		usLength = prvAppendText(pucLine, usLength, usCapacity, "(");
	}
	usLength = prvAppendSigned(pucLine, usLength, usCapacity,
		pxEntry->lResult);
	if (pcResultText != NULL) {
		usLength = prvAppendText(pucLine, usLength, usCapacity, ")");
	}
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetNativeLabel(pxEntry->xSource, pxEntry->lResult));
	pcNativeText = NULL;
	if (prvIsLwipNative(pxEntry->xSource, pxEntry->lResult) != 0U) {
		pcNativeText = prvGetLwipErrorText(pxEntry->lNativeError);
	} else if (prvIsSocketNative(pxEntry->xSource,
		pxEntry->lResult) != 0U) {
		pcNativeText = prvGetSocketErrorText(pxEntry->lNativeError);
	} else if (pxEntry->xSource == APP_COMM_SOURCE_RUNNING_STATE) {
		pcNativeText = (pxEntry->lNativeError ==
			APP_COMM_RUNNING_STATE_EVENT_CHANGED) ? "CHANGED" :
			"HEARTBEAT";
	}
	if (pcNativeText != NULL) {
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			pcNativeText);
		usLength = prvAppendText(pucLine, usLength, usCapacity, "(");
	}
	usLength = prvAppendSigned(pucLine, usLength, usCapacity,
		pxEntry->lNativeError);
	if (pcNativeText != NULL) {
		usLength = prvAppendText(pucLine, usLength, usCapacity, ")");
	}
	usLength = prvAppendAuxiliary(pxEntry, pucLine, usLength, usCapacity);
	usLength = prvAppendText(pucLine, usLength, usCapacity, "\r\n");
	return usLength;
}

/** @brief 拼接输出策略标记文本（CHANGED/HEARTBEAT/FIRST/REPEAT_SUMMARY） */
static uint16_t prvAppendEmission(const AppCommLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usLength, uint16_t usCapacity)
{
	const char *pcEmissionText;

	pcEmissionText = NULL;
	switch (pxEntry->xEmission) {
	case APP_COMM_LOG_EMISSION_EVENT:
		break;
	case APP_COMM_LOG_EMISSION_STATE_CHANGED:
		pcEmissionText = "CHANGED";
		break;
	case APP_COMM_LOG_EMISSION_STATE_HEARTBEAT:
		pcEmissionText = "HEARTBEAT";
		break;
	case APP_COMM_LOG_EMISSION_ERROR_FIRST:
		pcEmissionText = "FIRST";
		break;
	case APP_COMM_LOG_EMISSION_ERROR_SUMMARY:
		pcEmissionText = "REPEAT_SUMMARY";
		break;
	default:
		pcEmissionText = "UNKNOWN";
		break;
	}
	if (pcEmissionText != NULL) {
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			" event=");
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			pcEmissionText);
	}
	if (pxEntry->xEmission == APP_COMM_LOG_EMISSION_ERROR_SUMMARY) {
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			" repeat=");
		usLength = prvAppendUnsigned(pucLine, usLength, usCapacity,
			pxEntry->ulRepeatCount);
	}
	return usLength;
}

/*-----------------------------------------------------------*/
static uint16_t prvAppendText(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, const char *pcText)
{
	while ((*pcText != '\0') && (usLength < usCapacity)) {
		pucLine[usLength] = (uint8_t)*pcText;
		usLength++;
		pcText++;
	}
	return usLength;
}

/*-----------------------------------------------------------*/
static uint16_t prvAppendUnsigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint32_t ulValue)
{
	uint8_t aucDigits[10];
	uint8_t ucDigitCount;

	ucDigitCount = 0U;
	do {
		aucDigits[ucDigitCount] = (uint8_t)('0' + (ulValue % 10U));
		ucDigitCount++;
		ulValue /= 10U;
	} while ((ulValue != 0U) && (ucDigitCount < sizeof(aucDigits)));
	while ((ucDigitCount != 0U) && (usLength < usCapacity)) {
		ucDigitCount--;
		pucLine[usLength] = aucDigits[ucDigitCount];
		usLength++;
	}
	return usLength;
}

/*-----------------------------------------------------------*/
static uint16_t prvAppendSigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, int32_t lValue)
{
	uint32_t ulMagnitude;

	if (lValue < 0) {
		usLength = prvAppendText(pucLine, usLength, usCapacity, "-");
		ulMagnitude = (uint32_t)(-(lValue + 1)) + 1U;
	} else {
		ulMagnitude = (uint32_t)lValue;
	}
	return prvAppendUnsigned(pucLine, usLength, usCapacity, ulMagnitude);
}

/**
 * @brief 拼接来源特定的辅助诊断字段
 *
 * MILKTEA_FRAME -> quantity_or_value+exception, MILKTEA_FLOW -> detail,
 * RUNNING_STATE → previous，其余 → aux（非零时）。
 */
static uint16_t prvAppendAuxiliary(const AppCommLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usLength, uint16_t usCapacity)
{
	uint32_t ulPacked;

	if (pxEntry->xSource == APP_COMM_SOURCE_MILKTEA_FRAME) {
		ulPacked = (uint32_t)pxEntry->lAuxValue;
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			" quantity_or_value=");
		usLength = prvAppendUnsigned(pucLine, usLength, usCapacity,
			ulPacked >> 8U);
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			" exception=");
		return prvAppendUnsigned(pucLine, usLength, usCapacity,
			ulPacked & 0xFFU);
	}
	if (pxEntry->xSource == APP_COMM_SOURCE_MILKTEA_FLOW) {
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			" detail=");
		return prvAppendSigned(pucLine, usLength, usCapacity,
			pxEntry->lAuxValue);
	}
	if (pxEntry->xSource == APP_COMM_SOURCE_RUNNING_STATE) {
		const char *pcPreviousText;

		pcPreviousText = prvGetResultText(pxEntry->xSource,
			pxEntry->lAuxValue);
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			" previous=");
		if (pcPreviousText != NULL) {
			usLength = prvAppendText(pucLine, usLength, usCapacity,
				pcPreviousText);
			usLength = prvAppendText(pucLine, usLength, usCapacity, "(");
		}
		usLength = prvAppendSigned(pucLine, usLength, usCapacity,
			pxEntry->lAuxValue);
		if (pcPreviousText != NULL) {
			usLength = prvAppendText(pucLine, usLength, usCapacity, ")");
		}
		return usLength;
	}
	if (pxEntry->lAuxValue != 0) {
		usLength = prvAppendText(pucLine, usLength, usCapacity, " aux=");
		usLength = prvAppendSigned(pucLine, usLength, usCapacity,
			pxEntry->lAuxValue);
	}
	return usLength;
}

/**
 * @brief 推导日志等级（D/I/W/E）
 *
 * 规则：心跳→D，HEALTH→I，RUNNING_STATE按结果分级，
 * MILKTEA_FLOW按步骤分级，GUARD→W，后端/初始化失败→E，
 * 默认负值→W、非负→I。
 */
static const char *prvGetLevelText(const AppCommLogEntry_t *pxEntry)
{
	if (pxEntry->xEmission == APP_COMM_LOG_EMISSION_STATE_HEARTBEAT) {
		return "D";
	}
	if ((pxEntry->xSource == APP_COMM_SOURCE_MILKTEA_HEALTH) ||
		(pxEntry->xSource == APP_COMM_SOURCE_ROBOT_HEALTH)) {
		return "I";
	}
	if (pxEntry->xSource == APP_COMM_SOURCE_RUNNING_STATE) {
		if (pxEntry->lNativeError ==
			APP_COMM_RUNNING_STATE_EVENT_HEARTBEAT) {
			return "D";
		}
		if (pxEntry->lResult == 2) {
			return "E";
		}
		if ((pxEntry->lResult == 1) || (pxEntry->lResult == 4)) {
			return "W";
		}
		return "I";
	}
	if (pxEntry->xSource == APP_COMM_SOURCE_MILKTEA_FLOW) {
		switch (pxEntry->lResult) {
		case APP_COMM_MILKTEA_MAKE_REJECTED_EMPTY:
		case APP_COMM_MILKTEA_MAKE_REJECTED_BUSY:
		case APP_COMM_MILKTEA_IDENTIFIER_INVALID:
		case APP_COMM_MILKTEA_ABORT_IGNORED:
			return "W";
		case APP_COMM_MILKTEA_COMPLETE_FAILED:
		case APP_COMM_MILKTEA_ABORT_FAILED:
		case APP_COMM_MILKTEA_EXECUTOR_MISSING:
		case APP_COMM_MILKTEA_PRODUCTION_TIMEOUT:
			return "E";
		default:
			return "I";
		}
	}
	if ((pxEntry->xSource == APP_COMM_SOURCE_MILKTEA_FRAME) &&
		(((uint32_t)pxEntry->lAuxValue & 0xFFU) != 0U)) {
		return "W";
	}
	if (pxEntry->xSource == APP_COMM_SOURCE_MILKTEA_GUARD) {
		return "W";
	}
	if (((pxEntry->xSource == APP_COMM_SOURCE_LOG) ||
		 (pxEntry->xSource == APP_COMM_SOURCE_MODBUS_INIT)) &&
		(pxEntry->lResult < 0)) {
		return "E";
	}
	return (pxEntry->lResult < 0) ? "W" : "I";
}

/** @brief 映射日志来源到所属 FreeRTOS 任务名称 */
static const char *prvGetTaskText(AppCommSource_e xSource)
{
	switch (xSource) {
	case APP_COMM_SOURCE_BOOT:
	case APP_COMM_SOURCE_TASK:
		return "BOOT";
	case APP_COMM_SOURCE_NETWORK:
		return "defaultTask";
	case APP_COMM_SOURCE_MODBUS_INIT:
		return "Modbus";
	case APP_COMM_SOURCE_ROBOT_OPEN:
	case APP_COMM_SOURCE_ROBOT_TX:
	case APP_COMM_SOURCE_ROBOT_RX:
	case APP_COMM_SOURCE_ROBOT_MODBUS:
	case APP_COMM_SOURCE_ROBOT_CONNECT:
	case APP_COMM_SOURCE_ROBOT_CLOSE:
	case APP_COMM_SOURCE_ROBOT_HEALTH:
		return "RobotTcp";
	case APP_COMM_SOURCE_MILKTEA_OPEN:
	case APP_COMM_SOURCE_MILKTEA_REQUEST:
	case APP_COMM_SOURCE_MILKTEA_LINK:
	case APP_COMM_SOURCE_MILKTEA_HEALTH:
	case APP_COMM_SOURCE_MILKTEA_SESSION:
	case APP_COMM_SOURCE_MILKTEA_GUARD:
	case APP_COMM_SOURCE_MILKTEA_FRAME:
	case APP_COMM_SOURCE_MILKTEA_FLOW:
	case APP_COMM_SOURCE_RUNNING_STATE:
		return "MilkTeaTcp";
	case APP_COMM_SOURCE_LOG:
		return "LOG";
	default:
		return "UNKNOWN";
	}
}

/** @brief 映射日志来源到任务内的功能模块名称 */
static const char *prvGetModuleText(AppCommSource_e xSource)
{
	switch (xSource) {
	case APP_COMM_SOURCE_BOOT:
		return "SYSTEM";
	case APP_COMM_SOURCE_TASK:
		return "RTOS";
	case APP_COMM_SOURCE_NETWORK:
		return "NETWORK";
	case APP_COMM_SOURCE_MODBUS_INIT:
		return "MODBUS_CORE";
	case APP_COMM_SOURCE_ROBOT_OPEN:
	case APP_COMM_SOURCE_ROBOT_TX:
	case APP_COMM_SOURCE_ROBOT_RX:
	case APP_COMM_SOURCE_ROBOT_MODBUS:
		return "MODBUS_MASTER";
	case APP_COMM_SOURCE_ROBOT_CONNECT:
	case APP_COMM_SOURCE_ROBOT_CLOSE:
		return "TCP";
	case APP_COMM_SOURCE_ROBOT_HEALTH:
	case APP_COMM_SOURCE_MILKTEA_HEALTH:
		return "HEALTH";
	case APP_COMM_SOURCE_MILKTEA_OPEN:
	case APP_COMM_SOURCE_MILKTEA_REQUEST:
	case APP_COMM_SOURCE_MILKTEA_LINK:
	case APP_COMM_SOURCE_MILKTEA_SESSION:
	case APP_COMM_SOURCE_MILKTEA_GUARD:
	case APP_COMM_SOURCE_MILKTEA_FRAME:
		return "MODBUS_CLIENT";
	case APP_COMM_SOURCE_MILKTEA_FLOW:
		return "WORKFLOW";
	case APP_COMM_SOURCE_RUNNING_STATE:
		return "STATE";
	case APP_COMM_SOURCE_LOG:
		return "OUTPUT";
	default:
		return "UNKNOWN";
	}
}

/** @brief 映射日志来源到事件名称 */
static const char *prvGetEventText(AppCommSource_e xSource)
{
	switch (xSource) {
	case APP_COMM_SOURCE_BOOT:
		return "BOOT";
	case APP_COMM_SOURCE_TASK:
		return "TASK_CREATE";
	case APP_COMM_SOURCE_NETWORK:
		return "STATUS";
	case APP_COMM_SOURCE_MODBUS_INIT:
		return "INIT";
	case APP_COMM_SOURCE_ROBOT_OPEN:
		return "OPEN";
	case APP_COMM_SOURCE_ROBOT_TX:
		return "TX";
	case APP_COMM_SOURCE_ROBOT_RX:
		return "RX";
	case APP_COMM_SOURCE_ROBOT_MODBUS:
		return "TRANSACTION";
	case APP_COMM_SOURCE_MILKTEA_OPEN:
		return "OPEN";
	case APP_COMM_SOURCE_MILKTEA_REQUEST:
		return "REQUEST";
	case APP_COMM_SOURCE_MILKTEA_LINK:
		return "LINK";
	case APP_COMM_SOURCE_MILKTEA_HEALTH:
		return "HEARTBEAT";
	case APP_COMM_SOURCE_LOG:
		return "BACKEND";
	case APP_COMM_SOURCE_ROBOT_CONNECT:
		return "CONNECT";
	case APP_COMM_SOURCE_ROBOT_CLOSE:
		return "CLOSE";
	case APP_COMM_SOURCE_ROBOT_HEALTH:
		return "HEARTBEAT";
	case APP_COMM_SOURCE_MILKTEA_SESSION:
		return "SESSION";
	case APP_COMM_SOURCE_MILKTEA_GUARD:
		return "GUARD";
	case APP_COMM_SOURCE_MILKTEA_FRAME:
		return "FRAME";
	case APP_COMM_SOURCE_MILKTEA_FLOW:
		return "FLOW";
	case APP_COMM_SOURCE_RUNNING_STATE:
		return "RUNNING_STATE";
	default:
		return "UNKNOWN";
	}
}

/** @brief 映射来源×结果值到人类可读文本 */
static const char *prvGetResultText(AppCommSource_e xSource,
	int32_t lResult)
{
	if (xSource == APP_COMM_SOURCE_NETWORK) {
		switch (lResult) {
		case 2:
			return "ONLINE";
		case 1:
			return "INITIALIZED";
		case -1:
			return "OFFLINE";
		case -2:
			return "RECOVERING";
		default:
			return "UNKNOWN";
		}
	}
	if (xSource == APP_COMM_SOURCE_MODBUS_INIT) {
		switch (lResult) {
		case 0:
			return "OK";
		case 1:
			return "ALREADY_INITIALIZED";
		case -1:
			return "TRANSPORT";
		default:
			return "UNKNOWN";
		}
	}
	if ((xSource == APP_COMM_SOURCE_ROBOT_OPEN) ||
		(xSource == APP_COMM_SOURCE_ROBOT_TX) ||
		(xSource == APP_COMM_SOURCE_ROBOT_RX) ||
		(xSource == APP_COMM_SOURCE_ROBOT_MODBUS) ||
		(xSource == APP_COMM_SOURCE_MILKTEA_REQUEST)) {
		switch (lResult) {
		case 0:
			return "OK";
		case -1:
			return "INVALID_ARG";
		case -2:
			return "TIMEOUT";
		case -3:
			return "TRANSPORT";
		case -4:
			return "PROTOCOL";
		case -5:
			return "EXCEPTION";
		default:
			return "UNKNOWN";
		}
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_LINK) {
		switch (lResult) {
		case 1:
			return "CONNECTED";
		case -1:
			return "DISCONNECTED";
		default:
			return "UNKNOWN";
		}
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_HEALTH) {
		switch (lResult) {
		case 0:
			return "WAIT_NETWORK";
		case 1:
			return "WAIT_SERVICE";
		case 2:
			return "CONNECTING";
		case 3:
			return "ONLINE";
		case 4:
			return "RETRY_DELAY";
		default:
			return "UNKNOWN";
		}
	}
	if (xSource == APP_COMM_SOURCE_ROBOT_CONNECT) {
		return (lResult == 1) ? "BEGIN" : "UNKNOWN";
	}
	if (xSource == APP_COMM_SOURCE_ROBOT_CLOSE) {
		switch (lResult) {
		case 1:
			return "BEGIN";
		case 0:
			return "END";
		default:
			return "UNKNOWN";
		}
	}
	if (xSource == APP_COMM_SOURCE_ROBOT_HEALTH) {
		switch (lResult) {
		case 0:
			return "WAIT_NETWORK";
		case 1:
			return "WAIT_SERVICE";
		case 2:
			return "CONNECTING";
		case 3:
			return "ONLINE";
		case 4:
			return "RETRY_DELAY";
		default:
			return "UNKNOWN";
		}
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_SESSION) {
		return (lResult == 1) ? "CONNECTED" :
			((lResult == -1) ? "DISCONNECTED" : "UNKNOWN");
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_GUARD) {
		switch (lResult) {
		case -1:
			return "POOL_FULL";
		case -2:
			return "IDLE_TIMEOUT";
		case -3:
			return "FRAME_TIMEOUT";
		case -4:
			return "SOCKET_ERROR";
		case -5:
			return "ATTACH_ERROR";
		default:
			return "UNKNOWN";
		}
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_FRAME) {
		switch (lResult) {
		case 1:
			return "FC01_READ_COILS";
		case 3:
			return "FC03_READ_HOLDING";
		case 6:
			return "FC06_WRITE_SINGLE";
		case 16:
			return "FC16_WRITE_MULTIPLE";
		default:
			return "UNSUPPORTED_FC";
		}
	}
	if (xSource == APP_COMM_SOURCE_RUNNING_STATE) {
		switch (lResult) {
		case 0:
			return "ONLINE";
		case 1:
			return "WARNING";
		case 2:
			return "ERROR";
		case 3:
			return "STARTUP";
		case 4:
			return "DOWN";
		default:
			return "UNKNOWN";
		}
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_FLOW) {
		switch (lResult) {
		case APP_COMM_MILKTEA_READY:
			return "READY";
		case APP_COMM_MILKTEA_MAKE_ACCEPTED:
			return "MAKE_ACCEPTED";
		case APP_COMM_MILKTEA_MAKE_REJECTED_EMPTY:
			return "MAKE_REJECTED_EMPTY";
		case APP_COMM_MILKTEA_MAKE_REJECTED_BUSY:
			return "MAKE_REJECTED_BUSY";
		case APP_COMM_MILKTEA_MAKE_RECEIVED:
			return "MAKE_RECEIVED";
		case APP_COMM_MILKTEA_IDENTIFIER_VALID:
			return "IDENTIFIER_VALID";
		case APP_COMM_MILKTEA_IDENTIFIER_INVALID:
			return "IDENTIFIER_INVALID";
		case APP_COMM_MILKTEA_EXECUTOR_START:
			return "EXECUTOR_START";
		case APP_COMM_MILKTEA_EXECUTOR_RUNNING:
			return "EXECUTOR_RUNNING";
		case APP_COMM_MILKTEA_EXECUTOR_PROGRESS:
			return "EXECUTOR_PROGRESS";
		case APP_COMM_MILKTEA_COMPLETE_SUCCESS:
			return "COMPLETE_SUCCESS";
		case APP_COMM_MILKTEA_COMPLETE_FAILED:
			return "COMPLETE_FAILED";
		case APP_COMM_MILKTEA_ABORT_ACCEPTED:
			return "ABORT_ACCEPTED";
		case APP_COMM_MILKTEA_ABORT_IGNORED:
			return "ABORT_IGNORED";
		case APP_COMM_MILKTEA_ABORT_REQUEST:
			return "ABORT_REQUEST";
		case APP_COMM_MILKTEA_ABORT_WAIT:
			return "ABORT_WAIT";
		case APP_COMM_MILKTEA_ABORT_COMPLETE:
			return "ABORT_COMPLETE";
		case APP_COMM_MILKTEA_ABORT_FAILED:
			return "ABORT_FAILED";
		case APP_COMM_MILKTEA_EXECUTOR_MISSING:
			return "EXECUTOR_MISSING";
		case APP_COMM_MILKTEA_PRODUCTION_TIMEOUT:
			return "PRODUCTION_TIMEOUT";
		case APP_COMM_MILKTEA_SIMULATION_ENABLED:
			return "SIMULATION_ENABLED";
		case APP_COMM_MILKTEA_SIMULATION_START:
			return "SIMULATION_START";
		case APP_COMM_MILKTEA_SIMULATION_COMPLETE:
			return "SIMULATION_COMPLETE";
		case APP_COMM_MILKTEA_SIMULATION_ABORT:
			return "SIMULATION_ABORT";
		default:
			return "UNKNOWN";
		}
	}
	if ((xSource == APP_COMM_SOURCE_MILKTEA_OPEN) ||
		(xSource == APP_COMM_SOURCE_LOG) ||
		(xSource == APP_COMM_SOURCE_BOOT) ||
		(xSource == APP_COMM_SOURCE_TASK)) {
		return (lResult == 0) ? "OK" : "ERROR";
	}
	return NULL;
}

/** @brief 选择原生错误值的字段标签（flags=/lwip=/errno=/value= 等） */
static const char *prvGetNativeLabel(AppCommSource_e xSource,
	int32_t lResult)
{
	if (xSource == APP_COMM_SOURCE_NETWORK) {
		return " flags=";
	}
	if ((xSource == APP_COMM_SOURCE_ROBOT_OPEN) ||
		(xSource == APP_COMM_SOURCE_ROBOT_TX) ||
		(xSource == APP_COMM_SOURCE_ROBOT_RX)) {
		return " lwip=";
	}
	if (xSource == APP_COMM_SOURCE_ROBOT_MODBUS) {
		if (lResult == 0) {
			return " value=";
		}
		if (lResult == -5) {
			return " exception=";
		}
		return " detail=";
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_OPEN) {
		return (lResult == 0) ? " port=" : " errno=";
	}
	if ((xSource == APP_COMM_SOURCE_MILKTEA_REQUEST) &&
		((lResult == -2) || (lResult == -3))) {
		return " errno=";
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_LINK) {
		return (lResult < 0) ? " lwip=" : " count=";
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_HEALTH) {
		return " flags=";
	}
	if (xSource == APP_COMM_SOURCE_ROBOT_CONNECT) {
		return " attempt=";
	}
	if (xSource == APP_COMM_SOURCE_ROBOT_CLOSE) {
		return (lResult == 1) ? " count=" : " detail=";
	}
	if (xSource == APP_COMM_SOURCE_ROBOT_HEALTH) {
		return " flags=";
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_SESSION) {
		return " slot=";
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_GUARD) {
		return (lResult == -1) ? " active=" :
			((lResult == -4) ? " errno=" :
			((lResult == -5) ? " transport=" : " slot="));
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_FRAME) {
		return " address=";
	}
	if (xSource == APP_COMM_SOURCE_MILKTEA_FLOW) {
		return " order=";
	}
	if (xSource == APP_COMM_SOURCE_RUNNING_STATE) {
		return " event=";
	}
	return " detail=";
}

/** @brief 判断原生错误是否属于 lwIP err_t 域名 */
static uint8_t prvIsLwipNative(AppCommSource_e xSource, int32_t lResult)
{
	if ((xSource == APP_COMM_SOURCE_ROBOT_OPEN) ||
		(xSource == APP_COMM_SOURCE_ROBOT_TX) ||
		(xSource == APP_COMM_SOURCE_ROBOT_RX)) {
		return 1U;
	}
	if ((xSource == APP_COMM_SOURCE_MILKTEA_LINK) && (lResult < 0)) {
		return 1U;
	}
	return 0U;
}

/** @brief 判断原生错误是否属于 socket errno 域名 */
static uint8_t prvIsSocketNative(AppCommSource_e xSource,
	int32_t lResult)
{
	if ((xSource == APP_COMM_SOURCE_MILKTEA_OPEN) && (lResult != 0)) {
		return 1U;
	}
	if ((xSource == APP_COMM_SOURCE_MILKTEA_REQUEST) &&
		(lResult == -3)) {
		return 1U;
	}
	if ((xSource == APP_COMM_SOURCE_MILKTEA_GUARD) &&
		(lResult == -4)) {
		return 1U;
	}
	return 0U;
}

/** @brief 将 socket errno 转换为可读文本（覆盖 15 种常用 errno） */
static const char *prvGetSocketErrorText(int32_t lNativeError)
{
	switch (lNativeError) {
	case 0:
		return "OK";
	case EWOULDBLOCK:
#if (EAGAIN != EWOULDBLOCK)
	case EAGAIN:
#endif
		return "EWOULDBLOCK";
	case ETIMEDOUT:
		return "ETIMEDOUT";
	case ECONNRESET:
		return "ECONNRESET";
	case ECONNABORTED:
		return "ECONNABORTED";
	case ENOTCONN:
		return "ENOTCONN";
	case EPIPE:
		return "EPIPE";
	case ENOMEM:
		return "ENOMEM";
	case ENOBUFS:
		return "ENOBUFS";
	case ENETDOWN:
		return "ENETDOWN";
	case ENETUNREACH:
		return "ENETUNREACH";
	case EHOSTUNREACH:
		return "EHOSTUNREACH";
	case EADDRINUSE:
		return "EADDRINUSE";
	case ENOSYS:
		return "ENOSYS";
	default:
		return "ERRNO_UNKNOWN";
	}
}

/** @brief 将 lwIP err_t 转换为可读文本（覆盖全部 17 种） */
static const char *prvGetLwipErrorText(int32_t lNativeError)
{
	switch ((err_t)lNativeError) {
	case ERR_OK:
		return "ERR_OK";
	case ERR_MEM:
		return "ERR_MEM";
	case ERR_BUF:
		return "ERR_BUF";
	case ERR_TIMEOUT:
		return "ERR_TIMEOUT";
	case ERR_RTE:
		return "ERR_RTE";
	case ERR_INPROGRESS:
		return "ERR_INPROGRESS";
	case ERR_VAL:
		return "ERR_VAL";
	case ERR_WOULDBLOCK:
		return "ERR_WOULDBLOCK";
	case ERR_USE:
		return "ERR_USE";
	case ERR_ALREADY:
		return "ERR_ALREADY";
	case ERR_ISCONN:
		return "ERR_ISCONN";
	case ERR_CONN:
		return "ERR_CONN";
	case ERR_IF:
		return "ERR_IF";
	case ERR_ABRT:
		return "ERR_ABRT";
	case ERR_RST:
		return "ERR_RST";
	case ERR_CLSD:
		return "ERR_CLSD";
	case ERR_ARG:
		return "ERR_ARG";
	default:
		return "ERR_UNKNOWN";
	}
}

#else

/* 日志关闭构建：公共 API 保持安全，有意识地不执行任何操作 */
void vAppCommLogTask(void *pvArgument)
{
	(void)pvArgument;
}

/*-----------------------------------------------------------*/
AppCommLogResult_e xAppCommLogRegisterBackend(
	const AppCommLogBackend_t *pxBackend)
{
	(void)pxBackend;
	return APP_COMM_LOG_RESULT_NOT_READY;
}

/*-----------------------------------------------------------*/
void vAppCommLogWrite(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError)
{
	(void)xSource;
	(void)lResult;
	(void)lNativeError;
}

/*-----------------------------------------------------------*/
void vAppCommLogWriteExtended(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError, int32_t lAuxValue)
{
	(void)xSource;
	(void)lResult;
	(void)lNativeError;
	(void)lAuxValue;
}

/*-----------------------------------------------------------*/
uint8_t ucAppCommLogWriteState(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError)
{
	(void)xSource;
	(void)lResult;
	(void)lNativeError;
	return 0U;
}

/*-----------------------------------------------------------*/
void vAppCommLogWriteRateLimited(AppCommSource_e xSource,
	int32_t lResult, int32_t lNativeError)
{
	(void)xSource;
	(void)lResult;
	(void)lNativeError;
}

/*-----------------------------------------------------------*/
void vAppCommLogGetStatus(AppCommLogStatus_t *pxStatus)
{
	if (pxStatus != NULL) {
		memset(pxStatus, 0, sizeof(*pxStatus));
	}
}

#endif /* APP_COMM_LOG_ENABLE */
