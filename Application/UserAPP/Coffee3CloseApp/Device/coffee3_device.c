/**
  * @file      coffee3_device.c
  * @brief     实现 Coffee3 设备绑定、命令路由、状态与完成事件。
  * @author    WHong
  * @date      2026-09-24
  */

#include "coffee3_device.h"
#include "coffee3_robot_tcp.h"
#include "coffee3_rtu_bus.h"

#include <stddef.h>
#include <string.h>

#include "coffee3_app_config.h"
#include "coffee3_log.h"
#include "coffee3_workflow.h"
#include "task.h"

/** @brief 机器人路由与第二至第五路 RTU 路由的槽位总数。 */
#define COFFEE3_ROUTE_COUNT                  6U
#define COFFEE3_TERMINAL_HISTORY_COUNT       \
	(COFFEE3_COMMAND_QUEUE_LENGTH + 2U)

#include "coffee3_device_bindings.h"

/** @brief 保存有界命令完成历史，查询时同时匹配序号与订单代次。 */
typedef struct {
	uint32_t ulCommandId; /*!< 提交时分配的非零命令序号。 */
	uint32_t ulOrderEpoch; /*!< 命令归属与协作取消使用的代次。 */
	int32_t lResult; /*!< 设备拥有者返回的原始终态结果。 */
	uint16_t usAction; /*!< 已完成的产品动作编号。 */
	uint8_t ucTimedOut; /*!< 非零表示失败按超时分类。 */
	uint8_t ucValid; /*!< 非零表示本历史槽保存有效终态。 */
} Coffee3TerminalSnapshot_t;

/** @brief 按逻辑设备编号索引的公共运行状态表。 */
COFFEE3_CCM_DATA
Coffee3DeviceStatus_t
	g_axCoffee3DeviceStatus[COFFEE3_DEVICE_COUNT];

/** @brief 每个实际设备独立使用的静态事件组存储区。 */
COFFEE3_CCM_DATA
static StaticEventGroup_t
	s_axDeviceEventStorage[COFFEE3_DEVICE_COUNT];
/** @brief 按设备编号索引的独立事件组句柄。 */
COFFEE3_CCM_DATA
static EventGroupHandle_t
	s_axDeviceEvents[COFFEE3_DEVICE_COUNT];
/** @brief 按固定路由编号索引、由设备任务拥有的命令队列。 */
COFFEE3_CCM_DATA
static QueueHandle_t s_axRouteQueues[COFFEE3_ROUTE_COUNT];
/** @brief 提交命令时递增分配的命令序号，跳过零值。 */
COFFEE3_CCM_DATA
static uint32_t s_ulNextCommandId;
/** @brief 最近一次请求协作取消的工作流订单代次。 */
COFFEE3_CCM_DATA
static volatile uint32_t s_ulCanceledOrderEpoch;
/** @brief 非零表示所有设备事件组已经成功初始化。 */
COFFEE3_CCM_DATA
static uint8_t s_ucInitialized;
COFFEE3_CCM_DATA
static Coffee3TerminalSnapshot_t
	s_aaxTerminalHistory[COFFEE3_DEVICE_COUNT]
	[COFFEE3_TERMINAL_HISTORY_COUNT]; /*!< 各设备最近的有界完成历史。 */
COFFEE3_CCM_DATA
static uint8_t s_aucTerminalHistoryHead[COFFEE3_DEVICE_COUNT];
	/*!< 各设备下一次写入完成历史的槽位。 */

static BaseType_t prvSubmit(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks, uint8_t ucUrgent);

