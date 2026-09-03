/**
  * @file      syrup_machine_modbus.c
  * @brief     Implement the current four-channel syrup Modbus driver.
  * @author    WHong
  * @date      2026-08-21
  */

#include "syrup_machine_modbus.h"

#include <stddef.h>

const DeviceDriverDescriptor_t g_xSyrupMachineCurrentDriver = {
	DEVICE_DRIVER_SYRUP_CURRENT_MODBUS,
	DEVICE_CATEGORY_SYRUP_MACHINE,
	DEVICE_PROTOCOL_MODBUS_RTU
};

static ModbusPortResult_e prvStartAction(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint32_t ulTimeoutMs,
	SyrupMachineModbusImage_t *pxImage,
	DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext)
{
	ModbusPortResult_e xResult;

	(void)pxCancelCheck;
	(void)pvCancelContext;
	xResult = xModbusPortWriteRegister(pxPort, ucUnitId,
		usAddress, 1U, ulTimeoutMs);
	if ((xResult == MODBUS_PORT_RESULT_OK) && (pxImage != NULL)) {
		pxImage->ausRegisters[usAddress] = 1U;
	}
	return xResult;
}

ModbusPortResult_e xSyrupMachineRefresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	SyrupMachineModbusImage_t *pxImage)
{
	if ((pxPort == NULL) || (pxImage == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	return xModbusPortReadHolding(pxPort, ucUnitId, 0U,
		SYRUP_MACHINE_REGISTER_COUNT, pxImage->ausRegisters,
		ulTimeoutMs);
}

ModbusPortResult_e xSyrupMachineDispense(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucChannel, uint16_t usTimeTenthsS,
	uint32_t ulTimeoutMs, SyrupMachineModbusImage_t *pxImage,
	DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext)
{
	ModbusPortResult_e xResult;

	if ((pxPort == NULL) || (ulTimeoutMs == 0U) ||
		(ucChannel < SYRUP_MACHINE_CHANNEL_FIRST) ||
		(ucChannel > SYRUP_MACHINE_CHANNEL_LAST)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = xModbusPortWriteRegister(pxPort, ucUnitId, 0U,
		usTimeTenthsS, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	return prvStartAction(pxPort, ucUnitId, ucChannel, ulTimeoutMs,
		pxImage, pxCancelCheck, pvCancelContext);
}

ModbusPortResult_e xSyrupMachineClean(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	SyrupMachineModbusImage_t *pxImage,
	DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext)
{
	if ((pxPort == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	return prvStartAction(pxPort, ucUnitId, 5U, ulTimeoutMs,
		pxImage, pxCancelCheck, pvCancelContext);
}

ModbusPortResult_e xSyrupMachineSetRemaining(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usRemainingTenthsS,
	uint32_t ulTimeoutMs)
{
	if ((pxPort == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	return xModbusPortWriteRegister(pxPort, ucUnitId, 10U,
		usRemainingTenthsS, ulTimeoutMs);
}
