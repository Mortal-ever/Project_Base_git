#include "modbus_tcp_client.h"

#include <string.h>

#include "task.h"

#define MODBUS_TCP_MBAP_LENGTH               7U
#define MODBUS_TCP_FC_READ_COILS          0x01U
#define MODBUS_TCP_FC_READ_HOLDING        0x03U
#define MODBUS_TCP_FC_WRITE_REGISTER      0x06U
#define MODBUS_TCP_FC_WRITE_REGISTERS     0x10U

static uint16_t prvReadU16(const uint8_t *pucData);
static void prvWriteU16(uint8_t *pucData, uint16_t usValue);
static uint16_t prvNextTransactionId(ModbusTcpClient_t *pxClient);
static ModbusTcpResult_e prvExchange(ModbusTcpClient_t *pxClient,
	const uint8_t *pucRequest, uint16_t usRequestLength,
	uint8_t *pucResponse, uint16_t *pusResponseLength,
	uint32_t ulTimeoutMs);
static ModbusTcpResult_e prvReceiveAdu(TransportChannel_t *pxTransport,
	uint8_t *pucAdu, uint16_t *pusLength, uint32_t ulTimeoutMs);
static ModbusTcpResult_e prvReceiveExact(TransportChannel_t *pxTransport,
	uint8_t *pucData, uint16_t usLength, uint32_t ulTimeoutMs);
static ModbusTcpResult_e prvValidateResponse(ModbusTcpClient_t *pxClient,
	const uint8_t *pucResponse, uint16_t usResponseLength,
	uint16_t usTransactionId, uint8_t ucUnitId, uint8_t ucFunctionCode);
static ModbusTcpResult_e prvMapTransport(TransportResult_e xResult);
static void prvCapture(ModbusTcpFrame_t *pxFrame, const uint8_t *pucData,
	uint16_t usLength);

/*-----------------------------------------------------------*/
void vModbusTcpClientInit(ModbusTcpClient_t *pxClient,
	TransportChannel_t *pxTransport)
{
	if ((pxClient == NULL) || (pxTransport == NULL)) {
		return;
	}
	memset(pxClient, 0, sizeof(*pxClient));
	pxClient->pxTransport = pxTransport;
	pxClient->usNextTransactionId = 1U;
}

/*-----------------------------------------------------------*/
void vModbusTcpClientSetDebug(ModbusTcpClient_t *pxClient,
	ModbusTcpDebug_t *pxDebug)
{
	if (pxClient == NULL) {
		return;
	}
	pxClient->pxDebug = pxDebug;
	if (pxDebug != NULL) {
		memset(pxDebug, 0, sizeof(*pxDebug));
	}
}

/*-----------------------------------------------------------*/
ModbusTcpResult_e xModbusTcpReadCoils(ModbusTcpClient_t *pxClient,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbData, uint32_t ulTimeoutMs)
{
	uint8_t aucRequest[12];
	uint8_t aucResponse[MODBUS_TCP_MAX_ADU_LENGTH];
	uint16_t usTransactionId;
	uint16_t usResponseLength;
	uint16_t usIndex;
	uint8_t ucByteCount;
	ModbusTcpResult_e xResult;

	if ((pxClient == NULL) || (pbData == NULL) ||
		(usQuantity == 0U) || (usQuantity > 2000U)) {
		return MODBUS_TCP_RESULT_INVALID_ARG;
	}
	usTransactionId = prvNextTransactionId(pxClient);
	prvWriteU16(&aucRequest[0], usTransactionId);
	prvWriteU16(&aucRequest[2], 0U);
	prvWriteU16(&aucRequest[4], 6U);
	aucRequest[6] = ucUnitId;
	aucRequest[7] = MODBUS_TCP_FC_READ_COILS;
	prvWriteU16(&aucRequest[8], usAddress);
	prvWriteU16(&aucRequest[10], usQuantity);
	xResult = prvExchange(pxClient, aucRequest, sizeof(aucRequest),
		aucResponse, &usResponseLength, ulTimeoutMs);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	xResult = prvValidateResponse(pxClient, aucResponse, usResponseLength,
		usTransactionId, ucUnitId, MODBUS_TCP_FC_READ_COILS);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	ucByteCount = (uint8_t)((usQuantity + 7U) / 8U);
	if ((aucResponse[8] != ucByteCount) ||
		(usResponseLength != (uint16_t)(9U + ucByteCount))) {
		return MODBUS_TCP_RESULT_PROTOCOL;
	}
	for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
		pbData[usIndex] =
			((aucResponse[9U + (usIndex / 8U)] &
			(uint8_t)(1U << (usIndex % 8U))) != 0U);
	}
	return MODBUS_TCP_RESULT_OK;
}

