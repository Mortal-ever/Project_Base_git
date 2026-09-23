/**
  * @file      coffee3_workflow.c
  * @brief     Implement the Coffee3 order state machine and failure path.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee3_workflow.h"
#include "coffee3_config.h"

#include <stddef.h>
#include <string.h>

#include "coffee3_device.h"
#include "coffee3_device_image.h"
#include "coffee3_io.h"
#include "coffee3_io_names.h"
#include "coffee3_log.h"
#include "coffee3_server.h"
#include "coffee3_robot_tcp.h"
#include "queue.h"
#include "task.h"

/** @brief Workflow-specific failure results. */
#define COFFEE3_WORKFLOW_ERROR_QUEUE          (-1001)
#define COFFEE3_WORKFLOW_ERROR_TIMEOUT        (-1002)
#define COFFEE3_WORKFLOW_ERROR_DEVICE         (-1003)
#define COFFEE3_WORKFLOW_ERROR_CANCELED       (-1004)
#define COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT     (-1006)
#define COFFEE3_WORKFLOW_ERROR_ICE_RANGE      (-1011)
#define COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP     (-1012)
#define COFFEE3_WORKFLOW_ERROR_ICE_BASELINE   (-1013)
#define COFFEE3_WORKFLOW_ERROR_SAFE_STOP      (-1007)
#define COFFEE3_WORKFLOW_ERROR_UNSUPPORTED    (-1008)
#define COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED  (-1009)
#define COFFEE3_WORKFLOW_ERROR_IO             (-1010)

/** @brief Maximum time allowed for each safety-stop acknowledgement. */
#define COFFEE3_WORKFLOW_SAFE_STOP_MS          5000U
#define COFFEE3_WORKFLOW_ROBOT_MOTION_MS       60000U
#define COFFEE3_RESIDUAL_SETTLE_MS              800U
#define COFFEE3_RESIDUAL_SAMPLE_GAP_MS          200U
#define COFFEE3_CONDITION_CUP_1                1U
#define COFFEE3_CONDITION_CUP_2                2U
#define COFFEE3_CONDITION_LID_1                3U
#define COFFEE3_CONDITION_LID_2                4U
#define COFFEE3_CONDITION_OUTPUT_1             5U
#define COFFEE3_CONDITION_OUTPUT_2             6U
#define COFFEE3_CONDITION_STORAGE_1            7U
#define COFFEE3_CONDITION_STORAGE_2            8U

typedef enum {
	COFFEE3_HOT_WATER_IDLE = 0,
	COFFEE3_HOT_WATER_PREPARE_OFF = 1,
	COFFEE3_HOT_WATER_FILLING = 2,
	COFFEE3_HOT_WATER_WAIT_HEATER_ON = 3,
	COFFEE3_HOT_WATER_HEATING = 4,
	COFFEE3_HOT_WATER_WAIT_OFF_DONE = 5,
	COFFEE3_HOT_WATER_WAIT_OFF_ALARM = 6
} Coffee3HotWaterPhase_e;

typedef struct {
	Coffee3Command_t xCommand;
	TickType_t xStartTick;
	uint16_t usHeatMinutes;
	uint8_t ucPhase;
	uint8_t ucIoPending;
	uint8_t ucCancelRequested;
	uint8_t ucAlarmReason;
} Coffee3HotWaterContext_t;

typedef struct {
	Coffee3MaintenanceType_e xType;
	uint16_t usParameter0;
	uint16_t usParameter1;
	uint8_t ucPending;
} Coffee3MaintenanceRequest_t;

/** @brief Store workflow-owned queue resources. */
COFFEE3_CCM_DATA
static StaticQueue_t s_xOrderQueueStorage;
COFFEE3_CCM_DATA
static uint8_t s_aucOrderQueueStorage[
	COFFEE3_WORKFLOW_QUEUE_LENGTH * sizeof(Coffee3Order_t)];
COFFEE3_CCM_DATA
static QueueHandle_t s_xOrderQueue;
/** @brief One bounded maintenance request, consumed by the workflow owner. */
COFFEE3_CCM_DATA
static uint16_t s_usManualIceWeight;
COFFEE3_CCM_DATA
static uint8_t s_ucManualIcePending;
/** @brief Latest order waiting for the active order to cancel. */
COFFEE3_CCM_DATA
static Coffee3Order_t s_xPendingOrder;
COFFEE3_CCM_DATA
static uint8_t s_ucPendingOrder;
/** @brief Monotonic generation that separates reused host order numbers. */
COFFEE3_CCM_DATA
static uint32_t s_ulNextOrderEpoch;
COFFEE3_CCM_DATA
static uint8_t s_ucManualOverride;
static uint16_t s_usManualReservations;
static uint8_t s_ucOtaReserved;
static uint8_t s_ucMaintenanceActive;
static uint8_t s_ucPickupPending;
static uint8_t s_ucStoragePickupPending;
/* Storage pickup is concurrent with non-Robot work, but Robot ownership is
 * always exclusive. Active covers TAKE_STORAGE..PUT_OUTPUT; confirm covers
 * the asynchronous X3/source-sensor and outlet-door continuation. */
static uint8_t s_ucStoragePickupActive;
static uint8_t s_ucStoragePickupConfirmStorage;
static uint16_t s_usStoragePickupConfirmOrderId;
static uint16_t s_usDetachedPickupLogOrder;
static TickType_t s_xStoragePickupConfirmStart;
static uint8_t s_ucStoragePickupConfirmOverdueLogged;
static uint16_t s_ausStoredOrderId[2];
static uint8_t s_ucWaterAlarm;
static uint8_t s_ucCoffeeFillActive;
static uint8_t s_ucCoffeeFillAlarm;
static TickType_t s_xCoffeeFillStart;
static uint8_t s_ucOutletPhase;
static uint8_t s_ucDoorDirection;
static uint8_t s_ucDoorFault;
static TickType_t s_xDoorStart;
static uint8_t s_ucDoorDebugPending;
static uint8_t s_ucDoorDebugDirection;
static uint8_t s_ucDoorDebugActive;
static uint8_t s_ucDoorDebugActiveDirection;
static TickType_t s_xEmptyStart;
static uint8_t s_ucEmptyTiming;
static uint16_t s_usActiveDevices;
COFFEE3_CCM_DATA
static Coffee3HotWaterContext_t s_xHotWater;
COFFEE3_CCM_DATA
static Coffee3MaintenanceRequest_t s_xMaintenance;
COFFEE3_CCM_DATA
static uint8_t s_ucInitializationAcknowledged;
COFFEE3_CCM_DATA
static uint8_t s_ucInitializationComplete;
static uint8_t s_ucResidualState;
// static uint8_t s_ucHomeComplete;
static uint8_t s_ucDoorInitStarted;
static uint8_t s_ucDoorExpectedLimit;
static Coffee3Command_t s_xInitHome;
// static TickType_t s_xInitRetryTick;
static uint16_t s_usLastReadyMask;
static uint8_t s_ucLastBaseReady;
static int32_t s_lIceScaleEmptyBaselineGram;
static TickType_t s_xIceScaleBaselineRetryTick;
static uint8_t s_ucIceScaleBaselineValid;

COFFEE3_CCM_DATA
Coffee3WorkflowStatus_t g_xCoffee3WorkflowStatus;

/**
  * @brief  提交一个设备命令并等待该设备独立事件组的终态。
  * @param[in] usStep 当前工作流步骤编号。
  * @param[in] xDeviceId 命令目标逻辑设备。
  * @param[in] xAction 目标设备动作枚举值。
  * @param[in] usParameter0 动作参数 0。
  * @param[in] usParameter1 动作参数 1。
  * @param[in] ulTimeoutMs 当前步骤总超时时间，单位为毫秒。
  * @retval 0 设备报告命令完成。
  * @retval 负数 工作流取消、队列、设备或超时错误。
  */
static int32_t prvRunStep(uint16_t usStep, Coffee3DeviceId_e xDeviceId,
	Coffee3Action_e xAction, uint16_t usParameter0,
	uint16_t usParameter1, uint32_t ulTimeoutMs);
/**
  * @brief  按当前配置执行一份完整咖啡订单。
  * @param[in] pxOrder 已复制到工作流上下文的订单寄存器镜像。
  * @retval 0 所有必要步骤完成。
  * @retval 负数 设备、打印、称重、取消或工作流超时错误。
  */
static int32_t prvRunOrder(const Coffee3Order_t *pxOrder);
/**
  * @brief  Dispense ice by subtracting the cup-on-scale baseline.
  * @param[in] usTargetGram Target ice weight in grams.
  * @param[in] lCupBaselineGram Stable total before opening the valve.
  * @retval 0 The final ice weight is within the accepted safety range.
  * @retval Negative A scale, communication, cancel, or range check failed.
  */
static int32_t prvDispenseIce(uint16_t usTargetGram,
	int32_t lCupBaselineGram);
/**
  * @brief  Confirm a cup by subtracting a known empty-scale baseline.
  * @param[in] lEmptyBaselineGram Empty-scale total in grams.
  * @param[in] usStepBase First step identifier used by the stable read.
  * @param[out] plCupBaselineGram Stable cup-on-scale total in grams.
  * @retval 0 A cup heavier than the Coffee1 detection threshold is present.
  * @retval COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP Three checks found no cup.
  * @retval Negative A scale read or cup-weight validation failed.
  */
static int32_t prvConfirmIceCup(int32_t lEmptyBaselineGram,
	uint16_t usStepBase, int32_t *plCupBaselineGram);
/**
  * @brief  Calculate an ice pulse using the Coffee1 initial-time limits.
  * @param[in] usTargetGram Target or remaining ice in grams.
  * @param[in] usSlopeMsPerGram Calibration slope in ms/g.
  * @param[in] ucAttempt Zero for the initial pulse, otherwise a correction.
  * @retval Valve pulse in milliseconds, within the configured limits.
  */
static uint32_t prvCalculateIcePulseMs(uint16_t usTargetGram,
	uint16_t usSlopeMsPerGram, uint8_t ucAttempt);
/**
  * @brief  Read three scale samples and return the median in grams.
  * @param[in] usStepBase First step identifier used by the samples.
  * @param[out] plWeightGram Median stable total in grams.
  * @retval 0 Three valid samples were collected.
  * @retval Negative Scale communication or conversion failed.
  */
static int32_t prvReadStableScale(uint16_t usStepBase,
	int32_t *plWeightGram);
/**
  * @brief  发生失败后向相关执行机构提交尽力取消命令。
  * @note   函数本身不等待每个取消命令完成，调用者必须执行取消屏障。
  */
static int32_t prvAbortDevices(void);
static int32_t prvRunInitialization(void);
static int32_t prvProbeResidualCup(uint16_t usStepBase,
	Coffee3Action_e xPickupAction, uint8_t ucOccupiedPoint);
static int32_t prvRunMaintenance(const Coffee3MaintenanceRequest_t *pxRequest);
static int32_t prvRunFruit(uint8_t ucChannel, uint16_t usAmountMl,
	uint8_t ucClean, uint16_t usStepBase);
static int32_t prvRunIoOutput(uint16_t usStep, uint8_t ucPoint,
	uint8_t ucValue);
static int32_t prvSetProductOutputsOff(void);
static void prvServiceHotWater(void);
static BaseType_t prvSubmitHotWaterIo(uint8_t ucValue);
static int32_t prvPollHotWaterIo(uint8_t *pucDone);
static void prvSetHotWaterPublicState(uint8_t ucState,
	const char *pcEvent, int32_t lResult);
static uint8_t prvOrderValid(const Coffee3Order_t *pxOrder,
	int32_t *plError);
static void prvDelayWithServices(uint32_t ulDelayMs);
static int32_t prvWaitBusinessCondition(uint16_t usStep,
	uint8_t ucCondition);
static int32_t prvRefreshDeviceQuiet(uint16_t usStep,
	Coffee3DeviceId_e xDeviceId);
static uint8_t prvBusinessConditionActive(uint8_t ucCondition);
static const char *prvWorkflowDeviceName(Coffee3DeviceId_e xDeviceId);
static const char *prvWorkflowActionName(Coffee3Action_e xAction);
static int32_t prvWaitDeviceReportedComplete(uint16_t usStep,
	Coffee3DeviceId_e xDeviceId, uint8_t ucStatusIndex,
	uint16_t usSuccessValue, uint16_t usFailedValue);
/**
  * @brief  周期刷新本地 GPIO 镜像并服务取杯边界。
  * @note   外部 RTU IO 镜像由 Bus5 自主轮询，本函数不再投递刷新帧。
  */
static void prvServiceIoRefresh(void);
static uint16_t s_usLastInitializationFailure = 0xFFFFU;
/**
  * @brief  将工作流状态发布到全局状态和 Server 状态寄存器。
  * @param[in] xState 新的工作流状态。
  * @param[in] usStep 当前步骤编号。
  * @param[in] lError 当前错误码；无错误时为 0。
  */
static void prvPublish(Coffee3WorkflowState_e xState,
	uint16_t usStep, int32_t lError);
static uint8_t prvQueuePendingOrder(void);
static uint8_t prvRobotPositionAction(Coffee3Action_e xAction);
static void prvServicePickup(void);
static void prvPublishOutput(uint16_t usOutput, uint16_t usState);
static void prvPublishOutputForOrder(uint16_t usOutput, uint16_t usState,
	uint16_t usOrderId);
static int32_t prvCheckOutputEmpty(uint16_t usOutput);
static int32_t prvWaitOutputAvailableForOrder(void);
static int32_t prvSelectStorage(void);
static int32_t prvRunStoragePickup(uint16_t usStorage);
static int32_t prvTryRunInterleavedStoragePickup(void);
static uint16_t prvStorageStatusMask(const Coffee3IoState_t *pxIo);
static void prvRejectStoragePickup(uint16_t usStorage,
	const char *pcReason, uint16_t usStatusMask);
static int32_t prvWaitM50Clean(void);
static uint8_t prvIoValid(const Coffee3IoState_t *pxIo);
static void prvStartDoor(uint8_t ucDirection);
static void prvServiceInitialization(void);
static uint16_t prvReadyDevices(void);
static uint8_t prvOrderDevicesReady(const Coffee3Order_t *pxOrder);
static uint8_t prvMaintenanceReady(Coffee3MaintenanceType_e xType);
static uint8_t prvMaintenanceIdle(void);
static void prvServiceIceScaleBaseline(uint16_t usReadyMask);
static int32_t prvEstablishIceScaleBaseline(void);
static uint8_t prvIceTerminalFailure(int32_t lResult);

