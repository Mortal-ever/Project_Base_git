/**
  * @file      app_log.c
  * @brief     Implement the product-neutral asynchronous application log.
  * @author    WHong
  * @date      2026-08-20
  *
  * @details   The implementation owns one fixed-size CPU-only ring and one
  *            binary signal. Product code owns the output Transport channel
  *            and the task that calls vAppLogTask().
  *
  * @attention
  * - No dynamic memory or product header is used by this module.
  * - The ring keeps the newest records by overwriting the oldest record.
  */

#include "Log/app_log.h"

#include <string.h>

#include "compiler_compat.h"
#include "semphr.h"
#include "task.h"

/** @brief Fixed number of records retained by the overwrite ring. */
#define APP_LOG_RING_LENGTH             32U
/** @brief Maximum event text bytes retained, including the terminator. */
#define APP_LOG_TEXT_LENGTH             72U
/** @brief Maximum field-name bytes retained, including the terminator. */
#define APP_LOG_FIELD_LENGTH            24U
/** @brief Maximum formatted output line length in bytes. */
#define APP_LOG_LINE_LENGTH             160U
/** @brief Bounded Transport send timeout in milliseconds. */
#define APP_LOG_SEND_TIMEOUT_MS         100U
/** @brief Initial delay between retries after a send failure. */
#define APP_LOG_RETRY_BASE_MS           100U
/** @brief Maximum delay between retries after continuous failures. */
#define APP_LOG_RETRY_MAX_MS            1000U

/** @brief Store one queue-owned structured log record. */
typedef struct {
	uint32_t ulSequence;
	AppLogLevel_e xLevel;
	AppLogSourceId_t xSource;
	AppLogOrderId_t usOrderId;
	int32_t lResult;
	int32_t lFieldValue;
	char acText[APP_LOG_TEXT_LENGTH];
	char acFieldName[APP_LOG_FIELD_LENGTH];
	uint8_t ucHasResult;
	uint8_t ucHasField;
} AppLogEntry_t;

/** @brief Global status exposed to the product adapter and debugger. */
APP_CCM_DATA
AppLogStatus_t g_xAppLogStatus;

/** @brief Static overwrite-oldest ring records. */
APP_CCM_DATA
static AppLogEntry_t s_axLogRing[APP_LOG_RING_LENGTH];
/** @brief Next insertion index in the ring. */
APP_CCM_DATA
static uint16_t s_usLogHead;
/** @brief Oldest record index in the ring. */
APP_CCM_DATA
static uint16_t s_usLogTail;
/** @brief Number of valid records currently retained. */
APP_CCM_DATA
static uint16_t s_usLogCount;
/** @brief Monotonic record sequence used by peek and commit. */
APP_CCM_DATA
static uint32_t s_ulLogSequence;
/** @brief Static binary signal coalescing producer wakeups. */
APP_CCM_DATA
static SemaphoreHandle_t s_xLogSignal;
/** @brief Static binary signal control storage. */
APP_CCM_DATA
static StaticSemaphore_t s_xLogSignalStorage;
/** @brief Product-owned Transport channel used for output. */
static TransportChannel_t *s_pxLogChannel;
/** @brief Product source table stored in read-only memory. */
static const AppLogSourceDescriptor_t *s_pxSourceTable;
/** @brief Number of entries in the product source table. */
static uint8_t s_ucSourceCount;
/** @brief Nonzero when a Transport channel can be retried. */
APP_CCM_DATA
static uint8_t s_ucTransportConfigured;
/** @brief Nonzero when the output channel is currently open. */
APP_CCM_DATA
static uint8_t s_ucTransportReady;
/** @brief Current bounded retry delay in milliseconds. */
APP_CCM_DATA
static uint32_t s_ulRetryDelayMs;

/**
  * @brief  Append bounded text to an output line.
  * @param[in,out] pucLine Destination line buffer.
  * @param[in]     usLength Current line length.
  * @param[in]     usCapacity Total line capacity.
  * @param[in]     pcText Null-terminated text, or NULL.
  * @retval Updated line length, never greater than usCapacity.
  */