/*-----------------------------------------------------------*/
ModbusTcpResult_e xModbusTcpReadHolding(ModbusTcpClient_t *pxClient,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusData, uint32_t ulTimeoutMs)
{
	uint8_t aucRequest[12];
	uint8_t aucResponse[MODBUS_TCP_MAX_ADU_LENGTH];
	uint16_t usTransactionId;
	uint16_t usResponseLength;
	uint16_t usIndex;
	ModbusTcpResult_e xResult;

	if ((pxClient == NULL) || (pusData == NULL) ||
		(usQuantity == 0U) || (usQuantity > 125U)) {
		return MODBUS_TCP_RESULT_INVALID_ARG;
	}
	usTransactionId = prvNextTransactionId(pxClient);
	prvWriteU16(&aucRequest[0], usTransactionId);
	prvWriteU16(&aucRequest[2], 0U);
	prvWriteU16(&aucRequest[4], 6U);
	aucRequest[6] = ucUnitId;
	aucRequest[7] = MODBUS_TCP_FC_READ_HOLDING;
	prvWriteU16(&aucRequest[8], usAddress);
	prvWriteU16(&aucRequest[10], usQuantity);
	xResult = prvExchange(pxClient, aucRequest, sizeof(aucRequest),
		aucResponse, &usResponseLength, ulTimeoutMs);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	xResult = prvValidateResponse(pxClient, aucResponse, usResponseLength,
		usTransactionId, ucUnitId, MODBUS_TCP_FC_READ_HOLDING);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	if ((aucResponse[8] != (uint8_t)(usQuantity * 2U)) ||
		(usResponseLength != (uint16_t)(9U + (usQuantity * 2U)))) {
		return MODBUS_TCP_RESULT_PROTOCOL;
	}
	for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
		pusData[usIndex] =
			prvReadU16(&aucResponse[9U + (usIndex * 2U)]);
	}
	return MODBUS_TCP_RESULT_OK;
}

/*-----------------------------------------------------------*/
ModbusTcpResult_e xModbusTcpWriteRegister(ModbusTcpClient_t *pxClient,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usValue,
	uint32_t ulTimeoutMs)
{
	uint8_t aucRequest[12];
	uint8_t aucResponse[MODBUS_TCP_MAX_ADU_LENGTH];
	uint16_t usTransactionId;
	uint16_t usResponseLength;
	ModbusTcpResult_e xResult;

	if (pxClient == NULL) {
		return MODBUS_TCP_RESULT_INVALID_ARG;
	}
	usTransactionId = prvNextTransactionId(pxClient);
	prvWriteU16(&aucRequest[0], usTransactionId);
	prvWriteU16(&aucRequest[2], 0U);
	prvWriteU16(&aucRequest[4], 6U);
	aucRequest[6] = ucUnitId;
	aucRequest[7] = MODBUS_TCP_FC_WRITE_REGISTER;
	prvWriteU16(&aucRequest[8], usAddress);
	prvWriteU16(&aucRequest[10], usValue);
	xResult = prvExchange(pxClient, aucRequest, sizeof(aucRequest),
		aucResponse, &usResponseLength, ulTimeoutMs);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	xResult = prvValidateResponse(pxClient, aucResponse, usResponseLength,
		usTransactionId, ucUnitId, MODBUS_TCP_FC_WRITE_REGISTER);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	if ((usResponseLength != sizeof(aucRequest)) ||
		(memcmp(&aucResponse[8], &aucRequest[8], 4U) != 0)) {
		return MODBUS_TCP_RESULT_PROTOCOL;
	}
	return MODBUS_TCP_RESULT_OK;
}

