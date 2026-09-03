/**
  * @file      ice_machine_modbus.c
  * @brief     Implement the current ice-machine Modbus device driver.
  * @author    WHong
  * @date      2026-08-21
  */

#include "ice_machine_modbus.h"

#include <stddef.h>

const DeviceDriverDescriptor_t g_xIceMachineCurrentDriver = {
	DEVICE_DRIVER_ICE_CURRENT_MODBUS,
	DEVICE_CATEGORY_ICE_MACHINE,
	DEVICE_PROTOCOL_MODBUS_RTU
};

ModbusPortResult_e xIceMachineRefresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	IceMachineModbusImage_t *pxImage)
{
	if ((pxPort == NULL) || (pxImage == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	return xModbusPortReadHolding(pxPort, ucUnitId, 1U,
		ICE_MACHINE_REGISTER_COUNT, pxImage->ausRegisters, ulTimeoutMs);
}

uint8_t ucIceMachineGetFaultMask(const IceMachineModbusImage_t *pxImage)
{
	uint8_t ucMask;

	if (pxImage == NULL) {
		return 0U;
	}
	ucMask = 0U;
	if (pxImage->ausRegisters[3] != 0U) {
		ucMask = (uint8_t)(ucMask | ICE_MACHINE_FAULT_INLET_BIT);
	}
	if (pxImage->ausRegisters[4] != 0U) {
		ucMask = (uint8_t)(ucMask | ICE_MACHINE_FAULT_WATER_LINE_BIT);
	}
	if (pxImage->ausRegisters[5] != 0U) {
		ucMask = (uint8_t)(ucMask | ICE_MACHINE_FAULT_OVERLOAD_BIT);
	}
	return ucMask;
}

static ModbusPortResult_e prvSetBinaryRegister(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usAddress, uint8_t ucEnabled,
	uint32_t ulTimeoutMs)
{
	if ((pxPort == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	return xModbusPortWriteRegister(pxPort, ucUnitId, usAddress,
		(ucEnabled != 0U) ? 1U : 0U, ulTimeoutMs);
}

ModbusPortResult_e xIceMachineSetPower(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucEnabled, uint32_t ulTimeoutMs)
{
	return prvSetBinaryRegister(pxPort, ucUnitId, 7U, ucEnabled,
		ulTimeoutMs);
}

ModbusPortResult_e xIceMachineSetValve(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucEnabled, uint32_t ulTimeoutMs)
{
	return prvSetBinaryRegister(pxPort, ucUnitId, 10U, ucEnabled,
		ulTimeoutMs);
}

ModbusPortResult_e xIceMachineSetPump1(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucEnabled, uint32_t ulTimeoutMs)
{
	return prvSetBinaryRegister(pxPort, ucUnitId, 12U, ucEnabled,
		ulTimeoutMs);
}

ModbusPortResult_e xIceMachineSetPump3(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucEnabled, uint32_t ulTimeoutMs)
{
	return prvSetBinaryRegister(pxPort, ucUnitId, 13U, ucEnabled,
		ulTimeoutMs);
}
