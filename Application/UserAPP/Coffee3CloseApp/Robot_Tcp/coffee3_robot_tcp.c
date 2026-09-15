/**
  * @file      coffee3_robot_tcp.c
  * @brief     Implement Robot Modbus TCP commands and reconnect handling.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee3_robot_tcp.h"
#include "coffee3_server.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "coffee3_manager.h"
#include "coffee3_app_config.h"
#include "coffee3_device.h"
#include "coffee3_log.h"
#include "dobot_robot_device.h"
#include "modbus_port.h"
#include "queue.h"
#include "task.h"
#include "TcpClientSession/tcp_client_session.h"
#include "transport_tcp.h"
#include "stm32f4xx_hal.h"
#include "Ota/app_ota_flash.h"

/* VG-only journal: sectors 10/11, outside application and OTA staging.
 * Each committed record reserves a port BEFORE its SYN can be sent.
 * The other sector retains the last record during rollover erase. */
#define C3_PORT_BANK0 0x080C0000UL
#define C3_PORT_BANK1 0x080E0000UL
#define C3_PORT_END   0x08100000UL
#define C3_PORT_MAGIC 0x43535054UL
static uint8_t s_ucFreshReadyReset;
static uint8_t s_ucConnectionStartupPending;

static uint16_t prvReserveRobotPort(void)
{
	uint32_t ulAddress, ulLatest = 0U, ulSequence = 0U;
	uint32_t ulBank, ulEnd, ulSlot, ulError;
	const volatile uint32_t *pulRecord;
	FLASH_EraseInitTypeDef xErase;
	HAL_StatusTypeDef xResult = HAL_OK;
	uint16_t usPort;

	if (*(const volatile uint16_t *)FLASHSIZE_BASE != 1024U) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_ROBOT, "Source port journal requires 1MB Flash", -1);
		return 0U;
	}
	for (ulAddress = C3_PORT_BANK0; ulAddress < C3_PORT_END; ulAddress += 16U) {
		pulRecord = (const volatile uint32_t *)ulAddress;
		if ((pulRecord[0] == C3_PORT_MAGIC) &&
			(pulRecord[2] == ~pulRecord[1]) && (pulRecord[3] == 0U) &&
			((ulLatest == 0U) || ((int32_t)(pulRecord[1] - ulSequence) > 0))) {
			ulSequence = pulRecord[1];
			ulLatest = ulAddress;
		}
	}
	ulBank = (ulLatest >= C3_PORT_BANK1) ? C3_PORT_BANK1 : C3_PORT_BANK0;
	ulEnd = ulBank + 0x20000UL;
	ulSlot = (ulLatest == 0U) ? ulBank : ulLatest + 16U;
	for (; ulSlot < ulEnd; ulSlot += 16U) {
		pulRecord = (const volatile uint32_t *)ulSlot;
		if ((pulRecord[0] == 0xFFFFFFFFUL) && (pulRecord[1] == 0xFFFFFFFFUL) &&
			(pulRecord[2] == 0xFFFFFFFFUL) && (pulRecord[3] == 0xFFFFFFFFUL)) {
			break;
		}
	}
	/* Serialize with the task-owned OTA writer, without disabling IRQs. */
	vTaskSuspendAll();
	if (ucAppOtaFlashIsActive() != 0U) {
		(void)xTaskResumeAll();
		return 0U;
	}
	xResult = HAL_FLASH_Unlock();
	if ((xResult == HAL_OK) && (ulSlot >= ulEnd)) {
		ulSlot = (ulBank == C3_PORT_BANK0) ? C3_PORT_BANK1 : C3_PORT_BANK0;
		memset(&xErase, 0, sizeof(xErase));
		xErase.TypeErase = FLASH_TYPEERASE_SECTORS;
		xErase.Sector = (ulSlot == C3_PORT_BANK0) ? FLASH_SECTOR_10 : FLASH_SECTOR_11;
		xErase.NbSectors = 1U;
		xErase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
		xResult = HAL_FLASHEx_Erase(&xErase, &ulError);
	}
	ulSequence++;
	if (xResult == HAL_OK) xResult = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulSlot, C3_PORT_MAGIC);
	if (xResult == HAL_OK) xResult = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulSlot + 4U, ulSequence);
	if (xResult == HAL_OK) xResult = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulSlot + 8U, ~ulSequence);
	if (xResult == HAL_OK) xResult = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulSlot + 12U, 0U);
	if (HAL_FLASH_Lock() != HAL_OK) xResult = HAL_ERROR;
	pulRecord = (const volatile uint32_t *)ulSlot;
	if ((xResult == HAL_OK) && ((pulRecord[0] != C3_PORT_MAGIC) ||
		(pulRecord[1] != ulSequence) || (pulRecord[2] != ~ulSequence) ||
		(pulRecord[3] != 0U))) xResult = HAL_ERROR;
	(void)xTaskResumeAll();
	if (xResult != HAL_OK) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_ROBOT, "Source port persistence failed; connect deferred", (int32_t)xResult);
		return 0U;
	}
	usPort = (uint16_t)(49152UL + ((ulSequence - 1U) % 16384UL));
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, COFFEE3_LOG_ORDER_SYSTEM,
		"TCP source port reserved=%u destination=502 sequence=%lu", (unsigned int)usPort,
		(unsigned long)ulSequence);
	return usPort;
}

/** @brief Period between connected Robot health snapshots. */
#define COFFEE3_ROBOT_HEALTH_MS              2000U
#define COFFEE3_ROBOT_STARTUP_RETRY_MS        3000U
#define COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS  200U
#define COFFEE3_ROBOT_STARTUP_FINAL_WAIT_MS  8000U
#define COFFEE3_ROBOT_STARTUP_POLL_MS        100U
#define COFFEE3_ROBOT_LINK_PROBE_COUNT       2U
#define COFFEE3_ROBOT_LINK_PROBE_DELAY_MS    100U
/** @brief Maximum connection event text kept on the Robot task stack. */
#define COFFEE3_ROBOT_CONNECTION_EVENT_LENGTH 64U

#define COFFEE3_ROBOT_STARTUP_STATE_DRAG       0x0001U
#define COFFEE3_ROBOT_STARTUP_STATE_POWER      0x0002U
#define COFFEE3_ROBOT_STARTUP_STATE_ENABLE     0x0004U
#define COFFEE3_ROBOT_STARTUP_STATE_ALARM      0x0008U
#define COFFEE3_ROBOT_STARTUP_STATE_COLLISION  0x0010U
#define COFFEE3_ROBOT_STARTUP_STATE_SAFETY     0x0020U
#define COFFEE3_ROBOT_STARTUP_STATE_RECOVERY   0x0040U
#define COFFEE3_ROBOT_STARTUP_STATE_READY      0x0080U
#define COFFEE3_ROBOT_STARTUP_STATE_MOTION     0x0100U

#if (COFFEE3_ROBOT_PROTOCOL_VARIANT == \
	COFFEE3_ROBOT_PROTOCOL_2)
#define COFFEE3_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_2
#define COFFEE3_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_2
#elif (COFFEE3_ROBOT_PROTOCOL_VARIANT == \
	COFFEE3_ROBOT_PROTOCOL_3)
#define COFFEE3_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_3
#define COFFEE3_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_3
#else
#define COFFEE3_DOBOT_PROTOCOL_VARIANT \
	DOBOT_ROBOT_PROTOCOL_1
#define COFFEE3_ROBOT_LOG_DRIVER_ID \
	DEVICE_DRIVER_ROBOT_DOBOT_PROTOCOL_1
#endif

typedef enum {
	COFFEE3_ROBOT_STARTUP_OK = 0,
	COFFEE3_ROBOT_STARTUP_NOT_READY = 1
} Coffee3RobotStartupOutcome_e;

typedef enum {
	COFFEE3_DOBOT_COMMAND_NONE = 0,
	COFFEE3_DOBOT_COMMAND_BODY = 1,
	COFFEE3_DOBOT_COMMAND_ACTION = 2
} Coffee3DobotCommandKind_e;

typedef struct {
	Coffee3DobotCommandKind_e xKind;
	DobotRobotBodyCommand_e xBodyCommand;
	uint16_t usCommandCoil;
	uint16_t usResultCoil;
} Coffee3DobotCommand_t;

static const DobotRobotPoint_t s_axCoffee3DobotPoints[] = {
	{ COFFEE3_ACTION_ROBOT_HOME, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_HOME, DOBOT_ROBOT_P1_RESULT_HOME },
	{ COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_HOT_CUP, DOBOT_ROBOT_P1_RESULT_HOT_CUP },
	{ COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_COLD_CUP, DOBOT_ROBOT_P1_RESULT_COLD_CUP },
	{ COFFEE3_ACTION_ROBOT_TO_COFFEE, 0U, DOBOT_ROBOT_P1_COMMAND_COFFEE_FRONT, DOBOT_ROBOT_P1_RESULT_COFFEE_FRONT },
	{ COFFEE3_ACTION_ROBOT_TO_COFFEE, 1U, DOBOT_ROBOT_P1_COMMAND_COFFEE_INSIDE, DOBOT_ROBOT_P1_RESULT_COFFEE_INSIDE },
	{ COFFEE3_ACTION_ROBOT_TO_ICE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_ICE, DOBOT_ROBOT_P1_RESULT_ICE },
	{ COFFEE3_ACTION_ROBOT_TO_LID, 0U, DOBOT_ROBOT_P1_COMMAND_LID_1, DOBOT_ROBOT_P1_RESULT_LID_1 },
	{ COFFEE3_ACTION_ROBOT_TO_LID, 1U, DOBOT_ROBOT_P1_COMMAND_LID_2, DOBOT_ROBOT_P1_RESULT_LID_2 },
	{ COFFEE3_ACTION_ROBOT_TAKE_LID, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_LID, DOBOT_ROBOT_P1_RESULT_TAKE_LID },
	{ COFFEE3_ACTION_ROBOT_COVER_LID, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_COVER_LID, DOBOT_ROBOT_P1_RESULT_COVER_LID },
	{ COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, DOBOT_ROBOT_P1_COMMAND_PUT_OUTPUT, DOBOT_ROBOT_P1_RESULT_PUT_OUTPUT },
	{ COFFEE3_ACTION_ROBOT_PUT_STORAGE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_PUT_STORAGE, DOBOT_ROBOT_P1_RESULT_PUT_STORAGE },
	{ COFFEE3_ACTION_ROBOT_TO_PRINTER, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_PRINTER, DOBOT_ROBOT_P1_RESULT_PRINTER },
	{ COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_1, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_OUTPUT_1, DOBOT_ROBOT_P1_RESULT_OUTPUT_1 },
	{ COFFEE3_ACTION_ROBOT_TAKE_COFFEE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_COFFEE, DOBOT_ROBOT_P1_RESULT_TAKE_COFFEE },
	{ COFFEE3_ACTION_ROBOT_TAKE_STORAGE, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_TAKE_STORAGE, DOBOT_ROBOT_P1_RESULT_TAKE_STORAGE },
	{ COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP, DOBOT_ROBOT_SELECTOR_ANY, DOBOT_ROBOT_P1_COMMAND_FRUIT_SYRUP, DOBOT_ROBOT_P1_RESULT_FRUIT_SYRUP }
};

