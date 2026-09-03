/**
  * @file      cup_lid_shengshu.c
  * @brief     Implement the ShengShu cup and lid Modbus device driver.
  * @author    WHong
  * @date      2026-08-21
  */

#include "cup_lid_shengshu.h"

#include <stddef.h>

const DeviceDriverDescriptor_t g_xShengShuCupDriver = {
	DEVICE_DRIVER_CUP_SHENGSHU_MODBUS,
	DEVICE_CATEGORY_CUP_MACHINE,
	DEVICE_PROTOCOL_MODBUS_RTU
};

const DeviceDriverDescriptor_t g_xShengShuLidDriver = {
	DEVICE_DRIVER_LID_SHENGSHU_MODBUS,
	DEVICE_CATEGORY_LID_MACHINE,
	DEVICE_PROTOCOL_MODBUS_RTU
};

static uint8_t prvRoleValid(CupLidRole_e xRole)
{
	return ((xRole == CUP_LID_ROLE_CUP) ||
		(xRole == CUP_LID_ROLE_LID)) ? 1U : 0U;
}

static uint16_t prvTaskStart(CupLidRole_e xRole)
{
	return (xRole == CUP_LID_ROLE_LID) ? 2U : 0U;
}

static uint16_t prvCoilStart(CupLidRole_e xRole)
{
	return (xRole == CUP_LID_ROLE_LID) ? 0x100EU : 0x1004U;
}

ModbusPortResult_e xCupLidShengShuRefresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, CupLidRole_e xRole, uint32_t ulTimeoutMs,
	CupLidShengShuImage_t *pxImage)
{
	ModbusPortResult_e xResult;
	bool abCoils[CUP_LID_SHENGSHU_COIL_COUNT];
	uint16_t usTaskStart;
	uint16_t usCoilStart;
	uint8_t ucIndex;

	if ((pxPort == NULL) || (pxImage == NULL) ||
		(prvRoleValid(xRole) == 0U) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	usTaskStart = prvTaskStart(xRole);
	usCoilStart = prvCoilStart(xRole);
	xResult = xModbusPortReadHolding(pxPort, ucUnitId, usTaskStart,
		CUP_LID_SHENGSHU_TASK_COUNT, pxImage->ausTask, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xResult = xModbusPortReadCoils(pxPort, ucUnitId, usCoilStart,
		CUP_LID_SHENGSHU_COIL_COUNT, abCoils, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	for (ucIndex = 0U; ucIndex < CUP_LID_SHENGSHU_COIL_COUNT;
		ucIndex++) {
		pxImage->aucCoils[ucIndex] = abCoils[ucIndex] ? 1U : 0U;
	}
	return MODBUS_PORT_RESULT_OK;
}

ModbusPortResult_e xCupLidShengShuRun(ModbusPort_t *pxPort,
	uint8_t ucUnitId, CupLidRole_e xRole, uint8_t ucSlot,
	uint32_t ulTimeoutMs, CupLidShengShuImage_t *pxImage,
	DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext)
{
	ModbusPortResult_e xResult;
	uint16_t usAddress;

	if ((pxPort == NULL) || (prvRoleValid(xRole) == 0U) ||
		(ucSlot >= CUP_LID_SHENGSHU_TASK_COUNT) ||
		(ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	usAddress = (uint16_t)(prvTaskStart(xRole) + ucSlot);
	(void)pxCancelCheck;
	(void)pvCancelContext;
	xResult = xModbusPortWriteRegister(pxPort, ucUnitId,
		usAddress, 1U, ulTimeoutMs);
	if ((xResult == MODBUS_PORT_RESULT_OK) && (pxImage != NULL)) {
		pxImage->ausTask[ucSlot] = 1U;
	}
	return xResult;
}