static uint16_t prvAppendText(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, const char *pcText);
/**
  * @brief  Append one unsigned decimal value to an output line.
  * @param[in,out] pucLine Destination line buffer.
  * @param[in]     usLength Current line length.
  * @param[in]     usCapacity Total line capacity.
  * @param[in]     ulValue Value to format.
  * @retval Updated line length, never greater than usCapacity.
  */
static uint16_t prvAppendUnsigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint32_t ulValue);
/**
  * @brief  Append one fixed-width uppercase hexadecimal value.
  * @param[in,out] pucLine Destination line buffer.
  * @param[in] usLength Current line length.
  * @param[in] usCapacity Total line capacity.
  * @param[in] usValue Value to format.
  * @retval Updated line length, never greater than usCapacity.
  */
static uint16_t prvAppendHex16(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint16_t usValue);
/**
  * @brief  Append one signed decimal value to an output line.
  * @param[in,out] pucLine Destination line buffer.
  * @param[in]     usLength Current line length.
  * @param[in]     usCapacity Total line capacity.
  * @param[in]     lValue Value to format.
  * @retval Updated line length, never greater than usCapacity.
  */
static uint16_t prvAppendSigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, int32_t lValue);
/**
  * @brief  Format one record as the stable structured log line.
  * @param[in]     pxEntry Record to format.
  * @param[out]    pucLine Output buffer.
  * @param[in]     usCapacity Output buffer capacity.
  * @retval Formatted length including CRLF.
  */
static uint16_t prvFormatEntry(const AppLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usCapacity);
/**
  * @brief  Return the stable text for one log level.
  * @param[in]     xLevel Log level.
  * @retval Static level text, or INVALID for an unknown level.
  */
static const char *prvGetLevelText(AppLogLevel_e xLevel);
/**
  * @brief  Return the task label for one product source.
  * @param[in]     xSource Product source identifier.
  * @retval Static task label, or INVALID for an unknown source.
  */
static const char *prvGetTaskText(AppLogSourceId_t xSource);
/**
  * @brief  Return the module label for one product source.
  * @param[in]     xSource Product source identifier.
  * @retval Static module label, or INVALID for an unknown source.
  */
static const char *prvGetModuleText(AppLogSourceId_t xSource);
/**
  * @brief  Copy text into a fixed-size record field.
  * @param[out]    pcDestination Destination field.
  * @param[in]     usCapacity Destination capacity including NULL.
  * @param[in]     pcSource Source text, or NULL.
  */
static void prvCopyText(char *pcDestination, uint16_t usCapacity,
	const char *pcSource);
/** @brief Update pending count and the high-water mark in the locked state. */
static void prvUpdateRingDepthLocked(void);
/** @brief Copy the oldest record and its sequence under a short critical section. */
static BaseType_t prvPeekEntry(AppLogEntry_t *pxEntry, uint32_t *pulSequence);
/** @brief Remove the peeked record only when its sequence still matches. */
static BaseType_t prvCommitEntry(uint32_t ulSequence);
/** @brief Read the Transport-ready flag atomically. */
static uint8_t prvIsTransportReady(void);
/** @brief Reopen the configured Transport channel after a bounded delay. */
static uint8_t prvTryOpenTransport(void);
/** @brief Update counters and retry state after one send attempt. */
static void prvRecordTransportResult(TransportResult_e xResult,
	uint8_t ucCommitted);

