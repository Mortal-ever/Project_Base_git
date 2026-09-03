/**
  * @file      coffee2_robot_tcp.c
  * @brief     Implement Robot Modbus TCP commands and reconnect handling.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee2_robot_tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "coffee2_manager.h"
#include "coffee2_app_config.h"
#include "coffee2_device.h"
#include "coffee2_log.h"
#include "dobot_robot_device.h"
#include "modbus_port.h"
#include "queue.h"
#include "task.h"
#include "TcpClientSession/tcp_client_session.h"
#include "transport_tcp.h"

/** @brief Period between connected Robot health snapshots. */
#define COFFEE2_ROBOT_HEALTH_MS              2000U
#define COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS  200U
#define COFFEE2_ROBOT_STARTUP_FINAL_WAIT_MS  8000U
#define COFFEE2_ROBOT_STARTUP_POLL_MS        100U
#define COFFEE2_ROBOT_LINK_PROBE_COUNT       2U
#define COFFEE2_ROBOT_LINK_PROBE_DELAY_MS    100U
/** @brief Maximum connection event text kept on the Robot task stack. */
#define COFFEE2_ROBOT_CONNECTION_EVENT_LENGTH 64U

#define COFFEE2_ROBOT_STARTUP_STATE_DRAG       0x0001U
#define COFFEE2_ROBOT_STARTUP_STATE_POWER      0x0002U
#define COFFEE2_ROBOT_STARTUP_STATE_ENABLE     0x0004U
#define COFFEE2_ROBOT_STARTUP_STATE_ALARM      0x0008U
#define COFFEE2_ROBOT_STARTUP_STATE_COLLISION  0x0010U
#define COFFEE2_ROBOT_STARTUP_STATE_SAFETY     0x0020U
#define COFFEE2_ROBOT_STARTUP_STATE_RECOVERY   0x0040U
#define COFFEE2_ROBOT_STARTUP_STATE_READY      0x0080U
#define COFFEE2_ROBOT_STARTUP_STATE_MOTION     0x0100U

#if (COFFEE2_ROBOT_PROTOCOL_VARIANT == \
	COFFEE2_ROBOT_PROTOCOL_2)
#define COFFEE2_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_2
#define COFFEE2_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_2
#elif (COFFEE2_ROBOT_PROTOCOL_VARIANT == \
	COFFEE2_ROBOT_PROTOCOL_3)
#define COFFEE2_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_3
#define COFFEE2_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_3
#else
#define COFFEE2_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_1
#define COFFEE2_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_1
#endif

typedef enum {
	COFFEE2_ROBOT_STARTUP_OK = 0,
	COFFEE2_ROBOT_STARTUP_NOT_READY = 1
} Coffee2RobotStartupOutcome_e;

typedef enum {
	COFFEE2_DOBOT_COMMAND_NONE = 0,
	COFFEE2_DOBOT_COMMAND_BODY = 1,
	COFFEE2_DOBOT_COMMAND_ACTION = 2
} Coffee2DobotCommandKind_e;

typedef struct {
	Coffee2DobotCommandKind_e xKind;
	DobotRobotBodyCommand_e xBodyCommand;
	uint16_t usCommandCoil;
	uint16_t usResultCoil;
} Coffee2DobotCommand_t;

static const DobotRobotPoint_t s_axCoffee2DobotPoints[] = {
	{ COFFEE2_ACTION_ROBOT_HOME, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_HOME, DOBOT_ROBOT_P1_RESULT_HOME },
	{ COFFEE2_ACTION_ROBOT_TAKE_HOT_CUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_HOT_CUP, DOBOT_ROBOT_P1_RESULT_HOT_CUP },
	{ COFFEE2_ACTION_ROBOT_TAKE_COLD_CUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_COLD_CUP, DOBOT_ROBOT_P1_RESULT_COLD_CUP },
	{ COFFEE2_ACTION_ROBOT_TO_COFFEE, 0U, DOBOT_ROBOT_P1_COMMAND_COFFEE_FRONT, DOBOT_ROBOT_P1_RESULT_COFFEE_FRONT },
	{ COFFEE2_ACTION_ROBOT_TO_COFFEE, 1U, DOBOT_ROBOT_P1_COMMAND_COFFEE_INSIDE, DOBOT_ROBOT_P1_RESULT_COFFEE_INSIDE },
	{ COFFEE2_ACTION_ROBOT_TO_ICE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_ICE, DOBOT_ROBOT_P1_RESULT_ICE },
	{ COFFEE2_ACTION_ROBOT_TO_LID, 0U, DOBOT_ROBOT_P1_COMMAND_LID_1, DOBOT_ROBOT_P1_RESULT_LID_1 },
	{ COFFEE2_ACTION_ROBOT_TO_LID, 1U, DOBOT_ROBOT_P1_COMMAND_LID_2, DOBOT_ROBOT_P1_RESULT_LID_2 },
	{ COFFEE2_ACTION_ROBOT_TAKE_LID, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_LID, DOBOT_ROBOT_P1_RESULT_TAKE_LID },
	{ COFFEE2_ACTION_ROBOT_COVER_LID, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_COVER_LID, DOBOT_ROBOT_P1_RESULT_COVER_LID },
	{ COFFEE2_ACTION_ROBOT_PUT_OUTPUT, 1U, DOBOT_ROBOT_P1_COMMAND_OUTPUT_1, DOBOT_ROBOT_P1_RESULT_OUTPUT_1 },
	{ COFFEE2_ACTION_ROBOT_PUT_OUTPUT, 2U, DOBOT_ROBOT_P1_COMMAND_OUTPUT_2, DOBOT_ROBOT_P1_RESULT_OUTPUT_2 },
	{ COFFEE2_ACTION_ROBOT_PUT_STORAGE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_PUT_STORAGE, DOBOT_ROBOT_P1_RESULT_PUT_STORAGE },
	{ COFFEE2_ACTION_ROBOT_TO_PRINTER, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_PRINTER, DOBOT_ROBOT_P1_RESULT_PRINTER },
	{ COFFEE2_ACTION_ROBOT_TAKE_OUTPUT_1, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_OUTPUT_1, DOBOT_ROBOT_P1_RESULT_OUTPUT_1 },
	{ COFFEE2_ACTION_ROBOT_TAKE_OUTPUT_2, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_OUTPUT_2, DOBOT_ROBOT_P1_RESULT_OUTPUT_2 },
	{ COFFEE2_ACTION_ROBOT_TAKE_COFFEE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_COFFEE, DOBOT_ROBOT_P1_RESULT_TAKE_COFFEE },
	{ COFFEE2_ACTION_ROBOT_TAKE_STORAGE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_STORAGE, DOBOT_ROBOT_P1_RESULT_TAKE_STORAGE },
	{ COFFEE2_ACTION_ROBOT_TO_FRUIT_SYRUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_FRUIT_SYRUP, DOBOT_ROBOT_P1_RESULT_FRUIT_SYRUP }
};

