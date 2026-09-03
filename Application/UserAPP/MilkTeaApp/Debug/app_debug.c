/**
  * @file      app_debug.c
  * @brief     Implement real Modbus TCP calls from UART Debug commands.
  * @author    WHong
  * @date      2026-07-28
  */

#include "app_debug.h"

#include <stdbool.h>
#include <string.h>

#include "app_comm_log_port.h"
#include "app_debug_config.h"
#include "task.h"

/** @brief Maximum token count accepted in one Debug command. */
#define APP_DEBUG_MAX_TOKENS                8U
/** @brief Maximum FC01 quantity retained by the static scratch array. */
#define APP_DEBUG_MAX_READ_COILS          100U
/** @brief Maximum FC03 quantity retained by the static scratch array. */
#define APP_DEBUG_MAX_READ_REGISTERS      125U
/** @brief Maximum FC16 quantity accepted from one Debug command. */
#define APP_DEBUG_MAX_WRITE_REGISTERS      30U

/** @brief Define internal hexadecimal parameter parsing results. */
typedef enum {
	APP_DEBUG_PARSE_OK = 0, /*!< Input text was parsed successfully. */
	APP_DEBUG_PARSE_PARAM_FORMAT = -1 /*!< Input format or range is invalid. */
} AppDebugParseResult_e;

/** @brief Registered device descriptors copied during initialization. */
static AppDebugTcpDevice_t s_axDevices[APP_DEBUG_DEVICE_COUNT];
/** @brief Number of valid entries in s_axDevices. */
static uint8_t s_ucDeviceCount;
/** @brief In-progress null-terminated UART command line. */
static char s_acLine[APP_DEBUG_LINE_LENGTH];
/** @brief Number of command bytes currently stored in s_acLine. */
static uint16_t s_usLineLength;
/** @brief Nonzero after an input line exceeds s_acLine capacity. */
static uint8_t s_ucLineOverflow;
/** @brief Bounded chunk used for each UART receive operation. */
static uint8_t s_aucRxChunk[APP_DEBUG_RX_CHUNK_LENGTH];
/** @brief Shared formatted output buffer owned by the Debug task. */
static char s_acOutput[APP_DEBUG_OUTPUT_LENGTH];
/** @brief Static FC01 result scratch array. */
static bool s_abCoils[APP_DEBUG_MAX_READ_COILS];
/** @brief Static FC03 result scratch array. */
static uint16_t s_ausRegisters[APP_DEBUG_MAX_READ_REGISTERS];
/** @brief Static FC16 parsed-value scratch array. */
static uint16_t s_ausWriteRegisters[APP_DEBUG_MAX_WRITE_REGISTERS];
/** @brief Stable frame snapshot used while formatting transaction output. */
static ModbusPortTrace_t s_xDebugSnapshot;

/** @brief Consume one UART byte and finalize complete command lines. */
static void prvConsumeByte(uint8_t ucByte);
/** @brief Validate and execute one null-terminated Debug command line. */
static void prvProcessLine(char *pcLine);
/** @brief Split one command line into bounded in-place token pointers. */
static uint8_t prvSplit(char *pcLine, char **ppcTokens,
	uint8_t ucTokenCapacity);
/** @brief Find a registered Debug device by its stable command name. */
static AppDebugTcpDevice_t *prvFindDevice(const char *pcName);
/** @brief Parse one unsigned 16-bit hexadecimal parameter. */
static AppDebugParseResult_e prvParseHex(const char *pcText,
	uint16_t *pusValue);
/** @brief Parse an underscore-delimited list of hexadecimal values. */
static AppDebugParseResult_e prvParseValues(char *pcText,
	uint16_t *pusValues, uint16_t usCapacity, uint16_t *pusCount);
/** @brief Map an FC command token to its APP_DEBUG_FC* permission bit. */
static uint32_t prvFunctionMask(const char *pcFunction);
/** @brief Execute one validated Modbus operation for a registered device. */
static ModbusPortResult_e prvExecute(AppDebugTcpDevice_t *pxDevice,
	uint8_t ucUnitId, uint32_t ulFunction, char **ppcTokens,
	uint8_t ucTokenCount, const char **ppcError);