/*-----------------------------------------------------------*/
BaseType_t xCoffee3WorkflowAcquireManual(void)
{
	BaseType_t xResult;

	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((s_ucOtaReserved == 0U) && (s_ucCoffeeFillActive == 0U) &&
		(s_ucDoorDirection == 0U) &&
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(g_xCoffee3WorkflowStatus.xMachineState !=
		COFFEE3_MACHINE_INITIALIZING) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_CANCELING) &&
		(s_ucOutletPhase == 0U) && (s_ucStoragePickupPending == 0U) &&
		(s_ucStoragePickupActive == 0U) &&
		(s_ucStoragePickupConfirmStorage == 0U) &&
		(s_ucMaintenanceActive == 0U) && (s_xMaintenance.ucPending == 0U) &&
		(s_ucManualIcePending == 0U) &&
		(s_xHotWater.ucPhase == COFFEE3_HOT_WATER_IDLE) &&
		(s_xHotWater.ucIoPending == 0U) &&
		((g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen != 0U) ||
		 ((s_ucInitializationComplete == 0U) &&
		  (g_xCoffee3WorkflowStatus.xMachineState == COFFEE3_MACHINE_ALARM)))) {
		s_usManualReservations++;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3WorkflowAcquireDeferredManual(void)
{
	BaseType_t xResult;

	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((s_ucInitializationComplete != 0U) &&
		(s_ucOtaReserved == 0U) &&
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(g_xCoffee3WorkflowStatus.xMachineState != COFFEE3_MACHINE_INITIALIZING) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_CANCELING)) {
		s_usManualReservations++;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
void vCoffee3WorkflowReleaseManual(void)
{
	taskENTER_CRITICAL();
	if (s_usManualReservations != 0U) {
		s_usManualReservations--;
	}
	taskEXIT_CRITICAL();
}

/* OTA keeps an exclusive maintenance reservation until reset or start failure. */
BaseType_t xCoffee3WorkflowAcquireOta(void)
{
	Coffee3IoState_t xIo;
	uint8_t ucIndex;
	BaseType_t xResult;

	xResult = pdFAIL;
	if (s_ucOtaReserved != 0U) {
		return pdPASS;
	}
	vCoffee3IoRefreshLocal();
	vCoffee3IoGetSnapshot(&xIo);
	for (ucIndex = 0U; ucIndex < COFFEE3_LOCAL_IO_COUNT; ucIndex++) {
		if (xIo.xOutput.aucYPin[ucIndex] != 0U) {
			return pdFAIL;
		}
	}
	if ((xIo.aucModbusValid[1] == 0U) ||
		(g_axCoffee3DeviceStatus[COFFEE3_DEVICE_IO_OUTPUT].ucOnline == 0U)) {
		return pdFAIL;
	}
	for (ucIndex = 0U; ucIndex < COFFEE3_MODBUS_IO_COUNT; ucIndex++) {
		if (xIo.xOutput.aucMB2YPin[ucIndex] != 0U) {
			return pdFAIL;
		}
	}
	taskENTER_CRITICAL();
	if (s_ucOtaReserved != 0U) {
		xResult = pdPASS;
	} else if ((s_ucInitializationComplete != 0U) &&
		(s_usManualReservations == 0U) &&
		(xCoffee3WorkflowAcquireManual() == pdPASS)) {
		s_ucOtaReserved = 1U;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

void vCoffee3WorkflowReleaseOta(void)
{
	taskENTER_CRITICAL();
	if (s_ucOtaReserved != 0U) {
		s_ucOtaReserved = 0U;
		vCoffee3WorkflowReleaseManual();
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
void vCoffee3WorkflowConfirmPickup(uint16_t usOutput)
{
	taskENTER_CRITICAL();
	if ((usOutput == 1U) && (s_ucOutletPhase == 3U)) {
		s_ucPickupPending = 1U;
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
uint8_t ucCoffee3WorkflowManualDispatchAllowed(void)
{
	uint8_t ucAllowed;

	ucAllowed = 0U;
	taskENTER_CRITICAL();
	if ((s_usManualReservations != 0U) &&
		(s_ucInitializationComplete != 0U) &&
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_CANCELING) &&
		(s_ucOtaReserved == 0U) &&
		(s_ucStoragePickupActive == 0U) &&
		((g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_RUNNING) ||
		 ((s_ucMaintenanceActive == 0U) &&
		  (s_ucStoragePickupPending == 0U) &&
		  (s_xMaintenance.ucPending == 0U) &&
		  (s_ucManualIcePending == 0U) &&
		  (s_ucCoffeeFillActive == 0U) &&
		  (s_ucOutletPhase == 0U) &&
		  (s_xHotWater.ucPhase == COFFEE3_HOT_WATER_IDLE)))) {
		ucAllowed = 1U;
	}
	taskEXIT_CRITICAL();
	return ucAllowed;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3WorkflowSubmitDoorDebug(uint8_t ucDirection)
{
	BaseType_t xResult;

	if (ucDirection > 2U) {
		return pdFAIL;
	}
	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((s_ucInitializationComplete != 0U) &&
		(s_ucDoorDebugPending == 0U)) {
		s_ucDoorDebugDirection = ucDirection;
		s_ucDoorDebugPending = 1U;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/

/* Current owner state is authoritative. IO additionally needs fresh samples. */
static uint16_t prvReadyDevices(void)
{
	Coffee3IoState_t xIo;
	uint16_t usMask;
	uint8_t ucId;
	vCoffee3IoGetSnapshot(&xIo);
	usMask = 0U;
	for (ucId = 1U; ucId < COFFEE3_DEVICE_COUNT; ucId++) {
		if ((g_axCoffee3DeviceStatus[ucId].ucOnline != 0U) &&
			(g_axCoffee3DeviceStatus[ucId].ucReady != 0U)) {
			usMask |= (uint16_t)(1U << ucId);
		}
	}
	/* IO readiness is published by the RTU owner's health confirmation;
	 * RTU owners do not publish the robot owner's control-ready flag. */
	if ((g_axCoffee3DeviceStatus[COFFEE3_DEVICE_IO_INPUT].ucOnline == 0U) ||
		(prvIoValid(&xIo) == 0U)) {
		usMask &= (uint16_t)~(1U << COFFEE3_DEVICE_IO_INPUT);
	} else {
		usMask |= (uint16_t)(1U << COFFEE3_DEVICE_IO_INPUT);
	}
	if ((g_axCoffee3DeviceStatus[COFFEE3_DEVICE_IO_OUTPUT].ucOnline == 0U) ||
		(xIo.aucModbusValid[1] == 0U)) {
		usMask &= (uint16_t)~(1U << COFFEE3_DEVICE_IO_OUTPUT);
	} else {
		usMask |= (uint16_t)(1U << COFFEE3_DEVICE_IO_OUTPUT);
	}
	return usMask;
}

static uint8_t prvMaintenanceIdle(void)
{
	return ((s_ucResidualState != 1U) &&
		(s_xInitHome.ulCommandId == 0U) &&
		(s_ucMaintenanceActive == 0U) && (s_xMaintenance.ucPending == 0U) &&
		(s_ucManualIcePending == 0U) && (s_ucStoragePickupPending == 0U) &&
		(s_ucStoragePickupActive == 0U) &&
		(s_ucStoragePickupConfirmStorage == 0U) &&
		(s_usManualReservations == 0U) && (s_ucOtaReserved == 0U) &&
		(uxQueueMessagesWaiting(s_xOrderQueue) == 0U) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_CANCELING)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static int32_t prvEstablishIceScaleBaseline(void)
{
	int32_t lResult;
	int32_t lWeightGram;

	lWeightGram = 0;
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
		"Scale baseline start: reason=startup empty zero");
	lResult = prvRunStep(0xFC10U, COFFEE3_DEVICE_SCALE,
		COFFEE3_ACTION_SCALE_CLEAR_TARE, 0U, 0U, 3000U);
	if (lResult == 0) {
		lResult = prvRunStep(0xFC11U, COFFEE3_DEVICE_SCALE,
			COFFEE3_ACTION_SCALE_ZERO, 0U, 0U, 3000U);
	}
	if (lResult != 0) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Scale baseline failed: reason=zero command result=%ld",
			(long)lResult);
		return lResult;
	}
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_BASELINE_SETTLE_MS));
	lResult = prvReadStableScale(0xFC12U, &lWeightGram);
	if (lResult != 0) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Scale baseline failed: reason=read result=%ld", (long)lResult);
		return lResult;
	}
	if ((lWeightGram < -COFFEE3_ICE_BASELINE_TOLERANCE_GRAM) ||
		(lWeightGram > COFFEE3_ICE_BASELINE_TOLERANCE_GRAM)) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Scale baseline failed: reason=zero unstable weight=%ld g",
			(long)lWeightGram);
		return COFFEE3_WORKFLOW_ERROR_ICE_BASELINE;
	}
	taskENTER_CRITICAL();
	s_lIceScaleEmptyBaselineGram = lWeightGram;
	s_ucIceScaleBaselineValid = 1U;
	taskEXIT_CRITICAL();
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
		"Scale baseline ready: empty=%ld g reason=startup zero",
		(long)lWeightGram);
	return 0;
}

/*-----------------------------------------------------------*/
static void prvServiceIceScaleBaseline(uint16_t usReadyMask)
{
	TickType_t xNow;
	int32_t lResult;
	uint16_t usScaleMask;

	usScaleMask = (uint16_t)(1U << COFFEE3_DEVICE_SCALE);
	if ((usReadyMask & usScaleMask) == 0U) {
		if (s_ucIceScaleBaselineValid != 0U) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
				"Scale baseline lost: reason=scale offline");
		}
		taskENTER_CRITICAL();
		s_ucIceScaleBaselineValid = 0U;
		taskEXIT_CRITICAL();
		s_xIceScaleBaselineRetryTick = 0U;
		return;
	}
	if ((s_ucIceScaleBaselineValid != 0U) ||
		(s_ucResidualState != 2U) || (prvMaintenanceIdle() == 0U)) {
		return;
	}
	if (g_xCoffee3WorkflowStatus.ucRecoveryRequired != 0U) {
		return;
	}
	xNow = xTaskGetTickCount();
	if ((s_xIceScaleBaselineRetryTick != 0U) &&
		((int32_t)(xNow - s_xIceScaleBaselineRetryTick) < 0)) {
		return;
	}
	lResult = prvEstablishIceScaleBaseline();
	if (lResult != 0) {
		s_xIceScaleBaselineRetryTick = xTaskGetTickCount() +
			pdMS_TO_TICKS(COFFEE3_ICE_BASELINE_RETRY_MS);
	} else {
		s_xIceScaleBaselineRetryTick = 0U;
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvIceTerminalFailure(int32_t lResult)
{
	return ((lResult == COFFEE3_WORKFLOW_ERROR_ICE_RANGE) ||
		(lResult == COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP) ||
		(lResult == COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT)) ? 1U : 0U;
}

static uint8_t prvMaintenanceReady(Coffee3MaintenanceType_e xType)
{
	uint16_t usRequired;
	switch (xType) {
	case COFFEE3_MAINTENANCE_SYRUP_CLEAN:
		usRequired = (1U << COFFEE3_DEVICE_SYRUP_MACHINE);
		break;
	case COFFEE3_MAINTENANCE_COFFEE_CLEAN:
		usRequired = (1U << COFFEE3_DEVICE_COFFEE_MACHINE);
		break;
	default:
		usRequired = (1U << COFFEE3_DEVICE_IO_INPUT) |
			(1U << COFFEE3_DEVICE_IO_OUTPUT);
		break;
	}
	return ((prvReadyDevices() & usRequired) == usRequired) ? 1U : 0U;
}

static uint8_t prvOrderDevicesReady(const Coffee3Order_t *pxOrder)
{
	uint16_t usRequired;
	/* Robot RUNNING is established by the owner at order start, not admission. */
	usRequired = (1U << COFFEE3_DEVICE_IO_INPUT) | (1U << COFFEE3_DEVICE_CUP_MACHINE) |
		(1U << COFFEE3_DEVICE_LID_MACHINE);
	if (pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE] != 0xFFFFU) {
		usRequired |= (1U << COFFEE3_DEVICE_COFFEE_MACHINE);
	}
	if (pxOrder->ausRegister[COFFEE3_REG_ICE_AMOUNT] != 0U) {
		usRequired |= (1U << COFFEE3_DEVICE_ICE_MACHINE) |
			(1U << COFFEE3_DEVICE_SCALE);
		if (s_ucIceScaleBaselineValid == 0U) {
			return 0U;
		}
	}
	if ((pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_A] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_B] != 0U)) {
		usRequired |= (1U << COFFEE3_DEVICE_IO_OUTPUT);
	}
	if ((pxOrder->ausRegister[COFFEE3_REG_SYRUP_1] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_2] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_3] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_4] != 0U)) {
		usRequired |= (1U << COFFEE3_DEVICE_SYRUP_MACHINE);
	}
	return ((s_ucInitializationComplete != 0U) &&
		(g_xCoffee3RobotTcpStatus.ucConnected != 0U) &&
		((prvReadyDevices() & usRequired) == usRequired)) ? 1U : 0U;
}

/* One service on the existing Workflow task; no new task or queue. */
static void prvServiceInitialization(void)
{
	Coffee3IoState_t xIo;
	uint16_t usReady;
	uint16_t usResidualRequired;
	uint8_t ucBaseReady;
	int32_t lResult;

	usReady = prvReadyDevices();
	if (usReady != s_usLastReadyMask) {
		uint8_t ucId;
		for (ucId = 1U; ucId < (uint8_t)COFFEE3_DEVICE_COUNT; ucId++) {
			if (((usReady & (uint16_t)(1U << ucId)) != 0U) &&
				((s_usLastReadyMask & (uint16_t)(1U << ucId)) == 0U)) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					COFFEE3_LOG_ORDER_SYSTEM,
					"Device ready: name=%s reason=health poll recv",
					prvWorkflowDeviceName((Coffee3DeviceId_e)ucId));
			}
			if (((usReady & (uint16_t)(1U << ucId)) == 0U) &&
				((s_usLastReadyMask & (uint16_t)(1U << ucId)) != 0U)) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					COFFEE3_LOG_ORDER_SYSTEM,
					"Device not ready: name=%s reason=link or data lost",
					prvWorkflowDeviceName((Coffee3DeviceId_e)ucId));
			}
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Readiness changed: ready=0x%04X lost=0x%04X",
			(unsigned int)usReady,
			(unsigned int)(s_usLastReadyMask & ~usReady));
		s_usLastReadyMask = usReady;
	}
	usResidualRequired = (1U << COFFEE3_DEVICE_ROBOT) |
		(1U << COFFEE3_DEVICE_IO_INPUT);
	if ((s_ucResidualState == 0U) &&
		((usReady & usResidualRequired) == usResidualRequired) &&
		(prvMaintenanceIdle() != 0U) &&
		(g_axCoffee3DeviceStatus[COFFEE3_DEVICE_ROBOT].ucBusy == 0U)) {
		s_ucResidualState = 1U;
		lResult = prvRunInitialization();
		s_ucResidualState = (lResult == 0) ? 2U : 3U;
		g_xCoffee3WorkflowStatus.lLastError = lResult;
		(void)xCoffee3LogPrintfOrder((lResult == 0) ?
			COFFEE3_LOG_LEVEL_INFO : COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			(lResult == 0) ? "Residual check passed; debug ready; HOME runs before next order" :
			"Residual check failed (%ld); inspect cups and reset",
			(long)lResult);
		if (lResult != 0) {
			s_usActiveDevices = (1U << COFFEE3_DEVICE_ROBOT);
			g_xCoffee3WorkflowStatus.lSafetyResult = prvAbortDevices();
			g_xCoffee3WorkflowStatus.ucRecoveryRequired = 1U;
			g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
		}
	}
	prvServiceIceScaleBaseline(usReady);

	vCoffee3IoGetSnapshot(&xIo);
	if ((s_ucResidualState == 2U) && (s_ucDoorInitStarted == 0U)) {
		s_ucDoorInitStarted = 1U;
		prvStartDoor(1U);
	}
	ucBaseReady = ((s_ucResidualState == 2U) &&
		 (s_ucDoorFault == 0U) &&
		(s_ucDoorDirection == 0U) &&
		(xIo.xInput.aucXPin[0] != 0U) &&
		(xIo.xInput.aucXPin[1] == 0U) &&
		/* Completed residual initialization is not undone by manual STOP.
		 * The order owner prepares body readiness before production motion. */
		(g_xCoffee3RobotTcpStatus.ucConnected != 0U) &&
		((usReady & (1U << COFFEE3_DEVICE_IO_INPUT)) != 0U)) ? 1U : 0U;
	s_ucInitializationComplete = ucBaseReady;
	if (ucBaseReady != s_ucLastBaseReady) {
		s_ucLastBaseReady = ucBaseReady;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Production ready=%u; recipe devices checked per order",
			(unsigned int)ucBaseReady);
	}
	if (prvMaintenanceIdle() != 0U) {
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen =
			(ucBaseReady != 0U) &&
			(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U);
		g_xCoffee3WorkflowStatus.xMachineState = (s_ucResidualState == 3U) ?
			COFFEE3_MACHINE_ALARM : (ucBaseReady != 0U) ?
			COFFEE3_MACHINE_IDLE : COFFEE3_MACHINE_INITIALIZING;
	}
}

static uint8_t prvIoValid(const Coffee3IoState_t *pxIo)
{
	return ((pxIo->aucModbusValid[0] != 0U) &&
		(g_axCoffee3DeviceStatus[COFFEE3_DEVICE_IO_INPUT].ucOnline != 0U)) ?
		1U : 0U;
}

static void prvStartDoor(uint8_t ucDirection)
{
	(void)ucCoffee3IoSetLocalOutput(COFFEE3_LOCAL_DO_DOOR_UP, 0U);
	(void)ucCoffee3IoSetLocalOutput(COFFEE3_LOCAL_DO_DOOR_DOWN, 0U);
	s_ucDoorDirection = ucDirection;
	if (ucDirection != 0U) {
		s_ucDoorExpectedLimit = ucDirection;
	}
	s_xDoorStart = xTaskGetTickCount();
}

/* Workflow alone drives both directions; never energize them together. */
static void prvServicePickup(void)
{
	/* Keep outlet mechanics separate from production: the sensor edge, the
	 * 30-second empty hold and the host pickup acknowledgement are distinct. */
	Coffee3IoState_t xIo;
	TickType_t xNow;
	uint8_t ucCup;
	uint8_t ucDoorDebugRequest;
	uint8_t ucDoorDebugDirection;
	uint8_t ucDoorConflict;
	uint8_t ucDoorTimeout;
	uint8_t ucConfirmStorage;
	uint8_t ucSourceCup;
	uint16_t usConfirmOrderId;
	const char *pcDoorDirection;
	const char *pcPreviousDoorDirection;
	const char *pcDoorFailure;

	vCoffee3IoRefreshLocal();
	vCoffee3IoGetSnapshot(&xIo);
	xNow = xTaskGetTickCount();
	ucDoorDebugRequest = 0U;
	ucDoorDebugDirection = 0U;
	taskENTER_CRITICAL();
	if (s_ucDoorDebugPending != 0U) {
		ucDoorDebugRequest = 1U;
		ucDoorDebugDirection = s_ucDoorDebugDirection;
		s_ucDoorDebugPending = 0U;
	}
	taskEXIT_CRITICAL();
	if (ucDoorDebugRequest != 0U) {
		pcDoorDirection = (ucDoorDebugDirection == 1U) ? "CLOSE" :
			(ucDoorDebugDirection == 2U) ? "OPEN" : "STOP";
		if (s_ucDoorDirection != 0U) {
			pcPreviousDoorDirection = (s_ucDoorDirection == 1U) ?
				"CLOSE" : "OPEN";
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door %s interrupted by %s; DO1 and DO2 off",
				pcPreviousDoorDirection, pcDoorDirection);
			s_ucDoorDebugActive = 0U;
			s_ucDoorDebugActiveDirection = 0U;
		}
		if (ucDoorDebugDirection == 0U) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door STOP execution started: DO1 and DO2 off");
		} else {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door %s execution requested: DO%u, target DI%u", pcDoorDirection,
				(unsigned int)ucDoorDebugDirection,
				(unsigned int)ucDoorDebugDirection);
		}
		if (ucDoorDebugDirection == 0U) {
			prvStartDoor(0U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door STOP complete: DO1 and DO2 off");
		} else if ((xIo.xInput.aucXPin[0] != 0U) &&
			(xIo.xInput.aucXPin[1] != 0U)) {
			prvStartDoor(ucDoorDebugDirection);
			s_ucDoorDebugActive = 1U;
			s_ucDoorDebugActiveDirection = ucDoorDebugDirection;
		} else if (xIo.xInput.aucXPin[ucDoorDebugDirection - 1U] != 0U) {
			prvStartDoor(0U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"Door %s already at DI%u; DO1 and DO2 off",
				pcDoorDirection, (unsigned int)ucDoorDebugDirection);
		} else {
			prvStartDoor(ucDoorDebugDirection);
			s_ucDoorDebugActive = 1U;
			s_ucDoorDebugActiveDirection = ucDoorDebugDirection;
		}
		xNow = xTaskGetTickCount();
	}
	if ((s_ucDoorFault != 0U) && (s_ucDoorDirection == 0U) &&
		(xIo.xInput.aucXPin[s_ucDoorExpectedLimit - 1U] != 0U) &&
		(xIo.xInput.aucXPin[2U - s_ucDoorExpectedLimit] == 0U)) {
		s_ucDoorFault = 0U;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Outlet door recovered: DI%u confirmed", s_ucDoorExpectedLimit);
	}
	if ((s_ucInitializationComplete != 0U) && (prvIoValid(&xIo) != 0U) &&
		(xIo.xInput.aucMB1XPin[3] == 0U) && (s_ucWaterAlarm == 0U)) {
		s_ucWaterAlarm = 1U;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Water X4 lost; finish order; block next order until recovery");
	}
	if (s_ucDoorDirection != 0U) {
		ucDoorConflict = ((xIo.xInput.aucXPin[0] != 0U) &&
			(xIo.xInput.aucXPin[1] != 0U)) ? 1U : 0U;
		ucDoorTimeout = ((xNow - s_xDoorStart) >=
			pdMS_TO_TICKS(COFFEE3_DOOR_MOTION_TIMEOUT_MS)) ? 1U : 0U;
		if ((s_ucDoorFault != 0U) ||
			(ucDoorConflict != 0U) || (ucDoorTimeout != 0U)) {
			pcDoorFailure = (s_ucDoorFault != 0U) ?
				"Outlet door fault latched; outputs off; waiting for valid target limit" :
				((xIo.xInput.aucXPin[0] != 0U) &&
				 (xIo.xInput.aucXPin[1] != 0U)) ?
				"Door limit conflict: DI1 and DI2 active; outputs off" :
				(s_ucDoorDirection == 1U) ?
				"Outlet door CLOSE/UP timeout: DI1 not confirmed; DO1 off; waiting for DI1" :
				"Outlet door OPEN/DOWN timeout: DI2 not confirmed; DO2 off; waiting for DI2";
			prvStartDoor(0U);
			s_ucDoorFault = 1U;
			g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
			if (s_ucDoorDebugActive != 0U) {
				pcDoorDirection =
					(s_ucDoorDebugActiveDirection == 1U) ? "CLOSE" : "OPEN";
				if (ucDoorConflict != 0U) {
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
						COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
						"Door %s conflict: DI1 and DI2 active; DO1 and DO2 off",
						pcDoorDirection);
				} else if (ucDoorTimeout != 0U) {
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
						"Door %s timeout: DI%u not confirmed; DO1 and DO2 off",
						pcDoorDirection,
						(unsigned int)s_ucDoorDebugActiveDirection);
				} else {
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
						"Door %s interrupted: fault latched; DO1 and DO2 off",
						pcDoorDirection);
				}
				s_ucDoorDebugActive = 0U;
				s_ucDoorDebugActiveDirection = 0U;
			} else {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.ausOutputOrderId[0],
					"%s", pcDoorFailure);
			}
		} else if (xIo.xInput.aucXPin[s_ucDoorDirection - 1U] != 0U) {
			if (s_ucDoorDebugActive != 0U) {
				pcDoorDirection =
					(s_ucDoorDebugActiveDirection == 1U) ? "CLOSE" : "OPEN";
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
					"Door %s complete: DI%u confirmed; DO1 and DO2 off",
					pcDoorDirection,
					(unsigned int)s_ucDoorDebugActiveDirection);
				s_ucDoorDebugActive = 0U;
				s_ucDoorDebugActiveDirection = 0U;
			}
			prvStartDoor(0U);
		} else {
			(void)ucCoffee3IoSetLocalOutput((uint8_t)(s_ucDoorDirection - 1U), 1U);
		}
	}
	ucConfirmStorage = s_ucStoragePickupConfirmStorage;
	if ((ucConfirmStorage >= 1U) && (ucConfirmStorage <= 2U) &&
		(prvIoValid(&xIo) != 0U)) {
		ucCup = xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_OUTLET_CUP];
		ucSourceCup = xIo.xInput.aucMB1XPin[ucConfirmStorage - 1U];
		if ((ucCup != 0U) && (ucSourceCup == 0U) &&
			(s_ucDoorFault == 0U) && (s_ucDoorDirection == 0U)) {
			usConfirmOrderId = s_usStoragePickupConfirmOrderId;
			taskENTER_CRITICAL();
			s_ucStoragePickupConfirmStorage = 0U;
			s_usStoragePickupConfirmOrderId = 0U;
			s_ucStoragePickupConfirmOverdueLogged = 0U;
			s_ausStoredOrderId[ucConfirmStorage - 1U] = 0U;
			s_ucOutletPhase = 1U;
			s_ucPickupPending = 0U;
			s_ucEmptyTiming = 0U;
			taskEXIT_CRITICAL();
			prvPublishOutputForOrder(1U, 5U, usConfirmOrderId);
			prvStartDoor(2U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				(usConfirmOrderId != 0U) ? usConfirmOrderId :
					COFFEE3_LOG_ORDER_SYSTEM,
				"Pickup placement confirmed: device=IoInput storage=%u X3=1 source=0; open outlet",
				(unsigned int)ucConfirmStorage);
		} else if ((s_ucStoragePickupConfirmOverdueLogged == 0U) &&
			((xNow - s_xStoragePickupConfirmStart) >=
			 pdMS_TO_TICKS(COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS))) {
			s_ucStoragePickupConfirmOverdueLogged = 1U;
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				(s_usStoragePickupConfirmOrderId != 0U) ?
					s_usStoragePickupConfirmOrderId : COFFEE3_LOG_ORDER_SYSTEM,
				"Pickup confirmation overdue: device=IoInput storage=%u X3=%u source=%u; waiting",
				(unsigned int)ucConfirmStorage, (unsigned int)ucCup,
				(unsigned int)ucSourceCup);
		}
	}
	if ((s_ucOutletPhase == 0U) || (s_ucDoorFault != 0U)) {
		return;
	}
	if (prvIoValid(&xIo) == 0U) {
		s_ucEmptyTiming = 0U;
		return;
	}
	ucCup = xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_OUTLET_CUP];
	if ((s_ucOutletPhase == 1U) && (ucCup != 0U)) {
		s_ucOutletPhase = 2U;
	}
	if (s_ucOutletPhase == 2U) {
		if ((ucCup != 0U) || (s_ucDoorDirection != 0U)) {
			s_ucEmptyTiming = 0U;
		} else if (s_ucEmptyTiming == 0U) {
			s_ucEmptyTiming = 1U;
			s_xEmptyStart = xNow;
		} else if ((xNow - s_xEmptyStart) >= pdMS_TO_TICKS(COFFEE3_OUTLET_EMPTY_HOLD_MS)) {
			s_ucOutletPhase = 3U;
			prvPublishOutput(1U, 0x15U);
			prvStartDoor(1U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.ausOutputOrderId[0],
				"Outlet X3 empty for 30 s; customer take complete, raise door, wait ACK");
		}
	}
	if ((s_ucOutletPhase == 3U) && (s_ucPickupPending != 0U) &&
		(s_ucDoorDirection == 0U) && (xIo.xInput.aucXPin[0] != 0U) &&
		(ucCup == 0U)) {
		taskENTER_CRITICAL();
		s_ucPickupPending = 0U;
		s_ucOutletPhase = 0U;
		taskEXIT_CRITICAL();
		prvPublishOutput(1U, 0U);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.ausOutputOrderId[0],
			"Customer take ACK consumed; door closed, outlet released");
	}
}