/*-----------------------------------------------------------*/
ModbusTcpResult_e xModbusTcpWriteRegisters(ModbusTcpClient_t *pxClient,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usQuantity,
	const uint16_t *pusData, uint32_t ulTimeoutMs)
{
	uint8_t aucRequest[MODBUS_TCP_MAX_ADU_LENGTH];
	uint8_t aucResponse[MODBUS_TCP_MAX_ADU_LENGTH];
	uint16_t usTransactionId;
	uint16_t usRequestLength;
	uint16_t usResponseLength;
	uint16_t usIndex;
	ModbusTcpResult_e xResult;

	if ((pxClient == NULL) || (pusData == NULL) ||
		(usQuantity == 0U) || (usQuantity > 123U)) {
		return MODBUS_TCP_RESULT_INVALID_ARG;
	}
	usTransactionId = prvNextTransactionId(pxClient);
	usRequestLength = (uint16_t)(13U + (usQuantity * 2U));
	prvWriteU16(&aucRequest[0], usTransactionId);
	prvWriteU16(&aucRequest[2], 0U);
	prvWriteU16(&aucRequest[4], (uint16_t)(7U + (usQuantity * 2U)));
	aucRequest[6] = ucUnitId;
	aucRequest[7] = MODBUS_TCP_FC_WRITE_REGISTERS;
	prvWriteU16(&aucRequest[8], usAddress);
	prvWriteU16(&aucRequest[10], usQuantity);
	aucRequest[12] = (uint8_t)(usQuantity * 2U);
	for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
		prvWriteU16(&aucRequest[13U + (usIndex * 2U)],
			pusData[usIndex]);
	}
	xResult = prvExchange(pxClient, aucRequest, usRequestLength,
		aucResponse, &usResponseLength, ulTimeoutMs);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	xResult = prvValidateResponse(pxClient, aucResponse, usResponseLength,
		usTransactionId, ucUnitId, MODBUS_TCP_FC_WRITE_REGISTERS);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	if ((usResponseLength != 12U) ||
		(prvReadU16(&aucResponse[8]) != usAddress) ||
		(prvReadU16(&aucResponse[10]) != usQuantity)) {
		return MODBUS_TCP_RESULT_PROTOCOL;
	}
	return MODBUS_TCP_RESULT_OK;
}

/*-----------------------------------------------------------*/
static uint16_t prvReadU16(const uint8_t *pucData)
{
	return (uint16_t)(((uint16_t)pucData[0] << 8) | pucData[1]);
}

/*-----------------------------------------------------------*/
static void prvWriteU16(uint8_t *pucData, uint16_t usValue)
{
	pucData[0] = (uint8_t)(usValue >> 8);
	pucData[1] = (uint8_t)usValue;
}

/*-----------------------------------------------------------*/
static uint16_t prvNextTransactionId(ModbusTcpClient_t *pxClient)
{
	uint16_t usTransactionId;

	usTransactionId = pxClient->usNextTransactionId++;
	if (pxClient->usNextTransactionId == 0U) {
		pxClient->usNextTransactionId = 1U;
	}
	return usTransactionId;
}

/*-----------------------------------------------------------*/
static ModbusTcpResult_e prvExchange(ModbusTcpClient_t *pxClient,
	const uint8_t *pucRequest, uint16_t usRequestLength,
	uint8_t *pucResponse, uint16_t *pusResponseLength,
	uint32_t ulTimeoutMs)
{
	TransportResult_e xTransportResult;
	ModbusTcpResult_e xResult;

	if ((pxClient == NULL) || (pxClient->pxTransport == NULL)) {
		return MODBUS_TCP_RESULT_INVALID_ARG;
	}
	if (pxClient->pxDebug != NULL) {
		pxClient->pxDebug->ucTxSucceeded = 0U;
		pxClient->pxDebug->ucRxSucceeded = 0U;
		pxClient->pxDebug->xLastRx.usLength = 0U;
		pxClient->pxDebug->xLastRx.usCapturedLength = 0U;
		prvCapture(&pxClient->pxDebug->xLastTx, pucRequest,
			usRequestLength);
	}
	xTransportResult = xTransportSend(pxClient->pxTransport, pucRequest,
		usRequestLength, ulTimeoutMs);
	if (xTransportResult != TRANSPORT_RESULT_OK) {
		return prvMapTransport(xTransportResult);
	}
	if (pxClient->pxDebug != NULL) {
		pxClient->pxDebug->ucTxSucceeded = 1U;
	}
	xResult = prvReceiveAdu(pxClient->pxTransport, pucResponse,
		pusResponseLength, ulTimeoutMs);
	if ((xResult == MODBUS_TCP_RESULT_OK) &&
		(pxClient->pxDebug != NULL)) {
		prvCapture(&pxClient->pxDebug->xLastRx, pucResponse,
			*pusResponseLength);
		pxClient->pxDebug->ucRxSucceeded = 1U;
	}
	return xResult;
}