/*-----------------------------------------------------------*/
AppLogResult_e xAppLogInit(const AppLogConfig_t *pxConfig)
{
	TransportResult_e xTransportResult;

	if (pxConfig == NULL) {
		return APP_LOG_RESULT_INVALID_ARG;
	}
	if ((pxConfig->pxSourceTable == NULL) ||
		(pxConfig->ucSourceCount == 0U)) {
		return APP_LOG_RESULT_INVALID_ARG;
	}
	if (g_xAppLogStatus.ucInitialized != 0U) {
		return APP_LOG_RESULT_ALREADY_INITIALIZED;
	}

	memset(&g_xAppLogStatus, 0, sizeof(g_xAppLogStatus));
	memset(s_axLogRing, 0, sizeof(s_axLogRing));
	s_usLogHead = 0U;
	s_usLogTail = 0U;
	s_usLogCount = 0U;
	s_ulLogSequence = 0U;
	s_pxSourceTable = pxConfig->pxSourceTable;
	s_ucSourceCount = pxConfig->ucSourceCount;
	s_pxLogChannel = pxConfig->pxTransportChannel;
	s_ucTransportConfigured =
		((pxConfig->ucEnableTransport != 0U) &&
		 (s_pxLogChannel != NULL)) ? 1U : 0U;
	s_ucTransportReady = 0U;
	s_ulRetryDelayMs = APP_LOG_RETRY_BASE_MS;
	s_xLogSignal = xSemaphoreCreateBinaryStatic(&s_xLogSignalStorage);
	if (s_xLogSignal == NULL) {
		return APP_LOG_RESULT_NOT_READY;
	}
	g_xAppLogStatus.ucInitialized = 1U;
	g_xAppLogStatus.ucBufferReady = 1U;
	if (s_ucTransportConfigured == 0U) {
		g_xAppLogStatus.ucOutputPaused = 1U;
		g_xAppLogStatus.lLastTransportError =
			(int32_t)TRANSPORT_RESULT_NOT_READY;
		return APP_LOG_RESULT_TRANSPORT;
	}

	xTransportResult = xTransportOpen(s_pxLogChannel);
	if (xTransportResult != TRANSPORT_RESULT_OK) {
		g_xAppLogStatus.lLastTransportError = (int32_t)xTransportResult;
		g_xAppLogStatus.ucOutputPaused = 1U;
		return APP_LOG_RESULT_TRANSPORT;
	}

	s_ucTransportReady = 1U;
	g_xAppLogStatus.ucTransportReady = 1U;
	return APP_LOG_RESULT_OK;
}

/*-----------------------------------------------------------*/
AppLogResult_e xAppLogWrite(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, const char *pcText, int32_t lCode)
{
	return xAppLogWriteOrder(xLevel, xSource, 0U, pcText, lCode);
}

/*-----------------------------------------------------------*/
AppLogResult_e xAppLogWriteOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText, int32_t lCode)
{
	return xAppLogWriteFieldOrder(xLevel, xSource, usOrderId, pcText,
		lCode, NULL, 0);
}

/*-----------------------------------------------------------*/
AppLogResult_e xAppLogWriteField(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue)
{
	return xAppLogWriteFieldOrder(xLevel, xSource, 0U, pcText, lResult,
		pcFieldName, lFieldValue);
}

