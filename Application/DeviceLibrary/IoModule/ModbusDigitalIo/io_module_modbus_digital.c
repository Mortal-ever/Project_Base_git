/**
  * @file      io_module_modbus_digital.c
  * @brief     Implement the generic Modbus digital input and output driver.
  * @author    WHong
  * @date      2026-08-21
  */

#include "io_module_modbus_digital.h"

#include <stdbool.h>
#include <stddef.h>

const DeviceDriverDescriptor_t g_xIoModuleModbusDigitalDriver = {
	DEVICE_DRIVER_IO_MODBUS_DIGITAL,
	DEVICE_CATEGORY_IO,
	DEVICE_PROTOCOL_MODBUS_RTU
};

static uint8_t prvRangeValid(uint16_t usStartAddress,
	uint8_t ucPointCount)
{
	if ((ucPointCount == 0U) ||
		(ucPointCount > IO_MODULE_MODBUS_DIGITAL_MAX_POINTS)) {
		return 0U;
	}
	if ((uint32_t)usStartAddress + (uint32_t)ucPointCount > 65536UL) {
		return 0U;
	}
	return 1U;
}

static void prvCopyBits(IoModuleModbusDigitalImage_t *pxImage,
	const bool *pbValues, uint8_t ucPointCount)
{
	uint8_t ucIndex;

	pxImage->ucPointCount = ucPointCount;
	for (ucIndex = 0U; ucIndex < ucPointCount; ucIndex++) {
		pxImage->aucPoints[ucIndex] = pbValues[ucIndex] ? 1U : 0U;
	}
}

ModbusPortResult_e xIoModuleModbusReadInputs(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usStartAddress, uint8_t ucPointCount,
	uint32_t ulTimeoutMs, IoModuleModbusDigitalImage_t *pxImage)
{
	ModbusPortResult_e xResult;
	bool abValues[IO_MODULE_MODBUS_DIGITAL_MAX_POINTS];

	if ((pxPort == NULL) || (pxImage == NULL) || (ulTimeoutMs == 0U) ||
		(prvRangeValid(usStartAddress, ucPointCount) == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = xModbusPortReadDiscreteInputs(pxPort, ucUnitId,
		usStartAddress, ucPointCount, abValues, ulTimeoutMs);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		prvCopyBits(pxImage, abValues, ucPointCount);
	}
	return xResult;
}

ModbusPortResult_e xIoModuleModbusReadOutputs(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usStartAddress, uint8_t ucPointCount,
	uint32_t ulTimeoutMs, IoModuleModbusDigitalImage_t *pxImage)
{
	ModbusPortResult_e xResult;
	bool abValues[IO_MODULE_MODBUS_DIGITAL_MAX_POINTS];

	if ((pxPort == NULL) || (pxImage == NULL) || (ulTimeoutMs == 0U) ||
		(prvRangeValid(usStartAddress, ucPointCount) == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = xModbusPortReadCoils(pxPort, ucUnitId,
		usStartAddress, ucPointCount, abValues, ulTimeoutMs);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		prvCopyBits(pxImage, abValues, ucPointCount);
	}
	return xResult;
}

ModbusPortResult_e xIoModuleModbusWriteOutput(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usStartAddress, uint8_t ucPoint,
	uint8_t ucValue, uint8_t ucPointCount, uint32_t ulTimeoutMs,
	IoModuleModbusDigitalImage_t *pxImage)
{
	ModbusPortResult_e xResult;
	bool abValues[IO_MODULE_MODBUS_DIGITAL_MAX_POINTS];
	uint8_t ucExpected;

	if ((pxPort == NULL) || (pxImage == NULL) || (ulTimeoutMs == 0U) ||
		(prvRangeValid(usStartAddress, ucPointCount) == 0U) ||
		(ucPoint >= ucPointCount)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	ucExpected = (ucValue != 0U) ? 1U : 0U;
	xResult = xModbusPortWriteCoil(pxPort, ucUnitId,
		(uint16_t)(usStartAddress + ucPoint), (ucExpected != 0U),
		ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xResult = xModbusPortReadCoils(pxPort, ucUnitId,
		usStartAddress, ucPointCount, abValues, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	prvCopyBits(pxImage, abValues, ucPointCount);
	if (pxImage->aucPoints[ucPoint] != ucExpected) {
		return MODBUS_PORT_RESULT_PROTOCOL;
	}
	return MODBUS_PORT_RESULT_OK;
}
