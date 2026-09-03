/**
  * @file      scale_bsq_dg_v2.c
  * @brief     Implement the BSQ-DG-V2 weighing-module Modbus driver.
  * @author    WHong
  * @date      2026-08-21
  */

#include "scale_bsq_dg_v2.h"

#include <stddef.h>

const DeviceDriverDescriptor_t g_xScaleBsqDgV2Driver = {
	DEVICE_DRIVER_SCALE_BSQ_DG_V2,
	DEVICE_CATEGORY_SCALE,
	DEVICE_PROTOCOL_MODBUS_RTU
};

ModbusPortResult_e xScaleBsqDgV2Refresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	ScaleBsqDgV2Image_t *pxImage)
{
	ModbusPortResult_e xResult;
	uint16_t ausValues[SCALE_BSQ_DG_V2_REGISTER_COUNT];
	uint16_t usIndex;
	int32_t lScale;
	int32_t lRawValue;

	if ((pxPort == NULL) || (pxImage == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = xModbusPortReadHolding(pxPort, ucUnitId, 0U,
		SCALE_BSQ_DG_V2_REGISTER_COUNT, ausValues, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	pxImage->sRawValue = (int16_t)ausValues[0];
	pxImage->usDecimalPlaces = ausValues[1];
	pxImage->usUnit = ausValues[2];
	if (pxImage->usDecimalPlaces > 4U) {
		return MODBUS_PORT_RESULT_PROTOCOL;
	}
	if ((pxImage->usUnit != SCALE_BSQ_DG_V2_UNIT_KG) &&
		(pxImage->usUnit != SCALE_BSQ_DG_V2_UNIT_G)) {
		return MODBUS_PORT_RESULT_PROTOCOL;
	}
	lScale = 1;
	for (usIndex = 0U; usIndex < pxImage->usDecimalPlaces; usIndex++) {
		lScale *= 10;
	}
	lRawValue = (int32_t)pxImage->sRawValue;
	if (pxImage->usUnit == SCALE_BSQ_DG_V2_UNIT_KG) {
		pxImage->lWeightTenthGram = (lRawValue * 10000) / lScale;
	} else {
		pxImage->lWeightTenthGram = (lRawValue * 10) / lScale;
	}
	return MODBUS_PORT_RESULT_OK;
}

static ModbusPortResult_e prvWriteCommand(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint16_t usValue,
	uint32_t ulTimeoutMs)
{
	if ((pxPort == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	return xModbusPortWriteRegister(pxPort, ucUnitId, usAddress,
		usValue, ulTimeoutMs);
}

ModbusPortResult_e xScaleBsqDgV2Tare(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs)
{
	return prvWriteCommand(pxPort, ucUnitId, 0x0011U, 1U,
		ulTimeoutMs);
}

ModbusPortResult_e xScaleBsqDgV2ClearTare(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs)
{
	return prvWriteCommand(pxPort, ucUnitId, 0x0011U, 2U,
		ulTimeoutMs);
}

ModbusPortResult_e xScaleBsqDgV2Zero(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs)
{
	return prvWriteCommand(pxPort, ucUnitId, 0x0060U, 1U,
		ulTimeoutMs);
}
