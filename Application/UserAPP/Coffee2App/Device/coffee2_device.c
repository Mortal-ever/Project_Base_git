/**
  * @file      coffee2_device.c
  * @brief     Implement Coffee2 device binding, queue routing, and events.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee2_device.h"

#include <stddef.h>
#include <string.h>

#include "coffee2_app_config.h"
#include "coffee2_log.h"
#include "task.h"

/** @brief Robot route plus addressable RTU route slots through Bus5. */
#define COFFEE2_ROUTE_COUNT                  6U
#define COFFEE2_TERMINAL_HISTORY_COUNT       \
	(COFFEE2_COMMAND_QUEUE_LENGTH + 2U)

#if (COFFEE2_ROBOT_PROTOCOL_VARIANT == \
	COFFEE2_ROBOT_PROTOCOL_2)
#define COFFEE2_ROBOT_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_2
#elif (COFFEE2_ROBOT_PROTOCOL_VARIANT == \
	COFFEE2_ROBOT_PROTOCOL_3)
#define COFFEE2_ROBOT_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_3
#else
#define COFFEE2_ROBOT_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_1
#endif

typedef struct {
	uint32_t ulCommandId;
	uint32_t ulOrderEpoch;
	int32_t lResult;
	uint16_t usAction;
	uint8_t ucTimedOut;
	uint8_t ucValid;
} Coffee2TerminalSnapshot_t;

/** @brief Public device status table. */
COFFEE2_CCM_DATA
Coffee2DeviceStatus_t
	g_axCoffee2DeviceStatus[COFFEE2_DEVICE_COUNT];

/** @brief Independent static EventGroup storage for every real device. */
COFFEE2_CCM_DATA
static StaticEventGroup_t
	s_axDeviceEventStorage[COFFEE2_DEVICE_COUNT];
/** @brief Independent EventGroup handles indexed by device ID. */
COFFEE2_CCM_DATA
static EventGroupHandle_t
	s_axDeviceEvents[COFFEE2_DEVICE_COUNT];
/** @brief Task-owned queues indexed by immutable route ID. */
COFFEE2_CCM_DATA
static QueueHandle_t s_axRouteQueues[COFFEE2_ROUTE_COUNT];
/** @brief Monotonic command identifier assigned at submission. */
COFFEE2_CCM_DATA
static uint32_t s_ulNextCommandId;
/** @brief Latest workflow epoch requested to stop cooperatively. */
COFFEE2_CCM_DATA
static volatile uint32_t s_ulCanceledOrderEpoch;
/** @brief Nonzero after device event initialization succeeds. */
COFFEE2_CCM_DATA
static uint8_t s_ucInitialized;
COFFEE2_CCM_DATA
static Coffee2TerminalSnapshot_t
	s_aaxTerminalHistory[COFFEE2_DEVICE_COUNT]
	[COFFEE2_TERMINAL_HISTORY_COUNT];
COFFEE2_CCM_DATA
static uint8_t s_aucTerminalHistoryHead[COFFEE2_DEVICE_COUNT];

static BaseType_t prvSubmit(Coffee2Command_t *pxCommand,
	TickType_t xWaitTicks, uint8_t ucUrgent);

static EventBits_t prvTerminalBits(int32_t lResult,
	uint8_t ucTimedOut, uint16_t usAction)
{
	if ((lResult == COFFEE2_COMMAND_RESULT_CANCELED) ||
		(lResult == COFFEE2_COMMAND_RESULT_SUPERSEDED)) {
		return COFFEE2_DEVICE_EVENT_CANCELED;
	}
	if ((lResult == 0) &&
		(usAction != (uint16_t)COFFEE2_ACTION_CANCEL)) {
		return COFFEE2_DEVICE_EVENT_COMMAND_DONE;
	}
	if ((lResult == 0) &&
		(usAction == (uint16_t)COFFEE2_ACTION_CANCEL)) {
		return COFFEE2_DEVICE_EVENT_CANCELED;
	}
	if (ucTimedOut != 0U) {
		return COFFEE2_DEVICE_EVENT_COMMAND_FAILED |
			COFFEE2_DEVICE_EVENT_TIMEOUT;
	}
	return COFFEE2_DEVICE_EVENT_COMMAND_FAILED;
}

