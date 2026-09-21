/**
  * @file      coffee3_rtu_bus.c
  * @brief     Implement four serialized single-protocol UART buses.
  * @author    WHong
  * @date      2026-07-30
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

/** @brief Store one bus's static queue and Transport resources. */
typedef struct {
	StaticQueue_t xQueueStorage;
	uint8_t aucQueueStorage[
		COFFEE3_COMMAND_QUEUE_LENGTH * sizeof(Coffee3Command_t)];
	QueueHandle_t xQueue;
	TransportChannel_t xChannel;
	TransportUartContext_t xTransport;
	ModbusPort_t *pxPort;
	TickType_t xLastTransactionTick;
	TickType_t axNextPollTick[COFFEE3_DEVICE_COUNT];
	uint8_t aucPollMisses[COFFEE3_DEVICE_COUNT];
	uint8_t aucPollFaultMask[COFFEE3_DEVICE_COUNT];
	uint8_t aucPollFaultKnown[COFFEE3_DEVICE_COUNT];
	uint8_t aucPollIndex[COFFEE3_DEVICE_COUNT];
	uint8_t ucPollCount;
	uint8_t ucPollCursor;
	uint8_t ucPollingActive;
	volatile uint8_t ucPreemptRequested;
	uint8_t ucCreated;
} Coffee3RtuBusContext_t;

Coffee3RtuBusStatus_t
	g_axCoffee3RtuBusStatus[COFFEE3_RTU_BUS_COUNT];

/** @brief Coffee3 ModbusRtuBus参数设置: STM32 UART 映射. */
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

/** @brief Private bus runtime resources in the compact Bus2-to-Bus5 order. */
static Coffee3RtuBusContext_t
	s_axBusContexts[COFFEE3_RTU_BUS_COUNT];

/** @brief Allocate one Modbus context only for each selected RTU bus. */
static ModbusPort_t s_axModbusPorts[COFFEE3_MODBUS_BUS_COUNT];

/**
  * @brief  将一个 CubeMX UART 句柄重新初始化为 8-N-1。
  * @param[in,out] pxUart 待重新初始化的 UART 句柄，不能为 NULL。
  * @param[in]     ulBaudRate 目标波特率，单位为 bit/s。
  * @retval HAL_OK UART 反初始化、参数配置和初始化均成功。
  * @retval 其他 HAL_StatusTypeDef HAL 操作失败。
  */
static HAL_StatusTypeDef prvConfigureUart(UART_HandleTypeDef *pxUart,
	uint32_t ulBaudRate);
/**
  * @brief  通过公共 DeviceLibrary 执行一个 Coffee3 命令。
  * @param[in,out] pxContext 当前 RTU 总线运行上下文。
  * @param[in]     pxBinding 命令目标设备的固定总线绑定。
  * @param[in]     pxCommand 待执行的标准命令，包含动作和参数。
  * @retval MODBUS_PORT_RESULT_OK 设备协议事务完成。
  * @retval 其他 ModbusPortResult_e 设备协议、传输或超时错误。
  */
static ModbusPortResult_e prvExecute(Coffee3RtuBusContext_t *pxContext,
	const Coffee3DeviceBinding_t *pxBinding,
	const Coffee3Command_t *pxCommand);
/**
  * @brief  将物理总线 ID 映射为日志来源。
  * @param[in] ucBusId 物理 Bus2 至 Bus5 的编号。
  * @retval 对应总线日志来源；未知编号返回系统来源。
  */
static Coffee3LogSource_e prvGetLogSource(uint8_t ucBusId);
/**
  * @brief  Map one logical RTU device to its diagnostic log source.
  * @param[in] ucDeviceId Logical Coffee3 device identifier.
  * @return Device-specific source, or system source for an invalid device.
  */
static Coffee3LogSource_e prvGetDeviceLogSource(uint8_t ucDeviceId);
static const char *prvGetDeviceProtocolEvent(uint8_t ucDeviceId);
static const char *prvGetBusLinkEvent(uint8_t ucBusId);
static void prvLogDeviceBindings(const Coffee3RtuBusConfig_t *pxConfig);
/**
  * @brief  查找物理总线 ID 在紧凑上下文数组中的索引。
  * @param[in] ucBusId 需要查找的物理总线编号。
  * @retval 紧凑数组索引；找不到时返回 COFFEE3_RTU_BUS_COUNT。
  */
