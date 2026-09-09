/**
  * @file      coffee3_workflow.h
  * @brief     Define Coffee3 order workflow and observable state.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE3_WORKFLOW_H
#define COFFEE3_WORKFLOW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"

/** @brief Registers copied from one accepted host order. */
#define COFFEE3_ORDER_REGISTER_COUNT          32U

/** @brief Store one immutable accepted order snapshot. */
typedef struct {
	uint16_t ausRegister[COFFEE3_ORDER_REGISTER_COUNT]; /*!< Immutable 32-register accepted order copy owned by workflow. */
} Coffee3Order_t;

/** @brief Define high-level workflow lifecycle. */
typedef enum {
	COFFEE3_WORKFLOW_IDLE = 0,
	COFFEE3_WORKFLOW_RUNNING = 1,
	COFFEE3_WORKFLOW_COMPLETED = 2,
	COFFEE3_WORKFLOW_FAILED = 3,
	COFFEE3_WORKFLOW_CANCELING = 4
} Coffee3WorkflowState_e;

/** @brief Host-visible whole-machine state. */
typedef enum {
	COFFEE3_MACHINE_DEFAULT = 0,
	COFFEE3_MACHINE_IDLE = 1,
	COFFEE3_MACHINE_INITIALIZING = 2,
	COFFEE3_MACHINE_BUSY = 3,
	COFFEE3_MACHINE_ALARM = 4
} Coffee3MachineState_e;

/** @brief Maintenance operations serialized by the Workflow owner. */
typedef enum {
	COFFEE3_MAINTENANCE_NONE = 0,
	COFFEE3_MAINTENANCE_SYRUP_CLEAN = 1,
	COFFEE3_MAINTENANCE_COFFEE_CLEAN = 2,
	COFFEE3_MAINTENANCE_FRUIT_DISPENSE = 3,
	COFFEE3_MAINTENANCE_FRUIT_CLEAN = 4
} Coffee3MaintenanceType_e;

/** @brief Compact maintenance states projected to host registers. */
typedef enum {
	COFFEE3_MAINTENANCE_IDLE = 0,
	COFFEE3_MAINTENANCE_RUNNING = 1,
	COFFEE3_MAINTENANCE_COMPLETED = 2,
	COFFEE3_MAINTENANCE_FAILED = 3,
	COFFEE3_MAINTENANCE_ALARM = 4
} Coffee3MaintenanceState_e;

/** @brief Store workflow progress for Server monitoring. */
typedef struct {
	uint32_t ulCompletedOrderCount; /*!< Workflow increments after a successful order path. */
	uint32_t ulFailedOrderCount; /*!< Workflow increments on failed order processing. */
	uint32_t ulOrderEpoch; /*!< Cancellation generation; zero denotes non-order work. */
	uint16_t usCurrentOrderId; /*!< Current/last order projected to host status and logs. */
	uint16_t usCurrentStep; /*!< Last published workflow step; not a device address. */
	int32_t lLastError; /*!< Business-level failure, distinct from device terminal result. */
	Coffee3WorkflowState_e xState; /*!< Workflow lifecycle; not the whole-machine state enum. */
	Coffee3MachineState_e xMachineState; /*!< Host-facing whole-machine lifecycle and admission state. */
	uint8_t ucCancelRequested; /*!< Cooperative cancellation request inspected at wait points. */
	uint8_t ucOrderAdmissionOpen; /*!< Workflow admission permission, not network readiness. */
	uint8_t ucHotWaterState; /*!< Host-visible state of the serviced hot-water operation. */
	uint8_t ucCoffeeCleanState; /*!< Host-visible coffee cleaning progress. */
	uint8_t aucFruitState[2]; /*!< Host-visible maintenance state for fruit channels A and B. */
	uint16_t ausOutputState[2]; /*!< Retained outlet transaction states, separate from production. */
	uint16_t ausOutputOrderId[2]; /*!< Order association retained until each outlet is released. */
	uint16_t usActiveOutput; /*!< Selected one-based product output resource. */
	uint16_t usAction; /*!< Product action; owner translates it to a device-native request. */
	uint8_t ucDeviceId; /*!< Logical device id resolved through the immutable binding table. */
	uint8_t ucCommandSent; /*!< Queue accepted the step; physical action may not have started. */
	uint8_t ucDeviceDone; /*!< Matching owner command completed; sensor check is separate. */
	uint8_t ucPhysicalVerified; /*!< Workflow verified the required physical postcondition. */
	uint8_t ucPositionUncertain; /*!< Position action may have changed hardware before failure. */
	uint8_t ucRecoveryRequired; /*!< Admission lock requiring the product recovery procedure. */
	int32_t lSafetyResult; /*!< Safety-stop result kept separate from the original order error. */
	uint8_t ucStorageReserved; /*!< One-based Coffee3 storage reservation; zero means none. */
	uint8_t ucContentComplete; /*!< Coffee3 content milestone before offline final placement. */
} Coffee3WorkflowStatus_t;

