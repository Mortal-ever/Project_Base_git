/**
  * @file      coffee3_robot_tcp.h
  * @brief     Define the Coffee3 Robot Modbus TCP owner task.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE3_ROBOT_TCP_H
#define COFFEE3_ROBOT_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"

#if (COFFEE3_ROBOT_PROTOCOL_VARIANT == \
	COFFEE3_ROBOT_PROTOCOL_2) || \
	(COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_3)
#define COFFEE3_ROBOT_CONTROL_COIL_COUNT  61U
#else
#define COFFEE3_ROBOT_CONTROL_COIL_COUNT  40U
#endif

/** @brief Store Robot TCP lifecycle and transaction counters. */
typedef struct {
	uint32_t ulConnectAttemptCount;
	uint32_t ulConnectSuccessCount;
	uint32_t ulDisconnectCount;
	uint32_t ulCommandCount;
	uint32_t ulErrorCount;
	uint32_t ulConsecutiveFailures;
	uint32_t ulNextRetryDelayMs;
	int32_t lLastResult;
	uint8_t ucConnected;
	uint8_t ucReady;
} Coffee3RobotTcpStatus_t;

/** @brief Store Robot base inputs and action/status coils. */
typedef struct {
	uint8_t aucBaseInputs[16];
	uint8_t aucControlCoils[COFFEE3_ROBOT_CONTROL_COIL_COUNT];
} Coffee3RobotData_t;

extern Coffee3RobotTcpStatus_t g_xCoffee3RobotTcpStatus;
extern Coffee3RobotData_t g_xCoffee3RobotData;

/**
  * @brief Create and register the static Robot command queue.
  * @retval pdPASS Queue creation succeeded.
  * @retval pdFAIL Queue creation failed.
  */
BaseType_t xCoffee3RobotTcpInitialize(void);

/**
  * @brief Maintain Robot connection, execute commands, and reconnect.
  * @param[in] pvArgument Unused.
  */
void vCoffee3RobotTcpTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_ROBOT_TCP_H */
