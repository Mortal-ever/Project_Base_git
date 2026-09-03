/**
  * @file      coffee2_workflow.c
  * @brief     Implement the Coffee2 order state machine and failure path.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee2_workflow.h"

#include <stddef.h>
#include <string.h>

#include "coffee2_device.h"
#include "coffee2_device_image.h"
#include "coffee2_io.h"
#include "coffee2_log.h"
#include "coffee2_server.h"
#include "queue.h"
#include "task.h"

/** @brief Workflow-specific failure results. */
#define COFFEE2_WORKFLOW_ERROR_QUEUE          (-1001)
#define COFFEE2_WORKFLOW_ERROR_TIMEOUT        (-1002)
#define COFFEE2_WORKFLOW_ERROR_DEVICE         (-1003)
#define COFFEE2_WORKFLOW_ERROR_CANCELED       (-1004)
#define COFFEE2_WORKFLOW_ERROR_ICE_WEIGHT     (-1006)
#define COFFEE2_WORKFLOW_ERROR_SAFE_STOP      (-1007)
#define COFFEE2_WORKFLOW_ERROR_UNSUPPORTED    (-1008)
#define COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED  (-1009)
#define COFFEE2_WORKFLOW_ERROR_IO             (-1010)

/** @brief Maximum time allowed for each safety-stop acknowledgement. */
#define COFFEE2_WORKFLOW_SAFE_STOP_MS          5000U
#define COFFEE2_WORKFLOW_ROBOT_MOTION_MS       60000U
#define COFFEE2_CONDITION_CUP_1                1U
#define COFFEE2_CONDITION_CUP_2                2U
#define COFFEE2_CONDITION_LID_1                3U
#define COFFEE2_CONDITION_LID_2                4U
#define COFFEE2_CONDITION_OUTPUT_1             5U
#define COFFEE2_CONDITION_OUTPUT_2             6U
#define COFFEE2_F200_APPLICATION_COMPLETE       3U
#define COFFEE2_F200_APPLICATION_FAILED         4U

typedef enum {
	COFFEE2_HOT_WATER_IDLE = 0,
	COFFEE2_HOT_WATER_PREPARE_OFF = 1,
	COFFEE2_HOT_WATER_FILLING = 2,
	COFFEE2_HOT_WATER_WAIT_HEATER_ON = 3,
	COFFEE2_HOT_WATER_HEATING = 4,
	COFFEE2_HOT_WATER_WAIT_OFF_DONE = 5,
	COFFEE2_HOT_WATER_WAIT_OFF_ALARM = 6
} Coffee2HotWaterPhase_e;

typedef struct {
	Coffee2Command_t xCommand;
	TickType_t xStartTick;
	uint16_t usHeatMinutes;
	uint8_t ucPhase;
	uint8_t ucIoPending;
	uint8_t ucCancelRequested;
	uint8_t ucAlarmReason;
} Coffee2HotWaterContext_t;

typedef struct {
	Coffee2MaintenanceType_e xType;
	uint16_t usParameter0;
	uint16_t usParameter1;
	uint8_t ucPending;
} Coffee2MaintenanceRequest_t;

/** @brief Store workflow-owned queue resources. */
COFFEE2_CCM_DATA
static StaticQueue_t s_xOrderQueueStorage;
COFFEE2_CCM_DATA
static uint8_t s_aucOrderQueueStorage[
	COFFEE2_WORKFLOW_QUEUE_LENGTH * sizeof(Coffee2Order_t)];
COFFEE2_CCM_DATA
static QueueHandle_t s_xOrderQueue;
/** @brief One bounded maintenance request, consumed by the workflow owner. */
COFFEE2_CCM_DATA
static uint16_t s_usManualIceWeight;
COFFEE2_CCM_DATA
static uint8_t s_ucManualIcePending;
/** @brief Latest order waiting for the active order to cancel. */
COFFEE2_CCM_DATA
static Coffee2Order_t s_xPendingOrder;
COFFEE2_CCM_DATA
static uint8_t s_ucPendingOrder;
/** @brief Monotonic generation that separates reused host order numbers. */
COFFEE2_CCM_DATA
static uint32_t s_ulNextOrderEpoch;
COFFEE2_CCM_DATA
static uint8_t s_ucManualOverride;
COFFEE2_CCM_DATA
static Coffee2HotWaterContext_t s_xHotWater;
COFFEE2_CCM_DATA
static Coffee2MaintenanceRequest_t s_xMaintenance;
COFFEE2_CCM_DATA
static uint8_t s_ucInitializationAcknowledged;
COFFEE2_CCM_DATA
static uint8_t s_ucInitializationComplete;

COFFEE2_CCM_DATA
Coffee2WorkflowStatus_t g_xCoffee2WorkflowStatus;

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
static int32_t prvRunStep(uint16_t usStep, Coffee2DeviceId_e xDeviceId,
	Coffee2Action_e xAction, uint16_t usParameter0,
	uint16_t usParameter1, uint32_t ulTimeoutMs);
/**
  * @brief  按当前配置执行一份完整咖啡订单。
  * @param[in] pxOrder 已复制到工作流上下文的订单寄存器镜像。
  * @retval 0 所有必要步骤完成。
  * @retval 负数 设备、打印、称重、取消或工作流超时错误。
  */
static int32_t prvRunOrder(const Coffee2Order_t *pxOrder);
/**
  * @brief  在称重反馈下出冰，并确保任何路径都关闭阀门。
  * @param[in] usTargetDecigram 目标冰量，单位为 0.1 g。
  * @retval 0 达到允许误差范围内的目标重量。
  * @retval 负数 称重、通信、取消、超重或补偿失败。
  */
static int32_t prvDispenseIce(uint16_t usTargetDecigram);
/**
  * @brief  根据一阶模型计算并限制一次制冰阀脉冲时间。
  * @param[in] usTargetDecigram 目标冰量，单位为 0.1 g。
  * @retval 阀门脉冲时间，单位为毫秒，已限制在安全上下限内。
  */
static uint32_t prvCalculateIcePulseMs(uint16_t usTargetDecigram);
/**
  * @brief  读取三次称重采样并返回以 0.1 g 表示的中值。
  * @param[out] plWeightDecigram 输出中值重量，单位为 0.1 g。
  * @retval 0 三次采样有效且已得到中值。
  * @retval 负数 称重设备通信、单位或稳定性错误。
  */
static int32_t prvReadStableScale(int32_t *plWeightDecigram);
/**
  * @brief  发生失败后向相关执行机构提交尽力取消命令。
  * @note   函数本身不等待每个取消命令完成，调用者必须执行取消屏障。
  */
static int32_t prvAbortDevices(void);
static int32_t prvRunInitialization(void);
static int32_t prvProbeResidualCup(uint16_t usStepBase,
	Coffee2Action_e xPickupAction, uint8_t ucOccupiedPoint);
static int32_t prvRunMaintenance(const Coffee2MaintenanceRequest_t *pxRequest);
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
static uint8_t prvOrderValid(const Coffee2Order_t *pxOrder,
	int32_t *plError);
static void prvDelayWithServices(uint32_t ulDelayMs);
static int32_t prvWaitBusinessCondition(uint16_t usStep,
	uint8_t ucCondition);
static int32_t prvRefreshDeviceQuiet(uint16_t usStep,
	Coffee2DeviceId_e xDeviceId);
static uint8_t prvBusinessConditionActive(uint8_t ucCondition);
static int32_t prvWaitDeviceReportedComplete(uint16_t usStep,
	Coffee2DeviceId_e xDeviceId, uint8_t ucStatusIndex,
	uint16_t usSuccessValue, uint16_t usFailedValue);
/**
  * @brief  周期刷新本地 GPIO 和外部 IO 模块镜像。
  * @note   函数只提交非阻塞刷新命令，不拥有 RTU 总线。
  */
static void prvServiceIoRefresh(void);
static uint16_t s_usLastInitializationFailure = 0xFFFFU;
/**
  * @brief  将工作流状态发布到全局状态和 Server 状态寄存器。
  * @param[in] xState 新的工作流状态。
  * @param[in] usStep 当前步骤编号。
  * @param[in] lError 当前错误码；无错误时为 0。
  */
static void prvPublish(Coffee2WorkflowState_e xState,
	uint16_t usStep, int32_t lError);
static uint8_t prvQueuePendingOrder(void);
static uint8_t prvRobotPositionAction(Coffee2Action_e xAction);