/*-----------------------------------------------------------*/
int32_t lCoffee2DeviceGetTerminalResult(Coffee2DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, uint8_t *pucValid)
{
	uint8_t ucIndex;
	int32_t lResult;

	if (pucValid != NULL) {
		*pucValid = 0U;
	}
	if ((xDeviceId <= COFFEE2_DEVICE_NONE) ||
		(xDeviceId >= COFFEE2_DEVICE_COUNT)) {
		return 0;
	}
	lResult = 0;
	taskENTER_CRITICAL();
	for (ucIndex = 0U; ucIndex < COFFEE2_TERMINAL_HISTORY_COUNT;
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

/** @brief Immutable product device-to-bus binding table. */
static const Coffee2DeviceBinding_t s_axBindings[] = {
	{ COFFEE2_DEVICE_ROBOT, 0U, COFFEE2_ROBOT_UNIT_ID, 0U,
		DEVICE_CATEGORY_ROBOT, DEVICE_ROLE_ROBOT_1,
		COFFEE2_ROBOT_DRIVER_ID, DEVICE_PROTOCOL_MODBUS_TCP },
	{ COFFEE2_DEVICE_COFFEE_MACHINE, 2U, 0U, 0U,
		DEVICE_CATEGORY_COFFEE_MACHINE,
		DEVICE_ROLE_COFFEE_MACHINE,
		DEVICE_DRIVER_COFFEE_DRCOFFEE_F200,
		DEVICE_PROTOCOL_COFFEE_F200_UART },
	{ COFFEE2_DEVICE_CUP_MACHINE, 3U, 1U, 0U,
		DEVICE_CATEGORY_CUP_MACHINE,
		DEVICE_ROLE_CUP_MACHINE, DEVICE_DRIVER_CUP_SHENGSHU_MODBUS,
		DEVICE_PROTOCOL_MODBUS_RTU },
	{ COFFEE2_DEVICE_SYRUP_MACHINE, 3U, 2U, 0U,
		DEVICE_CATEGORY_SYRUP_MACHINE,
		DEVICE_ROLE_SYRUP_MACHINE, DEVICE_DRIVER_SYRUP_CURRENT_MODBUS,
		DEVICE_PROTOCOL_MODBUS_RTU },
	{ COFFEE2_DEVICE_LID_MACHINE, 3U, 1U, 0U,
		DEVICE_CATEGORY_LID_MACHINE,
		DEVICE_ROLE_LID_MACHINE, DEVICE_DRIVER_LID_SHENGSHU_MODBUS,
		DEVICE_PROTOCOL_MODBUS_RTU },
	{ COFFEE2_DEVICE_ICE_MACHINE, 4U, 1U,
		COFFEE2_ICE_MIN_FRAME_INTERVAL_MS,
		DEVICE_CATEGORY_ICE_MACHINE, DEVICE_ROLE_ICE_MACHINE,
		DEVICE_DRIVER_ICE_CURRENT_MODBUS, DEVICE_PROTOCOL_MODBUS_RTU },
	{ COFFEE2_DEVICE_SCALE, 4U, 2U, 0U,
		DEVICE_CATEGORY_SCALE, DEVICE_ROLE_SCALE,
		DEVICE_DRIVER_SCALE_BSQ_DG_V2, DEVICE_PROTOCOL_MODBUS_RTU },
	{ COFFEE2_DEVICE_POWER_METER, 3U, 3U, 0U,
		DEVICE_CATEGORY_POWER_METER,
		DEVICE_ROLE_POWER_METER, DEVICE_DRIVER_POWER_METER_DDSU666,
		DEVICE_PROTOCOL_MODBUS_RTU },
	{ COFFEE2_DEVICE_IO_INPUT, 5U, 1U,
		COFFEE2_IO_MIN_FRAME_INTERVAL_MS,
		DEVICE_CATEGORY_IO, DEVICE_ROLE_IO_INPUT,
		DEVICE_DRIVER_IO_MODBUS_DIGITAL, DEVICE_PROTOCOL_MODBUS_RTU },
	{ COFFEE2_DEVICE_IO_OUTPUT, 5U, 2U,
		COFFEE2_IO_MIN_FRAME_INTERVAL_MS,
		DEVICE_CATEGORY_IO, DEVICE_ROLE_IO_OUTPUT,
		DEVICE_DRIVER_IO_MODBUS_DIGITAL, DEVICE_PROTOCOL_MODBUS_RTU }
};

/*-----------------------------------------------------------*/
BaseType_t xCoffee2DeviceInitialize(void)
{
	uint8_t ucIndex;

	if (s_ucInitialized != 0U) {
		return pdPASS;
	}
	memset(g_axCoffee2DeviceStatus, 0,sizeof(g_axCoffee2DeviceStatus));
	memset(s_axDeviceEvents, 0, sizeof(s_axDeviceEvents));
	memset(s_axRouteQueues, 0, sizeof(s_axRouteQueues));
	memset(s_aaxTerminalHistory, 0, sizeof(s_aaxTerminalHistory));
	memset(s_aucTerminalHistoryHead, 0,
		sizeof(s_aucTerminalHistoryHead));
	for (ucIndex = 1U; ucIndex < COFFEE2_DEVICE_COUNT; ucIndex++) {
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
void vCoffee2DeviceRegisterRoute(uint8_t ucRouteId, QueueHandle_t xQueue)
{
	if ((ucRouteId >= COFFEE2_ROUTE_COUNT) || (xQueue == NULL)) {
		return;
	}
	s_axRouteQueues[ucRouteId] = xQueue;
}

/*-----------------------------------------------------------*/
const Coffee2DeviceBinding_t *pxCoffee2DeviceGetBinding(
	Coffee2DeviceId_e xDeviceId)
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
BaseType_t xCoffee2CommandSubmit(Coffee2Command_t *pxCommand,
	TickType_t xWaitTicks)
{
	return prvSubmit(pxCommand, xWaitTicks, 0U);
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee2CommandSubmitUrgent(Coffee2Command_t *pxCommand,
	TickType_t xWaitTicks)
{
	return prvSubmit(pxCommand, xWaitTicks, 1U);
}

/*-----------------------------------------------------------*/
void vCoffee2OrderCancelRequest(uint32_t ulOrderEpoch)
{
	if (ulOrderEpoch == 0U) {
		return;
	}
	taskENTER_CRITICAL();
	s_ulCanceledOrderEpoch = ulOrderEpoch;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
uint8_t ucCoffee2CommandIsCanceled(const Coffee2Command_t *pxCommand)
{
	if ((pxCommand == NULL) ||
		(pxCommand->ucSource !=
			(uint8_t)COFFEE2_COMMAND_SOURCE_WORKFLOW) ||
		(pxCommand->ulOrderEpoch == 0U)) {
		return 0U;
	}
	return (pxCommand->ulOrderEpoch == s_ulCanceledOrderEpoch) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static BaseType_t prvSubmit(Coffee2Command_t *pxCommand,
	TickType_t xWaitTicks, uint8_t ucUrgent)
{
	const Coffee2DeviceBinding_t *pxBinding;
	QueueHandle_t xQueue;

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId == (uint8_t)COFFEE2_DEVICE_NONE) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE2_DEVICE_COUNT)) {
		return pdFAIL;
	}
	pxBinding = pxCoffee2DeviceGetBinding(
		(Coffee2DeviceId_e)pxCommand->ucDeviceId);
	if ((pxBinding == NULL) ||
		(pxBinding->ucRouteId >= COFFEE2_ROUTE_COUNT)) {
		return pdFAIL;
	}
	xQueue = s_axRouteQueues[pxBinding->ucRouteId];
	if (xQueue == NULL) {
		return pdFAIL;
	}
	if (pxCommand->ulCommandId == 0U) {
		taskENTER_CRITICAL();
		s_ulNextCommandId++;
		if (s_ulNextCommandId == 0U) {
			s_ulNextCommandId = 1U;
		}
		pxCommand->ulCommandId = s_ulNextCommandId;
		taskEXIT_CRITICAL();
	}
	if (ucUrgent != 0U) {
		return xQueueSendToFront(xQueue, pxCommand, xWaitTicks);
	}
	return xQueueSend(xQueue, pxCommand, xWaitTicks);
}

/*-----------------------------------------------------------*/
void vCoffee2DeviceCommandStarted(const Coffee2Command_t *pxCommand)
{
	Coffee2DeviceStatus_t *pxStatus;
	EventGroupHandle_t xEvents;
	uint8_t ucDeviceId;

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId == (uint8_t)COFFEE2_DEVICE_NONE) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE2_DEVICE_COUNT)) {
		return;
	}
	ucDeviceId = pxCommand->ucDeviceId;
	pxStatus = &g_axCoffee2DeviceStatus[ucDeviceId];
	xEvents = s_axDeviceEvents[ucDeviceId];
	(void)xEventGroupClearBits(xEvents,
		COFFEE2_DEVICE_EVENT_TERMINAL |
		COFFEE2_DEVICE_EVENT_DATA_UPDATED |
		COFFEE2_DEVICE_EVENT_RECOVERING);
	taskENTER_CRITICAL();
	pxStatus->ulLastCommandId = pxCommand->ulCommandId;
	pxStatus->ulLastOrderEpoch = pxCommand->ulOrderEpoch;
	pxStatus->usLastAction = pxCommand->usAction;
	pxStatus->ucBusy = 1U;
	pxStatus->ucRecovering = 0U;
	if (ucDeviceId == (uint8_t)COFFEE2_DEVICE_ROBOT) {
		pxStatus->ucRobotPhase = (uint8_t)COFFEE2_ROBOT_PHASE_IDLE;
		pxStatus->ucRobotAccepted = 0U;
	}
	pxStatus->ulCommandCount++;
	taskEXIT_CRITICAL();
	(void)xEventGroupSetBits(xEvents, COFFEE2_DEVICE_EVENT_BUSY);
	if (pxCommand->ucSource ==
		(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
			"MANUAL_COMMAND_RUNNING", 0,
			"action", (int32_t)pxCommand->usAction);
	}
}

/*-----------------------------------------------------------*/
void vCoffee2DeviceCommandCompleted(const Coffee2Command_t *pxCommand,
	int32_t lResult, uint8_t ucTimedOut)
{
	Coffee2DeviceStatus_t *pxStatus;
	EventGroupHandle_t xEvents;
	EventBits_t xSetBits;
	uint8_t ucDeviceId;
	uint8_t ucHistoryIndex;

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId == (uint8_t)COFFEE2_DEVICE_NONE) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE2_DEVICE_COUNT)) {
		return;
	}
	ucDeviceId = pxCommand->ucDeviceId;
	pxStatus = &g_axCoffee2DeviceStatus[ucDeviceId];
	xEvents = s_axDeviceEvents[ucDeviceId];
	xSetBits = COFFEE2_DEVICE_EVENT_DATA_UPDATED;
	if (lResult == 0) {
		(void)xEventGroupClearBits(xEvents,
			COFFEE2_DEVICE_EVENT_COMM_FAULT |
			COFFEE2_DEVICE_EVENT_DEVICE_FAULT);
		xSetBits |= COFFEE2_DEVICE_EVENT_ONLINE;
		if ((ucDeviceId != (uint8_t)COFFEE2_DEVICE_ROBOT) ||
			(g_axCoffee2DeviceStatus[ucDeviceId].ucReady != 0U)) {
			xSetBits |= COFFEE2_DEVICE_EVENT_READY;
		} else {
			(void)xEventGroupClearBits(xEvents,
				COFFEE2_DEVICE_EVENT_READY);
		}
		if (pxCommand->usAction == (uint16_t)COFFEE2_ACTION_CANCEL) {
			xSetBits |= COFFEE2_DEVICE_EVENT_CANCELED;
		} else {
			xSetBits |= COFFEE2_DEVICE_EVENT_COMMAND_DONE;
		}
	} else if ((lResult == COFFEE2_COMMAND_RESULT_CANCELED) ||
		(lResult == COFFEE2_COMMAND_RESULT_SUPERSEDED)) {
		xSetBits |= COFFEE2_DEVICE_EVENT_CANCELED;
	} else if (ucTimedOut != 0U) {
		xSetBits |= COFFEE2_DEVICE_EVENT_COMMAND_FAILED |
			COFFEE2_DEVICE_EVENT_TIMEOUT |
			COFFEE2_DEVICE_EVENT_COMM_FAULT;
	} else {
		xSetBits |= COFFEE2_DEVICE_EVENT_COMMAND_FAILED;
		if ((lResult == -5) || (lResult == -2)) {
			xSetBits |= COFFEE2_DEVICE_EVENT_COMM_FAULT;
		} else if ((lResult == -6) || (lResult == -7)) {
			xSetBits |= COFFEE2_DEVICE_EVENT_DEVICE_FAULT |
				COFFEE2_DEVICE_EVENT_ONLINE;
		} else if (lResult == -8) {
			xSetBits |= COFFEE2_DEVICE_EVENT_DEVICE_FAULT;
		}
	}

	taskENTER_CRITICAL();
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
	if (ucHistoryIndex >= COFFEE2_TERMINAL_HISTORY_COUNT) {
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
	if (ucDeviceId == (uint8_t)COFFEE2_DEVICE_ROBOT) {
		pxStatus->ucRobotPhase = (uint8_t)COFFEE2_ROBOT_PHASE_IDLE;
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
	(void)xEventGroupClearBits(xEvents, COFFEE2_DEVICE_EVENT_BUSY);
	(void)xEventGroupClearBits(xEvents, COFFEE2_DEVICE_EVENT_RECOVERING);
	(void)xEventGroupSetBits(xEvents, xSetBits);
	if (pxCommand->ucSource ==
		(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) {
		if ((lResult == COFFEE2_COMMAND_RESULT_CANCELED) ||
			(lResult == COFFEE2_COMMAND_RESULT_SUPERSEDED)) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"MANUAL_COMMAND_CANCELED", lResult,
				"action", (int32_t)pxCommand->usAction);
		} else if (ucTimedOut != 0U) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"MANUAL_COMMAND_TIMEOUT", lResult,
				"action", (int32_t)pxCommand->usAction);
		} else if (lResult == 0) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"MANUAL_COMMAND_COMPLETED", lResult,
				"action", (int32_t)pxCommand->usAction);
		} else {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"MANUAL_COMMAND_FAILED", lResult,
				"action", (int32_t)pxCommand->usAction);
		}
	}
}

