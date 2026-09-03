/**
  * @file      ice_machine_modbus.h
  * @brief     Define the current ice-machine Modbus device driver.
  * @author    WHong
  * @date      2026-08-21
  *
  * @details   The image follows documented holding registers 1 through 13.
  *            Device fault bits are decoded separately from communication
  *            results so a valid status frame is never reported as a link
  *            or protocol failure solely because the machine has a fault.
  */

#ifndef ICE_MACHINE_MODBUS_H
#define ICE_MACHINE_MODBUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "device_library.h"
#include "modbus_port.h"

#define ICE_MACHINE_REGISTER_COUNT       13U
#define ICE_MACHINE_FAULT_INLET_BIT     0x01U
#define ICE_MACHINE_FAULT_WATER_LINE_BIT 0x02U
#define ICE_MACHINE_FAULT_OVERLOAD_BIT  0x04U

/** @brief Store the latest registers 1 through 13. */
typedef struct {
	uint16_t ausRegisters[ICE_MACHINE_REGISTER_COUNT];
} IceMachineModbusImage_t;

/** @brief Describe the current ice-machine Modbus protocol. */
extern const DeviceDriverDescriptor_t g_xIceMachineCurrentDriver;

/**
  * @brief  Read the documented ice-machine holding-register block.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @param[out] pxImage Caller-owned destination image.
  * @retval MODBUS_PORT_RESULT_OK Read completed.
  * @retval MODBUS_PORT_RESULT_INVALID_ARG A pointer or timeout is bad.
  */
ModbusPortResult_e xIceMachineRefresh(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint32_t ulTimeoutMs,
	IceMachineModbusImage_t *pxImage);

/**
  * @brief  Decode the documented inlet, water-line, and overload faults.
  * @param[in] pxImage Latest image returned by xIceMachineRefresh.
  * @retval 0 No decoded machine fault is active.
  * @retval Nonzero Bit mask of active machine faults.
  */
uint8_t ucIceMachineGetFaultMask(const IceMachineModbusImage_t *pxImage);

/**
  * @brief  Set the ice-machine power register.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ucEnabled Zero disables, nonzero enables.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @retval MODBUS_PORT_RESULT_OK Write completed.
  */
ModbusPortResult_e xIceMachineSetPower(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucEnabled, uint32_t ulTimeoutMs);

/**
  * @brief  Set the ice-machine inlet-valve register.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ucEnabled Zero closes, nonzero opens.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @retval MODBUS_PORT_RESULT_OK Write completed.
  */
ModbusPortResult_e xIceMachineSetValve(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucEnabled, uint32_t ulTimeoutMs);

/**
  * @brief  Set the ice-machine pump 1 register.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ucEnabled Zero stops, nonzero starts.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @retval MODBUS_PORT_RESULT_OK Write completed.
  */
ModbusPortResult_e xIceMachineSetPump1(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucEnabled, uint32_t ulTimeoutMs);

/**
  * @brief  Set the ice-machine pump 3 register.
  * @param[in,out] pxPort Initialized Modbus client port.
  * @param[in] ucUnitId Device Unit ID.
  * @param[in] ucEnabled Zero stops, nonzero starts.
  * @param[in] ulTimeoutMs Modbus transaction timeout in milliseconds.
  * @retval MODBUS_PORT_RESULT_OK Write completed.
  */
ModbusPortResult_e xIceMachineSetPump3(ModbusPort_t *pxPort,
	uint8_t ucUnitId, uint8_t ucEnabled, uint32_t ulTimeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* ICE_MACHINE_MODBUS_H */
