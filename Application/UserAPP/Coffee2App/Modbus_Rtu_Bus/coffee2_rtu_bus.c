/**
  * @file      coffee2_rtu_bus.c
  * @brief     Implement four serialized single-protocol UART buses.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee2_rtu_bus.h"

#include <stddef.h>
#include <string.h>

#include "coffee2_device.h"
#include "coffee2_device_image.h"
#include "coffee2_io.h"
#include "coffee2_log.h"
#include "coffee_machine_f200.h"
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
		COFFEE2_COMMAND_QUEUE_LENGTH * sizeof(Coffee2Command_t)];
	QueueHandle_t xQueue;
	TransportChannel_t xChannel;
	TransportUartContext_t xTransport;
	ModbusPort_t *pxPort;
	TickType_t xLastTransactionTick;
	uint8_t ucCreated;
} Coffee2RtuBusContext_t;

Coffee2RtuBusStatus_t
	g_axCoffee2RtuBusStatus[COFFEE2_RTU_BUS_COUNT];

/** @brief Coffee2 ModbusRtuBus参数设置: STM32 UART 映射. */
static const Coffee2RtuBusConfig_t s_axBusConfigs[
	COFFEE2_RTU_BUS_COUNT] = {
	{ &huart2, "coffee2_bus2", COFFEE2_BUS2_DEFAULT_BAUD, 2U,
		COFFEE2_BUS2_PROTOCOL },
	{ &huart3, "coffee2_bus3", COFFEE2_BUS3_DEFAULT_BAUD, 3U,
		COFFEE2_BUS3_PROTOCOL },
	{ &huart4, "coffee2_bus4", COFFEE2_BUS4_DEFAULT_BAUD, 4U,
		COFFEE2_BUS4_PROTOCOL },
	{ &huart5, "coffee2_bus5", COFFEE2_BUS5_DEFAULT_BAUD, 5U,
		COFFEE2_BUS5_PROTOCOL }
};

/** @brief Private bus runtime resources in the compact Bus2-to-Bus5 order. */
static Coffee2RtuBusContext_t
	s_axBusContexts[COFFEE2_RTU_BUS_COUNT];

/** @brief Allocate one Modbus context only for each selected RTU bus. */
static ModbusPort_t s_axModbusPorts[COFFEE2_MODBUS_BUS_COUNT];

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
  * @brief  通过公共 DeviceLibrary 执行一个 Coffee2 命令。
  * @param[in,out] pxContext 当前 RTU 总线运行上下文。
  * @param[in]     pxBinding 命令目标设备的固定总线绑定。
  * @param[in]     pxCommand 待执行的标准命令，包含动作和参数。
  * @retval MODBUS_PORT_RESULT_OK 设备协议事务完成。
  * @retval 其他 ModbusPortResult_e 设备协议、传输或超时错误。
  */
static ModbusPortResult_e prvExecute(Coffee2RtuBusContext_t *pxContext,
	const Coffee2DeviceBinding_t *pxBinding,
	const Coffee2Command_t *pxCommand);
/**
  * @brief  将物理总线 ID 映射为日志来源。
  * @param[in] ucBusId 物理 Bus2 至 Bus5 的编号。
  * @retval 对应总线日志来源；未知编号返回系统来源。
  */
static Coffee2LogSource_e prvGetLogSource(uint8_t ucBusId);
/**
  * @brief  Map one logical RTU device to its diagnostic log source.
  * @param[in] ucDeviceId Logical Coffee2 device identifier.
  * @return Device-specific source, or system source for an invalid device.
  */
static Coffee2LogSource_e prvGetDeviceLogSource(uint8_t ucDeviceId);
static const char *prvGetDeviceName(uint8_t ucDeviceId);
static const char *prvGetDeviceProtocolEvent(uint8_t ucDeviceId);
static const char *prvGetBusLinkEvent(uint8_t ucBusId);
static void prvLogDeviceBindings(const Coffee2RtuBusConfig_t *pxConfig);
/**
  * @brief  查找物理总线 ID 在紧凑上下文数组中的索引。
  * @param[in] ucBusId 需要查找的物理总线编号。
  * @retval 紧凑数组索引；找不到时返回 COFFEE2_RTU_BUS_COUNT。
  */
static uint8_t prvFindBusIndex(uint8_t ucBusId);
/**
  * @brief  Find a bus's compact Modbus context index.
  * @param[in] ucBusIndex Zero-based physical bus configuration index.
  * @retval Compact Modbus context index.
  * @retval COFFEE2_MODBUS_BUS_COUNT when the bus is not Modbus RTU.
  */