static uint8_t prvFindBusIndex(uint8_t ucBusId);
/**
  * @brief  Find a bus's compact Modbus context index.
  * @param[in] ucBusIndex Zero-based physical bus configuration index.
  * @retval Compact Modbus context index.
  * @retval COFFEE3_MODBUS_BUS_COUNT when the bus is not Modbus RTU.
  */
static uint8_t prvFindModbusPortIndex(uint8_t ucBusIndex);
/**
  * @brief  仅在事务失败状态变化时输出一次诊断日志。
  * @param[in] pxConfig 当前总线配置，提供总线名称和编号。
  * @param[in] pxCommand 当前失败的命令，提供设备和命令号。
  * @param[in] xResult 设备协议事务的失败结果。
  */
static void prvLogCommandFailure(const Coffee3RtuBusConfig_t *pxConfig,
	const Coffee3Command_t *pxCommand, ModbusPortResult_e xResult);
static void prvLogIoWriteExpected(const Coffee3Command_t *pxCommand);
static void prvLogIoWrite(const Coffee3Command_t *pxCommand,
	ModbusPortResult_e xResult, const IoModuleModbusDigitalImage_t *pxImage);
static uint8_t prvCommandCanceled(const void *pvContext);
static const char *prvModbusResultName(ModbusPortResult_e xResult);
static uint8_t prvPollPreempted(void *pvContext);
static void prvInitializePollSchedule(Coffee3RtuBusContext_t *pxContext,
	uint8_t ucBusId);
static BaseType_t prvTryBackgroundPoll(Coffee3RtuBusContext_t *pxContext,
	const Coffee3RtuBusConfig_t *pxConfig, Coffee3RtuBusStatus_t *pxStatus);
static void prvPublishPollHealth(Coffee3RtuBusContext_t *pxContext,
	const Coffee3RtuBusConfig_t *pxConfig,
	const Coffee3Command_t *pxCommand, ModbusPortResult_e xResult);
static const char *prvActionName(uint16_t usAction);

