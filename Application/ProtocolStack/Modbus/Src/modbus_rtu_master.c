#include "modbus_rtu_master.h"

#include <string.h>

#include "task.h"

#define MODBUS_RTU_FC_READ_COILS              0x01U
#define MODBUS_RTU_FC_READ_DISCRETE            0x02U
#define MODBUS_RTU_FC_READ_HOLDING             0x03U
#define MODBUS_RTU_FC_READ_INPUT               0x04U
#define MODBUS_RTU_FC_WRITE_COIL               0x05U
#define MODBUS_RTU_FC_WRITE_REGISTER           0x06U

static uint16_t prvReadU16(const uint8_t *pucData);
static void prvWriteU16(uint8_t *pucData, uint16_t usValue);
static uint16_t prvCrc(const uint8_t *pucData, uint16_t usLength);
static ModbusRtuResult_e prvReadBits(ModbusRtuMaster_t *pxMaster,
	uint8_t ucFunction, uint8_t ucSlaveId, uint16_t usAddress,
	uint16_t usQuantity, bool *pbData, uint32_t ulTimeoutMs);
static ModbusRtuResult_e prvReadRegisters(ModbusRtuMaster_t *pxMaster,
	uint8_t ucFunction, uint8_t ucSlaveId, uint16_t usAddress,
	uint16_t usQuantity, uint16_t *pusData, uint32_t ulTimeoutMs);
static ModbusRtuResult_e prvWriteSingle(ModbusRtuMaster_t *pxMaster,
	uint8_t ucFunction, uint8_t ucSlaveId, uint16_t usAddress,
	uint16_t usValue, uint32_t ulTimeoutMs);
static ModbusRtuResult_e prvSendRequest(ModbusRtuMaster_t *pxMaster,
	uint8_t *pucRequest, uint16_t usLength, uint32_t ulTimeoutMs);
static ModbusRtuResult_e prvReceiveExact(TransportChannel_t *pxTransport,
	uint8_t *pucData, uint16_t usLength, uint32_t ulTimeoutMs);
static ModbusRtuResult_e prvValidateCrc(const uint8_t *pucData,
	uint16_t usLength);
static ModbusRtuResult_e prvMapTransport(TransportResult_e xResult);

/*-----------------------------------------------------------*/
void vModbusRtuMasterInit(ModbusRtuMaster_t *pxMaster,
	TransportChannel_t *pxTransport)
{
	if ((pxMaster == NULL) || (pxTransport == NULL)) {
		return;
	}
	pxMaster->pxTransport = pxTransport;
	pxMaster->ucLastException = 0U;
}