#if (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_1)
static const DobotRobotDriverConfig_t s_xCoffee3DobotConfig1 = {
	&g_xDobotRobotProtocol1, s_axCoffee3DobotPoints,
	(uint16_t)(sizeof(s_axCoffee3DobotPoints) / sizeof(s_axCoffee3DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#elif (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_2)
static const DobotRobotDriverConfig_t s_xCoffee3DobotConfig2 = {
	&g_xDobotRobotProtocol2, s_axCoffee3DobotPoints,
	(uint16_t)(sizeof(s_axCoffee3DobotPoints) / sizeof(s_axCoffee3DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#elif (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_3)
static const DobotRobotDriverConfig_t s_xCoffee3DobotConfig3 = {
	&g_xDobotRobotProtocol3, s_axCoffee3DobotPoints,
	(uint16_t)(sizeof(s_axCoffee3DobotPoints) / sizeof(s_axCoffee3DobotPoints[0])),
	DEVICE_ROLE_ROBOT_1
};
#endif

static const DobotRobotDriverConfig_t *prvCoffee3DobotConfig(void)
{
	#if (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_2)
	return &s_xCoffee3DobotConfig2;
	#elif (COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_3)
	return &s_xCoffee3DobotConfig3;
	#else
	return &s_xCoffee3DobotConfig1;
	#endif
}

static uint8_t prvResolveCoffee3Dobot(const Coffee3Command_t *pxCommand,
	Coffee3DobotCommand_t *pxResolved)
{
	uint16_t usSelector;
	if ((pxCommand == NULL) || (pxResolved == NULL)) return 0U;
	pxResolved->xKind = COFFEE3_DOBOT_COMMAND_NONE;
	pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_START;
	pxResolved->usCommandCoil = 0U;
	pxResolved->usResultCoil = 0U;
	switch ((Coffee3Action_e)pxCommand->usAction) {
	case COFFEE3_ACTION_ROBOT_START: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_START; break;
	case COFFEE3_ACTION_ROBOT_STOP: case COFFEE3_ACTION_CANCEL: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_STOP; break;
	case COFFEE3_ACTION_ROBOT_PAUSE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_PAUSE; break;
	case COFFEE3_ACTION_ROBOT_ENABLE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_ENABLE; break;
	case COFFEE3_ACTION_ROBOT_DISABLE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_DISABLE; break;
	case COFFEE3_ACTION_ROBOT_CLEAR_ALARM: case COFFEE3_ACTION_RESET: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM; break;
	case COFFEE3_ACTION_ROBOT_ENTER_DRAG: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_ENTER_DRAG; break;
	case COFFEE3_ACTION_ROBOT_EXIT_DRAG: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_EXIT_DRAG; break;
	case COFFEE3_ACTION_ROBOT_AUTO_MODE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_AUTO_MODE; break;
	case COFFEE3_ACTION_ROBOT_MANUAL_MODE: pxResolved->xBodyCommand = DOBOT_ROBOT_BODY_COMMAND_MANUAL_MODE; break;
	default:
		usSelector = DOBOT_ROBOT_SELECTOR_ANY;
		if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_TO_COFFEE) usSelector = (pxCommand->ausParameter[0] == 0U) ? 0U : 1U;
		else if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_TO_LID) usSelector = (pxCommand->ausParameter[0] <= 1U) ? 0U : 1U;
		else if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_PUT_OUTPUT) {
			if (pxCommand->ausParameter[0] != 1U) return 0U;
			usSelector = pxCommand->ausParameter[0];
		}
		if (ucDobotRobotResolvePoint(prvCoffee3DobotConfig(), pxCommand->usAction,
			usSelector, &pxResolved->usCommandCoil, &pxResolved->usResultCoil) == 0U) return 0U;
		pxResolved->xKind = COFFEE3_DOBOT_COMMAND_ACTION;
		return 1U;
	}
	pxResolved->xKind = COFFEE3_DOBOT_COMMAND_BODY;
	return 1U;
}

/*-----------------------------------------------------------*/
/* Map product action identifiers to readable diagnostics. */
/* Map product actions to readable diagnostics. */
static const char *prvRobotActionName(uint16_t usAction)
{
	switch ((Coffee3Action_e)usAction) {
	case COFFEE3_ACTION_ROBOT_START: return "ROBOT_START";
	case COFFEE3_ACTION_ROBOT_STOP: return "ROBOT_STOP";
	case COFFEE3_ACTION_ROBOT_ENABLE: return "ROBOT_ENABLE";
	case COFFEE3_ACTION_ROBOT_CLEAR_ALARM: return "ROBOT_CLEAR_ALARM";
	case COFFEE3_ACTION_ROBOT_PAUSE: return "ROBOT_PAUSE";
	case COFFEE3_ACTION_ROBOT_DISABLE: return "ROBOT_DISABLE";
	case COFFEE3_ACTION_ROBOT_ENTER_DRAG: return "ROBOT_ENTER_DRAG";
	case COFFEE3_ACTION_ROBOT_EXIT_DRAG: return "ROBOT_EXIT_DRAG";
	case COFFEE3_ACTION_ROBOT_AUTO_MODE: return "ROBOT_AUTO_MODE";
	case COFFEE3_ACTION_ROBOT_MANUAL_MODE: return "ROBOT_MANUAL_MODE";
	case COFFEE3_ACTION_ROBOT_HOME: return "ROBOT_HOME";
	case COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP: return "ROBOT_TAKE_HOT_CUP";
	case COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP: return "ROBOT_TAKE_COLD_CUP";
	case COFFEE3_ACTION_ROBOT_TO_COFFEE: return "ROBOT_TO_COFFEE";
	case COFFEE3_ACTION_ROBOT_TO_ICE: return "ROBOT_TO_ICE";
	case COFFEE3_ACTION_ROBOT_TO_LID: return "ROBOT_TO_LID";
	case COFFEE3_ACTION_ROBOT_TAKE_LID: return "ROBOT_TAKE_LID";
	case COFFEE3_ACTION_ROBOT_COVER_LID: return "ROBOT_COVER_LID";
	case COFFEE3_ACTION_ROBOT_PUT_OUTPUT: return "ROBOT_PUT_OUTPUT";
	case COFFEE3_ACTION_ROBOT_PUT_STORAGE: return "ROBOT_PUT_STORAGE";
	case COFFEE3_ACTION_ROBOT_TO_PRINTER: return "ROBOT_TO_PRINTER";
	case COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_1: return "ROBOT_TAKE_OUTPUT_1";
	case COFFEE3_ACTION_ROBOT_TAKE_OUTPUT_2: return "ROBOT_TAKE_OUTPUT_2";
	case COFFEE3_ACTION_ROBOT_TAKE_COFFEE: return "ROBOT_TAKE_COFFEE";
	case COFFEE3_ACTION_ROBOT_TAKE_STORAGE: return "ROBOT_TAKE_STORAGE";
	case COFFEE3_ACTION_ROBOT_START_SIGNAL: return "ROBOT_START_SIGNAL";
	case COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP: return "ROBOT_TO_FRUIT_SYRUP";
	default: return "ROBOT_ACTION";
	}
}

typedef struct {
	Coffee3Command_t xCommand;
	uint16_t usCommandCoil;
	uint16_t usResultCoil;
	TickType_t xRecoveryStart;
	TickType_t xAcceptDeadline;
	TickType_t xMotionDeadline;
	TickType_t xNextPrepareRetryTick;
	TickType_t xAcceptedTick;
	TickType_t xLastAcceptLogTick;
	TickType_t xNextPollTick;
	uint8_t ucActive;
	uint8_t ucRecovering;
	uint8_t ucAmbiguous;
	uint8_t ucAccepted;
	uint8_t ucResultWhileCommandHigh;
	uint8_t ucCommandWriteAttempted;
	uint8_t ucCommandWriteConfirmed;
	uint8_t ucCompletionObserved;
	uint8_t ucPrepareRetryCount;
	uint8_t ucTerminalLogged;
	int32_t lTerminalResult;
	Coffee3RobotPhase_e xPhase;
} Coffee3RobotTransaction_t;

/* Motion transactions are owned by this task through acceptance, movement, and result acknowledgement. */

typedef struct {
	ModbusPort_t *pxPort;
	TransportTcpContext_t *pxTransport;
} Coffee3RobotSessionContext_t;

/**
  * @brief  Log one successful Robot TCP connection and remote endpoint.
  * @param[in] pucIpv4 Configured Robot IPv4 octets.
  * @param[in] usPort Configured Robot TCP port.
  * @param[in] ulAttempt Connection attempt number.
  */
static void prvLogRobotConnected(const uint8_t pucIpv4[4], uint16_t usPort,
	uint32_t ulAttempt);

COFFEE3_CCM_DATA
Coffee3RobotTcpStatus_t g_xCoffee3RobotTcpStatus;
COFFEE3_CCM_DATA
Coffee3RobotData_t g_xCoffee3RobotData;

COFFEE3_CCM_DATA
static StaticQueue_t s_xRobotQueueStorage;
COFFEE3_CCM_DATA
static uint8_t s_aucRobotQueueStorage[
	COFFEE3_COMMAND_QUEUE_LENGTH * sizeof(Coffee3Command_t)];
COFFEE3_CCM_DATA
static QueueHandle_t s_xRobotQueue;
/* 0=empty, 1=committed pending action, 2=reservation in progress. */
COFFEE3_CCM_DATA
static volatile uint8_t s_ucManualMotionPending;
COFFEE3_CCM_DATA
static Coffee3Command_t s_xManualMotionPending;
COFFEE3_CCM_DATA
static volatile Coffee3RobotTransaction_t s_xLastTransaction;

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
	const Coffee3Command_t *pxCommand,
	Coffee3RobotTransaction_t *pxTransaction,
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
	Coffee3RobotTransaction_t *pxTransaction, uint8_t *pucActionTimedOut);
static ModbusPortResult_e prvStartup(ModbusPort_t *pxPort,
	uint8_t ucRecoverySafe,
	Coffee3RobotStartupOutcome_e *pxOutcome);
static ModbusPortResult_e prvReconcile(ModbusPort_t *pxPort,
	Coffee3RobotTransaction_t *pxTransaction,
	uint8_t *pucDone);
static void prvBeginActionTransaction(Coffee3RobotTransaction_t *pxTransaction,
	const Coffee3Command_t *pxCommand, uint16_t usCommandCoil,
	uint16_t usResultCoil);
static ModbusPortResult_e prvSchedulePrepareRetry(
	Coffee3RobotTransaction_t *pxTransaction,
	ModbusPortResult_e xFailure);
static void prvArchiveAndResetTransaction(
	Coffee3RobotTransaction_t *pxTransaction, int32_t lTerminalResult);
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
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_LINK_CONFIRMED_LOST",
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
		ucProbeIndex < COFFEE3_ROBOT_LINK_PROBE_COUNT; ucProbeIndex++) {
		bProbe = false;
		xProbeResult = xModbusPortReadCoils(pxPort,
			COFFEE3_ROBOT_UNIT_ID, 3100U, 1U, &bProbe,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xProbeResult == MODBUS_PORT_RESULT_OK) {
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_LINK_PROBE_OK", 0,
				"attempt", (int32_t)(ucProbeIndex + 1U));
			return 0U;
		}
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_LINK_PROBE_FAILED",
			(int32_t)xProbeResult, "attempt",
			(int32_t)(ucProbeIndex + 1U));
		if ((ucProbeIndex + 1U) < COFFEE3_ROBOT_LINK_PROBE_COUNT) {
			vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_LINK_PROBE_DELAY_MS));
		}
	}
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_LINK_CONFIRMED_LOST",
		(int32_t)xResult, "attempts",
		(int32_t)COFFEE3_ROBOT_LINK_PROBE_COUNT);
	return 1U;
}