/*-----------------------------------------------------------*/
static void prvPublishOutput(uint16_t usOutput, uint16_t usState)
{
	prvPublishOutputForOrder(usOutput, usState,
		g_xCoffee3WorkflowStatus.usCurrentOrderId);
}

/*-----------------------------------------------------------*/
static void prvPublishOutputForOrder(uint16_t usOutput, uint16_t usState,
	uint16_t usOrderId)
{
	taskENTER_CRITICAL();
	g_xCoffee3WorkflowStatus.ausOutputState[usOutput - 1U] = usState;
	if (usState == 2U) {
		g_xCoffee3WorkflowStatus.ausOutputOrderId[usOutput - 1U] =
			usOrderId;
	}
	taskEXIT_CRITICAL();
	vCoffee3ServerPublishOutput(usOutput, usState);
}

/*-----------------------------------------------------------*/
static int32_t prvCheckOutputEmpty(uint16_t usOutput)
{
	Coffee3IoState_t xIo;
	int32_t lResult;

	prvServicePickup();
	if ((usOutput != 1U) || (s_ucOutletPhase != 0U) ||
		(s_ucStoragePickupConfirmStorage != 0U) ||
		(s_ucDoorFault != 0U) || (s_ucDoorDirection != 0U)) {
		return COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED;
	}
	lResult = prvRefreshDeviceQuiet(29U, COFFEE3_DEVICE_IO_INPUT);
	vCoffee3IoGetSnapshot(&xIo);
	if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
		return COFFEE3_WORKFLOW_ERROR_IO;
	}
	return ((xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_OUTLET_CUP] != 0U) ||
		(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_DOOR_UPPER] == 0U)) ?
		COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED : 0;
}

/*-----------------------------------------------------------*/
static int32_t prvWaitOutputAvailableForOrder(void)
{
	Coffee3IoState_t xIo;
	TickType_t xStart;
	uint8_t ucOverdueLogged;

	xStart = xTaskGetTickCount();
	ucOverdueLogged = 0U;
	for (;;) {
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		prvServicePickup();
		vCoffee3IoGetSnapshot(&xIo);
		if ((prvIoValid(&xIo) != 0U) &&
			(s_ucOutletPhase == 0U) &&
			(s_ucStoragePickupActive == 0U) &&
			(s_ucStoragePickupConfirmStorage == 0U) &&
			(s_ucDoorFault == 0U) && (s_ucDoorDirection == 0U) &&
			(xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_OUTLET_CUP] == 0U) &&
			(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_DOOR_UPPER] != 0U)) {
			return 0;
		}
		if ((ucOverdueLogged == 0U) &&
			((xTaskGetTickCount() - xStart) >=
			 pdMS_TO_TICKS(COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS))) {
			ucOverdueLogged = 1U;
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Outlet wait overdue: device=IoInput X3=%u phase=%u; waiting for pickup release",
				(unsigned int)xIo.xInput.aucMB1XPin[
					COFFEE3_EXTERNAL_DI_OUTLET_CUP],
				(unsigned int)s_ucOutletPhase);
		}
		prvDelayWithServices(100U);
	}
}

static int32_t prvSelectStorage(void)
{
	Coffee3IoState_t xIo;
	int32_t lResult;
	uint8_t ucIndex;
	uint16_t usEnabled;

	lResult = prvRefreshDeviceQuiet(0xFD1FU, COFFEE3_DEVICE_IO_INPUT);
	vCoffee3IoGetSnapshot(&xIo);
	if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
		return COFFEE3_WORKFLOW_ERROR_IO;
	}
	usEnabled = usCoffee3ConfigStorageMask() & COFFEE3_STORAGE_INSTALLED_MASK;
	for (ucIndex = 0U; ucIndex < 2U; ucIndex++) {
		if (((usEnabled & (1U << ucIndex)) != 0U) &&
			(xIo.xInput.aucMB1XPin[ucIndex] == 0U)) {
			return (int32_t)(ucIndex + 1U);
		}
	}
	return COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3WorkflowInitialize(void)
{
	memset(&g_xCoffee3WorkflowStatus, 0,
		sizeof(g_xCoffee3WorkflowStatus));
	s_usManualIceWeight = 0U;
	s_ucManualIcePending = 0U;
	memset(&s_xPendingOrder, 0, sizeof(s_xPendingOrder));
	s_ucPendingOrder = 0U;
	s_ulNextOrderEpoch = 0U;
	s_ucManualOverride = 0U;
	s_usManualReservations = 0U;
	s_ucMaintenanceActive = 0U;
	s_ucPickupPending = 0U;
	s_ucStoragePickupPending = 0U;
	s_ucStoragePickupActive = 0U;
	s_ucStoragePickupConfirmStorage = 0U;
	s_usStoragePickupConfirmOrderId = 0U;
	s_usDetachedPickupLogOrder = 0U;
	s_xStoragePickupConfirmStart = 0U;
	s_ucStoragePickupConfirmOverdueLogged = 0U;
	memset(s_ausStoredOrderId, 0, sizeof(s_ausStoredOrderId));
	s_ucWaterAlarm = 0U;
	s_ucOutletPhase = 0U;
	s_ucDoorDirection = 0U;
	s_ucDoorFault = 0U;
	s_ucDoorDebugPending = 0U;
	s_ucDoorDebugDirection = 0U;
	s_ucDoorDebugActive = 0U;
	s_ucDoorDebugActiveDirection = 0U;
	s_ucEmptyTiming = 0U;
	s_usActiveDevices = 0U;
	memset(&s_xHotWater, 0, sizeof(s_xHotWater));
	memset(&s_xMaintenance, 0, sizeof(s_xMaintenance));
	s_ucInitializationAcknowledged = 0U;
	s_ucInitializationComplete = 0U;
	s_ucResidualState = 0U;
	// s_ucHomeComplete = 0U;
	s_ucDoorInitStarted = 0U;
	s_ucDoorExpectedLimit = 1U;
	memset(&s_xInitHome, 0, sizeof(s_xInitHome));
	// s_xInitRetryTick = 0U;
	s_usLastReadyMask = 0U;
	s_ucLastBaseReady = 0U;
	s_lIceScaleEmptyBaselineGram = 0;
	s_xIceScaleBaselineRetryTick = 0U;
	s_ucIceScaleBaselineValid = 0U;
	g_xCoffee3WorkflowStatus.xMachineState =
		COFFEE3_MACHINE_INITIALIZING;
	g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
	s_xOrderQueue = xQueueCreateStatic(COFFEE3_WORKFLOW_QUEUE_LENGTH,
		sizeof(Coffee3Order_t), s_aucOrderQueueStorage,
		&s_xOrderQueueStorage);
	return (s_xOrderQueue != NULL) ? pdPASS : pdFAIL;
}