/*-----------------------------------------------------------*/
AppLogResult_e xAppLogWriteFieldOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText, int32_t lResult, const char *pcFieldName,
	int32_t lFieldValue)
{
	AppLogEntry_t xEntry;

	if ((pcText == NULL) ||
		((uint32_t)xLevel >= (uint32_t)APP_LOG_LEVEL_COUNT) ||
		((uint32_t)xSource >= (uint32_t)s_ucSourceCount)) {
		return APP_LOG_RESULT_INVALID_ARG;
	}
	if ((g_xAppLogStatus.ucInitialized == 0U) ||
		(g_xAppLogStatus.ucBufferReady == 0U)) {
		return APP_LOG_RESULT_NOT_READY;
	}

	memset(&xEntry, 0, sizeof(xEntry));
	xEntry.xLevel = xLevel;
	xEntry.xSource = xSource;
	xEntry.usOrderId = usOrderId;
	xEntry.lResult = lResult;
	xEntry.lFieldValue = lFieldValue;
	xEntry.ucHasResult = 1U;
	prvCopyText(xEntry.acText, APP_LOG_TEXT_LENGTH, pcText);
	if ((pcFieldName != NULL) && (pcFieldName[0] != '\0')) {
		prvCopyText(xEntry.acFieldName, APP_LOG_FIELD_LENGTH, pcFieldName);
		xEntry.ucHasField = 1U;
	}

	taskENTER_CRITICAL();
	s_ulLogSequence++;
	if (s_ulLogSequence == 0U) {
		s_ulLogSequence = 1U;
	}
	xEntry.ulSequence = s_ulLogSequence;
	s_axLogRing[s_usLogHead] = xEntry;
	s_usLogHead++;
	if (s_usLogHead >= APP_LOG_RING_LENGTH) {
		s_usLogHead = 0U;
	}
	if (s_usLogCount < APP_LOG_RING_LENGTH) {
		s_usLogCount++;
	} else {
		s_usLogTail++;
		if (s_usLogTail >= APP_LOG_RING_LENGTH) {
			s_usLogTail = 0U;
		}
		g_xAppLogStatus.ulDroppedCount++;
	}
	g_xAppLogStatus.ulQueuedCount++;
	prvUpdateRingDepthLocked();
	taskEXIT_CRITICAL();
	if (s_xLogSignal != NULL) {
		(void)xSemaphoreGive(s_xLogSignal);
	}
	return APP_LOG_RESULT_OK;
}

/*-----------------------------------------------------------*/
AppLogResult_e xAppLogWriteTextOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText)
{
	AppLogEntry_t xEntry;

	if ((pcText == NULL) ||
		((uint32_t)xLevel >= (uint32_t)APP_LOG_LEVEL_COUNT) ||
		((uint32_t)xSource >= (uint32_t)s_ucSourceCount)) {
		return APP_LOG_RESULT_INVALID_ARG;
	}
	if ((g_xAppLogStatus.ucInitialized == 0U) ||
		(g_xAppLogStatus.ucBufferReady == 0U)) {
		return APP_LOG_RESULT_NOT_READY;
	}
	memset(&xEntry, 0, sizeof(xEntry));
	xEntry.xLevel = xLevel;
	xEntry.xSource = xSource;
	xEntry.usOrderId = usOrderId;
	prvCopyText(xEntry.acText, APP_LOG_TEXT_LENGTH, pcText);
	taskENTER_CRITICAL();
	s_ulLogSequence++;
	if (s_ulLogSequence == 0U) {
		s_ulLogSequence = 1U;
	}
	xEntry.ulSequence = s_ulLogSequence;
	s_axLogRing[s_usLogHead] = xEntry;
	s_usLogHead++;
	if (s_usLogHead >= APP_LOG_RING_LENGTH) {
		s_usLogHead = 0U;
	}
	if (s_usLogCount < APP_LOG_RING_LENGTH) {
		s_usLogCount++;
	} else {
		s_usLogTail++;
		if (s_usLogTail >= APP_LOG_RING_LENGTH) {
			s_usLogTail = 0U;
		}
		g_xAppLogStatus.ulDroppedCount++;
	}
	g_xAppLogStatus.ulQueuedCount++;
	prvUpdateRingDepthLocked();
	taskEXIT_CRITICAL();
	if (s_xLogSignal != NULL) {
		(void)xSemaphoreGive(s_xLogSignal);
	}
	return APP_LOG_RESULT_OK;
}

/*-----------------------------------------------------------*/
int32_t lAppLogEarlyWrite(const uint8_t *pucData, uint16_t usLength)
{
	TransportResult_e xResult;

	if ((pucData == NULL) || (usLength == 0U) ||
		(g_xAppLogStatus.ucInitialized == 0U)) {
		return (int32_t)TRANSPORT_RESULT_INVALID_ARG;
	}
	if (prvIsTransportReady() == 0U) {
		return g_xAppLogStatus.lLastTransportError;
	}
	xResult = xTransportSend(s_pxLogChannel, pucData, usLength,
		APP_LOG_SEND_TIMEOUT_MS);
	return (int32_t)xResult;
}