/*-----------------------------------------------------------*/
ModbusRtuResult_e xModbusRtuReadCoils(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbData, uint32_t ulTimeoutMs)
{
	return prvReadBits(pxMaster, MODBUS_RTU_FC_READ_COILS, ucSlaveId,
		usAddress, usQuantity, pbData, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
ModbusRtuResult_e xModbusRtuReadDiscreteInputs(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usQuantity,
	bool *pbData, uint32_t ulTimeoutMs)
{
	return prvReadBits(pxMaster, MODBUS_RTU_FC_READ_DISCRETE, ucSlaveId,
		usAddress, usQuantity, pbData, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
ModbusRtuResult_e xModbusRtuReadHolding(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusData, uint32_t ulTimeoutMs)
{
	return prvReadRegisters(pxMaster, MODBUS_RTU_FC_READ_HOLDING,
		ucSlaveId, usAddress, usQuantity, pusData, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
ModbusRtuResult_e xModbusRtuReadInputRegisters(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usQuantity,
	uint16_t *pusData, uint32_t ulTimeoutMs)
{
	return prvReadRegisters(pxMaster, MODBUS_RTU_FC_READ_INPUT,
		ucSlaveId, usAddress, usQuantity, pusData, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
ModbusRtuResult_e xModbusRtuWriteCoil(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, bool bValue,
	uint32_t ulTimeoutMs)
{
	return prvWriteSingle(pxMaster, MODBUS_RTU_FC_WRITE_COIL, ucSlaveId,
		usAddress, bValue ? 0xFF00U : 0x0000U, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
ModbusRtuResult_e xModbusRtuWriteRegister(ModbusRtuMaster_t *pxMaster,
	uint8_t ucSlaveId, uint16_t usAddress, uint16_t usValue,
	uint32_t ulTimeoutMs)
{
	return prvWriteSingle(pxMaster, MODBUS_RTU_FC_WRITE_REGISTER,
		ucSlaveId, usAddress, usValue, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
static ModbusRtuResult_e prvReadBits(ModbusRtuMaster_t *pxMaster,
	uint8_t ucFunction, uint8_t ucSlaveId, uint16_t usAddress,
	uint16_t usQuantity, bool *pbData, uint32_t ulTimeoutMs)
{
	uint8_t aucRequest[8];
	uint8_t aucResponse[MODBUS_RTU_MAX_ADU_LENGTH];
	uint16_t usIndex;
	uint16_t usResponseLength;
	uint8_t ucByteCount;
	ModbusRtuResult_e xResult;

	if ((pxMaster == NULL) || (pbData == NULL) || (ucSlaveId == 0U) ||
		(usQuantity == 0U) || (usQuantity > 2000U)) {
		return MODBUS_RTU_RESULT_INVALID_ARG;
	}
	aucRequest[0] = ucSlaveId;
	aucRequest[1] = ucFunction;
	prvWriteU16(&aucRequest[2], usAddress);
	prvWriteU16(&aucRequest[4], usQuantity);
	xResult = prvSendRequest(pxMaster, aucRequest, 6U, ulTimeoutMs);
	if (xResult != MODBUS_RTU_RESULT_OK) {
		return xResult;
	}
	xResult = prvReceiveExact(pxMaster->pxTransport, aucResponse, 3U,
		ulTimeoutMs);
	if (xResult != MODBUS_RTU_RESULT_OK) {
		return xResult;
	}
	if ((aucResponse[0] != ucSlaveId) ||
		((aucResponse[1] != ucFunction) &&
		(aucResponse[1] != (uint8_t)(ucFunction | 0x80U)))) {
		return MODBUS_RTU_RESULT_PROTOCOL;
	}
	if ((aucResponse[1] & 0x80U) != 0U) {
		xResult = prvReceiveExact(pxMaster->pxTransport,
			&aucResponse[3], 2U, ulTimeoutMs);
		if (xResult != MODBUS_RTU_RESULT_OK) {
			return xResult;
		}
		if (prvValidateCrc(aucResponse, 5U) != MODBUS_RTU_RESULT_OK) {
			return MODBUS_RTU_RESULT_PROTOCOL;
		}
		pxMaster->ucLastException = aucResponse[2];
		return MODBUS_RTU_RESULT_EXCEPTION;
	}
	ucByteCount = aucResponse[2];
	usResponseLength = (uint16_t)(3U + ucByteCount + 2U);
	xResult = prvReceiveExact(pxMaster->pxTransport, &aucResponse[3],
		(uint16_t)(ucByteCount + 2U), ulTimeoutMs);
	if ((xResult != MODBUS_RTU_RESULT_OK) ||
		(ucByteCount != (uint8_t)((usQuantity + 7U) / 8U)) ||
		(prvValidateCrc(aucResponse, usResponseLength) !=
		MODBUS_RTU_RESULT_OK)) {
		return (xResult == MODBUS_RTU_RESULT_OK) ?
			MODBUS_RTU_RESULT_PROTOCOL : xResult;
	}
	for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
		pbData[usIndex] =
			((aucResponse[3U + (usIndex / 8U)] &
			(uint8_t)(1U << (usIndex % 8U))) != 0U);
	}
	pxMaster->ucLastException = 0U;
	return MODBUS_RTU_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusRtuResult_e prvReadRegisters(ModbusRtuMaster_t *pxMaster,
	uint8_t ucFunction, uint8_t ucSlaveId, uint16_t usAddress,
	uint16_t usQuantity, uint16_t *pusData, uint32_t ulTimeoutMs)
{
	uint8_t aucRequest[8];
	uint8_t aucResponse[MODBUS_RTU_MAX_ADU_LENGTH];
	uint16_t usIndex;
	uint16_t usResponseLength;
	uint8_t ucByteCount;
	ModbusRtuResult_e xResult;

	if ((pxMaster == NULL) || (pusData == NULL) || (ucSlaveId == 0U) ||
		(usQuantity == 0U) || (usQuantity > 125U)) {
		return MODBUS_RTU_RESULT_INVALID_ARG;
	}
	aucRequest[0] = ucSlaveId;
	aucRequest[1] = ucFunction;
	prvWriteU16(&aucRequest[2], usAddress);
	prvWriteU16(&aucRequest[4], usQuantity);
	xResult = prvSendRequest(pxMaster, aucRequest, 6U, ulTimeoutMs);
	if (xResult != MODBUS_RTU_RESULT_OK) {
		return xResult;
	}
	xResult = prvReceiveExact(pxMaster->pxTransport, aucResponse, 3U,
		ulTimeoutMs);
	if (xResult != MODBUS_RTU_RESULT_OK) {
		return xResult;
	}
	if ((aucResponse[0] != ucSlaveId) ||
		((aucResponse[1] != ucFunction) &&
		(aucResponse[1] != (uint8_t)(ucFunction | 0x80U)))) {
		return MODBUS_RTU_RESULT_PROTOCOL;
	}
	if ((aucResponse[1] & 0x80U) != 0U) {
		xResult = prvReceiveExact(pxMaster->pxTransport,
			&aucResponse[3], 2U, ulTimeoutMs);
		if (xResult != MODBUS_RTU_RESULT_OK) {
			return xResult;
		}
		if (prvValidateCrc(aucResponse, 5U) != MODBUS_RTU_RESULT_OK) {
			return MODBUS_RTU_RESULT_PROTOCOL;
		}
		pxMaster->ucLastException = aucResponse[2];
		return MODBUS_RTU_RESULT_EXCEPTION;
	}
	ucByteCount = aucResponse[2];
	usResponseLength = (uint16_t)(3U + ucByteCount + 2U);
	xResult = prvReceiveExact(pxMaster->pxTransport, &aucResponse[3],
		(uint16_t)(ucByteCount + 2U), ulTimeoutMs);
	if ((xResult != MODBUS_RTU_RESULT_OK) ||
		(ucByteCount != (uint8_t)(usQuantity * 2U)) ||
		(prvValidateCrc(aucResponse, usResponseLength) !=
		MODBUS_RTU_RESULT_OK)) {
		return (xResult == MODBUS_RTU_RESULT_OK) ?
			MODBUS_RTU_RESULT_PROTOCOL : xResult;
	}
	for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
		pusData[usIndex] =
			prvReadU16(&aucResponse[3U + (usIndex * 2U)]);
	}
	pxMaster->ucLastException = 0U;
	return MODBUS_RTU_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusRtuResult_e prvWriteSingle(ModbusRtuMaster_t *pxMaster,
	uint8_t ucFunction, uint8_t ucSlaveId, uint16_t usAddress,
	uint16_t usValue, uint32_t ulTimeoutMs)
{
	uint8_t aucRequest[8];
	uint8_t aucResponse[8];
	ModbusRtuResult_e xResult;

	if ((pxMaster == NULL) || (ucSlaveId == 0U)) {
		return MODBUS_RTU_RESULT_INVALID_ARG;
	}
	aucRequest[0] = ucSlaveId;
	aucRequest[1] = ucFunction;
	prvWriteU16(&aucRequest[2], usAddress);
	prvWriteU16(&aucRequest[4], usValue);
	xResult = prvSendRequest(pxMaster, aucRequest, 6U, ulTimeoutMs);
	if (xResult != MODBUS_RTU_RESULT_OK) {
		return xResult;
	}
	xResult = prvReceiveExact(pxMaster->pxTransport, aucResponse, 5U,
		ulTimeoutMs);
	if (xResult != MODBUS_RTU_RESULT_OK) {
		return xResult;
	}
	if ((aucResponse[0] == ucSlaveId) &&
		(aucResponse[1] == (uint8_t)(ucFunction | 0x80U))) {
		if (prvValidateCrc(aucResponse, 5U) != MODBUS_RTU_RESULT_OK) {
			return MODBUS_RTU_RESULT_PROTOCOL;
		}
		pxMaster->ucLastException = aucResponse[2];
		return MODBUS_RTU_RESULT_EXCEPTION;
	}
	xResult = prvReceiveExact(pxMaster->pxTransport, &aucResponse[5], 3U,
		ulTimeoutMs);
	if ((xResult != MODBUS_RTU_RESULT_OK) ||
		(prvValidateCrc(aucResponse, 8U) != MODBUS_RTU_RESULT_OK) ||
		(aucResponse[0] != ucSlaveId) ||
		(aucResponse[1] != ucFunction) ||
		(memcmp(&aucResponse[2], &aucRequest[2], 4U) != 0)) {
		return (xResult == MODBUS_RTU_RESULT_OK) ?
			MODBUS_RTU_RESULT_PROTOCOL : xResult;
	}
	pxMaster->ucLastException = 0U;
	return MODBUS_RTU_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusRtuResult_e prvSendRequest(ModbusRtuMaster_t *pxMaster,
	uint8_t *pucRequest, uint16_t usLength, uint32_t ulTimeoutMs)
{
	uint16_t usCrc;
	TransportResult_e xTransportResult;

	if ((pxMaster == NULL) || (pxMaster->pxTransport == NULL) ||
		((usLength + 2U) > MODBUS_RTU_MAX_ADU_LENGTH)) {
		return MODBUS_RTU_RESULT_INVALID_ARG;
	}
	usCrc = prvCrc(pucRequest, usLength);
	pucRequest[usLength] = (uint8_t)usCrc;
	pucRequest[usLength + 1U] = (uint8_t)(usCrc >> 8);
	(void)xTransportControl(pxMaster->pxTransport,
		TRANSPORT_CTRL_RX_FLUSH, NULL);
	xTransportResult = xTransportSend(pxMaster->pxTransport, pucRequest,
		(uint16_t)(usLength + 2U), ulTimeoutMs);
	return prvMapTransport(xTransportResult);
}

/*-----------------------------------------------------------*/
static ModbusRtuResult_e prvReceiveExact(TransportChannel_t *pxTransport,
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
			return MODBUS_RTU_RESULT_TIMEOUT;
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
			return MODBUS_RTU_RESULT_TRANSPORT;
		}
		usOffset = (uint16_t)(usOffset + usReceived);
	}
	return MODBUS_RTU_RESULT_OK;
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
static uint16_t prvCrc(const uint8_t *pucData, uint16_t usLength)
{
	uint16_t usCrc;
	uint16_t usIndex;
	uint8_t ucBit;

	usCrc = 0xFFFFU;
	for (usIndex = 0U; usIndex < usLength; usIndex++) {
		usCrc ^= pucData[usIndex];
		for (ucBit = 0U; ucBit < 8U; ucBit++) {
			if ((usCrc & 1U) != 0U) {
				usCrc = (uint16_t)((usCrc >> 1) ^ 0xA001U);
			} else {
				usCrc >>= 1;
			}
		}
	}
	return usCrc;
}

/*-----------------------------------------------------------*/
static ModbusRtuResult_e prvValidateCrc(const uint8_t *pucData,
	uint16_t usLength)
{
	uint16_t usExpected;
	uint16_t usActual;

	if (usLength < 4U) {
		return MODBUS_RTU_RESULT_PROTOCOL;
	}
	usExpected = prvCrc(pucData, (uint16_t)(usLength - 2U));
	usActual = (uint16_t)(pucData[usLength - 2U] |
		((uint16_t)pucData[usLength - 1U] << 8));
	return (usExpected == usActual) ? MODBUS_RTU_RESULT_OK :
		MODBUS_RTU_RESULT_PROTOCOL;
}

/*-----------------------------------------------------------*/
static ModbusRtuResult_e prvMapTransport(TransportResult_e xResult)
{
	if (xResult == TRANSPORT_RESULT_OK) {
		return MODBUS_RTU_RESULT_OK;
	}
	if (xResult == TRANSPORT_RESULT_TIMEOUT) {
		return MODBUS_RTU_RESULT_TIMEOUT;
	}
	return MODBUS_RTU_RESULT_TRANSPORT;
}
