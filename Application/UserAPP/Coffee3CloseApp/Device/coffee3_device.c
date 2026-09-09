/**
  * @file      coffee3_device.c
  * @brief     Implement Coffee3 device binding, queue routing, and events.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee3_device.h"

#include <stddef.h>
#include <string.h>

#include "coffee3_app_config.h"
#include "coffee3_log.h"
#include "coffee3_workflow.h"
#include "task.h"

/** @brief Robot route plus addressable RTU route slots through Bus5. */
#define COFFEE3_ROUTE_COUNT                  6U
#define COFFEE3_TERMINAL_HISTORY_COUNT       \
	(COFFEE3_COMMAND_QUEUE_LENGTH + 2U)

#include "coffee3_device_bindings.h"

/** @brief Bounded completion history; match both sequence and generation. */
typedef struct {
	uint32_t ulCommandId;
	uint32_t ulOrderEpoch;
	int32_t lResult;
	uint16_t usAction;
	uint8_t ucTimedOut;
	uint8_t ucValid;
} Coffee3TerminalSnapshot_t;

/** @brief Public device status table. */
COFFEE3_CCM_DATA
Coffee3DeviceStatus_t
	g_axCoffee3DeviceStatus[COFFEE3_DEVICE_COUNT];

/** @brief Independent static EventGroup storage for every real device. */
COFFEE3_CCM_DATA
static StaticEventGroup_t
	s_axDeviceEventStorage[COFFEE3_DEVICE_COUNT];
/** @brief Independent EventGroup handles indexed by device ID. */
COFFEE3_CCM_DATA
static EventGroupHandle_t
	s_axDeviceEvents[COFFEE3_DEVICE_COUNT];
/** @brief Task-owned queues indexed by immutable route ID. */
COFFEE3_CCM_DATA
static QueueHandle_t s_axRouteQueues[COFFEE3_ROUTE_COUNT];
/** @brief Monotonic command identifier assigned at submission. */
COFFEE3_CCM_DATA
static uint32_t s_ulNextCommandId;
/** @brief Latest workflow epoch requested to stop cooperatively. */
COFFEE3_CCM_DATA
static volatile uint32_t s_ulCanceledOrderEpoch;
/** @brief Nonzero after device event initialization succeeds. */
COFFEE3_CCM_DATA
static uint8_t s_ucInitialized;
COFFEE3_CCM_DATA
static Coffee3TerminalSnapshot_t
	s_aaxTerminalHistory[COFFEE3_DEVICE_COUNT]
	[COFFEE3_TERMINAL_HISTORY_COUNT];
COFFEE3_CCM_DATA
static uint8_t s_aucTerminalHistoryHead[COFFEE3_DEVICE_COUNT];

static BaseType_t prvSubmit(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks, uint8_t ucUrgent);

static EventBits_t prvTerminalBits(int32_t lResult,
	uint8_t ucTimedOut, uint16_t usAction)
{
	if ((lResult == COFFEE3_COMMAND_RESULT_CANCELED) ||
		(lResult == COFFEE3_COMMAND_RESULT_SUPERSEDED)) {
		return COFFEE3_DEVICE_EVENT_CANCELED;
	}
	if ((lResult == 0) &&
		(usAction != (uint16_t)COFFEE3_ACTION_CANCEL)) {
		return COFFEE3_DEVICE_EVENT_COMMAND_DONE;
	}
	if ((lResult == 0) &&
		(usAction == (uint16_t)COFFEE3_ACTION_CANCEL)) {
		return COFFEE3_DEVICE_EVENT_CANCELED;
	}
	if (ucTimedOut != 0U) {
		return COFFEE3_DEVICE_EVENT_COMMAND_FAILED |
			COFFEE3_DEVICE_EVENT_TIMEOUT;
	}
	return COFFEE3_DEVICE_EVENT_COMMAND_FAILED;
}