/*-----------------------------------------------------------*/
static ModbusTcpResult_e prvReceiveAdu(TransportChannel_t *pxTransport,
	uint8_t *pucAdu, uint16_t *pusLength, uint32_t ulTimeoutMs)
{
	uint16_t usMbapLength;
	uint16_t usRemaining;
	ModbusTcpResult_e xResult;

	xResult = prvReceiveExact(pxTransport, pucAdu,
		MODBUS_TCP_MBAP_LENGTH, ulTimeoutMs);
	if (xResult != MODBUS_TCP_RESULT_OK) {
		return xResult;
	}
	if (prvReadU16(&pucAdu[2]) != 0U) {
		return MODBUS_TCP_RESULT_PROTOCOL;
	}
	usMbapLength = prvReadU16(&pucAdu[4]);
	if ((usMbapLength < 2U) ||
		(usMbapLength > (MODBUS_TCP_MAX_PDU_LENGTH + 1U))) {
		return MODBUS_TCP_RESULT_PROTOCOL;
	}
	usRemaining = (uint16_t)(usMbapLength - 1U);
	xResult = prvReceiveExact(pxTransport, &pucAdu[MODBUS_TCP_MBAP_LENGTH],
		usRemaining, ulTimeoutMs);
	if (xResult == MODBUS_TCP_RESULT_OK) {
		*pusLength = (uint16_t)(MODBUS_TCP_MBAP_LENGTH + usRemaining);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
static ModbusTcpResult_e prvReceiveExact(TransportChannel_t *pxTransport,
	uint8_t *pucData, uint16_t usLength, uint32_t ulTimeoutMs)
{
	TickType_t xStart;
	TickType_t xTimeout;
	TickType_t xElapsed;
	uint16_t usOffset;
	uint16_t usReceived;
	uint32_t ulRemainingMs;
	TransportResult_e xTransportResult;

	xStart = xTaskGetTickCount();
	xTimeout = pdMS_TO_TICKS(ulTimeoutMs);
	if (xTimeout == 0U) {
		xTimeout = 1U;
	}
	usOffset = 0U;
	while (usOffset < usLength) {
		xElapsed = xTaskGetTickCount() - xStart;
		if (xElapsed >= xTimeout) {
			return MODBUS_TCP_RESULT_TIMEOUT;
		}
		ulRemainingMs = (uint32_t)((xTimeout - xElapsed) *
			portTICK_PERIOD_MS);
		if (ulRemainingMs == 0U) {
			ulRemainingMs = 1U;
		}
		xTransportResult = xTransportReceive(pxTransport,
			&pucData[usOffset], (uint16_t)(usLength - usOffset),
			&usReceived, ulRemainingMs);
		if (xTransportResult != TRANSPORT_RESULT_OK) {
			return prvMapTransport(xTransportResult);
		}
		if (usReceived == 0U) {
			return MODBUS_TCP_RESULT_TRANSPORT;
		}
		usOffset = (uint16_t)(usOffset + usReceived);
	}
	return MODBUS_TCP_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusTcpResult_e prvValidateResponse(ModbusTcpClient_t *pxClient,
	const uint8_t *pucResponse, uint16_t usResponseLength,
	uint16_t usTransactionId, uint8_t ucUnitId, uint8_t ucFunctionCode)
{
	if ((usResponseLength < 9U) ||
		(prvReadU16(&pucResponse[0]) != usTransactionId) ||
		(prvReadU16(&pucResponse[2]) != 0U) ||
		(pucResponse[6] != ucUnitId)) {
		return MODBUS_TCP_RESULT_PROTOCOL;
	}
	if ((pucResponse[7] & 0x80U) != 0U) {
		if (usResponseLength != 9U) {
			return MODBUS_TCP_RESULT_PROTOCOL;
		}
		pxClient->ucLastException = pucResponse[8];
		return MODBUS_TCP_RESULT_EXCEPTION;
	}
	if (pucResponse[7] != ucFunctionCode) {
		return MODBUS_TCP_RESULT_PROTOCOL;
	}
	pxClient->ucLastException = 0U;
	return MODBUS_TCP_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusTcpResult_e prvMapTransport(TransportResult_e xResult)
{
	if (xResult == TRANSPORT_RESULT_TIMEOUT) {
		return MODBUS_TCP_RESULT_TIMEOUT;
	}
	return MODBUS_TCP_RESULT_TRANSPORT;
}

/*-----------------------------------------------------------*/
static void prvCapture(ModbusTcpFrame_t *pxFrame, const uint8_t *pucData,
	uint16_t usLength)
{
	uint16_t usCopyLength;

	usCopyLength = usLength;
	if (usCopyLength > MODBUS_TCP_DEBUG_LENGTH) {
		usCopyLength = MODBUS_TCP_DEBUG_LENGTH;
	}
	memset(pxFrame->aucData, 0, sizeof(pxFrame->aucData));
	memcpy(pxFrame->aucData, pucData, usCopyLength);
	pxFrame->usLength = usLength;
	pxFrame->usCapturedLength = usCopyLength;
	pxFrame->ulSequence++;
}
