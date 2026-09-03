/**
  * @file      app_modbus_tcp_client.h
  * @brief     Define Robot and MilkTea Modbus TCP client tasks.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_MODBUS_TCP_CLIENT_H
#define APP_MODBUS_TCP_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "app_device_task.h"
#include "milktea_protocol.h"
#include "modbus_port.h"

/** @brief Define initialization results for the TCP client application. */
typedef enum {
	APP_MODBUS_TCP_RESULT_OK = 0, /*!< Both endpoint objects are ready. */
	APP_MODBUS_TCP_RESULT_ALREADY_INITIALIZED = 1,
		/*!< Initialization was already completed. */
	APP_MODBUS_TCP_RESULT_INVALID_ARG = -1, /*!< Static setup is invalid. */
	APP_MODBUS_TCP_RESULT_NO_RESOURCE = -2 /*!< Mutex creation failed. */
} AppModbusTcpResult_e;

/** @brief Store one TCP endpoint's observable runtime status. */
typedef struct {
	ModbusPortResult_e xLastResult; /*!< Latest Modbus transaction result. */
	TickType_t xLastSuccessTick; /*!< Tick of the latest successful request. */
	uint32_t ulRequestCount; /*!< Number of attempted Modbus requests. */
	uint32_t ulErrorCount; /*!< Number of failed Modbus requests. */
	uint32_t ulConnectAttemptCount; /*!< Number of TCP open attempts. */
	uint32_t ulConnectSuccessCount; /*!< Number of successful TCP opens. */
	uint32_t ulDisconnectCount; /*!< Number of endpoint disconnects. */
	uint32_t ulConsecutiveFailures; /*!< Failures used for backoff selection. */
	TickType_t xNextRetryTick; /*!< Earliest Tick for the next open attempt. */
	AppDeviceCommState_e xState; /*!< Current endpoint lifecycle state. */
	uint8_t ucConnected; /*!< Nonzero while the peer service is usable. */
	uint8_t ucEnabled; /*!< Nonzero when product configuration enables it. */
} AppModbusTcpStatus_t;

/** @brief Runtime status for the Robot TCP endpoint. */
extern AppModbusTcpStatus_t g_xRobotTcpStatus;
/** @brief Runtime status for the MilkTea TCP endpoint. */
extern AppModbusTcpStatus_t g_xMilkTeaTcpStatus;

/**
  * @brief Initialize both statically configured Modbus TCP endpoints.
  * @retval APP_MODBUS_TCP_RESULT_OK Endpoint objects were initialized.
  * @retval APP_MODBUS_TCP_RESULT_ALREADY_INITIALIZED Setup already ran.
  * @retval APP_MODBUS_TCP_RESULT_NO_RESOURCE A required mutex is unavailable.
  */
AppModbusTcpResult_e xAppModbusTcpClientInit(void);

/**
  * @brief Run the Robot TCP connection and health-monitor task.
  * @param[in] pvArgument Reserved task argument; currently unused.
  * @note This FreeRTOS task entry never returns.
  */
void vAppRobotTcpTask(void *pvArgument);

/**
  * @brief Run the MilkTea TCP connection and health-monitor task.
  * @param[in] pvArgument Reserved task argument; currently unused.
  * @note This FreeRTOS task entry never returns.
  */
void vAppMilkTeaTcpTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* APP_MODBUS_TCP_CLIENT_H */