#if (COFFEE2_ROBOT_PROTOCOL_VARIANT == COFFEE2_ROBOT_PROTOCOL_1)
static const DobotRobotDriverConfig_t s_xCoffee2DobotConfig1 = {
	&g_xDobotRobotProtocol1, s_axCoffee2DobotPoints,
	(uint16_t)(sizeof(s_axCoffee2DobotPoints) / sizeof(s_axCoffee2DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#elif (COFFEE2_ROBOT_PROTOCOL_VARIANT == COFFEE2_ROBOT_PROTOCOL_2)
static const DobotRobotDriverConfig_t s_xCoffee2DobotConfig2 = {
	&g_xDobotRobotProtocol2, s_axCoffee2DobotPoints,
	(uint16_t)(sizeof(s_axCoffee2DobotPoints) / sizeof(s_axCoffee2DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#elif (COFFEE2_ROBOT_PROTOCOL_VARIANT == COFFEE2_ROBOT_PROTOCOL_3)
static const DobotRobotDriverConfig_t s_xCoffee2DobotConfig3 = {
	&g_xDobotRobotProtocol3, s_axCoffee2DobotPoints,
	(uint16_t)(sizeof(s_axCoffee2DobotPoints) / sizeof(s_axCoffee2DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#endif

static const DobotRobotDriverConfig_t *prvCoffee2DobotConfig(void)
{
	#if (COFFEE2_ROBOT_PROTOCOL_VARIANT == COFFEE2_ROBOT_PROTOCOL_2)
	return &s_xCoffee2DobotConfig2;
	#elif (COFFEE2_ROBOT_PROTOCOL_VARIANT == COFFEE2_ROBOT_PROTOCOL_3)
	return &s_xCoffee2DobotConfig3;
	#else
	return &s_xCoffee2DobotConfig1;
	#endif
}

static uint8_t prvResolveCoffee2Dobot(const Coffee2Command_t *pxCommand,
	Coffee2DobotCommand_t *pxResolved)
{
	uint16_t usSelector;
	if ((pxCommand == NULL) || (pxResolved == NULL)) return 0U;
	pxResolved->xKind = COFFEE2_DOBOT_COMMAND_NONE;
	pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_START;
	pxResolved->usCommandCoil = 0U;
	pxResolved->usResultCoil = 0U;
	switch ((Coffee2Action_e)pxCommand->usAction) {
	case COFFEE2_ACTION_ROBOT_START: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_START; break;
	case COFFEE2_ACTION_ROBOT_STOP: case COFFEE2_ACTION_CANCEL: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_STOP; break;
	case COFFEE2_ACTION_ROBOT_PAUSE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_PAUSE; break;
	case COFFEE2_ACTION_ROBOT_ENABLE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_ENABLE; break;
	case COFFEE2_ACTION_ROBOT_DISABLE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_DISABLE; break;
	case COFFEE2_ACTION_ROBOT_CLEAR_ALARM: case COFFEE2_ACTION_RESET: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM; break;
	case COFFEE2_ACTION_ROBOT_ENTER_DRAG: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_ENTER_DRAG; break;
	case COFFEE2_ACTION_ROBOT_EXIT_DRAG: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_EXIT_DRAG; break;
	case COFFEE2_ACTION_ROBOT_AUTO_MODE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_AUTO_MODE; break;
	case COFFEE2_ACTION_ROBOT_MANUAL_MODE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_MANUAL_MODE; break;
	default:
		usSelector = DOBOT_ROBOT_SELECTOR_ANY;
		if (pxCommand->usAction == COFFEE2_ACTION_ROBOT_TO_COFFEE) usSelector = (pxCommand->ausParameter[0] == 0U) ? 0U : 1U;
		else if (pxCommand->usAction == COFFEE2_ACTION_ROBOT_TO_LID) usSelector = (pxCommand->ausParameter[0] <= 1U) ? 0U : 1U;
		else if (pxCommand->usAction == COFFEE2_ACTION_ROBOT_PUT_OUTPUT) {
			if ((pxCommand->ausParameter[0] < 1U) || (pxCommand->ausParameter[0] > 2U)) return 0U;
			usSelector = pxCommand->ausParameter[0];
		}
		if (ucDobotRobotResolvePoint(prvCoffee2DobotConfig(), pxCommand->usAction,
			usSelector, &pxResolved->usCommandCoil, &pxResolved->usResultCoil) == 0U) return 0U;
		pxResolved->xKind = COFFEE2_DOBOT_COMMAND_ACTION;
		return 1U;
	}
	pxResolved->xKind = COFFEE2_DOBOT_COMMAND_BODY;
	return 1U;
}

/*-----------------------------------------------------------*/
static const char *prvRobotActionName(uint16_t usAction)
{
	switch ((Coffee2Action_e)usAction) {
	case COFFEE2_ACTION_ROBOT_START: return "ROBOT_START";
	case COFFEE2_ACTION_ROBOT_STOP: return "ROBOT_STOP";
	case COFFEE2_ACTION_ROBOT_ENABLE: return "ROBOT_ENABLE";
	case COFFEE2_ACTION_ROBOT_CLEAR_ALARM: return "ROBOT_CLEAR_ALARM";
	case COFFEE2_ACTION_ROBOT_PAUSE: return "ROBOT_PAUSE";
	case COFFEE2_ACTION_ROBOT_DISABLE: return "ROBOT_DISABLE";
	case COFFEE2_ACTION_ROBOT_ENTER_DRAG: return "ROBOT_ENTER_DRAG";
	case COFFEE2_ACTION_ROBOT_EXIT_DRAG: return "ROBOT_EXIT_DRAG";
	case COFFEE2_ACTION_ROBOT_AUTO_MODE: return "ROBOT_AUTO_MODE";
	case COFFEE2_ACTION_ROBOT_MANUAL_MODE: return "ROBOT_MANUAL_MODE";
	case COFFEE2_ACTION_ROBOT_HOME: return "ROBOT_HOME";
	case COFFEE2_ACTION_ROBOT_TAKE_HOT_CUP: return "ROBOT_TAKE_HOT_CUP";
	case COFFEE2_ACTION_ROBOT_TAKE_COLD_CUP: return "ROBOT_TAKE_COLD_CUP";
	case COFFEE2_ACTION_ROBOT_TO_COFFEE: return "ROBOT_TO_COFFEE";
	case COFFEE2_ACTION_ROBOT_TO_ICE: return "ROBOT_TO_ICE";
	case COFFEE2_ACTION_ROBOT_TO_LID: return "ROBOT_TO_LID";
	case COFFEE2_ACTION_ROBOT_TAKE_LID: return "ROBOT_TAKE_LID";
	case COFFEE2_ACTION_ROBOT_COVER_LID: return "ROBOT_COVER_LID";
	case COFFEE2_ACTION_ROBOT_PUT_OUTPUT: return "ROBOT_PUT_OUTPUT";
	case COFFEE2_ACTION_ROBOT_PUT_STORAGE: return "ROBOT_PUT_STORAGE";
	case COFFEE2_ACTION_ROBOT_TO_PRINTER: return "ROBOT_TO_PRINTER";
	case COFFEE2_ACTION_ROBOT_TAKE_OUTPUT_1: return "ROBOT_TAKE_OUTPUT_1";
	case COFFEE2_ACTION_ROBOT_TAKE_OUTPUT_2: return "ROBOT_TAKE_OUTPUT_2";
	case COFFEE2_ACTION_ROBOT_TAKE_COFFEE: return "ROBOT_TAKE_COFFEE";
	case COFFEE2_ACTION_ROBOT_TAKE_STORAGE: return "ROBOT_TAKE_STORAGE";
	case COFFEE2_ACTION_ROBOT_START_SIGNAL: return "ROBOT_START_SIGNAL";
	case COFFEE2_ACTION_ROBOT_TO_FRUIT_SYRUP: return "ROBOT_TO_FRUIT_SYRUP";
	default: return "ROBOT_ACTION";
	}
}

typedef struct {
	Coffee2Command_t xCommand;
	uint16_t usCommandCoil;
	uint16_t usResultCoil;
	TickType_t xRecoveryStart;
	TickType_t xAcceptedTick;
	TickType_t xLastAcceptLogTick;
	TickType_t xNextPollTick;
	uint8_t ucActive;
	uint8_t ucRecovering;
	uint8_t ucAmbiguous;
	uint8_t ucAccepted;
	uint8_t ucResultWhileCommandHigh;
	Coffee2RobotPhase_e xPhase;
} Coffee2RobotTransaction_t;

typedef struct {
	ModbusPort_t *pxPort;
	TransportTcpContext_t *pxTransport;
} Coffee2RobotSessionContext_t;

/**
  * @brief  Log one successful Robot TCP connection and remote endpoint.
  * @param[in] pucIpv4 Configured Robot IPv4 octets.
  * @param[in] usPort Configured Robot TCP port.
  * @param[in] ulAttempt Connection attempt number.
  */
static void prvLogRobotConnected(const uint8_t pucIpv4[4], uint16_t usPort,
	uint32_t ulAttempt);

COFFEE2_CCM_DATA
Coffee2RobotTcpStatus_t g_xCoffee2RobotTcpStatus;
COFFEE2_CCM_DATA
Coffee2RobotData_t g_xCoffee2RobotData;

COFFEE2_CCM_DATA
static StaticQueue_t s_xRobotQueueStorage;
COFFEE2_CCM_DATA
static uint8_t s_aucRobotQueueStorage[
	COFFEE2_COMMAND_QUEUE_LENGTH * sizeof(Coffee2Command_t)];
COFFEE2_CCM_DATA
static QueueHandle_t s_xRobotQueue;

/**
  * @brief Refresh Robot base and control coils.
  * @param[in,out] pxPort Initialized Robot Modbus TCP port.
  * @param[in] ulTimeoutMs Transaction timeout in milliseconds.
  * @return Modbus transaction result.
  */
static ModbusPortResult_e prvRefresh(ModbusPort_t *pxPort,
	uint32_t ulTimeoutMs);
static uint8_t prvRobotSessionNetworkReady(void *pvOwnerContext);
static int32_t prvRobotSessionProbe(void *pvOwnerContext,
	uint32_t ulTimeoutMs);
static void prvRobotSessionEvent(void *pvOwnerContext,
	TcpClientSessionState_e xPreviousState,
	TcpClientSessionState_e xCurrentState, int32_t lReason,
	uint32_t ulAttempt, uint32_t ulRetryDelayMs);
/**
  * @brief Execute one standard command against Robot coils.
  * @param[in,out] pxPort Initialized Robot Modbus TCP port.
  * @param[in] pxCommand Command and parameters to execute.
  * @return Modbus transaction result.
  */
static ModbusPortResult_e prvExecute(ModbusPort_t *pxPort,
	const Coffee2Command_t *pxCommand,
	Coffee2RobotTransaction_t *pxTransaction,
	uint8_t *pucActionTimedOut);
static uint8_t prvRobotBasicAction(uint16_t usAction);
/**
  * @brief Generate a rising edge on a Robot control coil.
  * @param[in,out] pxPort Initialized Robot Modbus TCP port.
  * @param[in] usCoil Coil address to pulse.
  * @param[in] ulTimeoutMs Transaction timeout in milliseconds.
  * @return Modbus transaction result.
  */
static ModbusPortResult_e prvWriteRisingEdge(ModbusPort_t *pxPort,
	uint16_t usCoil, uint32_t ulTimeoutMs);
/**
  * @brief Wait for an action result and acknowledge it.
  * @param[in,out] pxPort Initialized Robot Modbus TCP port.
  * @param[in] usResultCoil Result coil address.
  * @param[in] ulTimeoutMs Total action timeout in milliseconds.
  * @param[in] pxCommand Command used for cooperative cancellation.
  * @return Modbus transaction result.
  */
static ModbusPortResult_e prvAdvanceAction(ModbusPort_t *pxPort,
	Coffee2RobotTransaction_t *pxTransaction, uint8_t *pucActionTimedOut);
static ModbusPortResult_e prvStartup(ModbusPort_t *pxPort,
	uint8_t ucRecoverySafe,
	Coffee2RobotStartupOutcome_e *pxOutcome);
static ModbusPortResult_e prvReconcile(ModbusPort_t *pxPort,
	Coffee2RobotTransaction_t *pxTransaction,
	uint8_t *pucDone);
static uint8_t prvRobotOperational(void);
static uint8_t prvRobotStrictReady(void);
static ModbusPortResult_e prvClearActionCoils(ModbusPort_t *pxPort,
	uint8_t *pucStateMismatch, uint16_t usOrderId);
static ModbusPortResult_e prvClearActionRange(ModbusPort_t *pxPort,
	uint16_t usStart, uint16_t usCount, uint8_t *pucStateMismatch);
static uint16_t prvRobotStartupStateMask(void);
static ModbusPortResult_e prvWriteControlValue(ModbusPort_t *pxPort,
	uint16_t usCoil, bool bValue, uint32_t ulTimeoutMs);
static uint8_t prvRobotLinkFailureConfirmed(ModbusPort_t *pxPort,
	ModbusPortResult_e xResult, uint8_t *pucProbeAttempted)
{
	ModbusPortFault_t xFault;
	ModbusPortResult_e xProbeResult;
	uint8_t ucProbeIndex;
	uint8_t ucProbeNeeded;
	bool bProbe;

	if (pucProbeAttempted != NULL) {
		*pucProbeAttempted = 0U;
	}
	if ((pxPort == NULL) || (xResult == MODBUS_PORT_RESULT_OK)) {
		return 0U;
	}
	memset(&xFault, 0, sizeof(xFault));
	vModbusPortGetLastFault(pxPort, &xFault);
	if ((xResult != MODBUS_PORT_RESULT_TIMEOUT) &&
		(xResult != MODBUS_PORT_RESULT_TRANSPORT) &&
		(xResult != MODBUS_PORT_RESULT_PROTOCOL) &&
		(xResult != MODBUS_PORT_RESULT_NOT_READY)) {
		return 0U;
	}
	if ((xFault.xTransportResult == TRANSPORT_RESULT_DISCONNECTED) ||
		(xFault.xTransportResult == TRANSPORT_RESULT_NOT_OPEN) ||
		(xFault.xTransportResult == TRANSPORT_RESULT_NOT_READY) ||
		(xFault.xTransportResult == TRANSPORT_RESULT_IO_ERROR)) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_LINK_CONFIRMED_LOST",
			(int32_t)xResult, "transport_result",
			(int32_t)xFault.xTransportResult);
		return 1U;
	}
	ucProbeNeeded = ((xResult == MODBUS_PORT_RESULT_TIMEOUT) ||
		(xResult == MODBUS_PORT_RESULT_PROTOCOL) ||
		((xResult == MODBUS_PORT_RESULT_TRANSPORT) &&
		 (xFault.xTransportResult == TRANSPORT_RESULT_TIMEOUT)) ||
		(xFault.xTransportResult == TRANSPORT_RESULT_TIMEOUT)) ? 1U : 0U;
	if (ucProbeNeeded == 0U) {
		return 0U;
	}
	if (pucProbeAttempted != NULL) {
		*pucProbeAttempted = 1U;
	}
	for (ucProbeIndex = 0U;
		ucProbeIndex < COFFEE2_ROBOT_LINK_PROBE_COUNT; ucProbeIndex++) {
		bProbe = false;
		xProbeResult = xModbusPortReadCoils(pxPort,
			COFFEE2_ROBOT_UNIT_ID, 3100U, 1U, &bProbe,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xProbeResult == MODBUS_PORT_RESULT_OK) {
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_LINK_PROBE_OK", 0,
				"attempt", (int32_t)(ucProbeIndex + 1U));
			return 0U;
		}
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_LINK_PROBE_FAILED",
			(int32_t)xProbeResult, "attempt",
			(int32_t)(ucProbeIndex + 1U));
		if ((ucProbeIndex + 1U) < COFFEE2_ROBOT_LINK_PROBE_COUNT) {
			vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_LINK_PROBE_DELAY_MS));
		}
	}
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_ERROR,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_LINK_CONFIRMED_LOST",
		(int32_t)xResult, "attempts",
		(int32_t)COFFEE2_ROBOT_LINK_PROBE_COUNT);
	return 1U;
}

/*-----------------------------------------------------------*/
static void prvFoldServerCommands(const Coffee2Command_t *pxFirst,
	Coffee2Command_t *pxLatest)
{
	Coffee2Command_t axPending[COFFEE2_COMMAND_QUEUE_LENGTH];
	Coffee2Command_t xCandidate;
	BaseType_t xQueued;
	uint8_t ucCount;
	uint8_t ucIndex;
	uint8_t ucLatestIndex;

	if ((pxFirst == NULL) || (pxLatest == NULL)) {
		return;
	}
	*pxLatest = *pxFirst;
	ucCount = 0U;
	axPending[ucCount++] = *pxFirst;
	while ((ucCount < COFFEE2_COMMAND_QUEUE_LENGTH) &&
		(xQueueReceive(s_xRobotQueue, &xCandidate, 0U) == pdPASS)) {
		axPending[ucCount++] = xCandidate;
	}
	ucLatestIndex = 0U;
	for (ucIndex = 1U; ucIndex < ucCount; ucIndex++) {
		if ((axPending[ucIndex].ucSource ==
			(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) &&
			(axPending[ucLatestIndex].ucSource !=
			(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER ||
			(axPending[ucIndex].ulCommandId >
				axPending[ucLatestIndex].ulCommandId))) {
			ucLatestIndex = ucIndex;
		}
	}
	if (axPending[ucLatestIndex].ucSource ==
		(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) {
		*pxLatest = axPending[ucLatestIndex];
	}
	for (ucIndex = 0U; ucIndex < ucCount; ucIndex++) {
		if ((axPending[ucIndex].ucSource ==
			(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) &&
			(ucIndex != ucLatestIndex)) {
			vCoffee2DeviceCommandStarted(&axPending[ucIndex]);
			vCoffee2DeviceCommandCompleted(&axPending[ucIndex],
				COFFEE2_COMMAND_RESULT_SUPERSEDED, 0U);
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)axPending[ucIndex].ulOrderId,
				"ROBOT_ACTION_SUPERSEDED",
				COFFEE2_COMMAND_RESULT_SUPERSEDED, "action",
				(int32_t)axPending[ucIndex].usAction);
		} else if (ucIndex != ucLatestIndex) {
			xQueued = xQueueSendToBack(s_xRobotQueue,
				&axPending[ucIndex], 0U);
			if (xQueued != pdPASS) {
				vCoffee2DeviceCommandStarted(&axPending[ucIndex]);
				vCoffee2DeviceCommandCompleted(&axPending[ucIndex],
					COFFEE2_COMMAND_RESULT_CANCELED, 0U);
				(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
					COFFEE2_LOG_SOURCE_ROBOT,
					"ROBOT_COMMAND_REQUEUE_FAILED", -1,
					"action", (int32_t)axPending[ucIndex].usAction);
			}
		}
	}
}

/*-----------------------------------------------------------*/
static void prvDisconnectRobot(TcpClientSession_t *pxSession,
	int32_t lReason);
static void prvSetRobotReady(uint8_t ucReady);
/**
  * @brief Calculate a capped reconnect delay.
  * @param[in] ulFailures Consecutive failure count.
  * @return Delay in milliseconds.
  */
static uint32_t prvRetryDelayMs(uint32_t ulFailures);

static const uint32_t s_aulCoffee2RobotRetryDelayMs[] = {
	1000U, 2000U, 5000U, 10000U, 30000U
};

static const TcpClientSessionConfig_t s_xCoffee2RobotSessionConfig = {
	prvRobotSessionNetworkReady,
	prvRobotSessionProbe,
	prvRobotSessionEvent,
	s_aulCoffee2RobotRetryDelayMs,
	(uint8_t)(sizeof(s_aulCoffee2RobotRetryDelayMs) /
		sizeof(s_aulCoffee2RobotRetryDelayMs[0])),
	COFFEE2_ROBOT_IO_TIMEOUT_MS
};

static const uint8_t s_aucCoffee2RobotIp[4] = {
	COFFEE2_ROBOT_IP_0, COFFEE2_ROBOT_IP_1,
	COFFEE2_ROBOT_IP_2, COFFEE2_ROBOT_IP_3
};

/*-----------------------------------------------------------*/
BaseType_t xCoffee2RobotTcpInitialize(void)
{
	memset(&g_xCoffee2RobotTcpStatus, 0,
		sizeof(g_xCoffee2RobotTcpStatus));
	memset(&g_xCoffee2RobotData, 0, sizeof(g_xCoffee2RobotData));
	s_xRobotQueue = xQueueCreateStatic(COFFEE2_COMMAND_QUEUE_LENGTH,
		sizeof(Coffee2Command_t), s_aucRobotQueueStorage,
		&s_xRobotQueueStorage);
	if (s_xRobotQueue == NULL) {
		return pdFAIL;
	}
	vCoffee2DeviceRegisterRoute(0U, s_xRobotQueue);
	return pdPASS;
}

/*-----------------------------------------------------------*/
static void prvLogRobotConnected(const uint8_t pucIpv4[4], uint16_t usPort,
	uint32_t ulAttempt)
{
	static const char acPrefix[] = "ROBOT_TCP_CONNECTED peer=";
	char acEvent[COFFEE2_ROBOT_CONNECTION_EVENT_LENGTH];
	uint16_t usPrefixLength;
	const char *pcEvent;

	usPrefixLength = (uint16_t)(sizeof(acPrefix) - 1U);
	memcpy(acEvent, acPrefix, usPrefixLength);
	if (ucTransportTcpFormatIpv4Endpoint(pucIpv4, usPort,
		&acEvent[usPrefixLength],
		(uint16_t)(sizeof(acEvent) - usPrefixLength)) != 0U) {
		pcEvent = acEvent;
	} else {
		pcEvent = "ROBOT_TCP_CONNECTED";
	}
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, pcEvent, 0, "attempt",
		(int32_t)ulAttempt);
}

/*-----------------------------------------------------------*/
void vCoffee2RobotTcpTask(void *pvArgument)
{
	TransportChannel_t xChannel;
	TransportTcpContext_t xTransport;
	TransportTcpConfig_t xConfig;
	ModbusPort_t xPort;
	TcpClientSession_t xSession;
	Coffee2RobotSessionContext_t xSessionContext;
	Coffee2Command_t xCommand;
	Coffee2Command_t xDeferredCommand;
	Coffee2RobotTransaction_t xTransaction;
	Coffee2DobotCommand_t xResolvedCommand;
	TransportResult_e xTransportResult;
	ModbusPortResult_e xResult;
	Coffee2RobotStartupOutcome_e xStartupOutcome;
	TickType_t xNextStartupRetryTick;
	TickType_t xNextHealthTick;
	uint32_t ulStartupFailures;
	uint8_t ucCreated;
	uint8_t ucSessionReady;
	uint8_t ucDone;
	uint8_t ucWorkflowTransaction;
	uint8_t ucReconciledSession;
	uint8_t ucRecoveryWaitingLogged;
	uint8_t ucDeferredCommand;
	uint8_t ucSessionCommandPending;
	uint8_t ucWarmAttachLogged;
	uint8_t ucActionTimedOut;
	uint8_t ucLinkProbeAttempted;
	uint8_t ucLinkConfirmed;
	BaseType_t xCommandReceived;

	(void)pvArgument;
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "TASK_RUNNING:C2Robot", 0);
	vAppTaskManagerWaitNetworkStackReady();
	memset(&xConfig, 0, sizeof(xConfig));
	xConfig.xMode = TRANSPORT_TCP_MODE_CLIENT;
	memcpy(xConfig.aucRemoteIp, s_aucCoffee2RobotIp,
		sizeof(s_aucCoffee2RobotIp));
	xConfig.usPort = COFFEE2_ROBOT_PORT;
	xConfig.ulConnectTimeoutMs = COFFEE2_ROBOT_CONNECT_TIMEOUT_MS;
	xConfig.ulIoTimeoutMs = COFFEE2_ROBOT_IO_TIMEOUT_MS;
	xTransportResult = xTransportTcpCreate(&xChannel, &xTransport,
		"coffee2_robot_tcp", &xConfig);
	ucCreated = 0U;
	if (xTransportResult == TRANSPORT_RESULT_OK) {
		xResult = xModbusPortClientInit(&xPort, &xChannel,
			MODBUS_PORT_TRANSPORT_TCP,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		ucCreated = (xResult == MODBUS_PORT_RESULT_OK) ? 1U : 0U;
	}
	memset(&xSessionContext, 0, sizeof(xSessionContext));
	if (ucCreated != 0U) {
		xSessionContext.pxPort = &xPort;
		xSessionContext.pxTransport = &xTransport;
		if (xTcpClientSessionInit(&xSession,
			&s_xCoffee2RobotSessionConfig, &xChannel,
			&xSessionContext) != pdPASS) {
			ucCreated = 0U;
		}
	}
	(void)xCoffee2LogWriteField(
		(ucCreated != 0U) ? COFFEE2_LOG_LEVEL_INFO :
			COFFEE2_LOG_LEVEL_ERROR,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_ENDPOINT_INIT",
		(ucCreated != 0U) ? 0 : -1, "init_code",
		(ucCreated != 0U) ? 0 : (int32_t)xTransportResult);
	if (ucCreated != 0U) {
#if COFFEE2_ROBOT_PROTOCOL_VARIANT == COFFEE2_ROBOT_PROTOCOL_2
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P2", 0,
			"driver", (int32_t)COFFEE2_ROBOT_LOG_DRIVER_ID);
#elif COFFEE2_ROBOT_PROTOCOL_VARIANT == COFFEE2_ROBOT_PROTOCOL_3
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P3", 0,
			"driver", (int32_t)COFFEE2_ROBOT_LOG_DRIVER_ID);
#else
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P1", 0,
			"driver", (int32_t)COFFEE2_ROBOT_LOG_DRIVER_ID);
#endif
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "DEVICE_LINK:ROBOT_TCP", 0,
			"unit", (int32_t)COFFEE2_ROBOT_UNIT_ID);
	}
	xNextStartupRetryTick = 0U;
	xNextHealthTick = 0U;
	ulStartupFailures = 0U;
	ucSessionReady = 0U;
	ucReconciledSession = 0U;
	ucRecoveryWaitingLogged = 0U;
	ucWarmAttachLogged = 0U;
	memset(&xTransaction, 0, sizeof(xTransaction));
	memset(&xDeferredCommand, 0, sizeof(xDeferredCommand));
	ucDeferredCommand = 0U;
	prvSetRobotReady(0U);
	vCoffee2DeviceSetRecovering(COFFEE2_DEVICE_ROBOT, 0U);

	for (;;) {
		 xCommandReceived = pdFAIL;
		ucSessionCommandPending = 0U;
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.xRecoveryStart != 0U)) {
			if (ucCoffee2CommandIsCanceled(
				&xTransaction.xCommand) != 0U) {
				vCoffee2DeviceSetRecovering(COFFEE2_DEVICE_ROBOT, 0U);
				vCoffee2DeviceCommandCompleted(&xTransaction.xCommand,
					COFFEE2_COMMAND_RESULT_CANCELED, 0U);
				memset(&xTransaction, 0, sizeof(xTransaction));
				ucRecoveryWaitingLogged = 0U;
			}
		}
		if (ucCreated == 0U) {
			vTaskDelay(pdMS_TO_TICKS(100U));
			continue;
		}
		vTcpClientSessionProcess(&xSession);
		if (ucTcpClientSessionIsOnline(&xSession) == 0U) {
			ucSessionReady = 0U;
			ucReconciledSession = 0U;
			ucWarmAttachLogged = 0U;
			vTaskDelay(pdMS_TO_TICKS(50U));
			continue;
		}

		if (ucSessionReady == 0U) {
			if ((xTransaction.ucActive == 0U) &&
				(ucDeferredCommand == 0U) &&
				(xQueuePeek(s_xRobotQueue, &xCommand, 0U) == pdPASS) &&
				(prvRobotBasicAction(xCommand.usAction) != 0U) &&
				(xQueueReceive(s_xRobotQueue, &xCommand, 0U) == pdPASS)) {
				if (xCommand.ucSource ==
					(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) {
					prvFoldServerCommands(&xCommand, &xCommand);
				}
				xDeferredCommand = xCommand;
				ucDeferredCommand = 1U;
				ucSessionCommandPending = 1U;
			}
			if ((xTransaction.ucActive != 0U) &&
				(ucDeferredCommand == 0U) &&
				(xQueueReceive(s_xRobotQueue, &xCommand, 0U) == pdPASS)) {
				if (xCommand.ucSource ==
					(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) {
					prvFoldServerCommands(&xCommand, &xCommand);
					vCoffee2DeviceCommandCompleted(&xTransaction.xCommand,
						COFFEE2_COMMAND_RESULT_SUPERSEDED, 0U);
					(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
						COFFEE2_LOG_SOURCE_ROBOT,
						(uint16_t)xTransaction.xCommand.ulOrderId,
						"ROBOT_ACTION_SUPERSEDED",
						COFFEE2_COMMAND_RESULT_SUPERSEDED,
						"action", (int32_t)xTransaction.xCommand.usAction);
					memset(&xTransaction, 0, sizeof(xTransaction));
					xDeferredCommand = xCommand;
					ucDeferredCommand = 1U;
					if (prvRobotBasicAction(xCommand.usAction) != 0U) {
						ucSessionCommandPending = 1U;
					}
				} else {
					/* Preserve non-server commands until the link is ready. */
					xDeferredCommand = xCommand;
					ucDeferredCommand = 1U;
					if (prvRobotBasicAction(xCommand.usAction) != 0U) {
						ucSessionCommandPending = 1U;
					}
					if ((xCommand.usAction ==
						(uint16_t)COFFEE2_ACTION_CANCEL) ||
						(ucCoffee2CommandIsCanceled(
							&xTransaction.xCommand) != 0U)) {
						vCoffee2DeviceSetRecovering(
							COFFEE2_DEVICE_ROBOT, 0U);
						vCoffee2DeviceCommandCompleted(
							&xTransaction.xCommand,
							COFFEE2_COMMAND_RESULT_CANCELED, 0U);
						memset(&xTransaction, 0, sizeof(xTransaction));
					}
				}
			}
			if ((xTransaction.ucActive != 0U) &&
				(xTransaction.ucAmbiguous == 0U)) {
				if (ucReconciledSession == 0U) {
					(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
						COFFEE2_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_RECONCILE", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
				}
				xResult = prvReconcile(&xPort, &xTransaction,
					&ucDone);
				if (xResult != MODBUS_PORT_RESULT_OK) {
					if (prvRobotLinkFailureConfirmed(&xPort, xResult,
						&ucLinkProbeAttempted) != 0U) {
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					}
					continue;
				}
				ucReconciledSession = 1U;
				if ((xTransaction.ucActive != 0U) &&
					(xTransaction.xRecoveryStart != 0U) &&
					(ucDone == 0U)) {
					xTransaction.xRecoveryStart = 0U;
					xTransaction.ucRecovering = 0U;
					vCoffee2DeviceSetRecovering(
						COFFEE2_DEVICE_ROBOT, 0U);
					vCoffee2DeviceSetRobotPhase(
						xTransaction.xPhase);
					ucSessionReady = 1U;
					prvSetRobotReady(prvRobotStrictReady());
				}
				if (ucDone != 0U) {
					(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
						COFFEE2_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_COMPLETED", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
					vCoffee2DeviceSetRecovering(COFFEE2_DEVICE_ROBOT, 0U);
					vCoffee2DeviceCommandCompleted(&xTransaction.xCommand,
						0, 0U);
					memset(&xTransaction, 0, sizeof(xTransaction));
				}
			}
			if (xTransaction.ucActive != 0U) {
				if ((ucRecoveryWaitingLogged == 0U) &&
					(xTransaction.xRecoveryStart != 0U)) {
					(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
						COFFEE2_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_WAITING", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
					ucRecoveryWaitingLogged = 1U;
				}
				if (ucCoffee2CommandIsCanceled(
					&xTransaction.xCommand) != 0U) {
					vCoffee2DeviceSetRecovering(
						COFFEE2_DEVICE_ROBOT, 0U);
					vCoffee2DeviceCommandCompleted(
					&xTransaction.xCommand,
					COFFEE2_COMMAND_RESULT_CANCELED, 0U);
					memset(&xTransaction, 0, sizeof(xTransaction));
					ucRecoveryWaitingLogged = 0U;
				}
				if (xTransaction.ucActive != 0U) {
					vTaskDelay(pdMS_TO_TICKS(
						COFFEE2_ROBOT_ACTION_POLL_MS));
					continue;
				}
			}
			if ((xTransaction.ucActive == 0U) &&
				(prvRobotOperational() != 0U)) {
				ucSessionReady = 1U;
				if (ucWarmAttachLogged == 0U) {
					(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
						COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_WARM_ATTACH", 0,
						"state_mask", (int32_t)prvRobotStartupStateMask());
					ucWarmAttachLogged = 1U;
				}
				prvSetRobotReady(prvRobotStrictReady());
			}
			if ((ucSessionCommandPending == 0U) &&
				(xTransaction.ucActive == 0U) &&
				(prvRobotOperational() == 0U)) {
				if ((int32_t)(xTaskGetTickCount() -
					xNextStartupRetryTick) < 0) {
					vTaskDelay(pdMS_TO_TICKS(50U));
					continue;
				}
				(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
					COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_ATTEMPT", 0,
					"attempt", (int32_t)(ulStartupFailures + 1U));
				xStartupOutcome = COFFEE2_ROBOT_STARTUP_OK;
				xResult = prvStartup(&xPort, 0U, &xStartupOutcome);
				if ((xResult != MODBUS_PORT_RESULT_OK) ||
					(xStartupOutcome != COFFEE2_ROBOT_STARTUP_OK)) {
					ulStartupFailures++;
					xNextStartupRetryTick = xTaskGetTickCount() + pdMS_TO_TICKS(
						prvRetryDelayMs(ulStartupFailures));
					if (prvRobotLinkFailureConfirmed(&xPort, xResult,
						&ucLinkProbeAttempted) != 0U) {
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					} else if (xStartupOutcome != COFFEE2_ROBOT_STARTUP_OK) {
						vCoffee2DeviceSetOnline(COFFEE2_DEVICE_ROBOT, 1U);
						vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT, 0U);
						prvSetRobotReady(0U);
						(void)xCoffee2LogWriteField(
							COFFEE2_LOG_LEVEL_INFO,
							COFFEE2_LOG_SOURCE_ROBOT,
							"ROBOT_STARTUP_RETRY_SCHEDULED", 0,
							"delay_ms", (int32_t)prvRetryDelayMs(
								ulStartupFailures));
					}
					continue;
				}
				xResult = prvRefresh(&xPort, COFFEE2_ROBOT_IO_TIMEOUT_MS);
				if (xResult != MODBUS_PORT_RESULT_OK) {
					if (prvRobotLinkFailureConfirmed(&xPort, xResult,
						&ucLinkProbeAttempted) != 0U) {
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					}
					continue;
				}
			}
			if (prvRobotOperational() != 0U) {
				ucSessionReady = 1U;
				prvSetRobotReady(prvRobotStrictReady());
				(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
					COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_SERVICE_READY", 0);
				ulStartupFailures = 0U;
				g_xCoffee2RobotTcpStatus.ulConsecutiveFailures = 0U;
				g_xCoffee2RobotTcpStatus.ulNextRetryDelayMs = 0U;
			}
			if (ucSessionCommandPending != 0U) {
				ucSessionReady = 1U;
			}
			continue;
		}
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.xPhase != COFFEE2_ROBOT_PHASE_IDLE) &&
			(xTransaction.xRecoveryStart == 0U)) {
			if ((int32_t)(xTaskGetTickCount() -
				xTransaction.xNextPollTick) >= 0) {
				xTransaction.xNextPollTick = xTaskGetTickCount() +
					pdMS_TO_TICKS(COFFEE2_ROBOT_ACTION_POLL_MS);
				xResult = prvAdvanceAction(&xPort, &xTransaction,
					&ucActionTimedOut);
				if (xResult == MODBUS_PORT_RESULT_OK) {
					vCoffee2DeviceCommandCompleted(
						&xTransaction.xCommand, 0, 0U);
					memset(&xTransaction, 0, sizeof(xTransaction));
				} else if (xResult == MODBUS_PORT_RESULT_CANCELED) {
					vCoffee2DeviceCommandCompleted(
						&xTransaction.xCommand,
						COFFEE2_COMMAND_RESULT_CANCELED, 0U);
					memset(&xTransaction, 0, sizeof(xTransaction));
				} else if (ucActionTimedOut != 0U) {
					vCoffee2DeviceCommandCompleted(
						&xTransaction.xCommand,
						MODBUS_PORT_RESULT_TIMEOUT, 1U);
					memset(&xTransaction, 0, sizeof(xTransaction));
				} else if (xResult != MODBUS_PORT_RESULT_BUSY) {
					ucLinkConfirmed = prvRobotLinkFailureConfirmed(
						&xPort, xResult, &ucLinkProbeAttempted);
					if (ucLinkConfirmed != 0U) {
						xTransaction.xRecoveryStart =
							xTaskGetTickCount();
						xTransaction.ucRecovering = 1U;
						xTransaction.xPhase =
							COFFEE2_ROBOT_PHASE_RECOVERING;
						vCoffee2DeviceSetRecovering(
							COFFEE2_DEVICE_ROBOT, 1U);
						vCoffee2DeviceSetRobotPhase(
							COFFEE2_ROBOT_PHASE_RECOVERING);
						ucSessionReady = 0U;
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					} else if ((xResult == MODBUS_PORT_RESULT_TIMEOUT) ||
						(xResult == MODBUS_PORT_RESULT_PROTOCOL) ||
						(ucLinkProbeAttempted != 0U)) {
						xTransaction.xRecoveryStart =
							xTaskGetTickCount();
						xTransaction.ucRecovering = 1U;
						xTransaction.xPhase =
							COFFEE2_ROBOT_PHASE_RECOVERING;
						vCoffee2DeviceSetRecovering(
							COFFEE2_DEVICE_ROBOT, 1U);
						vCoffee2DeviceSetRobotPhase(
							COFFEE2_ROBOT_PHASE_RECOVERING);
						ucSessionReady = 0U;
					} else {
						vCoffee2DeviceCommandCompleted(
							&xTransaction.xCommand,
							(int32_t)xResult, 0U);
						memset(&xTransaction, 0,
							sizeof(xTransaction));
					}
				}
			}
			if (xTransaction.ucActive != 0U) {
				if (xQueueReceive(s_xRobotQueue, &xCommand, 0U) ==
					pdPASS) {
					xCommandReceived = pdPASS;
					if (xCommand.ucSource ==
						(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) {
						prvFoldServerCommands(&xCommand, &xCommand);
						vCoffee2DeviceCommandCompleted(
							&xTransaction.xCommand,
							COFFEE2_COMMAND_RESULT_SUPERSEDED, 0U);
						(void)xCoffee2LogWriteFieldOrder(
							COFFEE2_LOG_LEVEL_WARNING,
							COFFEE2_LOG_SOURCE_ROBOT,
							(uint16_t)xTransaction.xCommand.ulOrderId,
							"ROBOT_ACTION_SUPERSEDED",
							COFFEE2_COMMAND_RESULT_SUPERSEDED,
							"action", (int32_t)
								xTransaction.xCommand.usAction);
						memset(&xTransaction, 0,
							sizeof(xTransaction));
					} else {
						vCoffee2DeviceCommandStarted(&xCommand);
						vCoffee2DeviceCommandCompleted(&xCommand,
							COFFEE2_COMMAND_RESULT_CANCELED, 0U);
						xCommandReceived = pdFAIL;
					}
				}
				if (xCommandReceived != pdPASS) {
					vTaskDelay(pdMS_TO_TICKS(
						COFFEE2_ROBOT_ACTION_POLL_MS));
					continue;
				}
			}
		}

		if (ucDeferredCommand != 0U) {
			xCommand = xDeferredCommand;
			ucDeferredCommand = 0U;
			xCommandReceived = pdPASS;
		}
		if ((xCommandReceived == pdPASS) ||
			(xQueueReceive(s_xRobotQueue, &xCommand,
			pdMS_TO_TICKS(COFFEE2_ROBOT_LOOP_MS)) == pdPASS)) {
			if ((xCommandReceived == pdFAIL) &&
				(xCommand.ucSource ==
					(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER)) {
				prvFoldServerCommands(&xCommand, &xCommand);
			}
			vCoffee2DeviceCommandStarted(&xCommand);
			ucWorkflowTransaction =
				(xCommand.ucSource ==
					(uint8_t)COFFEE2_COMMAND_SOURCE_WORKFLOW) ? 1U : 0U;
			memset(&xTransaction, 0, sizeof(xTransaction));
			if (ucWorkflowTransaction != 0U) {
				xTransaction.xCommand = xCommand;
				xTransaction.ucActive = 1U;
				xTransaction.xRecoveryStart = 0U;
				if ((prvResolveCoffee2Dobot(&xCommand,
					&xResolvedCommand) == 0U) ||
					(xResolvedCommand.xKind !=
						COFFEE2_DOBOT_COMMAND_ACTION)) {
					xTransaction.usCommandCoil = 0xFFFFU;
					xTransaction.usResultCoil = 0xFFFFU;
					xTransaction.ucAmbiguous = 1U;
					(void)xCoffee2LogWriteFieldOrder(
						COFFEE2_LOG_LEVEL_WARNING,
						COFFEE2_LOG_SOURCE_ROBOT,
						(uint16_t)xCommand.ulOrderId,
						"ROBOT_RECOVERY_AMBIGUOUS", 0,
						"action", (int32_t)xCommand.usAction);
				} else {
					xTransaction.usCommandCoil =
						xResolvedCommand.usCommandCoil;
					xTransaction.usResultCoil =
						xResolvedCommand.usResultCoil;
				}
			}
			(void)xCoffee2LogPrintfOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_ROBOT, (uint16_t)xCommand.ulOrderId,
				"ROBOT_ACTION_START=%s",
				prvRobotActionName(xCommand.usAction));
			ucActionTimedOut = 0U;
			xResult = prvExecute(&xPort, &xCommand,
			&xTransaction, &ucActionTimedOut);
			g_xCoffee2RobotTcpStatus.lLastResult = (int32_t)xResult;
			g_xCoffee2RobotTcpStatus.ulCommandCount++;
			if (xResult != MODBUS_PORT_RESULT_OK) {
				g_xCoffee2RobotTcpStatus.ulErrorCount++;
			}
			ucLinkConfirmed = 0U;
			if ((xResult == MODBUS_PORT_RESULT_BUSY) &&
				(xTransaction.ucActive != 0U)) {
				/* Position actions advance from the owner loop. */
			} else if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee2DeviceCommandCompleted(&xCommand, 0, 0U);
				memset(&xTransaction, 0, sizeof(xTransaction));
				if ((prvRobotBasicAction(xCommand.usAction) != 0U) &&
					(prvRobotOperational() == 0U)) {
					ucSessionReady = 0U;
				}
			} else if ((ucActionTimedOut != 0U) &&
				(ucWorkflowTransaction != 0U)) {
				if (xTransaction.ucRecovering == 0U) {
					(void)xCoffee2LogWriteFieldOrder(
						COFFEE2_LOG_LEVEL_WARNING,
						COFFEE2_LOG_SOURCE_ROBOT,
						(uint16_t)xCommand.ulOrderId,
						"ROBOT_ACTION_TIMEOUT_RECONCILE",
						MODBUS_PORT_RESULT_TIMEOUT, "command_id",
						(int32_t)xCommand.ulCommandId);
				}
				if (xTransaction.xRecoveryStart == 0U) {
					xTransaction.xRecoveryStart = xTaskGetTickCount();
				}
				xTransaction.ucRecovering = 1U;
				vCoffee2DeviceSetRecovering(COFFEE2_DEVICE_ROBOT, 1U);
				ucSessionReady = 0U;
			} else if (ucWorkflowTransaction != 0U) {
				ucLinkConfirmed = prvRobotLinkFailureConfirmed(&xPort,
					xResult, &ucLinkProbeAttempted);
				if (ucLinkConfirmed != 0U) {
					if (xTransaction.xRecoveryStart == 0U) {
						xTransaction.xRecoveryStart = xTaskGetTickCount();
					}
					xTransaction.ucRecovering = 1U;
					vCoffee2DeviceSetRecovering(COFFEE2_DEVICE_ROBOT, 1U);
					(void)xCoffee2LogWriteField(
						COFFEE2_LOG_LEVEL_WARNING,
						COFFEE2_LOG_SOURCE_ROBOT,
						"ROBOT_COMMAND_LINK_LOST", (int32_t)xResult,
						"command_id", (int32_t)xCommand.ulCommandId);
					vCoffee2DeviceSetOnline(COFFEE2_DEVICE_ROBOT, 0U);
					vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT, 0U);
					ucSessionReady = 0U;
					prvDisconnectRobot(&xSession, (int32_t)xResult);
				} else if ((xResult == MODBUS_PORT_RESULT_TIMEOUT) ||
					(xResult == MODBUS_PORT_RESULT_PROTOCOL) ||
					(ucLinkProbeAttempted != 0U)) {
					if (xTransaction.xRecoveryStart == 0U) {
						xTransaction.xRecoveryStart = xTaskGetTickCount();
						(void)xCoffee2LogWriteField(
							COFFEE2_LOG_LEVEL_WARNING,
							COFFEE2_LOG_SOURCE_ROBOT,
							"ROBOT_COMMAND_RECONCILE", (int32_t)xResult,
							"command_id", (int32_t)xCommand.ulCommandId);
					}
					xTransaction.ucRecovering = 1U;
					vCoffee2DeviceSetRecovering(COFFEE2_DEVICE_ROBOT, 1U);
					ucSessionReady = 0U;
				} else {
					vCoffee2DeviceCommandCompleted(&xCommand,
						(int32_t)xResult, 0U);
					memset(&xTransaction, 0, sizeof(xTransaction));
				}
			} else {
				vCoffee2DeviceCommandCompleted(&xCommand,
					(int32_t)xResult,
					(ucActionTimedOut != 0U) ? 1U : 0U);
				memset(&xTransaction, 0, sizeof(xTransaction));
				if ((prvRobotBasicAction(xCommand.usAction) != 0U) &&
					(prvRobotOperational() == 0U)) {
					ucSessionReady = 0U;
				}
				if (ucActionTimedOut == 0U) {
					ucLinkConfirmed = prvRobotLinkFailureConfirmed(&xPort,
						xResult, &ucLinkProbeAttempted);
				}
				if (ucLinkConfirmed != 0U) {
					vCoffee2DeviceSetOnline(COFFEE2_DEVICE_ROBOT, 0U);
					vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT, 0U);
					ucSessionReady = 0U;
					prvDisconnectRobot(&xSession, (int32_t)xResult);
				}
			}
		} else if ((int32_t)(xTaskGetTickCount() -
			xNextHealthTick) >= 0) {
			xResult = prvRefresh(&xPort, COFFEE2_ROBOT_IO_TIMEOUT_MS);
			g_xCoffee2RobotTcpStatus.lLastResult = (int32_t)xResult;
			if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee2DeviceSetOnline(COFFEE2_DEVICE_ROBOT, 1U);
				vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT,
					prvRobotStrictReady());
				prvSetRobotReady(prvRobotStrictReady());
			} else if (prvRobotLinkFailureConfirmed(&xPort, xResult,
				&ucLinkProbeAttempted) != 0U) {
				vCoffee2DeviceSetOnline(COFFEE2_DEVICE_ROBOT, 0U);
				vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT, 0U);
				prvSetRobotReady(0U);
				ucSessionReady = 0U;
				prvDisconnectRobot(&xSession, (int32_t)xResult);
			}
			xNextHealthTick = xTaskGetTickCount() + pdMS_TO_TICKS(
				COFFEE2_ROBOT_HEALTH_MS);
		}
	}
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvRefresh(ModbusPort_t *pxPort,
	uint32_t ulTimeoutMs)
{
	const DobotRobotProtocolConfig_t *pxProtocol;
	ModbusPortResult_e xResult;
	bool abBaseValues[DOBOT_ROBOT_BASE_INPUT_COUNT];
	bool abValues[COFFEE2_ROBOT_CONTROL_COIL_COUNT];
	uint8_t ucIndex;

	pxProtocol = prvCoffee2DobotConfig()->pxProtocol;
	xResult = xModbusPortReadDiscreteInputs(pxPort,
		COFFEE2_ROBOT_UNIT_ID, pxProtocol->usBaseInputStart,
		pxProtocol->usBaseInputCount, abBaseValues, ulTimeoutMs);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		for (ucIndex = 0U;
			ucIndex < pxProtocol->usBaseInputCount; ucIndex++) {
			g_xCoffee2RobotData.aucBaseInputs[ucIndex] =
				abBaseValues[ucIndex] ? 1U : 0U;
		}
		xResult = xModbusPortReadCoils(pxPort,
			COFFEE2_ROBOT_UNIT_ID, pxProtocol->usControlStart,
			pxProtocol->usControlCount,
			abValues, ulTimeoutMs);
	}
	if (xResult == MODBUS_PORT_RESULT_OK) {
		for (ucIndex = 0U;
			ucIndex < pxProtocol->usControlCount; ucIndex++) {
			g_xCoffee2RobotData.aucControlCoils[ucIndex] =
				abValues[ucIndex] ? 1U : 0U;
		}
	}
	return xResult;
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotSessionNetworkReady(void *pvOwnerContext)
{
	(void)pvOwnerContext;
	return ucAppTaskManagerIsNetworkReady();
}

