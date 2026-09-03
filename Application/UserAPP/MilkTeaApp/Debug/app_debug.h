/**
  * @file      app_debug.h
  * @brief     Define the UART device-protocol Debug command service.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_DEBUG_H
#define APP_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "modbus_port.h"
#include "semphr.h"

/** @brief Permit Debug FC01 read-coil commands. */
#define APP_DEBUG_FC01                     (1UL << 0)
/** @brief Permit Debug FC03 read-holding-register commands. */
#define APP_DEBUG_FC03                     (1UL << 1)
/** @brief Permit Debug FC06 write-single-register commands. */
#define APP_DEBUG_FC06                     (1UL << 2)
/** @brief Permit Debug FC16 write-multiple-register commands. */
#define APP_DEBUG_FC16                     (1UL << 3)

/** @brief Define Debug service initialization and registration results. */
typedef enum {
	APP_DEBUG_RESULT_OK = 0, /*!< Operation completed successfully. */
	APP_DEBUG_RESULT_ALREADY_REGISTERED = 1,
		/*!< The supplied name or object is already registered. */
	APP_DEBUG_RESULT_INVALID_ARG = -1, /*!< Descriptor validation failed. */
	APP_DEBUG_RESULT_NO_RESOURCE = -2 /*!< Registration table is full. */
} AppDebugResult_e;

/**
  * @brief Test whether a registered device is ready for a real transaction.
  * @param[in] pvContext Device-owned callback context.
  * @retval 1 The device may execute a transaction.
  * @retval 0 The device is offline or unavailable.
  */
typedef uint8_t (*AppDebugReadyCallback_t)(void *pvContext);

/**
  * @brief Report the result of a completed Debug Modbus transaction.
  * @param[in] xResult Modbus transaction result.
  * @param[in] pvContext Device-owned callback context.
  */
typedef void (*AppDebugResultCallback_t)(ModbusPortResult_e xResult,
	void *pvContext);

/** @brief Describe one TCP device exposed to the UART Debug service. */
typedef struct {
	const char *pcName; /*!< Stable command name; storage must persist. */
	ModbusPort_t *pxClient; /*!< Initialized client used for real requests. */
	ModbusPortTrace_t *pxTrace; /*!< Frame trace attached to pxClient. */
	SemaphoreHandle_t xMutex; /*!< Endpoint mutex shared with its owner task. */
	AppDebugReadyCallback_t pxIsReady; /*!< Optional readiness callback. */
	AppDebugResultCallback_t pxResult; /*!< Optional result callback. */
	void *pvContext; /*!< Opaque context passed to both callbacks. */
	uint32_t ulFunctionMask; /*!< Allowed APP_DEBUG_FC* bit mask. */
	uint16_t usCoilCount; /*!< Valid coil address-space size. */
	uint16_t usHoldingCount; /*!< Valid holding-register space size. */
	uint16_t usMaxWriteRegisters; /*!< Maximum FC16 quantity per command. */
} AppDebugTcpDevice_t;

/** @brief Reset the static Debug registration and input state. */
void vAppDebugInit(void);

/**
  * @brief Register one TCP device for UART Debug commands.
  * @param[in] pxDevice Persistent device descriptor copied by the service.
  * @retval APP_DEBUG_RESULT_OK The device was registered.
  * @retval APP_DEBUG_RESULT_ALREADY_REGISTERED The name already exists.
  * @retval APP_DEBUG_RESULT_INVALID_ARG The descriptor is incomplete.
  * @retval APP_DEBUG_RESULT_NO_RESOURCE The registration table is full.
  */
AppDebugResult_e xAppDebugRegisterTcp(
	const AppDebugTcpDevice_t *pxDevice);

/**
  * @brief Run the UART Debug command parser and executor task.
  * @param[in] pvArgument Reserved task argument; currently unused.
  * @note This FreeRTOS task entry never returns.
  */
void vAppDebugTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_H */