/*-----------------------------------------------------------*/
static void prvFoldServerCommands(const Coffee3Command_t *pxFirst,
	Coffee3Command_t *pxLatest)
{
	if ((pxFirst == NULL) || (pxLatest == NULL)) {
		return;
	}
	/* Server commands retain FIFO ownership.  A motion already accepted by
	 * this owner must never be silently replaced by a later manual request. */
	*pxLatest = *pxFirst;
}

/*-----------------------------------------------------------*/
static void prvDisconnectRobot(TcpClientSession_t *pxSession,
	int32_t lReason);
static void prvSetRobotReady(uint8_t ucReady);

static volatile uint8_t s_ucRobotShutdownRequested; // 请求关闭的标志
static volatile uint8_t s_ucRobotShutdownComplete;	// 关闭完成的标志

void vCoffee3RobotTcpRequestShutdown(void)
{
	s_ucRobotShutdownComplete = 0U;
	s_ucRobotShutdownRequested = 1U;
}

uint8_t ucCoffee3RobotTcpShutdownComplete(void)
{
	return s_ucRobotShutdownComplete;
}

static const uint32_t s_aulCoffee3RobotRetryDelayMs[] = {
	1000U, 2000U, 5000U, 10000U, COFFEE3_ROBOT_RETRY_MAX_MS
};

static const TcpClientSessionConfig_t s_xCoffee3RobotSessionConfig = {
	prvRobotSessionNetworkReady,
	prvRobotSessionProbe,
	prvRobotSessionEvent,
	s_aulCoffee3RobotRetryDelayMs,
	(uint8_t)(sizeof(s_aulCoffee3RobotRetryDelayMs) /
		sizeof(s_aulCoffee3RobotRetryDelayMs[0])),
	COFFEE3_ROBOT_IO_TIMEOUT_MS
};

static const uint8_t s_aucCoffee3RobotIp[4] = {
	COFFEE3_ROBOT_IP_0, COFFEE3_ROBOT_IP_1,
	COFFEE3_ROBOT_IP_2, COFFEE3_ROBOT_IP_3
};

/*-----------------------------------------------------------*/
BaseType_t xCoffee3RobotTcpInitialize(void)
{
	s_ucFreshReadyReset = 0U;
	s_ucConnectionStartupPending = 0U;
	memset(&g_xCoffee3RobotTcpStatus, 0,
		sizeof(g_xCoffee3RobotTcpStatus));
	memset(&g_xCoffee3RobotData, 0, sizeof(g_xCoffee3RobotData));
	s_ucManualMotionPending = 0U;
	memset(&s_xManualMotionPending, 0, sizeof(s_xManualMotionPending));
	{
		Coffee3RobotTransaction_t xEmptyTransaction;
		memset(&xEmptyTransaction, 0, sizeof(xEmptyTransaction));
		s_xLastTransaction = xEmptyTransaction;
	}
	s_xRobotQueue = xQueueCreateStatic(COFFEE3_COMMAND_QUEUE_LENGTH,
		sizeof(Coffee3Command_t), s_aucRobotQueueStorage,
		&s_xRobotQueueStorage);
	if (s_xRobotQueue == NULL) {
		return pdFAIL;
	}
	vCoffee3DeviceRegisterRoute(0U, s_xRobotQueue);
	return pdPASS;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3RobotTcpSubmitManualMotion(Coffee3Command_t *pxCommand)
{
	BaseType_t xResult;

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId != (uint8_t)COFFEE3_DEVICE_ROBOT) ||
		(pxCommand->ucSource != (uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) ||
		((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_DEBUG) == 0U)) {
		return pdFAIL;
	}
	/* Claim first so concurrent Server writes cannot both acquire a workflow
	 * reservation. The Robot task only consumes state 1. */
	taskENTER_CRITICAL();
	if (s_ucManualMotionPending != 0U) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	s_ucManualMotionPending = 2U;
	taskEXIT_CRITICAL();
	if (xCoffee3WorkflowAcquireDeferredManual() != pdPASS) {
		taskENTER_CRITICAL();
		s_ucManualMotionPending = 0U;
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	pxCommand->ucFlags |= COFFEE3_COMMAND_FLAG_MANUAL_RESERVED;
	taskENTER_CRITICAL();
	s_xManualMotionPending = *pxCommand;
	s_ucManualMotionPending = 1U;
	taskEXIT_CRITICAL();
	xResult = pdPASS;
	return xResult;
}

/*-----------------------------------------------------------*/
static uint8_t prvTakePendingManualMotion(Coffee3Command_t *pxCommand)
{
	if ((pxCommand == NULL) ||
		(ucCoffee3WorkflowManualDispatchAllowed() == 0U)) {
		return 0U;
	}
	taskENTER_CRITICAL();
	if (s_ucManualMotionPending != 1U) {
		taskEXIT_CRITICAL();
		return 0U;
	}
	*pxCommand = s_xManualMotionPending;
	memset(&s_xManualMotionPending, 0, sizeof(s_xManualMotionPending));
	s_ucManualMotionPending = 0U;
	taskEXIT_CRITICAL();
	return 1U;
}

/*-----------------------------------------------------------*/
static void prvLogRobotConnected(const uint8_t pucIpv4[4], uint16_t usPort,
	uint32_t ulAttempt)
{
	static const char acPrefix[] = "ROBOT_TCP_CONNECTED peer=";
	char acEvent[COFFEE3_ROBOT_CONNECTION_EVENT_LENGTH];
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
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, pcEvent, 0, "attempt",
		(int32_t)ulAttempt);
}

/*-----------------------------------------------------------*/
void vCoffee3RobotTcpTask(void *pvArgument)
{

/* Sole owner of Robot TCP, startup, recovery, and command execution. */
	/* ==================== 通信资源对象 (底层与协议层) ==================== */
	TransportChannel_t xChannel;          /* 传输层通道对象：抽象了底层的 TCP Socket 收发操作 */
	TransportTcpContext_t xTransport;     /* TCP 上下文：存储 TCP 特有的运行时状态（Socket句柄、连接状态等） */
	TransportTcpConfig_t xConfig;         /* TCP 配置参数：调用前由本任务填充（目标IP、端口、超时等），作为创建通道的输入 */
	ModbusPort_t xPort;                   /* Modbus 协议端口：将 xChannel 包装为标准 Modbus 协议接口，供业务层调用 */
	TcpClientSession_t xSession;          /* 客户端会话管理器：负责 TCP 连接的建立、保活心跳、异常断开与自动重连 */
	Coffee3RobotSessionContext_t xSessionContext; /* 自定义上下文：将 xPort 和 xTransport 的指针打包，供会话层回调时逆向寻找通信对象 */

	/* ==================== 业务数据对象 (指令与事务) ==================== */
	Coffee3Command_t xCommand;            /* 当前指令：存放从消息队列(s_xRobotQueue)最新接收到的业务指令 */
	Coffee3Command_t xDeferredCommand;    /* 暂存指令：当网络未就绪但收到指令时，先存于此，待网络恢复后优先执行 */
	Coffee3RobotTransaction_t xTransaction; /* 事务状态机：记录当前正在执行的指令进度、恢复状态、超时时间等，跨时钟周期跟踪动作 */

	/* ==================== 返回值与结果枚举 ==================== */
	TransportResult_e xTransportResult;    /* 传输层结果：捕获底层 TCP 通道创建/操作的成功或失败原因 */
	ModbusPortResult_e xResult;           /* Modbus 层结果：捕获协议交互（读写线圈、对账、启动）的执行结果 */
	Coffee3RobotStartupOutcome_e xStartupOutcome; /* 启动结果：记录机械臂唤醒过程的业务级判定（成功/失败/需重试） */

	/* ==================== 定时器与计数器 ==================== */
	TickType_t xNextStartupRetryTick;
	TickType_t xNextHealthTick;           /* 下次健康检查节拍：记录下次发送心跳包探测机械臂存活的系统时刻 */
	uint32_t ulStartupFailures;

	/* ==================== 状态机与流程控制标志 ==================== */
	uint8_t ucCreated;                    /* 资源就绪标志：底层通信积木(xChannel/xPort等)是否成功初始化。为0则任务空转 */
	uint8_t ucSessionReady;               /* 会话就绪标志：TCP已连接且机械臂处于可交互状态。为1时才允许下发业务指令 */
	uint8_t ucDone;                       /* 对账完成标志：在恢复流程中，标识之前未完成的动作是否已在机械臂侧执行完毕 */
	uint8_t ucWorkflowTransaction;        /* 事务来源标志：当前指令是否来自 Workflow。Workflow 指令通常需要更严格的恢复对账 */
	uint8_t ucReconciledSession;          /* 已对账标志：防止在单次网络恢复期间，对同一个未完成的事务进行重复对账操作 */
	uint8_t ucRecoveryWaitingLogged;       /* 恢复等待日志标志：确保在等待恢复期间，同一事务的“正在等待”日志只输出一次，防止刷屏 */
	uint8_t ucDeferredCommand;            /* 暂存有效标志：为1时表示 xDeferredCommand 里有指令，需在网络恢复后优先处理 */
	uint8_t ucSessionCommandPending;      /* 待执行命令标志：网络刚恢复且有暂存指令时置1，指示流程立即下发该指令 */
	uint8_t ucWarmAttachLogged;           /* 热挂接日志标志：确保“机械臂热挂接成功”的日志只输出一次，直到会话断开重置 */
	uint8_t ucActionTimedOut;             /* 动作超时标志：动作执行时间超过预期时置1，触发状态机进入对账或恢复流程 */
	uint8_t ucLinkProbeAttempted;         /* 链路探测已试标志：在判断链路是否彻底丢失时，标识是否已主动发过探测包 */
	uint8_t ucLinkConfirmed;              /* 链路断开确认标志：由链路探测函数填充，为1时表明网络确实断了，需断开连接并降级 */
	
	BaseType_t xCommandReceived;          /* 指令接收状态：标识当前要执行的指令是从队列新收到的，还是从暂存区取出的 */


	(void)pvArgument;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "TASK_RUNNING:C3Robot", 0);
	vAppTaskManagerWaitNetworkStackReady();
	// 1. 调用方准备输入数据（填配置）
	memset(&xConfig, 0, sizeof(xConfig));
	xConfig.xMode = TRANSPORT_TCP_MODE_CLIENT;
	memcpy(xConfig.aucRemoteIp, s_aucCoffee3RobotIp,
		sizeof(s_aucCoffee3RobotIp));
	xConfig.usPort = COFFEE3_ROBOT_PORT;
	xConfig.ulConnectTimeoutMs = COFFEE3_ROBOT_CONNECT_TIMEOUT_MS;
	xConfig.ulIoTimeoutMs = COFFEE3_ROBOT_IO_TIMEOUT_MS;
	// 2. 调用库函数，把配置变成实际对象
	xTransportResult = xTransportTcpCreate(&xChannel, &xTransport,
		"coffee3_robot_tcp", &xConfig);
	ucCreated = 0U;
	if (xTransportResult == TRANSPORT_RESULT_OK) {
		xTransport.usReserveLocalPort = prvReserveRobotPort;
		xResult = xModbusPortClientInit(&xPort, &xChannel,
			MODBUS_PORT_TRANSPORT_TCP,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		ucCreated = (xResult == MODBUS_PORT_RESULT_OK) ? 1U : 0U;
	}
	memset(&xSessionContext, 0, sizeof(xSessionContext));
	// 手动打包上下文并传给会话初始化函数
	if (ucCreated != 0U) {
		xSessionContext.pxPort = &xPort; 			 // 把 xPort 的地址存进去
		xSessionContext.pxTransport = &xTransport;  // 把 xTransport 的地址存进去
		if (xTcpClientSessionInit(&xSession,		// 用通道和上下文初始化会话
			&s_xCoffee3RobotSessionConfig, &xChannel,
			&xSessionContext) != pdPASS) {
			ucCreated = 0U;
		}
	}
	(void)xCoffee3LogWriteField(
		(ucCreated != 0U) ? COFFEE3_LOG_LEVEL_INFO :
			COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_ENDPOINT_INIT",
		(ucCreated != 0U) ? 0 : -1, "init_code",
		(ucCreated != 0U) ? 0 : (int32_t)xTransportResult);
	if (ucCreated != 0U) {
#if COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_2
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P2", 0,
			"driver", (int32_t)COFFEE3_ROBOT_LOG_DRIVER_ID);
#elif COFFEE3_ROBOT_PROTOCOL_VARIANT == COFFEE3_ROBOT_PROTOCOL_3
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P3", 0,
			"driver", (int32_t)COFFEE3_ROBOT_LOG_DRIVER_ID);