/*-----------------------------------------------------------*/
int32_t lCoffee3DeviceGetTerminalResult(Coffee3DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, uint8_t *pucValid)
{
	uint8_t ucIndex;
	int32_t lResult;

	if (pucValid != NULL) {
		*pucValid = 0U;
	}
	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT)) {
		return 0;
	}
	/* Search committed history under the same lock as owner publication.
	 * Missing records return zero with validity clear, not confirmed success. */
	lResult = 0;
	taskENTER_CRITICAL();
	for (ucIndex = 0U; ucIndex < COFFEE3_TERMINAL_HISTORY_COUNT;
		ucIndex++) {
		if ((s_aaxTerminalHistory[xDeviceId][ucIndex].ucValid != 0U) &&
			(s_aaxTerminalHistory[xDeviceId][ucIndex].ulOrderEpoch ==
				ulOrderEpoch) &&
			(s_aaxTerminalHistory[xDeviceId][ucIndex].ulCommandId ==
				ulCommandId)) {
			if (pucValid != NULL) {
				*pucValid = 1U;
			}
			lResult = s_aaxTerminalHistory[xDeviceId][ucIndex].lResult;
			break;
		}
	}
	taskEXIT_CRITICAL();
	return lResult;
}


/*-----------------------------------------------------------*/
BaseType_t xCoffee3DeviceInitialize(void)
{
	uint8_t ucIndex;

	if (s_ucInitialized != 0U) {
		return pdPASS;
	}
	memset(g_axCoffee3DeviceStatus, 0,sizeof(g_axCoffee3DeviceStatus));
	memset(s_axDeviceEvents, 0, sizeof(s_axDeviceEvents));
	memset(s_axRouteQueues, 0, sizeof(s_axRouteQueues));
	memset(s_aaxTerminalHistory, 0, sizeof(s_aaxTerminalHistory));
	memset(s_aucTerminalHistoryHead, 0,
		sizeof(s_aucTerminalHistoryHead));
	for (ucIndex = 1U; ucIndex < COFFEE3_DEVICE_COUNT; ucIndex++) {
		s_axDeviceEvents[ucIndex] = xEventGroupCreateStatic(
			&s_axDeviceEventStorage[ucIndex]);
		if (s_axDeviceEvents[ucIndex] == NULL) {
			return pdFAIL;
		}
	}
	s_ulNextCommandId = 0U;
	s_ulCanceledOrderEpoch = 0U;
	s_ucInitialized = 1U;
	return pdPASS;
}

/*-----------------------------------------------------------*/
void vCoffee3DeviceRegisterRoute(uint8_t ucRouteId, QueueHandle_t xQueue)
{
	if ((ucRouteId >= COFFEE3_ROUTE_COUNT) || (xQueue == NULL)) {
		return;
	}
	s_axRouteQueues[ucRouteId] = xQueue;
}