/** @brief Publish the latest completed transaction frames. */
static void prvPublishTransaction(const char *pcName,
	const ModbusPortTrace_t *pxTrace);
/** @brief Format and publish one TX or RX frame. */
static void prvPublishFrame(const char *pcName, const char *pcEvent,
	const ModbusPortFrame_t *pxFrame);
/** @brief Format and publish one Debug command error line. */
static void prvPublishError(const char *pcName, const char *pcError);
/** @brief Append bounded null-terminated text to the output buffer. */
static uint16_t prvAppendText(char *pcOutput, uint16_t usOffset,
	uint16_t usCapacity, const char *pcText);
/** @brief Append an unsigned decimal value to the output buffer. */
static uint16_t prvAppendU32(char *pcOutput, uint16_t usOffset,
	uint16_t usCapacity, uint32_t ulValue);
/** @brief Append one uppercase two-digit hexadecimal byte. */
static uint16_t prvAppendHexByte(char *pcOutput, uint16_t usOffset,
	uint16_t usCapacity, uint8_t ucValue);
/** @brief Send the prepared Debug output through the shared log UART. */
static void prvSendOutput(uint16_t usLength);

/*-----------------------------------------------------------*/
void vAppDebugInit(void)
{
	memset(s_axDevices, 0, sizeof(s_axDevices));
	s_ucDeviceCount = 0U;
	s_usLineLength = 0U;
	s_ucLineOverflow = 0U;
}

/*-----------------------------------------------------------*/
AppDebugResult_e xAppDebugRegisterTcp(
	const AppDebugTcpDevice_t *pxDevice)
{
	uint8_t ucIndex;

	if ((pxDevice == NULL) || (pxDevice->pcName == NULL) ||
		(pxDevice->pxClient == NULL) || (pxDevice->pxTrace == NULL) ||
		(pxDevice->xMutex == NULL) || (pxDevice->pxIsReady == NULL) ||
		(pxDevice->ulFunctionMask == 0U)) {
		return APP_DEBUG_RESULT_INVALID_ARG;
	}
	for (ucIndex = 0U; ucIndex < s_ucDeviceCount; ucIndex++) {
		if (strcmp(s_axDevices[ucIndex].pcName,
			pxDevice->pcName) == 0) {
			return APP_DEBUG_RESULT_ALREADY_REGISTERED;
		}
	}
	if (s_ucDeviceCount >= APP_DEBUG_DEVICE_COUNT) {
		return APP_DEBUG_RESULT_NO_RESOURCE;
	}
	s_axDevices[s_ucDeviceCount] = *pxDevice;
	s_ucDeviceCount++;
	return APP_DEBUG_RESULT_OK;
}

/*-----------------------------------------------------------*/
void vAppDebugTask(void *pvArgument)
{
	TransportResult_e xResult;
	uint16_t usReceived;
	uint16_t usIndex;

	(void)pvArgument;
#if (APP_DEBUG_ENABLE != 0U)
	for (;;) {
		usReceived = 0U;
		xResult = xAppCommLogPortRead(s_aucRxChunk,
			sizeof(s_aucRxChunk), &usReceived,
			APP_DEBUG_UART_WAIT_MS);
		if (xResult == TRANSPORT_RESULT_OK) {
			for (usIndex = 0U; usIndex < usReceived; usIndex++) {
				prvConsumeByte(s_aucRxChunk[usIndex]);
			}
		} else if (xResult != TRANSPORT_RESULT_TIMEOUT) {
			vTaskDelay(pdMS_TO_TICKS(APP_DEBUG_UART_WAIT_MS));
		}
	}
#else
	vTaskDelete(NULL);
#endif
}