/**
  * @brief  把原始命令终态转换为工作流等待使用的事件位。
  * @param[in] lResult 设备拥有者返回的原始结果。
  * @param[in] ucTimedOut 非零表示结果按超时分类。
  * @param[in] usAction 已完成的产品动作。
  * @retval EventBits_t 与该终态对应的完成、失败、超时或取消事件位。
  */
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
/**
  * @brief  查询指定设备、订单代次和命令序号对应的历史终态结果。
  * @param[in] xDeviceId 逻辑设备编号。
  * @param[in] ulOrderEpoch 命令归属与协作取消使用的代次。
  * @param[in] ulCommandId 非零命令序号。
  * @param[out] pucValid 查询有效标志；可为空。
  * @retval int32_t 命中时返回原始终态结果；未命中时返回零且有效标志清零。
  */
int32_t lCoffee3DeviceGetTerminalResult(Coffee3DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, uint8_t *pucValid)
{
	uint8_t ucIndex; /*!< 当前搜索的历史槽位。 */
	int32_t lResult; /*!< 命中的原始结果，未命中时保持零。 */

	if (pucValid != NULL) {
		*pucValid = 0U;
	}
	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT)) {
		return 0;
	}
	/* 与拥有者发布使用同一临界区搜索；零结果只有在有效标志置位时才代表成功。 */
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
/**
  * @brief  创建每个逻辑设备的静态事件组并清空运行状态。
  * @retval pdPASS 设备事件与状态初始化完成，或此前已经完成。
  * @retval pdFAIL 任一静态事件组创建失败。
  */