/*-----------------------------------------------------------*/
const Coffee3DeviceBinding_t *pxCoffee3DeviceGetBinding(
	Coffee3DeviceId_e xDeviceId)
{
	uint8_t ucIndex;

	for (ucIndex = 0U;
		ucIndex < (uint8_t)(sizeof(s_axBindings) /
			sizeof(s_axBindings[0]));
		ucIndex++) {
		if (s_axBindings[ucIndex].xDeviceId == xDeviceId) {
			return &s_axBindings[ucIndex];
		}
	}
	return NULL;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3CommandSubmit(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks)
{
	return prvSubmit(pxCommand, xWaitTicks, 0U);
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3CommandSubmitUrgent(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks)
{
	return prvSubmit(pxCommand, xWaitTicks, 1U);
}

/*-----------------------------------------------------------*/
void vCoffee3OrderCancelRequest(uint32_t ulOrderEpoch)
{
	if (ulOrderEpoch == 0U) {
		return;
	}
	taskENTER_CRITICAL();
	s_ulCanceledOrderEpoch = ulOrderEpoch;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
uint8_t ucCoffee3CommandIsCanceled(const Coffee3Command_t *pxCommand)
{
	if ((pxCommand != NULL) &&
		((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_SAFETY_STOP) != 0U) &&
		(((pxCommand->usAction == COFFEE3_ACTION_CANCEL) &&
		  ((pxCommand->ucDeviceId == COFFEE3_DEVICE_ROBOT) ||
		   (pxCommand->ucDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE))) ||
		 ((pxCommand->ucDeviceId == COFFEE3_DEVICE_ICE_MACHINE) &&
		  (pxCommand->usAction == COFFEE3_ACTION_ICE_SET_VALVE) &&
		  (pxCommand->ausParameter[0] == 0U)) ||
		 ((pxCommand->ucDeviceId == COFFEE3_DEVICE_IO_OUTPUT) &&
		  (pxCommand->usAction == COFFEE3_ACTION_IO_WRITE) &&
		  (pxCommand->ausParameter[1] == 0U)))) {
		return 0U;
	}
	if ((pxCommand == NULL) ||
		(pxCommand->ucSource !=
			(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW) ||
		(pxCommand->ulOrderEpoch == 0U)) {
		return 0U;
	}
	return (pxCommand->ulOrderEpoch == s_ulCanceledOrderEpoch) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static BaseType_t prvSubmit(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks, uint8_t ucUrgent)
{
	const Coffee3DeviceBinding_t *pxBinding;
	QueueHandle_t xQueue;
	BaseType_t xResult;

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId == (uint8_t)COFFEE3_DEVICE_NONE) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE3_DEVICE_COUNT)) {
		return pdFAIL;
	}
	pxBinding = pxCoffee3DeviceGetBinding(
		(Coffee3DeviceId_e)pxCommand->ucDeviceId);
	if ((pxBinding == NULL) ||
		(pxBinding->ucRouteId >= COFFEE3_ROUTE_COUNT)) {
		return pdFAIL;
	}
	xQueue = s_axRouteQueues[pxBinding->ucRouteId];
	if (xQueue == NULL) {
		return pdFAIL;
	}
	/* Allocate a nonzero identity before copying the message into its queue. */
	if (pxCommand->ulCommandId == 0U) {
		taskENTER_CRITICAL();
		s_ulNextCommandId++;
		if (s_ulNextCommandId == 0U) {
			s_ulNextCommandId = 1U;
		}
		pxCommand->ulCommandId = s_ulNextCommandId;
		taskEXIT_CRITICAL();
	}
	if ((pxCommand->ucSource == COFFEE3_COMMAND_SOURCE_SERVER) &&
		((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_DEBUG) == 0U)) {
		if (xCoffee3WorkflowAcquireManual() != pdPASS) {
			return pdFAIL;
		}
		pxCommand->ucFlags |= COFFEE3_COMMAND_FLAG_MANUAL_RESERVED;
		/* Scheduling uses command origin, never the log order identifier.
		 * Read-only refresh stays normal priority on the same route. */
		ucUrgent = (pxCommand->usAction != COFFEE3_ACTION_REFRESH) ?
			1U : 0U;
	}
	/* Queue insertion copies all fields; it does not preempt an active IO.
	 * A failed insertion must return any acquired manual reservation. */
	xResult = (ucUrgent != 0U) ?
		xQueueSendToFront(xQueue, pxCommand, xWaitTicks) :
		xQueueSend(xQueue, pxCommand, xWaitTicks);
	if ((xResult != pdPASS) &&
		((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_MANUAL_RESERVED) != 0U)) {
		vCoffee3WorkflowReleaseManual();
		pxCommand->ucFlags &= (uint8_t)~COFFEE3_COMMAND_FLAG_MANUAL_RESERVED;
	}
	return xResult;
}

/*-----------------------------------------------------------*/
void vCoffee3DeviceCommandStarted(const Coffee3Command_t *pxCommand)
{
	Coffee3DeviceStatus_t *pxStatus;
	EventGroupHandle_t xEvents;
	uint8_t ucDeviceId;

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId == (uint8_t)COFFEE3_DEVICE_NONE) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE3_DEVICE_COUNT)) {
		return;
	}
	ucDeviceId = pxCommand->ucDeviceId;
	pxStatus = &g_axCoffee3DeviceStatus[ucDeviceId];
	xEvents = s_axDeviceEvents[ucDeviceId];
	(void)xEventGroupClearBits(xEvents,
		COFFEE3_DEVICE_EVENT_TERMINAL |
		COFFEE3_DEVICE_EVENT_DATA_UPDATED |
		COFFEE3_DEVICE_EVENT_RECOVERING);
	taskENTER_CRITICAL();
	pxStatus->ulLastCommandId = pxCommand->ulCommandId;
	pxStatus->ulLastOrderEpoch = pxCommand->ulOrderEpoch;
	pxStatus->usLastAction = pxCommand->usAction;
	pxStatus->ucBusy = 1U;
	pxStatus->ucRecovering = 0U;
	if (ucDeviceId == (uint8_t)COFFEE3_DEVICE_ROBOT) {
		pxStatus->ucRobotPhase = (uint8_t)COFFEE3_ROBOT_PHASE_IDLE;
		pxStatus->ucRobotAccepted = 0U;
	}
	pxStatus->ulCommandCount++;
	taskEXIT_CRITICAL();
	(void)xEventGroupSetBits(xEvents, COFFEE3_DEVICE_EVENT_BUSY);
	if (pxCommand->ucSource ==
		(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
			"MANUAL_COMMAND_RUNNING", 0,
			"action", (int32_t)pxCommand->usAction);
	}
}