/*-----------------------------------------------------------*/
static void prvConsumeByte(uint8_t ucByte)
{
	if ((ucByte == '\r') || (ucByte == '\n')) {
		if (s_ucLineOverflow != 0U) {
			prvPublishError("debug", "FRAME_TOO_LONG");
		} else if (s_usLineLength > 0U) {
			s_acLine[s_usLineLength] = '\0';
			prvProcessLine(s_acLine);
		}
		s_usLineLength = 0U;
		s_ucLineOverflow = 0U;
		return;
	}
	if (s_ucLineOverflow != 0U) {
		return;
	}
	if (s_usLineLength >= (APP_DEBUG_LINE_LENGTH - 1U)) {
		s_ucLineOverflow = 1U;
		return;
	}
	s_acLine[s_usLineLength] = (char)ucByte;
	s_usLineLength++;
}

/*-----------------------------------------------------------*/
static void prvProcessLine(char *pcLine)
{
	char *apcTokens[APP_DEBUG_MAX_TOKENS];
	AppDebugTcpDevice_t *pxDevice;
	AppDebugParseResult_e xParseResult;
	ModbusPortResult_e xResult;
	const char *pcError;
	uint32_t ulFunction;
	uint16_t usUnitId;
	uint8_t ucTokenCount;

	ucTokenCount = prvSplit(pcLine, apcTokens, APP_DEBUG_MAX_TOKENS);
	if (ucTokenCount < 3U) {
		prvPublishError((ucTokenCount > 0U) ? apcTokens[0] : "debug",
			"PARAM_COUNT_MISMATCH");
		return;
	}
	pxDevice = prvFindDevice(apcTokens[0]);
	if (pxDevice == NULL) {
		prvPublishError(apcTokens[0], "UNKNOWN_PROTOCOL");
		return;
	}
	ulFunction = prvFunctionMask(apcTokens[2]);
	if ((ulFunction == 0U) ||
		((pxDevice->ulFunctionMask & ulFunction) == 0U)) {
		prvPublishError(pxDevice->pcName, "UNSUPPORTED_FC");
		return;
	}
	xParseResult = prvParseHex(apcTokens[1], &usUnitId);
	if ((xParseResult != APP_DEBUG_PARSE_OK) ||
		(usUnitId == 0U) || (usUnitId > 0xF7U)) {
		prvPublishError(pxDevice->pcName, "PARAM_FORMAT_INVALID");
		return;
	}
	if (pxDevice->pxIsReady(pxDevice->pvContext) == 0U) {
		prvPublishError(pxDevice->pcName, "DEVICE_OFFLINE");
		return;
	}
	if (xSemaphoreTake(pxDevice->xMutex,
		pdMS_TO_TICKS(APP_DEBUG_MUTEX_TIMEOUT_MS)) != pdTRUE) {
		prvPublishError(pxDevice->pcName, "DEVICE_BUSY");
		return;
	}
	if (pxDevice->pxIsReady(pxDevice->pvContext) == 0U) {
		(void)xSemaphoreGive(pxDevice->xMutex);
		prvPublishError(pxDevice->pcName, "DEVICE_OFFLINE");
		return;
	}
	pcError = NULL;
	xResult = prvExecute(pxDevice, (uint8_t)usUnitId, ulFunction,
		apcTokens, ucTokenCount, &pcError);
	if (pcError == NULL) {
		s_xDebugSnapshot = *pxDevice->pxTrace;
	}
	(void)xSemaphoreGive(pxDevice->xMutex);

	if (pcError != NULL) {
		prvPublishError(pxDevice->pcName, pcError);
		return;
	}
	prvPublishTransaction(pxDevice->pcName, &s_xDebugSnapshot);
	if ((xResult != MODBUS_PORT_RESULT_OK) &&
		(xResult != MODBUS_PORT_RESULT_EXCEPTION)) {
		if (xResult == MODBUS_PORT_RESULT_TIMEOUT) {
			prvPublishError(pxDevice->pcName, "TIMEOUT");
		} else if (s_xDebugSnapshot.ucTxSucceeded == 0U) {
			prvPublishError(pxDevice->pcName, "TX_FAILED");
		} else {
			prvPublishError(pxDevice->pcName, "RX_FAILED");
		}
	}
	if (pxDevice->pxResult != NULL) {
		pxDevice->pxResult(xResult, pxDevice->pvContext);
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvSplit(char *pcLine, char **ppcTokens,
	uint8_t ucTokenCapacity)
{
	char *pcScan;
	uint8_t ucCount;

	if ((pcLine == NULL) || (pcLine[0] == '\0') ||
		(ucTokenCapacity == 0U)) {
		return 0U;
	}
	ucCount = 1U;
	ppcTokens[0] = pcLine;
	pcScan = pcLine;
	while (*pcScan != '\0') {
		if ((pcScan[0] == ':') && (pcScan[1] == ':')) {
			if (ucCount >= ucTokenCapacity) {
				return ucTokenCapacity;
			}
			pcScan[0] = '\0';
			pcScan[1] = '\0';
			ppcTokens[ucCount] = &pcScan[2];
			ucCount++;
			pcScan += 2;
		} else {
			pcScan++;
		}
	}
	return ucCount;
}

/*-----------------------------------------------------------*/
static AppDebugTcpDevice_t *prvFindDevice(const char *pcName)
{
	uint8_t ucIndex;

	for (ucIndex = 0U; ucIndex < s_ucDeviceCount; ucIndex++) {
		if (strcmp(s_axDevices[ucIndex].pcName, pcName) == 0) {
			return &s_axDevices[ucIndex];
		}
	}
	return NULL;
}

/*-----------------------------------------------------------*/
static AppDebugParseResult_e prvParseHex(const char *pcText,
	uint16_t *pusValue)
{
	uint32_t ulValue;
	uint8_t ucDigit;
	uint8_t ucLength;

	if ((pcText == NULL) || (pusValue == NULL) ||
		(pcText[0] == '\0')) {
		return APP_DEBUG_PARSE_PARAM_FORMAT;
	}
	ulValue = 0U;
	ucLength = 0U;
	while (*pcText != '\0') {
		if ((*pcText >= '0') && (*pcText <= '9')) {
			ucDigit = (uint8_t)(*pcText - '0');
		} else if ((*pcText >= 'A') && (*pcText <= 'F')) {
			ucDigit = (uint8_t)(*pcText - 'A' + 10);
		} else {
			return APP_DEBUG_PARSE_PARAM_FORMAT;
		}
		if ((ucLength >= 4U) || (ulValue > 0x0FFFUL)) {
			return APP_DEBUG_PARSE_PARAM_FORMAT;
		}
		ulValue = (ulValue << 4) | ucDigit;
		ucLength++;
		pcText++;
	}
	*pusValue = (uint16_t)ulValue;
	return APP_DEBUG_PARSE_OK;
}

/*-----------------------------------------------------------*/
static AppDebugParseResult_e prvParseValues(char *pcText,
	uint16_t *pusValues, uint16_t usCapacity, uint16_t *pusCount)
{
	char *pcStart;
	char *pcScan;
	char cSeparator;
	uint16_t usCount;

	if ((pcText == NULL) || (pcText[0] == '\0')) {
		return APP_DEBUG_PARSE_PARAM_FORMAT;
	}
	pcStart = pcText;
	pcScan = pcText;
	usCount = 0U;
	for (;;) {
		if ((*pcScan == '_') || (*pcScan == '\0')) {
			if (usCount >= usCapacity) {
				return APP_DEBUG_PARSE_PARAM_FORMAT;
			}
			cSeparator = *pcScan;
			*pcScan = '\0';
			if (prvParseHex(pcStart, &pusValues[usCount]) !=
				APP_DEBUG_PARSE_OK) {
				return APP_DEBUG_PARSE_PARAM_FORMAT;
			}
			usCount++;
			if (cSeparator == '\0') {
				break;
			}
			pcStart = &pcScan[1];
		}
		pcScan++;
	}
	*pusCount = usCount;
	return APP_DEBUG_PARSE_OK;
}

/*-----------------------------------------------------------*/
static uint32_t prvFunctionMask(const char *pcFunction)
{
	if (strcmp(pcFunction, "FC01") == 0) {
		return APP_DEBUG_FC01;
	}
	if (strcmp(pcFunction, "FC03") == 0) {
		return APP_DEBUG_FC03;
	}
	if (strcmp(pcFunction, "FC06") == 0) {
		return APP_DEBUG_FC06;
	}
	if (strcmp(pcFunction, "FC16") == 0) {
		return APP_DEBUG_FC16;
	}
	return 0U;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvExecute(AppDebugTcpDevice_t *pxDevice,
	uint8_t ucUnitId, uint32_t ulFunction, char **ppcTokens,
	uint8_t ucTokenCount, const char **ppcError)
{
	AppDebugParseResult_e xParseResult;
	uint16_t usAddress;
	uint16_t usQuantity;

	if (ucTokenCount != 5U) {
		*ppcError = "PARAM_COUNT_MISMATCH";
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xParseResult = prvParseHex(ppcTokens[3], &usAddress);
	if (xParseResult != APP_DEBUG_PARSE_OK) {
		*ppcError = "PARAM_FORMAT_INVALID";
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	if (ulFunction == APP_DEBUG_FC01) {
		xParseResult = prvParseHex(ppcTokens[4], &usQuantity);
		if ((xParseResult != APP_DEBUG_PARSE_OK) ||
			(usQuantity == 0U) ||
			(usQuantity > APP_DEBUG_MAX_READ_COILS) ||
			(usAddress >= pxDevice->usCoilCount) ||
			(usQuantity > (uint16_t)(pxDevice->usCoilCount -
				usAddress))) {
			*ppcError = "PARAM_FORMAT_INVALID";
			return MODBUS_PORT_RESULT_INVALID_ARG;
		}
		return xModbusPortReadCoils(pxDevice->pxClient, ucUnitId,
			usAddress, usQuantity, s_abCoils,
			APP_DEBUG_TRANSACTION_TIMEOUT_MS);
	}
	if (ulFunction == APP_DEBUG_FC03) {
		xParseResult = prvParseHex(ppcTokens[4], &usQuantity);
		if ((xParseResult != APP_DEBUG_PARSE_OK) ||
			(usQuantity == 0U) ||
			(usQuantity > APP_DEBUG_MAX_READ_REGISTERS) ||
			(usAddress >= pxDevice->usHoldingCount) ||
			(usQuantity > (uint16_t)(pxDevice->usHoldingCount -
				usAddress))) {
			*ppcError = "PARAM_FORMAT_INVALID";
			return MODBUS_PORT_RESULT_INVALID_ARG;
		}
		return xModbusPortReadHolding(pxDevice->pxClient, ucUnitId,
			usAddress, usQuantity, s_ausRegisters,
			APP_DEBUG_TRANSACTION_TIMEOUT_MS);
	}
	if (ulFunction == APP_DEBUG_FC06) {
		xParseResult = prvParseHex(ppcTokens[4], &usQuantity);
		if ((xParseResult != APP_DEBUG_PARSE_OK) ||
			(usAddress >= pxDevice->usHoldingCount)) {
			*ppcError = "PARAM_FORMAT_INVALID";
			return MODBUS_PORT_RESULT_INVALID_ARG;
		}
		return xModbusPortWriteRegister(pxDevice->pxClient, ucUnitId,
			usAddress, usQuantity,
			APP_DEBUG_TRANSACTION_TIMEOUT_MS);
	}
	xParseResult = prvParseValues(ppcTokens[4], s_ausWriteRegisters,
		APP_DEBUG_MAX_WRITE_REGISTERS, &usQuantity);
	if ((xParseResult != APP_DEBUG_PARSE_OK) ||
		(usQuantity == 0U) ||
		(usQuantity > pxDevice->usMaxWriteRegisters) ||
		(usAddress >= pxDevice->usHoldingCount) ||
		(usQuantity > (uint16_t)(pxDevice->usHoldingCount -
			usAddress))) {
		*ppcError = "PARAM_FORMAT_INVALID";
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	return xModbusPortWriteRegisters(pxDevice->pxClient, ucUnitId,
		usAddress, usQuantity, s_ausWriteRegisters,
		APP_DEBUG_TRANSACTION_TIMEOUT_MS);
}

/*-----------------------------------------------------------*/
static void prvPublishTransaction(const char *pcName,
	const ModbusPortTrace_t *pxTrace)
{
	if (pxTrace->ucTxSucceeded != 0U) {
		prvPublishFrame(pcName, "TxData", &pxTrace->xLastTx);
	}
	if (pxTrace->ucRxSucceeded != 0U) {
		prvPublishFrame(pcName, "RxData", &pxTrace->xLastRx);
	}
}

/*-----------------------------------------------------------*/
static void prvPublishFrame(const char *pcName, const char *pcEvent,
	const ModbusPortFrame_t *pxFrame)
{
	uint16_t usIndex;
	uint16_t usLength;

	usLength = 0U;
	s_acOutput[0] = '\0';
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "[");
	usLength = prvAppendU32(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, (uint32_t)xTaskGetTickCount());
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "][D][");
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, pcName);
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "][Test] ");
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, pcEvent);
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "=");
	for (usIndex = 0U; usIndex < pxFrame->usCapturedLength; usIndex++) {
		usLength = prvAppendHexByte(s_acOutput, usLength,
			APP_DEBUG_OUTPUT_LENGTH, pxFrame->aucData[usIndex]);
	}
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "\r\n");
	prvSendOutput(usLength);
}

