/**
  * @file      ice_machine_modbus.c
  * @brief     Implement the current ice-machine Modbus device driver.
  * @author    WHong
  * @date      2026-08-21
  */

#include "ice_machine_modbus.h"

#include <stddef.h>

#define ICE_MACHINE_FIRST_REGISTER       1U
#define ICE_MACHINE_READ_FUNCTION        3U
#define ICE_MACHINE_RESPONSE_DATA_LENGTH \
	(2U + (2U * ICE_MACHINE_REGISTER_COUNT))

const DeviceDriverDescriptor_t g_xIceMachineCurrentDriver = {
	DEVICE_DRIVER_ICE_CURRENT_MODBUS,
	DEVICE_CATEGORY_ICE_MACHINE,
	DEVICE_PROTOCOL_MODBUS_RTU
};

ModbusPortResult_e xIceMachineRefresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	IceMachineModbusImage_t *pxImage)
{
	const uint8_t aucRequest[4] = {
		0U, ICE_MACHINE_FIRST_REGISTER,
		0U, ICE_MACHINE_REGISTER_COUNT
	};
	uint8_t aucResponse[ICE_MACHINE_RESPONSE_DATA_LENGTH];
	ModbusPortResult_e xResult;
	uint8_t ucIndex;

	if ((pxPort == NULL) || (pxImage == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	/* This ice controller replaces the standard FC03 byte count with a
	 * two-byte starting-address echo. The shared raw-PDU path still checks
	 * unit ID, function code, RTU CRC and the transaction deadline. */
	xResult = xModbusPortRawRequest(pxPort, ucUnitId,
		ICE_MACHINE_READ_FUNCTION, aucRequest, sizeof(aucRequest),
		aucResponse, sizeof(aucResponse), ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	if ((aucResponse[0] != aucRequest[0]) ||
		(aucResponse[1] != aucRequest[1])) {
		return MODBUS_PORT_RESULT_PROTOCOL;
	}
	for (ucIndex = 0U; ucIndex < ICE_MACHINE_REGISTER_COUNT; ucIndex++) {
		pxImage->ausRegisters[ucIndex] =
			(uint16_t)(((uint16_t)aucResponse[2U + (2U * ucIndex)] << 8) |
				aucResponse[3U + (2U * ucIndex)]);
	}
	return MODBUS_PORT_RESULT_OK;
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

const char *pcIceMachineFaultReason(uint8_t ucFaultMask)
{
	switch (ucFaultMask & (ICE_MACHINE_FAULT_INLET_BIT |
		ICE_MACHINE_FAULT_WATER_LINE_BIT |
		ICE_MACHINE_FAULT_OVERLOAD_BIT)) {
	case 0U: return "None";
	case ICE_MACHINE_FAULT_INLET_BIT: return "Water_Inlet";
	case ICE_MACHINE_FAULT_WATER_LINE_BIT: return "Water_Level";
	case ICE_MACHINE_FAULT_INLET_BIT |
		ICE_MACHINE_FAULT_WATER_LINE_BIT:
		return "Water_Inlet,Water_Level";
	case ICE_MACHINE_FAULT_OVERLOAD_BIT: return "Motor_Overload";
	case ICE_MACHINE_FAULT_INLET_BIT |
		ICE_MACHINE_FAULT_OVERLOAD_BIT:
		return "Water_Inlet,Motor_Overload";
	case ICE_MACHINE_FAULT_WATER_LINE_BIT |
		ICE_MACHINE_FAULT_OVERLOAD_BIT:
		return "Water_Level,Motor_Overload";
	default:
		return "Water_Inlet,Water_Level,Motor_Overload";
	}
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