/*-----------------------------------------------------------*/
BaseType_t xCoffee2WorkflowInitialize(void)
{
	memset(&g_xCoffee2WorkflowStatus, 0,
		sizeof(g_xCoffee2WorkflowStatus));
	s_usManualIceWeight = 0U;
	s_ucManualIcePending = 0U;
	memset(&s_xPendingOrder, 0, sizeof(s_xPendingOrder));
	s_ucPendingOrder = 0U;
	s_ulNextOrderEpoch = 0U;
	s_ucManualOverride = 0U;
	memset(&s_xHotWater, 0, sizeof(s_xHotWater));
	memset(&s_xMaintenance, 0, sizeof(s_xMaintenance));
	s_ucInitializationAcknowledged = 0U;
	s_ucInitializationComplete = 0U;
	g_xCoffee2WorkflowStatus.xMachineState =
		COFFEE2_MACHINE_INITIALIZING;
	g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 0U;
	s_xOrderQueue = xQueueCreateStatic(COFFEE2_WORKFLOW_QUEUE_LENGTH,
		sizeof(Coffee2Order_t), s_aucOrderQueueStorage,
		&s_xOrderQueueStorage);
	return (s_xOrderQueue != NULL) ? pdPASS : pdFAIL;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee2WorkflowSubmitMaintenance(
	Coffee2MaintenanceType_e xType, uint16_t usParameter0,
	uint16_t usParameter1)
{
	BaseType_t xResult;

	if ((xType <= COFFEE2_MAINTENANCE_NONE) ||
		(xType > COFFEE2_MAINTENANCE_FRUIT_CLEAN)) {
		return pdFAIL;
	}
	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if (s_xMaintenance.ucPending == 0U) {
		s_xMaintenance.xType = xType;
		s_xMaintenance.usParameter0 = usParameter0;
		s_xMaintenance.usParameter1 = usParameter1;
		s_xMaintenance.ucPending = 1U;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee2WorkflowSetHotWater(uint8_t ucStart,
	uint16_t usHeatMinutes)
{
	if (ucStart == 0U) {
		taskENTER_CRITICAL();
		s_xHotWater.ucCancelRequested = 1U;
		taskEXIT_CRITICAL();
		return pdPASS;
	}
	if (usHeatMinutes == 0U) {
		usHeatMinutes = COFFEE2_HOT_WATER_DEFAULT_HEAT_MIN;
	}
	if (usHeatMinutes > COFFEE2_HOT_WATER_MAX_HEAT_MIN) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if ((s_xHotWater.ucPhase != COFFEE2_HOT_WATER_IDLE) ||
		(s_xHotWater.ucIoPending != 0U)) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	memset(&s_xHotWater, 0, sizeof(s_xHotWater));
	s_xHotWater.usHeatMinutes = usHeatMinutes;
	s_xHotWater.ucPhase = COFFEE2_HOT_WATER_PREPARE_OFF;
	taskEXIT_CRITICAL();
	(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_WORKFLOW, COFFEE2_LOG_ORDER_DEBUG,
		"HOT_WATER_START", 0, "minutes", (int32_t)usHeatMinutes);
	return pdPASS;
}

/*-----------------------------------------------------------*/
void vCoffee2WorkflowAcknowledgeAlarm(void)
{
	taskENTER_CRITICAL();
	s_ucInitializationAcknowledged = 1U;
	if (s_xHotWater.ucPhase == COFFEE2_HOT_WATER_IDLE) {
		g_xCoffee2WorkflowStatus.ucHotWaterState =
			COFFEE2_MAINTENANCE_IDLE;
	}
	if (g_xCoffee2WorkflowStatus.xState != COFFEE2_WORKFLOW_RUNNING) {
		g_xCoffee2WorkflowStatus.xMachineState =
			(s_ucInitializationComplete != 0U) ? COFFEE2_MACHINE_IDLE :
				COFFEE2_MACHINE_INITIALIZING;
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee2WorkflowSubmitOrder(const Coffee2Order_t *pxOrder)
{
	BaseType_t xResult;

	if ((pxOrder == NULL) || (s_xOrderQueue == NULL)) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if ((s_ucManualIcePending != 0U) ||
		((g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen == 0U) &&
			(g_xCoffee2WorkflowStatus.xState !=
				COFFEE2_WORKFLOW_RUNNING) &&
			(g_xCoffee2WorkflowStatus.xState !=
				COFFEE2_WORKFLOW_CANCELING))) {
		taskEXIT_CRITICAL();
		return pdFAIL;
	}
	if (g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen == 0U) {
		s_xPendingOrder = *pxOrder;
		s_ucPendingOrder = 1U;
		g_xCoffee2WorkflowStatus.ucCancelRequested = 1U;
		xResult = pdPASS;
	} else {
		g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 0U;
		xResult = pdFAIL;
	}
	if (xResult == pdPASS) {
		taskEXIT_CRITICAL();
		vCoffee2OrderCancelRequest(
			g_xCoffee2WorkflowStatus.ulOrderEpoch);
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_WORKFLOW,
			pxOrder->ausRegister[COFFEE2_REG_ORDER_NUMBER],
			"ORDER_REPLACEMENT_REQUEST", 0,
			"order", (int32_t)pxOrder->ausRegister[
				COFFEE2_REG_ORDER_NUMBER]);
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_WORKFLOW,
			g_xCoffee2WorkflowStatus.usCurrentOrderId,
			"ORDER_CANCEL_REQUEST", 0,
			"order", (int32_t)
				g_xCoffee2WorkflowStatus.usCurrentOrderId);
		return pdPASS;
	}
	taskEXIT_CRITICAL();
	xResult = xQueueSend(s_xOrderQueue, pxOrder, 0U);
	if (xResult != pdPASS) {
		taskENTER_CRITICAL();
		g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 1U;
		taskEXIT_CRITICAL();
	}
	return xResult;
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee2WorkflowSubmitManualIce(uint16_t usTargetWeight)
{
	BaseType_t xResult;

	if ((usTargetWeight == 0U) || (s_xOrderQueue == NULL) ||
		(uxQueueMessagesWaiting(s_xOrderQueue) != 0U)) {
		return pdFAIL;
	}
	xResult = pdFAIL;
	taskENTER_CRITICAL();
	if ((s_ucManualIcePending == 0U) &&
		(g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen != 0U)) {
		s_usManualIceWeight = usTargetWeight;
		s_ucManualIcePending = 1U;
		g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 0U;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
void vCoffee2WorkflowRequestCancel(void)
{
	uint32_t ulOrderEpoch;

	taskENTER_CRITICAL();
	g_xCoffee2WorkflowStatus.ucCancelRequested = 1U;
	ulOrderEpoch = g_xCoffee2WorkflowStatus.ulOrderEpoch;
	taskEXIT_CRITICAL();
	vCoffee2OrderCancelRequest(ulOrderEpoch);
	(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
		COFFEE2_LOG_SOURCE_WORKFLOW,
		g_xCoffee2WorkflowStatus.usCurrentOrderId,
		"WORKFLOW_CANCEL_REQUEST", 0,
		"order", (int32_t)g_xCoffee2WorkflowStatus.usCurrentOrderId);
}

/*-----------------------------------------------------------*/
void vCoffee2WorkflowTask(void *pvArgument)
{
	Coffee2Order_t xOrder;
	int32_t lResult;
	int32_t lSafetyResult;
	int32_t lLastInitError;
	uint16_t usManualIceWeight;
	Coffee2MaintenanceRequest_t xMaintenance;

	(void)pvArgument;
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_WORKFLOW, "TASK_RUNNING:C2Workflow", 0);
	vTaskDelay(pdMS_TO_TICKS(500U));
	lLastInitError = 0;
	for (;;) {
		g_xCoffee2WorkflowStatus.xMachineState =
			COFFEE2_MACHINE_INITIALIZING;
		lResult = prvRunInitialization();
		if (lResult == 0) {
			s_usLastInitializationFailure = 0xFFFFU;
			break;
		}
		g_xCoffee2WorkflowStatus.xMachineState = COFFEE2_MACHINE_ALARM;
		if (lResult != lLastInitError) {
			(void)xCoffee2LogWriteFieldOrder(
				COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				COFFEE2_LOG_ORDER_SYSTEM,
				"MACHINE_INIT_BLOCKED", lResult,
				"reason", lResult);
			lLastInitError = lResult;
		}
		if (lResult == COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED) {
			while (s_ucInitializationAcknowledged == 0U) {
				prvServiceHotWater();
				prvServiceIoRefresh();
				vTaskDelay(pdMS_TO_TICKS(200U));
			}
			s_ucInitializationAcknowledged = 0U;
		} else {
			prvDelayWithServices(2000U);
		}
	}
	taskENTER_CRITICAL();
	s_ucInitializationComplete = 1U;
	g_xCoffee2WorkflowStatus.xMachineState = COFFEE2_MACHINE_IDLE;
	g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 1U;
	taskEXIT_CRITICAL();
	(void)xCoffee2LogWriteOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_WORKFLOW, COFFEE2_LOG_ORDER_SYSTEM,
		"MACHINE_INIT_COMPLETE", 0);
	prvPublish(COFFEE2_WORKFLOW_IDLE, 0U, 0);
	for (;;) {
		prvServiceHotWater();
		if (xQueueReceive(s_xOrderQueue, &xOrder,
			pdMS_TO_TICKS(COFFEE2_WORKFLOW_IO_REFRESH_MS)) !=
			pdPASS) {
			memset(&xMaintenance, 0, sizeof(xMaintenance));
			taskENTER_CRITICAL();
			if (s_ucManualIcePending != 0U) {
				usManualIceWeight = s_usManualIceWeight;
				s_ucManualIcePending = 0U;
			} else {
				usManualIceWeight = 0U;
			}
			if ((usManualIceWeight == 0U) &&
				(s_xMaintenance.ucPending != 0U)) {
				xMaintenance = s_xMaintenance;
				s_xMaintenance.ucPending = 0U;
			}
			taskEXIT_CRITICAL();
			if (usManualIceWeight != 0U) {
				taskENTER_CRITICAL();
				s_ulNextOrderEpoch++;
				if (s_ulNextOrderEpoch == 0U) {
					s_ulNextOrderEpoch = 1U;
				}
			g_xCoffee2WorkflowStatus.ulOrderEpoch =
				s_ulNextOrderEpoch;
			g_xCoffee2WorkflowStatus.usCurrentOrderId =
				COFFEE2_LOG_ORDER_DEBUG;
				g_xCoffee2WorkflowStatus.ucCancelRequested = 0U;
				taskEXIT_CRITICAL();
				(void)xCoffee2LogWriteFieldOrder(
					COFFEE2_LOG_LEVEL_INFO,
					COFFEE2_LOG_SOURCE_WORKFLOW,
					COFFEE2_LOG_ORDER_DEBUG,
					"MANUAL_ICE_START", 0, "target_dg",
					(int32_t)usManualIceWeight);
				prvPublish(COFFEE2_WORKFLOW_RUNNING, 900U, 0);
				lResult = prvDispenseIce(usManualIceWeight);
				if (lResult == 0) {
					taskENTER_CRITICAL();
					g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 1U;
					taskEXIT_CRITICAL();
					prvPublish(COFFEE2_WORKFLOW_COMPLETED,
						903U, 0);
					(void)xCoffee2LogWriteFieldOrder(
						COFFEE2_LOG_LEVEL_INFO,
						COFFEE2_LOG_SOURCE_WORKFLOW,
						COFFEE2_LOG_ORDER_DEBUG,
						"MANUAL_ICE_DONE", 0, "target_dg",
						(int32_t)usManualIceWeight);
				} else {
					lSafetyResult = prvAbortDevices();
					if (lSafetyResult == 0) {
						taskENTER_CRITICAL();
						g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 1U;
						taskEXIT_CRITICAL();
					} else {
						lResult = COFFEE2_WORKFLOW_ERROR_SAFE_STOP;
					}
					prvPublish(COFFEE2_WORKFLOW_FAILED,
						g_xCoffee2WorkflowStatus.usCurrentStep,
						lResult);
					(void)xCoffee2LogWriteFieldOrder(
						COFFEE2_LOG_LEVEL_ERROR,
						COFFEE2_LOG_SOURCE_WORKFLOW,
						COFFEE2_LOG_ORDER_DEBUG,
						"MANUAL_ICE_FAILED", lResult, "step",
						(int32_t)
							g_xCoffee2WorkflowStatus.usCurrentStep);
				}
			}
			if (xMaintenance.xType != COFFEE2_MAINTENANCE_NONE) {
				g_xCoffee2WorkflowStatus.xMachineState =
					COFFEE2_MACHINE_BUSY;
				lResult = prvRunMaintenance(&xMaintenance);
				g_xCoffee2WorkflowStatus.xMachineState =
					(lResult == 0) ? COFFEE2_MACHINE_IDLE :
					COFFEE2_MACHINE_ALARM;
				(void)xCoffee2LogWriteFieldOrder(
					(lResult == 0) ? COFFEE2_LOG_LEVEL_INFO :
						COFFEE2_LOG_LEVEL_ERROR,
					COFFEE2_LOG_SOURCE_WORKFLOW,
					COFFEE2_LOG_ORDER_DEBUG,
					(lResult == 0) ? "MAINTENANCE_COMPLETE" :
						"MAINTENANCE_FAILED",
					lResult, "type", (int32_t)xMaintenance.xType);
			}
			prvServiceIoRefresh();
			continue;
		}
		taskENTER_CRITICAL();
		s_ulNextOrderEpoch++;
		if (s_ulNextOrderEpoch == 0U) {
			s_ulNextOrderEpoch = 1U;
		}
		g_xCoffee2WorkflowStatus.ulOrderEpoch = s_ulNextOrderEpoch;
		g_xCoffee2WorkflowStatus.usCurrentOrderId =
			xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER];
		g_xCoffee2WorkflowStatus.ucCancelRequested = 0U;
		s_ucManualOverride = 0U;
		taskEXIT_CRITICAL();
		prvPublish(COFFEE2_WORKFLOW_RUNNING, 1U, 0);
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_WORKFLOW,
			g_xCoffee2WorkflowStatus.usCurrentOrderId,
			"ORDER_START", 0,
			"order", (int32_t)g_xCoffee2WorkflowStatus.usCurrentOrderId);
		lResult = prvRunOrder(&xOrder);
		if (lResult == 0) {
			if (prvQueuePendingOrder() == 0U) {
				taskENTER_CRITICAL();
				g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 1U;
				taskEXIT_CRITICAL();
			}
			taskENTER_CRITICAL();
			g_xCoffee2WorkflowStatus.ulCompletedOrderCount++;
			taskEXIT_CRITICAL();
			prvPublish(COFFEE2_WORKFLOW_COMPLETED,
				g_xCoffee2WorkflowStatus.usCurrentStep, 0);
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				g_xCoffee2WorkflowStatus.usCurrentOrderId,
				"ORDER_COMPLETED", 0,
				"order",
				(int32_t)g_xCoffee2WorkflowStatus.usCurrentOrderId);
		} else {
			taskENTER_CRITICAL();
			g_xCoffee2WorkflowStatus.ulFailedOrderCount++;
			taskEXIT_CRITICAL();
			lSafetyResult = prvAbortDevices();
			if (lSafetyResult == 0) {
				if (prvQueuePendingOrder() == 0U) {
					taskENTER_CRITICAL();
					g_xCoffee2WorkflowStatus.ucOrderAdmissionOpen = 1U;
					taskEXIT_CRITICAL();
				}
			} else {
				lResult = COFFEE2_WORKFLOW_ERROR_SAFE_STOP;
			}
			prvPublish(COFFEE2_WORKFLOW_FAILED,
				g_xCoffee2WorkflowStatus.usCurrentStep, lResult);
			(void)xCoffee2LogWriteFieldOrder(
				(s_ucManualOverride != 0U) ? COFFEE2_LOG_LEVEL_WARNING :
					COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				g_xCoffee2WorkflowStatus.usCurrentOrderId,
				(s_ucManualOverride != 0U) ? "ORDER_CANCELED" : "ORDER_FAILED",
				lResult, "step",
				(int32_t)g_xCoffee2WorkflowStatus.usCurrentStep);
		}
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvRobotPositionAction(Coffee2Action_e xAction)
{
	return (((xAction >= COFFEE2_ACTION_ROBOT_HOME) &&
		(xAction <= COFFEE2_ACTION_ROBOT_TAKE_STORAGE)) ||
		(xAction == COFFEE2_ACTION_ROBOT_TO_FRUIT_SYRUP)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static uint8_t prvQueuePendingOrder(void)
{
	Coffee2Order_t xOrder;

	taskENTER_CRITICAL();
	if (s_ucPendingOrder == 0U) {
		taskEXIT_CRITICAL();
		return 0U;
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
	(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_WORKFLOW,
		g_xCoffee2WorkflowStatus.usCurrentOrderId,
		"ORDER_CANCELED", 0,
		"order", (int32_t)
			g_xCoffee2WorkflowStatus.usCurrentOrderId);
	(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_WORKFLOW,
		xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER],
		"ORDER_ACCEPTED", 0,
		"order", (int32_t)xOrder.ausRegister[
			COFFEE2_REG_ORDER_NUMBER]);
	return 1U;
}

/*-----------------------------------------------------------*/
static int32_t prvRunStep(uint16_t usStep, Coffee2DeviceId_e xDeviceId,
	Coffee2Action_e xAction, uint16_t usParameter0,
	uint16_t usParameter1, uint32_t ulTimeoutMs)
{
	Coffee2Command_t xCommand;
	EventBits_t xEvents;
	TickType_t xStartTick;
	TickType_t xTimeoutTicks;
	uint8_t ucRobotTimingStarted;
	uint8_t ucOrderStep;
	uint16_t usLogOrder;
	uint8_t ucInitializationStep;

	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	ucInitializationStep = ((usStep >= 0xFD00U) &&
		(usStep < 0xFE00U)) ? 1U : 0U;
	usLogOrder = (ucOrderStep != 0U) ?
		g_xCoffee2WorkflowStatus.usCurrentOrderId :
		COFFEE2_LOG_ORDER_DEBUG;
	if ((ucOrderStep != 0U) &&
		(g_xCoffee2WorkflowStatus.ucCancelRequested != 0U)) {
		return COFFEE2_WORKFLOW_ERROR_CANCELED;
	}
	if (ucInitializationStep == 0U) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_WORKFLOW,
			usLogOrder,
			"WORKFLOW_STEP_START",
			(int32_t)xAction, "step", (int32_t)usStep);
	}
	if (ucOrderStep != 0U) {
		prvPublish(COFFEE2_WORKFLOW_RUNNING, usStep, 0);
	}
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId = usLogOrder;
	xCommand.ulOrderEpoch = (ucOrderStep != 0U) ?
		g_xCoffee2WorkflowStatus.ulOrderEpoch : 0U;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)xAction;
	xCommand.ausParameter[0] = usParameter0;
	xCommand.ausParameter[1] = usParameter1;
	if (xDeviceId == COFFEE2_DEVICE_ROBOT) {
		xCommand.ulTimeoutMs = ulTimeoutMs;
	} else if (xDeviceId == COFFEE2_DEVICE_COFFEE_MACHINE) {
		xCommand.ulTimeoutMs = COFFEE2_WORKFLOW_DEVICE_IO_TIMEOUT_MS;
	} else {
		xCommand.ulTimeoutMs = COFFEE2_RTU_IO_TIMEOUT_MS;
	}
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = (ucOrderStep != 0U) ?
		(uint8_t)COFFEE2_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE2_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee2CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) !=
		pdPASS) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_WORKFLOW,
			usLogOrder,
			"WORKFLOW_STEP_QUEUE_FAILED",
			COFFEE2_WORKFLOW_ERROR_QUEUE, "step", (int32_t)usStep);
		return COFFEE2_WORKFLOW_ERROR_QUEUE;
	}
	xStartTick = xTaskGetTickCount();
	xTimeoutTicks = pdMS_TO_TICKS((xDeviceId == COFFEE2_DEVICE_ROBOT) ?
		ulTimeoutMs : (ulTimeoutMs +
			(4U * COFFEE2_WORKFLOW_DEVICE_IO_TIMEOUT_MS)));
	ucRobotTimingStarted = ((xDeviceId == COFFEE2_DEVICE_ROBOT) &&
		(prvRobotPositionAction(xAction) != 0U)) ? 0U : 1U;
	for (;;) {
		xEvents = xCoffee2DeviceWaitCommand(xDeviceId,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		if ((xEvents & COFFEE2_DEVICE_EVENT_CANCELED) != 0U) {
			uint8_t ucTerminalValid;
			int32_t lTerminalResult;
			lTerminalResult = lCoffee2DeviceGetTerminalResult(xDeviceId,
				xCommand.ulOrderEpoch, xCommand.ulCommandId,
				&ucTerminalValid);
			if ((ucOrderStep != 0U) &&
				(xDeviceId == COFFEE2_DEVICE_ROBOT) &&
				(ucTerminalValid != 0U) &&
				(lTerminalResult == COFFEE2_COMMAND_RESULT_SUPERSEDED)) {
				s_ucManualOverride = 1U;
				(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
					COFFEE2_LOG_SOURCE_WORKFLOW,
					usLogOrder,
					"WORKFLOW_MANUAL_OVERRIDE",
					COFFEE2_COMMAND_RESULT_CANCELED,
					"step", (int32_t)usStep);
			}
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_CANCELED",
				COFFEE2_WORKFLOW_ERROR_CANCELED, "step",
				(int32_t)usStep);
			return COFFEE2_WORKFLOW_ERROR_CANCELED;
		}
		if ((xEvents & COFFEE2_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			if (ucInitializationStep == 0U) {
				(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
					COFFEE2_LOG_SOURCE_WORKFLOW,
					usLogOrder,
					"WORKFLOW_STEP_DONE", 0,
					"step", (int32_t)usStep);
			}
			return 0;
		}
		if ((xEvents & COFFEE2_DEVICE_EVENT_TERMINAL) != 0U) {
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
					(void)xCoffee2LogPrintfOrder(COFFEE2_LOG_LEVEL_ERROR,
						COFFEE2_LOG_SOURCE_WORKFLOW,
						COFFEE2_LOG_ORDER_SYSTEM,
						"%s_INIT_FAILED_RESULT=%ld",
						pcDeviceName,
						(long)g_axCoffee2DeviceStatus[xDeviceId].lLastResult);
					s_usLastInitializationFailure = usStep;
				}
			} else {
				(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
					COFFEE2_LOG_SOURCE_WORKFLOW,
					usLogOrder,
					"WORKFLOW_STEP_DEVICE_FAILED",
					g_axCoffee2DeviceStatus[xDeviceId].lLastResult, "step",
					(int32_t)usStep);
			}
			return COFFEE2_WORKFLOW_ERROR_DEVICE;
		}
		if ((ucOrderStep != 0U) &&
			(g_xCoffee2WorkflowStatus.ucCancelRequested != 0U)) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_CANCELED",
				COFFEE2_WORKFLOW_ERROR_CANCELED, "step",
				(int32_t)usStep);
			return COFFEE2_WORKFLOW_ERROR_CANCELED;
		}
		if ((xDeviceId == COFFEE2_DEVICE_ROBOT) &&
			(prvRobotPositionAction(xAction) != 0U)) {
			if ((g_axCoffee2DeviceStatus[xDeviceId].ulLastCommandId ==
					xCommand.ulCommandId) &&
					(g_axCoffee2DeviceStatus[xDeviceId].ulLastOrderEpoch ==
						xCommand.ulOrderEpoch) &&
					(g_axCoffee2DeviceStatus[xDeviceId].ucRobotAccepted != 0U) &&
					(ucRobotTimingStarted == 0U)) {
				ucRobotTimingStarted = 1U;
				xStartTick = xTaskGetTickCount();
			}
			if ((g_axCoffee2DeviceStatus[xDeviceId].ulLastCommandId !=
				xCommand.ulCommandId) ||
				(g_axCoffee2DeviceStatus[xDeviceId].ulLastOrderEpoch !=
					xCommand.ulOrderEpoch) ||
				(g_axCoffee2DeviceStatus[xDeviceId].ucRobotAccepted == 0U) ||
				(g_axCoffee2DeviceStatus[xDeviceId].ucRecovering != 0U)) {
				xStartTick = xTaskGetTickCount();
			} else if (ucRobotTimingStarted == 0U) {
				ucRobotTimingStarted = 1U;
				xStartTick = xTaskGetTickCount();
			}
		}
		if ((ucRobotTimingStarted != 0U) &&
			((xTaskGetTickCount() - xStartTick) >= xTimeoutTicks)) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				usLogOrder,
				"WORKFLOW_STEP_TIMEOUT",
				COFFEE2_WORKFLOW_ERROR_TIMEOUT, "step",
				(int32_t)usStep);
			return COFFEE2_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
		prvServiceIoRefresh();
	}
}