#else
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "DEVICE_PROTOCOL:DOBOT_P1", 0,
			"driver", (int32_t)COFFEE3_ROBOT_LOG_DRIVER_ID);
#endif
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "DEVICE_LINK:ROBOT_TCP", 0,
			"unit", (int32_t)COFFEE3_ROBOT_UNIT_ID);
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
	vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
	vTaskDelay(pdMS_TO_TICKS(20000U)); // 等待服务器回收完资源再进入

	for (;;) {
		if (s_ucRobotShutdownRequested != 0U) {
			(void)xTransportClose(&xChannel);
			prvSetRobotReady(0U);
			s_ucRobotShutdownRequested = 0U;
			s_ucRobotShutdownComplete = 1U;
			(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_TCP_CLOSED_FOR_RESET", 0);
			for (;;) {
				vTaskDelay(pdMS_TO_TICKS(20U));
			}
		}
		xCommandReceived = pdFAIL;
		ucSessionCommandPending = 0U;
		/* Absolute motion/acceptance budgets also run while TCP is offline. */
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.ucCommandWriteAttempted != 0U) &&
			((int32_t)(xTaskGetTickCount() -
				((xTransaction.ucAccepted != 0U) ?
				 xTransaction.xMotionDeadline : xTransaction.xAcceptDeadline)) >= 0)) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)xTransaction.xCommand.ulOrderId,
				"Robot action timeout: action=%u command_id=%lu accepted=%u; inspect robot before recovery",
				xTransaction.xCommand.usAction,
				(unsigned long)xTransaction.xCommand.ulCommandId,
				xTransaction.ucAccepted);
			vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
				MODBUS_PORT_RESULT_TIMEOUT, 1U);
			prvArchiveAndResetTransaction(&xTransaction, MODBUS_PORT_RESULT_TIMEOUT);
		}
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.xRecoveryStart != 0U) &&
			((xTaskGetTickCount() - xTransaction.xRecoveryStart) >=
				pdMS_TO_TICKS(COFFEE3_ROBOT_RECOVERY_TIMEOUT_MS))) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)xTransaction.xCommand.ulOrderId,
				"ROBOT_RECOVERY_TIMEOUT", MODBUS_PORT_RESULT_TIMEOUT,
				"command_id", (int32_t)
					xTransaction.xCommand.ulCommandId);
			vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
			vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
				MODBUS_PORT_RESULT_TIMEOUT, 1U);
			prvArchiveAndResetTransaction(&xTransaction,
				MODBUS_PORT_RESULT_TIMEOUT);
			ucRecoveryWaitingLogged = 0U;
		}
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.xRecoveryStart != 0U)) {
			if (ucCoffee3CommandIsCanceled(
				&xTransaction.xCommand) != 0U) {
				vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
				vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
					COFFEE3_COMMAND_RESULT_CANCELED, 0U);
				prvArchiveAndResetTransaction(&xTransaction,
					COFFEE3_COMMAND_RESULT_CANCELED);
				ucRecoveryWaitingLogged = 0U;
			}
		}
		if (ucCreated == 0U) {
			vTaskDelay(pdMS_TO_TICKS(100U));
			continue;
		}
		vTcpClientSessionProcess(&xSession);
		/* Body controls have a TCP-only admission boundary, even before the
		 * custom-program readiness probe or transaction recovery completes. */
		if ((g_xCoffee3RobotTcpStatus.ucConnected != 0U) &&
			(xQueuePeek(s_xRobotQueue, &xCommand, 0U) == pdPASS) &&
			(xCommand.usAction >= COFFEE3_ACTION_ROBOT_START) &&
			(xCommand.usAction <= COFFEE3_ACTION_ROBOT_MANUAL_MODE) &&
			(xQueueReceive(s_xRobotQueue, &xCommand, 0U) == pdPASS)) {
			if (xTransaction.ucActive == 0U) {
				vCoffee3DeviceCommandStarted(&xCommand);
			}
			xResult = prvExecute(&xPort, &xCommand, &xTransaction, &ucActionTimedOut);
			/* Body controls must not replace the live motion's identity or phase. */
			if (xTransaction.ucActive == 0U) {
				vCoffee3DeviceCommandCompleted(&xCommand, (int32_t)xResult,
					(xResult == MODBUS_PORT_RESULT_TIMEOUT) ? 1U : 0U);
			} else if ((xCommand.ucFlags & COFFEE3_COMMAND_FLAG_MANUAL_RESERVED) != 0U) {
				vCoffee3WorkflowReleaseManual();
			}
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)xCommand.ulOrderId,
				"Robot body control completed", (int32_t)xResult,
				"action", (int32_t)xCommand.usAction);
		}
		if (ucTcpClientSessionIsOnline(&xSession) == 0U) {
			ucSessionReady = 0U;
			ucReconciledSession = 0U;
			ucWarmAttachLogged = 0U;
			xNextStartupRetryTick = 0U;
			ulStartupFailures = 0U;
			vTaskDelay(pdMS_TO_TICKS(50U));
			continue;
		}

		if ((ucSessionReady == 0U) || (s_ucConnectionStartupPending != 0U)) {
			/* A reconnect must inspect an in-flight action before any body START
			 * sequence.  START/clear operations can corrupt evidence for a robot
			 * that is still physically moving. */
			if ((xTransaction.ucActive != 0U) &&
				(ucReconciledSession == 0U)) {
				(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT,
					"ROBOT_RECOVERY_RECONCILE", 0,
					"command_id", (int32_t)
						xTransaction.xCommand.ulCommandId);
				xResult = prvReconcile(&xPort, &xTransaction, &ucDone);
				if (xResult != MODBUS_PORT_RESULT_OK) {
					if (prvRobotLinkFailureConfirmed(&xPort, xResult,
						&ucLinkProbeAttempted) != 0U) {
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					}
					vTaskDelay(pdMS_TO_TICKS(50U));
					continue;
				}
				ucReconciledSession = 1U;
				if (ucDone != 0U) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_COMPLETED", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
						0, 0U);
					prvArchiveAndResetTransaction(&xTransaction, 0);
				} else {
					xTransaction.xRecoveryStart = 0U;
					xTransaction.ucRecovering = 0U;
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceSetRobotPhase(xTransaction.xPhase);
					/* The transaction owns this connection until it terminates. */
					s_ucConnectionStartupPending = 0U;
					ucSessionReady = 1U;
					prvSetRobotReady(prvRobotStrictReady());
				}
			}
			if ((s_ucConnectionStartupPending != 0U) &&
				(xTransaction.ucActive == 0U)) {
				if ((ulStartupFailures != 0U) &&
					((int32_t)(xTaskGetTickCount() -
					xNextStartupRetryTick) < 0)) {
					vTaskDelay(pdMS_TO_TICKS(50U));
					continue;
				}
				xResult = prvRefresh(&xPort,
					COFFEE3_ROBOT_IO_TIMEOUT_MS);
				if ((xResult == MODBUS_PORT_RESULT_OK) &&
					(prvRobotStrictReady() != 0U)) {
					s_ucConnectionStartupPending = 0U;
					ulStartupFailures = 0U;
					xNextStartupRetryTick = 0U;
					(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_STARTUP_FRESH_READY", 0);
				} else {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_STARTUP_ATTEMPT", 0,
						"attempt", (int32_t)(ulStartupFailures + 1U));
					xStartupOutcome = COFFEE3_ROBOT_STARTUP_OK;
					if (xResult == MODBUS_PORT_RESULT_OK) {
						xResult = prvStartup(&xPort,
							xTransaction.ucActive, &xStartupOutcome);
					}
					if ((xResult == MODBUS_PORT_RESULT_OK) &&
						(xStartupOutcome == COFFEE3_ROBOT_STARTUP_OK) &&
						(prvRobotStrictReady() != 0U)) {
						s_ucConnectionStartupPending = 0U;
						ulStartupFailures = 0U;
						xNextStartupRetryTick = 0U;
					} else {
						ulStartupFailures++;
						xNextStartupRetryTick = xTaskGetTickCount() +
							pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_RETRY_MS);
						if (prvRobotLinkFailureConfirmed(&xPort, xResult,
							&ucLinkProbeAttempted) != 0U) {
							prvDisconnectRobot(&xSession, (int32_t)xResult);
						} else {
							vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 1U);
							vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
							prvSetRobotReady(0U);
							(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
								COFFEE3_LOG_SOURCE_ROBOT,
								"ROBOT_STARTUP_RETRY_SCHEDULED", 0,
								"delay_ms", COFFEE3_ROBOT_STARTUP_RETRY_MS);
						}
						vTaskDelay(pdMS_TO_TICKS(50U));
						continue;
					}
				}
			}
			if ((xTransaction.ucActive == 0U) &&
				(ucDeferredCommand == 0U) &&
				(xQueuePeek(s_xRobotQueue, &xCommand, 0U) == pdPASS) &&
				(prvRobotBasicAction(xCommand.usAction) != 0U) &&
				(xQueueReceive(s_xRobotQueue, &xCommand, 0U) == pdPASS)) {
				if (xCommand.ucSource ==
					(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) {
					prvFoldServerCommands(&xCommand, &xCommand);
				}
				xDeferredCommand = xCommand;
				ucDeferredCommand = 1U;
				ucSessionCommandPending = 1U;
			}
			/* An active motion keeps queue ownership until it publishes a
			 * terminal result. Manual motion is held in its dedicated slot. */
			if ((xTransaction.ucActive != 0U) &&
				(ucReconciledSession == 0U)) {
				if (ucReconciledSession == 0U) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
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
					vCoffee3DeviceSetRecovering(
						COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceSetRobotPhase(
						xTransaction.xPhase);
					ucSessionReady = 1U;
					prvSetRobotReady(prvRobotStrictReady());
				}
				if (ucDone != 0U) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_COMPLETED", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceCommandCompleted(&xTransaction.xCommand,
						0, 0U);
					prvArchiveAndResetTransaction(&xTransaction, 0);
				}
			}
			if (xTransaction.ucActive != 0U) {
				if ((ucRecoveryWaitingLogged == 0U) &&
					(xTransaction.xRecoveryStart != 0U)) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_RECOVERY_WAITING", 0,
						"command_id", (int32_t)
							xTransaction.xCommand.ulCommandId);
					ucRecoveryWaitingLogged = 1U;
				}
				if (ucCoffee3CommandIsCanceled(
					&xTransaction.xCommand) != 0U) {
					vCoffee3DeviceSetRecovering(
						COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceCommandCompleted(
					&xTransaction.xCommand,
					COFFEE3_COMMAND_RESULT_CANCELED, 0U);
					prvArchiveAndResetTransaction(&xTransaction,
						COFFEE3_COMMAND_RESULT_CANCELED);
					ucRecoveryWaitingLogged = 0U;
				}
				if (xTransaction.ucActive != 0U) {
					vTaskDelay(pdMS_TO_TICKS(
						COFFEE3_ROBOT_ACTION_POLL_MS));
					continue;
				}
			}
			if ((xTransaction.ucActive == 0U) &&
				(prvRobotOperational() != 0U)) {
				ucSessionReady = 1U;
				if (ucWarmAttachLogged == 0U) {
					(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_WARM_ATTACH", 0,
						"state_mask", (int32_t)prvRobotStartupStateMask());
					ucWarmAttachLogged = 1U;
				}
				prvSetRobotReady(prvRobotStrictReady());
			}
			/* Session handling is available even when a manual STOP leaves
			 * the body non-running. Readiness still gates protocol motion. */
			ucSessionReady = 1U;
			prvSetRobotReady(prvRobotStrictReady());
			if (prvRobotOperational() != 0U) {
				ucSessionReady = 1U;
				prvSetRobotReady(prvRobotStrictReady());
				(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_SERVICE_READY", 0);
				g_xCoffee3RobotTcpStatus.ulConsecutiveFailures = 0U;
				g_xCoffee3RobotTcpStatus.ulNextRetryDelayMs = 0U;
			}
			if (ucSessionCommandPending != 0U) {
				ucSessionReady = 1U;
			}
			continue;
		}
		if ((xTransaction.ucActive != 0U) &&
			(xTransaction.xPhase != COFFEE3_ROBOT_PHASE_IDLE) &&
			(xTransaction.xRecoveryStart == 0U)) {
			if ((int32_t)(xTaskGetTickCount() -
				xTransaction.xNextPollTick) >= 0) {
				xTransaction.xNextPollTick = xTaskGetTickCount() +
					pdMS_TO_TICKS(COFFEE3_ROBOT_ACTION_POLL_MS);
				xResult = prvAdvanceAction(&xPort, &xTransaction,
					&ucActionTimedOut);
				if (xResult == MODBUS_PORT_RESULT_OK) {
					vCoffee3DeviceCommandCompleted(
						&xTransaction.xCommand, 0, 0U);
					prvArchiveAndResetTransaction(&xTransaction, 0);
				} else if (xResult == MODBUS_PORT_RESULT_CANCELED) {
					vCoffee3DeviceCommandCompleted(
						&xTransaction.xCommand,
						COFFEE3_COMMAND_RESULT_CANCELED, 0U);
					prvArchiveAndResetTransaction(&xTransaction,
						COFFEE3_COMMAND_RESULT_CANCELED);
				} else if (ucActionTimedOut != 0U) {
					vCoffee3DeviceCommandCompleted(
						&xTransaction.xCommand,
						MODBUS_PORT_RESULT_TIMEOUT, 1U);
					prvArchiveAndResetTransaction(&xTransaction,
						MODBUS_PORT_RESULT_TIMEOUT);
				} else if (xResult != MODBUS_PORT_RESULT_BUSY) {
					ucLinkConfirmed = prvRobotLinkFailureConfirmed(
						&xPort, xResult, &ucLinkProbeAttempted);
					if (ucLinkConfirmed != 0U) {
						xTransaction.xRecoveryStart =
							xTaskGetTickCount();
						xTransaction.ucRecovering = 1U;
						xTransaction.xPhase =
							COFFEE3_ROBOT_PHASE_RECOVERING;
						vCoffee3DeviceSetRecovering(
							COFFEE3_DEVICE_ROBOT, 1U);
						vCoffee3DeviceSetRobotPhase(
							COFFEE3_ROBOT_PHASE_RECOVERING);
						ucSessionReady = 0U;
						prvDisconnectRobot(&xSession, (int32_t)xResult);
					} else if ((xResult == MODBUS_PORT_RESULT_TIMEOUT) ||
						(xResult == MODBUS_PORT_RESULT_PROTOCOL) ||
						(ucLinkProbeAttempted != 0U)) {
						xTransaction.xRecoveryStart =
							xTaskGetTickCount();
						xTransaction.ucRecovering = 1U;
						xTransaction.xPhase =
							COFFEE3_ROBOT_PHASE_RECOVERING;
						vCoffee3DeviceSetRecovering(
							COFFEE3_DEVICE_ROBOT, 1U);
						vCoffee3DeviceSetRobotPhase(
							COFFEE3_ROBOT_PHASE_RECOVERING);
						ucSessionReady = 0U;
					} else {
						vCoffee3DeviceCommandCompleted(
							&xTransaction.xCommand,
							(int32_t)xResult, 0U);
						prvArchiveAndResetTransaction(&xTransaction,
							(int32_t)xResult);
					}
				}
			}
			if (xTransaction.ucActive != 0U) {
				vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_ACTION_POLL_MS));
				continue;
			}
		}

		if ((xTransaction.ucActive == 0U) && (ucDeferredCommand == 0U) &&
			(prvTakePendingManualMotion(&xCommand) != 0U)) {
			xCommandReceived = pdPASS;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, COFFEE3_LOG_ORDER_DEBUG,
				"MANUAL_ROBOT_DISPATCH", 0,
				"action", (int32_t)xCommand.usAction);
		}
		if (ucDeferredCommand != 0U) {
			xCommand = xDeferredCommand;
			ucDeferredCommand = 0U;
			xCommandReceived = pdPASS;
		}
		if ((xCommandReceived == pdPASS) ||
			(xQueueReceive(s_xRobotQueue, &xCommand,
			pdMS_TO_TICKS(COFFEE3_ROBOT_LOOP_MS)) == pdPASS)) {
			if ((xCommandReceived == pdFAIL) &&
				(xCommand.ucSource ==
					(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER)) {
				prvFoldServerCommands(&xCommand, &xCommand);
			}
			vCoffee3DeviceCommandStarted(&xCommand);
			if (xCommand.usAction == COFFEE3_ACTION_ROBOT_PREPARE_ORDER) {
				xResult = MODBUS_PORT_RESULT_CANCELED;
				if ((xCommand.ucSource == COFFEE3_COMMAND_SOURCE_WORKFLOW) &&
					(ucCoffee3CommandIsCanceled(&xCommand) == 0U)) {
					xResult = prvRefresh(&xPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
					if ((xResult == MODBUS_PORT_RESULT_OK) &&
						((prvRobotOperational() == 0U) ||
						 (g_xCoffee3RobotData.aucBaseInputs[DOBOT_ROBOT_BODY_STATUS_POWERED] == 0U))) {
						xStartupOutcome = COFFEE3_ROBOT_STARTUP_OK;
						(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
							COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)xCommand.ulOrderId,
							"Order requires robot startup: power/enable/run/alarm state not suitable");
						xResult = prvStartup(&xPort, 0U, &xStartupOutcome);
						if ((xResult == MODBUS_PORT_RESULT_OK) &&
							(xStartupOutcome != COFFEE3_ROBOT_STARTUP_OK)) {
							xResult = MODBUS_PORT_RESULT_NOT_READY;
						}
					}
					if ((xResult == MODBUS_PORT_RESULT_OK) &&
						((prvRobotStrictReady() == 0U) ||
						 (g_xCoffee3RobotData.aucBaseInputs[DOBOT_ROBOT_BODY_STATUS_POWERED] == 0U))) {
						xResult = MODBUS_PORT_RESULT_NOT_READY;
					}
				}
				if (ucCoffee3CommandIsCanceled(&xCommand) != 0U) {
					xResult = MODBUS_PORT_RESULT_CANCELED;
				}
				prvSetRobotReady(prvRobotStrictReady());
				(void)xCoffee3LogPrintfOrder(
					(xResult == MODBUS_PORT_RESULT_OK) ? COFFEE3_LOG_LEVEL_INFO :
					COFFEE3_LOG_LEVEL_WARNING, COFFEE3_LOG_SOURCE_ROBOT,
					(uint16_t)xCommand.ulOrderId,
					"Order robot preparation finished: result=%ld", (long)xResult);
				vCoffee3DeviceCommandCompleted(&xCommand, (int32_t)xResult, 0U);
				continue;
			}
			ucWorkflowTransaction =
				(xCommand.ucSource ==
					(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW) ? 1U : 0U;
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)xCommand.ulOrderId,
				"ROBOT_ACTION_START=%s",
				prvRobotActionName(xCommand.usAction));
			ucActionTimedOut = 0U;
			xResult = prvExecute(&xPort, &xCommand,
			&xTransaction, &ucActionTimedOut);
			g_xCoffee3RobotTcpStatus.lLastResult = (int32_t)xResult;
			g_xCoffee3RobotTcpStatus.ulCommandCount++;
			if (xResult != MODBUS_PORT_RESULT_OK) {
				g_xCoffee3RobotTcpStatus.ulErrorCount++;
			}
			ucLinkConfirmed = 0U;
			if ((xResult == MODBUS_PORT_RESULT_BUSY) &&
				(xTransaction.ucActive != 0U)) {
				/* Position actions advance from the owner loop. */
			} else if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee3DeviceCommandCompleted(&xCommand, 0, 0U);
				prvArchiveAndResetTransaction(&xTransaction, 0);
				if ((prvRobotBasicAction(xCommand.usAction) != 0U) &&
					(prvRobotOperational() == 0U)) {
					ucSessionReady = 0U;
				}
			} else if ((ucActionTimedOut != 0U) &&
				(ucWorkflowTransaction != 0U)) {
				if (xTransaction.ucRecovering == 0U) {
					(void)xCoffee3LogWriteFieldOrder(
						COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_ROBOT,
						(uint16_t)xCommand.ulOrderId,
						"ROBOT_ACTION_TIMEOUT_RECONCILE",
						MODBUS_PORT_RESULT_TIMEOUT, "command_id",
						(int32_t)xCommand.ulCommandId);
				}
				if (xTransaction.xRecoveryStart == 0U) {
					xTransaction.xRecoveryStart = xTaskGetTickCount();
				}
				xTransaction.ucRecovering = 1U;
				vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 1U);
				ucSessionReady = 0U;
			} else if ((ucWorkflowTransaction != 0U) ||
				(xTransaction.ucActive != 0U)) {
				ucLinkConfirmed = prvRobotLinkFailureConfirmed(&xPort,
					xResult, &ucLinkProbeAttempted);
				if (ucLinkConfirmed != 0U) {
					if (xTransaction.xRecoveryStart == 0U) {
						xTransaction.xRecoveryStart = xTaskGetTickCount();
					}
					xTransaction.ucRecovering = 1U;
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 1U);
					(void)xCoffee3LogWriteField(
						COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_ROBOT,
						"ROBOT_COMMAND_LINK_LOST", (int32_t)xResult,
						"command_id", (int32_t)xCommand.ulCommandId);
					vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
					ucSessionReady = 0U;
					prvDisconnectRobot(&xSession, (int32_t)xResult);
				} else if ((xResult == MODBUS_PORT_RESULT_TIMEOUT) ||
					(xResult == MODBUS_PORT_RESULT_PROTOCOL) ||
					(ucLinkProbeAttempted != 0U)) {
					if (xTransaction.xRecoveryStart == 0U) {
						xTransaction.xRecoveryStart = xTaskGetTickCount();
						(void)xCoffee3LogWriteField(
							COFFEE3_LOG_LEVEL_WARNING,
							COFFEE3_LOG_SOURCE_ROBOT,
							"ROBOT_COMMAND_RECONCILE", (int32_t)xResult,
							"command_id", (int32_t)xCommand.ulCommandId);
					}
					xTransaction.ucRecovering = 1U;
					vCoffee3DeviceSetRecovering(COFFEE3_DEVICE_ROBOT, 1U);
					ucSessionReady = 0U;
				} else {
					vCoffee3DeviceCommandCompleted(&xCommand,
						(int32_t)xResult, 0U);
					prvArchiveAndResetTransaction(&xTransaction,
						(int32_t)xResult);
				}
			} else {
				vCoffee3DeviceCommandCompleted(&xCommand,
					(int32_t)xResult,
					(ucActionTimedOut != 0U) ? 1U : 0U);
					prvArchiveAndResetTransaction(&xTransaction,
						(int32_t)xResult);
				if ((prvRobotBasicAction(xCommand.usAction) != 0U) &&
					(prvRobotOperational() == 0U)) {
					ucSessionReady = 0U;
				}
				if (ucActionTimedOut == 0U) {
					ucLinkConfirmed = prvRobotLinkFailureConfirmed(&xPort,
						xResult, &ucLinkProbeAttempted);
				}
				if (ucLinkConfirmed != 0U) {
					vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
					vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
					ucSessionReady = 0U;
					prvDisconnectRobot(&xSession, (int32_t)xResult);
				}
			}
		} else if ((int32_t)(xTaskGetTickCount() -
			xNextHealthTick) >= 0) {
			xResult = prvRefresh(&xPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
			g_xCoffee3RobotTcpStatus.lLastResult = (int32_t)xResult;
			if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 1U);
				vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT,
					prvRobotStrictReady());
				prvSetRobotReady(prvRobotStrictReady());
			} else if (prvRobotLinkFailureConfirmed(&xPort, xResult,
				&ucLinkProbeAttempted) != 0U) {
				vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
				vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
				prvSetRobotReady(0U);
				ucSessionReady = 0U;
				prvDisconnectRobot(&xSession, (int32_t)xResult);
			}
			xNextHealthTick = xTaskGetTickCount() + pdMS_TO_TICKS(
				COFFEE3_ROBOT_HEALTH_MS);
		}
	}
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvRefresh(ModbusPort_t *pxPort,
	uint32_t ulTimeoutMs)
{
	/* Refresh the controller snapshot before readiness and motion decisions. */
	const DobotRobotProtocolConfig_t *pxProtocol;
	ModbusPortResult_e xResult;
	bool abBaseValues[DOBOT_ROBOT_BASE_INPUT_COUNT];
	bool abValues[COFFEE3_ROBOT_CONTROL_COIL_COUNT];
	uint8_t ucIndex;

	pxProtocol = prvCoffee3DobotConfig()->pxProtocol;
	xResult = xModbusPortReadDiscreteInputs(pxPort,
		COFFEE3_ROBOT_UNIT_ID, pxProtocol->usBaseInputStart,
		pxProtocol->usBaseInputCount, abBaseValues, ulTimeoutMs);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		for (ucIndex = 0U;
			ucIndex < pxProtocol->usBaseInputCount; ucIndex++) {
			g_xCoffee3RobotData.aucBaseInputs[ucIndex] =
				abBaseValues[ucIndex] ? 1U : 0U;
		}
		xResult = xModbusPortReadCoils(pxPort,
			COFFEE3_ROBOT_UNIT_ID, pxProtocol->usControlStart,
			pxProtocol->usControlCount,
			abValues, ulTimeoutMs);
	}
	if (xResult == MODBUS_PORT_RESULT_OK) {
		for (ucIndex = 0U;
			ucIndex < pxProtocol->usControlCount; ucIndex++) {
			g_xCoffee3RobotData.aucControlCoils[ucIndex] =
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
	Coffee3RobotSessionContext_t *pxContext;
	ModbusPortResult_e xResult;

	pxContext = (Coffee3RobotSessionContext_t *)pvOwnerContext;
	if ((pxContext == NULL) || (pxContext->pxPort == NULL)) {
		return (int32_t)MODBUS_PORT_RESULT_INVALID_ARG;
	}
	/* TCP reachability is independent of program RUNNING/3100 readiness. */
	xResult = xModbusPortWriteCoil(pxContext->pxPort,
		COFFEE3_ROBOT_UNIT_ID, 3100U, false, ulTimeoutMs);
	s_ucFreshReadyReset = (xResult == MODBUS_PORT_RESULT_OK) ? 1U : 0U;
	g_xCoffee3RobotData.aucControlCoils[0U] = 0U;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO, COFFEE3_LOG_SOURCE_ROBOT,
		"Reconnect: clear 3100; wait for fresh program ready signal", (int32_t)xResult);
	return (int32_t)xResult;
}

