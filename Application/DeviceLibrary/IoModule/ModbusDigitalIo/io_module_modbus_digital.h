/**
  * @file      io_module_modbus_digital.h
  * @brief     Define the generic Modbus digital input and output driver.
  * @author    WHong
  * @date      2026-08-21
  *
  * @details   The public capacity is 48 points, while every operation
  *            receives the actual point count used by the selected module.
  *            Input modules use FC02 only.  Output modules use FC01 for
  *            observation and FC05 for one-point writes.
  */

#ifndef IO_MODULE_MODBUS_DIGITAL_H
#define IO_MODULE_MODBUS_DIGITAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "device_library.h"
#include "modbus_port.h"

#define IO_MODULE_MODBUS_DIGITAL_MAX_POINTS 48U

/** @brief Store normalized logical states for one digital IO module. */
typedef struct {
	uint8_t aucPoints[IO_MODULE_MODBUS_DIGITAL_MAX_POINTS];
	uint8_t ucPointCount;
} IoModuleModbusDigitalImage_t;

/** @brief Describe the generic Modbus digital IO protocol. */
extern const DeviceDriverDescriptor_t g_xIoModuleModbusDigitalDriver;

/**
  * @brief  Read discrete inputs from an input-only module with FC02.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] usStartAddress First discrete-input address.
  * @param[in] ucPointCount Number of points from 1 through 48.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[out] pxImage Caller-owned normalized image.
  * @retval MODBUS_PORT_RESULT_OK Inputs read.
  * @retval MODBUS_PORT_RESULT_INVALID_ARG A pointer, count, or timeout is bad.
  */
ModbusPortResult_e xIoModuleModbusReadInputs(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usStartAddress, uint8_t ucPointCount,
	uint32_t ulTimeoutMs, IoModuleModbusDigitalImage_t *pxImage);

/**
  * @brief  Read coils from an output-only module with FC01.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] usStartAddress First coil address.
  * @param[in] ucPointCount Number of points from 1 through 48.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[out] pxImage Caller-owned normalized image.
  * @retval MODBUS_PORT_RESULT_OK Outputs read.
  * @retval MODBUS_PORT_RESULT_INVALID_ARG A pointer, count, or timeout is bad.
  */
ModbusPortResult_e xIoModuleModbusReadOutputs(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usStartAddress, uint8_t ucPointCount,
	uint32_t ulTimeoutMs, IoModuleModbusDigitalImage_t *pxImage);

/**
  * @brief  Write one output and verify it with an FC01 full-range readback.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] usStartAddress First coil address.
  * @param[in] ucPoint Zero-based point index within the requested range.
  * @param[in] ucValue Requested logical state, zero or nonzero.
  * @param[in] ucPointCount Number of points from 1 through 48.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[out] pxImage Caller-owned normalized readback image.
  * @retval MODBUS_PORT_RESULT_OK Write and readback matched.
  * @retval MODBUS_PORT_RESULT_PROTOCOL Readback did not match the target.
  */
ModbusPortResult_e xIoModuleModbusWriteOutput(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usStartAddress, uint8_t ucPoint,
	uint8_t ucValue, uint8_t ucPointCount, uint32_t ulTimeoutMs,
	IoModuleModbusDigitalImage_t *pxImage);

#ifdef __cplusplus
}
#endif

#endif /* IO_MODULE_MODBUS_DIGITAL_H */