/*-----------------------------------------------------------*/
void vCoffee2DeviceSetRobotPhase(Coffee2RobotPhase_e xPhase)
{
	if (xPhase > COFFEE2_ROBOT_PHASE_RECOVERING) {
		return;
	}
	taskENTER_CRITICAL();
	g_axCoffee2DeviceStatus[COFFEE2_DEVICE_ROBOT].ucRobotPhase =
		(uint8_t)xPhase;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
void vCoffee2DeviceSetRobotAccepted(uint8_t ucAccepted)
{
	taskENTER_CRITICAL();
	g_axCoffee2DeviceStatus[COFFEE2_DEVICE_ROBOT].ucRobotAccepted =
		(ucAccepted != 0U) ? 1U : 0U;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
void vCoffee2DeviceSetReady(Coffee2DeviceId_e xDeviceId,
	uint8_t ucReady)
{
	EventGroupHandle_t xEvents;

	if ((xDeviceId <= COFFEE2_DEVICE_NONE) ||
		(xDeviceId >= COFFEE2_DEVICE_COUNT)) {
		return;
	}
	xEvents = s_axDeviceEvents[xDeviceId];
	ucReady = (ucReady != 0U) ? 1U : 0U;
	taskENTER_CRITICAL();
	g_axCoffee2DeviceStatus[xDeviceId].ucReady = ucReady;
	taskEXIT_CRITICAL();
	if (ucReady != 0U) {
		(void)xEventGroupSetBits(xEvents, COFFEE2_DEVICE_EVENT_READY);
	} else {
		(void)xEventGroupClearBits(xEvents, COFFEE2_DEVICE_EVENT_READY);
	}
}

/*-----------------------------------------------------------*/
void vCoffee2DeviceSetRecovering(Coffee2DeviceId_e xDeviceId,
	uint8_t ucRecovering)
{
	EventGroupHandle_t xEvents;

	if ((xDeviceId <= COFFEE2_DEVICE_NONE) ||
		(xDeviceId >= COFFEE2_DEVICE_COUNT)) {
		return;
	}
	xEvents = s_axDeviceEvents[xDeviceId];
	ucRecovering = (ucRecovering != 0U) ? 1U : 0U;
	taskENTER_CRITICAL();
	g_axCoffee2DeviceStatus[xDeviceId].ucRecovering = ucRecovering;
	taskEXIT_CRITICAL();
	if (ucRecovering != 0U) {
		(void)xEventGroupSetBits(xEvents, COFFEE2_DEVICE_EVENT_RECOVERING);
	} else {
		(void)xEventGroupClearBits(xEvents,
			COFFEE2_DEVICE_EVENT_RECOVERING);
	}
}

/*-----------------------------------------------------------*/
void vCoffee2DeviceSetOnline(Coffee2DeviceId_e xDeviceId,
	uint8_t ucOnline)
{
	EventGroupHandle_t xEvents;

	if ((xDeviceId <= COFFEE2_DEVICE_NONE) ||
		(xDeviceId >= COFFEE2_DEVICE_COUNT)) {
		return;
	}
	xEvents = s_axDeviceEvents[xDeviceId];
	ucOnline = (ucOnline != 0U) ? 1U : 0U;
	taskENTER_CRITICAL();
	g_axCoffee2DeviceStatus[xDeviceId].ucOnline = ucOnline;
	taskEXIT_CRITICAL();
	if (ucOnline != 0U) {
		(void)xEventGroupClearBits(xEvents,
			COFFEE2_DEVICE_EVENT_COMM_FAULT);
		(void)xEventGroupSetBits(xEvents, COFFEE2_DEVICE_EVENT_ONLINE);
		if ((xDeviceId != COFFEE2_DEVICE_ROBOT) ||
			(g_axCoffee2DeviceStatus[xDeviceId].ucReady != 0U)) {
			(void)xEventGroupSetBits(xEvents,
				COFFEE2_DEVICE_EVENT_READY);
		} else {
			(void)xEventGroupClearBits(xEvents,
				COFFEE2_DEVICE_EVENT_READY);
		}
	} else {
		(void)xEventGroupClearBits(xEvents,
			COFFEE2_DEVICE_EVENT_ONLINE |
			COFFEE2_DEVICE_EVENT_READY);
		(void)xEventGroupSetBits(xEvents,
			COFFEE2_DEVICE_EVENT_COMM_FAULT);
		vCoffee2DeviceSetReady(xDeviceId, 0U);
	}
}

/*-----------------------------------------------------------*/
EventBits_t xCoffee2DeviceGetEvents(Coffee2DeviceId_e xDeviceId)
{
	if ((xDeviceId <= COFFEE2_DEVICE_NONE) ||
		(xDeviceId >= COFFEE2_DEVICE_COUNT) ||
		(s_axDeviceEvents[xDeviceId] == NULL)) {
		return 0U;
	}
	return xEventGroupGetBits(s_axDeviceEvents[xDeviceId]);
}

/*-----------------------------------------------------------*/
EventBits_t xCoffee2DeviceWaitCommand(Coffee2DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, TickType_t xWaitTicks)
{
	EventBits_t xBits;
	Coffee2DeviceStatus_t xStatus;
	TickType_t xWaitStart;
	TickType_t xRemaining;
	EventBits_t xTerminal;
	uint8_t ucIndex;

	if ((xDeviceId <= COFFEE2_DEVICE_NONE) ||
		(xDeviceId >= COFFEE2_DEVICE_COUNT) ||
		(s_axDeviceEvents[xDeviceId] == NULL) ||
		(ulCommandId == 0U)) {
		return 0U;
	}
	xWaitStart = xTaskGetTickCount();
	for (;;) {
		taskENTER_CRITICAL();
		xStatus = g_axCoffee2DeviceStatus[xDeviceId];
		xTerminal = 0U;
		for (ucIndex = 0U; ucIndex < COFFEE2_TERMINAL_HISTORY_COUNT;
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
			COFFEE2_DEVICE_EVENT_TERMINAL, pdFALSE, pdFALSE,
			(xRemaining > pdMS_TO_TICKS(100U)) ?
				pdMS_TO_TICKS(100U) : xRemaining);
		if ((xBits & COFFEE2_DEVICE_EVENT_TERMINAL) != 0U) {
			taskENTER_CRITICAL();
			xStatus = g_axCoffee2DeviceStatus[xDeviceId];
			taskEXIT_CRITICAL();
			if ((xStatus.ulLastOrderEpoch == ulOrderEpoch) &&
				(xStatus.ulLastCommandId == ulCommandId)) {
				return xBits & COFFEE2_DEVICE_EVENT_TERMINAL;
			}
		}
	}
}