BaseType_t xCoffee3DeviceInitialize(void)
{
	uint8_t ucIndex; /*!< 当前创建事件组的设备编号。 */

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
/**
  * @brief  注册由设备任务拥有的一条命令路由队列。
  * @param[in] ucRouteId 固定路由编号。
  * @param[in] xQueue 生命周期覆盖设备服务期的队列句柄。
  */
void vCoffee3DeviceRegisterRoute(uint8_t ucRouteId, QueueHandle_t xQueue)
{
	if ((ucRouteId >= COFFEE3_ROUTE_COUNT) || (xQueue == NULL)) {
		return;
	}
	s_axRouteQueues[ucRouteId] = xQueue;
}

/*-----------------------------------------------------------*/
/**
  * @brief  查找指定逻辑设备的固定通信绑定。
  * @param[in] xDeviceId 逻辑设备编号。
  * @retval 非空 与设备对应的只读静态绑定。
  * @retval NULL 设备没有绑定。
  */
const Coffee3DeviceBinding_t *pxCoffee3DeviceGetBinding(
	Coffee3DeviceId_e xDeviceId)
{
	uint8_t ucIndex; /*!< 当前检查的设备绑定索引。 */

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
/**
  * @brief  把命令提交到设备绑定选择的普通队列位置。
  * @param[in,out] pxCommand 待提交命令；返回 pdFAIL 前也可能已分配非零命令序号。
  * @param[in] xWaitTicks 等待队列空间的最大 RTOS 节拍数。
  * @retval pdPASS 命令已复制进路由队列或机器人延迟槽。
  * @retval pdFAIL 参数、绑定、路由或队列无效，或队列未接收命令。
  * @note pdPASS 只表示命令已排队，不表示设备已经执行成功。
  * @note 普通队列提交失败时，已申请的人工占用标志会回退。
  */
BaseType_t xCoffee3CommandSubmit(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks)
{
	return prvSubmit(pxCommand, xWaitTicks, 0U);
}

/*-----------------------------------------------------------*/
/**
  * @brief  把紧急命令提交到设备路由队列前端。
  * @param[in,out] pxCommand 待提交命令；返回 pdFAIL 前也可能已分配非零命令序号。
  * @param[in] xWaitTicks 等待队列空间的最大 RTOS 节拍数。
  * @retval pdPASS 命令已复制进路由队列或机器人延迟槽。
  * @retval pdFAIL 参数、绑定、路由或队列无效，或队列未接收命令。
  * @note pdPASS 只表示命令已排队，不表示设备已经执行成功；本接口不要求安全停止标志。
  */
BaseType_t xCoffee3CommandSubmitUrgent(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks)
{
	return prvSubmit(pxCommand, xWaitTicks, 1U);
}

/*-----------------------------------------------------------*/
/**
  * @brief  发布需要协作取消的非零命令归属代次。
  * @param[in] ulOrderEpoch 待取消命令归属代次；零值会被忽略。
  */
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
/**
  * @brief  判断工作流命令是否属于已取消订单代次。
  * @param[in] pxCommand 设备任务当前拥有的命令；可为空。
  * @retval 1 命令应在下一个协作检查点停止。
  * @retval 0 命令不受当前取消请求影响，或属于安全停止动作。
  */
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
/**
  * @brief  完成命令编号分配、人工占用申请与路由队列提交。
  * @param[in,out] pxCommand 待提交命令；成功前会写入命令序号与标志。
  * @param[in] xWaitTicks 等待路由队列空间的最大节拍数。
  * @param[in] ucUrgent 非零时提交到队列前端。
  * @retval pdPASS 命令已排队或已交给机器人延迟槽。
  * @retval pdFAIL 校验、人工占用申请或队列提交失败。
  */
static BaseType_t prvSubmit(Coffee3Command_t *pxCommand,
	TickType_t xWaitTicks, uint8_t ucUrgent)
{
	const Coffee3DeviceBinding_t *pxBinding; /*!< 命令目标设备的固定绑定。 */
	QueueHandle_t xQueue; /*!< 绑定选择的设备拥有者队列。 */
	BaseType_t xResult; /*!< 队列或机器人延迟槽的提交结果。 */

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
	/* 步骤 1：在复制到队列前分配不会为零的命令序号。 */
	if (pxCommand->ulCommandId == 0U) {
		taskENTER_CRITICAL();
		s_ulNextCommandId++;
		if (s_ulNextCommandId == 0U) {
			s_ulNextCommandId = 1U;
		}
		pxCommand->ulCommandId = s_ulNextCommandId;
		taskEXIT_CRITICAL();
	}
	/* 步骤 2：机器人运动调试进入拥有者的单一延迟槽，基础控制仍走普通路由。 */
	if ((pxCommand->ucDeviceId == (uint8_t)COFFEE3_DEVICE_ROBOT) &&
		(pxCommand->ucSource == (uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) &&
		((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_DEBUG) != 0U) &&
		((pxCommand->usAction < COFFEE3_ACTION_ROBOT_START) ||
		 (pxCommand->usAction > COFFEE3_ACTION_ROBOT_MANUAL_MODE)) &&
		(pxCommand->usAction != COFFEE3_ACTION_REFRESH)) {
		return xCoffee3RobotTcpSubmitManualMotion(pxCommand);
	}
	if ((pxCommand->ucSource == COFFEE3_COMMAND_SOURCE_SERVER) &&
		((pxCommand->ucDeviceId != (uint8_t)COFFEE3_DEVICE_ROBOT) ||
		 ((pxCommand->ucFlags & COFFEE3_COMMAND_FLAG_DEBUG) == 0U))) {
		if (xCoffee3WorkflowAcquireManual() != pdPASS) {
			return pdFAIL;
		}
		pxCommand->ucFlags |= COFFEE3_COMMAND_FLAG_MANUAL_RESERVED;
		/* 步骤 3：服务端命令申请人工占用；只读刷新保持普通优先级。 */
		ucUrgent = (pxCommand->usAction != COFFEE3_ACTION_REFRESH) ?
			1U : 0U;
	}
	/* 步骤 4：队列复制完整命令但不抢占当前 IO；失败时归还人工占用。 */
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
/**
  * @brief  发布命令开始状态并清除上一轮终态事件。
  * @param[in] pxCommand 设备任务即将执行的命令。
  */
void vCoffee3DeviceCommandStarted(const Coffee3Command_t *pxCommand)
{
	Coffee3DeviceStatus_t *pxStatus; /*!< 目标设备的公共状态。 */
	const Coffee3DeviceBinding_t *pxBinding; /*!< 目标设备的固定绑定。 */
	EventGroupHandle_t xEvents; /*!< 目标设备的独立事件组。 */
	uint8_t ucDeviceId; /*!< 经校验的目标设备编号。 */

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId == (uint8_t)COFFEE3_DEVICE_NONE) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE3_DEVICE_COUNT)) {
		return;
	}
	ucDeviceId = pxCommand->ucDeviceId;
	pxBinding = pxCoffee3DeviceGetBinding((Coffee3DeviceId_e)ucDeviceId);
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
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
			"Debug running: device=%s action=%u",
			(pxBinding != NULL) ? pxBinding->pcName : "Unknown",
			(unsigned int)pxCommand->usAction);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  发布命令终态、原始结果、事件分类与完成历史。
  * @param[in] pxCommand 已完成的命令。
  * @param[in] lResult 设备拥有者返回的原始结果，零表示成功。
  * @param[in] ucTimedOut 非零表示失败按超时分类。
  * @note 终态需结合 ucTerminalValid 或历史查询有效标志判断。
  */
void vCoffee3DeviceCommandCompleted(const Coffee3Command_t *pxCommand,
	int32_t lResult, uint8_t ucTimedOut)
{
	Coffee3DeviceStatus_t *pxStatus; /*!< 目标设备的公共状态。 */
	const Coffee3DeviceBinding_t *pxBinding; /*!< 目标设备的固定绑定。 */
	EventGroupHandle_t xEvents; /*!< 目标设备的独立事件组。 */
	EventBits_t xSetBits; /*!< 本次完成需要发布的事件位集合。 */
	uint8_t ucDeviceId; /*!< 经校验的目标设备编号。 */
	uint8_t ucHistoryIndex; /*!< 本次写入的完成历史槽位。 */

	if ((pxCommand == NULL) ||
		(pxCommand->ucDeviceId == (uint8_t)COFFEE3_DEVICE_NONE) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE3_DEVICE_COUNT)) {
		return;
	}
	ucDeviceId = pxCommand->ucDeviceId;
	pxBinding = pxCoffee3DeviceGetBinding((Coffee3DeviceId_e)ucDeviceId);
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
		if (g_axCoffee3DeviceStatus[ucDeviceId].ucReady != 0U) {
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
	/* 步骤 1：先提交命令身份与原始结果，再唤醒等待者。 */
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
	/* 步骤 2：前台命令不改写 RTU 的 ucOnline 字段；成功终态仍会置 ONLINE 事件位。 */
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
		if (lResult == COFFEE3_COMMAND_RESULT_SUPERSEDED) {
			/* 机器人拥有者会把被替换动作与新动作合并记录。 */
		} else if (lResult == COFFEE3_COMMAND_RESULT_CANCELED) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"Debug canceled: device=%s action=%u result=%ld",
				(pxBinding != NULL) ? pxBinding->pcName : "Unknown",
				(unsigned int)pxCommand->usAction, (long)lResult);
		} else if (ucTimedOut != 0U) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"Debug timeout: device=%s action=%u result=%ld",
				(pxBinding != NULL) ? pxBinding->pcName : "Unknown",
				(unsigned int)pxCommand->usAction, (long)lResult);
		} else if (lResult == 0) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"Debug complete: device=%s action=%u",
				(pxBinding != NULL) ? pxBinding->pcName : "Unknown",
				(unsigned int)pxCommand->usAction);
		} else {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"Debug failed: device=%s action=%u result=%ld",
				(pxBinding != NULL) ? pxBinding->pcName : "Unknown",
				(unsigned int)pxCommand->usAction, (long)lResult);
		}
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  发布机器人当前事务阶段。
  * @param[in] xPhase 不超过恢复阶段的机器人事务阶段。
  */
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
/**
  * @brief  发布机器人是否已经接受当前命令。
  * @param[in] ucAccepted 非零表示已观察到接受边沿。
  */