/*-----------------------------------------------------------*/
uint8_t ucCoffee3WorkflowInitializationComplete(void)
{
	return s_ucInitializationComplete;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3WorkflowSubmitMaintenance(
	Coffee3MaintenanceType_e xType, uint16_t usParameter0,
	uint16_t usParameter1)
{
	BaseType_t xResult;

	if ((xType <= COFFEE3_MAINTENANCE_NONE) ||
		(xType > COFFEE3_MAINTENANCE_FRUIT_CLEAN)) {
		return pdFAIL;
	}
	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((prvMaintenanceIdle() != 0U) &&
		(prvMaintenanceReady(xType) != 0U)) {
		s_xMaintenance.xType = xType;
		s_xMaintenance.usParameter0 = usParameter0;
		s_xMaintenance.usParameter1 = usParameter1;
		s_xMaintenance.ucPending = 1U;
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3WorkflowSetHotWater(uint8_t ucStart,
	uint16_t usHeatMinutes)
{
	if (ucStart == 0U) {
		taskENTER_CRITICAL();
		s_xHotWater.ucCancelRequested = 1U;
		taskEXIT_CRITICAL();
		return pdPASS;
	}
	if (usHeatMinutes == 0U) {
		usHeatMinutes = COFFEE3_HOT_WATER_DEFAULT_HEAT_MIN;
	}
	if (usHeatMinutes > COFFEE3_HOT_WATER_MAX_HEAT_MIN) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if ((prvMaintenanceReady(COFFEE3_MAINTENANCE_FRUIT_CLEAN) == 0U) ||
		(s_usManualReservations != 0U) ||
		(s_xHotWater.ucPhase != COFFEE3_HOT_WATER_IDLE) ||
		(s_xHotWater.ucIoPending != 0U)) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	memset(&s_xHotWater, 0, sizeof(s_xHotWater));
	s_xHotWater.usHeatMinutes = usHeatMinutes;
	s_xHotWater.ucPhase = COFFEE3_HOT_WATER_PREPARE_OFF;
	taskEXIT_CRITICAL();
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
		"HOT_WATER_START", 0, "minutes", (int32_t)usHeatMinutes);
	return pdPASS;
}

/*-----------------------------------------------------------*/
void vCoffee3WorkflowAcknowledgeAlarm(void)
{
	Coffee3IoState_t xIo;
	vCoffee3IoGetSnapshot(&xIo);
	taskENTER_CRITICAL();
	if ((s_ucInitializationComplete != 0U) &&
		(prvIoValid(&xIo) != 0U) && (xIo.xInput.aucMB1XPin[3] != 0U)) {
		s_ucWaterAlarm = 0U;
		if (xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_COFFEE_WATER_LOW] != 0U) {
			s_ucCoffeeFillAlarm = 0U;
		}
	}

	s_ucInitializationAcknowledged = 1U;
	if ((s_ucInitializationComplete == 0U) || (s_ucWaterAlarm != 0U)) {
		taskEXIT_CRITICAL();
		return;
	}
	if (s_xHotWater.ucPhase == COFFEE3_HOT_WATER_IDLE) {
		g_xCoffee3WorkflowStatus.ucHotWaterState =
			COFFEE3_MAINTENANCE_IDLE;
	}
	if ((g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING)) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_IDLE;
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3WorkflowSubmitOrder(const Coffee3Order_t *pxOrder)
{
	BaseType_t xResult;
	if ((pxOrder == NULL) || (s_xOrderQueue == NULL)) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if ((prvOrderDevicesReady(pxOrder) == 0U) ||
		(g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen == 0U) ||
		(s_ucWaterAlarm != 0U) || (s_ucCoffeeFillAlarm != 0U) ||
		(s_ucStoragePickupPending != 0U) || (s_usManualReservations != 0U) ||
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired != 0U)) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
	xResult = xQueueSend(s_xOrderQueue, pxOrder, 0U);
	if (xResult != pdPASS) {
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

BaseType_t xCoffee3WorkflowSubmitStoragePickup(uint16_t usStorage,
	uint16_t usOutput)
{
	if ((usStorage < 1U) || (usStorage > 2U) || (usOutput != 1U) ||
		((usCoffee3ConfigStorageMask() &
		 (uint16_t)(1U << (usStorage - 1U))) == 0U)) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if ((s_ucInitializationComplete == 0U) ||
		((g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING) &&
		 (g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen == 0U)) ||
		(g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_CANCELING) ||
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired != 0U) ||
		(s_ucMaintenanceActive != 0U) || (s_xMaintenance.ucPending != 0U) ||
		(s_ucManualIcePending != 0U) || (s_ucOtaReserved != 0U) ||
		(s_ucOutletPhase != 0U) || (s_ucDoorDirection != 0U) ||
		(s_ucDoorFault != 0U) || (s_usManualReservations != 0U) ||
		(s_ucStoragePickupPending != 0U) ||
		(s_ucStoragePickupActive != 0U) ||
		(s_ucStoragePickupConfirmStorage != 0U)) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	s_ucStoragePickupPending = (uint8_t)usStorage;
	if (g_xCoffee3WorkflowStatus.xState != COFFEE3_WORKFLOW_RUNNING) {
		g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
	}
	taskEXIT_CRITICAL();
	return pdPASS;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3WorkflowSubmitManualIce(uint16_t usTargetGrams)
{
	BaseType_t xResult;
	const char *pcRejectReason;
	uint16_t usReadyMask;

	xResult = pdFAIL;
	pcRejectReason = NULL;
	usReadyMask = 0U;
	if (usTargetGrams == 0U) {
		pcRejectReason = "target_zero";
	} else if (s_xOrderQueue == NULL) {
		pcRejectReason = "workflow_not_ready";
	} else if (uxQueueMessagesWaiting(s_xOrderQueue) != 0U) {
		pcRejectReason = "order_pending";
	} else {
		taskENTER_CRITICAL();
		usReadyMask = prvReadyDevices();
		if (s_ucResidualState == 1U) {
			pcRejectReason = "residual_check_running";
		} else if (prvMaintenanceIdle() == 0U) {
			pcRejectReason = "workflow_busy";
		} else if ((usReadyMask &
			(1U << COFFEE3_DEVICE_ICE_MACHINE)) == 0U) {
			pcRejectReason = "ice_not_ready";
		} else if ((usReadyMask &
			(1U << COFFEE3_DEVICE_SCALE)) == 0U) {
			pcRejectReason = "scale_not_ready";
		} else if (s_ucIceScaleBaselineValid == 0U) {
			pcRejectReason = "scale_baseline_pending";
		} else {
			s_usManualIceWeight = usTargetGrams;
			s_ucManualIcePending = 1U;
			g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
			xResult = pdPASS;
		}
		taskEXIT_CRITICAL();
	}
	if (xResult != pdPASS) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
			"Manual ice rejected: %s, ready=0x%04X, target=%u g",
			pcRejectReason, (unsigned int)usReadyMask,
			(unsigned int)usTargetGrams);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
void vCoffee3WorkflowRequestCancel(void)
{
	uint32_t ulOrderEpoch;

	taskENTER_CRITICAL();
	g_xCoffee3WorkflowStatus.ucCancelRequested = 1U;
	ulOrderEpoch = g_xCoffee3WorkflowStatus.ulOrderEpoch;
	taskEXIT_CRITICAL();
	vCoffee3OrderCancelRequest(ulOrderEpoch);
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"WORKFLOW_CANCEL_REQUEST", 0,
		"order", (int32_t)g_xCoffee3WorkflowStatus.usCurrentOrderId);
}

/*-----------------------------------------------------------*/
void vCoffee3WorkflowTask(void *pvArgument)
{
	Coffee3Order_t xOrder;
	int32_t lResult;
	int32_t lSafetyResult;
	uint16_t usStoragePickup;
	uint16_t usManualIceWeight;
	uint8_t ucManualBarrier;
	uint8_t ucManualIceDispenseStarted;
	const char *pcIceFailureLog;
	Coffee3MaintenanceRequest_t xMaintenance;

	(void)pvArgument;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, "TASK_RUNNING:C3Workflow", 0);
	vTaskDelay(pdMS_TO_TICKS(500U));
	prvPublish(COFFEE3_WORKFLOW_IDLE, 0U, 0);
	for (;;) {
		prvServiceHotWater();
		prvServiceInitialization();
		taskENTER_CRITICAL();
		usStoragePickup = 0U;
		if ((s_ucStoragePickupPending != 0U) &&
			(s_ucOutletPhase == 0U) &&
			(s_ucStoragePickupConfirmStorage == 0U) &&
			(s_ucDoorDirection == 0U) && (s_ucDoorFault == 0U)) {
			usStoragePickup = s_ucStoragePickupPending;
			s_ucStoragePickupPending = 0U;
		}
		if (usStoragePickup != 0U) {
			s_ucStoragePickupActive = 1U;
			s_ucMaintenanceActive = 1U;
		}
		taskEXIT_CRITICAL();
		if (usStoragePickup != 0U) {
			s_usActiveDevices = 0U;
			g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
			g_xCoffee3WorkflowStatus.ucCancelRequested = 0U;
			g_xCoffee3WorkflowStatus.ulOrderEpoch = ++s_ulNextOrderEpoch;
			lResult = prvRunStoragePickup(usStoragePickup);
			if (lResult != 0) {
				g_xCoffee3WorkflowStatus.lSafetyResult =
				(s_usActiveDevices != 0U) ? prvAbortDevices() : 0;
				g_xCoffee3WorkflowStatus.ucRecoveryRequired =
					(g_xCoffee3WorkflowStatus.ucPositionUncertain != 0U) ||
					(g_xCoffee3WorkflowStatus.lSafetyResult != 0);
			}
			s_ucStoragePickupActive = 0U;
			s_ucMaintenanceActive = 0U;
			g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen =
				(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) ? 1U : 0U;
			g_xCoffee3WorkflowStatus.xState = (lResult == 0) ?
				COFFEE3_WORKFLOW_IDLE : COFFEE3_WORKFLOW_FAILED;
			vCoffee3ServerFinishRequest(1U);
			continue;
		}
		/* Existing pickup or maintenance work above retains ownership. Once it
		 * releases, a deferred Robot reservation gets the next boundary before
		 * an order can be dequeued. */
		taskENTER_CRITICAL();
		ucManualBarrier = ((s_usManualReservations != 0U) &&
			(s_ucManualIcePending == 0U) &&
			(s_xMaintenance.ucPending == 0U)) ? 1U : 0U;
		taskEXIT_CRITICAL();
		if (ucManualBarrier != 0U) {
			prvServiceIoRefresh();
			vTaskDelay(pdMS_TO_TICKS(COFFEE3_WORKFLOW_IO_REFRESH_MS));
			continue;
		}
		if (prvQueuePendingOrder() != 0U) {
			continue;
		}
		if (xQueueReceive(s_xOrderQueue, &xOrder,
			pdMS_TO_TICKS(COFFEE3_WORKFLOW_IO_REFRESH_MS)) !=
			pdPASS) {
			memset(&xMaintenance, 0, sizeof(xMaintenance));
			taskENTER_CRITICAL();
			if (s_ucManualIcePending != 0U) {
				usManualIceWeight = s_usManualIceWeight;
				s_ucManualIcePending = 0U;
				s_ucMaintenanceActive = 1U;
			} else {
				usManualIceWeight = 0U;
			}
			if ((usManualIceWeight == 0U) &&
				(s_xMaintenance.ucPending != 0U)) {
				xMaintenance = s_xMaintenance;
				s_xMaintenance.ucPending = 0U;
				s_ucMaintenanceActive = 1U;
			}
			taskEXIT_CRITICAL();
			if (usManualIceWeight != 0U) {
				int32_t lCupBaselineGram;

				s_usActiveDevices = 0U;
				ucManualIceDispenseStarted = 0U;
				lCupBaselineGram = 0;
				taskENTER_CRITICAL();
				s_ulNextOrderEpoch++;
				if (s_ulNextOrderEpoch == 0U) {
					s_ulNextOrderEpoch = 1U;
				}
				g_xCoffee3WorkflowStatus.ulOrderEpoch =
					s_ulNextOrderEpoch;
				g_xCoffee3WorkflowStatus.usCurrentOrderId =
					COFFEE3_LOG_ORDER_DEBUG;
				g_xCoffee3WorkflowStatus.ucCancelRequested = 0U;
				taskEXIT_CRITICAL();
				(void)xCoffee3LogWriteFieldOrder(
					COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					COFFEE3_LOG_ORDER_DEBUG,
					"MANUAL_ICE_START", 0, "target_g",
					(int32_t)usManualIceWeight);
				prvPublish(COFFEE3_WORKFLOW_RUNNING, 900U, 0);
				lResult = prvConfirmIceCup(
					s_lIceScaleEmptyBaselineGram, 901U,
					&lCupBaselineGram);
				if (lResult == 0) {
					ucManualIceDispenseStarted = 1U;
					lResult = prvDispenseIce(usManualIceWeight,
						lCupBaselineGram);
				}
				if (lResult == 0) {
					taskENTER_CRITICAL();
					g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
					taskEXIT_CRITICAL();
					prvPublish(COFFEE3_WORKFLOW_COMPLETED,
						903U, 0);
					(void)xCoffee3LogWriteFieldOrder(
						COFFEE3_LOG_LEVEL_INFO,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						COFFEE3_LOG_ORDER_DEBUG,
						"MANUAL_ICE_DONE", 0, "target_g",
						(int32_t)usManualIceWeight);
				} else {
					/* A pre-dispense cup-check failure has no moving actuator to abort.
					 * Range failures also return after the valve has been closed. */
					if ((prvIceTerminalFailure(lResult) != 0U) ||
						(ucManualIceDispenseStarted == 0U)) {
						s_usActiveDevices = 0U;
						lSafetyResult = 0;
						g_xCoffee3WorkflowStatus.lSafetyResult = 0;
						taskENTER_CRITICAL();
						g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
						taskEXIT_CRITICAL();
					} else {
						lSafetyResult = prvAbortDevices();
						g_xCoffee3WorkflowStatus.lSafetyResult = lSafetyResult;
						if (lSafetyResult == 0) {
							taskENTER_CRITICAL();
							g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
							taskEXIT_CRITICAL();
						} else {
							g_xCoffee3WorkflowStatus.ucRecoveryRequired = 1U;
						}
					}
					prvPublish(COFFEE3_WORKFLOW_FAILED,
						g_xCoffee3WorkflowStatus.usCurrentStep,
						lResult);
					(void)xCoffee3LogWriteFieldOrder(
						COFFEE3_LOG_LEVEL_ERROR,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						COFFEE3_LOG_ORDER_DEBUG,
						"MANUAL_ICE_FAILED", lResult, "step",
						(int32_t)
							g_xCoffee3WorkflowStatus.usCurrentStep);
				}
			}
			if (usManualIceWeight != 0U) {
				s_ucMaintenanceActive = 0U;
			}
			if (xMaintenance.xType != COFFEE3_MAINTENANCE_NONE) {
				s_usActiveDevices = 0U;
				taskENTER_CRITICAL();
				s_ulNextOrderEpoch++;
				if (s_ulNextOrderEpoch == 0U) {
					s_ulNextOrderEpoch = 1U;
				}
				g_xCoffee3WorkflowStatus.ulOrderEpoch = s_ulNextOrderEpoch;
				g_xCoffee3WorkflowStatus.usCurrentOrderId = COFFEE3_LOG_ORDER_DEBUG;
				g_xCoffee3WorkflowStatus.ucCancelRequested = 0U;
				taskEXIT_CRITICAL();
				g_xCoffee3WorkflowStatus.xMachineState =
					COFFEE3_MACHINE_BUSY;
				lResult = prvRunMaintenance(&xMaintenance);
				if (lResult != 0) {
					g_xCoffee3WorkflowStatus.lSafetyResult = prvAbortDevices();
					/* Maintenance failure remains local and retryable. */
				}
				taskENTER_CRITICAL();
				s_ucMaintenanceActive = 0U;
				g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen =
					(lResult == 0) ? 1U : 0U;
				taskEXIT_CRITICAL();
				g_xCoffee3WorkflowStatus.xMachineState =
					(lResult == 0) ? COFFEE3_MACHINE_IDLE :
						COFFEE3_MACHINE_ALARM;
				g_xCoffee3WorkflowStatus.xState = (lResult == 0) ?
					COFFEE3_WORKFLOW_IDLE : COFFEE3_WORKFLOW_FAILED;
				(void)xCoffee3LogWriteFieldOrder(
					(lResult == 0) ? COFFEE3_LOG_LEVEL_INFO :
						COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					COFFEE3_LOG_ORDER_DEBUG,
					(lResult == 0) ? "MAINTENANCE_COMPLETE" :
						"MAINTENANCE_FAILED",
					lResult, "type", (int32_t)xMaintenance.xType);
			}
			prvServiceIoRefresh();
			continue;
		}
		taskENTER_CRITICAL();
		/* Claim a new order under the same reservation boundary. Debug accepted
		 * during a running order is inserted only when Robot ownership is free. */
		if (s_usManualReservations != 0U) {
			taskEXIT_CRITICAL();
			if (xQueueSendToFront(s_xOrderQueue, &xOrder, 0U) != pdPASS) {
				(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					"ORDER_REQUEUE_FAILED_FOR_MANUAL", -1);
			}
			continue;
		}
		s_ulNextOrderEpoch++;
		if (s_ulNextOrderEpoch == 0U) {
			s_ulNextOrderEpoch = 1U;
		}
		g_xCoffee3WorkflowStatus.ulOrderEpoch = s_ulNextOrderEpoch;
		g_xCoffee3WorkflowStatus.usCurrentOrderId =
			xOrder.ausRegister[COFFEE3_REG_ORDER_NUMBER];
		g_xCoffee3WorkflowStatus.ucCancelRequested = 0U;
		s_ucManualOverride = 0U;
		s_usActiveDevices = 0U;
		g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
		g_xCoffee3WorkflowStatus.ucCommandSent = 0U;
		g_xCoffee3WorkflowStatus.ucDeviceDone = 0U;
		g_xCoffee3WorkflowStatus.ucPhysicalVerified = 0U;
		g_xCoffee3WorkflowStatus.lSafetyResult = 0;
		g_xCoffee3WorkflowStatus.usActiveOutput = 1U;
		g_xCoffee3WorkflowStatus.ucContentComplete = 0U;
		g_xCoffee3WorkflowStatus.xState = COFFEE3_WORKFLOW_RUNNING;
		taskEXIT_CRITICAL();
		vCoffee3ServerPublishOrder(&xOrder);
		prvPublish(COFFEE3_WORKFLOW_RUNNING, 1U, 0);
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"ORDER_START", 0,
			"order", (int32_t)g_xCoffee3WorkflowStatus.usCurrentOrderId);
		lResult = prvRunOrder(&xOrder);
		vCoffee3ServerFinishRequest(0U);
		if (lResult == 0) {
			g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
			if (prvQueuePendingOrder() == 0U) {
				taskENTER_CRITICAL();
				if ((s_ucStoragePickupPending == 0U) &&
					(s_ucStoragePickupActive == 0U)) {
					g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
				}
				taskEXIT_CRITICAL();
			}
			taskENTER_CRITICAL();
			g_xCoffee3WorkflowStatus.ulCompletedOrderCount++;
			taskEXIT_CRITICAL();
			prvPublish(COFFEE3_WORKFLOW_COMPLETED,
				g_xCoffee3WorkflowStatus.usCurrentStep, 0);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"ORDER_COMPLETED", 0,
				"order",
				(int32_t)g_xCoffee3WorkflowStatus.usCurrentOrderId);
		} else {
			taskENTER_CRITICAL();
			g_xCoffee3WorkflowStatus.ulFailedOrderCount++;
			taskEXIT_CRITICAL();
			if (prvIceTerminalFailure(lResult) != 0U) {
				/* A confirmed weight/cup terminal failure leaves the robot at the
				 * ice station and does not issue recovery motion. */
				s_usActiveDevices = 0U;
				lSafetyResult = 0;
				g_xCoffee3WorkflowStatus.lSafetyResult = 0;
				g_xCoffee3WorkflowStatus.ucRecoveryRequired = 1U;
				g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
				s_ucPendingOrder = 0U;
				if (lResult == COFFEE3_WORKFLOW_ERROR_ICE_RANGE) {
					pcIceFailureLog =
						"Order failed: reason=ice weight unsafe; cup stays at ice station";
				} else if (lResult == COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP) {
					pcIceFailureLog =
						"Order failed: reason=no cup at ice station; motion skipped";
				} else {
					pcIceFailureLog =
						"Order failed: reason=cup weight invalid; motion skipped";
				}
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					pcIceFailureLog);
			} else {
				lSafetyResult = (s_usActiveDevices != 0U) ? prvAbortDevices() : 0;
				g_xCoffee3WorkflowStatus.lSafetyResult = lSafetyResult;
				if ((lSafetyResult == 0) &&
					(g_xCoffee3WorkflowStatus.ucPositionUncertain == 0U)) {
					if (prvQueuePendingOrder() == 0U) {
						taskENTER_CRITICAL();
						if ((s_ucStoragePickupPending == 0U) &&
							(s_ucStoragePickupActive == 0U)) {
							g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 1U;
						}
						taskEXIT_CRITICAL();
					}
				} else {
					g_xCoffee3WorkflowStatus.ucRecoveryRequired = 1U;
					g_xCoffee3WorkflowStatus.ucOrderAdmissionOpen = 0U;
					s_ucPendingOrder = 0U;
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						g_xCoffee3WorkflowStatus.usCurrentOrderId,
						"Recovery locked: stop=%ld position_unknown=%u; inspect/reset",
						(long)lSafetyResult,
						(unsigned int)g_xCoffee3WorkflowStatus.ucPositionUncertain);
				}
			}
			prvPublish(COFFEE3_WORKFLOW_FAILED,
				g_xCoffee3WorkflowStatus.usCurrentStep, lResult);
			(void)xCoffee3LogWriteFieldOrder(
				(s_ucManualOverride != 0U) ? COFFEE3_LOG_LEVEL_WARNING :
					COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				(s_ucManualOverride != 0U) ? "ORDER_CANCELED" : "ORDER_FAILED",
				lResult, "step",
				(int32_t)g_xCoffee3WorkflowStatus.usCurrentStep);
		}
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotPositionAction(Coffee3Action_e xAction)
{
	return (((xAction >= COFFEE3_ACTION_ROBOT_HOME) &&
		(xAction <= COFFEE3_ACTION_ROBOT_TAKE_STORAGE)) ||
		(xAction == COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static uint8_t prvQueuePendingOrder(void)
{
	Coffee3Order_t xOrder;

	taskENTER_CRITICAL();
	if ((s_ucPendingOrder == 0U) || (s_usManualReservations != 0U)) {
		taskEXIT_CRITICAL();
		return (s_ucPendingOrder != 0U) ? 1U : 0U;
	}
	xOrder = s_xPendingOrder;
	s_ucPendingOrder = 0U;
	taskEXIT_CRITICAL();
	if (xQueueSendToFront(s_xOrderQueue, &xOrder, 0U) != pdPASS) {
		taskENTER_CRITICAL();
		s_xPendingOrder = xOrder;
		s_ucPendingOrder = 1U;
		taskEXIT_CRITICAL();
		return 1U;
	}
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"ORDER_CANCELED", 0,
		"order", (int32_t)
			g_xCoffee3WorkflowStatus.usCurrentOrderId);
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		xOrder.ausRegister[COFFEE3_REG_ORDER_NUMBER],
		"ORDER_ACCEPTED", 0,
		"order", (int32_t)xOrder.ausRegister[
			COFFEE3_REG_ORDER_NUMBER]);
	return 1U;
}

/*-----------------------------------------------------------*/
static int32_t prvRunStep(uint16_t usStep, Coffee3DeviceId_e xDeviceId,
	Coffee3Action_e xAction, uint16_t usParameter0,
	uint16_t usParameter1, uint32_t ulTimeoutMs)
{
	/* Own one complete submit/wait/result lifecycle. A successful device
	 * transaction may still require a sensor postcondition in the caller. */
	Coffee3Command_t xCommand;
	EventBits_t xEvents;
	TickType_t xStartTick;
	TickType_t xTimeoutTicks;
	uint8_t ucOrderStep;
	uint16_t usLogOrder;
	uint8_t ucInitializationStep;
	uint8_t ucResultValid;
	uint8_t ucRobotMotion;
	uint8_t ucTimeoutLogged;
	int32_t lCommandResult;
	int32_t lInterleaveResult;
	const Coffee3DeviceBinding_t *pxBinding;

	/* FDxx initialization steps are diagnostics; lower steps are order steps. */
	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	pxBinding = pxCoffee3DeviceGetBinding(xDeviceId);
	ucInitializationStep = ((usStep >= 0xFD00U) &&
		(usStep < 0xFE00U)) ? 1U : 0U;
	ucRobotMotion = ((xDeviceId == COFFEE3_DEVICE_ROBOT) &&
		(prvRobotPositionAction(xAction) != 0U)) ? 1U : 0U;
	ucTimeoutLogged = 0U;
	usLogOrder = (ucOrderStep != 0U) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId :
		((s_usDetachedPickupLogOrder != 0U) ?
		 s_usDetachedPickupLogOrder : COFFEE3_LOG_ORDER_DEBUG);
	if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
		(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
		return COFFEE3_WORKFLOW_ERROR_CANCELED;
	}
	if (ucInitializationStep == 0U) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
			"Step start: %s %s step=%u",
			prvWorkflowDeviceName(xDeviceId), prvWorkflowActionName(xAction),
			(unsigned int)usStep);
	}
	if (ucOrderStep != 0U) {
		g_xCoffee3WorkflowStatus.ucDeviceId = (uint8_t)xDeviceId;
		g_xCoffee3WorkflowStatus.usAction = (uint16_t)xAction;
		g_xCoffee3WorkflowStatus.ucCommandSent = 0U;
		g_xCoffee3WorkflowStatus.ucDeviceDone = 0U;
		g_xCoffee3WorkflowStatus.ucPhysicalVerified = 0U;
		prvPublish(COFFEE3_WORKFLOW_RUNNING, usStep, 0);
	}
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId = usLogOrder;
	xCommand.ulOrderEpoch = ((ucOrderStep != 0U) ||
		(s_ucMaintenanceActive != 0U)) ?
		g_xCoffee3WorkflowStatus.ulOrderEpoch : 0U;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)xAction;
	xCommand.ausParameter[0] = usParameter0;
	xCommand.ausParameter[1] = usParameter1;
	if (xDeviceId == COFFEE3_DEVICE_ROBOT) {
		xCommand.ulTimeoutMs = ulTimeoutMs;
	} else if (xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) {
		xCommand.ulTimeoutMs = (xAction == COFFEE3_ACTION_COFFEE_MAKE) ?
			COFFEE3_COFFEE_ACTION_TIMEOUT_MS : COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS;
	} else {
		xCommand.ulTimeoutMs = COFFEE3_RTU_IO_TIMEOUT_MS;
	}
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = ((ucOrderStep != 0U) ||
		(s_ucMaintenanceActive != 0U)) ?
		(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee3CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) !=
		pdPASS) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			usLogOrder,
			"WORKFLOW_STEP_QUEUE_FAILED",
			COFFEE3_WORKFLOW_ERROR_QUEUE, "step", (int32_t)usStep);
		return COFFEE3_WORKFLOW_ERROR_QUEUE;
	}
	xStartTick = xTaskGetTickCount();
	if (ucOrderStep != 0U) {
		g_xCoffee3WorkflowStatus.ucCommandSent = 1U;
		if ((xDeviceId == COFFEE3_DEVICE_ROBOT) &&
			(prvRobotPositionAction(xAction) != 0U)) {
			g_xCoffee3WorkflowStatus.ucPositionUncertain = 1U;
		}
	}
	if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
		(xAction != COFFEE3_ACTION_REFRESH)) {
		s_usActiveDevices |= (uint16_t)(1U << (uint8_t)xDeviceId);
	}
	xTimeoutTicks = pdMS_TO_TICKS((xDeviceId == COFFEE3_DEVICE_ROBOT) ?
		ulTimeoutMs : (ulTimeoutMs +
			(4U * COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS)));
	if (ucRobotMotion != 0U) {
		/* Keep an independent diagnostic deadline for a command stranded before
		 * the Robot owner receives it. Expiry does not terminate Robot motion. */
		xTimeoutTicks = pdMS_TO_TICKS(COFFEE3_ROBOT_RECOVERY_TIMEOUT_MS +
			COFFEE3_ROBOT_ACCEPT_TIMEOUT_MS + COFFEE3_ROBOT_MOTION_TIMEOUT_MS +
			(COFFEE3_ROBOT_PREPARE_RETRY_LIMIT + 1U) *
			COFFEE3_ROBOT_PREPARE_RETRY_MS + 2U * COFFEE3_ROBOT_IO_TIMEOUT_MS);
	}
	for (;;) {
		xEvents = xCoffee3DeviceWaitCommand(xDeviceId,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		if ((xEvents & COFFEE3_DEVICE_EVENT_CANCELED) != 0U) {
			uint8_t ucTerminalValid;
			int32_t lTerminalResult;
			lTerminalResult = lCoffee3DeviceGetTerminalResult(xDeviceId,
				xCommand.ulOrderEpoch, xCommand.ulCommandId,
				&ucTerminalValid);
			if ((ucOrderStep != 0U) &&
				(xDeviceId == COFFEE3_DEVICE_ROBOT) &&
				(ucTerminalValid != 0U) &&
				(lTerminalResult == COFFEE3_COMMAND_RESULT_SUPERSEDED)) {
				s_ucManualOverride = 1U;
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					usLogOrder,
					"WORKFLOW_MANUAL_OVERRIDE",
					COFFEE3_COMMAND_RESULT_CANCELED,
					"step", (int32_t)usStep);
			}
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_CANCELED",
				COFFEE3_WORKFLOW_ERROR_CANCELED, "step",
				(int32_t)usStep);
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		if ((xEvents & COFFEE3_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			if (ucOrderStep != 0U) {
				g_xCoffee3WorkflowStatus.ucDeviceDone = 1U;
			}
			if (ucInitializationStep == 0U) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"Step done: %s %s step=%u result=0",
				prvWorkflowDeviceName(xDeviceId), prvWorkflowActionName(xAction),
				(unsigned int)usStep);
			}
			return 0;
		}
		if ((xEvents & COFFEE3_DEVICE_EVENT_TERMINAL) != 0U) {
			lCommandResult = lCoffee3DeviceGetTerminalResult(xDeviceId,
				xCommand.ulOrderEpoch, xCommand.ulCommandId, &ucResultValid);
			if (ucResultValid == 0U) {
				lCommandResult = COFFEE3_WORKFLOW_ERROR_DEVICE;
			}
			if (ucInitializationStep != 0U) {
				if (s_usLastInitializationFailure != usStep) {
					const char *pcDeviceName;
					switch (usStep) {
					case 0xFD10U:
						pcDeviceName = "IO_INPUT_MODULE_16CH";
						break;
					case 0xFD11U:
						pcDeviceName = "CUP_MACHINE";
						break;
					case 0xFD12U:
						pcDeviceName = "LID_MACHINE";
						break;
					default:
						pcDeviceName = "INITIALIZATION_DEVICE";
						break;
					}
					(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						COFFEE3_LOG_ORDER_SYSTEM,
						"%s_INIT_FAILED_RESULT=%ld",
						pcDeviceName,
						(long)lCommandResult);
					s_usLastInitializationFailure = usStep;
				}
			} else {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					usLogOrder,
					"Step %u %s failed: action=%u result=%ld",
					(unsigned int)usStep,
					(pxBinding != NULL) ? pxBinding->pcName : "Unknown",
					(unsigned int)xAction, (long)lCommandResult);
			}
			return COFFEE3_WORKFLOW_ERROR_DEVICE;
		}
		if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
			(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_CANCELED",
				COFFEE3_WORKFLOW_ERROR_CANCELED, "step",
				(int32_t)usStep);
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		/* Coffee1 permits pickup while the coffee machine owns the cup and the
		 * Robot is free. Reuse this wait loop as the scheduling point; the
		 * detached pickup steps remain serialized by the Robot owner. */
		if ((xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) &&
			(xAction == COFFEE3_ACTION_COFFEE_MAKE)) {
			lInterleaveResult = prvTryRunInterleavedStoragePickup();
			if (lInterleaveResult != 0) {
				return lInterleaveResult;
			}
		}
		if ((xTaskGetTickCount() - xStartTick) >= xTimeoutTicks) {
			if (ucRobotMotion != 0U) {
				if (ucTimeoutLogged == 0U) {
					ucTimeoutLogged = 1U;
					(void)xCoffee3LogPrintfOrder(
						COFFEE3_LOG_LEVEL_WARNING,
						COFFEE3_LOG_SOURCE_WORKFLOW,
						usLogOrder,
						"Step overdue: device=Robot action=%s step=%u; waiting",
						prvWorkflowActionName(xAction),
						(unsigned int)usStep);
				}
				prvServiceHotWater();
				prvServiceIoRefresh();
				continue;
			}
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_TIMEOUT",
				COFFEE3_WORKFLOW_ERROR_TIMEOUT, "step",
				(int32_t)usStep);
			return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
		prvServiceIoRefresh();
	}
}

