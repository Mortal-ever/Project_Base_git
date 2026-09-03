/**
  * @file      coffee2_workflow.h
  * @brief     Define Coffee2 order workflow and observable state.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE2_WORKFLOW_H
#define COFFEE2_WORKFLOW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee2_app_config.h"

/** @brief Registers copied from one accepted host order. */
#define COFFEE2_ORDER_REGISTER_COUNT          32U

/** @brief Store one immutable accepted order snapshot. */
typedef struct {
	uint16_t ausRegister[COFFEE2_ORDER_REGISTER_COUNT];
} Coffee2Order_t;

/** @brief Define high-level workflow lifecycle. */
typedef enum {
	COFFEE2_WORKFLOW_IDLE = 0,
	COFFEE2_WORKFLOW_RUNNING = 1,
	COFFEE2_WORKFLOW_COMPLETED = 2,
	COFFEE2_WORKFLOW_FAILED = 3,
	COFFEE2_WORKFLOW_CANCELING = 4
} Coffee2WorkflowState_e;

/** @brief Host-visible whole-machine state. */
typedef enum {
	COFFEE2_MACHINE_DEFAULT = 0,
	COFFEE2_MACHINE_IDLE = 1,
	COFFEE2_MACHINE_INITIALIZING = 2,
	COFFEE2_MACHINE_BUSY = 3,
	COFFEE2_MACHINE_ALARM = 4
} Coffee2MachineState_e;

/** @brief Maintenance operations serialized by the Workflow owner. */
typedef enum {
	COFFEE2_MAINTENANCE_NONE = 0,
	COFFEE2_MAINTENANCE_SYRUP_CLEAN = 1,
	COFFEE2_MAINTENANCE_COFFEE_CLEAN = 2,
	COFFEE2_MAINTENANCE_FRUIT_DISPENSE = 3,
	COFFEE2_MAINTENANCE_FRUIT_CLEAN = 4
} Coffee2MaintenanceType_e;

/** @brief Compact maintenance states projected to host registers. */
typedef enum {
	COFFEE2_MAINTENANCE_IDLE = 0,
	COFFEE2_MAINTENANCE_RUNNING = 1,
	COFFEE2_MAINTENANCE_COMPLETED = 2,
	COFFEE2_MAINTENANCE_FAILED = 3,
	COFFEE2_MAINTENANCE_ALARM = 4
} Coffee2MaintenanceState_e;

/** @brief Store workflow progress for Server monitoring. */
typedef struct {
	uint32_t ulCompletedOrderCount;
	uint32_t ulFailedOrderCount;
	uint32_t ulOrderEpoch;
	uint16_t usCurrentOrderId;
	uint16_t usCurrentStep;
	int32_t lLastError;
	Coffee2WorkflowState_e xState;
	Coffee2MachineState_e xMachineState;
	uint8_t ucCancelRequested;
	uint8_t ucOrderAdmissionOpen;
	uint8_t ucHotWaterState;
	uint8_t ucCoffeeCleanState;
	uint8_t aucFruitState[2];
} Coffee2WorkflowStatus_t;

extern Coffee2WorkflowStatus_t g_xCoffee2WorkflowStatus;

/**
  * @brief Create the bounded static order queue.
  * @retval pdPASS Queue creation succeeded.
  * @retval pdFAIL Queue creation failed.
  */
BaseType_t xCoffee2WorkflowInitialize(void);

/**
  * @brief Submit an accepted and verified order snapshot.
  * @param[in] pxOrder Immutable order copied into the workflow queue.
  * @retval pdPASS The order was accepted.
  * @retval pdFAIL Workflow is busy or the queue is full.
  */
BaseType_t xCoffee2WorkflowSubmitOrder(const Coffee2Order_t *pxOrder);

/**
  * @brief Submit a maintenance ice-dispense request using scale feedback.
  * @param[in] usTargetWeight Target weight in 0.1 g from register 0x0071.
  * @retval pdPASS The maintenance request was accepted.
  * @retval pdFAIL Workflow is busy, pending, or the target is zero.
  */
BaseType_t xCoffee2WorkflowSubmitManualIce(uint16_t usTargetWeight);

/** @brief Submit one non-order maintenance operation to Workflow. */
BaseType_t xCoffee2WorkflowSubmitMaintenance(
	Coffee2MaintenanceType_e xType, uint16_t usParameter0,
	uint16_t usParameter1);

/** @brief Start or stop the independent hot-water state machine. */
BaseType_t xCoffee2WorkflowSetHotWater(uint8_t ucStart,
	uint16_t usHeatMinutes);

/** @brief Acknowledge an initialization or maintenance alarm. */
void vCoffee2WorkflowAcknowledgeAlarm(void);

/** @brief Request cooperative cancellation of the active workflow. */
void vCoffee2WorkflowRequestCancel(void);

/**
  * @brief Run deterministic device steps and event-driven error handling.
  * @param[in] pvArgument Unused.
  */
void vCoffee2WorkflowTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE2_WORKFLOW_H */