/*-----------------------------------------------------------*/
static void prvRobotSessionEvent(void *pvOwnerContext,
	TcpClientSessionState_e xPreviousState,
	TcpClientSessionState_e xCurrentState, int32_t lReason,
	uint32_t ulAttempt, uint32_t ulRetryDelayMs)
{
	Coffee3RobotSessionContext_t *pxContext;
	ModbusPortFault_t xFault;
	int32_t lNativeError;

	pxContext = (Coffee3RobotSessionContext_t *)pvOwnerContext;
	lNativeError = 0;
	memset(&xFault, 0, sizeof(xFault));
	if ((pxContext != NULL) && (pxContext->pxTransport != NULL)) {
		lNativeError = pxContext->pxTransport->lLastNativeError;
	}
	if ((pxContext != NULL) && (pxContext->pxPort != NULL)) {
		vModbusPortGetLastFault(pxContext->pxPort, &xFault);
	}
	g_xCoffee3RobotTcpStatus.ulConnectAttemptCount = ulAttempt;
	g_xCoffee3RobotTcpStatus.lLastResult = lReason;

	if ((xPreviousState == TCP_CLIENT_SESSION_NETWORK_WAIT) &&
		(xCurrentState == TCP_CLIENT_SESSION_BACKOFF)) {
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_BACKOFF) &&
		(xCurrentState == TCP_CLIENT_SESSION_CONNECTING)) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_BEGIN", 0,
			"attempt", (int32_t)ulAttempt);
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_CONNECTING) &&
		(xCurrentState == TCP_CLIENT_SESSION_PROTOCOL_CHECK)) {
		g_xCoffee3RobotTcpStatus.ucConnected = 1U;
		s_ucConnectionStartupPending = 1U;
		s_ucFreshReadyReset = 0U;
		g_xCoffee3RobotTcpStatus.ulConnectSuccessCount++;
		prvSetRobotReady(0U);
		prvLogRobotConnected(s_aucCoffee3RobotIp, COFFEE3_ROBOT_PORT,
			ulAttempt);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_BEGIN", 0,
			"timeout_ms", (int32_t)COFFEE3_ROBOT_IO_TIMEOUT_MS);
		return;
	}
	if ((xPreviousState == TCP_CLIENT_SESSION_PROTOCOL_CHECK) &&
		(xCurrentState == TCP_CLIENT_SESSION_ONLINE)) {
		g_xCoffee3RobotTcpStatus.ulConsecutiveFailures = 0U;
		g_xCoffee3RobotTcpStatus.ulNextRetryDelayMs = 0U;
		vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 1U);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_OK", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		return;
	}
	if (xCurrentState == TCP_CLIENT_SESSION_BACKOFF) {
		s_ucConnectionStartupPending = 0U;
		s_ucFreshReadyReset = 0U;
		g_xCoffee3RobotTcpStatus.ulErrorCount++;
		g_xCoffee3RobotTcpStatus.ulConsecutiveFailures++;
		g_xCoffee3RobotTcpStatus.ulNextRetryDelayMs = ulRetryDelayMs;
		if (g_xCoffee3RobotTcpStatus.ucConnected != 0U) {
			g_xCoffee3RobotTcpStatus.ulDisconnectCount++;
		}
		g_xCoffee3RobotTcpStatus.ucConnected = 0U;
		prvSetRobotReady(0U);
		vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
		vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
		if (xPreviousState == TCP_CLIENT_SESSION_CONNECTING) {
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_FAILED", lReason,
				"native_error", lNativeError);
			if (lReason == (int32_t)TRANSPORT_RESULT_TIMEOUT) {
				(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
					COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_CONNECT_TIMEOUT",
					lReason, "attempt", (int32_t)ulAttempt);
			}
		} else if (xPreviousState ==
			TCP_CLIENT_SESSION_PROTOCOL_CHECK) {
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_CHECK_FAILED",
				lReason, "transport_result",
				(int32_t)xFault.xTransportResult);
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_PROTOCOL",
				lReason, "protocol_code", xFault.lProtocolCode);
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_EXCEPTION",
				lReason, "exception_code", (int32_t)xFault.ucExceptionCode);
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_PROTOCOL_FAULT_NATIVE",
				lReason, "native_error", xFault.lNativeError);
		} else if (xPreviousState == TCP_CLIENT_SESSION_ONLINE) {
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_LINK_LOST", lReason,
				"attempt", (int32_t)ulAttempt);
		}
		if (lNativeError != 0) {
			vCoffee3LogLwipResourceFailure(COFFEE3_LOG_SOURCE_ROBOT,
				lNativeError);
		}
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_RETRY_SCHEDULED", lReason,
			"delay_ms", (int32_t)ulRetryDelayMs);
		return;
	}
	if (xCurrentState == TCP_CLIENT_SESSION_NETWORK_WAIT) {
		s_ucConnectionStartupPending = 0U;
		s_ucFreshReadyReset = 0U;
		if (g_xCoffee3RobotTcpStatus.ucConnected != 0U) {
			g_xCoffee3RobotTcpStatus.ulDisconnectCount++;
		}
		g_xCoffee3RobotTcpStatus.ucConnected = 0U;
		prvSetRobotReady(0U);
		vCoffee3DeviceSetOnline(COFFEE3_DEVICE_ROBOT, 0U);
		vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, 0U);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_NETWORK_WAITING", lReason,
			"attempt", (int32_t)ulAttempt);
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotOperational(void)
{
	/* READY/IDLE are availability states; motion is authorized only when the
	 * robot positively reports RUNNING, matching the physical safety rule. */
	if ((g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ENABLED] == 0U) ||
		(g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ALARM] != 0U)) {
		return 0U;
	}
	return (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotBasicAction(uint16_t usAction)
{
	return (((usAction >= (uint16_t)COFFEE3_ACTION_ROBOT_START) &&
		(usAction <= (uint16_t)COFFEE3_ACTION_ROBOT_MANUAL_MODE)) ||
		(usAction == (uint16_t)COFFEE3_ACTION_CANCEL) ||
		(usAction == (uint16_t)COFFEE3_ACTION_RESET)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotStrictReady(void)
{
	uint8_t ucReady;
	uint8_t ucRunning;

	ucReady = g_xCoffee3RobotData.aucControlCoils[0U];
	ucRunning = g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RUNNING];
	if ((s_ucFreshReadyReset == 0U) || (ucReady == 0U) || (ucRunning == 0U) ||
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
	ucPrevious = g_xCoffee3RobotTcpStatus.ucReady;
	if (ucPrevious == ucReady) {
		vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, ucReady);
		return;
	}
	g_xCoffee3RobotTcpStatus.ucReady = ucReady;
	vCoffee3DeviceSetReady(COFFEE3_DEVICE_ROBOT, ucReady);
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT,
		(ucReady != 0U) ? "ROBOT_READY_RISE" : "ROBOT_READY_FALL",
		(int32_t)ucReady, "previous", (int32_t)ucPrevious);
}

/*-----------------------------------------------------------*/
static uint16_t prvRobotStartupStateMask(void)
{
	uint16_t usMask;

	usMask = 0U;
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_DRAG] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_DRAG;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_POWERED] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_POWER;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ENABLED] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_ENABLE;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_ALARM] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_ALARM;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_COLLISION] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_COLLISION;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_SAFETY_PAUSED] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_SAFETY;
	}
	if (g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RECOVERY] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_RECOVERY;
	}
	if (g_xCoffee3RobotData.aucControlCoils[0U] != 0U) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_READY;
	}
	if ((g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U) ||
		(g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_IDLE] != 0U)) {
		usMask |= COFFEE3_ROBOT_STARTUP_STATE_MOTION;
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
	pxProtocol = prvCoffee3DobotConfig()->pxProtocol;
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
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, usOrderId,
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
	xResult = xModbusPortWriteCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usStart, usCount, abClear, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usStart, usCount, abRead, COFFEE3_ROBOT_IO_TIMEOUT_MS);
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
	return xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usCoil, bValue, ulTimeoutMs);
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvStartup(ModbusPort_t *pxPort,
	uint8_t ucRecoverySafe,
	Coffee3RobotStartupOutcome_e *pxOutcome)
{
	/* Clear stale actions, clear alarms, enable, start, then require RUNNING. */
	bool abBaseClear[10];
	uint8_t ucIndex;
	ModbusPortResult_e xResult;
	uint8_t ucOperational;
	uint8_t ucPowerObserved;
	TickType_t xWaitStartTick;

	*pxOutcome = COFFEE3_ROBOT_STARTUP_OK;
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_BEGIN",
		(int32_t)ucRecoverySafe, "state_mask",
		(int32_t)prvRobotStartupStateMask());
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 0,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	for (ucIndex = 0U; ucIndex < 10U; ucIndex++) {
		abBaseClear[ucIndex] = false;
	}
	xResult = xModbusPortWriteCoils(pxPort,
		COFFEE3_ROBOT_UNIT_ID, 0U, 10U, abBaseClear,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 1,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 2,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	if (ucRecoverySafe == 0U) {
		xResult = prvWriteRisingEdge(pxPort,
			DOBOT_ROBOT_BODY_COMMAND_STOP,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 3,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteControlValue(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM, false,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 4,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_CLEAR_ALARM,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 5,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_EXIT_DRAG,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 6,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_ENABLE,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	if (ucRecoverySafe == 0U) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 7,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		xResult = prvWriteRisingEdge(pxPort,
			DOBOT_ROBOT_BODY_COMMAND_STOP,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_STEP", 8,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	xResult = prvWriteRisingEdge(pxPort,
		DOBOT_ROBOT_BODY_COMMAND_START,
		COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_STEP_DELAY_MS));
	xWaitStartTick = xTaskGetTickCount();
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_WAIT", 0,
		"timeout_ms", (int32_t)COFFEE3_ROBOT_STARTUP_FINAL_WAIT_MS);
	for (;;) {
		xResult = prvRefresh(pxPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		ucOperational = ((g_xCoffee3RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_ENABLED] != 0U) &&
			(g_xCoffee3RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_ALARM] == 0U) &&
			(g_xCoffee3RobotData.aucBaseInputs[
			DOBOT_ROBOT_BODY_STATUS_RUNNING] != 0U)) ? 1U : 0U;
		if (ucOperational != 0U) {
			break;
		}
		if ((xTaskGetTickCount() - xWaitStartTick) >= pdMS_TO_TICKS(
			COFFEE3_ROBOT_STARTUP_FINAL_WAIT_MS)) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_STARTUP_POLL_MS));
	}
	ucPowerObserved = g_xCoffee3RobotData.aucBaseInputs[
		DOBOT_ROBOT_BODY_STATUS_POWERED];
	if (ucOperational == 0U) {
		*pxOutcome = COFFEE3_ROBOT_STARTUP_NOT_READY;
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_NOT_READY", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucPowerObserved == 0U) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT,
			"ROBOT_POWER_SIGNAL_NOT_OBSERVED", 0,
			"state_mask", (int32_t)prvRobotStartupStateMask());
	}
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "ROBOT_STARTUP_DONE", 0,
		"state_mask", (int32_t)prvRobotStartupStateMask());
	return MODBUS_PORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static void prvBeginActionTransaction(Coffee3RobotTransaction_t *pxTransaction,
	const Coffee3Command_t *pxCommand, uint16_t usCommandCoil,
	uint16_t usResultCoil)
{
	TickType_t xNow;

	if ((pxTransaction == NULL) || (pxCommand == NULL) ||
		(pxTransaction->ucActive != 0U)) {
		return;
	}
	xNow = xTaskGetTickCount();
	memset(pxTransaction, 0, sizeof(*pxTransaction));
	pxTransaction->xCommand = *pxCommand;
	pxTransaction->usCommandCoil = usCommandCoil;
	pxTransaction->usResultCoil = usResultCoil;
	pxTransaction->xAcceptDeadline = 0U;
	pxTransaction->xNextPollTick = xNow;
	pxTransaction->ucActive = 1U;
	pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
	vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
	vCoffee3DeviceSetRobotAccepted(0U);
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvSchedulePrepareRetry(
	Coffee3RobotTransaction_t *pxTransaction,
	ModbusPortResult_e xFailure)
{
	if ((pxTransaction == NULL) || (pxTransaction->ucActive == 0U) ||
		(pxTransaction->ucCommandWriteAttempted != 0U)) {
		return xFailure;
	}
	if (pxTransaction->ucPrepareRetryCount >=
		COFFEE3_ROBOT_PREPARE_RETRY_LIMIT) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_PREPARE_RETRY_EXHAUSTED", (int32_t)xFailure,
			"action", (int32_t)pxTransaction->xCommand.usAction);
		return MODBUS_PORT_RESULT_TIMEOUT;
	}
	pxTransaction->ucPrepareRetryCount++;
	pxTransaction->xNextPrepareRetryTick = xTaskGetTickCount() +
		pdMS_TO_TICKS(COFFEE3_ROBOT_PREPARE_RETRY_MS);
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_ROBOT,
		(uint16_t)pxTransaction->xCommand.ulOrderId,
		"ROBOT_ACTION_PREPARE_RETRY", (int32_t)xFailure,
		"retry", (int32_t)pxTransaction->ucPrepareRetryCount);
	return MODBUS_PORT_RESULT_BUSY;
}