/*-----------------------------------------------------------*/
static int32_t prvRunOrder(const Coffee3Order_t *pxOrder)
{
	/* Reserve storage/outlet resources before robot motion, then execute the
	 * ordered device sequence and verify each physical postcondition. */
	int32_t lResult;
	int32_t lValidationError;
	int32_t lIceEmptyBaselineGram;
	int32_t lIceCupBaselineGram;
	int32_t lIceEmptyDeltaGram;
	uint16_t usIceAmount;
	uint16_t usSyrupAmount;
	uint16_t usColdOrder;
	uint16_t usLidLane;
	uint16_t usOutput;
	uint8_t ucOffline;
	Coffee3IoState_t xIo;
	uint8_t ucNeedCoffeeStation;
	uint8_t ucNeedFlavorStation;

	lIceEmptyBaselineGram = 0;
	lIceCupBaselineGram = 0;
	lIceEmptyDeltaGram = 0;

	if (prvOrderValid(pxOrder, &lValidationError) == 0U) {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			(pxOrder != NULL) ? pxOrder->ausRegister[
				COFFEE3_REG_ORDER_NUMBER] : COFFEE3_LOG_ORDER_SYSTEM,
			"ORDER_VALIDATION_FAILED", lValidationError,
			"reason", lValidationError);
		return lValidationError;
	}
	if (prvOrderDevicesReady(pxOrder) == 0U) {
		return COFFEE3_WORKFLOW_ERROR_DEVICE;
	}
	usIceAmount = pxOrder->ausRegister[COFFEE3_REG_ICE_AMOUNT];
	usColdOrder = (usIceAmount != 0U) ? 1U : 0U;
	usLidLane = (usColdOrder != 0U) ? 2U : 1U;
	ucOffline = ((pxOrder->ausRegister[COFFEE3_REG_ORDER_NUMBER] & 0xF000U) == 0xD000U);
	usOutput = 1U;
	g_xCoffee3WorkflowStatus.ucContentComplete = 0U;
	g_xCoffee3WorkflowStatus.ucStorageReserved = 0U;
	lResult = prvRefreshDeviceQuiet(9U, COFFEE3_DEVICE_IO_INPUT);
	vCoffee3IoGetSnapshot(&xIo);
	if ((lResult != 0) || (prvIoValid(&xIo) == 0U) ||
		(xIo.xInput.aucMB1XPin[3] == 0U) ||
		(xIo.xInput.aucMB1XPin[4] == 0U) ||
		(xIo.xInput.aucMB1XPin[5] == 0U) ||
		(xIo.xInput.aucMB1XPin[6] != 0U)) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Order rejected before motion: IO, water, bins, or waste not ready");
		return COFFEE3_WORKFLOW_ERROR_IO;
	}
	lResult = (ucOffline != 0U) ? prvCheckOutputEmpty(1U) : prvSelectStorage();
	if ((ucOffline == 0U) && (lResult > 0)) {
		g_xCoffee3WorkflowStatus.ucStorageReserved = (uint8_t)lResult;
		lResult = 0;
	}
	if (lResult != 0) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW, g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Order rejected: %s unavailable; no production action sent",
			(ucOffline != 0U) ? "outlet" : "storage");
		return lResult;
	}
	ucNeedCoffeeStation =
		(pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE] != 0xFFFFU) ?
			1U : 0U;
	ucNeedFlavorStation =
		((pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_A] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_B] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_1] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_2] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_3] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_4] != 0U)) ? 1U : 0U;
	lResult = prvRunStep(10U, COFFEE3_DEVICE_ROBOT,
		COFFEE3_ACTION_ROBOT_PREPARE_ORDER, 0U, 0U, 30000U);
	if ((lResult == 0) && (ucNeedCoffeeStation != 0U)) {
		lResult = prvRunStep(20U, COFFEE3_DEVICE_COFFEE_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if (lResult == 0) {
		lResult = prvRunStep(22U, COFFEE3_DEVICE_CUP_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_LID_ENABLE] != 0U)) {
		lResult = prvRunStep(24U, COFFEE3_DEVICE_LID_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) &&
		((pxOrder->ausRegister[COFFEE3_REG_SYRUP_1] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_2] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_3] != 0U) ||
		(pxOrder->ausRegister[COFFEE3_REG_SYRUP_4] != 0U))) {
		lResult = prvRunStep(26U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) && (usIceAmount != 0U)) {
		lResult = prvRunStep(28U, COFFEE3_DEVICE_ICE_MACHINE,
			COFFEE3_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) && (ucOffline != 0U)) {
		lResult = prvCheckOutputEmpty(usOutput);
	}
	if (lResult == 0) {
		lResult = prvRunStep(40U, COFFEE3_DEVICE_ROBOT,
			(usColdOrder != 0U) ?
				COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP :
				COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP,
			0U, 0U, COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvRunStep(50U, COFFEE3_DEVICE_CUP_MACHINE,
			(usColdOrder != 0U) ?
				COFFEE3_ACTION_CUP_DROP_2 :
				COFFEE3_ACTION_CUP_DROP_1,
			0U, 0U, 3000U);
	}
	if (lResult == 0) {
		lResult = prvWaitBusinessCondition(55U,
			(usColdOrder != 0U) ? COFFEE3_CONDITION_CUP_2 :
				COFFEE3_CONDITION_CUP_1);
	}
	if ((lResult == 0) && (usIceAmount != 0U)) {
		lResult = prvReadStableScale(56U, &lIceEmptyBaselineGram);
		if (lResult == 0) {
			lIceEmptyDeltaGram = lIceEmptyBaselineGram -
				s_lIceScaleEmptyBaselineGram;
			if (lIceEmptyDeltaGram < 0) {
				lIceEmptyDeltaGram = -lIceEmptyDeltaGram;
			}
			if (lIceEmptyDeltaGram > COFFEE3_ICE_CUP_DETECT_GRAM) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Empty scale failed: current=%ld g startup=%ld g reason=load present",
					(long)lIceEmptyBaselineGram,
					(long)s_lIceScaleEmptyBaselineGram);
				lResult = COFFEE3_WORKFLOW_ERROR_ICE_BASELINE;
			} else {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Empty scale ready: current=%ld g startup=%ld g",
					(long)lIceEmptyBaselineGram,
					(long)s_lIceScaleEmptyBaselineGram);
			}
		}
	}
	if ((lResult == 0) && (usIceAmount != 0U)) {
		lResult = prvRunStep(60U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TO_ICE, 0U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvConfirmIceCup(lIceEmptyBaselineGram, 61U,
				&lIceCupBaselineGram);
		}
		if (lResult == 0) {
			lResult = prvDispenseIce(usIceAmount,
				lIceCupBaselineGram);
		}
	}
	if ((lResult == 0) && (ucNeedFlavorStation != 0U)) {
		lResult = prvRunStep(65U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TO_FRUIT_SYRUP, 0U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_A] != 0U)) {
		lResult = prvRunFruit(1U,
			pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_A], 0U, 66U);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_B] != 0U)) {
		lResult = prvRunFruit(2U,
			pxOrder->ausRegister[COFFEE3_REG_FRUIT_MILK_B], 0U, 72U);
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE3_REG_SYRUP_1];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(110U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_DISPENSE, 1U,
			(uint16_t)(usSyrupAmount *
				COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(111U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 1U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE3_REG_SYRUP_2];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(120U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_DISPENSE, 2U,
			(uint16_t)(usSyrupAmount *
				COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(121U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 2U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE3_REG_SYRUP_3];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(123U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_DISPENSE, 3U,
			(uint16_t)(usSyrupAmount *
				COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(124U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 3U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE3_REG_SYRUP_4];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(126U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_DISPENSE, 4U,
			(uint16_t)(usSyrupAmount *
				COFFEE3_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(127U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 4U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	if ((lResult == 0) && (ucNeedCoffeeStation != 0U)) {
		lResult = prvRunStep(70U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TO_COFFEE, 0U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvRunStep(75U, COFFEE3_DEVICE_ROBOT,
				COFFEE3_ACTION_ROBOT_TO_COFFEE, 1U, 0U,
				COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		}
		if ((lResult == 0) &&
			(pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE] != 0xFFFFU)) {
			lResult = prvRunStep(80U,
				COFFEE3_DEVICE_COFFEE_MACHINE,
				COFFEE3_ACTION_COFFEE_MAKE,
				pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE],
				0U, COFFEE3_COFFEE_ACTION_TIMEOUT_MS);
		}
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE] != 0xFFFFU)) {
		lResult = prvRunStep(90U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TAKE_COFFEE, 0U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE3_REG_LID_ENABLE] != 0U)) {
		lResult = prvRunStep(140U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_TO_LID, usLidLane, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvRunStep(150U,
				COFFEE3_DEVICE_LID_MACHINE,
				(usLidLane == 1U) ?
					COFFEE3_ACTION_LID_DROP_1 :
					COFFEE3_ACTION_LID_DROP_2,
				0U, 0U, 3000U);
		}
		if (lResult == 0) {
			lResult = prvWaitBusinessCondition(155U,
				(usLidLane == 1U) ? COFFEE3_CONDITION_LID_1 :
					COFFEE3_CONDITION_LID_2);
		}
		if (lResult == 0) {
			lResult = prvRunStep(160U, COFFEE3_DEVICE_ROBOT,
				COFFEE3_ACTION_ROBOT_TAKE_LID,
				0U, 0U, COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		}
		if (lResult == 0) {
			lResult = prvRunStep(170U, COFFEE3_DEVICE_ROBOT,
				COFFEE3_ACTION_ROBOT_COVER_LID,
				0U, 0U, COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		}
	}
	if (lResult != 0) {
		return lResult;
	}
	if (ucOffline == 0U) {
		uint8_t ucStorage;
		ucStorage = g_xCoffee3WorkflowStatus.ucStorageReserved;
		lResult = prvRefreshDeviceQuiet(179U, COFFEE3_DEVICE_IO_INPUT);
		vCoffee3IoGetSnapshot(&xIo);
		if ((lResult != 0) || (prvIoValid(&xIo) == 0U) ||
			(xIo.xInput.aucMB1XPin[ucStorage - 1U] != 0U)) {
			return COFFEE3_WORKFLOW_ERROR_INIT_OCCUPIED;
		}
		vCoffee3ServerSelectStorage(ucStorage);
		lResult = prvRunStep(180U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_PUT_STORAGE, ucStorage, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvWaitBusinessCondition(185U,
				(ucStorage == 1U) ? COFFEE3_CONDITION_STORAGE_1 :
					COFFEE3_CONDITION_STORAGE_2);
		}
		if (lResult == 0) {
			s_ausStoredOrderId[ucStorage - 1U] = g_xCoffee3WorkflowStatus.usCurrentOrderId;
		}
	} else {
		g_xCoffee3WorkflowStatus.ucContentComplete = 1U;
		vCoffee3ServerPublishWorkflow(g_xCoffee3WorkflowStatus.usCurrentOrderId,
			COFFEE3_PRODUCTION_COMPLETED, 179U, 0);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Content done; report production=2 before placement; no host ACK");
		/* An interleaved online pickup may still own the only outlet. Keep the
		 * current offline order alive until customer-take ACK releases it. */
		lResult = prvWaitOutputAvailableForOrder();
		if (lResult == 0) {
			prvPublishOutput(1U, 2U);
			lResult = prvRunStep(180U, COFFEE3_DEVICE_ROBOT,
				COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, 0U,
				COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
		}
		if (lResult == 0) {
			lResult = prvWaitBusinessCondition(185U, COFFEE3_CONDITION_OUTPUT_1);
		}
		if (lResult == 0) {
			prvPublishOutput(1U, 5U);
			s_ucOutletPhase = 1U;
			s_ucPickupPending = 0U;
			s_ucEmptyTiming = 0U;
			prvStartDoor(2U);
		} else {
			prvPublishOutput(1U, 3U);
		}
	}
	return lResult;
}

/*-----------------------------------------------------------*/
static int32_t prvConfirmIceCup(int32_t lEmptyBaselineGram,
	uint16_t usStepBase, int32_t *plCupBaselineGram)
{
	int32_t lResult;
	int32_t lCupWeightGram;
	int32_t lTotalWeightGram;
	uint8_t ucCommRetry;
	uint8_t ucNoCupAttempt;

	if (plCupBaselineGram == NULL) {
		return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	ucCommRetry = 0U;
	ucNoCupAttempt = 0U;
	lCupWeightGram = 0;
	lTotalWeightGram = 0;
	vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_CUP_FIRST_SETTLE_MS));
	for (;;) {
		lResult = prvReadStableScale(usStepBase, &lTotalWeightGram);
		if (lResult != 0) {
			ucCommRetry++;
			if (ucCommRetry >= COFFEE3_ICE_CUP_COMM_RETRIES) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Cup check failed: reason=scale read try=%u/%u result=%ld",
					(unsigned int)ucCommRetry,
					(unsigned int)COFFEE3_ICE_CUP_COMM_RETRIES,
					(long)lResult);
				return lResult;
			}
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Cup check retry: reason=scale read try=%u/%u result=%ld",
				(unsigned int)ucCommRetry,
				(unsigned int)COFFEE3_ICE_CUP_COMM_RETRIES,
				(long)lResult);
			vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_CUP_RETRY_MS));
			continue;
		}
		ucCommRetry = 0U;
		lCupWeightGram = lTotalWeightGram - lEmptyBaselineGram;
		*plCupBaselineGram = lTotalWeightGram;
		if (lCupWeightGram > COFFEE3_ICE_CUP_MAX_GRAM) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Cup check failed: reason=weight high cup=%ld g max=%ld g",
				(long)lCupWeightGram,
				(long)COFFEE3_ICE_CUP_MAX_GRAM);
			return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
		}
		if (lCupWeightGram > COFFEE3_ICE_CUP_DETECT_GRAM) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Cup confirmed: cup=%ld g total=%ld g empty=%ld g",
				(long)lCupWeightGram, (long)lTotalWeightGram,
				(long)lEmptyBaselineGram);
			return 0;
		}
		ucNoCupAttempt++;
		if (ucNoCupAttempt >= COFFEE3_ICE_CUP_DETECT_ATTEMPTS) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Cup check failed: reason=no cup weight=%ld g try=%u/%u",
				(long)lCupWeightGram, (unsigned int)ucNoCupAttempt,
				(unsigned int)COFFEE3_ICE_CUP_DETECT_ATTEMPTS);
			return COFFEE3_WORKFLOW_ERROR_ICE_NO_CUP;
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Cup check retry: reason=no cup weight=%ld g try=%u/%u",
			(long)lCupWeightGram, (unsigned int)ucNoCupAttempt,
			(unsigned int)COFFEE3_ICE_CUP_DETECT_ATTEMPTS);
		vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_CUP_RETRY_MS));
	}
}

/*-----------------------------------------------------------*/
static uint16_t prvStorageStatusMask(const Coffee3IoState_t *pxIo)
{
	uint16_t usPhysicalMask;

	if ((pxIo == NULL) || (prvIoValid(pxIo) == 0U)) {
		return 0U;
	}
	usPhysicalMask = (uint16_t)(
		((pxIo->xInput.aucMB1XPin[0] != 0U) ? 0x0001U : 0U) |
		((pxIo->xInput.aucMB1XPin[1] != 0U) ? 0x0002U : 0U));
	return (uint16_t)(usPhysicalMask & usCoffee3ConfigStorageMask() &
		COFFEE3_STORAGE_INSTALLED_MASK);
}

/*-----------------------------------------------------------*/
static void prvRejectStoragePickup(uint16_t usStorage,
	const char *pcReason, uint16_t usStatusMask)
{
	prvPublishOutputForOrder(1U, 3U, COFFEE3_LOG_ORDER_SYSTEM);
	vCoffee3ServerFinishRequest(1U);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
		"Pickup failed: device=Robot storage=%u outlet=1 reason=%s status_0x1027=0x%04X",
		(unsigned int)usStorage,
		(pcReason != NULL) ? pcReason : "unknown",
		(unsigned int)usStatusMask);
}

/*-----------------------------------------------------------*/
static int32_t prvDispenseIce(uint16_t usTargetGram,
	int32_t lCupBaselineGram)
{
	int32_t lResult;
	int32_t lCloseResult;
	int32_t lWeightGram;
	int32_t lTotalWeightGram;
	int32_t lDeficitGram;
	int32_t lSafeMinGram;
	int32_t lSafeMaxGram;
	int32_t lDeviationGram;
	uint32_t ulPulseMs;
	uint32_t ulWaitedMs;
	uint8_t ucAttempt;
	uint16_t usSlopeMsPerGram;

	if (usTargetGram == 0U) {
		return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	/* Integer-only safety bounds: lower is rounded up, upper is rounded down.
	 * This keeps the accepted integer-gram range inside 70%..130%. */
	lSafeMinGram = (int32_t)(((uint32_t)usTargetGram *
		COFFEE3_ICE_SAFE_MIN_PERCENT + 99U) / 100U);
	lSafeMaxGram = (int32_t)(((uint32_t)usTargetGram *
		COFFEE3_ICE_SAFE_MAX_PERCENT) / 100U);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"Ice start: target=%u g cup_base=%ld g",
		(unsigned int)usTargetGram, (long)lCupBaselineGram);
	/* Keep one calibration value for the entire dispense and correction cycle. */
	usSlopeMsPerGram = usCoffee3ConfigIceSlopeMsPerGram();
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, "ICE_SLOPE_SELECTED", 0,
		"ms_per_g", (int32_t)usSlopeMsPerGram);
	lWeightGram = 0;
	lTotalWeightGram = lCupBaselineGram;
	lDeficitGram = (int32_t)usTargetGram;
	for (ucAttempt = 0U;
		ucAttempt <= COFFEE3_ICE_MAX_CORRECTIONS; ucAttempt++) {
		ulPulseMs = prvCalculateIcePulseMs(
			(uint16_t)lDeficitGram, usSlopeMsPerGram,
			ucAttempt);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice valve open: try=%u/%u pulse=%lu ms deficit=%ld g",
			(unsigned int)(ucAttempt + 1U),
			(unsigned int)(COFFEE3_ICE_MAX_CORRECTIONS + 1U),
			(unsigned long)ulPulseMs, (long)lDeficitGram);
		lResult = prvRunStep((uint16_t)(101U + ucAttempt * 4U),
			COFFEE3_DEVICE_ICE_MACHINE,
			COFFEE3_ACTION_ICE_SET_VALVE, 1U, 0U, 3000U);
		ulWaitedMs = 0U;
		while ((lResult == 0) && (ulWaitedMs < ulPulseMs)) {
			if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
				lResult = COFFEE3_WORKFLOW_ERROR_CANCELED;
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_PULSE_STEP_MS));
			ulWaitedMs += COFFEE3_ICE_PULSE_STEP_MS;
		}
		lCloseResult = prvRunStep(
			(uint16_t)(102U + ucAttempt * 4U),
			COFFEE3_DEVICE_ICE_MACHINE,
			COFFEE3_ACTION_ICE_SET_VALVE, 0U, 0U, 3000U);
		if (lResult == 0) {
			lResult = lCloseResult;
		}
		if (lResult != 0) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice dispense failed: step=valve close result=%ld",
				(long)lResult);
			return lResult;
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice valve closed: try=%u reason=pulse complete result=0",
			(unsigned int)(ucAttempt + 1U));
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice weight wait: try=%u settle_ms=%u reason=ice settle",
			(unsigned int)(ucAttempt + 1U),
			(unsigned int)COFFEE3_ICE_SETTLE_MS);
		vTaskDelay(pdMS_TO_TICKS(COFFEE3_ICE_SETTLE_MS));
		lResult = prvReadStableScale(
			(uint16_t)(300U + (uint16_t)ucAttempt * 3U),
			&lTotalWeightGram);
		if (lResult != 0) {
			return lResult;
		}
		lWeightGram = lTotalWeightGram - lCupBaselineGram;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice weight: try=%u/%u target=%u g actual=%ld g total=%ld g last=%d",
			(unsigned int)(ucAttempt + 1U),
			(unsigned int)(COFFEE3_ICE_MAX_CORRECTIONS + 1U),
			(unsigned int)usTargetGram, (long)lWeightGram,
			(long)lTotalWeightGram,
			(int)g_xCoffee3ScaleImage.sRawValue);
		lDeficitGram = (int32_t)usTargetGram - lWeightGram;
		lDeviationGram = -lDeficitGram;
		if (lDeviationGram < 0) {
			lDeviationGram = -lDeviationGram;
		}
		if (lDeviationGram > COFFEE3_ICE_TOLERANCE_GRAM) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice weight warning: target=%u g actual=%ld g deviation=%ld g",
				(unsigned int)usTargetGram, (long)lWeightGram,
				(long)(-lDeficitGram));
		}
		/* Ice cannot be removed after it is dispensed. A high result is therefore
		 * final immediately; only the configured safety range decides failure. */
		if (lWeightGram > lSafeMaxGram) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice failed: reason=above 130%% target=%u g actual=%ld g limit=%ld g",
				(unsigned int)usTargetGram, (long)lWeightGram,
				(long)lSafeMaxGram);
			return COFFEE3_WORKFLOW_ERROR_ICE_RANGE;
		}
		if (lDeficitGram <= 0) {
			(void)xCoffee3LogPrintfOrder(
				(lDeviationGram > COFFEE3_ICE_TOLERANCE_GRAM) ?
				COFFEE3_LOG_LEVEL_WARNING : COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice done: reason=target reached target=%u g actual=%ld g",
				(unsigned int)usTargetGram, (long)lWeightGram);
			return 0;
		}
		if (ucAttempt == COFFEE3_ICE_MAX_CORRECTIONS) {
			if (lWeightGram < lSafeMinGram) {
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Ice failed: reason=below 70%% target=%u g actual=%ld g limit=%ld g",
					(unsigned int)usTargetGram, (long)lWeightGram,
					(long)lSafeMinGram);
				return COFFEE3_WORKFLOW_ERROR_ICE_RANGE;
			}
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Ice done: reason=low but safe target=%u g actual=%ld g limit=%ld g",
				(unsigned int)usTargetGram, (long)lWeightGram,
				(long)lSafeMinGram);
			return 0;
		}
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Ice correction: target=%u g actual=%ld g deficit=%ld g",
			(unsigned int)usTargetGram, (long)lWeightGram,
			(long)lDeficitGram);
	}
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"Ice failed: reason=correction limit target=%u g actual=%ld g corrections=%u",
		(unsigned int)usTargetGram, (long)lWeightGram,
		(unsigned int)COFFEE3_ICE_MAX_CORRECTIONS);
	return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
}