/*-----------------------------------------------------------*/
static int32_t prvRobotSessionProbe(void *pvOwnerContext,
	uint32_t ulTimeoutMs)
{
	Coffee2RobotSessionContext_t *pxContext;

	pxContext = (Coffee2RobotSessionContext_t *)pvOwnerContext;
	if ((pxContext == NULL) || (pxContext->pxPort == NULL)) {
		return (int32_t)MODBUS_PORT_RESULT_INVALID_ARG;
	}
	return (int32_t)prvRefresh(pxContext->pxPort, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
static void prvRobotSessionEvent(void *pvOwnerContext,
	TcpClientSessionState_e xPreviousState,
	TcpClientSessionState_e xCurrentState, int32_t lReason,
	uint32_t ulAttempt, uint32_t ulRetryDelayMs)
{
	Coffee2RobotSessionContext_t *pxContext;
	ModbusPortFault_t xFault;
	int32_t lNativeError;

	pxContext = (Coffee2RobotSessionContext_t *)pvOwnerContext;
	lNativeError = 0;
	memset(&xFault, 0, sizeof(xFault));
	if ((pxContext != NULL) && (pxContext->pxTransport != NULL)) {
		lNativeError = pxContext->pxTransport->lLastNativeError;
	}
	if ((pxContext != NULL) && (pxContext->pxPort != NULL)) {
		vModbusPortGetLastFault(pxContext->pxPort, &xFault);
	}
	g_xCoffee2RobotTcpStatus.ulConnectAttemptCount = ulAttempt;
	g_xCoffee2RobotTcpStatus.lLastResult = lReason;

	if ((xPreviousState == TCP_CLIENT_SESSION_NETWORK_WAIT) &&
		(xCurrentState == TCP_CLIENT_SESSION_BACKOFF)) {
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_BACKOFF) &&
		(xCurrentState == TCP_CLIENT_SESSION_CONNECTING)) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_BEGIN", 0,
			"attempt", (int32_t)ulAttempt);
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_CONNECTING) &&
		(xCurrentState == TCP_CLIENT_SESSION_PROTOCOL_CHECK)) {
		g_xCoffee2RobotTcpStatus.ucConnected = 1U;
		g_xCoffee2RobotTcpStatus.ulConnectSuccessCount++;
		prvSetRobotReady(0U);
		prvLogRobotConnected(s_aucCoffee2RobotIp, COFFEE2_ROBOT_PORT,
			ulAttempt);
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_BEGIN", 0,
			"timeout_ms", (int32_t)COFFEE2_ROBOT_IO_TIMEOUT_MS);
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_PROTOCOL_CHECK) &&
		(xCurrentState == TCP_CLIENT_SESSION_ONLINE)) {
		g_xCoffee2RobotTcpStatus.ulConsecutiveFailures = 0U;
		g_xCoffee2RobotTcpStatus.ulNextRetryDelayMs = 0U;
		vCoffee2DeviceSetOnline(COFFEE2_DEVICE_ROBOT, 1U);
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_OK", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		return;
	}
	if (xCurrentState == TCP_CLIENT_SESSION_BACKOFF) {
		g_xCoffee2RobotTcpStatus.ulErrorCount++;
		g_xCoffee2RobotTcpStatus.ulConsecutiveFailures++;
		g_xCoffee2RobotTcpStatus.ulNextRetryDelayMs = ulRetryDelayMs;
		if (g_xCoffee2RobotTcpStatus.ucConnected != 0U) {
			g_xCoffee2RobotTcpStatus.ulDisconnectCount++;
		}
		g_xCoffee2RobotTcpStatus.ucConnected = 0U;
		prvSetRobotReady(0U);
		vCoffee2DeviceSetOnline(COFFEE2_DEVICE_ROBOT, 0U);
		vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT, 0U);
		if (xPreviousState == TCP_CLIENT_SESSION_CONNECTING) {
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_FAILED", lReason,
				"native_error", lNativeError);
			if (lReason == (int32_t)TRANSPORT_RESULT_TIMEOUT) {
				(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
					COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_TIMEOUT",
					lReason, "attempt", (int32_t)ulAttempt);
			}
		} else if (xPreviousState ==
			TCP_CLIENT_SESSION_PROTOCOL_CHECK) {
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_FAILED",
				lReason, "transport_result",
				(int32_t)xFault.xTransportResult);
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_PROTOCOL",
				lReason, "protocol_code", xFault.lProtocolCode);
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_EXCEPTION",
				lReason, "exception_code", (int32_t)xFault.ucExceptionCode);
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_NATIVE",
				lReason, "native_error", xFault.lNativeError);
		} else if (xPreviousState == TCP_CLIENT_SESSION_ONLINE) {
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_LINK_LOST", lReason,
				"attempt", (int32_t)ulAttempt);
		}
		if (lNativeError != 0) {
			vCoffee2LogLwipResourceFailure(COFFEE2_LOG_SOURCE_ROBOT,
				lNativeError);
		}
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_RETRY_SCHEDULED", lReason,
			"delay_ms", (int32_t)ulRetryDelayMs);
		return;
	}
	if (xCurrentState == TCP_CLIENT_SESSION_NETWORK_WAIT) {
		if (g_xCoffee2RobotTcpStatus.ucConnected != 0U) {
			g_xCoffee2RobotTcpStatus.ulDisconnectCount++;
		}
		g_xCoffee2RobotTcpStatus.ucConnected = 0U;
		prvSetRobotReady(0U);
		vCoffee2DeviceSetOnline(COFFEE2_DEVICE_ROBOT, 0U);
		vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT, 0U);
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_NETWORK_WAITING", lReason,
			"attempt", (int32_t)ulAttempt);
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotOperational(void)
{
	if ((g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ENABLED] == 0U) ||
		(g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ALARM] != 0U)) {
		return 0U;
	}
	if ((g_xCoffee2RobotData.aucControlCoils[0U] != 0U) ||
		(g_xCoffee2RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U) ||
		(g_xCoffee2RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_IDLE] != 0U)) {
		return 1U;
	}
	return 0U;
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotBasicAction(uint16_t usAction)
{
	return (((usAction >= (uint16_t)COFFEE2_ACTION_ROBOT_START) &&
		(usAction <= (uint16_t)COFFEE2_ACTION_ROBOT_MANUAL_MODE)) ||
		(usAction == (uint16_t)COFFEE2_ACTION_CANCEL) ||
		(usAction == (uint16_t)COFFEE2_ACTION_RESET)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotStrictReady(void)
{
	uint8_t ucReady;
	uint8_t ucRunning;
	uint8_t ucIdle;

	ucReady = g_xCoffee2RobotData.aucControlCoils[0U];
	ucRunning = g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RUNNING];
	ucIdle = g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_IDLE];
	if ((ucReady == 0U) || ((ucRunning == 0U) && (ucIdle == 0U)) ||
		(prvRobotOperational() == 0U)) {
		return 0U;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
static void prvSetRobotReady(uint8_t ucReady)
{
	uint8_t ucPrevious;

	ucReady = (ucReady != 0U) ? 1U : 0U;
	ucPrevious = g_xCoffee2RobotTcpStatus.ucReady;
	if (ucPrevious == ucReady) {
		vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT, ucReady);
		return;
	}
	g_xCoffee2RobotTcpStatus.ucReady = ucReady;
	vCoffee2DeviceSetReady(COFFEE2_DEVICE_ROBOT, ucReady);
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT,
		(ucReady != 0U) ? "ROBOT_READY_RISE" : "ROBOT_READY_FALL",
		(int32_t)ucReady, "previous", (int32_t)ucPrevious);
}