/*-----------------------------------------------------------*/
void vAppLogTask(void *pvArgument)
{
	AppLogEntry_t xEntry;
	TransportResult_e xTransportResult;
	uint8_t aucLine[APP_LOG_LINE_LENGTH];
	uint16_t usLength;
	uint32_t ulSequence;
	uint8_t ucTransportReady;
	BaseType_t xCommitted;

	(void)pvArgument;
	taskENTER_CRITICAL();
	g_xAppLogStatus.ucTaskReady = 1U;
	taskEXIT_CRITICAL();
	for (;;) {
		if (prvPeekEntry(&xEntry, &ulSequence) != pdPASS) {
			if (s_xLogSignal != NULL) {
				(void)xSemaphoreTake(s_xLogSignal, portMAX_DELAY);
			}
			continue;
		}
		ucTransportReady = prvIsTransportReady();
		if (ucTransportReady == 0U) {
			(void)prvTryOpenTransport();
			ucTransportReady = prvIsTransportReady();
		}
		if (ucTransportReady == 0U) {
			taskENTER_CRITICAL();
			g_xAppLogStatus.ucOutputPaused = 1U;
			taskEXIT_CRITICAL();
			vTaskDelay(pdMS_TO_TICKS(s_ulRetryDelayMs));
			continue;
		}
		usLength = prvFormatEntry(&xEntry, aucLine, APP_LOG_LINE_LENGTH);
		xTransportResult = xTransportSend(s_pxLogChannel, aucLine,
			usLength, APP_LOG_SEND_TIMEOUT_MS);
		if (xTransportResult == TRANSPORT_RESULT_OK) {
			xCommitted = prvCommitEntry(ulSequence);
			prvRecordTransportResult(xTransportResult,
				(xCommitted == pdPASS) ? 1U : 0U);
		} else {
			prvRecordTransportResult(xTransportResult, 0U);
			vTaskDelay(pdMS_TO_TICKS(s_ulRetryDelayMs));
		}
	}
}

/*-----------------------------------------------------------*/
void vAppLogSetTaskReady(uint8_t ucCreated)
{
	taskENTER_CRITICAL();
	g_xAppLogStatus.ucTaskReady = (ucCreated != 0U) ? 1U : 0U;
	if (ucCreated == 0U) {
		g_xAppLogStatus.ucOutputPaused = 1U;
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
void vAppLogGetStatus(AppLogStatus_t *pxStatus)
{
	if (pxStatus == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	*pxStatus = g_xAppLogStatus;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
static uint16_t prvFormatEntry(const AppLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usCapacity)
{
	uint16_t usLength;

	if ((pxEntry == NULL) || (pucLine == NULL) || (usCapacity == 0U)) {
		return 0U;
	}
	usLength = 0U;
	usLength = prvAppendText(pucLine, usLength, usCapacity, "[");
	usLength = prvAppendHex16(pucLine, usLength, usCapacity,
		pxEntry->usOrderId);
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetLevelText(pxEntry->xLevel));
	usLength = prvAppendText(pucLine, usLength, usCapacity, "][");
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetTaskText(pxEntry->xSource));
	usLength = prvAppendText(pucLine, usLength, usCapacity, ":");
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetModuleText(pxEntry->xSource));
	usLength = prvAppendText(pucLine, usLength, usCapacity, "] ");
	usLength = prvAppendText(pucLine, usLength, usCapacity, pxEntry->acText);
	if (pxEntry->ucHasResult != 0U) {
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			" result=");
		usLength = prvAppendSigned(pucLine, usLength, usCapacity,
			pxEntry->lResult);
	}
	if ((pxEntry->ucHasResult != 0U) &&
		(pxEntry->ucHasField != 0U)) {
		usLength = prvAppendText(pucLine, usLength, usCapacity, " ");
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			pxEntry->acFieldName);
		usLength = prvAppendText(pucLine, usLength, usCapacity, "=");
		usLength = prvAppendSigned(pucLine, usLength, usCapacity,
			pxEntry->lFieldValue);
	}
	usLength = prvAppendText(pucLine, usLength, usCapacity, "\r\n");
	return usLength;
}