/*-----------------------------------------------------------*/
static uint32_t prvCalculateIcePulseMs(uint16_t usTargetGram,
	uint16_t usSlopeMsPerGram, uint8_t ucAttempt)
{
	int32_t lPulseMs;
	int32_t lMaximumMs;

	lPulseMs = (int32_t)usTargetGram * (int32_t)usSlopeMsPerGram;
	lPulseMs += COFFEE3_ICE_CORRECTION_OFFSET_MS;
	lMaximumMs = (int32_t)COFFEE3_ICE_MAX_PULSE_MS;
	if (ucAttempt == 0U) {
		lMaximumMs = (usTargetGram < COFFEE3_ICE_INITIAL_SPLIT_GRAM) ?
			(int32_t)COFFEE3_ICE_INITIAL_SMALL_MS :
			(int32_t)COFFEE3_ICE_INITIAL_LARGE_MS;
	}
	if (lPulseMs < (int32_t)COFFEE3_ICE_MIN_PULSE_MS) {
		lPulseMs = (int32_t)COFFEE3_ICE_MIN_PULSE_MS;
	}
	lPulseMs = ((lPulseMs + (int32_t)COFFEE3_ICE_PULSE_STEP_MS - 1L) /
		(int32_t)COFFEE3_ICE_PULSE_STEP_MS) *
		(int32_t)COFFEE3_ICE_PULSE_STEP_MS;
	if (lPulseMs > lMaximumMs) {
		lPulseMs = lMaximumMs;
	}
	return (uint32_t)lPulseMs;
}

/*-----------------------------------------------------------*/
static int32_t prvReadStableScale(uint16_t usStepBase,
	int32_t *plWeightGram)
{
	int32_t alSample[3];
	int32_t lSwap;
	int32_t lResult;
	uint8_t ucIndex;

	if (plWeightGram == NULL) {
		return COFFEE3_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		lResult = prvRunStep((uint16_t)(usStepBase + ucIndex),
			COFFEE3_DEVICE_SCALE, COFFEE3_ACTION_REFRESH,
			0U, 0U, 3000U);
		if (lResult != 0) {
			return lResult;
		}
		alSample[ucIndex] = g_xCoffee3ScaleImage.lWeightGram;
		vTaskDelay(pdMS_TO_TICKS(100U));
	}
	if (alSample[0] > alSample[1]) {
		lSwap = alSample[0];
		alSample[0] = alSample[1];
		alSample[1] = lSwap;
	}
	if (alSample[1] > alSample[2]) {
		lSwap = alSample[1];
		alSample[1] = alSample[2];
		alSample[2] = lSwap;
	}
	if (alSample[0] > alSample[1]) {
		lSwap = alSample[0];
		alSample[0] = alSample[1];
		alSample[1] = lSwap;
	}
	*plWeightGram = alSample[1];
	return 0;
}

/*-----------------------------------------------------------*/
static int32_t prvRunIoOutput(uint16_t usStep, uint8_t ucPoint,
	uint8_t ucValue)
{
	Coffee3Command_t xCommand;
	EventBits_t xEvents;
	TickType_t xStartTick;
	uint8_t ucTerminalValid;
	int32_t lTerminalResult;

	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId =
		(g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_RUNNING) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId :
		COFFEE3_LOG_ORDER_DEBUG;
	xCommand.ulOrderEpoch = g_xCoffee3WorkflowStatus.ulOrderEpoch;
	xCommand.ulTimeoutMs = COFFEE3_RTU_IO_TIMEOUT_MS;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)COFFEE3_ACTION_IO_WRITE;
	xCommand.ausParameter[0] = ucPoint;
	xCommand.ausParameter[1] = (ucValue != 0U) ? 1U : 0U;
	xCommand.ucDeviceId = (uint8_t)COFFEE3_DEVICE_IO_OUTPUT;
	xCommand.ucSource =
		(g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_RUNNING) ?
		(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee3CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) != pdPASS) {
		return COFFEE3_WORKFLOW_ERROR_QUEUE;
	}
	xStartTick = xTaskGetTickCount();
	for (;;) {
		xEvents = xCoffee3DeviceWaitCommand(COFFEE3_DEVICE_IO_OUTPUT,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		if ((xEvents & COFFEE3_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			return 0;
		}
		if ((xEvents & COFFEE3_DEVICE_EVENT_TERMINAL) != 0U) {
			lTerminalResult = lCoffee3DeviceGetTerminalResult(
				COFFEE3_DEVICE_IO_OUTPUT, xCommand.ulOrderEpoch,
				xCommand.ulCommandId, &ucTerminalValid);
			return (ucTerminalValid != 0U) ? lTerminalResult :
				COFFEE3_WORKFLOW_ERROR_IO;
		}
		if ((xTaskGetTickCount() - xStartTick) >=
			pdMS_TO_TICKS(COFFEE3_WORKFLOW_IO_ACTION_TIMEOUT_MS)) {
			return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
	}
}

/*-----------------------------------------------------------*/
static int32_t prvSetProductOutputsOff(void)
{
	static const uint8_t aucPoints[] = {
		COFFEE3_EXTERNAL_DO_WATER_HEATER_RELAY,
		COFFEE3_EXTERNAL_DO_MILK_VALVE,
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_PUMP,
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_PUMP,
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_VALVE,
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_VALVE
	};
	int32_t lResult;
	uint8_t ucIndex;

	(void)ucCoffee3IoSetLocalOutput(
		COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
	lResult = 0;
	for (ucIndex = 0U;
		ucIndex < (sizeof(aucPoints) / sizeof(aucPoints[0])); ucIndex++) {
		if (prvRunIoOutput((uint16_t)(0xFE00U + ucIndex),
			aucPoints[ucIndex], 0U) != 0) {
			lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
		}
	}
	return lResult;
}

/*-----------------------------------------------------------*/
static int32_t prvRunInitialization(void)
{
	int32_t lResult;
	uint8_t ucSource;
	static const Coffee3Action_e axSources[3] = {
		(Coffee3Action_e)0, COFFEE3_ACTION_ROBOT_TAKE_COFFEE,
		COFFEE3_ACTION_ROBOT_TAKE_LID
	};

	/* Called only after robot and fresh input-module feedback are available.
	 * Missing prerequisites before the first move are waiting, not failure. */
	for (ucSource = 0U; ucSource < 3U; ucSource++) {
		lResult = prvProbeResidualCup((uint16_t)(0xFD20U + 4U * ucSource),
			axSources[ucSource], (uint8_t)(ucSource + 1U));
		if (lResult != 0) {
			return lResult;
		}
	}
	return 0;
}

/*-----------------------------------------------------------*/
static int32_t prvProbeResidualCup(uint16_t usStepBase,
	Coffee3Action_e xPickupAction, uint8_t ucOccupiedPoint)
{
	Coffee3IoState_t xIo;
	int32_t lResult;
	int32_t lStorage;
	uint8_t ucFirstCup;
	uint8_t ucCupPresent;
	uint8_t ucMismatchLogged;
	const char *pcIoName;

	lStorage = prvSelectStorage();
	if (lStorage < 1) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
			"Residual source=%u has no verified empty storage; init failed (%ld)",
			(unsigned int)ucOccupiedPoint, (long)lStorage);
		return lStorage;
	}
	vCoffee3ServerSelectStorage((uint16_t)lStorage);
	lResult = 0;
	if ((uint16_t)xPickupAction != 0U) {
		lResult = prvRunStep(usStepBase, COFFEE3_DEVICE_ROBOT,
			xPickupAction, 0U, 0U, COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvRunStep((uint16_t)(usStepBase + 1U), COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_PUT_STORAGE, (uint16_t)lStorage, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult != 0) {
		return lResult;
	}
	prvDelayWithServices(COFFEE3_RESIDUAL_SETTLE_MS);
	pcIoName = (lStorage == 1) ? COFFEE3_IO_NAME_MB1_DI_1 :
		COFFEE3_IO_NAME_MB1_DI_2;
	ucMismatchLogged = 0U;
	for (;;) {
		lResult = prvRefreshDeviceQuiet((uint16_t)(usStepBase + 2U),
			COFFEE3_DEVICE_IO_INPUT);
		vCoffee3IoGetSnapshot(&xIo);
		if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
			return (lResult != 0) ? lResult : COFFEE3_WORKFLOW_ERROR_IO;
		}
		ucFirstCup = xIo.xInput.aucMB1XPin[lStorage - 1];
		prvDelayWithServices(COFFEE3_RESIDUAL_SAMPLE_GAP_MS);
		lResult = prvRefreshDeviceQuiet((uint16_t)(usStepBase + 2U),
			COFFEE3_DEVICE_IO_INPUT);
		vCoffee3IoGetSnapshot(&xIo);
		if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
			return (lResult != 0) ? lResult : COFFEE3_WORKFLOW_ERROR_IO;
		}
		ucCupPresent = xIo.xInput.aucMB1XPin[lStorage - 1];
		if (ucFirstCup == ucCupPresent) {
			break;
		}
		if (ucMismatchLogged == 0U) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
				"Residual %s unstable: %u/%u; retry",
				pcIoName, (unsigned int)ucFirstCup,
				(unsigned int)ucCupPresent);
			ucMismatchLogged = 1U;
		}
	}
	(void)xCoffee3LogPrintfOrder(
		(ucCupPresent != 0U) ?
			COFFEE3_LOG_LEVEL_WARNING : COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
		"Residual source=%u storage=%ld io=%s cup=%u",
		(unsigned int)ucOccupiedPoint, (long)lStorage,
		pcIoName, (unsigned int)ucCupPresent);
	return 0;
}

/*-----------------------------------------------------------*/
static int32_t prvRunFruit(uint8_t ucChannel, uint16_t usAmountMl,
	uint8_t ucClean, uint16_t usStepBase)
{
	Coffee3IoState_t xIo;
	uint8_t ucInputPoint;
	uint8_t ucValvePoint;
	uint8_t ucPumpPoint;
	uint8_t ucValveRunValue;
	uint32_t ulRunMs;
	uint16_t usLogOrder;
	int32_t lResult;

	if ((ucChannel < 1U) || (ucChannel > 2U) ||
		((ucClean == 0U) && (usAmountMl == 0U))) {
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucInputPoint = (ucChannel == 1U) ?
		COFFEE3_EXTERNAL_DI_FRUIT_MILK_A_LOW :
		COFFEE3_EXTERNAL_DI_FRUIT_MILK_B_LOW;
	ucValvePoint = (ucChannel == 1U) ?
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_VALVE :
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_VALVE;
	ucPumpPoint = (ucChannel == 1U) ?
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_A_PUMP :
		COFFEE3_EXTERNAL_DO_FRUIT_MILK_B_PUMP;
	ucValveRunValue = ((ucClean != 0U) && (ucChannel == 1U)) ? 1U : 0U;
	usLogOrder = (g_xCoffee3WorkflowStatus.xState ==
		COFFEE3_WORKFLOW_RUNNING) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId : COFFEE3_LOG_ORDER_DEBUG;
	g_xCoffee3WorkflowStatus.aucFruitState[ucChannel - 1U] =
		COFFEE3_MAINTENANCE_RUNNING;
	lResult = prvRunStep(usStepBase,
		COFFEE3_DEVICE_IO_INPUT, COFFEE3_ACTION_REFRESH,
		0U, 0U, 3000U);
	if (lResult == 0) {
		vCoffee3IoGetSnapshot(&xIo);
		if (xIo.xInput.aucMB1XPin[ucInputPoint] != 0U) {
			lResult = COFFEE3_WORKFLOW_ERROR_DEVICE;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"FRUIT_LOW_LEVEL", lResult, "channel",
				(int32_t)ucChannel);
		}
	}
	if (lResult == 0) {
		lResult = prvRunIoOutput((uint16_t)(usStepBase + 1U),
			ucValvePoint, ucValveRunValue);
	}
	if (lResult == 0) {
		lResult = prvRunIoOutput((uint16_t)(usStepBase + 2U),
			ucPumpPoint, 1U);
	}
	if (lResult == 0) {
		ulRunMs = (ucClean != 0U) ? COFFEE3_FRUIT_MILK_CLEAN_MS :
			((uint32_t)usAmountMl *
			usCoffee3ConfigFruitCoefficient(ucChannel));
		prvDelayWithServices(ulRunMs);
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			lResult = COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
	}
	if (prvRunIoOutput((uint16_t)(usStepBase + 3U),
		ucPumpPoint, 0U) != 0) {
		lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
	}
	if (prvRunIoOutput((uint16_t)(usStepBase + 4U),
		ucValvePoint, 0U) != 0) {
		lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
	}
	g_xCoffee3WorkflowStatus.aucFruitState[ucChannel - 1U] =
		(lResult == 0) ? COFFEE3_MAINTENANCE_COMPLETED :
		COFFEE3_MAINTENANCE_FAILED;
	return lResult;
}

