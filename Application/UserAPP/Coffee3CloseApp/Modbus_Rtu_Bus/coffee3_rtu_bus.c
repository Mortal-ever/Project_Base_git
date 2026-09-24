/**
  * @file      coffee3_rtu_bus.c
  * @brief     实现 Coffee3 四路串行、单协议 UART 总线拥有者任务。
  * @author    WHong
  * @date      2026-09-24
  */

#include "coffee3_rtu_bus.h"

#include <stddef.h>
#include <string.h>

#include "coffee3_device.h"
#include "coffee3_device_image.h"
#include "coffee3_io.h"
#include "coffee3_log.h"
#include "coffee_machine_m50.h"
#include "cup_lid_shengshu.h"
#include "ice_machine_modbus.h"
#include "io_module_modbus_digital.h"
#include "power_meter_ddsu666.h"
#include "scale_bsq_dg_v2.h"
#include "syrup_machine_modbus.h"
#include "queue.h"
#include "task.h"
#include "transport_uart.h"
#include "usart.h"

/** @brief 保存一路总线的静态队列、传输通道和健康轮询状态。 */
typedef struct {
	StaticQueue_t xQueueStorage; /*!< FreeRTOS 静态命令队列控制块。 */
	uint8_t aucQueueStorage[
		COFFEE3_COMMAND_QUEUE_LENGTH * sizeof(Coffee3Command_t)]; /*!< 队列项的静态存储区。 */
	QueueHandle_t xQueue; /*!< 本总线唯一接收前台命令的队列句柄。 */
	TransportChannel_t xChannel; /*!< 绑定到物理 UART 的通用传输通道。 */
	TransportUartContext_t xTransport; /*!< UART 传输层的私有运行上下文。 */
	ModbusPort_t *pxPort; /*!< 所选协议为 Modbus RTU 时使用的客户机端口。 */
	TickType_t xLastTransactionTick; /*!< 最近一次前台事务结束的系统节拍。 */
	TickType_t axNextPollTick[COFFEE3_DEVICE_COUNT]; /*!< 各设备下一次后台轮询的绝对节拍。 */
	uint8_t aucPollMisses[COFFEE3_DEVICE_COUNT]; /*!< 各设备连续健康轮询失败次数。 */
	uint8_t aucPollFaultMask[COFFEE3_DEVICE_COUNT]; /*!< 各设备最近一次已记录的故障位掩码。 */
	uint8_t aucPollFaultKnown[COFFEE3_DEVICE_COUNT]; /*!< 对应故障位掩码是否已有有效样本。 */
	uint8_t aucPollIndex[COFFEE3_DEVICE_COUNT]; /*!< 本总线轮询表中的逻辑设备编号。 */
	uint8_t ucPollCount; /*!< 本总线参与后台轮询的设备数量。 */
	uint8_t ucPollCursor; /*!< 下一次查找到期设备时使用的轮询起点。 */
	uint8_t ucCreated; /*!< 传输通道和所选协议端口均创建成功时为 1。 */
} Coffee3RtuBusContext_t;

/** @brief Bus2 至 Bus5 按紧凑下标排列的公开运行状态。 */
Coffee3RtuBusStatus_t
	g_axCoffee3RtuBusStatus[COFFEE3_RTU_BUS_COUNT];

/** @brief Bus2 至 Bus5 的 UART、波特率、物理路由和唯一协议映射。 */
static const Coffee3RtuBusConfig_t s_axBusConfigs[
	COFFEE3_RTU_BUS_COUNT] = {
	{ &huart2, "coffee3_bus2", COFFEE3_BUS2_DEFAULT_BAUD, 2U,
		COFFEE3_BUS2_PROTOCOL },
	{ &huart3, "coffee3_bus3", COFFEE3_BUS3_DEFAULT_BAUD, 3U,
		COFFEE3_BUS3_PROTOCOL },
	{ &huart4, "coffee3_bus4", COFFEE3_BUS4_DEFAULT_BAUD, 4U,
		COFFEE3_BUS4_PROTOCOL },
	{ &huart5, "coffee3_bus5", COFFEE3_BUS5_DEFAULT_BAUD, 5U,
		COFFEE3_BUS5_PROTOCOL }
};

/** @brief Bus2 至 Bus5 按紧凑下标排列的私有运行资源。 */
static Coffee3RtuBusContext_t
	s_axBusContexts[COFFEE3_RTU_BUS_COUNT];

/** @brief 仅为配置为 Modbus RTU 的总线预留客户机端口。 */
static ModbusPort_t s_axModbusPorts[COFFEE3_MODBUS_BUS_COUNT];
static uint16_t s_usM50LastState; /*!< 最近一次成功上报的咖啡机原始状态字。 */
static uint8_t s_ucM50StateKnown; /*!< 已获得咖啡机状态样本时为 1。 */

static HAL_StatusTypeDef prvConfigureUart(UART_HandleTypeDef *pxUart,
	uint32_t ulBaudRate);
static ModbusPortResult_e prvExecute(Coffee3RtuBusContext_t *pxContext,
	const Coffee3DeviceBinding_t *pxBinding,
	const Coffee3Command_t *pxCommand);
static Coffee3LogSource_e prvGetLogSource(uint8_t ucBusId);
static Coffee3LogSource_e prvGetDeviceLogSource(uint8_t ucDeviceId);
static const char *prvGetDeviceProtocolEvent(uint8_t ucDeviceId);
static const char *prvGetBusLinkEvent(uint8_t ucBusId);
static void prvLogDeviceBindings(const Coffee3RtuBusConfig_t *pxConfig);
static uint8_t prvFindBusIndex(uint8_t ucBusId);
static uint8_t prvFindModbusPortIndex(uint8_t ucBusIndex);
static void prvLogCommandFailure(const Coffee3RtuBusConfig_t *pxConfig,
	const Coffee3Command_t *pxCommand, ModbusPortResult_e xResult);
static void prvLogIoWriteExpected(const Coffee3Command_t *pxCommand);
static void prvLogIoWrite(const Coffee3Command_t *pxCommand,
	ModbusPortResult_e xResult, const IoModuleModbusDigitalImage_t *pxImage);
static void prvM50StatusCallback(const CoffeeMachineM50Image_t *pxImage,
	ModbusPortResult_e xResult, uint8_t ucConsecutiveMisses,
	const void *pvContext);
static uint8_t prvCommandCanceled(const void *pvContext);
static const char *prvModbusResultName(ModbusPortResult_e xResult);
static void prvInitializePollSchedule(Coffee3RtuBusContext_t *pxContext,
	uint8_t ucBusId);
static BaseType_t prvTryBackgroundPoll(Coffee3RtuBusContext_t *pxContext,
	const Coffee3RtuBusConfig_t *pxConfig, Coffee3RtuBusStatus_t *pxStatus);
static void prvPublishPollHealth(Coffee3RtuBusContext_t *pxContext,
	const Coffee3RtuBusConfig_t *pxConfig,
	const Coffee3Command_t *pxCommand, ModbusPortResult_e xResult);
static const char *prvActionName(uint16_t usAction);

/*-----------------------------------------------------------*/
/**
  * @brief  依次将 UART2 至 UART5 恢复为 Coffee3 业务默认参数。
  * @retval HAL_OK 四路业务 UART 初始化成功，且 UART6 已停用。
  * @retval 其他 HAL_StatusTypeDef 首个失败的 HAL 操作结果。
  * @note   每路 UART 均配置为 8 数据位、无校验、1 停止位。
  */