/*-----------------------------------------------------------*/
static uint16_t prvAppendText(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, const char *pcText)
{
	if (pcText == NULL) {
		return usLength;
	}
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
	char acDigits[10];
	uint8_t ucCount;

	ucCount = 0U;
	do {
		acDigits[ucCount] = (char)('0' + (ulValue % 10U));
		ucCount++;
		ulValue /= 10U;
	} while ((ulValue != 0U) && (ucCount < (uint8_t)sizeof(acDigits)));

	while ((ucCount > 0U) && (usLength < usCapacity)) {
		ucCount--;
		pucLine[usLength] = (uint8_t)acDigits[ucCount];
		usLength++;
	}
	return usLength;
}

/*-----------------------------------------------------------*/
static uint16_t prvAppendHex16(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint16_t usValue)
{
	static const char acHex[] = "0123456789ABCDEF";
	uint8_t ucIndex;

	for (ucIndex = 0U; ucIndex < 4U; ucIndex++) {
		if (usLength >= usCapacity) {
			break;
		}
		pucLine[usLength] = (uint8_t)acHex[
			(usValue >> (uint8_t)(12U - (ucIndex * 4U))) & 0x0FU];
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
		ulMagnitude = (uint32_t)(-(lValue + 1));
		ulMagnitude++;
	} else {
		ulMagnitude = (uint32_t)lValue;
	}
	return prvAppendUnsigned(pucLine, usLength, usCapacity, ulMagnitude);
}

/*-----------------------------------------------------------*/
static const char *prvGetLevelText(AppLogLevel_e xLevel)
{
	static const char * const apcLevels[APP_LOG_LEVEL_COUNT] = {
		"INFO", "WARN", "ERROR"
	};

	if ((uint32_t)xLevel >= (uint32_t)APP_LOG_LEVEL_COUNT) {
		return "INVALID";
	}
	return apcLevels[xLevel];
}

/*-----------------------------------------------------------*/
static const char *prvGetTaskText(AppLogSourceId_t xSource)
{
	if ((s_pxSourceTable == NULL) ||
		((uint32_t)xSource >= (uint32_t)s_ucSourceCount) ||
		(s_pxSourceTable[xSource].pcTaskName == NULL)) {
		return "INVALID";
	}
	return s_pxSourceTable[xSource].pcTaskName;
}

/*-----------------------------------------------------------*/
static const char *prvGetModuleText(AppLogSourceId_t xSource)
{
	if ((s_pxSourceTable == NULL) ||
		((uint32_t)xSource >= (uint32_t)s_ucSourceCount) ||
		(s_pxSourceTable[xSource].pcModuleName == NULL)) {
		return "INVALID";
	}
	return s_pxSourceTable[xSource].pcModuleName;
}

/*-----------------------------------------------------------*/
static void prvCopyText(char *pcDestination, uint16_t usCapacity,
	const char *pcSource)
{
	uint16_t usIndex;

	if ((pcDestination == NULL) || (usCapacity == 0U)) {
		return;
	}
	if (pcSource == NULL) {
		pcDestination[0] = '\0';
		return;
	}
	usIndex = 0U;
	while ((pcSource[usIndex] != '\0') &&
		(usIndex < (uint16_t)(usCapacity - 1U))) {
		pcDestination[usIndex] = pcSource[usIndex];
		usIndex++;
	}
	pcDestination[usIndex] = '\0';
}

/*-----------------------------------------------------------*/
static void prvUpdateRingDepthLocked(void)
{
	g_xAppLogStatus.usPendingCount = s_usLogCount;
	if (s_usLogCount > g_xAppLogStatus.usQueueHighWatermark) {
		g_xAppLogStatus.usQueueHighWatermark = s_usLogCount;
	}
}