static uint8_t prvFindModbusPortIndex(uint8_t ucBusIndex);
/**
  * @brief  仅在事务失败状态变化时输出一次诊断日志。
  * @param[in] pxConfig 当前总线配置，提供总线名称和编号。
  * @param[in] pxCommand 当前失败的命令，提供设备和命令号。
  * @param[in] xResult 设备协议事务的失败结果。
  */
static void prvLogCommandFailure(const Coffee2Command_t *pxCommand,
	ModbusPortResult_e xResult);
static void prvLogIoWriteExpected(const Coffee2Command_t *pxCommand);
static void prvLogIoWrite(const Coffee2Command_t *pxCommand,
	ModbusPortResult_e xResult, const IoModuleModbusDigitalImage_t *pxImage);
static uint8_t prvCommandCanceled(const void *pvContext);
static ModbusPortResult_e prvMapF200Result(CoffeeMachineF200Result_e xResult);

/*-----------------------------------------------------------*/
HAL_StatusTypeDef xCoffee2SerialApplyDefaults(void)
{
	HAL_StatusTypeDef xResult;

	xResult = prvConfigureUart(&huart2, COFFEE2_BUS2_DEFAULT_BAUD);
	if (xResult == HAL_OK) {
		xResult = prvConfigureUart(&huart3,
			COFFEE2_BUS3_DEFAULT_BAUD);
	}
	if (xResult == HAL_OK) {
		xResult = prvConfigureUart(&huart4,
			COFFEE2_BUS4_DEFAULT_BAUD);
	}
	if (xResult == HAL_OK) {
		xResult = prvConfigureUart(&huart5,
			COFFEE2_BUS5_DEFAULT_BAUD);
	}
	if (xResult == HAL_OK) {
		(void)HAL_UART_Abort(&huart6);
		xResult = HAL_UART_DeInit(&huart6);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
HAL_StatusTypeDef xCoffee2LogSerialApplyDefault(void)
{
	return prvConfigureUart(&huart1, COFFEE2_LOG_BAUD);
}

/*-----------------------------------------------------------*/
BaseType_t xCoffee2RtuBusInitialize(void)
{
	Coffee2RtuBusContext_t *pxContext;
	uint8_t ucIndex;

	memset(s_axBusContexts, 0, sizeof(s_axBusContexts));
	memset(g_axCoffee2RtuBusStatus, 0,
		sizeof(g_axCoffee2RtuBusStatus));
	for (ucIndex = 0U; ucIndex < COFFEE2_RTU_BUS_COUNT; ucIndex++) {
		pxContext = &s_axBusContexts[ucIndex];
		pxContext->xQueue = xQueueCreateStatic(
			COFFEE2_COMMAND_QUEUE_LENGTH,
			sizeof(Coffee2Command_t),
			pxContext->aucQueueStorage,
			&pxContext->xQueueStorage);
		if (pxContext->xQueue == NULL) {
			return pdFAIL;
		}
		vCoffee2DeviceRegisterRoute(s_axBusConfigs[ucIndex].ucBusId,
			pxContext->xQueue);
	}
	return pdPASS;
}

/*-----------------------------------------------------------*/
const Coffee2RtuBusConfig_t *pxCoffee2RtuBusGetConfig(uint8_t ucIndex)
{
	if (ucIndex >= COFFEE2_RTU_BUS_COUNT) {
		return NULL;
	}
	return &s_axBusConfigs[ucIndex];  /* 返回静态配置数组中的指针。 */
}

/*-----------------------------------------------------------*/
void vCoffee2RtuBusTask(void *pvArgument)
{
	static const char * const apcTaskRunning[COFFEE2_RTU_BUS_COUNT] = {
		"TASK_RUNNING:C2Bus2", "TASK_RUNNING:C2Bus3",
		"TASK_RUNNING:C2Bus4", "TASK_RUNNING:C2Bus5"
	};
	const Coffee2RtuBusConfig_t *pxConfig;
	const Coffee2DeviceBinding_t *pxBinding;
	Coffee2RtuBusContext_t *pxContext;
	Coffee2RtuBusStatus_t *pxStatus;
	TransportUartConfig_t xUartConfig;
	TransportResult_e xTransportResult;
	ModbusPortResult_e xResult;
	Coffee2Command_t xCommand;
	uint8_t ucAttempt;
	uint8_t ucIndex;
	uint8_t ucModbusPortIndex;
	uint8_t ucWasOnline;

	pxConfig = (const Coffee2RtuBusConfig_t *)pvArgument;
	if (pxConfig == NULL) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_SYSTEM, "RTU_TASK_ARGUMENT", -1);
		vTaskDelete(NULL);
		return;
	}
	ucIndex = prvFindBusIndex(pxConfig->ucBusId);
	if (ucIndex >= COFFEE2_RTU_BUS_COUNT) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_SYSTEM, "RTU_TASK_ARGUMENT", -1);
		vTaskDelete(NULL);
		return;
	}
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		prvGetLogSource(pxConfig->ucBusId), apcTaskRunning[ucIndex], 0);
	pxContext = &s_axBusContexts[ucIndex];
	pxStatus = &g_axCoffee2RtuBusStatus[ucIndex];
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
			COFFEE2_BUS_PROTOCOL_MODBUS_RTU) {
			ucModbusPortIndex = prvFindModbusPortIndex(ucIndex);
			if (ucModbusPortIndex >= COFFEE2_MODBUS_BUS_COUNT) {
				xResult = MODBUS_PORT_RESULT_INVALID_ARG;
			} else {
				pxContext->pxPort =
					&s_axModbusPorts[ucModbusPortIndex];
				xResult = xModbusPortClientInit(pxContext->pxPort,
					&pxContext->xChannel,
					MODBUS_PORT_TRANSPORT_RTU,
					COFFEE2_RTU_IO_TIMEOUT_MS);
			}
		} else if (pxConfig->ucProtocolId !=
			COFFEE2_BUS_PROTOCOL_F200_UART) {
			xResult = MODBUS_PORT_RESULT_NOT_SUPPORTED;
		}
		if (xResult == MODBUS_PORT_RESULT_OK) {
			pxContext->ucCreated = 1U;
			pxStatus->ulCurrentBaudRate =
				pxConfig->ulDefaultBaudRate;
			pxStatus->ucReady = 1U;
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
				prvGetLogSource(pxConfig->ucBusId), "BUS_READY", 0,
				"protocol", (int32_t)pxConfig->ucProtocolId);
			prvLogDeviceBindings(pxConfig);
		}
	} else {
		xResult = MODBUS_PORT_RESULT_TRANSPORT;
	}
	if (pxContext->ucCreated == 0U) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_ERROR,
			prvGetLogSource(pxConfig->ucBusId), "BUS_INIT_FAILED",
			(int32_t)xResult, "baud",
			(int32_t)pxConfig->ulDefaultBaudRate);
	}

	for (;;) {
		if (xQueueReceive(pxContext->xQueue, &xCommand,
			pdMS_TO_TICKS(COFFEE2_RTU_IDLE_MS)) != pdPASS) {
			continue;
		}
		pxBinding = pxCoffee2DeviceGetBinding(
			(Coffee2DeviceId_e)xCommand.ucDeviceId);
		if ((pxContext->ucCreated == 0U) ||
			(pxBinding == NULL) ||
			(pxBinding->ucRouteId != pxConfig->ucBusId) ||
			(pxBinding->ucProtocolId != pxConfig->ucProtocolId)) {
			xResult = MODBUS_PORT_RESULT_NOT_READY;
			vCoffee2DeviceCommandStarted(&xCommand);
			vCoffee2DeviceCommandCompleted(&xCommand,
				(int32_t)xResult, 0U);
			continue;
		}
		vCoffee2DeviceCommandStarted(&xCommand);
		ucWasOnline =
			g_axCoffee2DeviceStatus[xCommand.ucDeviceId].ucOnline;
		pxStatus->ucActiveDevice = xCommand.ucDeviceId;
		xResult = MODBUS_PORT_RESULT_OK;
		for (ucAttempt = 0U;
			(xResult == MODBUS_PORT_RESULT_OK) &&
			(ucAttempt <= xCommand.ucRetryLimit);
			ucAttempt++) {
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
			xResult = prvExecute(pxContext, pxBinding, &xCommand);
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
		if (xResult != MODBUS_PORT_RESULT_OK) {
			pxStatus->ulErrorCount++;
			prvLogCommandFailure(&xCommand, xResult);
		}
		vCoffee2DeviceCommandCompleted(&xCommand, (int32_t)xResult,
			(xResult == MODBUS_PORT_RESULT_TIMEOUT) ? 1U : 0U);
		if ((xResult == MODBUS_PORT_RESULT_OK) &&
			(ucWasOnline == 0U)) {
				(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
					prvGetDeviceLogSource(xCommand.ucDeviceId),
					(uint16_t)xCommand.ulOrderId,
					"RTU_DEVICE_ONLINE", 0, "device",
				(int32_t)xCommand.ucDeviceId);
		}
		if (ucModbusPortResultIsLinkFailure(xResult) != 0U) {
			vCoffee2DeviceSetOnline(
				(Coffee2DeviceId_e)xCommand.ucDeviceId, 0U);
			if (ucWasOnline != 0U) {
				(void)xCoffee2LogWriteFieldOrder(
					COFFEE2_LOG_LEVEL_WARNING,
					prvGetDeviceLogSource(xCommand.ucDeviceId),
					(uint16_t)xCommand.ulOrderId,
					"RTU_DEVICE_OFFLINE", (int32_t)xResult,
					"device", (int32_t)xCommand.ucDeviceId);
			}
		}
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
static ModbusPortResult_e prvExecute(Coffee2RtuBusContext_t *pxContext,
	const Coffee2DeviceBinding_t *pxBinding,
	const Coffee2Command_t *pxCommand)
{
	IoModuleModbusDigitalImage_t xIoImage;
	CupLidShengShuImage_t xCupLidImage;
	CoffeeMachineF200Status_t xF200Status;
	ModbusPortResult_e xResult;
	CoffeeMachineF200Result_e xF200Result;
	CoffeeMachineF200Action_e xF200Action;
	uint8_t ucDrinkId;

	memset(&xIoImage, 0, sizeof(xIoImage));
	memset(&xCupLidImage, 0, sizeof(xCupLidImage));
	switch (pxBinding->xDeviceId) {
	case COFFEE2_DEVICE_COFFEE_MACHINE:
		ucDrinkId = 0U;
		switch ((Coffee2Action_e)pxCommand->usAction) {
		case COFFEE2_ACTION_REFRESH: xF200Action = COFFEE_MACHINE_F200_ACTION_REFRESH; break;
		case COFFEE2_ACTION_COFFEE_MAKE:
			xF200Action = COFFEE_MACHINE_F200_ACTION_MAKE;
			xF200Result = xCoffeeMachineF200MapRecipe(pxCommand->ausParameter[0], &ucDrinkId);
			if (xF200Result != COFFEE_MACHINE_F200_RESULT_OK) return prvMapF200Result(xF200Result);
			break;
		case COFFEE2_ACTION_COFFEE_CLEAN:
			xF200Action = COFFEE_MACHINE_F200_ACTION_CLEAN;
			switch (pxCommand->ausParameter[0]) {
			case 1U: ucDrinkId = COFFEE_MACHINE_F200_COMMAND_BREW_RINSE; break;
			case 2U: ucDrinkId = COFFEE_MACHINE_F200_COMMAND_MILK_RINSE; break;
			case 3U: ucDrinkId = COFFEE_MACHINE_F200_COMMAND_POWDER_RINSE; break;
			case 4U: ucDrinkId = COFFEE_MACHINE_F200_COMMAND_MILK_FORCED_RINSE; break;
			case 5U: ucDrinkId = COFFEE_MACHINE_F200_COMMAND_ONE_KEY_CLEAN; break;
			case 6U: ucDrinkId = COFFEE_MACHINE_F200_COMMAND_MILK_SYSTEM_CLEAN; break;
			default: return MODBUS_PORT_RESULT_INVALID_ARG;
			}
			break;
		case COFFEE2_ACTION_CANCEL: xF200Action = COFFEE_MACHINE_F200_ACTION_CANCEL; break;
		default: return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		}
		memset(&xF200Status, 0, sizeof(xF200Status));
		xF200Result = xCoffeeMachineF200Execute(&pxContext->xChannel,
			xF200Action, ucDrinkId, pxCommand->ulTimeoutMs, &xF200Status,
			prvCommandCanceled, pxCommand);
		xResult = prvMapF200Result(xF200Result);
		if ((xResult == MODBUS_PORT_RESULT_OK) ||
			(xResult == MODBUS_PORT_RESULT_EXCEPTION)) {
			vCoffee2DeviceImageCommitF200(&xF200Status);
		}
		return xResult;

	case COFFEE2_DEVICE_CUP_MACHINE:
		if (pxCommand->usAction == COFFEE2_ACTION_REFRESH) {
			xResult = xCupLidShengShuRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				CUP_LID_ROLE_CUP, pxCommand->ulTimeoutMs, &xCupLidImage);
		} else if ((pxCommand->usAction == COFFEE2_ACTION_CUP_DROP_1) ||
			(pxCommand->usAction == COFFEE2_ACTION_CUP_DROP_2)) {
			xResult = xCupLidShengShuRun(pxContext->pxPort, pxBinding->ucUnitId,
				CUP_LID_ROLE_CUP,
				(pxCommand->usAction == COFFEE2_ACTION_CUP_DROP_1) ? 0U : 1U,
				pxCommand->ulTimeoutMs, &xCupLidImage, prvCommandCanceled, pxCommand);
		} else return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		if ((xResult == MODBUS_PORT_RESULT_OK) ||
			(xResult == MODBUS_PORT_RESULT_PROTOCOL)) {
			vCoffee2DeviceImageCommitCup(&xCupLidImage,
				(pxCommand->usAction == COFFEE2_ACTION_REFRESH) ? 1U : 0U,
				(pxCommand->usAction == COFFEE2_ACTION_CUP_DROP_2) ? 1U : 0U);
		}
		return xResult;

	case COFFEE2_DEVICE_SYRUP_MACHINE:
		if (pxCommand->usAction == COFFEE2_ACTION_REFRESH)
			return xSyrupMachineRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ulTimeoutMs, &g_xCoffee2SyrupImage);
		if (pxCommand->usAction == COFFEE2_ACTION_SYRUP_DISPENSE)
			return xSyrupMachineDispense(pxContext->pxPort, pxBinding->ucUnitId,
				(uint8_t)pxCommand->ausParameter[0], pxCommand->ausParameter[1],
				pxCommand->ulTimeoutMs, &g_xCoffee2SyrupImage,
				prvCommandCanceled, pxCommand);
		if (pxCommand->usAction == COFFEE2_ACTION_SYRUP_CLEAN)
			return xSyrupMachineClean(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ulTimeoutMs, &g_xCoffee2SyrupImage,
				prvCommandCanceled, pxCommand);
		if (pxCommand->usAction == COFFEE2_ACTION_SYRUP_SET_REMAINING)
			return xSyrupMachineSetRemaining(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ausParameter[0], pxCommand->ulTimeoutMs);
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;

	case COFFEE2_DEVICE_LID_MACHINE:
		if (pxCommand->usAction == COFFEE2_ACTION_REFRESH) {
			xResult = xCupLidShengShuRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				CUP_LID_ROLE_LID, pxCommand->ulTimeoutMs, &xCupLidImage);
		} else if ((pxCommand->usAction == COFFEE2_ACTION_LID_DROP_1) ||
			(pxCommand->usAction == COFFEE2_ACTION_LID_DROP_2)) {
			xResult = xCupLidShengShuRun(pxContext->pxPort, pxBinding->ucUnitId,
				CUP_LID_ROLE_LID,
				(pxCommand->usAction == COFFEE2_ACTION_LID_DROP_1) ? 0U : 1U,
				pxCommand->ulTimeoutMs, &xCupLidImage, prvCommandCanceled, pxCommand);
		} else return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		if ((xResult == MODBUS_PORT_RESULT_OK) ||
			(xResult == MODBUS_PORT_RESULT_PROTOCOL)) {
			vCoffee2DeviceImageCommitLid(&xCupLidImage,
				(pxCommand->usAction == COFFEE2_ACTION_REFRESH) ? 1U : 0U,
				(pxCommand->usAction == COFFEE2_ACTION_LID_DROP_2) ? 1U : 0U);
		}
		return xResult;

	case COFFEE2_DEVICE_ICE_MACHINE:
		if (pxCommand->usAction == COFFEE2_ACTION_REFRESH) {
			xResult = xIceMachineRefresh(pxContext->pxPort, pxBinding->ucUnitId,
				pxCommand->ulTimeoutMs, &g_xCoffee2IceImage);
			if ((xResult == MODBUS_PORT_RESULT_OK) &&
				(ucIceMachineGetFaultMask(&g_xCoffee2IceImage) != 0U))
				return MODBUS_PORT_RESULT_PROTOCOL;
			return xResult;
		}
		if (pxCommand->usAction == COFFEE2_ACTION_ICE_SET_VALVE)
			return xIceMachineSetValve(pxContext->pxPort, pxBinding->ucUnitId,
				(uint8_t)pxCommand->ausParameter[0], pxCommand->ulTimeoutMs);
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;

	case COFFEE2_DEVICE_SCALE:
		switch ((Coffee2Action_e)pxCommand->usAction) {
		case COFFEE2_ACTION_REFRESH: return xScaleBsqDgV2Refresh(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs, &g_xCoffee2ScaleImage);
		case COFFEE2_ACTION_SCALE_TARE: return xScaleBsqDgV2Tare(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		case COFFEE2_ACTION_SCALE_CLEAR_TARE: return xScaleBsqDgV2ClearTare(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		case COFFEE2_ACTION_SCALE_ZERO: return xScaleBsqDgV2Zero(pxContext->pxPort, pxBinding->ucUnitId, pxCommand->ulTimeoutMs);
		default: return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		}

	case COFFEE2_DEVICE_POWER_METER:
		if (pxCommand->usAction != COFFEE2_ACTION_REFRESH)
			return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		return xPowerMeterDdsu666Refresh(pxContext->pxPort, pxBinding->ucUnitId,
			pxCommand->ulTimeoutMs, &g_xCoffee2PowerMeterImage);

	case COFFEE2_DEVICE_IO_INPUT:
		if (pxCommand->usAction != COFFEE2_ACTION_REFRESH)
			return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		xResult = xIoModuleModbusReadInputs(pxContext->pxPort, pxBinding->ucUnitId,
			0U, COFFEE2_EXTERNAL_IO_POINT_COUNT, pxCommand->ulTimeoutMs, &xIoImage);
		if (xResult == MODBUS_PORT_RESULT_OK) {
			vCoffee2IoCommitModbusInput(xIoImage.aucPoints);
		}
		return xResult;

	case COFFEE2_DEVICE_IO_OUTPUT:
		if (pxCommand->usAction == COFFEE2_ACTION_REFRESH) {
			xResult = xIoModuleModbusReadOutputs(pxContext->pxPort,
				pxBinding->ucUnitId, 0U, COFFEE2_EXTERNAL_IO_POINT_COUNT,
				pxCommand->ulTimeoutMs, &xIoImage);
			if (xResult == MODBUS_PORT_RESULT_OK) {
				vCoffee2IoCommitModbusOutputImage(xIoImage.aucPoints);
			}
			return xResult;
		}
		prvLogIoWriteExpected(pxCommand);
		if (pxCommand->usAction != COFFEE2_ACTION_IO_WRITE)
			return MODBUS_PORT_RESULT_NOT_SUPPORTED;
		xResult = xIoModuleModbusWriteOutput(pxContext->pxPort, pxBinding->ucUnitId,
			0U, (uint8_t)pxCommand->ausParameter[0],
			(pxCommand->ausParameter[1] != 0U) ? 1U : 0U,
			COFFEE2_EXTERNAL_IO_POINT_COUNT, pxCommand->ulTimeoutMs, &xIoImage);
		prvLogIoWrite(pxCommand, xResult, &xIoImage);
		if ((xResult == MODBUS_PORT_RESULT_OK) ||
			(xResult == MODBUS_PORT_RESULT_PROTOCOL)) {
			vCoffee2IoCommitModbusOutputImage(xIoImage.aucPoints);
		}
		return xResult;

	default:
		return MODBUS_PORT_RESULT_NOT_SUPPORTED;
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvCommandCanceled(const void *pvContext)
{
	return ucCoffee2CommandIsCanceled(
		(const Coffee2Command_t *)pvContext);
}

/*-----------------------------------------------------------*/
static ModbusPortResult_e prvMapF200Result(CoffeeMachineF200Result_e xResult)
{
	switch (xResult) {
	case COFFEE_MACHINE_F200_RESULT_OK: return MODBUS_PORT_RESULT_OK;
	case COFFEE_MACHINE_F200_RESULT_INVALID_ARG: return MODBUS_PORT_RESULT_INVALID_ARG;
	case COFFEE_MACHINE_F200_RESULT_TIMEOUT: return MODBUS_PORT_RESULT_TIMEOUT;
	case COFFEE_MACHINE_F200_RESULT_TRANSPORT: return MODBUS_PORT_RESULT_TRANSPORT;
	case COFFEE_MACHINE_F200_RESULT_PROTOCOL: return MODBUS_PORT_RESULT_PROTOCOL;
	case COFFEE_MACHINE_F200_RESULT_REJECTED: return MODBUS_PORT_RESULT_EXCEPTION;
	case COFFEE_MACHINE_F200_RESULT_CANCELED: return MODBUS_PORT_RESULT_CANCELED;
	default: return MODBUS_PORT_RESULT_PROTOCOL;
	}
}

/*-----------------------------------------------------------*/
static Coffee2LogSource_e prvGetLogSource(uint8_t ucBusId)
{
	switch (ucBusId) {
	case 2U:
		return COFFEE2_LOG_SOURCE_BUS2;
	case 3U:
		return COFFEE2_LOG_SOURCE_BUS3;
	case 4U:
		return COFFEE2_LOG_SOURCE_BUS4;
	default:
		return COFFEE2_LOG_SOURCE_BUS5;
	}
}

/*-----------------------------------------------------------*/
static Coffee2LogSource_e prvGetDeviceLogSource(uint8_t ucDeviceId)
{
	switch ((Coffee2DeviceId_e)ucDeviceId) {
	case COFFEE2_DEVICE_COFFEE_MACHINE:
		return COFFEE2_LOG_SOURCE_COFFEE;
	case COFFEE2_DEVICE_CUP_MACHINE:
		return COFFEE2_LOG_SOURCE_CUP;
	case COFFEE2_DEVICE_SYRUP_MACHINE:
		return COFFEE2_LOG_SOURCE_SYRUP;
	case COFFEE2_DEVICE_LID_MACHINE:
		return COFFEE2_LOG_SOURCE_LID;
	case COFFEE2_DEVICE_ICE_MACHINE:
		return COFFEE2_LOG_SOURCE_ICE;
	case COFFEE2_DEVICE_SCALE:
		return COFFEE2_LOG_SOURCE_WEIGH;
	case COFFEE2_DEVICE_POWER_METER:
		return COFFEE2_LOG_SOURCE_ENERGY_METER;
	case COFFEE2_DEVICE_IO_INPUT:
		return COFFEE2_LOG_SOURCE_IO_INPUT;
	case COFFEE2_DEVICE_IO_OUTPUT:
		return COFFEE2_LOG_SOURCE_IO_OUTPUT;
	default:
		return COFFEE2_LOG_SOURCE_SYSTEM;
	}
}

/*-----------------------------------------------------------*/
static const char *prvGetDeviceName(uint8_t ucDeviceId)
{
	switch ((Coffee2DeviceId_e)ucDeviceId) {
	case COFFEE2_DEVICE_COFFEE_MACHINE: return "COFFEE_MACHINE";
	case COFFEE2_DEVICE_CUP_MACHINE: return "CUP_MACHINE";
	case COFFEE2_DEVICE_SYRUP_MACHINE: return "SYRUP_MACHINE";
	case COFFEE2_DEVICE_LID_MACHINE: return "LID_MACHINE";
	case COFFEE2_DEVICE_ICE_MACHINE: return "ICE_MACHINE";
	case COFFEE2_DEVICE_SCALE: return "SCALE_MODULE";
	case COFFEE2_DEVICE_POWER_METER: return "POWER_METER";
	case COFFEE2_DEVICE_IO_INPUT: return "IO_INPUT_MODULE_16CH";
	case COFFEE2_DEVICE_IO_OUTPUT: return "IO_OUTPUT_MODULE_16CH";
	default: return "DEVICE";
	}
}

/*-----------------------------------------------------------*/
static const char *prvGetDeviceProtocolEvent(uint8_t ucDeviceId)
{
	switch ((Coffee2DeviceId_e)ucDeviceId) {
	case COFFEE2_DEVICE_COFFEE_MACHINE:
		return "DEVICE_PROTOCOL:F200";
	case COFFEE2_DEVICE_CUP_MACHINE:
		return "DEVICE_PROTOCOL:SHENGSHU_CUP";
	case COFFEE2_DEVICE_SYRUP_MACHINE:
		return "DEVICE_PROTOCOL:SYRUP_MODBUS";
	case COFFEE2_DEVICE_LID_MACHINE:
		return "DEVICE_PROTOCOL:SHENGSHU_LID";
	case COFFEE2_DEVICE_ICE_MACHINE:
		return "DEVICE_PROTOCOL:ICE_MODBUS";
	case COFFEE2_DEVICE_SCALE:
		return "DEVICE_PROTOCOL:BSQ_DG_V2";
	case COFFEE2_DEVICE_POWER_METER:
		return "DEVICE_PROTOCOL:DDSU666";
	case COFFEE2_DEVICE_IO_INPUT:
	case COFFEE2_DEVICE_IO_OUTPUT:
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
static void prvLogDeviceBindings(const Coffee2RtuBusConfig_t *pxConfig)
{
	const Coffee2DeviceBinding_t *pxBinding;
	Coffee2LogSource_e xSource;
	uint8_t ucDeviceId;

	if (pxConfig == NULL) {
		return;
	}
	for (ucDeviceId = (uint8_t)COFFEE2_DEVICE_COFFEE_MACHINE;
		ucDeviceId < (uint8_t)COFFEE2_DEVICE_COUNT; ucDeviceId++) {
		pxBinding = pxCoffee2DeviceGetBinding(
			(Coffee2DeviceId_e)ucDeviceId);
		if ((pxBinding == NULL) ||
			(pxBinding->ucRouteId != pxConfig->ucBusId)) {
			continue;
		}
		xSource = prvGetDeviceLogSource(ucDeviceId);
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO, xSource,
			prvGetDeviceProtocolEvent(ucDeviceId), 0, "driver",
			(int32_t)pxBinding->ucDriverId);
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO, xSource,
			prvGetBusLinkEvent(pxConfig->ucBusId), 0, "unit",
			(int32_t)pxBinding->ucUnitId);
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvFindBusIndex(uint8_t ucBusId)
{
	uint8_t ucIndex;

	for (ucIndex = 0U; ucIndex < COFFEE2_RTU_BUS_COUNT; ucIndex++) {
		if (s_axBusConfigs[ucIndex].ucBusId == ucBusId) {
			return ucIndex;
		}
	}
	return COFFEE2_RTU_BUS_COUNT;
}

/*-----------------------------------------------------------*/
static uint8_t prvFindModbusPortIndex(uint8_t ucBusIndex)
{
	uint8_t ucIndex;
	uint8_t ucPortIndex;

	if ((ucBusIndex >= COFFEE2_RTU_BUS_COUNT) ||
		(s_axBusConfigs[ucBusIndex].ucProtocolId !=
		COFFEE2_BUS_PROTOCOL_MODBUS_RTU)) {
		return COFFEE2_MODBUS_BUS_COUNT;
	}
	ucPortIndex = 0U;
	for (ucIndex = 0U; ucIndex < ucBusIndex; ucIndex++) {
		if (s_axBusConfigs[ucIndex].ucProtocolId ==
			COFFEE2_BUS_PROTOCOL_MODBUS_RTU) {
			ucPortIndex++;
		}
	}
	return ucPortIndex;
}

/*-----------------------------------------------------------*/
static void prvLogCommandFailure(const Coffee2Command_t *pxCommand,
	ModbusPortResult_e xResult)
{
	if ((pxCommand->ucDeviceId >= (uint8_t)COFFEE2_DEVICE_COUNT) ||
		(g_axCoffee2DeviceStatus[pxCommand->ucDeviceId].lLastResult ==
			(int32_t)xResult)) {
		return;
	}
	(void)xCoffee2LogPrintfOrder(COFFEE2_LOG_LEVEL_WARNING,
		prvGetDeviceLogSource(pxCommand->ucDeviceId),
		(uint16_t)pxCommand->ulOrderId,
		"%s_COMMAND_FAILED RESULT=%d DEVICE_ID=%u",
		prvGetDeviceName(pxCommand->ucDeviceId), (int)xResult,
		(unsigned int)pxCommand->ucDeviceId);
}

/*-----------------------------------------------------------*/
static void prvLogIoWriteExpected(const Coffee2Command_t *pxCommand)
{
	uint8_t ucValue;

	if ((pxCommand == NULL) ||
		(pxCommand->ucSource !=
			(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) ||
		(pxCommand->usAction != (uint16_t)COFFEE2_ACTION_IO_WRITE)) {
		return;
	}
	ucValue = (pxCommand->ausParameter[1] != 0U) ? 1U : 0U;
	(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
		"IO_WRITE_EXPECTED", 0, "value", (int32_t)ucValue);
}

/*-----------------------------------------------------------*/
static void prvLogIoWrite(const Coffee2Command_t *pxCommand,
	ModbusPortResult_e xResult, const IoModuleModbusDigitalImage_t *pxImage)
{
	uint8_t ucPoint;

	if ((pxCommand == NULL) ||
		(pxCommand->ucSource !=
			(uint8_t)COFFEE2_COMMAND_SOURCE_SERVER) ||
		(pxCommand->usAction != (uint16_t)COFFEE2_ACTION_IO_WRITE)) {
		return;
	}
	ucPoint = (uint8_t)pxCommand->ausParameter[0];
	if ((xResult == MODBUS_PORT_RESULT_OK) ||
		(xResult == MODBUS_PORT_RESULT_PROTOCOL)) {
		if ((pxImage == NULL) || (ucPoint >= pxImage->ucPointCount)) {
			return;
		}
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
			"IO_WRITE_OBSERVED", 0, "value",
			(int32_t)pxImage->aucPoints[ucPoint]);
		if (xResult == MODBUS_PORT_RESULT_PROTOCOL) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"IO_WRITE_MISMATCH", (int32_t)xResult,
				"point", (int32_t)(ucPoint + 1U));
		} else {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_SERVER,
				(uint16_t)pxCommand->ulOrderId,
				"IO_WRITE_SUCCESS", 0,
				"point", (int32_t)(ucPoint + 1U));
		}
	} else {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER, (uint16_t)pxCommand->ulOrderId,
			"IO_WRITE_READ_FAILED", (int32_t)xResult,
			"point", (int32_t)(ucPoint + 1U));
	}
}