/*-----------------------------------------------------------*/
void vCoffee3DeviceCommandCompleted(const Coffee3Command_t *pxCommand,
	int32_t lResult, uint8_t ucTimedOut)
{
	Coffee3DeviceStatus_t *pxStatus;
	EventGroupHandle_t xEvents;
	EventBits_t xSetBits;
	uint8_t ucDeviceId;
	uint8_t ucHistoryIndex;

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId == (uint8_t)COFFEE3_DEVICE_NONE) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE3_DEVICE_COUNT)) {
		return;
	}
	ucDeviceId = pxCommand->ucDeviceId;
	pxStatus = &g_axCoffee3DeviceStatus[ucDeviceId];
	xEvents = s_axDeviceEvents[ucDeviceId];
	xSetBits = COFFEE3_DEVICE_EVENT_DATA_UPDATED;
	if ((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_MANUAL_RESERVED) != 0U) {
		vCoffee3WorkflowReleaseManual();
	}
	if (lResult == 0) {
		(void)xEventGroupClearBits(xEvents,
			COFFEE3_DEVICE_EVENT_COMM_FAULT |
			COFFEE3_DEVICE_EVENT_DEVICE_FAULT);
		xSetBits |= COFFEE3_DEVICE_EVENT_ONLINE;
		if ((ucDeviceId != (uint8_t)COFFEE3_DEVICE_ROBOT) ||
			(g_axCoffee3DeviceStatus[ucDeviceId].ucReady != 0U)) {
			xSetBits |= COFFEE3_DEVICE_EVENT_READY;
		} else {
			(void)xEventGroupClearBits(xEvents,
				COFFEE3_DEVICE_EVENT_READY);
		}
		if (pxCommand->usAction == (uint16_t)COFFEE3_ACTION_CANCEL) {
			xSetBits |= COFFEE3_DEVICE_EVENT_CANCELED;
		} else {
			xSetBits |= COFFEE3_DEVICE_EVENT_COMMAND_DONE;
		}
	} else if ((lResult == COFFEE3_COMMAND_RESULT_CANCELED) ||
		(lResult == COFFEE3_COMMAND_RESULT_SUPERSEDED)) {
		xSetBits |= COFFEE3_DEVICE_EVENT_CANCELED;
	} else if (ucTimedOut != 0U) {
		xSetBits |= COFFEE3_DEVICE_EVENT_COMMAND_FAILED |
			COFFEE3_DEVICE_EVENT_TIMEOUT |
			COFFEE3_DEVICE_EVENT_COMM_FAULT;
	} else {
		xSetBits |= COFFEE3_DEVICE_EVENT_COMMAND_FAILED;
		if ((lResult == -5) || (lResult == -2)) {
			xSetBits |= COFFEE3_DEVICE_EVENT_COMM_FAULT;
		} else if ((lResult == -6) || (lResult == -7)) {
			xSetBits |= COFFEE3_DEVICE_EVENT_DEVICE_FAULT |
				COFFEE3_DEVICE_EVENT_ONLINE;
		} else if (lResult == -8) {
			xSetBits |= COFFEE3_DEVICE_EVENT_DEVICE_FAULT;
		}
	}

	taskENTER_CRITICAL();
	/* Commit identity and result before waking consumers. The bounded ring
	 * retains completions even if a following refresh replaces live status. */
	ucHistoryIndex = s_aucTerminalHistoryHead[ucDeviceId];
	s_aaxTerminalHistory[ucDeviceId][ucHistoryIndex].ulCommandId =
		pxCommand->ulCommandId;
	s_aaxTerminalHistory[ucDeviceId][ucHistoryIndex].ulOrderEpoch =
		pxCommand->ulOrderEpoch;
	s_aaxTerminalHistory[ucDeviceId][ucHistoryIndex].lResult = lResult;
	s_aaxTerminalHistory[ucDeviceId][ucHistoryIndex].usAction =
		pxCommand->usAction;
	s_aaxTerminalHistory[ucDeviceId][ucHistoryIndex].ucTimedOut =
		ucTimedOut;
	s_aaxTerminalHistory[ucDeviceId][ucHistoryIndex].ucValid = 1U;
	ucHistoryIndex++;
	if (ucHistoryIndex >= COFFEE3_TERMINAL_HISTORY_COUNT) {
		ucHistoryIndex = 0U;
	}
	s_aucTerminalHistoryHead[ucDeviceId] = ucHistoryIndex;
	pxStatus->ulPreviousTerminalCommandId =
		pxStatus->ulTerminalCommandId;
	pxStatus->ulPreviousTerminalOrderEpoch =
		pxStatus->ulTerminalOrderEpoch;
	pxStatus->lPreviousTerminalResult = pxStatus->lTerminalResult;
	pxStatus->usPreviousTerminalAction = pxStatus->usTerminalAction;
	pxStatus->ucPreviousTerminalTimedOut = pxStatus->ucTerminalTimedOut;
	pxStatus->ucPreviousTerminalValid = pxStatus->ucTerminalValid;
	pxStatus->ulTerminalCommandId = pxCommand->ulCommandId;
	pxStatus->ulTerminalOrderEpoch = pxCommand->ulOrderEpoch;
	pxStatus->lTerminalResult = lResult;
	pxStatus->usTerminalAction = pxCommand->usAction;
	pxStatus->ucTerminalTimedOut = ucTimedOut;
	pxStatus->ucTerminalValid = 1U;
	pxStatus->lLastResult = lResult;
	pxStatus->ucBusy = 0U;
	pxStatus->ucRecovering = 0U;
	if (ucDeviceId == (uint8_t)COFFEE3_DEVICE_ROBOT) {
		pxStatus->ucRobotPhase = (uint8_t)COFFEE3_ROBOT_PHASE_IDLE;
		pxStatus->ucRobotAccepted = 0U;
	}
	if ((lResult == 0) || (lResult == -6) || (lResult == -7)) {
		pxStatus->ucOnline = 1U;
	}
	if (lResult == 0) {
		pxStatus->ulLastSuccessTick = (uint32_t)xTaskGetTickCount();
	} else {
		pxStatus->ulErrorCount++;
	}
	taskEXIT_CRITICAL();
	(void)xEventGroupClearBits(xEvents, COFFEE3_DEVICE_EVENT_BUSY);
	(void)xEventGroupClearBits(xEvents, COFFEE3_DEVICE_EVENT_RECOVERING);
	(void)xEventGroupSetBits(xEvents, xSetBits);
	if (pxCommand->ucSource ==
		(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) {
		if ((lResult == COFFEE3_COMMAND_RESULT_CANCELED) ||
			(lResult == COFFEE3_COMMAND_RESULT_SUPERSEDED)) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"MANUAL_COMMAND_CANCELED", lResult,
				"action", (int32_t)pxCommand->usAction);
		} else if (ucTimedOut != 0U) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"MANUAL_COMMAND_TIMEOUT", lResult,
				"action", (int32_t)pxCommand->usAction);
		} else if (lResult == 0) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"MANUAL_COMMAND_COMPLETED", lResult,
				"action", (int32_t)pxCommand->usAction);
		} else {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"MANUAL_COMMAND_FAILED", lResult,
				"action", (int32_t)pxCommand->usAction);
		}
	}
}

