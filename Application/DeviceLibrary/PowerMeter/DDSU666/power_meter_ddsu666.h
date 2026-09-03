/**
  * @file      power_meter_ddsu666.h
  * @brief     Define the DDSU666 input-register power-meter driver.
  * @author    WHong
  * @date      2026-08-21
  *
  * @details   The driver uses FC04 exactly as required by the meter manual.
  *            IEEE-754 values use the high word at the lower register
  *            address, matching the existing product decoder.
  */

#ifndef POWER_METER_DDSU666_H
#define POWER_METER_DDSU666_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "device_library.h"
#include "modbus_port.h"

/** @brief Store DDSU666 instantaneous and accumulated engineering values. */
typedef struct {
	float fVoltage;
	float fCurrent;
	float fActivePower;
	float fReactivePower;
	float fApparentPower;
	float fPowerFactor;
	float fFrequency;
	float fEnergy;
} PowerMeterDdsu666Image_t;

/** @brief Describe the DDSU666 Modbus protocol. */
extern const DeviceDriverDescriptor_t g_xPowerMeterDdsu666Driver;

/**
  * @brief  Read and decode the DDSU666 input-register measurements.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[out] pxImage Caller-owned destination image.
  * @retval MODBUS_PORT_RESULT_OK Measurements decoded.
  * @retval MODBUS_PORT_RESULT_INVALID_ARG A pointer or timeout is bad.
  */
ModbusPortResult_e xPowerMeterDdsu666Refresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	PowerMeterDdsu666Image_t *pxImage);

#ifdef __cplusplus
}
#endif

#endif /* POWER_METER_DDSU666_H */