/*-----------------------------------------------------------*/
static void prvArchiveAndResetTransaction(
	Coffee3RobotTransaction_t *pxTransaction, int32_t lTerminalResult)
{
	if (pxTransaction == NULL) {
		return;
	}
	if (pxTransaction->ucActive != 0U) {
		pxTransaction->ucTerminalLogged = 1U;
		pxTransaction->lTerminalResult = lTerminalResult;
		s_xLastTransaction = *pxTransaction;
	}
	memset(pxTransaction, 0, sizeof(*pxTransaction));
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvReconcile(ModbusPort_t *pxPort,
	Coffee3RobotTransaction_t *pxTransaction,
	uint8_t *pucDone)
{
	/* Re-read command/result state after TCP loss to avoid duplicate motion. */
	const DobotRobotProtocolConfig_t *pxProtocol;
	uint32_t ulControlEnd;
	uint8_t ucCommand;
	uint8_t ucResult;
	bool bResult;
	ModbusPortResult_e xResult;

	*pucDone = 0U;
	/* No action was published: only the preparation retry path may proceed. */
	if (pxTransaction->ucCommandWriteAttempted == 0U) {
		return MODBUS_PORT_RESULT_OK;
	}
	if (pxTransaction->usCommandCoil == 0xFFFFU) {
		return MODBUS_PORT_RESULT_OK;
	}
	pxProtocol = prvCoffee3DobotConfig()->pxProtocol;
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
	xResult = prvRefresh(pxPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return xResult;
	}
	ucCommand = g_xCoffee3RobotData.aucControlCoils[
		pxTransaction->usCommandCoil - pxProtocol->usControlStart];
	ucResult = g_xCoffee3RobotData.aucControlCoils[
		pxTransaction->usResultCoil - pxProtocol->usControlStart];
	if ((ucCommand != 0U) && (ucResult != 0U) &&
		(pxTransaction->ucAccepted == 0U)) {
		pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
		vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
		if (pxTransaction->ucResultWhileCommandHigh == 0U) {
			pxTransaction->ucResultWhileCommandHigh = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_PROTOCOL_SEQUENCE",
			MODBUS_PORT_RESULT_PROTOCOL, "action",
			(int32_t)pxTransaction->xCommand.usAction);
		}
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucCommand != 0U) {
		pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
		vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
		return MODBUS_PORT_RESULT_OK;
	}
	if (ucCommand == 0U) {
		if ((ucResult == 0U) &&
			(pxTransaction->ucCompletionObserved != 0U)) {
			/* The result acknowledgement may have reached Robot before TCP
			 * failed.  Completion evidence is latched locally first. */
			*pucDone = 1U;
			return MODBUS_PORT_RESULT_OK;
		}
		if ((ucResult == 0U) && (pxTransaction->ucAccepted == 0U)) {
			/* A cleared command without a previous local acceptance observation
			 * is ambiguous.  Keep the original absolute acceptance deadline;
			 * never invent acceptance and never publish the action again. */
			pxTransaction->ucAmbiguous = 1U;
			pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
			vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
			return MODBUS_PORT_RESULT_OK;
		}
		if (pxTransaction->ucAccepted == 0U) {
			pxTransaction->ucAccepted = 1U;
			pxTransaction->xAcceptedTick = xTaskGetTickCount();
			pxTransaction->xMotionDeadline = pxTransaction->xAcceptedTick +
				pdMS_TO_TICKS(COFFEE3_ROBOT_MOTION_TIMEOUT_MS);
			vCoffee3DeviceSetRobotAccepted(1U);
			vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_MOVING);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_ACCEPTED", 0,
				"coil", (int32_t)pxTransaction->usCommandCoil);
		}
		if (ucResult != 0U) {
			pxTransaction->ucCompletionObserved = 1U;
			pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_CLEAR_RESULT;
			xResult = xModbusPortWriteCoil(pxPort,
				COFFEE3_ROBOT_UNIT_ID, pxTransaction->usResultCoil,
				false, COFFEE3_ROBOT_IO_TIMEOUT_MS);
			if (xResult != MODBUS_PORT_RESULT_OK) {
				return xResult;
			}
			xResult = xModbusPortReadCoils(pxPort,
				COFFEE3_ROBOT_UNIT_ID, pxTransaction->usResultCoil,
				1U, &bResult, COFFEE3_ROBOT_IO_TIMEOUT_MS);
			if (xResult != MODBUS_PORT_RESULT_OK) {
				return xResult;
			}
			if (bResult) {
				return MODBUS_PORT_RESULT_PROTOCOL;
			}
			*pucDone = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_RESULT_CLEARED", 0,
				"coil", (int32_t)pxTransaction->usResultCoil);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_COMPLETE", 0,
				"action", (int32_t)pxTransaction->xCommand.usAction);
			return MODBUS_PORT_RESULT_OK;
		}
		pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_MOVING;
		vCoffee3DeviceSetRobotPhase(COFFEE3_ROBOT_PHASE_MOVING);
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
	const Coffee3Command_t *pxCommand,
	Coffee3RobotTransaction_t *pxTransaction,
	uint8_t *pucActionTimedOut)
{
	Coffee3DobotCommand_t xResolvedCommand;
	ModbusPortResult_e xResult;
	bool bResult;
	uint16_t usCommandCoil;
	uint16_t usResultCoil;
	uint8_t ucActionResolved;
	uint8_t ucStrictReady;

	if (pucActionTimedOut != NULL) {
		*pucActionTimedOut = 0U;
	}

	if (pxCommand->usAction == COFFEE3_ACTION_REFRESH) {
		return prvRefresh(pxPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	}
	/* Body controls must work while the custom protocol program is stopped. */
	if ((prvResolveCoffee3Dobot(pxCommand, &xResolvedCommand) != 0U) &&
		(xResolvedCommand.xKind == COFFEE3_DOBOT_COMMAND_BODY)) {
		return prvWriteRisingEdge(pxPort, xResolvedCommand.xBodyCommand,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
	}
	ucActionResolved = 0U;
	if (prvResolveCoffee3Dobot(
		pxCommand, &xResolvedCommand) != 0U) {
		if (xResolvedCommand.xKind == COFFEE3_DOBOT_COMMAND_ACTION) {
			usCommandCoil = xResolvedCommand.usCommandCoil;
			usResultCoil = xResolvedCommand.usResultCoil;
			ucActionResolved = 1U;
		}
	}
	if (ucActionResolved != 0U) {
		prvBeginActionTransaction(pxTransaction, pxCommand, usCommandCoil,
			usResultCoil);
	}
	xResult = prvRefresh(pxPort, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return prvSchedulePrepareRetry(pxTransaction, xResult);
	}
	ucStrictReady = prvRobotStrictReady();
	if (ucStrictReady == 0U) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
			"ROBOT_COMMAND_NOT_READY",
			MODBUS_PORT_RESULT_PROTOCOL, "action",
			(int32_t)pxCommand->usAction);
		return prvSchedulePrepareRetry(pxTransaction, MODBUS_PORT_RESULT_PROTOCOL);
	}
	if (ucActionResolved == 0U) {
		if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_START_SIGNAL) {
			return prvWriteControlValue(pxPort, DOBOT_ROBOT_P1_COMMAND_START_SIGNAL,
				(pxCommand->ausParameter[0U] != 0U), COFFEE3_ROBOT_IO_TIMEOUT_MS);
		}
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;
	}
	if (pxCommand->usAction == COFFEE3_ACTION_ROBOT_PUT_OUTPUT) {
		vCoffee3ServerSelectOutlet();
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxCommand->ulOrderId,
			"ROBOT_OUTPUT_ROUTE_PUBLISHED output=1 route_reg=0x000A selector_reg=0x0033");
	}
	if ((pxCommand->usAction == COFFEE3_ACTION_ROBOT_PUT_STORAGE) ||
		(pxCommand->usAction == COFFEE3_ACTION_ROBOT_TAKE_STORAGE)) {
		if ((pxCommand->ausParameter[0] < 1U) ||
			(pxCommand->ausParameter[0] > 2U)) {
			return MODBUS_PORT_RESULT_INVALID_ARG;
		}
		vCoffee3ServerSelectStorage(pxCommand->ausParameter[0]);
	}
	xResult = prvClearActionCoils(pxPort, NULL,
		(uint16_t)pxCommand->ulOrderId);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return prvSchedulePrepareRetry(pxTransaction, xResult);
	}
	xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usResultCoil, 1U, &bResult, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult != MODBUS_PORT_RESULT_OK) {
		return prvSchedulePrepareRetry(pxTransaction, xResult);
	}
	if (bResult) {
		xResult = xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
			usResultCoil, false, COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return prvSchedulePrepareRetry(pxTransaction, xResult);
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			usResultCoil, 1U, &bResult, COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return prvSchedulePrepareRetry(pxTransaction, xResult);
		}
		if (bResult) {
			return prvSchedulePrepareRetry(pxTransaction,
				MODBUS_PORT_RESULT_PROTOCOL);
		}
	}
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
		"ROBOT_ACTION_PREPARED", 0,
		"coil", (int32_t)usCommandCoil);
	if (pxTransaction != NULL) {
		pxTransaction->ucCommandWriteAttempted = 1U;
		pxTransaction->xAcceptDeadline = xTaskGetTickCount() +
			pdMS_TO_TICKS(COFFEE3_ROBOT_ACCEPT_TIMEOUT_MS);
	}
	xResult = xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usCommandCoil, true, COFFEE3_ROBOT_IO_TIMEOUT_MS);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT, (uint16_t)pxCommand->ulOrderId,
			"ROBOT_ACTION_SENT", 0,
			"coil", (int32_t)usCommandCoil);
		if (pxTransaction != NULL) {
			pxTransaction->ucCommandWriteConfirmed = 1U;
			pxTransaction->xNextPollTick = xTaskGetTickCount();
			pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_WAIT_ACCEPT;
			vCoffee3DeviceSetRobotPhase(
				COFFEE3_ROBOT_PHASE_WAIT_ACCEPT);
			vCoffee3DeviceSetRobotAccepted(0U);
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

	xResult = xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
		usCoil, false, ulTimeoutMs);
	if (xResult == MODBUS_PORT_RESULT_OK) {
		vTaskDelay(pdMS_TO_TICKS(COFFEE3_ROBOT_EDGE_LOW_MS));
		xResult = xModbusPortWriteCoil(pxPort,
			COFFEE3_ROBOT_UNIT_ID, usCoil, true, ulTimeoutMs);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvAdvanceAction(ModbusPort_t *pxPort,
	Coffee3RobotTransaction_t *pxTransaction, uint8_t *pucActionTimedOut)
{
	/* Advance WAIT_ACCEPT -> MOVING -> CLEAR_RESULT and verify completion. */
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
	if (ucCoffee3CommandIsCanceled(&pxTransaction->xCommand) != 0U) {
		return MODBUS_PORT_RESULT_CANCELED;
	}
	if (pxTransaction->ucCommandWriteAttempted == 0U) {
		if ((int32_t)(xTaskGetTickCount() -
			pxTransaction->xNextPrepareRetryTick) < 0) {
			return MODBUS_PORT_RESULT_BUSY;
		}
		xResult = prvExecute(pxPort, &pxTransaction->xCommand,
			pxTransaction, pucActionTimedOut);
		if ((xResult == MODBUS_PORT_RESULT_TIMEOUT) &&
			(pxTransaction->ucCommandWriteAttempted == 0U) &&
			(pucActionTimedOut != NULL)) {
			*pucActionTimedOut = 1U;
		}
		return xResult;
	}
	if (pxTransaction->xPhase == COFFEE3_ROBOT_PHASE_WAIT_ACCEPT) {
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usCommandCoil, 1U, &bCommand,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResult,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xNow = xTaskGetTickCount();
		if ((bCommand == false) &&
			!((pxTransaction->ucAmbiguous != 0U) &&
				(pxTransaction->ucAccepted == 0U) &&
				(bResult == false))) {
			if (pxTransaction->ucAccepted == 0U) {
				pxTransaction->ucAccepted = 1U;
				pxTransaction->xAcceptedTick = xNow;
				pxTransaction->xMotionDeadline = xNow +
					pdMS_TO_TICKS(COFFEE3_ROBOT_MOTION_TIMEOUT_MS);
				vCoffee3DeviceSetRobotAccepted(1U);
				vCoffee3DeviceSetRobotPhase(
					COFFEE3_ROBOT_PHASE_MOVING);
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_ACCEPTED", 0,
					"coil", (int32_t)pxTransaction->usCommandCoil);
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_MOVING", 0,
					"action", (int32_t)pxTransaction->xCommand.usAction);
			}
			if (bResult != false) {
				pxTransaction->ucCompletionObserved = 1U;
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_ROBOT,
					(uint16_t)pxTransaction->xCommand.ulOrderId,
					"ROBOT_ACTION_COMPLETE_SIGNAL", 0,
					"coil", (int32_t)pxTransaction->usResultCoil);
				pxTransaction->xPhase =
					COFFEE3_ROBOT_PHASE_CLEAR_RESULT;
			} else {
				pxTransaction->xPhase =
					COFFEE3_ROBOT_PHASE_MOVING;
			}
			return MODBUS_PORT_RESULT_BUSY;
		}
		if ((bCommand != false) && (bResult != false) &&
			(pxTransaction->ucResultWhileCommandHigh == 0U)) {
			pxTransaction->ucResultWhileCommandHigh = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_PROTOCOL_SEQUENCE",
				MODBUS_PORT_RESULT_PROTOCOL, "action",
				(int32_t)pxTransaction->xCommand.usAction);
		}
		if ((int32_t)(xNow - pxTransaction->xAcceptDeadline) >= 0) {
			if (pucActionTimedOut != NULL) {
				*pucActionTimedOut = 1U;
			}
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_ACCEPT_TIMEOUT", MODBUS_PORT_RESULT_TIMEOUT,
				"coil", (int32_t)pxTransaction->usCommandCoil);
			return MODBUS_PORT_RESULT_TIMEOUT;
		}
		if ((pxTransaction->xLastAcceptLogTick == 0U) ||
			((xNow - pxTransaction->xLastAcceptLogTick) >=
				pdMS_TO_TICKS(COFFEE3_ROBOT_ACCEPT_LOG_INTERVAL_MS))) {
			pxTransaction->xLastAcceptLogTick = xNow;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				COFFEE3_LOG_ORDER_SYSTEM,
				"ROBOT_ACTION_ACCEPT_WAITING", 0,
				"coil", (int32_t)pxTransaction->usCommandCoil);
		}
		return MODBUS_PORT_RESULT_BUSY;
	}
	if (pxTransaction->xPhase == COFFEE3_ROBOT_PHASE_MOVING) {
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResult,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		if (bResult) {
			pxTransaction->ucCompletionObserved = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_COMPLETE_SIGNAL", 0,
				"coil", (int32_t)pxTransaction->usResultCoil);
			pxTransaction->xPhase = COFFEE3_ROBOT_PHASE_CLEAR_RESULT;
			vCoffee3DeviceSetRobotPhase(
				COFFEE3_ROBOT_PHASE_CLEAR_RESULT);
			return MODBUS_PORT_RESULT_BUSY;
		}
		if ((int32_t)(xTaskGetTickCount() -
			pxTransaction->xMotionDeadline) >= 0) {
			if (pucActionTimedOut != NULL) {
				*pucActionTimedOut = 1U;
			}
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_ROBOT,
				(uint16_t)pxTransaction->xCommand.ulOrderId,
				"ROBOT_ACTION_TIMEOUT=%s RESULT=%d",
				prvRobotActionName(
					pxTransaction->xCommand.usAction),
				(int)MODBUS_PORT_RESULT_TIMEOUT);
			return MODBUS_PORT_RESULT_TIMEOUT;
		}
		return MODBUS_PORT_RESULT_BUSY;
	}
	if (pxTransaction->xPhase == COFFEE3_ROBOT_PHASE_CLEAR_RESULT) {
		xResult = xModbusPortWriteCoil(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, false,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		xResult = xModbusPortReadCoils(pxPort, COFFEE3_ROBOT_UNIT_ID,
			pxTransaction->usResultCoil, 1U, &bResultZero,
			COFFEE3_ROBOT_IO_TIMEOUT_MS);
		if (xResult != MODBUS_PORT_RESULT_OK) {
			return xResult;
		}
		if (bResultZero) {
			return MODBUS_PORT_RESULT_PROTOCOL;
		}
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_RESULT_CLEARED", 0,
			"coil", (int32_t)pxTransaction->usResultCoil);
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_ROBOT,
			(uint16_t)pxTransaction->xCommand.ulOrderId,
			"ROBOT_ACTION_COMPLETE", 0,
			"action", (int32_t)pxTransaction->xCommand.usAction);
		return MODBUS_PORT_RESULT_OK;
	}
	return MODBUS_PORT_RESULT_BUSY;
}

/*-----------------------------------------------------------*/