/*-----------------------------------------------------------*/
static BaseType_t prvPeekEntry(AppLogEntry_t *pxEntry,
	uint32_t *pulSequence)
{
	BaseType_t xResult;

	if ((pxEntry == NULL) || (pulSequence == NULL)) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if (s_usLogCount == 0U) {
		xResult = pdFAIL;
	} else {
		*pxEntry = s_axLogRing[s_usLogTail];
		*pulSequence = pxEntry->ulSequence;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
static BaseType_t prvCommitEntry(uint32_t ulSequence)
{
	BaseType_t xResult;

	taskENTER_CRITICAL();
	if ((s_usLogCount == 0U) ||
		(s_axLogRing[s_usLogTail].ulSequence != ulSequence)) {
		xResult = pdFAIL;
	} else {
		s_usLogTail++;
		if (s_usLogTail >= APP_LOG_RING_LENGTH) {
			s_usLogTail = 0U;
		}
		s_usLogCount--;
		prvUpdateRingDepthLocked();
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
static uint8_t prvIsTransportReady(void)
{
	uint8_t ucReady;

	taskENTER_CRITICAL();
	ucReady = s_ucTransportReady;
	taskEXIT_CRITICAL();
	return ucReady;
}

/*-----------------------------------------------------------*/
static uint8_t prvTryOpenTransport(void)
{
	TransportResult_e xResult;

	if ((s_ucTransportConfigured == 0U) ||
		(s_pxLogChannel == NULL)) {
		return 0U;
	}
	xResult = xTransportOpen(s_pxLogChannel);
	if (xResult == TRANSPORT_RESULT_OK) {
		taskENTER_CRITICAL();
		s_ucTransportReady = 1U;
		g_xAppLogStatus.ucTransportReady = 1U;
		g_xAppLogStatus.ucOutputPaused = 0U;
		g_xAppLogStatus.lLastTransportError =
			(int32_t)TRANSPORT_RESULT_OK;
		taskEXIT_CRITICAL();
		return 1U;
	}
	taskENTER_CRITICAL();
	g_xAppLogStatus.lLastTransportError = (int32_t)xResult;
	g_xAppLogStatus.ucOutputPaused = 1U;
	taskEXIT_CRITICAL();
	return 0U;
}

/*-----------------------------------------------------------*/
static void prvRecordTransportResult(TransportResult_e xResult,
	uint8_t ucCommitted)
{
	taskENTER_CRITICAL();
	g_xAppLogStatus.lLastTransportError = (int32_t)xResult;
	if (xResult == TRANSPORT_RESULT_OK) {
		if (ucCommitted != 0U) {
			g_xAppLogStatus.ulSentCount++;
		}
		s_ulRetryDelayMs = APP_LOG_RETRY_BASE_MS;
		g_xAppLogStatus.ucOutputPaused = 0U;
		g_xAppLogStatus.ucTransportReady = 1U;
		s_ucTransportReady = 1U;
	} else {
		g_xAppLogStatus.ulTransmitFailureCount++;
		g_xAppLogStatus.ulRetryCount++;
		g_xAppLogStatus.ucOutputPaused = 1U;
		if ((xResult == TRANSPORT_RESULT_NOT_OPEN) ||
			(xResult == TRANSPORT_RESULT_NOT_READY)) {
			s_ucTransportReady = 0U;
			g_xAppLogStatus.ucTransportReady = 0U;
		}
		if (s_ulRetryDelayMs < APP_LOG_RETRY_MAX_MS) {
			s_ulRetryDelayMs *= 2U;
			if (s_ulRetryDelayMs > APP_LOG_RETRY_MAX_MS) {
				s_ulRetryDelayMs = APP_LOG_RETRY_MAX_MS;
			}
		}
	}
	taskEXIT_CRITICAL();
}