/*-----------------------------------------------------------*/
static int32_t prvRunMaintenance(const Coffee3MaintenanceRequest_t *pxRequest)
{
	int32_t lResult;

	if (pxRequest == NULL) {
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	switch (pxRequest->xType) {
	case COFFEE3_MAINTENANCE_SYRUP_CLEAN:
		lResult = prvRunStep(0xFB10U, COFFEE3_DEVICE_SYRUP_MACHINE,
			COFFEE3_ACTION_SYRUP_CLEAN, 0U, 0U, 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(0xFB11U,
				COFFEE3_DEVICE_SYRUP_MACHINE, 5U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
		return lResult;
	case COFFEE3_MAINTENANCE_COFFEE_CLEAN:
		g_xCoffee3WorkflowStatus.ucCoffeeCleanState =
			COFFEE3_MAINTENANCE_RUNNING;
		lResult = prvRunStep(0xFB20U,
			COFFEE3_DEVICE_COFFEE_MACHINE,
			COFFEE3_ACTION_COFFEE_CLEAN,
			pxRequest->usParameter0, 0U, 3000U);
		if (lResult == 0) {
			lResult = prvWaitM50Clean();
		}
		g_xCoffee3WorkflowStatus.ucCoffeeCleanState =
			(lResult == 0) ? COFFEE3_MAINTENANCE_COMPLETED :
			COFFEE3_MAINTENANCE_FAILED;
		return lResult;
	case COFFEE3_MAINTENANCE_FRUIT_DISPENSE:
		return prvRunFruit((uint8_t)pxRequest->usParameter0,
			pxRequest->usParameter1, 0U,
			(uint16_t)(0xFC10U + (pxRequest->usParameter0 * 8U)));
	case COFFEE3_MAINTENANCE_FRUIT_CLEAN:
		return prvRunFruit((uint8_t)pxRequest->usParameter0, 0U, 1U,
			(uint16_t)(0xFC30U + (pxRequest->usParameter0 * 8U)));
	default:
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvOrderValid(const Coffee3Order_t *pxOrder,
	int32_t *plError)
{
	uint16_t usCoffeeType;
	uint8_t ucOffline;
	if (plError != NULL) {
		*plError = COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	if (pxOrder == NULL) {
		return 0U;
	}
	usCoffeeType = pxOrder->ausRegister[COFFEE3_REG_COFFEE_TYPE];
	if ((usCoffeeType != 0xFFFFU) && (usCoffeeType > COFFEE3_COFFEE_RECIPE_MAX)) {
		return 0U;
	}
	ucOffline = ((pxOrder->ausRegister[COFFEE3_REG_ORDER_NUMBER] & 0xF000U) == 0xD000U);
	if ((pxOrder->ausRegister[COFFEE3_REG_ICE_AMOUNT] > 6553U) ||
		((ucOffline != 0U) &&
		(pxOrder->ausRegister[COFFEE3_REG_OFFLINE_OUTPUT] != 1U))) {
		return 0U;
	}
	if (plError != NULL) {
		*plError = 0;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
static int32_t prvRefreshDeviceQuiet(uint16_t usStep,
	Coffee3DeviceId_e xDeviceId)
{
	Coffee3Command_t xCommand;
	EventBits_t xEvents;
	TickType_t xStartTick;
	uint8_t ucTerminalValid;
	uint8_t ucOrderStep;
	int32_t lTerminalResult;

	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT)) {
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId = (ucOrderStep != 0U) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId : COFFEE3_LOG_ORDER_DEBUG;
	xCommand.ulOrderEpoch = ((ucOrderStep != 0U) ||
		(s_ucMaintenanceActive != 0U)) ?
		g_xCoffee3WorkflowStatus.ulOrderEpoch : 0U;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)COFFEE3_ACTION_REFRESH;
	xCommand.ulTimeoutMs =
		(xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) ?
			COFFEE3_WORKFLOW_DEVICE_IO_TIMEOUT_MS :
			COFFEE3_RTU_IO_TIMEOUT_MS;
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = ((ucOrderStep != 0U) ||
		(s_ucMaintenanceActive != 0U)) ?
		(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee3CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) != pdPASS) {
		return COFFEE3_WORKFLOW_ERROR_QUEUE;
	}
	xStartTick = xTaskGetTickCount();
	for (;;) {
		xEvents = xCoffee3DeviceWaitCommand(xDeviceId,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		if ((xEvents & COFFEE3_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			return 0;
		}
		if ((xEvents & COFFEE3_DEVICE_EVENT_TERMINAL) != 0U) {
			lTerminalResult = lCoffee3DeviceGetTerminalResult(xDeviceId,
				xCommand.ulOrderEpoch, xCommand.ulCommandId,
				&ucTerminalValid);
			return (ucTerminalValid != 0U) ? lTerminalResult :
				COFFEE3_WORKFLOW_ERROR_DEVICE;
		}
		if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
			(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		if ((xTaskGetTickCount() - xStartTick) >= pdMS_TO_TICKS(5000U)) {
			return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvBusinessConditionActive(uint8_t ucCondition)
{
	Coffee3IoState_t xIo;

	switch (ucCondition) {
	case COFFEE3_CONDITION_CUP_1:
		return (g_xCoffee3CupLidImage.aucCupCoils[4U] != 0U) ? 1U : 0U;
	case COFFEE3_CONDITION_CUP_2:
		return (g_xCoffee3CupLidImage.aucCupCoils[9U] != 0U) ? 1U : 0U;
	case COFFEE3_CONDITION_LID_1:
		return (g_xCoffee3CupLidImage.aucLidCoils[4U] != 0U) ? 1U : 0U;
	case COFFEE3_CONDITION_LID_2:
		return (g_xCoffee3CupLidImage.aucLidCoils[9U] != 0U) ? 1U : 0U;
	case COFFEE3_CONDITION_OUTPUT_1:
	case COFFEE3_CONDITION_STORAGE_1:
	case COFFEE3_CONDITION_STORAGE_2:
		vCoffee3IoGetSnapshot(&xIo);
		if (prvIoValid(&xIo) == 0U) {
			return 0U;
		}
		return xIo.xInput.aucMB1XPin[
			(ucCondition == COFFEE3_CONDITION_OUTPUT_1) ? 2U :
			((ucCondition == COFFEE3_CONDITION_STORAGE_1) ? 0U : 1U)];
	default:
		return 0U;
	}
}

/*-----------------------------------------------------------*/
static int32_t prvWaitBusinessCondition(uint16_t usStep,
	uint8_t ucCondition)
{
	Coffee3DeviceId_e xDeviceId;
	int32_t lResult;
	uint8_t ucWaitLogged;
	uint8_t ucOverdueLogged;
	uint8_t ucPhysicalPlacement;
	uint8_t ucDelayIndex;
	TickType_t xStart;
	const char *pcIoName;

	ucWaitLogged = 0U;
	ucOverdueLogged = 0U;
	ucPhysicalPlacement = 0U;
	pcIoName = NULL;
	xStart = xTaskGetTickCount();
	switch (ucCondition) {
	case COFFEE3_CONDITION_CUP_1:
	case COFFEE3_CONDITION_CUP_2:
		xDeviceId = COFFEE3_DEVICE_CUP_MACHINE;
		break;
	case COFFEE3_CONDITION_LID_1:
	case COFFEE3_CONDITION_LID_2:
		xDeviceId = COFFEE3_DEVICE_LID_MACHINE;
		break;
	case COFFEE3_CONDITION_OUTPUT_1:
		xDeviceId = COFFEE3_DEVICE_IO_INPUT;
		ucPhysicalPlacement = 1U;
		pcIoName = "X3_OUTLET_CUP";
		break;
	case COFFEE3_CONDITION_STORAGE_1:
		xDeviceId = COFFEE3_DEVICE_IO_INPUT;
		ucPhysicalPlacement = 1U;
		pcIoName = "X1_FINISHED_FRONT_CUP";
		break;
	case COFFEE3_CONDITION_STORAGE_2:
		xDeviceId = COFFEE3_DEVICE_IO_INPUT;
		ucPhysicalPlacement = 1U;
		pcIoName = "X2_FINISHED_REAR_CUP";
		break;
	default:
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	prvPublish(COFFEE3_WORKFLOW_RUNNING, usStep, 0);
	for (;;) {
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		if ((xTaskGetTickCount() - xStart) >=
			pdMS_TO_TICKS(COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS)) {
			if (ucPhysicalPlacement == 0U) {
				return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
			}
			if (ucOverdueLogged == 0U) {
				ucOverdueLogged = 1U;
				(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"Physical confirmation overdue: device=IoInput io=%s; waiting",
					pcIoName);
			}
		}
		lResult = prvRefreshDeviceQuiet(usStep, xDeviceId);
		if (lResult != 0) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_REFRESH_FAILED", lResult,
				"condition", (int32_t)ucCondition);
			return lResult;
		}
		if (prvBusinessConditionActive(ucCondition) != 0U) {
			g_xCoffee3WorkflowStatus.ucPhysicalVerified = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_READY", 0,
				"condition", (int32_t)ucCondition);
			return 0;
		}
		if (ucWaitLogged == 0U) {
			ucWaitLogged = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_WAIT", 0,
				"condition", (int32_t)ucCondition);
		}
		for (ucDelayIndex = 0U; ucDelayIndex < 5U; ucDelayIndex++) {
			if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
				return COFFEE3_WORKFLOW_ERROR_CANCELED;
			}
			prvServiceHotWater();
			vTaskDelay(pdMS_TO_TICKS(100U));
		}
	}
}

/*-----------------------------------------------------------*/
static int32_t prvWaitDeviceReportedComplete(uint16_t usStep,
	Coffee3DeviceId_e xDeviceId, uint8_t ucStatusIndex,
	uint16_t usSuccessValue, uint16_t usFailedValue)
{
	int32_t lResult;
	uint16_t usState;
	uint16_t usLogOrder;
	uint8_t ucWaitLogged;
	uint8_t ucOrderStep;
	uint8_t ucDelayIndex;
	TickType_t xStart;

	if (((xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) &&
		(ucStatusIndex >= 24U)) ||
		((xDeviceId == COFFEE3_DEVICE_SYRUP_MACHINE) &&
		(ucStatusIndex >= SYRUP_MACHINE_REGISTER_COUNT)) ||
		((xDeviceId != COFFEE3_DEVICE_COFFEE_MACHINE) &&
		(xDeviceId != COFFEE3_DEVICE_SYRUP_MACHINE))) {
		return COFFEE3_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	usLogOrder = (ucOrderStep != 0U) ?
		g_xCoffee3WorkflowStatus.usCurrentOrderId : COFFEE3_LOG_ORDER_DEBUG;
	ucWaitLogged = 0U;
	xStart = xTaskGetTickCount();
	if (ucOrderStep != 0U) {
		prvPublish(COFFEE3_WORKFLOW_RUNNING, usStep, 0);
	}
	for (;;) {
		if ((xTaskGetTickCount() - xStart) >=
			pdMS_TO_TICKS(COFFEE3_COFFEE_ACTION_TIMEOUT_MS)) {
			return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
		}
		if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
			(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		lResult = prvRefreshDeviceQuiet(usStep, xDeviceId);
		if (lResult != 0) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_REFRESH_FAILED", lResult,
				"device", (int32_t)xDeviceId);
			return lResult;
		}
		usState = (xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) ?
			g_xCoffee3CoffeeMachineImage.ausStatus[ucStatusIndex] :
			g_xCoffee3SyrupImage.ausRegisters[ucStatusIndex];
		if (usState == usSuccessValue) {
			s_usActiveDevices &= (uint16_t)~(1U << (uint8_t)xDeviceId);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_COMPLETE", 0,
				"device", (int32_t)xDeviceId);
			return 0;
		}
		if (usState == usFailedValue) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_FAILED", COFFEE3_WORKFLOW_ERROR_DEVICE,
				"device", (int32_t)xDeviceId);
			return COFFEE3_WORKFLOW_ERROR_DEVICE;
		}
		if (ucWaitLogged == 0U) {
			ucWaitLogged = 1U;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_WAIT", (int32_t)usState,
				"device", (int32_t)xDeviceId);
		}
		for (ucDelayIndex = 0U; ucDelayIndex < 5U; ucDelayIndex++) {
			if (((ucOrderStep != 0U) || (s_ucMaintenanceActive != 0U)) &&
				(g_xCoffee3WorkflowStatus.ucCancelRequested != 0U)) {
				return COFFEE3_WORKFLOW_ERROR_CANCELED;
			}
			prvServiceHotWater();
			vTaskDelay(pdMS_TO_TICKS(100U));
		}
	}
}

/*-----------------------------------------------------------*/
static void prvDelayWithServices(uint32_t ulDelayMs)
{
	TickType_t xStartTick;

	xStartTick = xTaskGetTickCount();
	while ((xTaskGetTickCount() - xStartTick) < pdMS_TO_TICKS(ulDelayMs)) {
		prvServiceHotWater();
		prvServiceIoRefresh();
		vTaskDelay(pdMS_TO_TICKS(100U));
	}
}

/*-----------------------------------------------------------*/
static BaseType_t prvSubmitHotWaterIo(uint8_t ucValue)
{
	if (s_xHotWater.ucIoPending != 0U) {
		return pdFAIL;
	}
	memset(&s_xHotWater.xCommand, 0, sizeof(s_xHotWater.xCommand));
	s_xHotWater.xCommand.ulOrderId = COFFEE3_LOG_ORDER_DEBUG;
	s_xHotWater.xCommand.usAction = (uint16_t)COFFEE3_ACTION_IO_WRITE;
	s_xHotWater.xCommand.ausParameter[0] =
		COFFEE3_EXTERNAL_DO_WATER_HEATER_RELAY;
	s_xHotWater.xCommand.ausParameter[1] = (ucValue != 0U) ? 1U : 0U;
	s_xHotWater.xCommand.ulTimeoutMs = COFFEE3_RTU_IO_TIMEOUT_MS;
	s_xHotWater.xCommand.ucDeviceId =
		(uint8_t)COFFEE3_DEVICE_IO_OUTPUT;
	s_xHotWater.xCommand.ucSource =
		(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	s_xHotWater.xCommand.ucRetryLimit = 1U;
	if (xCoffee3CommandSubmit(&s_xHotWater.xCommand, 0U) != pdPASS) {
		return pdFAIL;
	}
	s_xHotWater.ucIoPending = 1U;
	return pdPASS;
}

/*-----------------------------------------------------------*/
static int32_t prvPollHotWaterIo(uint8_t *pucDone)
{
	uint8_t ucValid;
	int32_t lResult;

	if (pucDone == NULL) {
		return COFFEE3_WORKFLOW_ERROR_IO;
	}
	*pucDone = 0U;
	if (s_xHotWater.ucIoPending == 0U) {
		return 0;
	}
	lResult = lCoffee3DeviceGetTerminalResult(
		COFFEE3_DEVICE_IO_OUTPUT,
		s_xHotWater.xCommand.ulOrderEpoch,
		s_xHotWater.xCommand.ulCommandId, &ucValid);
	if (ucValid == 0U) {
		return 0;
	}
	s_xHotWater.ucIoPending = 0U;
	*pucDone = 1U;
	return lResult;
}

/*-----------------------------------------------------------*/
static void prvSetHotWaterPublicState(uint8_t ucState,
	const char *pcEvent, int32_t lResult)
{
	if (g_xCoffee3WorkflowStatus.ucHotWaterState == ucState) {
		return;
	}
	g_xCoffee3WorkflowStatus.ucHotWaterState = ucState;
	if (ucState == COFFEE3_MAINTENANCE_ALARM) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_ALARM;
	}
	if (pcEvent != NULL) {
		(void)xCoffee3LogWriteFieldOrder(
			(ucState == COFFEE3_MAINTENANCE_ALARM) ?
				COFFEE3_LOG_LEVEL_ERROR : COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
			pcEvent, lResult, "state", (int32_t)ucState);
	}
}

/*-----------------------------------------------------------*/
static void prvServiceCoffeeFill(void)
{
	Coffee3IoState_t xIo;
	TickType_t xNow;
	uint8_t ucAllowed;
	uint8_t ucTimedOut;

	vCoffee3IoGetSnapshot(&xIo);
	xNow = xTaskGetTickCount();
	ucAllowed = ((prvIoValid(&xIo) != 0U) &&
		(xIo.xInput.aucMB1XPin[3] != 0U) &&
		(xIo.xInput.aucMB1XPin[5] != 0U) &&
		(xIo.xInput.aucMB1XPin[6] == 0U)) ? 1U : 0U;
	if (s_ucCoffeeFillActive != 0U) {
		ucTimedOut = ((xNow - s_xCoffeeFillStart) >=
			pdMS_TO_TICKS(20000U)) ? 1U : 0U;
		if ((ucAllowed == 0U) || (ucTimedOut != 0U) ||
			(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_COFFEE_WATER_HIGH] != 0U)) {
			(void)ucCoffee3IoSetLocalOutput(COFFEE3_LOCAL_DO_COFFEE_WATER_PUMP, 0U);
			s_ucCoffeeFillActive = 0U;
			if (ucTimedOut != 0U) {
				s_ucCoffeeFillAlarm = 1U;
			}
			(void)xCoffee3LogPrintfOrder((ucTimedOut != 0U) ?
				COFFEE3_LOG_LEVEL_WARNING : COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_SYSTEM,
				"M50 tank fill stopped: high=%u supply_ok=%u timeout=%u; DO4 off",
				(unsigned int)xIo.xInput.aucXPin[2],
				(unsigned int)ucAllowed, (unsigned int)ucTimedOut);
		}
		return;
	}
	taskENTER_CRITICAL();
	if ((ucAllowed != 0U) &&
		(s_ucCoffeeFillAlarm == 0U) && (s_ucOtaReserved == 0U) &&
		((s_usManualReservations == 0U) ||
		 (g_xCoffee3WorkflowStatus.xState == COFFEE3_WORKFLOW_RUNNING) ||
		 (s_ucMaintenanceActive != 0U)) &&
		(g_xCoffee3WorkflowStatus.ucRecoveryRequired == 0U) &&
		(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_COFFEE_WATER_LOW] == 0U) &&
		(xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_COFFEE_WATER_HIGH] == 0U)) {
		s_ucCoffeeFillActive = 1U;
		s_xCoffeeFillStart = xNow;
	}
	taskEXIT_CRITICAL();
	if (s_ucCoffeeFillActive != 0U) {
		(void)ucCoffee3IoSetLocalOutput(COFFEE3_LOCAL_DO_COFFEE_WATER_PUMP, 1U);
	}
}

/*-----------------------------------------------------------*/
static void prvServiceHotWater(void)
{
	Coffee3IoState_t xIo;
	TickType_t xNow;
	uint32_t ulHeatMs;
	uint8_t ucDone;
	int32_t lResult;

	prvServiceIoRefresh();
	prvServiceCoffeeFill();
	if ((s_xHotWater.ucPhase == COFFEE3_HOT_WATER_IDLE) &&
		(s_xHotWater.ucCancelRequested == 0U)) {
		return;
	}
	vCoffee3IoRefreshLocal();
	vCoffee3IoGetSnapshot(&xIo);
	xNow = xTaskGetTickCount();
	if ((s_xHotWater.ucPhase < COFFEE3_HOT_WATER_WAIT_OFF_DONE) &&
		((prvIoValid(&xIo) == 0U) ||
		(xIo.xInput.aucMB1XPin[COFFEE3_EXTERNAL_DI_PURE_WATER_LOW] == 0U))) {
		s_xHotWater.ucCancelRequested = 1U;
	}
	if (s_xHotWater.ucCancelRequested != 0U) {
		(void)ucCoffee3IoSetLocalOutput(
			COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
		if (s_xHotWater.ucIoPending != 0U) {
			(void)prvPollHotWaterIo(&ucDone);
			return;
		}
		if (s_xHotWater.ucIoPending == 0U) {
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_DONE;
				s_xHotWater.ucCancelRequested = 0U;
			}
		}
		return;
	}
	switch ((Coffee3HotWaterPhase_e)s_xHotWater.ucPhase) {
	case COFFEE3_HOT_WATER_PREPARE_OFF:
		if (s_xHotWater.ucIoPending == 0U) {
			(void)prvSubmitHotWaterIo(0U);
			return;
		}
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone == 0U) {
			return;
		}
		if (lResult != 0) {
			s_xHotWater.ucAlarmReason = 3U;
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			return;
		}
		if (xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			s_xHotWater.ucAlarmReason = 1U;
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			return;
		}
		(void)ucCoffee3IoSetLocalOutput(
			COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 1U);
		s_xHotWater.xStartTick = xNow;
		s_xHotWater.ucPhase = COFFEE3_HOT_WATER_FILLING;
		prvSetHotWaterPublicState(COFFEE3_MAINTENANCE_RUNNING,
			"HOT_WATER_FILLING", 0);
		break;
	case COFFEE3_HOT_WATER_FILLING:
		if (xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			(void)ucCoffee3IoSetLocalOutput(
				COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			s_xHotWater.ucAlarmReason = 1U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			}
		} else if (xIo.xInput.aucXPin[
			COFFEE3_LOCAL_DI_HOT_WATER_LOW] != 0U) {
			(void)ucCoffee3IoSetLocalOutput(
				COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
				"HOT_WATER_FILL_COMPLETE", 0, "low_level", 1);
			if (prvSubmitHotWaterIo(1U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_HEATER_ON;
			}
		} else if ((xNow - s_xHotWater.xStartTick) >=
			pdMS_TO_TICKS(COFFEE3_HOT_WATER_FILL_TIMEOUT_MS)) {
			(void)ucCoffee3IoSetLocalOutput(
				COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			s_xHotWater.ucAlarmReason = 2U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			}
		}
		break;
	case COFFEE3_HOT_WATER_WAIT_HEATER_ON:
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone == 0U) {
			break;
		}
		if (lResult != 0) {
			s_xHotWater.ucAlarmReason = 3U;
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			break;
		}
		s_xHotWater.xStartTick = xNow;
		s_xHotWater.ucPhase = COFFEE3_HOT_WATER_HEATING;
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, COFFEE3_LOG_ORDER_DEBUG,
			"HOT_WATER_HEATING", 0, "minutes",
			(int32_t)s_xHotWater.usHeatMinutes);
		break;
	case COFFEE3_HOT_WATER_HEATING:
		ulHeatMs = (uint32_t)s_xHotWater.usHeatMinutes * 60000U;
		if (xIo.xInput.aucXPin[COFFEE3_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			s_xHotWater.ucAlarmReason = 1U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_ALARM;
			}
		} else if ((xNow - s_xHotWater.xStartTick) >=
			pdMS_TO_TICKS(ulHeatMs)) {
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE3_HOT_WATER_WAIT_OFF_DONE;
			}
		}
		break;
	case COFFEE3_HOT_WATER_WAIT_OFF_DONE:
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone != 0U) {
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_IDLE;
			prvSetHotWaterPublicState(
				(lResult == 0) ? COFFEE3_MAINTENANCE_COMPLETED :
					COFFEE3_MAINTENANCE_ALARM,
				(lResult == 0) ? "HOT_WATER_COMPLETE" :
					"HOT_WATER_STOP_FAILED", lResult);
		}
		break;
	case COFFEE3_HOT_WATER_WAIT_OFF_ALARM:
		if (s_xHotWater.ucIoPending == 0U) {
			(void)prvSubmitHotWaterIo(0U);
			break;
		}
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone != 0U) {
			s_xHotWater.ucPhase = COFFEE3_HOT_WATER_IDLE;
			prvSetHotWaterPublicState(COFFEE3_MAINTENANCE_ALARM,
				"HOT_WATER_ALARM", (lResult != 0) ? lResult :
					-(int32_t)s_xHotWater.ucAlarmReason);
		}
		break;
	default:
		s_xHotWater.ucPhase = COFFEE3_HOT_WATER_IDLE;
		break;
	}
}

/*-----------------------------------------------------------*/
static int32_t prvAbortDevices(void)
{
	Coffee3Command_t axCommand[3];
	EventBits_t xEvents;
	BaseType_t axSubmitted[3];
	uint8_t ucIndex;
	int32_t lResult;

	prvPublish(COFFEE3_WORKFLOW_CANCELING,
		g_xCoffee3WorkflowStatus.usCurrentStep,
		g_xCoffee3WorkflowStatus.lLastError);
	s_xHotWater.ucCancelRequested = 1U;
	(void)ucCoffee3IoSetLocalOutput(
		COFFEE3_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
	vCoffee3OrderCancelRequest(g_xCoffee3WorkflowStatus.ulOrderEpoch);
	memset(axCommand, 0, sizeof(axCommand));
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		axCommand[ucIndex].ucSource =
			(uint8_t)COFFEE3_COMMAND_SOURCE_WORKFLOW;
		axCommand[ucIndex].ucFlags = COFFEE3_COMMAND_FLAG_SAFETY_STOP;
		axCommand[ucIndex].ulOrderId =
			g_xCoffee3WorkflowStatus.usCurrentOrderId;
		axCommand[ucIndex].ulOrderEpoch =
			g_xCoffee3WorkflowStatus.ulOrderEpoch;
		axCommand[ucIndex].ulTimeoutMs =
			COFFEE3_WORKFLOW_SAFE_STOP_MS;
		axCommand[ucIndex].usStepId = 0xFFF0U + ucIndex;
	}
	axCommand[0].ucDeviceId = (uint8_t)COFFEE3_DEVICE_ICE_MACHINE;
	axCommand[0].usAction = (uint16_t)COFFEE3_ACTION_ICE_SET_VALVE;
	axCommand[0].ausParameter[0] = 0U;
	axCommand[1].ucDeviceId =
		(uint8_t)COFFEE3_DEVICE_COFFEE_MACHINE;
	axCommand[1].usAction = (uint16_t)COFFEE3_ACTION_CANCEL;
	axCommand[2].ucDeviceId = (uint8_t)COFFEE3_DEVICE_ROBOT;
	axCommand[2].usAction = (uint16_t)COFFEE3_ACTION_CANCEL;

	lResult = 0;
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		if ((s_usActiveDevices & (1U << axCommand[ucIndex].ucDeviceId)) == 0U) {
			axSubmitted[ucIndex] = pdFAIL;
			continue;
		}
		if ((ucIndex == 2U) && (s_ucManualOverride != 0U)) {
			axSubmitted[ucIndex] = pdPASS;
			continue;
		}
		axSubmitted[ucIndex] = xCoffee3CommandSubmitUrgent(
			&axCommand[ucIndex], pdMS_TO_TICKS(100U));
		if (axSubmitted[ucIndex] != pdPASS) {
			lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"SAFE_STOP_QUEUE_FAILED", lResult, "device",
				(int32_t)axCommand[ucIndex].ucDeviceId);
		}
	}
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		if (axSubmitted[ucIndex] != pdPASS) {
			continue;
		}
		if ((ucIndex == 2U) && (s_ucManualOverride != 0U)) {
			continue;
		}
		xEvents = xCoffee3DeviceWaitCommand(
			(Coffee3DeviceId_e)axCommand[ucIndex].ucDeviceId,
			axCommand[ucIndex].ulOrderEpoch,
			axCommand[ucIndex].ulCommandId,
			pdMS_TO_TICKS(COFFEE3_WORKFLOW_SAFE_STOP_MS));
		{
			uint8_t ucValid;
			int32_t lStopResult;
			lStopResult = lCoffee3DeviceGetTerminalResult(
				(Coffee3DeviceId_e)axCommand[ucIndex].ucDeviceId,
				axCommand[ucIndex].ulOrderEpoch,
				axCommand[ucIndex].ulCommandId, &ucValid);
			if (((xEvents & COFFEE3_DEVICE_EVENT_TERMINAL) == 0U) ||
				(ucValid == 0U) || (lStopResult != 0)) {
				lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
				(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_ERROR,
					COFFEE3_LOG_SOURCE_WORKFLOW,
					g_xCoffee3WorkflowStatus.usCurrentOrderId,
					"SAFE_STOP_ACK_FAILED", lResult, "device",
					(int32_t)axCommand[ucIndex].ucDeviceId);
			}
		}
	}
	if (prvSetProductOutputsOff() != 0) {
		lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
	}
	if ((s_usActiveDevices & (1U << COFFEE3_DEVICE_SYRUP_MACHINE)) != 0U) {
		/* No verified syrup STOP command exists in this device protocol. */
		lResult = COFFEE3_WORKFLOW_ERROR_SAFE_STOP;
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW,
			g_xCoffee3WorkflowStatus.usCurrentOrderId,
			"Syrup action unresolved: STOP unsupported; inspect before reset");
	}
	(void)xCoffee3LogWriteOrder((lResult == 0) ? COFFEE3_LOG_LEVEL_INFO :
		COFFEE3_LOG_LEVEL_ERROR,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		(lResult == 0) ? "SAFE_STOP_CONFIRMED" :
			"SAFE_STOP_INCOMPLETE", lResult);
	return lResult;
}