/*-----------------------------------------------------------*/
void vCoffee3DeviceSetRobotPhase(Coffee3RobotPhase_e xPhase)
{
	if (xPhase > COFFEE3_ROBOT_PHASE_RECOVERING) {
		return;
	}
	taskENTER_CRITICAL();
	g_axCoffee3DeviceStatus[COFFEE3_DEVICE_ROBOT].ucRobotPhase =
		(uint8_t)xPhase;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
void vCoffee3DeviceSetRobotAccepted(uint8_t ucAccepted)
{
	taskENTER_CRITICAL();
	g_axCoffee3DeviceStatus[COFFEE3_DEVICE_ROBOT].ucRobotAccepted =
		(ucAccepted != 0U) ? 1U : 0U;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
void vCoffee3DeviceSetReady(Coffee3DeviceId_e xDeviceId,
	uint8_t ucReady)
{
	EventGroupHandle_t xEvents;

	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT)) {
		return;
	}
	xEvents = s_axDeviceEvents[xDeviceId];
	ucReady = (ucReady != 0U) ? 1U : 0U;
	taskENTER_CRITICAL();
	g_axCoffee3DeviceStatus[xDeviceId].ucReady = ucReady;
	taskEXIT_CRITICAL();
	if (ucReady != 0U) {
		(void)xEventGroupSetBits(xEvents, COFFEE3_DEVICE_EVENT_READY);
	} else {
		(void)xEventGroupClearBits(xEvents, COFFEE3_DEVICE_EVENT_READY);
	}
}

