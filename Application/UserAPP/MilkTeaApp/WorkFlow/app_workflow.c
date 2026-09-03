/**
  * @file      app_workflow.c
  * @brief     Implement the current product workflow owner task.
  * @author    WHong
  * @date      2026-07-28
  */

#include "app_workflow.h"

#include <string.h>
#include "io_state.h"

#include "task.h"

/** @brief Period of the current IO mapping workflow in milliseconds. */
#define APP_WORKFLOW_LOOP_MS               20U

/** @brief Observable status of the workflow owner task. */
AppWorkFlowStatus_t g_xWorkFlowStatus;

/*-----------------------------------------------------------*/
void vAppWorkFlowTask(void *pvArgument)
{
	TickType_t xLastWake;

	(void)pvArgument;
	memset(&g_xWorkFlowStatus, 0, sizeof(g_xWorkFlowStatus));
	g_xWorkFlowStatus.ucRunning = 1U;
	xLastWake = xTaskGetTickCount();
	for (;;) {
		vIOStateUpdate();

		/*
		 * Product sequencing is added here as device protocols become
		 * available. Communication remains owned by the TCP and RTU tasks.
		 */
		g_xWorkFlowStatus.ulLoopCount++;
		g_xWorkFlowStatus.xLastRunTick = xTaskGetTickCount();
		vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(APP_WORKFLOW_LOOP_MS));
	}
}