void vCoffee3DeviceSetRobotAccepted(uint8_t ucAccepted)
{
	taskENTER_CRITICAL();
	g_axCoffee3DeviceStatus[COFFEE3_DEVICE_ROBOT].ucRobotAccepted =
		(ucAccepted != 0U) ? 1U : 0U;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
/**
  * @brief  设置设备控制就绪状态并同步就绪事件位。
  * @param[in] xDeviceId 逻辑设备编号。
  * @param[in] ucReady 非零表示设备允许接受业务动作。
  */
void vCoffee3DeviceSetReady(Coffee3DeviceId_e xDeviceId,
	uint8_t ucReady)
{
	EventGroupHandle_t xEvents; /*!< 目标设备的独立事件组。 */

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
/**
  * @brief  设置设备命令是否正在等待链路恢复。
  * @param[in] xDeviceId 逻辑设备编号。
  * @param[in] ucRecovering 非零表示当前命令保持忙并等待恢复。
  */
void vCoffee3DeviceSetRecovering(Coffee3DeviceId_e xDeviceId,
	uint8_t ucRecovering)
{
	EventGroupHandle_t xEvents; /*!< 目标设备的独立事件组。 */

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
/**
  * @brief  设置设备通信在线状态并同步通信故障事件位。
  * @param[in] xDeviceId 逻辑设备编号。
  * @param[in] ucOnline 非零表示设备通信拥有者确认链路在线。
  */
void vCoffee3DeviceSetOnline(Coffee3DeviceId_e xDeviceId,
	uint8_t ucOnline)
{
	EventGroupHandle_t xEvents; /*!< 目标设备的独立事件组。 */

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
		if (g_axCoffee3DeviceStatus[xDeviceId].ucReady != 0U) {
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
/**
  * @brief  读取指定设备当前的事件组位。
  * @param[in] xDeviceId 逻辑设备编号。
  * @retval EventBits_t 当前事件位；设备无效或未初始化时返回零。
  */
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
/**
  * @brief  在限定时间内等待指定命令身份对应的终态。
  * @param[in] xDeviceId 逻辑设备编号。
  * @param[in] ulOrderEpoch 预期命令归属代次。
  * @param[in] ulCommandId 预期非零命令序号。
  * @param[in] xWaitTicks 最大等待 RTOS 节拍数。
  * @retval EventBits_t 匹配命令的终态事件；参数无效或超时返回零。
  */
EventBits_t xCoffee3DeviceWaitCommand(Coffee3DeviceId_e xDeviceId,
	uint32_t ulOrderEpoch, uint32_t ulCommandId, TickType_t xWaitTicks)
{
	EventBits_t xBits; /*!< 本轮事件组等待返回的唤醒位。 */
	Coffee3DeviceStatus_t xStatus; /*!< 临界区内复制的设备状态快照。 */
	TickType_t xWaitStart; /*!< 本次等待开始的 RTOS 节拍。 */
	TickType_t xRemaining; /*!< 当前剩余的等待节拍预算。 */
	EventBits_t xTerminal; /*!< 精确匹配命令身份后的终态事件。 */
	uint8_t ucIndex; /*!< 当前搜索的完成历史槽位。 */

	if ((xDeviceId <= COFFEE3_DEVICE_NONE) ||
		(xDeviceId >= COFFEE3_DEVICE_COUNT) ||
		(s_axDeviceEvents[xDeviceId] == NULL) ||
		(ulCommandId == 0U)) {
		return 0U;
	}
	/* 事件位只负责唤醒；在同一总预算内精确匹配归属代次与命令序号。 */
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
			/* 无关终态可能保持置位，短暂让出 CPU，避免空转影响总线拥有者。 */
			vTaskDelay(1U);
		}
	}
}