/*-----------------------------------------------------------*/
void vCoffee3DeviceSetRecovering(Coffee3DeviceId_e xDeviceId,
	uint8_t ucRecovering)
{
	EventGroupHandle_t xEvents;

	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT)) {
		return;
	}
	xEvents = s_axDeviceEvents[xDeviceId];
	ucRecovering = (ucRecovering != 0U) ? 1U : 0U;
	taskENTER_CRITICAL();
	g_axCoffee3DeviceStatus[xDeviceId].ucRecovering = ucRecovering;
	taskEXIT_CRITICAL();
	if (ucRecovering != 0U) {
		(void)xEventGroupSetBits(xEvents, COFFEE3_DEVICE_EVENT_RECOVERING);
	} else {
		(void)xEventGroupClearBits(xEvents,
			COFFEE3_DEVICE_EVENT_RECOVERING);
	}
}

/*-----------------------------------------------------------*/
void vCoffee3DeviceSetOnline(Coffee3DeviceId_e xDeviceId,
	uint8_t ucOnline)
{
	EventGroupHandle_t xEvents;

	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT)) {
		return;
	}
	xEvents = s_axDeviceEvents[xDeviceId];
	ucOnline = (ucOnline != 0U) ? 1U : 0U;
	taskENTER_CRITICAL();
	g_axCoffee3DeviceStatus[xDeviceId].ucOnline = ucOnline;
	taskEXIT_CRITICAL();
	if (ucOnline != 0U) {
		(void)xEventGroupClearBits(xEvents,
			COFFEE3_DEVICE_EVENT_COMM_FAULT);
		(void)xEventGroupSetBits(xEvents, COFFEE3_DEVICE_EVENT_ONLINE);
		if ((xDeviceId != COFFEE3_DEVICE_ROBOT) ||
			(g_axCoffee3DeviceStatus[xDeviceId].ucReady != 0U)) {
			(void)xEventGroupSetBits(xEvents,
				COFFEE3_DEVICE_EVENT_READY);
		} else {
			(void)xEventGroupClearBits(xEvents,
				COFFEE3_DEVICE_EVENT_READY);
		}
	} else {
		(void)xEventGroupClearBits(xEvents,
			COFFEE3_DEVICE_EVENT_ONLINE |
			COFFEE3_DEVICE_EVENT_READY);
		(void)xEventGroupSetBits(xEvents,
			COFFEE3_DEVICE_EVENT_COMM_FAULT);
		vCoffee3DeviceSetReady(xDeviceId, 0U);
	}
}