/*-----------------------------------------------------------*/
static uint16_t prvRobotStartupStateMask(void)
{
	uint16_t usMask;

	usMask = 0U;
	if (g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_DRAG] != 0U) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_DRAG;
	}
	if (g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_POWERED] != 0U) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_POWER;
	}
	if (g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ENABLED] != 0U) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_ENABLE;
	}
	if (g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ALARM] != 0U) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_ALARM;
	}
	if (g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_COLLISION] != 0U) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_COLLISION;
	}
	if (g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_SAFETY_PAUSED] != 0U) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_SAFETY;
	}
	if (g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RECOVERY] != 0U) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_RECOVERY;
	}
	if (g_xCoffee2RobotData.aucControlCoils[0U] != 0U) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_READY;
	}
	if ((g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U) ||
		(g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_IDLE] != 0U)) {
		usMask |= COFFEE2_ROBOT_STARTUP_STATE_MOTION;
	}
	return usMask;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvClearActionCoils(ModbusPort_t *pxPort,
	uint8_t *pucStateMismatch, uint16_t usOrderId)
{
	const DobotRobotProtocolConfig_t *pxProtocol;
	ModbusPortResult_e xResult;

	if (pucStateMismatch != NULL) {
		*pucStateMismatch = 0U;
	}
	pxProtocol = prvCoffee2DobotConfig()->pxProtocol;
	xResult = prvClearActionRange(pxPort,
		pxProtocol->usActionClearStart,
		pxProtocol->usActionClearCount, pucStateMismatch);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	if ((pucStateMismatch != NULL) && (*pucStateMismatch != 0U)) {
		return MODBUS_PORT_RESULT_OK;
	}
	xResult = prvClearActionRange(pxPort,
		pxProtocol->usActionClearSecondStart,
		pxProtocol->usActionClearSecondCount, pucStateMismatch);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, usOrderId,
		"ROBOT_ACTION_COILS_CLEARED", 0,
		"count", (int32_t)(pxProtocol->usActionClearCount +
			pxProtocol->usActionClearSecondCount));
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvClearActionRange(ModbusPort_t *pxPort,
	uint16_t usStart, uint16_t usCount, uint8_t *pucStateMismatch)
{
	bool abClear[10];
	bool abRead[10];
	uint16_t usIndex;
	ModbusPortResult_e xResult;

	if ((usCount == 0U) || (usCount > 10U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	for (usIndex = 0U; usIndex < usCount; usIndex++) {
		abClear[usIndex] = false;
	}
	xResult = xModbusPortWriteCoils(pxPort, COFFEE2_ROBOT_UNIT_ID,
		usStart, usCount, abClear, COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xResult = xModbusPortReadCoils(pxPort, COFFEE2_ROBOT_UNIT_ID,
		usStart, usCount, abRead, COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	for (usIndex = 0U; usIndex < usCount; usIndex++) {
		if (abRead[usIndex]) {
			if (pucStateMismatch != NULL) {
				*pucStateMismatch = 1U;
				return MODBUS_PORT_RESULT_OK;
			}
			return MODBUS_PORT_RESULT_PROTOCOL;
		}
	}
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvWriteControlValue(ModbusPort_t *pxPort,
	uint16_t usCoil, bool bValue, uint32_t ulTimeoutMs)
{
	return xModbusPortWriteCoil(pxPort, COFFEE2_ROBOT_UNIT_ID,
		usCoil, bValue, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvStartup(ModbusPort_t *pxPort,
	uint8_t ucRecoverySafe,
	Coffee2RobotStartupOutcome_e *pxOutcome)
{
	bool abBaseClear[10];
	uint8_t ucIndex;
	ModbusPortResult_e xResult;
	uint8_t ucOperational;
	uint8_t ucPowerObserved;
	TickType_t xWaitStartTick;

	*pxOutcome = COFFEE2_ROBOT_STARTUP_OK;
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_BEGIN",
		(int32_t)ucRecoverySafe, "state_mask",
		(int32_t)prvRobotStartupStateMask());
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 0,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	for (ucIndex = 0U; ucIndex < 10U; ucIndex++) {
		abBaseClear[ucIndex] = false;
	}
	xResult = xModbusPortWriteCoils(pxPort,
		COFFEE2_ROBOT_UNIT_ID, 0U, 10U, abBaseClear,
		COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 1,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteControlValue(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM, true,
		COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 2,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	if (ucRecoverySafe == 0U) {
		xResult = prvWriteControlValue(pxPort,
			DOBOT_ROBOT_BODY_COMMAND_STOP, true,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 3,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteControlValue(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM, false,
		COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 4,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteControlValue(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM, true,
		COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 5,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteControlValue(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_EXIT_DRAG, true,
		COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 6,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteControlValue(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_ENABLE, true,
		COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	if (ucRecoverySafe == 0U) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 7,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		xResult = prvWriteControlValue(pxPort,
			DOBOT_ROBOT_BODY_COMMAND_STOP, true,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 8,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteControlValue(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_START, true,
		COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_STEP_DELAY_MS));
	xWaitStartTick = xTaskGetTickCount();
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_WAIT", 0,
		"timeout_ms", (int32_t)COFFEE2_ROBOT_STARTUP_FINAL_WAIT_MS);
	for (;;) {
		xResult = prvRefresh(pxPort, COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		ucOperational = ((g_xCoffee2RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_ENABLED] != 0U) &&
			(g_xCoffee2RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_ALARM] == 0U) &&
			((g_xCoffee2RobotData.aucControlCoils[0U] != 0U) ||
			(g_xCoffee2RobotData.aucBaseInputs[
				DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U) ||
			(g_xCoffee2RobotData.aucBaseInputs[
				DOBOT_ROBOT_BODY_STATUS_IDLE] != 0U))) ? 1U : 0U;
		if (ucOperational != 0U) {
			break;
		}
		if ((xTaskGetTickCount() - xWaitStartTick) >= pdMS_TO_TICKS(
			COFFEE2_ROBOT_STARTUP_FINAL_WAIT_MS)) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_STARTUP_POLL_MS));
	}
	ucPowerObserved = g_xCoffee2RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_POWERED];
	if (ucOperational == 0U) {
		*pxOutcome = COFFEE2_ROBOT_STARTUP_NOT_READY;
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_RETRY", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucPowerObserved == 0U) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_ROBOT,
			"ROBOT_POWER_SIGNAL_NOT_OBSERVED", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
	}
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_DONE", 0,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvReconcile(ModbusPort_t *pxPort,
	Coffee2RobotTransaction_t *pxTransaction,
	uint8_t *pucDone)
{
	const DobotRobotProtocolConfig_t *pxProtocol;
	uint32_t ulControlEnd;
	uint8_t ucCommand;
	uint8_t ucResult;
	bool bResult;
	ModbusPortResult_e xResult;

	*pucDone = 0U;
	if (pxTransaction->usCommandCoil == 0xFFFFU) {
		return MODBUS_PORT_RESULT_OK;
	}
	pxProtocol = prvCoffee2DobotConfig()->pxProtocol;
	ulControlEnd = (uint32_t)pxProtocol->usControlStart +
		(uint32_t)pxProtocol->usControlCount;
	if (((uint32_t)pxTransaction->usCommandCoil <
		(uint32_t)pxProtocol->usControlStart) ||
		((uint32_t)pxTransaction->usCommandCoil >= ulControlEnd) ||
		((uint32_t)pxTransaction->usResultCoil <
		(uint32_t)pxProtocol->usControlStart) ||
		((uint32_t)pxTransaction->usResultCoil >= ulControlEnd)) {
		return MODBUS_PORT_RESULT_PROTOCOL;
	}
	xResult = prvRefresh(pxPort, COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	ucCommand = g_xCoffee2RobotData.aucControlCoils[
		pxTransaction->usCommandCoil - pxProtocol->usControlStart];
	ucResult = g_xCoffee2RobotData.aucControlCoils[
		pxTransaction->usResultCoil - pxProtocol->usControlStart];
	if ((ucCommand != 0U) && (ucResult != 0U) &&
		(pxTransaction->ucAccepted == 0U)) {
		pxTransaction->xPhase = COFFEE2_ROBOT_PHASE_WAIT_ACCEPT;
		vCoffee2DeviceSetRobotPhase(COFFEE2_ROBOT_PHASE_WAIT_ACCEPT);
		if (pxTransaction->ucResultWhileCommandHigh == 0U) {
			pxTransaction->ucResultWhileCommandHigh = 1U;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_PROTOCOL_SEQUENCE",
			MODBUS_PORT_RESULT_PROTOCOL, "action",
			(int32_t)pxTransaction->xCommand.usAction);
		}
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucCommand != 0U) {
		pxTransaction->xPhase = COFFEE2_ROBOT_PHASE_WAIT_ACCEPT;
		vCoffee2DeviceSetRobotPhase(COFFEE2_ROBOT_PHASE_WAIT_ACCEPT);
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucCommand == 0U) {
		if (pxTransaction->ucAccepted == 0U) {
			pxTransaction->ucAccepted = 1U;
			pxTransaction->xAcceptedTick = xTaskGetTickCount();
			vCoffee2DeviceSetRobotAccepted(1U);
			vCoffee2DeviceSetRobotPhase(COFFEE2_ROBOT_PHASE_MOVING);
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_ACCEPTED", 0,
				"coil", (int32_t)pxTransaction->usCommandCoil);
		}
		if (ucResult != 0U) {
			(void)xCoffee2LogPrintfOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_DONE=%s",
				prvRobotActionName(
					pxTransaction->xCommand.usAction));
			xResult = xModbusPortWriteCoil(pxPort,
				COFFEE2_ROBOT_UNIT_ID, pxTransaction->usResultCoil,
				false, COFFEE2_ROBOT_IO_TIMEOUT_MS);
			if (xResult != MODBUS_PORT_RESULT_OK) {
				return xResult;
			}
			xResult = xModbusPortReadCoils(pxPort,
				COFFEE2_ROBOT_UNIT_ID, pxTransaction->usResultCoil,
				1U, &bResult, COFFEE2_ROBOT_IO_TIMEOUT_MS);
			if (xResult != MODBUS_PORT_RESULT_OK) {
				return xResult;
			}
			if (bResult) {
				return MODBUS_PORT_RESULT_PROTOCOL;
			}
			*pucDone = 1U;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_RESULT_CLEARED", 0,
				"coil", (int32_t)pxTransaction->usResultCoil);
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_COMPLETE", 0,
				"action", (int32_t)pxTransaction->xCommand.usAction);
		}
		pxTransaction->xPhase = (ucResult != 0U) ?
			COFFEE2_ROBOT_PHASE_CLEAR_RESULT :
			COFFEE2_ROBOT_PHASE_MOVING;
	}
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static void prvDisconnectRobot(TcpClientSession_t *pxSession,
	int32_t lReason)
{
	vTcpClientSessionForceReconnect(pxSession, lReason);
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvExecute(ModbusPort_t *pxPort,
	const Coffee2Command_t *pxCommand,
	Coffee2RobotTransaction_t *pxTransaction,
	uint8_t *pucActionTimedOut)
{
	Coffee2DobotCommand_t xResolvedCommand;
	ModbusPortResult_e xResult;
	bool bResult;
	uint16_t usCommandCoil;
	uint16_t usResultCoil;
	uint8_t ucActionResolved;
	uint8_t ucStrictReady;

	if (pucActionTimedOut != NULL) {
		*pucActionTimedOut = 0U;
	}

	if (pxCommand->usAction == COFFEE2_ACTION_REFRESH) {
		return prvRefresh(pxPort, COFFEE2_ROBOT_IO_TIMEOUT_MS);
	}
	xResult = prvRefresh(pxPort, COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	if (pxCommand->usAction ==
		(uint16_t)COFFEE2_ACTION_ROBOT_START_SIGNAL) {
		return prvWriteControlValue(pxPort,
			DOBOT_ROBOT_P1_COMMAND_START_SIGNAL,
			(pxCommand->ausParameter[0U] != 0U),
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
	}
	ucActionResolved = 0U;
	if (prvResolveCoffee2Dobot(
		pxCommand, &xResolvedCommand) != 0U) {
		if (xResolvedCommand.xKind == COFFEE2_DOBOT_COMMAND_BODY) {
			return prvWriteRisingEdge(pxPort,
				xResolvedCommand.xBodyCommand,
				COFFEE2_ROBOT_IO_TIMEOUT_MS);
		}
		if (xResolvedCommand.xKind == COFFEE2_DOBOT_COMMAND_ACTION) {
			usCommandCoil = xResolvedCommand.usCommandCoil;
			usResultCoil = xResolvedCommand.usResultCoil;
			ucActionResolved = 1U;
		}
	}
	ucStrictReady = prvRobotStrictReady();
	if (ucStrictReady == 0U) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
			"ROBOT_COMMAND_NOT_READY",
			MODBUS_PORT_RESULT_PROTOCOL, "action",
			(int32_t)pxCommand->usAction);
		return MODBUS_PORT_RESULT_PROTOCOL;
	}
	if (ucActionResolved == 0U) {
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;
	}
	xResult = prvClearActionCoils(pxPort, NULL,
		(uint16_t)pxCommand->ulOrderId);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xResult = xModbusPortReadCoils(pxPort, COFFEE2_ROBOT_UNIT_ID,
		usResultCoil, 1U, &bResult, COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	if (bResult) {
		xResult = xModbusPortWriteCoil(pxPort, COFFEE2_ROBOT_UNIT_ID,
			usResultCoil, false, COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE2_ROBOT_UNIT_ID,
			usResultCoil, 1U, &bResult, COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		if (bResult) {
			return MODBUS_PORT_RESULT_PROTOCOL;
		}
	}
	(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
		"ROBOT_ACTION_PREPARED", 0,
		"coil", (int32_t)usCommandCoil);
	xResult = xModbusPortWriteCoil(pxPort, COFFEE2_ROBOT_UNIT_ID,
		usCommandCoil, true, COFFEE2_ROBOT_IO_TIMEOUT_MS);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
			"ROBOT_ACTION_SENT", 0,
			"coil", (int32_t)usCommandCoil);
		if (pxTransaction != NULL) {
			pxTransaction->xCommand = *pxCommand;
			pxTransaction->usCommandCoil = usCommandCoil;
			pxTransaction->usResultCoil = usResultCoil;
			pxTransaction->xRecoveryStart = 0U;
			pxTransaction->xAcceptedTick = 0U;
			pxTransaction->xLastAcceptLogTick = 0U;
			pxTransaction->xNextPollTick = xTaskGetTickCount();
			pxTransaction->ucActive = 1U;
			pxTransaction->ucRecovering = 0U;
			pxTransaction->ucAccepted = 0U;
			pxTransaction->ucResultWhileCommandHigh = 0U;
			pxTransaction->xPhase = COFFEE2_ROBOT_PHASE_WAIT_ACCEPT;
			vCoffee2DeviceSetRobotPhase(
				COFFEE2_ROBOT_PHASE_WAIT_ACCEPT);
			vCoffee2DeviceSetRobotAccepted(0U);
			return MODBUS_PORT_RESULT_BUSY;
		}
	}
	return xResult;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvWriteRisingEdge(ModbusPort_t *pxPort,
	uint16_t usCoil, uint32_t ulTimeoutMs)
{
	ModbusPortResult_e xResult;

	xResult = xModbusPortWriteCoil(pxPort, COFFEE2_ROBOT_UNIT_ID,
		usCoil, false, ulTimeoutMs);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		vTaskDelay(pdMS_TO_TICKS(COFFEE2_ROBOT_EDGE_LOW_MS));
		xResult = xModbusPortWriteCoil(pxPort,
			COFFEE2_ROBOT_UNIT_ID, usCoil, true, ulTimeoutMs);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvAdvanceAction(ModbusPort_t *pxPort,
	Coffee2RobotTransaction_t *pxTransaction, uint8_t *pucActionTimedOut)
{
	ModbusPortResult_e xResult;
	bool bCommand;
	bool bResult;
	bool bResultZero;
	TickType_t xNow;

	if ((pxTransaction == NULL) || (pxTransaction->ucActive == 0U)) {
		return MODBUS_PORT_RESULT_INVALID_ARG;
	}
	if (pucActionTimedOut != NULL) {
		*pucActionTimedOut = 0U;
	}
	if (ucCoffee2CommandIsCanceled(&pxTransaction->xCommand) != 0U) {
		return MODBUS_PORT_RESULT_CANCELED;
	}
	if (pxTransaction->xPhase == COFFEE2_ROBOT_PHASE_WAIT_ACCEPT) {
		xResult = xModbusPortReadCoils(pxPort, COFFEE2_ROBOT_UNIT_ID,
			pxTransaction->usCommandCoil, 1U, &bCommand,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE2_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResult,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xNow = xTaskGetTickCount();
		if (bCommand == false) {
			if (pxTransaction->ucAccepted == 0U) {
				pxTransaction->ucAccepted = 1U;
				pxTransaction->xAcceptedTick = xNow;
				vCoffee2DeviceSetRobotAccepted(1U);
				vCoffee2DeviceSetRobotPhase(
					COFFEE2_ROBOT_PHASE_MOVING);
				(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
					COFFEE2_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_ACCEPTED", 0,
					"coil", (int32_t)pxTransaction->usCommandCoil);
				(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
					COFFEE2_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_MOVING", 0,
					"action", (int32_t)pxTransaction->xCommand.usAction);
			}
			if (bResult != false) {
				(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
					COFFEE2_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_COMPLETE_SIGNAL", 0,
					"coil", (int32_t)pxTransaction->usResultCoil);
				pxTransaction->xPhase =
					COFFEE2_ROBOT_PHASE_CLEAR_RESULT;
			} else {
				pxTransaction->xPhase =
					COFFEE2_ROBOT_PHASE_MOVING;
			}
			return MODBUS_PORT_RESULT_BUSY;
		}
		if ((bCommand != false) && (bResult != false) &&
			(pxTransaction->ucResultWhileCommandHigh == 0U)) {
			pxTransaction->ucResultWhileCommandHigh = 1U;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_PROTOCOL_SEQUENCE",
				MODBUS_PORT_RESULT_PROTOCOL, "action",
				(int32_t)pxTransaction->xCommand.usAction);
		}
		if ((pxTransaction->xLastAcceptLogTick == 0U) ||
			((xNow - pxTransaction->xLastAcceptLogTick) >=
				pdMS_TO_TICKS(COFFEE2_ROBOT_ACCEPT_LOG_INTERVAL_MS))) {
			pxTransaction->xLastAcceptLogTick = xNow;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_ACCEPT_WAITING", 0,
				"coil", (int32_t)pxTransaction->usCommandCoil);
		}
		return MODBUS_PORT_RESULT_BUSY;
	}
	if (pxTransaction->xPhase == COFFEE2_ROBOT_PHASE_MOVING) {
		xResult = xModbusPortReadCoils(pxPort, COFFEE2_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResult,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		if (bResult) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_COMPLETE_SIGNAL", 0,
				"coil", (int32_t)pxTransaction->usResultCoil);
			pxTransaction->xPhase = COFFEE2_ROBOT_PHASE_CLEAR_RESULT;
			vCoffee2DeviceSetRobotPhase(
				COFFEE2_ROBOT_PHASE_CLEAR_RESULT);
			return MODBUS_PORT_RESULT_BUSY;
		}
		if ((xTaskGetTickCount() - pxTransaction->xAcceptedTick) >=
			pdMS_TO_TICKS(COFFEE2_ROBOT_MOTION_TIMEOUT_MS)) {
			if (pucActionTimedOut != NULL) {
				*pucActionTimedOut = 1U;
			}
			(void)xCoffee2LogPrintfOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_TIMEOUT=%s RESULT=%d",
				prvRobotActionName(
					pxTransaction->xCommand.usAction),
				(int)MODBUS_PORT_RESULT_TIMEOUT);
			return MODBUS_PORT_RESULT_TIMEOUT;
		}
		return MODBUS_PORT_RESULT_BUSY;
	}
	if (pxTransaction->xPhase == COFFEE2_ROBOT_PHASE_CLEAR_RESULT) {
		xResult = xModbusPortWriteCoil(pxPort, COFFEE2_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, false,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE2_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResultZero,
			COFFEE2_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		if (bResultZero) {
			return MODBUS_PORT_RESULT_PROTOCOL;
		}
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_RESULT_CLEARED", 0,
			"coil", (int32_t)pxTransaction->usResultCoil);
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_COMPLETE", 0,
			"action", (int32_t)pxTransaction->xCommand.usAction);
		return MODBUS_PORT_RESULT_OK;
	}
	return MODBUS_PORT_RESULT_BUSY;
}

/*-----------------------------------------------------------*/
static uint32_t prvRetryDelayMs(uint32_t ulFailures)
{
	static const uint32_t aulDelayMs[] = {
		1000U, 2000U, 5000U, 10000U, 30000U
	};
	uint32_t ulIndex;

	ulIndex = (ulFailures == 0U) ? 0U : ulFailures - 1U;
	if (ulIndex >= (sizeof(aulDelayMs) / sizeof(aulDelayMs[0]))) {
		ulIndex = (sizeof(aulDelayMs) /
			sizeof(aulDelayMs[0])) - 1U;
	}
	return aulDelayMs[ulIndex];
}