HAL_StatusTypeDef xCoffee3SerialApplyDefaults(void)
{
	HAL_StatusTypeDef xResult; /*!< 当前 UART 配置步骤的 HAL 结果。 */

	/* 按总线顺序配置串口，任一路失败后不再继续配置后续串口。 */
	xResult = prvConfigureUart(&huart2, 
			COFFEE3_BUS2_DEFAULT_BAUD);

	if (xResult == HAL_OK) {
		xResult = prvConfigureUart(&huart3,
			COFFEE3_BUS3_DEFAULT_BAUD);
	}
	if (xResult == HAL_OK) {
		xResult = prvConfigureUart(&huart4,
			COFFEE3_BUS4_DEFAULT_BAUD);
	}
	if (xResult == HAL_OK) {
		xResult = prvConfigureUart(&huart5,
			COFFEE3_BUS5_DEFAULT_BAUD);
	}
	if (xResult == HAL_OK) {
		/* Coffee3 不使用 UART6，业务串口全部成功后将其停用。 */
		(void)HAL_UART_Abort(&huart6);
		xResult = HAL_UART_DeInit(&huart6);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  将日志 UART1 恢复为 Coffee3 日志默认参数。
  * @retval HAL_OK 日志 UART 初始化成功。
  * @retval 其他 HAL_StatusTypeDef UART 反初始化或初始化失败。
  */
HAL_StatusTypeDef xCoffee3LogSerialApplyDefault(void)
{
	return prvConfigureUart(&huart1, COFFEE3_LOG_BAUD);
}

/*-----------------------------------------------------------*/
/**
  * @brief  创建四路静态命令队列并注册 Bus2 至 Bus5 路由。
  * @retval pdPASS 四路队列均创建并完成路由注册。
  * @retval pdFAIL 任一路静态队列创建失败。
  * @note   返回成功只表示队列可入队，不表示命令已经执行。
  */
BaseType_t xCoffee3RtuBusInitialize(void)
{
	Coffee3RtuBusContext_t *pxContext; /*!< 当前正在初始化的总线上下文。 */
	uint8_t ucIndex; /*!< Bus2 至 Bus5 的紧凑数组下标。 */

	/* 清除上次初始化遗留的队列句柄、轮询状态和公开统计。 */
	memset(s_axBusContexts, 0, sizeof(s_axBusContexts));
	memset(g_axCoffee3RtuBusStatus, 0,
		sizeof(g_axCoffee3RtuBusStatus));
	/* 为每路物理总线创建静态队列，并把物理路由交给该队列。 */
	for (ucIndex = 0U; ucIndex < COFFEE3_RTU_BUS_COUNT; ucIndex++) {
		pxContext = &s_axBusContexts[ucIndex];
		pxContext->xQueue = xQueueCreateStatic(
			COFFEE3_COMMAND_QUEUE_LENGTH,
			sizeof(Coffee3Command_t),
			pxContext->aucQueueStorage,
			&pxContext->xQueueStorage);
		if (pxContext->xQueue == NULL) {
			return pdFAIL;
		}
		vCoffee3DeviceRegisterRoute(s_axBusConfigs[ucIndex].ucBusId,
			pxContext->xQueue);
	}
	return pdPASS;
}

/*-----------------------------------------------------------*/
/**
  * @brief  获取一路总线任务的固定配置。
  * @param[in] ucIndex Bus2 至 Bus5 的零起始紧凑下标。
  * @retval 非空 指向静态配置的只读指针，生命周期覆盖整个程序。
  * @retval NULL 下标超出 COFFEE3_RTU_BUS_COUNT。
  */
const Coffee3RtuBusConfig_t *pxCoffee3RtuBusGetConfig(uint8_t ucIndex)
{
	if (ucIndex >= COFFEE3_RTU_BUS_COUNT) {
		return NULL;
	}
	return &s_axBusConfigs[ucIndex];  /* 配置存储为静态数组，无需释放。 */
}

/*-----------------------------------------------------------*/
/**
  * @brief  建立指定物理总线的后台健康轮询表和首次轮询时间。
  * @param[in,out] pxContext 待初始化的总线上下文，不能为 NULL。
  * @param[in] ucBusId 物理总线编号，支持 Bus2 至 Bus5。
  * @note   首轮轮询按设备错峰，Bus5 使用外部 IO 专用错峰周期。
  */
static void prvInitializePollSchedule(Coffee3RtuBusContext_t *pxContext,
	uint8_t ucBusId)
{
	static const Coffee3DeviceId_e axBus2[] = {
		COFFEE3_DEVICE_COFFEE_MACHINE }; /*!< Bus2 轮询设备表。 */
	static const Coffee3DeviceId_e axBus3[] = {
		COFFEE3_DEVICE_CUP_MACHINE, COFFEE3_DEVICE_SYRUP_MACHINE,
		COFFEE3_DEVICE_LID_MACHINE, COFFEE3_DEVICE_POWER_METER }; /*!< Bus3 轮询设备表。 */
	static const Coffee3DeviceId_e axBus4[] = {
		COFFEE3_DEVICE_ICE_MACHINE, COFFEE3_DEVICE_SCALE }; /*!< Bus4 轮询设备表。 */
	static const Coffee3DeviceId_e axBus5[] = {
		COFFEE3_DEVICE_IO_INPUT, COFFEE3_DEVICE_IO_OUTPUT }; /*!< Bus5 外部 IO 轮询表。 */
	const Coffee3DeviceId_e *pxDevices; /*!< 指向当前物理总线的设备表。 */
	uint8_t ucCount; /*!< 当前总线的轮询设备数量。 */
	uint8_t ucIndex; /*!< 设备表遍历下标。 */
	TickType_t xNow; /*!< 建立轮询表时的系统节拍。 */
	uint32_t ulStagger; /*!< 当前设备相对首个设备的错峰时间，单位为 ms。 */

	/* 根据物理总线选择固定设备表。 */
	pxDevices = NULL;
	ucCount = 0U;
	if (ucBusId == 2U) {
		pxDevices = axBus2;
		ucCount = (uint8_t)(sizeof(axBus2) / sizeof(axBus2[0]));
	} else if (ucBusId == 3U) {
		pxDevices = axBus3;
		ucCount = (uint8_t)(sizeof(axBus3) / sizeof(axBus3[0]));
	} else if (ucBusId == 4U) {
		pxDevices = axBus4;
		ucCount = (uint8_t)(sizeof(axBus4) / sizeof(axBus4[0]));
	} else if (ucBusId == 5U) {
		pxDevices = axBus5;
		ucCount = (uint8_t)(sizeof(axBus5) / sizeof(axBus5[0]));
	}
	if ((pxContext == NULL) || (pxDevices == NULL)) {
		return;
	}
	/* 保存轮询顺序，并为首轮轮询安排递增的到期时间。 */
	pxContext->ucPollCount = ucCount;
	pxContext->ucPollCursor = 0U;
	xNow = xTaskGetTickCount();
	for (ucIndex = 0U; ucIndex < ucCount; ucIndex++) {
		ulStagger = (ucBusId == 5U) ?
			((uint32_t)ucIndex * COFFEE3_RTU_IO_POLL_STAGGER_MS) :
			((uint32_t)ucIndex * COFFEE3_RTU_POLL_STAGGER_MS);
		pxContext->aucPollIndex[ucIndex] = (uint8_t)pxDevices[ucIndex];
		pxContext->axNextPollTick[pxDevices[ucIndex]] = xNow +
			pdMS_TO_TICKS(ulStagger);
		pxContext->aucPollMisses[pxDevices[ucIndex]] = 0U;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  在前台队列空闲时执行一个已经到期的设备健康轮询。
  * @param[in,out] pxContext 当前总线上下文，保存轮询游标和设备计划。
  * @param[in] pxConfig 当前物理总线固定配置。
  * @param[in,out] pxStatus 当前总线公开状态，轮询期间更新活动设备。
  * @retval pdTRUE 已选择并完成一次后台健康轮询。
  * @retval pdFALSE 参数无效、总线未就绪、前台命令待处理或无轮询到期。
  * @note   后台轮询不增加 g_axCoffee3RtuBusStatus 中的前台命令数和错误数；设备状态计数仍可能更新。
  */
static BaseType_t prvTryBackgroundPoll(Coffee3RtuBusContext_t *pxContext,
	const Coffee3RtuBusConfig_t *pxConfig, Coffee3RtuBusStatus_t *pxStatus)
{
	Coffee3Command_t xCommand; /*!< 为健康轮询临时构造的维护来源刷新命令。 */
	Coffee3DeviceId_e xDeviceId; /*!< 本次选中的逻辑设备。 */
	ModbusPortResult_e xResult; /*!< 本次健康轮询的原始协议结果。 */
	TickType_t xNow; /*!< 搜索到期设备时的系统节拍。 */
	uint8_t ucIndex; /*!< 轮询表中的当前候选下标。 */
	uint8_t ucOffset; /*!< 从游标开始检查的相对偏移。 */
	uint32_t ulPeriodMs; /*!< 选中设备下一次轮询的周期，单位为 ms。 */

	/* 前台队列拥有优先权；队列非空时不启动后台协议事务。 */
	if ((pxContext == NULL) || (pxConfig == NULL) ||
		(pxStatus == NULL) || (pxContext->ucCreated == 0U) ||
		(pxContext->ucPollCount == 0U) ||
		(uxQueueMessagesWaiting(pxContext->xQueue) != 0U)) {
		return pdFALSE;
	}
	/* 从保存的游标循环查找第一台已到期设备，避免固定设备饥饿。 */
	xNow = xTaskGetTickCount();
	ucIndex = pxContext->ucPollCursor;
	if (ucIndex >= pxContext->ucPollCount) {
		ucIndex = 0U;
	}
	for (ucOffset = 0U; ucOffset < pxContext->ucPollCount; ucOffset++) {
		uint8_t ucCandidate; /*!< 当前偏移对应的轮询表下标。 */
		ucCandidate = (uint8_t)((ucIndex + ucOffset) %
			pxContext->ucPollCount);
		xDeviceId = (Coffee3DeviceId_e)
			pxContext->aucPollIndex[ucCandidate];
		if ((int32_t)(xNow - pxContext->axNextPollTick[xDeviceId]) >= 0) {
			ucIndex = ucCandidate;
			break;
		}
	}
	if (ucOffset >= pxContext->ucPollCount) {
		return pdFALSE;
	}
	/* 构造维护刷新命令并同步执行，队列入队过程不参与此路径。 */
	pxContext->ucPollCursor =
		(uint8_t)((ucIndex + 1U) % pxContext->ucPollCount);
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = (uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	xCommand.usAction = (uint16_t)COFFEE3_ACTION_REFRESH;
	xCommand.ulTimeoutMs = (pxConfig->ucBusId == 5U) ?
		COFFEE3_RTU_POLL_TIMEOUT_MS : COFFEE3_RTU_IO_TIMEOUT_MS;
	pxStatus->ucActiveDevice = (uint8_t)xDeviceId;
	vCoffee3DeviceCommandStarted(&xCommand);
	xResult = prvExecute(pxContext,
		pxCoffee3DeviceGetBinding(xDeviceId), &xCommand);
	/* 发布在线和就绪状态，再把本次维护命令结果提交给设备层。 */
	prvPublishPollHealth(pxContext, pxConfig, &xCommand, xResult);
	vCoffee3DeviceCommandCompleted(&xCommand, (int32_t)xResult,
		(xResult == MODBUS_PORT_RESULT_TIMEOUT) ? 1U : 0U);
	/* Bus5 外部 IO 使用独立周期，随后清除总线活动设备标记。 */
	ulPeriodMs = (pxConfig->ucBusId == 5U) ?
		COFFEE3_RTU_IO_POLL_PERIOD_MS : COFFEE3_RTU_POLL_PERIOD_MS;
	pxContext->axNextPollTick[xDeviceId] = xTaskGetTickCount() +
		pdMS_TO_TICKS(ulPeriodMs);
	pxStatus->ucActiveDevice = 0U;
	return pdTRUE;
}

/*-----------------------------------------------------------*/
/**
  * @brief  根据后台轮询结果发布设备在线、就绪和故障变化。
  * @param[in,out] pxContext 当前总线上下文，保存连续失败和故障快照。
  * @param[in] pxConfig 当前物理总线固定配置。
  * @param[in] pxCommand 已完成的后台刷新命令。
  * @param[in] xResult 后台刷新事务的 Modbus 原始结果。
  * @note   普通设备连续失败 2 次离线，Bus5 外部 IO 连续失败 3 次离线。
  */
static void prvPublishPollHealth(Coffee3RtuBusContext_t *pxContext,
	const Coffee3RtuBusConfig_t *pxConfig,
	const Coffee3Command_t *pxCommand, ModbusPortResult_e xResult)
{
	const Coffee3DeviceBinding_t *pxBinding; /*!< 被轮询设备的固定绑定信息。 */
	Coffee3DeviceId_e xDeviceId; /*!< 被轮询设备的逻辑编号。 */
	uint8_t ucLimit; /*!< 判定离线所需的连续失败次数。 */
	uint8_t ucMiss; /*!< 更新后的连续健康轮询失败次数。 */
	uint8_t ucWasOnline; /*!< 处理本次结果前的设备在线状态。 */
	uint8_t ucReady; /*!< 根据协议结果和设备状态计算的就绪标志。 */
	uint8_t ucFaultMask; /*!< 制冰机当前故障位掩码。 */

	if ((pxContext == NULL) || (pxConfig == NULL) || (pxCommand == NULL)) {
		return;
	}
	xDeviceId = (Coffee3DeviceId_e)pxCommand->ucDeviceId;
	pxBinding = pxCoffee3DeviceGetBinding(xDeviceId);
	if (pxBinding == NULL) {
		return;
	}
	/* Bus5 外部 IO 要求三次失败，其余总线使用普通两次阈值。 */
	ucWasOnline = g_axCoffee3DeviceStatus[xDeviceId].ucOnline;
	ucLimit = (pxConfig->ucBusId == 5U) ?
		COFFEE3_RTU_IO_OFFLINE_MISS_LIMIT :
		COFFEE3_RTU_OFFLINE_MISS_LIMIT;
	/* 有效 Modbus 异常仍能证明设备应答，因此在线但不能判为就绪。 */
	if ((xResult == MODBUS_PORT_RESULT_OK) ||
		(xResult == MODBUS_PORT_RESULT_EXCEPTION)) {
		pxContext->aucPollMisses[xDeviceId] = 0U;
		vCoffee3DeviceSetOnline(xDeviceId, 1U);
		ucReady = (xResult == MODBUS_PORT_RESULT_OK) ? 1U : 0U;
		/* 咖啡机只有处于协议定义的空闲状态才可以接收新业务。 */
		if ((xResult == MODBUS_PORT_RESULT_OK) &&
			(xDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE)) {
			ucReady = (g_xCoffee3CoffeeMachineImage.ausStatus[0U] ==
				g_xCoffeeMachineM50Config.usIdleValue) ? 1U : 0U;
		}
		/* 制冰机通信成功后仍需结合故障位决定就绪状态。 */
		if ((xResult == MODBUS_PORT_RESULT_OK) &&
			(xDeviceId == COFFEE3_DEVICE_ICE_MACHINE)) {
			ucFaultMask = ucIceMachineGetFaultMask(&g_xCoffee3IceImage);
			ucReady = (ucFaultMask == 0U) ? 1U : 0U;
			if ((pxContext->aucPollFaultKnown[xDeviceId] == 0U) ||
				(pxContext->aucPollFaultMask[xDeviceId] != ucFaultMask)) {
				(void)xCoffee3LogPrintfOrder(
					(ucFaultMask == 0U) ? COFFEE3_LOG_LEVEL_INFO :
					COFFEE3_LOG_LEVEL_WARNING,
					prvGetDeviceLogSource((uint8_t)xDeviceId),
					COFFEE3_LOG_ORDER_SYSTEM,
					"Fault changed: id=%u reason=%s mask=%u result=%d",
					(unsigned int)pxBinding->ucUnitId,
					pcIceMachineFaultReason(ucFaultMask),
					(unsigned int)ucFaultMask, (int)xResult);
			}
			pxContext->aucPollFaultMask[xDeviceId] = ucFaultMask;
			pxContext->aucPollFaultKnown[xDeviceId] = 1U;
		}
		vCoffee3DeviceSetReady(xDeviceId, ucReady);
		if (ucWasOnline == 0U) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
				prvGetDeviceLogSource((uint8_t)xDeviceId),
				COFFEE3_LOG_ORDER_SYSTEM,
				"Device online: id=%u reason=health poll recv result=%d",
				(unsigned int)pxBinding->ucUnitId, (int)xResult);
		}
		return;
	}
	/* 被前台事务抢占不代表设备失联，不累加掉线计数。 */
	if (xResult == MODBUS_PORT_RESULT_PREEMPTED) {
		return;
	}
	ucMiss = pxContext->aucPollMisses[xDeviceId];
	if (ucMiss < 255U) {
		ucMiss++;
	}
	pxContext->aucPollMisses[xDeviceId] = ucMiss;
	/* 只记录首次失败和达到阈值的失败，已确认离线后不重复刷日志。 */
	if ((ucMiss == 1U) || (ucMiss == ucLimit)) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
			prvGetDeviceLogSource((uint8_t)xDeviceId),
			COFFEE3_LOG_ORDER_SYSTEM,
			"Health poll failed: id=%u miss=%u/%u reason=%s result=%d",
			(unsigned int)pxBinding->ucUnitId,
			(unsigned int)ucMiss, (unsigned int)ucLimit,
			prvModbusResultName(xResult), (int)xResult);
	}
	if (ucMiss == ucLimit) {
		if (ucWasOnline != 0U) {
			vCoffee3DeviceSetOnline(xDeviceId, 0U);
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				prvGetDeviceLogSource((uint8_t)xDeviceId),
				COFFEE3_LOG_ORDER_SYSTEM,
				"Device offline: id=%u reason=two missed polls count=%u result=%d",
				(unsigned int)pxBinding->ucUnitId,
				(unsigned int)ucLimit, (int)xResult);
		} else {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				prvGetDeviceLogSource((uint8_t)xDeviceId),
				COFFEE3_LOG_ORDER_SYSTEM,
				"Device unavailable: id=%u reason=no health response count=%u result=%d",
				(unsigned int)pxBinding->ucUnitId,
				(unsigned int)ucLimit, (int)xResult);
		}
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  独占一路 UART，串行执行前台命令并穿插后台健康轮询。
  * @param[in] pvArgument 指向静态 Coffee3RtuBusConfig_t 的任务参数。
  * @note   一个任务只拥有一条物理总线和一种线协议；函数不会返回。
  * @warning 队列发送成功仅代表命令入队，执行结果由设备状态通道发布。
  */
void vCoffee3RtuBusTask(void *pvArgument)
{
	static const char * const apcTaskRunning[COFFEE3_RTU_BUS_COUNT] = {
		"TASK_RUNNING:C3Bus2", "TASK_RUNNING:C3Bus3",
		"TASK_RUNNING:C3Bus4", "TASK_RUNNING:C3Bus5"
	}; /*!< 各紧凑总线下标对应的任务启动事件。 */
	const Coffee3RtuBusConfig_t *pxConfig; /*!< 当前任务独占的物理总线配置。 */
	const Coffee3DeviceBinding_t *pxBinding; /*!< 当前前台命令目标设备的固定绑定。 */
	Coffee3RtuBusContext_t *pxContext; /*!< 当前任务独占的队列和传输资源。 */
	Coffee3RtuBusStatus_t *pxStatus; /*!< 当前任务更新的公开总线状态。 */
	TransportUartConfig_t xUartConfig; /*!< 创建 UART 传输通道的临时配置。 */
	TransportResult_e xTransportResult; /*!< 传输通道创建或打开结果。 */
	ModbusPortResult_e xResult; /*!< 初始化或当前前台命令的协议结果。 */
	Coffee3Command_t xCommand; /*!< 从本总线队列取出的前台命令副本。 */
	uint8_t ucAttempt; /*!< 当前前台命令的尝试序号，从 0 开始。 */
	uint8_t ucIndex; /*!< Bus2 至 Bus5 的紧凑上下文下标。 */
	uint8_t ucModbusPortIndex; /*!< 仅在 Modbus 总线中的紧凑端口下标。 */

	/* 校验任务参数并定位本任务唯一拥有的上下文和公开状态。 */
	pxConfig = (const Coffee3RtuBusConfig_t *)pvArgument;
	if (pxConfig == NULL) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_SYSTEM, "RTU_TASK_ARGUMENT", -1);
		vTaskDelete(NULL);
		return;
	}
	ucIndex = prvFindBusIndex(pxConfig->ucBusId);
	if (ucIndex >= COFFEE3_RTU_BUS_COUNT) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_SYSTEM, "RTU_TASK_ARGUMENT", -1);
		vTaskDelete(NULL);
		return;
	}
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		prvGetLogSource(pxConfig->ucBusId), apcTaskRunning[ucIndex], 0);
	pxContext = &s_axBusContexts[ucIndex];
	pxStatus = &g_axCoffee3RtuBusStatus[ucIndex];
	/* 创建并打开 UART 传输通道，再按总线配置初始化唯一协议。 */
	memset(&xUartConfig, 0, sizeof(xUartConfig));
	xUartConfig.pxUart = pxConfig->pxUart;
	xUartConfig.xTxEnableLevel = GPIO_PIN_SET;
	xUartConfig.ucReceiveEnabled = 1U;
	xTransportResult = xTransportUartCreate(&pxContext->xChannel,
		&pxContext->xTransport, pxConfig->pcName, &xUartConfig);
	if (xTransportResult == TRANSPORT_RESULT_OK) {
		xTransportResult = xTransportOpen(&pxContext->xChannel);
	}
	if (xTransportResult == TRANSPORT_RESULT_OK) {
		xResult = MODBUS_PORT_RESULT_OK;
		if (pxConfig->ucProtocolId ==
			COFFEE3_BUS_PROTOCOL_MODBUS_RTU) {
			ucModbusPortIndex = prvFindModbusPortIndex(ucIndex);
			if (ucModbusPortIndex >= COFFEE3_MODBUS_BUS_COUNT) {
				xResult = MODBUS_PORT_RESULT_INVALID_ARG;
			} else {
				pxContext->pxPort =
					&s_axModbusPorts[ucModbusPortIndex];
				xResult = xModbusPortClientInit(pxContext->pxPort,
					&pxContext->xChannel,
					MODBUS_PORT_TRANSPORT_RTU,
					COFFEE3_RTU_IO_TIMEOUT_MS);
			}
		}
		if (xResult == MODBUS_PORT_RESULT_OK) {
			/* 协议通道就绪后才发布总线状态并建立后台轮询表。 */
			pxContext->ucCreated = 1U;
			prvInitializePollSchedule(pxContext, pxConfig->ucBusId);
			pxStatus->ulCurrentBaudRate =
				pxConfig->ulDefaultBaudRate;
			pxStatus->ucReady = 1U;
			(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
				prvGetLogSource(pxConfig->ucBusId), "BUS_READY", 0,
				"protocol", (int32_t)pxConfig->ucProtocolId);
			prvLogDeviceBindings(pxConfig);
		}
	} else {
		xResult = MODBUS_PORT_RESULT_TRANSPORT;
	}
	if (pxContext->ucCreated == 0U) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_ERROR,
			prvGetLogSource(pxConfig->ucBusId), "BUS_INIT_FAILED",
			(int32_t)xResult, "baud",
			(int32_t)pxConfig->ulDefaultBaudRate);
	}

	/* 永久串行化前台命令；队列空闲窗口用于执行后台健康轮询。 */
	for (;;) {
		if (xQueueReceive(pxContext->xQueue, &xCommand,
			pdMS_TO_TICKS(COFFEE3_RTU_IDLE_MS)) != pdPASS) {
			(void)prvTryBackgroundPoll(pxContext, pxConfig, pxStatus);
			continue;
		}
		/* 拒绝未初始化、路由不属于本总线或协议不匹配的命令。 */
		pxBinding = pxCoffee3DeviceGetBinding(
			(Coffee3DeviceId_e)xCommand.ucDeviceId);
		if ((pxContext->ucCreated == 0U) ||
			(pxBinding == NULL) ||
			(pxBinding->ucRouteId != pxConfig->ucBusId) ||
			(pxBinding->ucProtocolId != pxConfig->ucProtocolId)) {
			xResult = MODBUS_PORT_RESULT_NOT_READY;
			vCoffee3DeviceCommandStarted(&xCommand);
			vCoffee3DeviceCommandCompleted(&xCommand,
				(int32_t)xResult, 0U);
			continue;
		}
		vCoffee3DeviceCommandStarted(&xCommand);
		pxStatus->ucActiveDevice = xCommand.ucDeviceId;
		/* 咖啡机所有非刷新命令不可因响应丢失而重复触发，强制禁止重试。 */
		if ((xCommand.ucDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) &&
			(xCommand.usAction != COFFEE3_ACTION_REFRESH)) {
			xCommand.ucRetryLimit = 0U;
		}
		xResult = MODBUS_PORT_RESULT_OK;
		/* 在设备最小事务间隔约束下执行有限次尝试，并响应取消。 */
		for (ucAttempt = 0U;
			(xResult == MODBUS_PORT_RESULT_OK) &&
			(ucAttempt <= xCommand.ucRetryLimit);
			ucAttempt++) {
			if (ucCoffee3CommandIsCanceled(&xCommand) != 0U) {
				xResult = MODBUS_PORT_RESULT_CANCELED;
				break;
			}
			if (pxBinding->usMinimumIntervalMs != 0U) {
				TickType_t xMinimumTicks; /*!< 设备要求的最小事务间隔节拍数。 */
				TickType_t xElapsedTicks; /*!< 距上次事务结束已过去的节拍数。 */

				xMinimumTicks = pdMS_TO_TICKS(
					pxBinding->usMinimumIntervalMs);
				xElapsedTicks = xTaskGetTickCount() -
					pxContext->xLastTransactionTick;
				if (xElapsedTicks < xMinimumTicks) {
					vTaskDelay(xMinimumTicks - xElapsedTicks);
				}
			}
			xResult = (ucCoffee3CommandIsCanceled(&xCommand) != 0U) ?
				MODBUS_PORT_RESULT_CANCELED :
				prvExecute(pxContext, pxBinding, &xCommand);
			pxContext->xLastTransactionTick = xTaskGetTickCount();
			if (xResult == MODBUS_PORT_RESULT_OK) {
				break;
			}
			if (xResult == MODBUS_PORT_RESULT_CANCELED) {
				break;
			}
			if (ucAttempt < xCommand.ucRetryLimit) {
				vTaskDelay(pdMS_TO_TICKS(50U));
				xResult = MODBUS_PORT_RESULT_OK;
			}
		}
		/* 发布前台统计和完成事件；取消和维护来源失败不计业务错误。 */
		pxStatus->lLastResult = (int32_t)xResult;
		pxStatus->ulCommandCount++;
		pxStatus->ucActiveDevice = 0U;
		if ((xResult != MODBUS_PORT_RESULT_OK) &&
			(xResult != MODBUS_PORT_RESULT_CANCELED) &&
			(xCommand.ucSource !=
				(uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE)) {
			pxStatus->ulErrorCount++;
			prvLogCommandFailure(pxConfig, &xCommand, xResult);
		}
		vCoffee3DeviceCommandCompleted(&xCommand, (int32_t)xResult,
			(xResult == MODBUS_PORT_RESULT_TIMEOUT) ? 1U : 0U);
		/* 当前实现对成功的前台刷新统一置就绪；咖啡机和制冰机的细分判断在状态回调及后台轮询路径。 */
		if ((xResult == MODBUS_PORT_RESULT_OK) &&
			(xCommand.usAction == (uint16_t)COFFEE3_ACTION_REFRESH)) {
			vCoffee3DeviceSetReady(
				(Coffee3DeviceId_e)xCommand.ucDeviceId, 1U);
		}
		/* 在线状态只由后台健康轮询维护，前台失败不能直接判设备离线。 */
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  将一个 CubeMX UART 句柄重新初始化为指定波特率的 8-N-1。
  * @param[in,out] pxUart 待配置的 HAL UART 句柄，不能为 NULL。
  * @param[in] ulBaudRate 目标波特率，单位为 bit/s，不能为 0。
  * @retval HAL_OK UART 反初始化和重新初始化均成功。
  * @retval HAL_ERROR 参数无效。
  * @retval 其他 HAL_StatusTypeDef UART 反初始化或初始化失败。
  */
static HAL_StatusTypeDef prvConfigureUart(UART_HandleTypeDef *pxUart,
	uint32_t ulBaudRate)
{
	HAL_StatusTypeDef xResult; /*!< UART 反初始化的 HAL 结果。 */

	if ((pxUart == NULL) || (ulBaudRate == 0U)) {
		return HAL_ERROR;
	}
	/* 先中止未完成收发并反初始化，再写入统一帧格式和目标波特率。 */
	(void)HAL_UART_Abort(pxUart);
	xResult = HAL_UART_DeInit(pxUart);
	if (xResult != HAL_OK) {
		return xResult;
	}
	pxUart->Init.BaudRate = ulBaudRate;
	pxUart->Init.WordLength = UART_WORDLENGTH_8B;
	pxUart->Init.StopBits = UART_STOPBITS_1;
	pxUart->Init.Parity = UART_PARITY_NONE;
	pxUart->Init.Mode = UART_MODE_TX_RX;
	pxUart->Init.HwFlowCtl = UART_HWCONTROL_NONE;
	pxUart->Init.OverSampling = UART_OVERSAMPLING_16;
	return HAL_UART_Init(pxUart);
}

/*-----------------------------------------------------------*/
/**
  * @brief  按设备绑定把一条标准命令分派给对应设备协议驱动。
  * @param[in,out] pxContext 当前总线上下文，提供唯一 Modbus 客户机端口。
  * @param[in] pxBinding 目标设备的固定路由、协议和从站地址绑定。
  * @param[in] pxCommand 待执行的前台或维护命令。
  * @retval MODBUS_PORT_RESULT_OK 设备事务完成且协议结果符合预期。
  * @retval MODBUS_PORT_RESULT_PROTOCOL 应答格式或内容校验失败，也可能是写后读回值不匹配。
  * @retval MODBUS_PORT_RESULT_NOT_SUPPORTED 设备不支持该动作。
  * @retval 其他 ModbusPortResult_e 传输、超时、异常、取消等原始结果。
  * @note   设备镜像按各驱动或提交函数分别更新，不构成跨设备原子快照。
  */
static ModbusPortResult_e prvExecute(Coffee3RtuBusContext_t *pxContext,
	const Coffee3DeviceBinding_t *pxBinding,
	const Coffee3Command_t *pxCommand)
{
	IoModuleModbusDigitalImage_t xIoImage; /*!< 外部 IO 读写后的临时设备镜像。 */
	CupLidShengShuImage_t xCupLidImage; /*!< 落杯或落盖设备的临时镜像。 */
	CoffeeMachineM50Image_t xM50Image; /*!< 咖啡机执行过程中的临时镜像。 */
	ModbusPortResult_e xResult; /*!< 当前设备协议调用的原始结果。 */
	CoffeeMachineM50Action_e xM50Action; /*!< 标准动作映射后的 M50 专用动作。 */
	uint8_t ucPoint; /*!< 外部 IO 输出掩码遍历的零起始点位。 */
	uint8_t ucValue; /*!< 输出掩码要求当前点位达到的 0 或 1。 */

	/* 清除可能由失败路径读取的临时镜像，再按目标设备分派。 */
	memset(&xIoImage, 0, sizeof(xIoImage));
	memset(&xCupLidImage, 0, sizeof(xCupLidImage));
	switch (pxBinding->xDeviceId) {
	case COFFEE3_DEVICE_COFFEE_MACHINE:
		/* 将公共动作映射为 M50 动作，并允许驱动回调发布中间状态。 */
		switch ((Coffee3Action_e)pxCommand->usAction) {
		case COFFEE3_ACTION_REFRESH: xM50Action = COFFEE_MACHINE_M50_ACTION_REFRESH; break;
		case COFFEE3_ACTION_COFFEE_MAKE:
			xM50Action = COFFEE_MACHINE_M50_ACTION_MAKE;
			break;
		case COFFEE3_ACTION_COFFEE_CLEAN:
			xM50Action = COFFEE_MACHINE_M50_ACTION_CLEAN;
			break;
		case COFFEE3_ACTION_CANCEL: xM50Action = COFFEE_MACHINE_M50_ACTION_CANCEL; break;
		default: return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		}
		memset(&xM50Image, 0, sizeof(xM50Image));
		xResult = xCoffeeMachineM50Execute(&g_xCoffeeMachineM50Config,
			pxContext->pxPort, pxBinding->ucUnitId, xM50Action,
			pxCommand->ausParameter[0], pxCommand->ulTimeoutMs, &xM50Image,
			prvM50StatusCallback, pxCommand,
			prvCommandCanceled, pxCommand);
		return xResult;

	case COFFEE3_DEVICE_CUP_MACHINE:
		/* 落杯刷新或执行后，结果为 OK 或 PROTOCOL 时提交驱动产生的镜像。 */
		if (pxCommand->usAction == COFFEE3_ACTION_REFRESH) {
			xResult = xCupLidShengShuRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				CUP_LID_ROLE_CUP, pxCommand->ulTimeoutMs, &xCupLidImage);
		} else if ((pxCommand->usAction == COFFEE3_ACTION_CUP_DROP_1) ||
			(pxCommand->usAction == COFFEE3_ACTION_CUP_DROP_2)) {
			xResult = xCupLidShengShuRun(pxContext->pxPort, pxBinding->ucUnitId,
				CUP_LID_ROLE_CUP,
				(pxCommand->usAction == COFFEE3_ACTION_CUP_DROP_1) ? 0U : 1U,
				pxCommand->ulTimeoutMs, &xCupLidImage, prvCommandCanceled, pxCommand);
		} else return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		if ((xResult == MODBUS_PORT_RESULT_OK) ||
			(xResult == MODBUS_PORT_RESULT_PROTOCOL)) {
			vCoffee3DeviceImageCommitCup(&xCupLidImage,
				(pxCommand->usAction == COFFEE3_ACTION_REFRESH) ? 1U : 0U,
				(pxCommand->usAction == COFFEE3_ACTION_CUP_DROP_2) ? 1U : 0U);
		}
		return xResult;

	case COFFEE3_DEVICE_SYRUP_MACHINE:
		/* 糖浆机驱动直接维护其全局镜像，并在长动作中检查取消。 */
		if (pxCommand->usAction == COFFEE3_ACTION_REFRESH)
			return xSyrupMachineRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ulTimeoutMs, &g_xCoffee3SyrupImage);
		if (pxCommand->usAction == COFFEE3_ACTION_SYRUP_DISPENSE)
			return xSyrupMachineDispense(pxContext->pxPort, pxBinding->ucUnitId,
				(uint8_t)pxCommand->ausParameter[0], pxCommand->ausParameter[1],
				pxCommand->ulTimeoutMs, &g_xCoffee3SyrupImage,
				prvCommandCanceled, pxCommand);
		if (pxCommand->usAction == COFFEE3_ACTION_SYRUP_CLEAN)
			return xSyrupMachineClean(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ulTimeoutMs, &g_xCoffee3SyrupImage,
				prvCommandCanceled, pxCommand);
		if (pxCommand->usAction == COFFEE3_ACTION_SYRUP_SET_REMAINING)
			return xSyrupMachineSetRemaining(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ausParameter[0], pxCommand->ulTimeoutMs);
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;

	case COFFEE3_DEVICE_LID_MACHINE:
		/* 落盖与落杯共用驱动，但使用独立角色和设备镜像。 */
		if (pxCommand->usAction == COFFEE3_ACTION_REFRESH) {
			xResult = xCupLidShengShuRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				CUP_LID_ROLE_LID, pxCommand->ulTimeoutMs, &xCupLidImage);
		} else if ((pxCommand->usAction == COFFEE3_ACTION_LID_DROP_1) ||
			(pxCommand->usAction == COFFEE3_ACTION_LID_DROP_2)) {
			xResult = xCupLidShengShuRun(pxContext->pxPort, pxBinding->ucUnitId,
				CUP_LID_ROLE_LID,
				(pxCommand->usAction == COFFEE3_ACTION_LID_DROP_1) ? 0U : 1U,
				pxCommand->ulTimeoutMs, &xCupLidImage, prvCommandCanceled, pxCommand);
		} else return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		if ((xResult == MODBUS_PORT_RESULT_OK) ||
			(xResult == MODBUS_PORT_RESULT_PROTOCOL)) {
			vCoffee3DeviceImageCommitLid(&xCupLidImage,
				(pxCommand->usAction == COFFEE3_ACTION_REFRESH) ? 1U : 0U,
				(pxCommand->usAction == COFFEE3_ACTION_LID_DROP_2) ? 1U : 0U);
		}
		return xResult;

	case COFFEE3_DEVICE_ICE_MACHINE:
		if (pxCommand->usAction == COFFEE3_ACTION_REFRESH) {
			/* 制冰机 FC03 应答回显起始寄存器 00 01，而不是字节数。
			 * 驱动会校验该回显及原始 PDU CRC；机器故障不等同于通信失败。 */
			return xIceMachineRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ulTimeoutMs, &g_xCoffee3IceImage);
		}
		if (pxCommand->usAction == COFFEE3_ACTION_ICE_SET_VALVE)
			return xIceMachineSetValve(pxContext->pxPort, pxBinding->ucUnitId,
				(uint8_t)pxCommand->ausParameter[0], pxCommand->ulTimeoutMs);
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;

	case COFFEE3_DEVICE_SCALE:
		/* 称重模块只接受刷新、去皮、清除皮重和置零动作。 */
		switch ((Coffee3Action_e)pxCommand->usAction) {
		case COFFEE3_ACTION_REFRESH: return xScaleBsqDgV2RefreshGram(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs, &g_xCoffee3ScaleImage);
		case COFFEE3_ACTION_SCALE_TARE: return xScaleBsqDgV2Tare(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		case COFFEE3_ACTION_SCALE_CLEAR_TARE: return xScaleBsqDgV2ClearTare(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		case COFFEE3_ACTION_SCALE_ZERO: return xScaleBsqDgV2Zero(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		default: return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		}

	case COFFEE3_DEVICE_POWER_METER:
		/* 电表仅支持刷新测量镜像。 */
		if (pxCommand->usAction != COFFEE3_ACTION_REFRESH)
			return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		return xPowerMeterDdsu666Refresh(pxContext->pxPort, pxBinding->ucUnitId,
			pxCommand->ulTimeoutMs, &g_xCoffee3PowerMeterImage);

	case COFFEE3_DEVICE_IO_INPUT:
		/* 读取全部外部输入点，成功后再发布输入镜像。 */
		if (pxCommand->usAction != COFFEE3_ACTION_REFRESH)
			return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		xResult = xIoModuleModbusReadInputs(pxContext->pxPort, pxBinding->ucUnitId,
			0U, COFFEE3_EXTERNAL_IO_POINT_COUNT, pxCommand->ulTimeoutMs, &xIoImage);
		if (xResult == MODBUS_PORT_RESULT_OK) {
			vCoffee3IoCommitModbusInput(xIoImage.aucPoints);
		}
		return xResult;

	case COFFEE3_DEVICE_IO_OUTPUT:
		if (pxCommand->usAction == COFFEE3_ACTION_IO_WRITE_MASK) {
			/* 先读回全部输出，再逐点修正与目标掩码不同的点位。 */
			xResult = xIoModuleModbusReadOutputs(pxContext->pxPort,
				pxBinding->ucUnitId, 0U, COFFEE3_EXTERNAL_IO_POINT_COUNT,
				pxCommand->ulTimeoutMs, &xIoImage);
			for (ucPoint = 0U; (xResult == MODBUS_PORT_RESULT_OK) &&
				(ucPoint < COFFEE3_EXTERNAL_IO_POINT_COUNT); ucPoint++) {
				ucValue = (uint8_t)((pxCommand->ausParameter[0] >> ucPoint) & 1U);
				if (xIoImage.aucPoints[ucPoint] != ucValue) {
					xResult = xIoModuleModbusWriteOutput(pxContext->pxPort,
						pxBinding->ucUnitId, 0U, ucPoint, ucValue,
						COFFEE3_EXTERNAL_IO_POINT_COUNT,
						pxCommand->ulTimeoutMs, &xIoImage);
				}
			}
			if ((xIoImage.ucPointCount == COFFEE3_EXTERNAL_IO_POINT_COUNT) &&
				((xResult == MODBUS_PORT_RESULT_OK) ||
				 (xResult == MODBUS_PORT_RESULT_PROTOCOL))) {
				vCoffee3IoCommitModbusOutputImage(xIoImage.aucPoints);
			}
			(void)xCoffee3LogPrintfOrder((xResult == MODBUS_PORT_RESULT_OK) ?
				COFFEE3_LOG_LEVEL_INFO : COFFEE3_LOG_LEVEL_ERROR,
				COFFEE3_LOG_SOURCE_IO_OUTPUT, (uint16_t)pxCommand->ulOrderId,
				"Output mask 0x%04X: result=%ld (readback verified on success)",
				(unsigned int)pxCommand->ausParameter[0], (long)xResult);
			return xResult;
		}
		/* 刷新路径只读取并提交当前输出镜像。 */
		if (pxCommand->usAction == COFFEE3_ACTION_REFRESH) {
			xResult = xIoModuleModbusReadOutputs(pxContext->pxPort,
				pxBinding->ucUnitId, 0U, COFFEE3_EXTERNAL_IO_POINT_COUNT,
				pxCommand->ulTimeoutMs, &xIoImage);
			if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee3IoCommitModbusOutputImage(xIoImage.aucPoints);
			}
			return xResult;
		}
		/* 单点写入先记录期望值，驱动读回后再记录观察值和结果。 */
		prvLogIoWriteExpected(pxCommand);
		if (pxCommand->usAction != COFFEE3_ACTION_IO_WRITE)
			return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		xResult = xIoModuleModbusWriteOutput(pxContext->pxPort, pxBinding->ucUnitId,
			0U, (uint8_t)pxCommand->ausParameter[0],
			(pxCommand->ausParameter[1] != 0U) ? 1U : 0U,
			COFFEE3_EXTERNAL_IO_POINT_COUNT, pxCommand->ulTimeoutMs, &xIoImage);
		prvLogIoWrite(pxCommand, xResult, &xIoImage);
		if ((xIoImage.ucPointCount == COFFEE3_EXTERNAL_IO_POINT_COUNT) &&
			((xResult == MODBUS_PORT_RESULT_OK) ||
			 (xResult == MODBUS_PORT_RESULT_PROTOCOL))) {
			vCoffee3IoCommitModbusOutputImage(xIoImage.aucPoints);
		}
		return xResult;

	default:
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  查询设备驱动是否应取消当前长事务。
  * @param[in] pvContext 指向当前 Coffee3Command_t 的只读上下文。
  * @retval 1 命令已被请求取消。
  * @retval 0 命令可继续执行，或上下文未指示取消。
  */
static uint8_t prvCommandCanceled(const void *pvContext)
{
	return ucCoffee3CommandIsCanceled(
		(const Coffee3Command_t *)pvContext);
}

/*-----------------------------------------------------------*/
/**
  * @brief  将物理总线编号映射为总线日志来源。
  * @param[in] ucBusId 物理总线编号 Bus2 至 Bus5。
  * @retval Coffee3LogSource_e Bus2、Bus3、Bus4 对应来源；其余返回 Bus5。
  */
static Coffee3LogSource_e prvGetLogSource(uint8_t ucBusId)
{
	switch (ucBusId) {
	case 2U:
		return COFFEE3_LOG_SOURCE_BUS2;
	case 3U:
		return COFFEE3_LOG_SOURCE_BUS3;
	case 4U:
		return COFFEE3_LOG_SOURCE_BUS4;
	default:
		return COFFEE3_LOG_SOURCE_BUS5;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  将逻辑设备编号映射为设备诊断日志来源。
  * @param[in] ucDeviceId Coffee3DeviceId_e 逻辑设备编号。
  * @retval Coffee3LogSource_e 对应设备来源；无效编号返回系统来源。
  */
static Coffee3LogSource_e prvGetDeviceLogSource(uint8_t ucDeviceId)
{
	switch ((Coffee3DeviceId_e)ucDeviceId) {
	case COFFEE3_DEVICE_COFFEE_MACHINE:
		return COFFEE3_LOG_SOURCE_COFFEE;
	case COFFEE3_DEVICE_CUP_MACHINE:
		return COFFEE3_LOG_SOURCE_CUP;
	case COFFEE3_DEVICE_SYRUP_MACHINE:
		return COFFEE3_LOG_SOURCE_SYRUP;
	case COFFEE3_DEVICE_LID_MACHINE:
		return COFFEE3_LOG_SOURCE_LID;
	case COFFEE3_DEVICE_ICE_MACHINE:
		return COFFEE3_LOG_SOURCE_ICE;
	case COFFEE3_DEVICE_SCALE:
		return COFFEE3_LOG_SOURCE_WEIGH;
	case COFFEE3_DEVICE_POWER_METER:
		return COFFEE3_LOG_SOURCE_ENERGY_METER;
	case COFFEE3_DEVICE_IO_INPUT:
		return COFFEE3_LOG_SOURCE_IO_INPUT;
	case COFFEE3_DEVICE_IO_OUTPUT:
		return COFFEE3_LOG_SOURCE_IO_OUTPUT;
	default:
		return COFFEE3_LOG_SOURCE_SYSTEM;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  获取逻辑设备协议绑定的日志事件名称。
  * @param[in] ucDeviceId Coffee3DeviceId_e 逻辑设备编号。
  * @retval 非空 静态事件字符串；未知设备返回 UNKNOWN 事件。
  */
static const char *prvGetDeviceProtocolEvent(uint8_t ucDeviceId)
{
	switch ((Coffee3DeviceId_e)ucDeviceId) {
	case COFFEE3_DEVICE_COFFEE_MACHINE:
		return "DEVICE_PROTOCOL:M50_MODBUS";
	case COFFEE3_DEVICE_CUP_MACHINE:
		return "DEVICE_PROTOCOL:SHENGSHU_CUP";
	case COFFEE3_DEVICE_SYRUP_MACHINE:
		return "DEVICE_PROTOCOL:SYRUP_MODBUS";
	case COFFEE3_DEVICE_LID_MACHINE:
		return "DEVICE_PROTOCOL:SHENGSHU_LID";
	case COFFEE3_DEVICE_ICE_MACHINE:
		return "DEVICE_PROTOCOL:ICE_MODBUS";
	case COFFEE3_DEVICE_SCALE:
		return "DEVICE_PROTOCOL:BSQ_DG_V2";
	case COFFEE3_DEVICE_POWER_METER:
		return "DEVICE_PROTOCOL:DDSU666";
	case COFFEE3_DEVICE_IO_INPUT:
	case COFFEE3_DEVICE_IO_OUTPUT:
		return "DEVICE_PROTOCOL:DIGITAL_IO";
	default:
		return "DEVICE_PROTOCOL:UNKNOWN";
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  获取物理总线及默认波特率的日志事件名称。
  * @param[in] ucBusId 物理总线编号 Bus2 至 Bus5。
  * @retval 非空 静态事件字符串；未知总线返回 UNKNOWN 事件。
  */
static const char *prvGetBusLinkEvent(uint8_t ucBusId)
{
	switch (ucBusId) {
	case 2U:
		return "DEVICE_LINK:BUS2_19200";
	case 3U:
		return "DEVICE_LINK:BUS3_9600";
	case 4U:
		return "DEVICE_LINK:BUS4_19200";
	case 5U:
		return "DEVICE_LINK:BUS5_38400";
	default:
		return "DEVICE_LINK:UNKNOWN";
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  输出当前物理总线上每个设备的驱动、从站和链路绑定。
  * @param[in] pxConfig 当前物理总线配置；为 NULL 时不输出。
  */
static void prvLogDeviceBindings(const Coffee3RtuBusConfig_t *pxConfig)
{
	const Coffee3DeviceBinding_t *pxBinding; /*!< 当前逻辑设备的固定绑定。 */
	Coffee3LogSource_e xSource; /*!< 当前设备专用的日志来源。 */
	uint8_t ucDeviceId; /*!< 遍历 Coffee3 逻辑设备的编号。 */

	if (pxConfig == NULL) {
		return;
	}
	for (ucDeviceId = (uint8_t)COFFEE3_DEVICE_COFFEE_MACHINE;
		ucDeviceId < (uint8_t)COFFEE3_DEVICE_COUNT; ucDeviceId++) {
		pxBinding = pxCoffee3DeviceGetBinding(
			(Coffee3DeviceId_e)ucDeviceId);
		if ((pxBinding == NULL) ||
			(pxBinding->ucRouteId != pxConfig->ucBusId)) {
			continue;
		}
		xSource = prvGetDeviceLogSource(ucDeviceId);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO, xSource,
			prvGetDeviceProtocolEvent(ucDeviceId), 0, "driver",
			(int32_t)pxBinding->ucDriverId);
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO, xSource,
			prvGetBusLinkEvent(pxConfig->ucBusId), 0, "unit",
			(int32_t)pxBinding->ucUnitId);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  查找物理总线在 Bus2 至 Bus5 紧凑数组中的下标。
  * @param[in] ucBusId 待查找的物理总线编号。
  * @retval 0 至 COFFEE3_RTU_BUS_COUNT-1 找到的紧凑数组下标。
  * @retval COFFEE3_RTU_BUS_COUNT 未找到该物理总线。
  */
static uint8_t prvFindBusIndex(uint8_t ucBusId)
{
	uint8_t ucIndex; /*!< 遍历静态总线配置的紧凑下标。 */

	for (ucIndex = 0U; ucIndex < COFFEE3_RTU_BUS_COUNT; ucIndex++) {
		if (s_axBusConfigs[ucIndex].ucBusId == ucBusId) {
			return ucIndex;
		}
	}
	return COFFEE3_RTU_BUS_COUNT;
}

/*-----------------------------------------------------------*/
/**
  * @brief  计算一条 RTU 总线在 Modbus 客户机端口数组中的紧凑下标。
  * @param[in] ucBusIndex Bus2 至 Bus5 配置数组的零起始下标。
  * @retval 0 至 COFFEE3_MODBUS_BUS_COUNT-1 对应 Modbus 端口下标。
  * @retval COFFEE3_MODBUS_BUS_COUNT 下标无效或该总线不是 Modbus RTU。
  */
static uint8_t prvFindModbusPortIndex(uint8_t ucBusIndex)
{
	uint8_t ucIndex; /*!< 扫描目标总线之前配置项的下标。 */
	uint8_t ucPortIndex; /*!< 已遇到的 Modbus RTU 总线数量。 */

	if ((ucBusIndex >= COFFEE3_RTU_BUS_COUNT) ||
		(s_axBusConfigs[ucBusIndex].ucProtocolId !=
		COFFEE3_BUS_PROTOCOL_MODBUS_RTU)) {
		return COFFEE3_MODBUS_BUS_COUNT;
	}
	ucPortIndex = 0U;
	for (ucIndex = 0U; ucIndex < ucBusIndex; ucIndex++) {
		if (s_axBusConfigs[ucIndex].ucProtocolId ==
			COFFEE3_BUS_PROTOCOL_MODBUS_RTU) {
			ucPortIndex++;
		}
	}
	return ucPortIndex;
}

/*-----------------------------------------------------------*/
/**
  * @brief  将 Modbus 失败结果转换为日志可读的原因文本。
  * @param[in] xResult Modbus 端口原始结果。
  * @retval 非空 指向静态英文原因字符串，未知结果返回 other。
  */
static const char *prvModbusResultName(ModbusPortResult_e xResult)
{
	switch (xResult) {
	case MODBUS_PORT_RESULT_NOT_READY: return "not ready";
	case MODBUS_PORT_RESULT_BUSY: return "busy";
	case MODBUS_PORT_RESULT_TIMEOUT: return "timeout";
	case MODBUS_PORT_RESULT_TRANSPORT: return "transport";
	case MODBUS_PORT_RESULT_PROTOCOL: return "protocol";
	case MODBUS_PORT_RESULT_EXCEPTION: return "exception";
	case MODBUS_PORT_RESULT_CANCELED: return "canceled";
	case MODBUS_PORT_RESULT_PREEMPTED: return "preempted";
	default: return "other";
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  接收 M50 执行期间的状态轮询结果并发布镜像和就绪状态。
  * @param[in] pxImage 本次 M50 状态镜像，不能为 NULL。
  * @param[in] xResult 本次状态轮询的 Modbus 原始结果。
  * @param[in] ucConsecutiveMisses 当前连续状态轮询失败次数。
  * @param[in] pvContext 指向正在执行的 Coffee3Command_t。
  */
static void prvM50StatusCallback(const CoffeeMachineM50Image_t *pxImage,
	ModbusPortResult_e xResult, uint8_t ucConsecutiveMisses,
	const void *pvContext)
{
	const Coffee3Command_t *pxCommand; /*!< 触发本次 M50 执行的命令。 */
	const Coffee3DeviceBinding_t *pxBinding; /*!< 咖啡机固定设备绑定。 */
	uint16_t usState; /*!< M50 状态镜像中的主状态寄存器值。 */

	pxCommand = (const Coffee3Command_t *)pvContext;
	pxBinding = pxCoffee3DeviceGetBinding(COFFEE3_DEVICE_COFFEE_MACHINE);
	if ((pxImage == NULL) || (pxCommand == NULL) || (pxBinding == NULL)) {
		return;
	}
	/* 状态轮询失败立即撤销就绪，只在首次和阈值失败时记录日志。 */
	if (xResult != MODBUS_PORT_RESULT_OK) {
		vCoffee3DeviceSetReady(COFFEE3_DEVICE_COFFEE_MACHINE, 0U);
		if ((ucConsecutiveMisses == 1U) ||
			(ucConsecutiveMisses == COFFEE_MACHINE_M50_POLL_MISS_LIMIT)) {
			(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
				prvGetDeviceLogSource((uint8_t)COFFEE3_DEVICE_COFFEE_MACHINE),
				(uint16_t)pxCommand->ulOrderId,
				"Coffee status missed: device=%s miss=%u/%u reason=%s result=%d",
				pxBinding->pcName, (unsigned int)ucConsecutiveMisses,
				(unsigned int)COFFEE_MACHINE_M50_POLL_MISS_LIMIT,
				prvModbusResultName(xResult), (int)xResult);
		}
		return;
	}
	/* 成功时提交独立设备镜像，并依据空闲状态更新就绪标志。 */
	usState = pxImage->ausStatus[0U];
	vCoffee3DeviceImageCommitM50(pxImage);
	vCoffee3DeviceSetReady(COFFEE3_DEVICE_COFFEE_MACHINE,
		(usState == g_xCoffeeMachineM50Config.usIdleValue) ? 1U : 0U);
	/* 仅在首次获得状态或状态值变化时输出状态变更日志。 */
	if ((s_ucM50StateKnown == 0U) || (s_usM50LastState != usState)) {
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			prvGetDeviceLogSource((uint8_t)COFFEE3_DEVICE_COFFEE_MACHINE),
			(uint16_t)pxCommand->ulOrderId,
			"Coffee state changed: device=%s state=%s status=%u action=%s",
			pxBinding->pcName,
			(usState == g_xCoffeeMachineM50Config.usIdleValue) ?
				"idle" : "working",
			(unsigned int)usState, prvActionName(pxCommand->usAction));
	}
	s_usM50LastState = usState;
	s_ucM50StateKnown = 1U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  将标准设备动作转换为动作失败日志中的稳定名称。
  * @param[in] usAction Coffee3Action_e 动作编号。
  * @retval 非空 指向静态动作名称；未列出的动作返回 DEVICE_ACTION。
  */
static const char *prvActionName(uint16_t usAction)
{
	switch ((Coffee3Action_e)usAction) {
	case COFFEE3_ACTION_REFRESH: return "STATUS_REFRESH";
	case COFFEE3_ACTION_COFFEE_MAKE: return "COFFEE_MAKE";
	case COFFEE3_ACTION_COFFEE_CLEAN: return "COFFEE_CLEAN";
	case COFFEE3_ACTION_CUP_DROP_1: return "CUP_DROP_1";
	case COFFEE3_ACTION_CUP_DROP_2: return "CUP_DROP_2";
	case COFFEE3_ACTION_LID_DROP_1: return "LID_DROP_1";
	case COFFEE3_ACTION_LID_DROP_2: return "LID_DROP_2";
	case COFFEE3_ACTION_SYRUP_DISPENSE: return "SYRUP_DISPENSE";
	case COFFEE3_ACTION_SYRUP_CLEAN: return "SYRUP_CLEAN";
	case COFFEE3_ACTION_SYRUP_SET_REMAINING: return "SYRUP_SET_REMAINING";
	case COFFEE3_ACTION_ICE_SET_VALVE: return "ICE_VALVE";
	case COFFEE3_ACTION_SCALE_TARE: return "SCALE_TARE";
	case COFFEE3_ACTION_SCALE_CLEAR_TARE: return "SCALE_CLEAR_TARE";
	case COFFEE3_ACTION_SCALE_ZERO: return "SCALE_ZERO";
	case COFFEE3_ACTION_IO_WRITE: return "IO_WRITE";
	case COFFEE3_ACTION_IO_WRITE_MASK: return "IO_WRITE_MASK";
	default: return "DEVICE_ACTION";
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  输出一条前台设备动作失败日志。
  * @param[in] pxConfig 当前总线配置；仅用于参数完整性校验。
  * @param[in] pxCommand 失败命令，包含设备、动作、订单和步骤编号。
  * @param[in] xResult 设备协议事务的原始失败结果。
  */
static void prvLogCommandFailure(const Coffee3RtuBusConfig_t *pxConfig,
	const Coffee3Command_t *pxCommand, ModbusPortResult_e xResult)
{
	const Coffee3DeviceBinding_t *pxBinding; /*!< 失败命令目标设备的固定绑定。 */

	if ((pxConfig == NULL) || (pxCommand == NULL) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE3_DEVICE_COUNT)) {
		return;
	}
	pxBinding = pxCoffee3DeviceGetBinding(
		(Coffee3DeviceId_e)pxCommand->ucDeviceId);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
		prvGetDeviceLogSource(pxCommand->ucDeviceId),
		(uint16_t)pxCommand->ulOrderId,
		"Action failed: device=%s action=%s reason=%s result=%d step=%u",
		(pxBinding != NULL) ? pxBinding->pcName : "Unknown",
		prvActionName(pxCommand->usAction), prvModbusResultName(xResult),
		(int)xResult,
		(unsigned int)pxCommand->usStepId);
}

/*-----------------------------------------------------------*/
/**
  * @brief  为服务器来源的单点 IO 写入记录期望值。
  * @param[in] pxCommand 待执行的命令；非服务器单点写入时不记录。
  */
static void prvLogIoWriteExpected(const Coffee3Command_t *pxCommand)
{
	uint8_t ucValue; /*!< 命令要求写入的归一化输出值 0 或 1。 */

	if ((pxCommand == NULL) ||
		(pxCommand->ucSource !=
			(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) ||
		(pxCommand->usAction != (uint16_t)COFFEE3_ACTION_IO_WRITE)) {
		return;
	}
	ucValue = (pxCommand->ausParameter[1] != 0U) ? 1U : 0U;
	(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
		"IO_WRITE_EXPECTED", 0, "value", (int32_t)ucValue);
}

/*-----------------------------------------------------------*/
/**
  * @brief  为服务器来源的单点 IO 写入记录读回值和最终结果。
  * @param[in] pxCommand 已执行的单点 IO 写入命令。
  * @param[in] xResult 写入并读回后的 Modbus 原始结果。
  * @param[in] pxImage 写入后读回的输出镜像；有效结果时不能为 NULL。
  */
static void prvLogIoWrite(const Coffee3Command_t *pxCommand,
	ModbusPortResult_e xResult, const IoModuleModbusDigitalImage_t *pxImage)
{
	uint8_t ucPoint; /*!< 命令指定的零起始外部输出点位。 */

	if ((pxCommand == NULL) ||
		(pxCommand->ucSource !=
			(uint8_t)COFFEE3_COMMAND_SOURCE_SERVER) ||
		(pxCommand->usAction != (uint16_t)COFFEE3_ACTION_IO_WRITE)) {
		return;
	}
	ucPoint = (uint8_t)pxCommand->ausParameter[0];
	if ((xResult == MODBUS_PORT_RESULT_OK) ||
		(xResult == MODBUS_PORT_RESULT_PROTOCOL)) {
		if ((pxImage == NULL) || (ucPoint >= pxImage->ucPointCount)) {
			return;
		}
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
			"IO_WRITE_OBSERVED", 0, "value",
			(int32_t)pxImage->aucPoints[ucPoint]);
		if (xResult == MODBUS_PORT_RESULT_PROTOCOL) {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"IO_WRITE_MISMATCH", (int32_t)xResult,
				"point", (int32_t)(ucPoint + 1U));
		} else {
			(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_INFO,
				COFFEE3_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"IO_WRITE_SUCCESS", 0,
				"point", (int32_t)(ucPoint + 1U));
		}
	} else {
		(void)xCoffee3LogWriteFieldOrder(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
			"IO_WRITE_READ_FAILED", (int32_t)xResult,
			"point", (int32_t)(ucPoint + 1U));
	}
}