/*-----------------------------------------------------------*/
static void prvServiceIoRefresh(void)
{
	/* RTU owners now refresh their own devices. Workflow only samples local
	 * GPIO here; it must not flood every Bus queue with periodic reads. */
	prvServicePickup();
	vCoffee3IoRefreshLocal();
}

/*-----------------------------------------------------------*/
static const char *prvWorkflowDeviceName(Coffee3DeviceId_e xDeviceId)
{
	switch (xDeviceId) {
	case COFFEE3_DEVICE_ROBOT: return "Robot";
	case COFFEE3_DEVICE_COFFEE_MACHINE: return "CoffeeMachine";
	case COFFEE3_DEVICE_CUP_MACHINE: return "CupMachine";
	case COFFEE3_DEVICE_SYRUP_MACHINE: return "SyrupMachine";
	case COFFEE3_DEVICE_LID_MACHINE: return "LidMachine";
	case COFFEE3_DEVICE_ICE_MACHINE: return "IceMachine";
	case COFFEE3_DEVICE_SCALE: return "Weigh Scale";
	case COFFEE3_DEVICE_POWER_METER: return "EnergyMeter";
	case COFFEE3_DEVICE_IO_INPUT: return "IoInput";
	case COFFEE3_DEVICE_IO_OUTPUT: return "IoOutput";
	default: return "UnknownDevice";
	}
}

/*-----------------------------------------------------------*/
/*-----------------------------------------------------------*/
static const char *prvWorkflowActionName(Coffee3Action_e xAction)
{
 switch (xAction) {
 case COFFEE3_ACTION_REFRESH: return "status refresh";
 case COFFEE3_ACTION_SCALE_TARE: return "tare scale";
 case COFFEE3_ACTION_SCALE_CLEAR_TARE: return "clear scale tare";
 case COFFEE3_ACTION_SCALE_ZERO: return "zero scale";
 case COFFEE3_ACTION_ICE_SET_VALVE: return "set ice valve";
 case COFFEE3_ACTION_COFFEE_MAKE: return "make coffee";
 case COFFEE3_ACTION_COFFEE_CLEAN: return "clean coffee";
 case COFFEE3_ACTION_SYRUP_DISPENSE: return "dispense syrup";
 case COFFEE3_ACTION_SYRUP_CLEAN: return "clean syrup";
 case COFFEE3_ACTION_CUP_DROP_1: return "drop cup 1";
 case COFFEE3_ACTION_CUP_DROP_2: return "drop cup 2";
 case COFFEE3_ACTION_LID_DROP_1: return "drop lid 1";
 case COFFEE3_ACTION_LID_DROP_2: return "drop lid 2";
 case COFFEE3_ACTION_ROBOT_HOME: return "robot home";
 case COFFEE3_ACTION_ROBOT_TO_COFFEE: return "move to coffee";
 case COFFEE3_ACTION_ROBOT_TO_ICE: return "move to ice";
 case COFFEE3_ACTION_ROBOT_TO_LID: return "move to lid";
 case COFFEE3_ACTION_ROBOT_TAKE_HOT_CUP: return "take hot cup";
 case COFFEE3_ACTION_ROBOT_TAKE_COLD_CUP: return "take cold cup";
 case COFFEE3_ACTION_ROBOT_TAKE_COFFEE: return "take coffee";
 case COFFEE3_ACTION_ROBOT_TAKE_LID: return "take lid";
 case COFFEE3_ACTION_ROBOT_COVER_LID: return "cover lid";
 case COFFEE3_ACTION_ROBOT_PUT_OUTPUT: return "put output";
 case COFFEE3_ACTION_ROBOT_PUT_STORAGE: return "put storage";
 case COFFEE3_ACTION_ROBOT_TAKE_STORAGE: return "take storage";
 case COFFEE3_ACTION_ROBOT_START: return "robot start";
 case COFFEE3_ACTION_ROBOT_STOP: return "robot stop";
 case COFFEE3_ACTION_ROBOT_ENABLE: return "robot enable";
 case COFFEE3_ACTION_ROBOT_CLEAR_ALARM: return "clear robot alarm";
 case COFFEE3_ACTION_IO_WRITE: return "write IO";
 case COFFEE3_ACTION_IO_WRITE_MASK: return "write IO mask";
 case COFFEE3_ACTION_CANCEL: return "cancel";
 default: return "unknown action";
 }
}
static void prvPublish(Coffee3WorkflowState_e xState,
	uint16_t usStep, int32_t lError)
{
	uint16_t usProductionStatus;

	taskENTER_CRITICAL();
	g_xCoffee3WorkflowStatus.xState = xState;
	g_xCoffee3WorkflowStatus.usCurrentStep = usStep;
	g_xCoffee3WorkflowStatus.lLastError = lError;
	if ((g_xCoffee3WorkflowStatus.ucHotWaterState ==
		COFFEE3_MAINTENANCE_ALARM) ||
		(((s_ucWaterAlarm != 0U) || (s_ucCoffeeFillAlarm != 0U)) &&
		 (xState != COFFEE3_WORKFLOW_RUNNING))) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_ALARM;
	} else if ((xState == COFFEE3_WORKFLOW_RUNNING) ||
		(xState == COFFEE3_WORKFLOW_CANCELING)) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_BUSY;
	} else if (xState == COFFEE3_WORKFLOW_FAILED) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_ALARM;
	} else if (g_xCoffee3WorkflowStatus.xMachineState !=
		COFFEE3_MACHINE_INITIALIZING) {
		g_xCoffee3WorkflowStatus.xMachineState = COFFEE3_MACHINE_IDLE;
	}
	taskEXIT_CRITICAL();
	switch (xState) {
	case COFFEE3_WORKFLOW_RUNNING:
	case COFFEE3_WORKFLOW_CANCELING:
		usProductionStatus = COFFEE3_PRODUCTION_RUNNING;
		break;
	case COFFEE3_WORKFLOW_COMPLETED:
		usProductionStatus = COFFEE3_PRODUCTION_COMPLETED;
		break;
	case COFFEE3_WORKFLOW_FAILED:
		usProductionStatus = COFFEE3_PRODUCTION_FAILED;
		break;
	default:
		usProductionStatus = COFFEE3_PRODUCTION_IDLE;
		break;
	}
	if (s_ucMaintenanceActive != 0U) {
		return;
	}
	if ((g_xCoffee3WorkflowStatus.ucContentComplete != 0U) &&
		(usProductionStatus == COFFEE3_PRODUCTION_RUNNING)) {
		usProductionStatus = COFFEE3_PRODUCTION_COMPLETED;
	}
	vCoffee3ServerPublishWorkflow(
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		usProductionStatus, usStep, lError);
}


/*-----------------------------------------------------------*/
static int32_t prvWaitM50Clean(void)
{
	TickType_t xStart;
	int32_t lResult;
	uint8_t ucWorking;

	xStart = xTaskGetTickCount();
	ucWorking = 0U;
	while ((xTaskGetTickCount() - xStart) <
		pdMS_TO_TICKS(COFFEE3_COFFEE_ACTION_TIMEOUT_MS)) {
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			return COFFEE3_WORKFLOW_ERROR_CANCELED;
		}
		lResult = prvRefreshDeviceQuiet(0xFB21U, COFFEE3_DEVICE_COFFEE_MACHINE);
		if (lResult != 0) {
			return lResult;
		}
		if (g_xCoffee3CoffeeMachineImage.ausStatus[0] !=
			g_xCoffeeMachineM50Config.usIdleValue) {
			ucWorking = 1U;
		} else if (ucWorking != 0U) {
			s_usActiveDevices &= (uint16_t)~(1U << COFFEE3_DEVICE_COFFEE_MACHINE);
			return 0;
		}
		prvDelayWithServices(500U);
	}
	return COFFEE3_WORKFLOW_ERROR_TIMEOUT;
}

/*-----------------------------------------------------------*/
static int32_t prvTryRunInterleavedStoragePickup(void)
{
	Coffee3IoState_t xIo;
	uint16_t usStorage;
	uint16_t usStorageBit;
	uint16_t usStatusMask;
	uint16_t usStoredOrderId;
	uint8_t ucPhysicalCup;
	int32_t lResult;

	/* A previously accepted debug action has the next free Robot boundary.
	 * Pickup remains pending; neither request is turned into a queue. */
	taskENTER_CRITICAL();
	usStorage = s_ucStoragePickupPending;
	if ((usStorage == 0U) || (s_ucStoragePickupActive != 0U) ||
		(s_ucStoragePickupConfirmStorage != 0U) ||
		(s_usManualReservations != 0U)) {
		usStorage = 0U;
	}
	taskEXIT_CRITICAL();
	if (usStorage == 0U) {
		return 0;
	}
	if ((usStorage < 1U) || (usStorage > 2U)) {
		taskENTER_CRITICAL();
		s_ucStoragePickupPending = 0U;
		taskEXIT_CRITICAL();
		prvRejectStoragePickup(usStorage, "storage unsupported", 0U);
		return 0;
	}
	/* 0x0007 is a host request gate, not a statement that the accepted order
	 * has finished. Defer until the host clears the request latch. */
	if (usCoffee3ServerGetCommandRegister(
		COFFEE3_REG_ORDER_PRESENT) != 0U) {
		return 0;
	}
	if (prvCheckOutputEmpty(1U) != 0) {
		return 0;
	}
	vCoffee3IoGetSnapshot(&xIo);
	usStorageBit = (uint16_t)(1U << (usStorage - 1U));
	usStatusMask = prvStorageStatusMask(&xIo);
	ucPhysicalCup = ((prvIoValid(&xIo) != 0U) &&
		(xIo.xInput.aucMB1XPin[usStorage - 1U] != 0U)) ? 1U : 0U;

	/* Claim after the read-only checks. A debug reservation arriving before
	 * this boundary wins; once claimed, Robot ownership is exclusive. */
	taskENTER_CRITICAL();
	if ((s_ucStoragePickupPending != (uint8_t)usStorage) ||
		(s_usManualReservations != 0U) ||
		(s_ucStoragePickupActive != 0U)) {
		taskEXIT_CRITICAL();
		return 0;
	}
	s_ucStoragePickupPending = 0U;
	s_ucStoragePickupActive = 1U;
	taskEXIT_CRITICAL();

	if ((usCoffee3ConfigStorageMask() & usStorageBit &
		COFFEE3_STORAGE_INSTALLED_MASK) == 0U) {
		prvRejectStoragePickup(usStorage, "storage disabled", usStatusMask);
		s_ucStoragePickupActive = 0U;
		return 0;
	}
	if (ucPhysicalCup == 0U) {
		prvRejectStoragePickup(usStorage, "cup not detected", usStatusMask);
		s_ucStoragePickupActive = 0U;
		return 0;
	}
	if ((usStatusMask & usStorageBit) == 0U) {
		prvRejectStoragePickup(usStorage, "0x1027 storage bit not set",
			usStatusMask);
		s_ucStoragePickupActive = 0U;
		return 0;
	}

	usStoredOrderId = s_ausStoredOrderId[usStorage - 1U];
	s_usDetachedPickupLogOrder = (usStoredOrderId != 0U) ?
		usStoredOrderId : COFFEE3_LOG_ORDER_SYSTEM;
	g_xCoffee3WorkflowStatus.ucPositionUncertain = 1U;
	s_usActiveDevices |= (uint16_t)(1U << COFFEE3_DEVICE_ROBOT);
	vCoffee3ServerSelectStorage(usStorage);
	prvPublishOutputForOrder(1U, 2U, s_usDetachedPickupLogOrder);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, s_usDetachedPickupLogOrder,
		"Pickup interleave start: device=Robot storage=%u outlet=1 status_0x1027=0x%04X",
		(unsigned int)usStorage, (unsigned int)usStatusMask);
	lResult = prvRunStep(0xFC80U, COFFEE3_DEVICE_ROBOT,
		COFFEE3_ACTION_ROBOT_TAKE_STORAGE, usStorage, 0U,
		COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	if (lResult == 0) {
		lResult = prvRunStep(0xFC81U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
		s_usActiveDevices &= (uint16_t)~(1U << COFFEE3_DEVICE_ROBOT);
		taskENTER_CRITICAL();
		s_ucStoragePickupConfirmStorage = (uint8_t)usStorage;
		s_usStoragePickupConfirmOrderId = usStoredOrderId;
		s_xStoragePickupConfirmStart = xTaskGetTickCount();
		s_ucStoragePickupConfirmOverdueLogged = 0U;
		s_ucStoragePickupActive = 0U;
		taskEXIT_CRITICAL();
		vCoffee3ServerFinishRequest(1U);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_WORKFLOW, s_usDetachedPickupLogOrder,
			"Pickup Robot released: storage=%u outlet=1; wait X3=1 and source=0",
			(unsigned int)usStorage);
		s_usDetachedPickupLogOrder = 0U;
		return 0;
	}

	prvPublishOutputForOrder(1U, 3U, s_usDetachedPickupLogOrder);
	vCoffee3ServerFinishRequest(1U);
	s_ucStoragePickupActive = 0U;
	s_usDetachedPickupLogOrder = 0U;
	return lResult;
}

/*-----------------------------------------------------------*/
static int32_t prvRunStoragePickup(uint16_t usStorage)
{
	Coffee3IoState_t xIo;
	int32_t lResult;
	uint16_t usStoredOrderId;
	uint16_t usStorageBit;
	uint16_t usStatusMask;
	uint8_t ucStorageClearOverdueLogged;
	TickType_t xStorageClearStart;
	const char *pcStorageIoName;

	if ((usStorage < 1U) || (usStorage > 2U)) {
		prvRejectStoragePickup(usStorage, "storage unsupported", 0U);
		return 0;
	}
	usStorageBit = (uint16_t)(1U << (usStorage - 1U));
	/* Recheck the host order gate immediately before any robot motion. */
	if (usCoffee3ServerGetCommandRegister(
		COFFEE3_REG_ORDER_PRESENT) != 0U) {
		prvRejectStoragePickup(usStorage, "0x0007 order request not cleared", 0U);
		return 0;
	}
	if ((usCoffee3ConfigStorageMask() & usStorageBit &
		COFFEE3_STORAGE_INSTALLED_MASK) == 0U) {
		prvRejectStoragePickup(usStorage, "storage disabled", 0U);
		return 0;
	}
	lResult = prvCheckOutputEmpty(1U);
	if (lResult != 0) {
		prvRejectStoragePickup(usStorage, "outlet unavailable", 0U);
		return 0;
	}
	vCoffee3IoGetSnapshot(&xIo);
	usStatusMask = prvStorageStatusMask(&xIo);
	if (prvIoValid(&xIo) == 0U) {
		prvRejectStoragePickup(usStorage, "IoInput stale", usStatusMask);
		return 0;
	}
	if (xIo.xInput.aucMB1XPin[usStorage - 1U] == 0U) {
		prvRejectStoragePickup(usStorage, "cup not detected", usStatusMask);
		return 0;
	}
	if ((usStatusMask & usStorageBit) == 0U) {
		prvRejectStoragePickup(usStorage, "0x1027 storage bit not set",
			usStatusMask);
		return 0;
	}
	/* The host pickup request plus the physical cup sensor are authoritative.
	 * The RAM order id is optional correlation and is lost after a restart. */
	usStoredOrderId = s_ausStoredOrderId[usStorage - 1U];
	g_xCoffee3WorkflowStatus.usCurrentOrderId = (usStoredOrderId != 0U) ?
		usStoredOrderId : COFFEE3_LOG_ORDER_SYSTEM;
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW,
		g_xCoffee3WorkflowStatus.usCurrentOrderId,
		"Pickup start: device=Robot storage=%u outlet=1 tracked_order=%u status_0x1027=0x%04X",
		(unsigned int)usStorage, (unsigned int)usStoredOrderId,
		(unsigned int)usStatusMask);
	vCoffee3ServerSelectStorage(usStorage);
	prvPublishOutput(1U, 2U);
	lResult = prvRunStep(800U, COFFEE3_DEVICE_ROBOT,
		COFFEE3_ACTION_ROBOT_TAKE_STORAGE, usStorage, 0U,
		COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	if (lResult == 0) {
		lResult = prvRunStep(810U, COFFEE3_DEVICE_ROBOT,
			COFFEE3_ACTION_ROBOT_PUT_OUTPUT, 1U, 0U,
			COFFEE3_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvWaitBusinessCondition(815U, COFFEE3_CONDITION_OUTPUT_1);
	}
	ucStorageClearOverdueLogged = 0U;
	xStorageClearStart = xTaskGetTickCount();
	pcStorageIoName = (usStorage == 1U) ?
		"X1_FINISHED_FRONT_CUP" : "X2_FINISHED_REAR_CUP";
	while (lResult == 0) {
		if (g_xCoffee3WorkflowStatus.ucCancelRequested != 0U) {
			lResult = COFFEE3_WORKFLOW_ERROR_CANCELED;
			break;
		}
		lResult = prvRefreshDeviceQuiet(816U, COFFEE3_DEVICE_IO_INPUT);
		vCoffee3IoGetSnapshot(&xIo);
		if ((lResult != 0) || (prvIoValid(&xIo) == 0U)) {
			lResult = (lResult != 0) ? lResult : COFFEE3_WORKFLOW_ERROR_IO;
			break;
		}
		if (xIo.xInput.aucMB1XPin[usStorage - 1U] == 0U) {
			break;
		}
		if ((ucStorageClearOverdueLogged == 0U) &&
			((xTaskGetTickCount() - xStorageClearStart) >=
			 pdMS_TO_TICKS(COFFEE3_WORKFLOW_DEFAULT_TIMEOUT_MS))) {
			ucStorageClearOverdueLogged = 1U;
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_WORKFLOW,
				g_xCoffee3WorkflowStatus.usCurrentOrderId,
				"Physical confirmation overdue: device=IoInput io=%s expected=0; waiting",
				pcStorageIoName);
		}
		prvDelayWithServices(500U);
	}
	if (lResult != 0) {
		prvPublishOutput(1U, 3U);
		return lResult;
	}
	s_ausStoredOrderId[usStorage - 1U] = 0U;
	prvPublishOutput(1U, 5U);
	s_ucOutletPhase = 1U;
	s_ucPickupPending = 0U;
	s_ucEmptyTiming = 0U;
	prvStartDoor(2U);
	g_xCoffee3WorkflowStatus.ucPositionUncertain = 0U;
	return lResult;
}








