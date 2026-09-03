/**
  * @file      syrup_machine_modbus.h
  * @brief     Define the current four-channel syrup Modbus driver.
  * @author    WHong
  * @date      2026-08-21
  *
  * @details   Register addresses are part of this protocol.  Unit ID,
  *            baud rate, bus ownership, and product semantics remain with
  *            the caller.
  */

#ifndef SYRUP_MACHINE_MODBUS_H
#define SYRUP_MACHINE_MODBUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "device_library.h"
#include "modbus_port.h"

#define SYRUP_MACHINE_REGISTER_COUNT      15U
#define SYRUP_MACHINE_CHANNEL_FIRST       1U
#define SYRUP_MACHINE_CHANNEL_LAST        4U
#define SYRUP_MACHINE_STATUS_WORKING      2U
#define SYRUP_MACHINE_STATUS_SUCCESS      3U
#define SYRUP_MACHINE_STATUS_FAILED       4U
#define SYRUP_MACHINE_POLL_MS             100U

/** @brief Store the latest holding-register image. */
typedef struct {
	uint16_t ausRegisters[SYRUP_MACHINE_REGISTER_COUNT];
} SyrupMachineModbusImage_t;

/** @brief Describe the current syrup-machine Modbus protocol. */
extern const DeviceDriverDescriptor_t g_xSyrupMachineCurrentDriver;

/**
  * @brief  Read all documented syrup holding registers.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[out] pxImage Caller-owned destination image.
  * @retval MODBUS_PORT_RESULT_OK Read completed.
  * @retval MODBUS_PORT_RESULT_INVALID_ARG A pointer or timeout is bad.
  */
ModbusPortResult_e xSyrupMachineRefresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	SyrupMachineModbusImage_t *pxImage);

/**
  * @brief  Start one syrup channel and return after both FC06 responses.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ucChannel Channel number from 1 through 4.
  * @param[in] usTimeTenthsS Dispense time in 0.1 second units.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[in,out] pxImage Optional image marked as command accepted.
  * @param[in] pxCancelCheck Reserved for API compatibility.
  * @param[in] pvCancelContext Reserved for API compatibility.
  * @retval MODBUS_PORT_RESULT_OK The slave acknowledged both writes.
  * @retval MODBUS_PORT_RESULT_TIMEOUT The slave did not reply in time.
  */
ModbusPortResult_e xSyrupMachineDispense(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucChannel, uint16_t usTimeTenthsS,
	uint32_t ulTimeoutMs, SyrupMachineModbusImage_t *pxImage,
	DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext);

/**
  * @brief  Start the cleaning action and return after the FC06 response.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[in,out] pxImage Optional image marked as command accepted.
  * @param[in] pxCancelCheck Reserved for API compatibility.
  * @param[in] pvCancelContext Reserved for API compatibility.
  * @retval MODBUS_PORT_RESULT_OK The slave acknowledged the write.
  * @retval MODBUS_PORT_RESULT_TIMEOUT The slave did not reply in time.
  */
ModbusPortResult_e xSyrupMachineClean(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	SyrupMachineModbusImage_t *pxImage,
	DeviceCancelCheck_t pxCancelCheck, const void *pvCancelContext);

/**
  * @brief  Set the syrup remaining-time register.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] usRemainingTenthsS Remaining time in 0.1 second units.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @retval MODBUS_PORT_RESULT_OK Write completed.
  * @retval MODBUS_PORT_RESULT_INVALID_ARG A pointer or timeout is bad.
  */
ModbusPortResult_e xSyrupMachineSetRemaining(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint16_t usRemainingTenthsS,
	uint32_t ulTimeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* SYRUP_MACHINE_MODBUS_H */