/*-----------------------------------------------------------*/
static int32_t prvRunOrder(const Coffee2Order_t *pxOrder)
{
	int32_t lResult;
	int32_t lValidationError;
	uint16_t usIceAmount;
	uint16_t usSyrupAmount;
	uint16_t usColdOrder;
	uint16_t usLidLane;
	uint16_t usOutput;
	uint8_t ucNeedCoffeeStation;
	uint8_t ucNeedFlavorStation;

	if (prvOrderValid(pxOrder, &lValidationError) == 0U) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_WORKFLOW,
			(pxOrder != NULL) ? pxOrder->ausRegister[
				COFFEE2_REG_ORDER_NUMBER] : COFFEE2_LOG_ORDER_SYSTEM,
			"ORDER_VALIDATION_FAILED", lValidationError,
			"reason", lValidationError);
		return lValidationError;
	}
	usIceAmount = pxOrder->ausRegister[COFFEE2_REG_ICE_AMOUNT];
	usColdOrder = (usIceAmount != 0U) ? 1U : 0U;
	usLidLane = (usColdOrder != 0U) ? 2U : 1U;
	usOutput = pxOrder->ausRegister[COFFEE2_REG_ONLINE_OUTPUT];
	ucNeedCoffeeStation =
		(pxOrder->ausRegister[COFFEE2_REG_COFFEE_TYPE] != 0xFFFFU) ?
			1U : 0U;
	ucNeedFlavorStation =
		((pxOrder->ausRegister[COFFEE2_REG_FRUIT_MILK_A] != 0U) ||
		(pxOrder->ausRegister[COFFEE2_REG_FRUIT_MILK_B] != 0U) ||
		(pxOrder->ausRegister[COFFEE2_REG_SYRUP_1] != 0U) ||
		(pxOrder->ausRegister[COFFEE2_REG_SYRUP_2] != 0U) ||
		(pxOrder->ausRegister[COFFEE2_REG_SYRUP_3] != 0U) ||
		(pxOrder->ausRegister[COFFEE2_REG_SYRUP_4] != 0U)) ? 1U : 0U;
	lResult = prvRunStep(10U, COFFEE2_DEVICE_ROBOT,
		COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	if (lResult == 0) {
		lResult = prvRunStep(20U, COFFEE2_DEVICE_COFFEE_MACHINE,
			COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if (lResult == 0) {
		lResult = prvRunStep(22U, COFFEE2_DEVICE_CUP_MACHINE,
			COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE2_REG_LID_ENABLE] != 0U)) {
		lResult = prvRunStep(24U, COFFEE2_DEVICE_LID_MACHINE,
			COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) &&
		((pxOrder->ausRegister[COFFEE2_REG_SYRUP_1] != 0U) ||
		(pxOrder->ausRegister[COFFEE2_REG_SYRUP_2] != 0U) ||
		(pxOrder->ausRegister[COFFEE2_REG_SYRUP_3] != 0U) ||
		(pxOrder->ausRegister[COFFEE2_REG_SYRUP_4] != 0U))) {
		lResult = prvRunStep(26U, COFFEE2_DEVICE_SYRUP_MACHINE,
			COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if ((lResult == 0) && (usIceAmount != 0U)) {
		lResult = prvRunStep(28U, COFFEE2_DEVICE_ICE_MACHINE,
			COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if (lResult == 0) {
		lResult = prvRunStep(30U, COFFEE2_DEVICE_ROBOT,
		COFFEE2_ACTION_ROBOT_HOME, 0U, 0U,
		COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvRunStep(40U, COFFEE2_DEVICE_ROBOT,
			(usColdOrder != 0U) ?
				COFFEE2_ACTION_ROBOT_TAKE_COLD_CUP :
				COFFEE2_ACTION_ROBOT_TAKE_HOT_CUP,
			0U, 0U, COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvRunStep(50U, COFFEE2_DEVICE_CUP_MACHINE,
			(usColdOrder != 0U) ?
				COFFEE2_ACTION_CUP_DROP_2 :
				COFFEE2_ACTION_CUP_DROP_1,
			0U, 0U, 3000U);
	}
	if (lResult == 0) {
		lResult = prvWaitBusinessCondition(55U,
			(usColdOrder != 0U) ? COFFEE2_CONDITION_CUP_2 :
				COFFEE2_CONDITION_CUP_1);
	}
	if ((lResult == 0) && (usIceAmount != 0U)) {
		lResult = prvRunStep(60U, COFFEE2_DEVICE_ROBOT,
			COFFEE2_ACTION_ROBOT_TO_ICE, 0U, 0U,
			COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvDispenseIce((uint16_t)(usIceAmount * 10U));
		}
	}
	if ((lResult == 0) && (ucNeedFlavorStation != 0U)) {
		lResult = prvRunStep(65U, COFFEE2_DEVICE_ROBOT,
			COFFEE2_ACTION_ROBOT_TO_FRUIT_SYRUP, 0U, 0U,
			COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE2_REG_FRUIT_MILK_A] != 0U)) {
		lResult = prvRunFruit(1U,
			pxOrder->ausRegister[COFFEE2_REG_FRUIT_MILK_A], 0U, 66U);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE2_REG_FRUIT_MILK_B] != 0U)) {
		lResult = prvRunFruit(2U,
			pxOrder->ausRegister[COFFEE2_REG_FRUIT_MILK_B], 0U, 72U);
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE2_REG_SYRUP_1];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(110U, COFFEE2_DEVICE_SYRUP_MACHINE,
			COFFEE2_ACTION_SYRUP_DISPENSE, 1U,
			(uint16_t)(usSyrupAmount *
				COFFEE2_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(111U,
				COFFEE2_DEVICE_SYRUP_MACHINE, 1U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE2_REG_SYRUP_2];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(120U, COFFEE2_DEVICE_SYRUP_MACHINE,
			COFFEE2_ACTION_SYRUP_DISPENSE, 2U,
			(uint16_t)(usSyrupAmount *
				COFFEE2_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(121U,
				COFFEE2_DEVICE_SYRUP_MACHINE, 2U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE2_REG_SYRUP_3];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(123U, COFFEE2_DEVICE_SYRUP_MACHINE,
			COFFEE2_ACTION_SYRUP_DISPENSE, 3U,
			(uint16_t)(usSyrupAmount *
				COFFEE2_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(124U,
				COFFEE2_DEVICE_SYRUP_MACHINE, 3U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	usSyrupAmount = pxOrder->ausRegister[COFFEE2_REG_SYRUP_4];
	if ((lResult == 0) && (usSyrupAmount != 0U)) {
		lResult = prvRunStep(126U, COFFEE2_DEVICE_SYRUP_MACHINE,
			COFFEE2_ACTION_SYRUP_DISPENSE, 4U,
			(uint16_t)(usSyrupAmount *
				COFFEE2_SYRUP_TIME_PER_VOLUME_UNIT), 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(127U,
				COFFEE2_DEVICE_SYRUP_MACHINE, 4U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
	}
	if ((lResult == 0) && (ucNeedCoffeeStation != 0U)) {
		lResult = prvRunStep(70U, COFFEE2_DEVICE_ROBOT,
			COFFEE2_ACTION_ROBOT_TO_COFFEE, 0U, 0U,
			COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvRunStep(75U, COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_TO_COFFEE, 1U, 0U,
				COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
		}
		if ((lResult == 0) &&
			(pxOrder->ausRegister[COFFEE2_REG_COFFEE_TYPE] != 0xFFFFU)) {
			lResult = prvRunStep(80U,
				COFFEE2_DEVICE_COFFEE_MACHINE,
				COFFEE2_ACTION_COFFEE_MAKE,
				pxOrder->ausRegister[COFFEE2_REG_COFFEE_TYPE],
				0U, 3000U);
		}
		if ((lResult == 0) &&
			(pxOrder->ausRegister[COFFEE2_REG_COFFEE_TYPE] != 0xFFFFU)) {
			lResult = prvWaitDeviceReportedComplete(85U,
				COFFEE2_DEVICE_COFFEE_MACHINE, 1U,
				COFFEE2_F200_APPLICATION_COMPLETE,
				COFFEE2_F200_APPLICATION_FAILED);
		}
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE2_REG_COFFEE_TYPE] != 0xFFFFU)) {
		lResult = prvRunStep(90U, COFFEE2_DEVICE_ROBOT,
			COFFEE2_ACTION_ROBOT_TAKE_COFFEE, 0U, 0U,
			COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
	}
	if ((lResult == 0) &&
		(pxOrder->ausRegister[COFFEE2_REG_LID_ENABLE] != 0U)) {
		lResult = prvRunStep(140U, COFFEE2_DEVICE_ROBOT,
			COFFEE2_ACTION_ROBOT_TO_LID, usLidLane, 0U,
			COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult == 0) {
			lResult = prvRunStep(150U,
				COFFEE2_DEVICE_LID_MACHINE,
				(usLidLane == 1U) ?
					COFFEE2_ACTION_LID_DROP_1 :
					COFFEE2_ACTION_LID_DROP_2,
				0U, 0U, 3000U);
		}
		if (lResult == 0) {
			lResult = prvWaitBusinessCondition(155U,
				(usLidLane == 1U) ? COFFEE2_CONDITION_LID_1 :
					COFFEE2_CONDITION_LID_2);
		}
		if (lResult == 0) {
			lResult = prvRunStep(160U, COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_TAKE_LID,
				0U, 0U, COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
		}
		if (lResult == 0) {
			lResult = prvRunStep(170U, COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_COVER_LID,
				0U, 0U, COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
		}
	}
	if (lResult == 0) {
		vCoffee2ServerPublishOutput(usOutput, 2U);
		lResult = prvRunStep(180U, COFFEE2_DEVICE_ROBOT,
			COFFEE2_ACTION_ROBOT_PUT_OUTPUT,
			usOutput, 0U, COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
		if (lResult != 0) {
			vCoffee2ServerPublishOutput(usOutput, 3U);
		}
	}
	if (lResult == 0) {
		lResult = prvWaitBusinessCondition(185U,
			(usOutput == 1U) ? COFFEE2_CONDITION_OUTPUT_1 :
				COFFEE2_CONDITION_OUTPUT_2);
		if (lResult == 0) {
			vCoffee2ServerPublishOutput(usOutput, 5U);
		}
	}
	if (lResult == 0) {
		lResult = prvRunStep(190U, COFFEE2_DEVICE_ROBOT,
			COFFEE2_ACTION_ROBOT_HOME, 0U, 0U,
			COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
	}
	return lResult;
}

/*-----------------------------------------------------------*/
static int32_t prvDispenseIce(uint16_t usTargetDecigram)
{
	int32_t lResult;
	int32_t lCloseResult;
	int32_t lWeightDecigram;
	int32_t lDeficitDecigram;
	uint32_t ulPulseMs;
	uint32_t ulWaitedMs;
	uint8_t ucAttempt;

	if (usTargetDecigram == 0U) {
		return COFFEE2_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	lWeightDecigram = 0;
	lResult = prvRunStep(100U, COFFEE2_DEVICE_SCALE,
		COFFEE2_ACTION_SCALE_TARE, 0U, 0U, 3000U);
	if (lResult != 0) {
		return lResult;
	}
	vTaskDelay(pdMS_TO_TICKS(300U));
	lResult = prvReadStableScale(&lWeightDecigram);
	if ((lResult != 0) ||
		(lWeightDecigram < -COFFEE2_ICE_TOLERANCE_DECIGRAM) ||
		(lWeightDecigram > COFFEE2_ICE_TOLERANCE_DECIGRAM)) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_WORKFLOW, "ICE_TARE_UNSTABLE",
			COFFEE2_WORKFLOW_ERROR_ICE_WEIGHT, "weight_dg",
			lWeightDecigram);
		return (lResult != 0) ? lResult :
			COFFEE2_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	lDeficitDecigram = (int32_t)usTargetDecigram;
	for (ucAttempt = 0U;
		ucAttempt <= COFFEE2_ICE_MAX_CORRECTIONS; ucAttempt++) {
		ulPulseMs = prvCalculateIcePulseMs(
			(uint16_t)lDeficitDecigram);
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_WORKFLOW, "ICE_VALVE_PULSE", 0,
			"pulse_ms", (int32_t)ulPulseMs);
		lResult = prvRunStep((uint16_t)(101U + ucAttempt * 4U),
			COFFEE2_DEVICE_ICE_MACHINE,
			COFFEE2_ACTION_ICE_SET_VALVE, 1U, 0U, 3000U);
		ulWaitedMs = 0U;
		while ((lResult == 0) && (ulWaitedMs < ulPulseMs)) {
			if (g_xCoffee2WorkflowStatus.ucCancelRequested != 0U) {
				lResult = COFFEE2_WORKFLOW_ERROR_CANCELED;
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(50U));
			ulWaitedMs += 50U;
		}
		lCloseResult = prvRunStep(
			(uint16_t)(102U + ucAttempt * 4U),
			COFFEE2_DEVICE_ICE_MACHINE,
			COFFEE2_ACTION_ICE_SET_VALVE, 0U, 0U, 3000U);
		if (lResult == 0) {
			lResult = lCloseResult;
		}
		if (lResult != 0) {
			return lResult;
		}
		vTaskDelay(pdMS_TO_TICKS(COFFEE2_ICE_SETTLE_MS));
		lResult = prvReadStableScale(&lWeightDecigram);
		if (lResult != 0) {
			return lResult;
		}
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_WORKFLOW, "ICE_WEIGHT_SAMPLE", 0,
			"weight_dg", lWeightDecigram);
		lDeficitDecigram = (int32_t)usTargetDecigram -
			lWeightDecigram;
		if ((lDeficitDecigram >=
			-COFFEE2_ICE_TOLERANCE_DECIGRAM) &&
			(lDeficitDecigram <=
				COFFEE2_ICE_TOLERANCE_DECIGRAM)) {
			return 0;
		}
		if (lDeficitDecigram < 0) {
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_WORKFLOW, "ICE_WEIGHT_OVER",
				COFFEE2_WORKFLOW_ERROR_ICE_WEIGHT, "over_dg",
				-lDeficitDecigram);
			return COFFEE2_WORKFLOW_ERROR_ICE_WEIGHT;
		}
	}
	lCloseResult = prvRunStep(119U, COFFEE2_DEVICE_ICE_MACHINE,
		COFFEE2_ACTION_ICE_SET_VALVE, 0U, 0U, 3000U);
	(void)lCloseResult;
	return COFFEE2_WORKFLOW_ERROR_ICE_WEIGHT;
}

/*-----------------------------------------------------------*/
static uint32_t prvCalculateIcePulseMs(uint16_t usTargetDecigram)
{
	int32_t lPulseMs;

	lPulseMs = (((int32_t)usTargetDecigram *
		COFFEE2_ICE_SLOPE_MS_PER_GRAM) + 5L) / 10L;
	lPulseMs += COFFEE2_ICE_OFFSET_MS;
	lPulseMs = lPulseMs * COFFEE2_ICE_COMPENSATION_FACTOR;
	if (lPulseMs < (int32_t)COFFEE2_ICE_MIN_PULSE_MS) {
		lPulseMs = (int32_t)COFFEE2_ICE_MIN_PULSE_MS;
	} else if (lPulseMs > (int32_t)COFFEE2_ICE_MAX_PULSE_MS) {
		lPulseMs = (int32_t)COFFEE2_ICE_MAX_PULSE_MS;
	}
	return (uint32_t)lPulseMs;
}

/*-----------------------------------------------------------*/
static int32_t prvReadStableScale(int32_t *plWeightDecigram)
{
	int32_t alSample[3];
	int32_t lSwap;
	int32_t lResult;
	uint8_t ucIndex;

	if (plWeightDecigram == NULL) {
		return COFFEE2_WORKFLOW_ERROR_ICE_WEIGHT;
	}
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		lResult = prvRunStep((uint16_t)(120U + ucIndex),
			COFFEE2_DEVICE_SCALE, COFFEE2_ACTION_REFRESH,
			0U, 0U, 3000U);
		if (lResult != 0) {
			return lResult;
		}
		alSample[ucIndex] = g_xCoffee2ScaleImage.lWeightTenthGram;
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
	*plWeightDecigram = alSample[1];
	return 0;
}

/*-----------------------------------------------------------*/
static int32_t prvRunIoOutput(uint16_t usStep, uint8_t ucPoint,
	uint8_t ucValue)
{
	Coffee2Command_t xCommand;
	EventBits_t xEvents;
	TickType_t xStartTick;
	uint8_t ucTerminalValid;
	int32_t lTerminalResult;

	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId =
		(g_xCoffee2WorkflowStatus.xState == COFFEE2_WORKFLOW_RUNNING) ?
		g_xCoffee2WorkflowStatus.usCurrentOrderId :
		COFFEE2_LOG_ORDER_DEBUG;
	xCommand.ulOrderEpoch = g_xCoffee2WorkflowStatus.ulOrderEpoch;
	xCommand.ulTimeoutMs = COFFEE2_RTU_IO_TIMEOUT_MS;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)COFFEE2_ACTION_IO_WRITE;
	xCommand.ausParameter[0] = ucPoint;
	xCommand.ausParameter[1] = (ucValue != 0U) ? 1U : 0U;
	xCommand.ucDeviceId = (uint8_t)COFFEE2_DEVICE_IO_OUTPUT;
	xCommand.ucSource =
		(g_xCoffee2WorkflowStatus.xState == COFFEE2_WORKFLOW_RUNNING) ?
		(uint8_t)COFFEE2_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE2_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee2CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) != pdPASS) {
		return COFFEE2_WORKFLOW_ERROR_QUEUE;
	}
	xStartTick = xTaskGetTickCount();
	for (;;) {
		xEvents = xCoffee2DeviceWaitCommand(COFFEE2_DEVICE_IO_OUTPUT,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		if ((xEvents & COFFEE2_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			return 0;
		}
		if ((xEvents & COFFEE2_DEVICE_EVENT_TERMINAL) != 0U) {
			lTerminalResult = lCoffee2DeviceGetTerminalResult(
				COFFEE2_DEVICE_IO_OUTPUT, xCommand.ulOrderEpoch,
				xCommand.ulCommandId, &ucTerminalValid);
			return (ucTerminalValid != 0U) ? lTerminalResult :
				COFFEE2_WORKFLOW_ERROR_IO;
		}
		if ((xTaskGetTickCount() - xStartTick) >=
			pdMS_TO_TICKS(COFFEE2_WORKFLOW_IO_ACTION_TIMEOUT_MS)) {
			return COFFEE2_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
	}
}

/*-----------------------------------------------------------*/
static int32_t prvSetProductOutputsOff(void)
{
	static const uint8_t aucPoints[] = {
		COFFEE2_EXTERNAL_DO_WATER_HEATER_RELAY,
		COFFEE2_EXTERNAL_DO_MILK_VALVE,
		COFFEE2_EXTERNAL_DO_FRUIT_MILK_A_PUMP,
		COFFEE2_EXTERNAL_DO_FRUIT_MILK_B_PUMP,
		COFFEE2_EXTERNAL_DO_FRUIT_MILK_A_VALVE,
		COFFEE2_EXTERNAL_DO_FRUIT_MILK_B_VALVE,
		COFFEE2_EXTERNAL_DO_BOOSTER_PUMP
	};
	int32_t lResult;
	uint8_t ucIndex;

	(void)ucCoffee2IoSetLocalOutput(
		COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
	lResult = 0;
	for (ucIndex = 0U;
		ucIndex < (sizeof(aucPoints) / sizeof(aucPoints[0])); ucIndex++) {
		if (prvRunIoOutput((uint16_t)(0xFE00U + ucIndex),
			aucPoints[ucIndex], 0U) != 0) {
			lResult = COFFEE2_WORKFLOW_ERROR_SAFE_STOP;
		}
	}
	return lResult;
}

/*-----------------------------------------------------------*/
static int32_t prvRunInitialization(void)
{
	Coffee2IoState_t xIo;
	int32_t lResult;
	uint8_t ucOccupiedPoint;

	lResult = prvSetProductOutputsOff();
	if (lResult != 0) {
		return lResult;
	}
	lResult = prvRunStep(0xFD10U, COFFEE2_DEVICE_IO_INPUT,
		COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	if (lResult == 0) {
		lResult = prvRunStep(0xFD11U, COFFEE2_DEVICE_CUP_MACHINE,
			COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if (lResult == 0) {
		lResult = prvRunStep(0xFD12U, COFFEE2_DEVICE_LID_MACHINE,
			COFFEE2_ACTION_REFRESH, 0U, 0U, 3000U);
	}
	if (lResult != 0) {
		return lResult;
	}
	vCoffee2IoGetSnapshot(&xIo);
	ucOccupiedPoint = 0U;
	if (xIo.xInput.aucMB1XPin[
		COFFEE2_EXTERNAL_DI_OUTPUT_FRONT_CUP] != 0U) {
		ucOccupiedPoint = 1U;
	} else if (xIo.xInput.aucMB1XPin[
		COFFEE2_EXTERNAL_DI_OUTPUT_REAR_CUP] != 0U) {
		ucOccupiedPoint = 2U;
	}
	if (ucOccupiedPoint == 0U) {
		lResult = prvProbeResidualCup(0xFD20U,
			(Coffee2Action_e)0, 7U);
		if (lResult == COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED) {
			ucOccupiedPoint = 7U;
		}
	}
	if ((ucOccupiedPoint == 0U) && (lResult == 0)) {
		lResult = prvProbeResidualCup(0xFD24U,
			COFFEE2_ACTION_ROBOT_TAKE_COFFEE, 8U);
		if (lResult == COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED) {
			ucOccupiedPoint = 8U;
		}
	}
	if ((ucOccupiedPoint == 0U) && (lResult == 0)) {
		lResult = prvProbeResidualCup(0xFD28U,
			COFFEE2_ACTION_ROBOT_TAKE_LID, 9U);
		if (lResult == COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED) {
			ucOccupiedPoint = 9U;
		}
	}
	if ((ucOccupiedPoint == 0U) && (lResult != 0)) {
		return lResult;
	}
	if ((ucOccupiedPoint == 0U) &&
		(g_xCoffee2CupLidImage.aucCupCoils[4U] != 0U)) {
		ucOccupiedPoint = 3U;
	} else if ((ucOccupiedPoint == 0U) &&
		(g_xCoffee2CupLidImage.aucCupCoils[9U] != 0U)) {
		ucOccupiedPoint = 4U;
	} else if ((ucOccupiedPoint == 0U) &&
		(g_xCoffee2CupLidImage.aucLidCoils[4U] != 0U)) {
		ucOccupiedPoint = 5U;
	} else if ((ucOccupiedPoint == 0U) &&
		(g_xCoffee2CupLidImage.aucLidCoils[9U] != 0U)) {
		ucOccupiedPoint = 6U;
	}
	if (ucOccupiedPoint != 0U) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_WORKFLOW, COFFEE2_LOG_ORDER_SYSTEM,
			"MACHINE_INIT_CUP_PRESENT",
			COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED, "point",
			(int32_t)ucOccupiedPoint);
		return COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED;
	}
	return prvRunStep(0xFD2FU, COFFEE2_DEVICE_ROBOT,
		COFFEE2_ACTION_ROBOT_HOME, 0U, 0U,
		COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
}

/*-----------------------------------------------------------*/
static int32_t prvProbeResidualCup(uint16_t usStepBase,
	Coffee2Action_e xPickupAction, uint8_t ucOccupiedPoint)
{
	Coffee2IoState_t xIo;
	int32_t lResult;

	lResult = prvRunStep(usStepBase, COFFEE2_DEVICE_ROBOT,
		COFFEE2_ACTION_ROBOT_HOME, 0U, 0U,
		COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
	if ((lResult == 0) && ((uint16_t)xPickupAction != 0U)) {
		lResult = prvRunStep((uint16_t)(usStepBase + 1U),
			COFFEE2_DEVICE_ROBOT, xPickupAction, 0U, 0U,
			COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		lResult = prvRunStep((uint16_t)(usStepBase + 2U),
			COFFEE2_DEVICE_ROBOT, COFFEE2_ACTION_ROBOT_PUT_OUTPUT,
			1U, 0U, COFFEE2_WORKFLOW_ROBOT_MOTION_MS);
	}
	if (lResult == 0) {
		vTaskDelay(pdMS_TO_TICKS(500U));
		lResult = prvRunStep((uint16_t)(usStepBase + 3U),
			COFFEE2_DEVICE_IO_INPUT, COFFEE2_ACTION_REFRESH,
			0U, 0U, 3000U);
	}
	if (lResult != 0) {
		return lResult;
	}
	vCoffee2IoGetSnapshot(&xIo);
	if (xIo.xInput.aucMB1XPin[
		COFFEE2_EXTERNAL_DI_OUTPUT_FRONT_CUP] != 0U) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_WORKFLOW, COFFEE2_LOG_ORDER_SYSTEM,
			"MACHINE_INIT_RESIDUAL_CUP",
			COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED, "point",
			(int32_t)ucOccupiedPoint);
		return COFFEE2_WORKFLOW_ERROR_INIT_OCCUPIED;
	}
	return 0;
}

/*-----------------------------------------------------------*/
static int32_t prvRunFruit(uint8_t ucChannel, uint16_t usAmountMl,
	uint8_t ucClean, uint16_t usStepBase)
{
	Coffee2IoState_t xIo;
	uint8_t ucInputPoint;
	uint8_t ucValvePoint;
	uint8_t ucPumpPoint;
	uint8_t ucValveRunValue;
	uint32_t ulRunMs;
	uint16_t usLogOrder;
	int32_t lResult;

	if ((ucChannel < 1U) || (ucChannel > 2U) ||
		((ucClean == 0U) && (usAmountMl == 0U))) {
		return COFFEE2_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucInputPoint = (ucChannel == 1U) ?
		COFFEE2_EXTERNAL_DI_FRUIT_MILK_A_LOW :
		COFFEE2_EXTERNAL_DI_FRUIT_MILK_B_LOW;
	ucValvePoint = (ucChannel == 1U) ?
		COFFEE2_EXTERNAL_DO_FRUIT_MILK_A_VALVE :
		COFFEE2_EXTERNAL_DO_FRUIT_MILK_B_VALVE;
	ucPumpPoint = (ucChannel == 1U) ?
		COFFEE2_EXTERNAL_DO_FRUIT_MILK_A_PUMP :
		COFFEE2_EXTERNAL_DO_FRUIT_MILK_B_PUMP;
	ucValveRunValue = ((ucClean != 0U) && (ucChannel == 1U)) ? 1U : 0U;
	usLogOrder = (g_xCoffee2WorkflowStatus.xState ==
		COFFEE2_WORKFLOW_RUNNING) ?
		g_xCoffee2WorkflowStatus.usCurrentOrderId : COFFEE2_LOG_ORDER_DEBUG;
	g_xCoffee2WorkflowStatus.aucFruitState[ucChannel - 1U] =
		COFFEE2_MAINTENANCE_RUNNING;
	lResult = prvRunStep(usStepBase,
		COFFEE2_DEVICE_IO_INPUT, COFFEE2_ACTION_REFRESH,
		0U, 0U, 3000U);
	if (lResult == 0) {
		vCoffee2IoGetSnapshot(&xIo);
		if (xIo.xInput.aucMB1XPin[ucInputPoint] != 0U) {
			lResult = COFFEE2_WORKFLOW_ERROR_DEVICE;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_WORKFLOW, usLogOrder,
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
		ulRunMs = (ucClean != 0U) ? COFFEE2_FRUIT_MILK_CLEAN_MS :
			((uint32_t)usAmountMl * COFFEE2_FRUIT_MILK_MS_PER_ML);
		prvDelayWithServices(ulRunMs);
		if (g_xCoffee2WorkflowStatus.ucCancelRequested != 0U) {
			lResult = COFFEE2_WORKFLOW_ERROR_CANCELED;
		}
	}
	if (prvRunIoOutput((uint16_t)(usStepBase + 3U),
		ucPumpPoint, 0U) != 0) {
		lResult = COFFEE2_WORKFLOW_ERROR_SAFE_STOP;
	}
	if (prvRunIoOutput((uint16_t)(usStepBase + 4U),
		ucValvePoint, 0U) != 0) {
		lResult = COFFEE2_WORKFLOW_ERROR_SAFE_STOP;
	}
	g_xCoffee2WorkflowStatus.aucFruitState[ucChannel - 1U] =
		(lResult == 0) ? COFFEE2_MAINTENANCE_COMPLETED :
		COFFEE2_MAINTENANCE_FAILED;
	return lResult;
}

/*-----------------------------------------------------------*/
static int32_t prvRunMaintenance(const Coffee2MaintenanceRequest_t *pxRequest)
{
	int32_t lResult;

	if (pxRequest == NULL) {
		return COFFEE2_WORKFLOW_ERROR_UNSUPPORTED;
	}
	switch (pxRequest->xType) {
	case COFFEE2_MAINTENANCE_SYRUP_CLEAN:
		lResult = prvRunStep(0xFB10U, COFFEE2_DEVICE_SYRUP_MACHINE,
			COFFEE2_ACTION_SYRUP_CLEAN, 0U, 0U, 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(0xFB11U,
				COFFEE2_DEVICE_SYRUP_MACHINE, 5U,
				SYRUP_MACHINE_STATUS_SUCCESS,
				SYRUP_MACHINE_STATUS_FAILED);
		}
		return lResult;
	case COFFEE2_MAINTENANCE_COFFEE_CLEAN:
		g_xCoffee2WorkflowStatus.ucCoffeeCleanState =
			COFFEE2_MAINTENANCE_RUNNING;
		lResult = prvRunStep(0xFB20U,
			COFFEE2_DEVICE_COFFEE_MACHINE,
			COFFEE2_ACTION_COFFEE_CLEAN,
			pxRequest->usParameter0, 0U, 3000U);
		if (lResult == 0) {
			lResult = prvWaitDeviceReportedComplete(0xFB21U,
				COFFEE2_DEVICE_COFFEE_MACHINE, 1U,
				COFFEE2_F200_APPLICATION_COMPLETE,
				COFFEE2_F200_APPLICATION_FAILED);
		}
		g_xCoffee2WorkflowStatus.ucCoffeeCleanState =
			(lResult == 0) ? COFFEE2_MAINTENANCE_COMPLETED :
			COFFEE2_MAINTENANCE_FAILED;
		return lResult;
	case COFFEE2_MAINTENANCE_FRUIT_DISPENSE:
		return prvRunFruit((uint8_t)pxRequest->usParameter0,
			pxRequest->usParameter1, 0U,
			(uint16_t)(0xFC10U + (pxRequest->usParameter0 * 8U)));
	case COFFEE2_MAINTENANCE_FRUIT_CLEAN:
		return prvRunFruit((uint8_t)pxRequest->usParameter0, 0U, 1U,
			(uint16_t)(0xFC30U + (pxRequest->usParameter0 * 8U)));
	default:
		return COFFEE2_WORKFLOW_ERROR_UNSUPPORTED;
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvOrderValid(const Coffee2Order_t *pxOrder,
	int32_t *plError)
{
	uint16_t usCoffeeType;
	uint16_t usOutput;

	if (plError != NULL) {
		*plError = COFFEE2_WORKFLOW_ERROR_UNSUPPORTED;
	}
	if (pxOrder == NULL) {
		return 0U;
	}
	usCoffeeType = pxOrder->ausRegister[COFFEE2_REG_COFFEE_TYPE];
	if ((usCoffeeType != 0xFFFFU) &&
		(usCoffeeType > COFFEE2_COFFEE_RECIPE_MAX)) {
		return 0U;
	}
	if (pxOrder->ausRegister[COFFEE2_REG_RESERVED_0009] != 0U) {
		return 0U;
	}
	if (pxOrder->ausRegister[COFFEE2_REG_RESERVED_000C] != 0U) {
		return 0U;
	}
	if (pxOrder->ausRegister[COFFEE2_REG_ICE_AMOUNT] > 6553U) {
		return 0U;
	}
	usOutput = pxOrder->ausRegister[COFFEE2_REG_ONLINE_OUTPUT];
	if ((usOutput != 1U) && (usOutput != 2U)) {
		return 0U;
	}
	if (plError != NULL) {
		*plError = 0;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
static int32_t prvRefreshDeviceQuiet(uint16_t usStep,
	Coffee2DeviceId_e xDeviceId)
{
	Coffee2Command_t xCommand;
	EventBits_t xEvents;
	TickType_t xStartTick;
	uint8_t ucTerminalValid;
	uint8_t ucOrderStep;
	int32_t lTerminalResult;

	if ((xDeviceId <= COFFEE2_DEVICE_NONE) ||
		(xDeviceId >= COFFEE2_DEVICE_COUNT)) {
		return COFFEE2_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ulOrderId = (ucOrderStep != 0U) ?
		g_xCoffee2WorkflowStatus.usCurrentOrderId : COFFEE2_LOG_ORDER_DEBUG;
	xCommand.ulOrderEpoch = (ucOrderStep != 0U) ?
		g_xCoffee2WorkflowStatus.ulOrderEpoch : 0U;
	xCommand.usStepId = usStep;
	xCommand.usAction = (uint16_t)COFFEE2_ACTION_REFRESH;
	xCommand.ulTimeoutMs =
		(xDeviceId == COFFEE2_DEVICE_COFFEE_MACHINE) ?
			COFFEE2_WORKFLOW_DEVICE_IO_TIMEOUT_MS :
			COFFEE2_RTU_IO_TIMEOUT_MS;
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = (ucOrderStep != 0U) ?
		(uint8_t)COFFEE2_COMMAND_SOURCE_WORKFLOW :
		(uint8_t)COFFEE2_COMMAND_SOURCE_MAINTENANCE;
	xCommand.ucRetryLimit = 1U;
	if (xCoffee2CommandSubmit(&xCommand, pdMS_TO_TICKS(100U)) != pdPASS) {
		return COFFEE2_WORKFLOW_ERROR_QUEUE;
	}
	xStartTick = xTaskGetTickCount();
	for (;;) {
		xEvents = xCoffee2DeviceWaitCommand(xDeviceId,
			xCommand.ulOrderEpoch, xCommand.ulCommandId,
			pdMS_TO_TICKS(100U));
		if ((xEvents & COFFEE2_DEVICE_EVENT_COMMAND_DONE) != 0U) {
			return 0;
		}
		if ((xEvents & COFFEE2_DEVICE_EVENT_TERMINAL) != 0U) {
			lTerminalResult = lCoffee2DeviceGetTerminalResult(xDeviceId,
				xCommand.ulOrderEpoch, xCommand.ulCommandId,
				&ucTerminalValid);
			return (ucTerminalValid != 0U) ? lTerminalResult :
				COFFEE2_WORKFLOW_ERROR_DEVICE;
		}
		if ((ucOrderStep != 0U) &&
			(g_xCoffee2WorkflowStatus.ucCancelRequested != 0U)) {
			return COFFEE2_WORKFLOW_ERROR_CANCELED;
		}
		if ((xTaskGetTickCount() - xStartTick) >= pdMS_TO_TICKS(5000U)) {
			return COFFEE2_WORKFLOW_ERROR_TIMEOUT;
		}
		prvServiceHotWater();
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvBusinessConditionActive(uint8_t ucCondition)
{
	Coffee2IoState_t xIo;

	switch (ucCondition) {
	case COFFEE2_CONDITION_CUP_1:
		return (g_xCoffee2CupLidImage.aucCupCoils[4U] != 0U) ? 1U : 0U;
	case COFFEE2_CONDITION_CUP_2:
		return (g_xCoffee2CupLidImage.aucCupCoils[9U] != 0U) ? 1U : 0U;
	case COFFEE2_CONDITION_LID_1:
		return (g_xCoffee2CupLidImage.aucLidCoils[4U] != 0U) ? 1U : 0U;
	case COFFEE2_CONDITION_LID_2:
		return (g_xCoffee2CupLidImage.aucLidCoils[9U] != 0U) ? 1U : 0U;
	case COFFEE2_CONDITION_OUTPUT_1:
	case COFFEE2_CONDITION_OUTPUT_2:
		vCoffee2IoGetSnapshot(&xIo);
		return (xIo.xInput.aucMB1XPin[
			(ucCondition == COFFEE2_CONDITION_OUTPUT_1) ?
				COFFEE2_EXTERNAL_DI_OUTPUT_FRONT_CUP :
				COFFEE2_EXTERNAL_DI_OUTPUT_REAR_CUP] != 0U) ? 1U : 0U;
	default:
		return 0U;
	}
}

/*-----------------------------------------------------------*/
static int32_t prvWaitBusinessCondition(uint16_t usStep,
	uint8_t ucCondition)
{
	Coffee2DeviceId_e xDeviceId;
	int32_t lResult;
	uint8_t ucWaitLogged;
	uint8_t ucDelayIndex;

	ucWaitLogged = 0U;
	switch (ucCondition) {
	case COFFEE2_CONDITION_CUP_1:
	case COFFEE2_CONDITION_CUP_2:
		xDeviceId = COFFEE2_DEVICE_CUP_MACHINE;
		break;
	case COFFEE2_CONDITION_LID_1:
	case COFFEE2_CONDITION_LID_2:
		xDeviceId = COFFEE2_DEVICE_LID_MACHINE;
		break;
	case COFFEE2_CONDITION_OUTPUT_1:
	case COFFEE2_CONDITION_OUTPUT_2:
		xDeviceId = COFFEE2_DEVICE_IO_INPUT;
		break;
	default:
		return COFFEE2_WORKFLOW_ERROR_UNSUPPORTED;
	}
	prvPublish(COFFEE2_WORKFLOW_RUNNING, usStep, 0);
	for (;;) {
		if (g_xCoffee2WorkflowStatus.ucCancelRequested != 0U) {
			return COFFEE2_WORKFLOW_ERROR_CANCELED;
		}
		lResult = prvRefreshDeviceQuiet(usStep, xDeviceId);
		if (lResult != 0) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				g_xCoffee2WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_REFRESH_FAILED", lResult,
				"condition", (int32_t)ucCondition);
			return lResult;
		}
		if (prvBusinessConditionActive(ucCondition) != 0U) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				g_xCoffee2WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_READY", 0,
				"condition", (int32_t)ucCondition);
			return 0;
		}
		if (ucWaitLogged == 0U) {
			ucWaitLogged = 1U;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				g_xCoffee2WorkflowStatus.usCurrentOrderId,
				"BUSINESS_CONDITION_WAIT", 0,
				"condition", (int32_t)ucCondition);
		}
		for (ucDelayIndex = 0U; ucDelayIndex < 5U; ucDelayIndex++) {
			if (g_xCoffee2WorkflowStatus.ucCancelRequested != 0U) {
				return COFFEE2_WORKFLOW_ERROR_CANCELED;
			}
			prvServiceHotWater();
			vTaskDelay(pdMS_TO_TICKS(100U));
		}
	}
}

/*-----------------------------------------------------------*/
static int32_t prvWaitDeviceReportedComplete(uint16_t usStep,
	Coffee2DeviceId_e xDeviceId, uint8_t ucStatusIndex,
	uint16_t usSuccessValue, uint16_t usFailedValue)
{
	int32_t lResult;
	uint16_t usState;
	uint16_t usLogOrder;
	uint8_t ucWaitLogged;
	uint8_t ucOrderStep;
	uint8_t ucDelayIndex;

	if (((xDeviceId == COFFEE2_DEVICE_COFFEE_MACHINE) &&
		(ucStatusIndex >= 24U)) ||
		((xDeviceId == COFFEE2_DEVICE_SYRUP_MACHINE) &&
		(ucStatusIndex >= SYRUP_MACHINE_REGISTER_COUNT)) ||
		((xDeviceId != COFFEE2_DEVICE_COFFEE_MACHINE) &&
		(xDeviceId != COFFEE2_DEVICE_SYRUP_MACHINE))) {
		return COFFEE2_WORKFLOW_ERROR_UNSUPPORTED;
	}
	ucOrderStep = (usStep < 0xF000U) ? 1U : 0U;
	usLogOrder = (ucOrderStep != 0U) ?
		g_xCoffee2WorkflowStatus.usCurrentOrderId : COFFEE2_LOG_ORDER_DEBUG;
	ucWaitLogged = 0U;
	if (ucOrderStep != 0U) {
		prvPublish(COFFEE2_WORKFLOW_RUNNING, usStep, 0);
	}
	for (;;) {
		if ((ucOrderStep != 0U) &&
			(g_xCoffee2WorkflowStatus.ucCancelRequested != 0U)) {
			return COFFEE2_WORKFLOW_ERROR_CANCELED;
		}
		lResult = prvRefreshDeviceQuiet(usStep, xDeviceId);
		if (lResult != 0) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_REFRESH_FAILED", lResult,
				"device", (int32_t)xDeviceId);
			return lResult;
		}
		usState = (xDeviceId == COFFEE2_DEVICE_COFFEE_MACHINE) ?
			g_xCoffee2CoffeeMachineImage.ausStatus[ucStatusIndex] :
			g_xCoffee2SyrupImage.ausRegisters[ucStatusIndex];
		if (usState == usSuccessValue) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_COMPLETE", 0,
				"device", (int32_t)xDeviceId);
			return 0;
		}
		if (usState == usFailedValue) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_FAILED", COFFEE2_WORKFLOW_ERROR_DEVICE,
				"device", (int32_t)xDeviceId);
			return COFFEE2_WORKFLOW_ERROR_DEVICE;
		}
		if (ucWaitLogged == 0U) {
			ucWaitLogged = 1U;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_WORKFLOW, usLogOrder,
				"DEVICE_ACTION_WAIT", (int32_t)usState,
				"device", (int32_t)xDeviceId);
		}
		for (ucDelayIndex = 0U; ucDelayIndex < 5U; ucDelayIndex++) {
			if ((ucOrderStep != 0U) &&
				(g_xCoffee2WorkflowStatus.ucCancelRequested != 0U)) {
				return COFFEE2_WORKFLOW_ERROR_CANCELED;
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
	s_xHotWater.xCommand.ulOrderId = COFFEE2_LOG_ORDER_DEBUG;
	s_xHotWater.xCommand.usAction = (uint16_t)COFFEE2_ACTION_IO_WRITE;
	s_xHotWater.xCommand.ausParameter[0] =
		COFFEE2_EXTERNAL_DO_WATER_HEATER_RELAY;
	s_xHotWater.xCommand.ausParameter[1] = (ucValue != 0U) ? 1U : 0U;
	s_xHotWater.xCommand.ulTimeoutMs = COFFEE2_RTU_IO_TIMEOUT_MS;
	s_xHotWater.xCommand.ucDeviceId =
		(uint8_t)COFFEE2_DEVICE_IO_OUTPUT;
	s_xHotWater.xCommand.ucSource =
		(uint8_t)COFFEE2_COMMAND_SOURCE_MAINTENANCE;
	s_xHotWater.xCommand.ucRetryLimit = 1U;
	if (xCoffee2CommandSubmit(&s_xHotWater.xCommand, 0U) != pdPASS) {
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
		return COFFEE2_WORKFLOW_ERROR_IO;
	}
	*pucDone = 0U;
	if (s_xHotWater.ucIoPending == 0U) {
		return 0;
	}
	lResult = lCoffee2DeviceGetTerminalResult(
		COFFEE2_DEVICE_IO_OUTPUT,
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
	if (g_xCoffee2WorkflowStatus.ucHotWaterState == ucState) {
		return;
	}
	g_xCoffee2WorkflowStatus.ucHotWaterState = ucState;
	if (ucState == COFFEE2_MAINTENANCE_ALARM) {
		g_xCoffee2WorkflowStatus.xMachineState = COFFEE2_MACHINE_ALARM;
	}
	if (pcEvent != NULL) {
		(void)xCoffee2LogWriteFieldOrder(
			(ucState == COFFEE2_MAINTENANCE_ALARM) ?
				COFFEE2_LOG_LEVEL_ERROR : COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_WORKFLOW, COFFEE2_LOG_ORDER_DEBUG,
			pcEvent, lResult, "state", (int32_t)ucState);
	}
}

/*-----------------------------------------------------------*/
static void prvServiceHotWater(void)
{
	Coffee2IoState_t xIo;
	TickType_t xNow;
	uint32_t ulHeatMs;
	uint8_t ucDone;
	int32_t lResult;

	if ((s_xHotWater.ucPhase == COFFEE2_HOT_WATER_IDLE) &&
		(s_xHotWater.ucCancelRequested == 0U)) {
		return;
	}
	vCoffee2IoRefreshLocal();
	vCoffee2IoGetSnapshot(&xIo);
	xNow = xTaskGetTickCount();
	if (s_xHotWater.ucCancelRequested != 0U) {
		(void)ucCoffee2IoSetLocalOutput(
			COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
		if (s_xHotWater.ucIoPending != 0U) {
			(void)prvPollHotWaterIo(&ucDone);
			return;
		}
		if (s_xHotWater.ucIoPending == 0U) {
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE2_HOT_WATER_WAIT_OFF_DONE;
				s_xHotWater.ucCancelRequested = 0U;
			}
		}
		return;
	}
	switch ((Coffee2HotWaterPhase_e)s_xHotWater.ucPhase) {
	case COFFEE2_HOT_WATER_PREPARE_OFF:
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
			s_xHotWater.ucPhase = COFFEE2_HOT_WATER_WAIT_OFF_ALARM;
			return;
		}
		if (xIo.xInput.aucXPin[COFFEE2_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			s_xHotWater.ucAlarmReason = 1U;
			s_xHotWater.ucPhase = COFFEE2_HOT_WATER_WAIT_OFF_ALARM;
			return;
		}
		(void)ucCoffee2IoSetLocalOutput(
			COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 1U);
		s_xHotWater.xStartTick = xNow;
		s_xHotWater.ucPhase = COFFEE2_HOT_WATER_FILLING;
		prvSetHotWaterPublicState(COFFEE2_MAINTENANCE_RUNNING,
			"HOT_WATER_FILLING", 0);
		break;
	case COFFEE2_HOT_WATER_FILLING:
		if (xIo.xInput.aucXPin[COFFEE2_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			(void)ucCoffee2IoSetLocalOutput(
				COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			s_xHotWater.ucAlarmReason = 1U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE2_HOT_WATER_WAIT_OFF_ALARM;
			}
		} else if (xIo.xInput.aucXPin[
			COFFEE2_LOCAL_DI_HOT_WATER_LOW] != 0U) {
			(void)ucCoffee2IoSetLocalOutput(
				COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_WORKFLOW, COFFEE2_LOG_ORDER_DEBUG,
				"HOT_WATER_FILL_COMPLETE", 0, "low_level", 1);
			if (prvSubmitHotWaterIo(1U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE2_HOT_WATER_WAIT_HEATER_ON;
			}
		} else if ((xNow - s_xHotWater.xStartTick) >=
			pdMS_TO_TICKS(COFFEE2_HOT_WATER_FILL_TIMEOUT_MS)) {
			(void)ucCoffee2IoSetLocalOutput(
				COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
			s_xHotWater.ucAlarmReason = 2U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE2_HOT_WATER_WAIT_OFF_ALARM;
			}
		}
		break;
	case COFFEE2_HOT_WATER_WAIT_HEATER_ON:
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone == 0U) {
			break;
		}
		if (lResult != 0) {
			s_xHotWater.ucAlarmReason = 3U;
			s_xHotWater.ucPhase = COFFEE2_HOT_WATER_WAIT_OFF_ALARM;
			break;
		}
		s_xHotWater.xStartTick = xNow;
		s_xHotWater.ucPhase = COFFEE2_HOT_WATER_HEATING;
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_WORKFLOW, COFFEE2_LOG_ORDER_DEBUG,
			"HOT_WATER_HEATING", 0, "minutes",
			(int32_t)s_xHotWater.usHeatMinutes);
		break;
	case COFFEE2_HOT_WATER_HEATING:
		ulHeatMs = (uint32_t)s_xHotWater.usHeatMinutes * 60000U;
		if (xIo.xInput.aucXPin[COFFEE2_LOCAL_DI_HOT_WATER_HIGH] != 0U) {
			s_xHotWater.ucAlarmReason = 1U;
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE2_HOT_WATER_WAIT_OFF_ALARM;
			}
		} else if ((xNow - s_xHotWater.xStartTick) >=
			pdMS_TO_TICKS(ulHeatMs)) {
			if (prvSubmitHotWaterIo(0U) == pdPASS) {
				s_xHotWater.ucPhase =
					COFFEE2_HOT_WATER_WAIT_OFF_DONE;
			}
		}
		break;
	case COFFEE2_HOT_WATER_WAIT_OFF_DONE:
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone != 0U) {
			s_xHotWater.ucPhase = COFFEE2_HOT_WATER_IDLE;
			prvSetHotWaterPublicState(
				(lResult == 0) ? COFFEE2_MAINTENANCE_COMPLETED :
					COFFEE2_MAINTENANCE_ALARM,
				(lResult == 0) ? "HOT_WATER_COMPLETE" :
					"HOT_WATER_STOP_FAILED", lResult);
		}
		break;
	case COFFEE2_HOT_WATER_WAIT_OFF_ALARM:
		if (s_xHotWater.ucIoPending == 0U) {
			(void)prvSubmitHotWaterIo(0U);
			break;
		}
		lResult = prvPollHotWaterIo(&ucDone);
		if (ucDone != 0U) {
			s_xHotWater.ucPhase = COFFEE2_HOT_WATER_IDLE;
			prvSetHotWaterPublicState(COFFEE2_MAINTENANCE_ALARM,
				"HOT_WATER_ALARM", (lResult != 0) ? lResult :
					-(int32_t)s_xHotWater.ucAlarmReason);
		}
		break;
	default:
		s_xHotWater.ucPhase = COFFEE2_HOT_WATER_IDLE;
		break;
	}
}

/*-----------------------------------------------------------*/
static int32_t prvAbortDevices(void)
{
	Coffee2Command_t axCommand[3];
	EventBits_t xEvents;
	BaseType_t axSubmitted[3];
	uint8_t ucIndex;
	int32_t lResult;

	prvPublish(COFFEE2_WORKFLOW_CANCELING,
		g_xCoffee2WorkflowStatus.usCurrentStep,
		g_xCoffee2WorkflowStatus.lLastError);
	s_xHotWater.ucCancelRequested = 1U;
	(void)ucCoffee2IoSetLocalOutput(
		COFFEE2_LOCAL_DO_HOT_WATER_SUPPLY_VALVE, 0U);
	vCoffee2OrderCancelRequest(g_xCoffee2WorkflowStatus.ulOrderEpoch);
	memset(axCommand, 0, sizeof(axCommand));
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		axCommand[ucIndex].ucSource =
			(uint8_t)COFFEE2_COMMAND_SOURCE_WORKFLOW;
		axCommand[ucIndex].ulOrderId =
			g_xCoffee2WorkflowStatus.usCurrentOrderId;
		axCommand[ucIndex].ulOrderEpoch =
			g_xCoffee2WorkflowStatus.ulOrderEpoch;
		axCommand[ucIndex].ulTimeoutMs =
			COFFEE2_WORKFLOW_SAFE_STOP_MS;
		axCommand[ucIndex].usStepId = 0xFFF0U + ucIndex;
	}
	axCommand[0].ucDeviceId = (uint8_t)COFFEE2_DEVICE_ICE_MACHINE;
	axCommand[0].usAction = (uint16_t)COFFEE2_ACTION_ICE_SET_VALVE;
	axCommand[0].ausParameter[0] = 0U;
	axCommand[1].ucDeviceId =
		(uint8_t)COFFEE2_DEVICE_COFFEE_MACHINE;
	axCommand[1].usAction = (uint16_t)COFFEE2_ACTION_CANCEL;
	axCommand[2].ucDeviceId = (uint8_t)COFFEE2_DEVICE_ROBOT;
	axCommand[2].usAction = (uint16_t)COFFEE2_ACTION_CANCEL;

	lResult = 0;
	for (ucIndex = 0U; ucIndex < 3U; ucIndex++) {
		if ((ucIndex == 2U) && (s_ucManualOverride != 0U)) {
			axSubmitted[ucIndex] = pdPASS;
			continue;
		}
		axSubmitted[ucIndex] = xCoffee2CommandSubmitUrgent(
			&axCommand[ucIndex], pdMS_TO_TICKS(100U));
		if (axSubmitted[ucIndex] != pdPASS) {
			lResult = COFFEE2_WORKFLOW_ERROR_SAFE_STOP;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				g_xCoffee2WorkflowStatus.usCurrentOrderId,
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
		xEvents = xCoffee2DeviceWaitCommand(
			(Coffee2DeviceId_e)axCommand[ucIndex].ucDeviceId,
			axCommand[ucIndex].ulOrderEpoch,
			axCommand[ucIndex].ulCommandId,
			pdMS_TO_TICKS(COFFEE2_WORKFLOW_SAFE_STOP_MS));
		if ((xEvents & (COFFEE2_DEVICE_EVENT_COMMAND_DONE |
			COFFEE2_DEVICE_EVENT_CANCELED)) == 0U) {
			lResult = COFFEE2_WORKFLOW_ERROR_SAFE_STOP;
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_ERROR,
				COFFEE2_LOG_SOURCE_WORKFLOW,
				g_xCoffee2WorkflowStatus.usCurrentOrderId,
				"SAFE_STOP_ACK_FAILED", lResult, "device",
				(int32_t)axCommand[ucIndex].ucDeviceId);
		}
	}
	if (prvSetProductOutputsOff() != 0) {
		lResult = COFFEE2_WORKFLOW_ERROR_SAFE_STOP;
	}
	(void)xCoffee2LogWriteOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_WORKFLOW,
		g_xCoffee2WorkflowStatus.usCurrentOrderId,
		(lResult == 0) ? "SAFE_STOP_CONFIRMED" :
			"SAFE_STOP_INCOMPLETE", lResult);
	return lResult;
}

/*-----------------------------------------------------------*/
static void prvServiceIoRefresh(void)
{
	static const Coffee2DeviceId_e axIdleRefreshDevices[] = {
		COFFEE2_DEVICE_COFFEE_MACHINE,
		COFFEE2_DEVICE_CUP_MACHINE,
		COFFEE2_DEVICE_SYRUP_MACHINE,
		COFFEE2_DEVICE_LID_MACHINE,
		COFFEE2_DEVICE_ICE_MACHINE,
		COFFEE2_DEVICE_SCALE,
		COFFEE2_DEVICE_POWER_METER
	};
	static TickType_t xNextRefreshTick;
	static uint8_t ucIdleRefreshIndex;
	Coffee2Command_t xCommand;
	TickType_t xNow;

	xNow = xTaskGetTickCount();
	if ((int32_t)(xNow - xNextRefreshTick) < 0) {
		return;
	}
	xNextRefreshTick = xNow +
		pdMS_TO_TICKS(COFFEE2_WORKFLOW_IO_REFRESH_MS);
	vCoffee2IoRefreshLocal();
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ucSource = (uint8_t)COFFEE2_COMMAND_SOURCE_WORKFLOW;
	xCommand.usAction = (uint16_t)COFFEE2_ACTION_REFRESH;
	xCommand.ulTimeoutMs = 1000U;
	xCommand.ucDeviceId = (uint8_t)COFFEE2_DEVICE_IO_INPUT;
	(void)xCoffee2CommandSubmit(&xCommand, 0U);
	xCommand.ulCommandId = 0U;
	xCommand.ucDeviceId = (uint8_t)COFFEE2_DEVICE_IO_OUTPUT;
	(void)xCoffee2CommandSubmit(&xCommand, 0U);
	if ((g_xCoffee2WorkflowStatus.xState !=
		COFFEE2_WORKFLOW_RUNNING) &&
		(g_xCoffee2WorkflowStatus.xState !=
		COFFEE2_WORKFLOW_CANCELING)) {
		xCommand.ulCommandId = 0U;
		xCommand.ucDeviceId = (uint8_t)
			axIdleRefreshDevices[ucIdleRefreshIndex];
		(void)xCoffee2CommandSubmit(&xCommand, 0U);
		ucIdleRefreshIndex++;
		if (ucIdleRefreshIndex >=
			(sizeof(axIdleRefreshDevices) /
				sizeof(axIdleRefreshDevices[0]))) {
			ucIdleRefreshIndex = 0U;
		}
	}
}

/*-----------------------------------------------------------*/
static void prvPublish(Coffee2WorkflowState_e xState,
	uint16_t usStep, int32_t lError)
{
	uint16_t usProductionStatus;

	taskENTER_CRITICAL();
	g_xCoffee2WorkflowStatus.xState = xState;
	g_xCoffee2WorkflowStatus.usCurrentStep = usStep;
	g_xCoffee2WorkflowStatus.lLastError = lError;
	if (g_xCoffee2WorkflowStatus.ucHotWaterState ==
		COFFEE2_MAINTENANCE_ALARM) {
		g_xCoffee2WorkflowStatus.xMachineState = COFFEE2_MACHINE_ALARM;
	} else if ((xState == COFFEE2_WORKFLOW_RUNNING) ||
		(xState == COFFEE2_WORKFLOW_CANCELING)) {
		g_xCoffee2WorkflowStatus.xMachineState = COFFEE2_MACHINE_BUSY;
	} else if (xState == COFFEE2_WORKFLOW_FAILED) {
		g_xCoffee2WorkflowStatus.xMachineState = COFFEE2_MACHINE_ALARM;
	} else if (g_xCoffee2WorkflowStatus.xMachineState !=
		COFFEE2_MACHINE_INITIALIZING) {
		g_xCoffee2WorkflowStatus.xMachineState = COFFEE2_MACHINE_IDLE;
	}
	taskEXIT_CRITICAL();
	switch (xState) {
	case COFFEE2_WORKFLOW_RUNNING:
	case COFFEE2_WORKFLOW_CANCELING:
		usProductionStatus = COFFEE2_PRODUCTION_RUNNING;
		break;
	case COFFEE2_WORKFLOW_COMPLETED:
		usProductionStatus = COFFEE2_PRODUCTION_COMPLETED;
		break;
	case COFFEE2_WORKFLOW_FAILED:
		usProductionStatus = COFFEE2_PRODUCTION_FAILED;
		break;
	default:
		usProductionStatus = COFFEE2_PRODUCTION_IDLE;
		break;
	}
	vCoffee2ServerPublishWorkflow(
		g_xCoffee2WorkflowStatus.usCurrentOrderId,
		usProductionStatus, usStep, lError);
}