/*-----------------------------------------------------------*/
EventBits_t xCoffee3DeviceGetEvents(Coffee3DeviceId_e xDeviceId)
{
	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT) ||
		(s_axDeviceEvents[xDeviceId] == NULL)) {
		return 0U;
	}
	return xEventGroupGetBits(s_axDeviceEvents[xDeviceId]);
}

/*-----------------------------------------------------------*/
EventBits_t xCoffee3DeviceWaitCommand(Coffee3DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, TickType_t xWaitTicks)
{
	EventBits_t xBits;
	Coffee3DeviceStatus_t xStatus;
	TickType_t xWaitStart;
	TickType_t xRemaining;
	EventBits_t xTerminal;
	uint8_t ucIndex;

	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT) ||
		(s_axDeviceEvents[xDeviceId] == NULL) ||
		(ulCommandId == 0U)) {
		return 0U;
	}
	/* Event bits are only wakeups. Search history and retained snapshots
	 * for the exact (epoch, command id), within this call's tick budget. */
	xWaitStart = xTaskGetTickCount();
	for (;;) {
		taskENTER_CRITICAL();
		xStatus = g_axCoffee3DeviceStatus[xDeviceId];
		xTerminal = 0U;
		for (ucIndex = 0U; ucIndex < COFFEE3_TERMINAL_HISTORY_COUNT;
			ucIndex++) {
			if ((s_aaxTerminalHistory[xDeviceId][ucIndex].ucValid != 0U) &&
				(s_aaxTerminalHistory[xDeviceId][ucIndex].ulOrderEpoch ==
					ulOrderEpoch) &&
				(s_aaxTerminalHistory[xDeviceId][ucIndex].ulCommandId ==
					ulCommandId)) {
				xTerminal = prvTerminalBits(
					s_aaxTerminalHistory[xDeviceId][ucIndex].lResult,
					s_aaxTerminalHistory[xDeviceId][ucIndex].ucTimedOut,
					s_aaxTerminalHistory[xDeviceId][ucIndex].usAction);
				break;
			}
		}
		taskEXIT_CRITICAL();
		if (xTerminal != 0U) {
			return xTerminal;
		}
		if ((xStatus.ucTerminalValid != 0U) &&
			(xStatus.ulTerminalOrderEpoch == ulOrderEpoch) &&
			(xStatus.ulTerminalCommandId == ulCommandId)) {
			return prvTerminalBits(xStatus.lTerminalResult,
				xStatus.ucTerminalTimedOut,
				xStatus.usTerminalAction);
		}
		if ((xStatus.ucPreviousTerminalValid != 0U) &&
			(xStatus.ulPreviousTerminalOrderEpoch == ulOrderEpoch) &&
			(xStatus.ulPreviousTerminalCommandId == ulCommandId)) {
			return prvTerminalBits(xStatus.lPreviousTerminalResult,
				xStatus.ucPreviousTerminalTimedOut,
				xStatus.usPreviousTerminalAction);
		}
		if (xWaitTicks == 0U) {
			return 0U;
		}
		xRemaining = xWaitTicks - (xTaskGetTickCount() - xWaitStart);
		if ((xRemaining == 0U) ||
			((xTaskGetTickCount() - xWaitStart) >= xWaitTicks)) {
			return 0U;
		}
		xBits = xEventGroupWaitBits(s_axDeviceEvents[xDeviceId],
			COFFEE3_DEVICE_EVENT_TERMINAL, pdFALSE, pdFALSE,
			(xRemaining > pdMS_TO_TICKS(100U)) ?
				pdMS_TO_TICKS(100U) : xRemaining);
		if ((xBits & COFFEE3_DEVICE_EVENT_TERMINAL) != 0U) {
			/* Event bits are wakeups, not transaction identity. A latched
			 * unrelated result must not spin and starve the bus owner. */
			vTaskDelay(1U);
		}
	}
}