extern Coffee3WorkflowStatus_t g_xCoffee3WorkflowStatus;

/** @brief Reserve manual access until the submitted command terminates. */
BaseType_t xCoffee3WorkflowAcquireManual(void);
/** @brief Release one successful manual reservation in task context. */
void vCoffee3WorkflowReleaseManual(void);
/** @brief Reserve OTA only when initialized, idle and outputs are verified off. */
BaseType_t xCoffee3WorkflowAcquireOta(void);
/** @brief Release the OTA reservation only when its startup failed. */
void vCoffee3WorkflowReleaseOta(void);
/** @brief Queue a pickup ACK only for an already placed outlet transaction. */
void vCoffee3WorkflowConfirmPickup(uint16_t usOutput);
/** @brief Reserve one storage-to-outlet transfer for the Workflow owner. */
BaseType_t xCoffee3WorkflowSubmitStoragePickup(uint16_t usStorage);

/**
  * @brief Create the bounded static order queue.
  * @retval pdPASS Queue creation succeeded.
  * @retval pdFAIL Queue creation failed.
  */
BaseType_t xCoffee3WorkflowInitialize(void);

/** @brief Return nonzero only after hardware/software/residual-cup initialization succeeds. */
uint8_t ucCoffee3WorkflowInitializationComplete(void);

/**
  * @brief Submit an accepted and verified order snapshot.
  * @param[in] pxOrder Immutable order copied into the workflow queue.
  * @retval pdPASS The order was accepted.
  * @retval pdFAIL Workflow is busy or the queue is full.
  */
BaseType_t xCoffee3WorkflowSubmitOrder(const Coffee3Order_t *pxOrder);

/**
  * @brief Submit a maintenance ice-dispense request using scale feedback.
  * @param[in] usTargetWeight Target weight in 0.1 g from register 0x0071.
  * @retval pdPASS The maintenance request was accepted.
  * @retval pdFAIL Workflow is busy, pending, or the target is zero.
  */
BaseType_t xCoffee3WorkflowSubmitManualIce(uint16_t usTargetWeight);

/** @brief Submit one non-order maintenance operation to Workflow. */
BaseType_t xCoffee3WorkflowSubmitMaintenance(
	Coffee3MaintenanceType_e xType, uint16_t usParameter0,
	uint16_t usParameter1);

/** @brief Start or stop the independent hot-water state machine. */
BaseType_t xCoffee3WorkflowSetHotWater(uint8_t ucStart,
	uint16_t usHeatMinutes);

/** @brief Acknowledge an initialization or maintenance alarm. */
void vCoffee3WorkflowAcknowledgeAlarm(void);

/** @brief Request cooperative cancellation of the active workflow. */
void vCoffee3WorkflowRequestCancel(void);

/**
  * @brief Run deterministic device steps and event-driven error handling.
  * @param[in] pvArgument Unused.
  */
void vCoffee3WorkflowTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_WORKFLOW_H */
