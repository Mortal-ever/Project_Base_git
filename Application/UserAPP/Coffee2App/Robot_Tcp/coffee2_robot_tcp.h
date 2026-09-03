/**
  * @file      coffee2_robot_tcp.h
  * @brief     Define the Coffee2 Robot Modbus TCP owner task.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE2_ROBOT_TCP_H
#define COFFEE2_ROBOT_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee2_app_config.h"

#if (COFFEE2_ROBOT_PROTOCOL_VARIANT == \
	COFFEE2_ROBOT_PROTOCOL_2) || \
	(COFFEE2_ROBOT_PROTOCOL_VARIANT == COFFEE2_ROBOT_PROTOCOL_3)
#define COFFEE2_ROBOT_CONTROL_COIL_COUNT  61U
#else
#define COFFEE2_ROBOT_CONTROL_COIL_COUNT  40U
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
} Coffee2RobotTcpStatus_t;

/** @brief Store Robot base inputs and action/status coils. */
typedef struct {
	uint8_t aucBaseInputs[16];
	uint8_t aucControlCoils[COFFEE2_ROBOT_CONTROL_COIL_COUNT];
} Coffee2RobotData_t;

extern Coffee2RobotTcpStatus_t g_xCoffee2RobotTcpStatus;
extern Coffee2RobotData_t g_xCoffee2RobotData;

/**
  * @brief Create and register the static Robot command queue.
  * @retval pdPASS Queue creation succeeded.
  * @retval pdFAIL Queue creation failed.
  */
BaseType_t xCoffee2RobotTcpInitialize(void);

/**
  * @brief Maintain Robot connection, execute commands, and reconnect.
  * @param[in] pvArgument Unused.
  */
void vCoffee2RobotTcpTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE2_ROBOT_TCP_H */