/*-----------------------------------------------------------*/
HAL_StatusTypeDef xCoffee3SerialApplyDefaults(void)
{
	HAL_StatusTypeDef xResult;
	// 重新初始化所有总线 UART 为 8-N-1，波特率为默认值。
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
		(void)HAL_UART_Abort(&huart6);
		xResult = HAL_UART_DeInit(&huart6);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
HAL_StatusTypeDef xCoffee3LogSerialApplyDefault(void)
{
	return prvConfigureUart(&huart1, COFFEE3_LOG_BAUD);
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee3RtuBusInitialize(void)
{
	Coffee3RtuBusContext_t *pxContext;
	uint8_t ucIndex;

	memset(s_axBusContexts, 0, sizeof(s_axBusContexts));
	memset(g_axCoffee3RtuBusStatus, 0,
		sizeof(g_axCoffee3RtuBusStatus));
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
const Coffee3RtuBusConfig_t *pxCoffee3RtuBusGetConfig(uint8_t ucIndex)
{
	if (ucIndex >= COFFEE3_RTU_BUS_COUNT) {
		return NULL;
	}
	return &s_axBusConfigs[ucIndex];  /* 返回静态配置数组中的指针。 */
}

/*-----------------------------------------------------------*/
void vCoffee3RtuBusRequestPreempt(uint8_t ucBusId)
{
	uint8_t ucIndex;

	ucIndex = prvFindBusIndex(ucBusId);
	if (ucIndex < COFFEE3_RTU_BUS_COUNT) {
		s_axBusContexts[ucIndex].ucPreemptRequested = 1U;
	}
}

/*-----------------------------------------------------------*/
static void prvInitializePollSchedule(Coffee3RtuBusContext_t *pxContext,
	uint8_t ucBusId)
{
	static const Coffee3DeviceId_e axBus2[] = {
		COFFEE3_DEVICE_COFFEE_MACHINE };
	static const Coffee3DeviceId_e axBus3[] = {
		COFFEE3_DEVICE_CUP_MACHINE, COFFEE3_DEVICE_SYRUP_MACHINE,
		COFFEE3_DEVICE_LID_MACHINE, COFFEE3_DEVICE_POWER_METER };
	static const Coffee3DeviceId_e axBus4[] = {
		COFFEE3_DEVICE_ICE_MACHINE, COFFEE3_DEVICE_SCALE };
	static const Coffee3DeviceId_e axBus5[] = {
		COFFEE3_DEVICE_IO_INPUT, COFFEE3_DEVICE_IO_OUTPUT };
	const Coffee3DeviceId_e *pxDevices;
	uint8_t ucCount;
	uint8_t ucIndex;
	TickType_t xNow;
	uint32_t ulStagger;

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
static uint8_t prvPollPreempted(void *pvContext)
{
	Coffee3RtuBusContext_t *pxContext;

	pxContext = (Coffee3RtuBusContext_t *)pvContext;
	return ((pxContext != NULL) &&
		(pxContext->ucPreemptRequested != 0U) &&
		(pxContext->ucPollingActive != 0U)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static BaseType_t prvTryBackgroundPoll(Coffee3RtuBusContext_t *pxContext,
	const Coffee3RtuBusConfig_t *pxConfig, Coffee3RtuBusStatus_t *pxStatus)
{
	Coffee3Command_t xCommand;
	Coffee3DeviceId_e xDeviceId;
	ModbusPortResult_e xResult;
	TickType_t xNow;
	uint8_t ucIndex;
	uint8_t ucOffset;
	uint32_t ulPeriodMs;

	if ((pxContext == NULL) || (pxConfig == NULL) ||
		(pxStatus == NULL) || (pxContext->ucCreated == 0U) ||
		(pxContext->ucPollCount == 0U) ||
		(uxQueueMessagesWaiting(pxContext->xQueue) != 0U)) {
		return pdFALSE;
	}
	xNow = xTaskGetTickCount();
	ucIndex = pxContext->ucPollCursor;
	if (ucIndex >= pxContext->ucPollCount) {
		ucIndex = 0U;
	}
	for (ucOffset = 0U; ucOffset < pxContext->ucPollCount; ucOffset++) {
		uint8_t ucCandidate;
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
	pxContext->ucPollCursor =
		(uint8_t)((ucIndex + 1U) % pxContext->ucPollCount);
	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = (uint8_t)COFFEE3_COMMAND_SOURCE_MAINTENANCE;
	xCommand.usAction = (uint16_t)COFFEE3_ACTION_REFRESH;
	xCommand.ulTimeoutMs = (pxConfig->ucBusId == 5U) ?
		COFFEE3_RTU_POLL_TIMEOUT_MS : COFFEE3_RTU_IO_TIMEOUT_MS;
	pxContext->ucPollingActive = 1U;
	pxContext->ucPreemptRequested = 0U;
	pxStatus->ucActiveDevice = (uint8_t)xDeviceId;
	vCoffee3DeviceCommandStarted(&xCommand);
	xResult = prvExecute(pxContext,
		pxCoffee3DeviceGetBinding(xDeviceId), &xCommand);
	pxContext->ucPollingActive = 0U;
	if (xResult == MODBUS_PORT_RESULT_PREEMPTED) {
		(void)xTransportControl(&pxContext->xChannel,
			TRANSPORT_CTRL_RX_FLUSH, NULL);
		/* The request was sacrificed; keep the cursor on this device and
		 * retry it after the foreground queue has drained. */
		pxContext->ucPollCursor = ucIndex;
		pxContext->axNextPollTick[xDeviceId] = xTaskGetTickCount() +
			pdMS_TO_TICKS(1U);
		vCoffee3DeviceCommandCompleted(&xCommand,
			MODBUS_PORT_RESULT_PREEMPTED, 0U);
		(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_INFO,
			prvGetDeviceLogSource((uint8_t)xDeviceId),
			COFFEE3_LOG_ORDER_SYSTEM,
			"Status poll paused: id=%u reason=foreground command result=%d",
			(unsigned int)pxCoffee3DeviceGetBinding(xDeviceId)->ucUnitId,
			(int)xResult);
		pxStatus->ucActiveDevice = 0U;
		return pdTRUE;
	}
	prvPublishPollHealth(pxContext, pxConfig, &xCommand, xResult);
	vCoffee3DeviceCommandCompleted(&xCommand, (int32_t)xResult,
		(xResult == MODBUS_PORT_RESULT_TIMEOUT) ? 1U : 0U);
	ulPeriodMs = (pxConfig->ucBusId == 5U) ?
		COFFEE3_RTU_IO_POLL_PERIOD_MS : COFFEE3_RTU_POLL_PERIOD_MS;
	pxContext->axNextPollTick[xDeviceId] = xTaskGetTickCount() +
		pdMS_TO_TICKS(ulPeriodMs);
	pxStatus->ucActiveDevice = 0U;
	return pdTRUE;
}

/*-----------------------------------------------------------*/
static void prvPublishPollHealth(Coffee3RtuBusContext_t *pxContext,
	const Coffee3RtuBusConfig_t *pxConfig,
	const Coffee3Command_t *pxCommand, ModbusPortResult_e xResult)
{
	const Coffee3DeviceBinding_t *pxBinding;
	Coffee3DeviceId_e xDeviceId;
	uint8_t ucLimit;
	uint8_t ucMiss;
	uint8_t ucWasOnline;
	uint8_t ucReady;
	uint8_t ucFaultMask;

	if ((pxContext == NULL) || (pxConfig == NULL) || (pxCommand == NULL)) {
		return;
	}
	xDeviceId = (Coffee3DeviceId_e)pxCommand->ucDeviceId;
	pxBinding = pxCoffee3DeviceGetBinding(xDeviceId);
	if (pxBinding == NULL) {
		return;
	}
	ucWasOnline = g_axCoffee3DeviceStatus[xDeviceId].ucOnline;
	ucLimit = (pxConfig->ucBusId == 5U) ?
		COFFEE3_RTU_IO_OFFLINE_MISS_LIMIT :
		COFFEE3_RTU_OFFLINE_MISS_LIMIT;
	/* A valid Modbus exception still proves that the target responded. It is
	 * online, but not ready for the requested operation. */
	if ((xResult == MODBUS_PORT_RESULT_OK) ||
		(xResult == MODBUS_PORT_RESULT_EXCEPTION)) {
		pxContext->aucPollMisses[xDeviceId] = 0U;
		vCoffee3DeviceSetOnline(xDeviceId, 1U);
		ucReady = (xResult == MODBUS_PORT_RESULT_OK) ? 1U : 0U;
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
	if (xResult == MODBUS_PORT_RESULT_PREEMPTED) {
		return;
	}
	ucMiss = pxContext->aucPollMisses[xDeviceId];
	if (ucMiss < 255U) {
		ucMiss++;
	}
	pxContext->aucPollMisses[xDeviceId] = ucMiss;
	/* Report only the first miss and the threshold miss. Repeated failures
	 * after the state has already been confirmed offline are silent. */
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
void vCoffee3RtuBusTask(void *pvArgument)
{
	static const char * const apcTaskRunning[COFFEE3_RTU_BUS_COUNT] = {
		"TASK_RUNNING:C3Bus2", "TASK_RUNNING:C3Bus3",
		"TASK_RUNNING:C3Bus4", "TASK_RUNNING:C3Bus5"
	};
	const Coffee3RtuBusConfig_t *pxConfig;
	const Coffee3DeviceBinding_t *pxBinding;
	Coffee3RtuBusContext_t *pxContext;
	Coffee3RtuBusStatus_t *pxStatus;
	TransportUartConfig_t xUartConfig;
	TransportResult_e xTransportResult;
	ModbusPortResult_e xResult;
	Coffee3Command_t xCommand;
	uint8_t ucAttempt;
	uint8_t ucIndex;
	uint8_t ucModbusPortIndex;

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
			pxContext->ucCreated = 1U;
			prvInitializePollSchedule(pxContext, pxConfig->ucBusId);
			vModbusPortSetPreemptCheck(pxContext->pxPort,
				prvPollPreempted, pxContext);
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

	for (;;) {
		if (xQueueReceive(pxContext->xQueue, &xCommand,
			pdMS_TO_TICKS(COFFEE3_RTU_IDLE_MS)) != pdPASS) {
			(void)prvTryBackgroundPoll(pxContext, pxConfig, pxStatus);
			continue;
		}
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
		/* A missed response must not cause a second drink or clean cycle. */
		if ((xCommand.ucDeviceId == COFFEE3_DEVICE_COFFEE_MACHINE) &&
			(xCommand.usAction != COFFEE3_ACTION_REFRESH)) {
			xCommand.ucRetryLimit = 0U;
		}
		xResult = MODBUS_PORT_RESULT_OK;
		for (ucAttempt = 0U;
			(xResult == MODBUS_PORT_RESULT_OK) &&
			(ucAttempt <= xCommand.ucRetryLimit);
			ucAttempt++) {
			if (ucCoffee3CommandIsCanceled(&xCommand) != 0U) {
				xResult = MODBUS_PORT_RESULT_CANCELED;
				break;
			}
			if (pxBinding->usMinimumIntervalMs != 0U) {
				TickType_t xMinimumTicks;
				TickType_t xElapsedTicks;

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
		/* A successful RTU refresh proves this device is ready for work.
		 * Online alone is not enough for Workflow's readiness mask. */
		if ((xResult == MODBUS_PORT_RESULT_OK) &&
			(xCommand.usAction == (uint16_t)COFFEE3_ACTION_REFRESH)) {
			vCoffee3DeviceSetReady(
				(Coffee3DeviceId_e)xCommand.ucDeviceId, 1U);
		}
		/* Online/offline is owned exclusively by the autonomous health poll.
		 * A foreground action failure is reported as an action failure, but it
		 * cannot by itself declare a device offline. */
	}
}

/*-----------------------------------------------------------*/
static HAL_StatusTypeDef prvConfigureUart(UART_HandleTypeDef *pxUart,
	uint32_t ulBaudRate)
{
	HAL_StatusTypeDef xResult;

	if ((pxUart == NULL) || (ulBaudRate == 0U)) {
		return HAL_ERROR;
	}
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
static ModbusPortResult_e prvExecute(Coffee3RtuBusContext_t *pxContext,
	const Coffee3DeviceBinding_t *pxBinding,
	const Coffee3Command_t *pxCommand)
{
	IoModuleModbusDigitalImage_t xIoImage;
	CupLidShengShuImage_t xCupLidImage;
	CoffeeMachineM50Image_t xM50Image;
	ModbusPortResult_e xResult;
	CoffeeMachineM50Action_e xM50Action;
	uint8_t ucPoint;
	uint8_t ucValue;

	memset(&xIoImage, 0, sizeof(xIoImage));
	memset(&xCupLidImage, 0, sizeof(xCupLidImage));
	switch (pxBinding->xDeviceId) {
	case COFFEE3_DEVICE_COFFEE_MACHINE:
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
			prvCommandCanceled, pxCommand);
		if ((xResult == MODBUS_PORT_RESULT_OK) &&
			((xM50Action == COFFEE_MACHINE_M50_ACTION_REFRESH) ||
			 (xM50Action == COFFEE_MACHINE_M50_ACTION_MAKE))) {
			vCoffee3DeviceImageCommitM50(&xM50Image);
		}
		return xResult;

	case COFFEE3_DEVICE_CUP_MACHINE:
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
			/* The ice controller uses a non-standard FC03 payload: the
			 * response echoes the start register (00 01) instead of a byte
			 * count. The driver validates that echo and the raw PDU CRC;
			 * a reported machine fault is not a communication failure. */
			return xIceMachineRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ulTimeoutMs, &g_xCoffee3IceImage);
		}
		if (pxCommand->usAction == COFFEE3_ACTION_ICE_SET_VALVE)
			return xIceMachineSetValve(pxContext->pxPort, pxBinding->ucUnitId,
				(uint8_t)pxCommand->ausParameter[0], pxCommand->ulTimeoutMs);
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;

	case COFFEE3_DEVICE_SCALE:
		switch ((Coffee3Action_e)pxCommand->usAction) {
		case COFFEE3_ACTION_REFRESH: return xScaleBsqDgV2RefreshGram(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs, &g_xCoffee3ScaleImage);
		case COFFEE3_ACTION_SCALE_TARE: return xScaleBsqDgV2Tare(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		case COFFEE3_ACTION_SCALE_CLEAR_TARE: return xScaleBsqDgV2ClearTare(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		case COFFEE3_ACTION_SCALE_ZERO: return xScaleBsqDgV2Zero(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		default: return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		}

	case COFFEE3_DEVICE_POWER_METER:
		if (pxCommand->usAction != COFFEE3_ACTION_REFRESH)
			return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		return xPowerMeterDdsu666Refresh(pxContext->pxPort, pxBinding->ucUnitId,
			pxCommand->ulTimeoutMs, &g_xCoffee3PowerMeterImage);

	case COFFEE3_DEVICE_IO_INPUT:
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
		if (pxCommand->usAction == COFFEE3_ACTION_REFRESH) {
			xResult = xIoModuleModbusReadOutputs(pxContext->pxPort,
				pxBinding->ucUnitId, 0U, COFFEE3_EXTERNAL_IO_POINT_COUNT,
				pxCommand->ulTimeoutMs, &xIoImage);
			if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee3IoCommitModbusOutputImage(xIoImage.aucPoints);
			}
			return xResult;
		}
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
static uint8_t prvCommandCanceled(const void *pvContext)
{
	return ucCoffee3CommandIsCanceled(
		(const Coffee3Command_t *)pvContext);
}

/*-----------------------------------------------------------*/
/*-----------------------------------------------------------*/
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
static const char *prvGetBusLinkEvent(uint8_t ucBusId)
{
	switch (ucBusId) {
	case 2U:
		return "DEVICE_LINK:BUS2_115200";
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
static void prvLogDeviceBindings(const Coffee3RtuBusConfig_t *pxConfig)
{
	const Coffee3DeviceBinding_t *pxBinding;
	Coffee3LogSource_e xSource;
	uint8_t ucDeviceId;

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
static uint8_t prvFindBusIndex(uint8_t ucBusId)
{
	uint8_t ucIndex;

	for (ucIndex = 0U; ucIndex < COFFEE3_RTU_BUS_COUNT; ucIndex++) {
		if (s_axBusConfigs[ucIndex].ucBusId == ucBusId) {
			return ucIndex;
		}
	}
	return COFFEE3_RTU_BUS_COUNT;
}

/*-----------------------------------------------------------*/
static uint8_t prvFindModbusPortIndex(uint8_t ucBusIndex)
{
	uint8_t ucIndex;
	uint8_t ucPortIndex;

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
static const char *prvModbusResultName(ModbusPortResult_e xResult)
{
	switch (xResult) {
	case MODBUS_PORT_RESULT_TIMEOUT: return "TIMEOUT";
	case MODBUS_PORT_RESULT_PROTOCOL: return "PROTOCOL";
	case MODBUS_PORT_RESULT_BUSY: return "BUSY";
	case MODBUS_PORT_RESULT_CANCELED: return "CANCELED";
	case MODBUS_PORT_RESULT_PREEMPTED: return "PREEMPTED";
	default: return "OTHER";
	}
}

/*-----------------------------------------------------------*/
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
static void prvLogCommandFailure(const Coffee3RtuBusConfig_t *pxConfig,
	const Coffee3Command_t *pxCommand, ModbusPortResult_e xResult)
{
	const Coffee3DeviceBinding_t *pxBinding;

	if ((pxConfig == NULL) || (pxCommand == NULL) ||
		(pxCommand->ucDeviceId >= (uint8_t)COFFEE3_DEVICE_COUNT) ||
		(g_axCoffee3DeviceStatus[pxCommand->ucDeviceId].lLastResult ==
			(int32_t)xResult)) {
		return;
	}
	pxBinding = pxCoffee3DeviceGetBinding(
		(Coffee3DeviceId_e)pxCommand->ucDeviceId);
	(void)xCoffee3LogPrintfOrder(COFFEE3_LOG_LEVEL_WARNING,
		prvGetDeviceLogSource(pxCommand->ucDeviceId),
		(uint16_t)pxCommand->ulOrderId,
		"Action failed: action=%s reason=%s id=%u result=%d code=%u step=%u",
		prvActionName(pxCommand->usAction), prvModbusResultName(xResult),
		(pxBinding != NULL) ? (unsigned int)pxBinding->ucUnitId : 0U,
		(int)xResult,
		(unsigned int)pxCommand->ucDeviceId,
		(unsigned int)pxCommand->usAction,
		(unsigned int)pxCommand->ucSource,
		(unsigned int)pxCommand->usStepId);
}

/*-----------------------------------------------------------*/
static void prvLogIoWriteExpected(const Coffee3Command_t *pxCommand)
{
	uint8_t ucValue;

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
static void prvLogIoWrite(const Coffee3Command_t *pxCommand,
	ModbusPortResult_e xResult, const IoModuleModbusDigitalImage_t *pxImage)
{
	uint8_t ucPoint;

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
