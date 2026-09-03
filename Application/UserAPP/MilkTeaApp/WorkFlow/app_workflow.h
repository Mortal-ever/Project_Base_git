/**
  * @file      app_workflow.h
  * @brief     Define the current product workflow task status.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_WORKFLOW_H
#define APP_WORKFLOW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"

/** @brief Store the observable state of the workflow task. */
typedef struct {
	TickType_t xLastRunTick; /*!< Tick of the latest completed cycle. */
	uint32_t ulLoopCount; /*!< Number of completed workflow cycles. */
	uint8_t ucRunning; /*!< Nonzero while the task loop is active. */
} AppWorkFlowStatus_t;

/** @brief Global workflow status exposed for diagnostics. */
extern AppWorkFlowStatus_t g_xWorkFlowStatus;

/**
  * @brief Run the product workflow owner task.
  * @param[in] pvArgument Reserved task argument; currently unused.
  * @note This function is a FreeRTOS task entry and never returns.
  */
void vAppWorkFlowTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* APP_WORKFLOW_H */
