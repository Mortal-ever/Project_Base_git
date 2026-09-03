/**
  * @file      power_meter_ddsu666.c
  * @brief     Implement the DDSU666 input-register power-meter driver.
  * @author    WHong
  * @date      2026-08-21
  */

#include "power_meter_ddsu666.h"

#include <stddef.h>
#include <string.h>

const DeviceDriverDescriptor_t g_xPowerMeterDdsu666Driver = {
	DEVICE_DRIVER_POWER_METER_DDSU666,
	DEVICE_CATEGORY_POWER_METER,
	DEVICE_PROTOCOL_MODBUS_RTU
};

static float prvRegistersToFloat(const uint16_t *pusRegisters)
{
	uint32_t ulBits;
	float fValue;

	ulBits = ((uint32_t)pusRegisters[0] << 16) |
		(uint32_t)pusRegisters[1];
	memcpy(&fValue, &ulBits, sizeof(fValue));
	return fValue;
}

ModbusPortResult_e xPowerMeterDdsu666Refresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	PowerMeterDdsu666Image_t *pxImage)
{
	ModbusPortResult_e xResult;
	uint16_t ausInstantaneous[16];
	uint16_t ausEnergy[2];

	if ((pxPort == NULL) || (pxImage == NULL) || (ulTimeoutMs == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	xResult = xModbusPortReadInputRegisters(pxPort, ucUnitId,
		0x2000U, 16U, ausInstantaneous, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xResult = xModbusPortReadInputRegisters(pxPort, ucUnitId,
		0x4000U, 2U, ausEnergy, ulTimeoutMs);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	pxImage->fVoltage = prvRegistersToFloat(&ausInstantaneous[0]);
	pxImage->fCurrent = prvRegistersToFloat(&ausInstantaneous[2]);
	pxImage->fActivePower = prvRegistersToFloat(&ausInstantaneous[4]);
	pxImage->fReactivePower = prvRegistersToFloat(&ausInstantaneous[6]);
	pxImage->fApparentPower = prvRegistersToFloat(&ausInstantaneous[8]);
	pxImage->fPowerFactor = prvRegistersToFloat(&ausInstantaneous[10]);
	pxImage->fFrequency = prvRegistersToFloat(&ausInstantaneous[14]);
	pxImage->fEnergy = prvRegistersToFloat(ausEnergy);
	return MODBUS_PORT_RESULT_OK;
}