/*-----------------------------------------------------------*/
static void prvPublishError(const char *pcName, const char *pcError)
{
	uint16_t usLength;

	usLength = 0U;
	s_acOutput[0] = '\0';
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "[");
	usLength = prvAppendU32(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, (uint32_t)xTaskGetTickCount());
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "][D][");
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, pcName);
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "][Test] Error=");
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, pcError);
	usLength = prvAppendText(s_acOutput, usLength,
		APP_DEBUG_OUTPUT_LENGTH, "\r\n");
	prvSendOutput(usLength);
}

/*-----------------------------------------------------------*/
static uint16_t prvAppendText(char *pcOutput, uint16_t usOffset,
	uint16_t usCapacity, const char *pcText)
{
	while ((*pcText != '\0') && (usOffset < (usCapacity - 1U))) {
		pcOutput[usOffset] = *pcText;
		usOffset++;
		pcText++;
	}
	pcOutput[usOffset] = '\0';
	return usOffset;
}

/*-----------------------------------------------------------*/
static uint16_t prvAppendU32(char *pcOutput, uint16_t usOffset,
	uint16_t usCapacity, uint32_t ulValue)
{
	char acDigits[10];
	uint8_t ucCount;

	ucCount = 0U;
	do {
		acDigits[ucCount] = (char)('0' + (ulValue % 10U));
		ulValue /= 10U;
		ucCount++;
	} while ((ulValue != 0U) && (ucCount < sizeof(acDigits)));
	while (ucCount > 0U) {
		ucCount--;
		if (usOffset < (usCapacity - 1U)) {
			pcOutput[usOffset] = acDigits[ucCount];
			usOffset++;
		}
	}
	pcOutput[usOffset] = '\0';
	return usOffset;
}

/*-----------------------------------------------------------*/
static uint16_t prvAppendHexByte(char *pcOutput, uint16_t usOffset,
	uint16_t usCapacity, uint8_t ucValue)
{
	static const char acHex[] = "0123456789ABCDEF";

	if (usOffset < (usCapacity - 1U)) {
		pcOutput[usOffset] = acHex[ucValue >> 4];
		usOffset++;
	}
	if (usOffset < (usCapacity - 1U)) {
		pcOutput[usOffset] = acHex[ucValue & 0x0FU];
		usOffset++;
	}
	pcOutput[usOffset] = '\0';
	return usOffset;
}

/*-----------------------------------------------------------*/
static void prvSendOutput(uint16_t usLength)
{
	if (usLength > 0U) {
		(void)lAppCommLogPortDebugWrite(
			(const uint8_t *)s_acOutput, usLength,
			APP_DEBUG_OUTPUT_TIMEOUT_MS);
	}
}
