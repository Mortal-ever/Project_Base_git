/**
  * @file      app_task_manager.c
  * @brief     Create product tasks and coordinate LwIP readiness.
  * @author    WHong
  * @date      2026-07-28
  */

#include "app_task_manager.h"

#include <string.h>

#include "FreeRTOS.h"
#include "app_comm_log.h"
#include "app_comm_log_port.h"
#include "compiler_compat.h"
#include "app_debug.h"
#include "app_modbus_rtu_bus.h"
#include "app_modbus_tcp_client.h"
#include "app_net_monitor.h"
#include "app_workflow.h"
#include "event_groups.h"
#include "main.h"
#include "task.h"
#include "transport.h"

/** @brief FreeRTOS heap placed in CPU-only CCM by the linker section. */
#if (configAPPLICATION_ALLOCATED_HEAP == 1)
APP_CCM_HEAP
uint8_t ucHeap[configTOTAL_HEAP_SIZE];
#endif

/** @brief Event bit published after MX_LWIP_Init() returns. */
#define APP_TASK_EVENT_NETWORK_STACK_READY  (1UL << 0)
/** @brief Event bit mirroring physical, interface, and IPv4 readiness. */
#define APP_TASK_EVENT_NETWORK_READY        (1UL << 1)

/** @brief Shared readiness event group handle. */
static EventGroupHandle_t s_xReadyEvents;
/** @brief Static storage backing the readiness event group. */
static StaticEventGroup_t s_xReadyEventStorage;
/** @brief Observable task-manager startup and network status. */
static AppTaskManagerStatus_t s_xStatus;

/** @brief Publish network readiness and update the shared status snapshot. */
static void prvPublishNetworkReady(uint8_t ucReady);

/*-----------------------------------------------------------*/
AppTaskManagerResult_e xAppTaskManagerCreateTasks(void)
{
#if (APP_COMM_LOG_EARLY_UART_ENABLE != 0U)
	static const uint8_t aucBootMessage[] = "BOOT LOG UART READY\r\n";
	static const uint8_t aucTaskOkMessage[] = "TASK CREATE OK\r\n";
	static const uint8_t aucEventFailMessage[] = "TASK EVENT FAIL\r\n";
	static const uint8_t aucTaskFailMessage[] = "TASK CREATE FAIL\r\n";
#endif
	BaseType_t xResult;
	uint8_t ucBusIndex;
	const AppModbusRtuBusConfig_t *pxBusConfig;
	AppModbusTcpResult_e xClientResult;
#if (APP_COMM_LOG_ENABLE != 0U)
	AppCommLogResult_e xLogResult;
#endif

	if (s_xStatus.ucTasksCreated != 0U) {
		return APP_TASK_MANAGER_RESULT_ALREADY_CREATED;
	}

	memset(&s_xStatus, 0, sizeof(s_xStatus));
	vTransportManagerInit();
#if (APP_COMM_LOG_ENABLE != 0U)
	xLogResult = xAppCommLogPortRegister();
	if ((xLogResult != APP_COMM_LOG_RESULT_OK) &&
		(xLogResult != APP_COMM_LOG_RESULT_REPLACED)) {
		s_xStatus.xStartResult = APP_TASK_MANAGER_RESULT_LOG_INIT;
		return APP_TASK_MANAGER_RESULT_LOG_INIT;
	}
#endif

#if (APP_COMM_LOG_EARLY_UART_ENABLE != 0U)
	(void)lAppCommLogPortEarlyWrite(aucBootMessage,
		(uint16_t)(sizeof(aucBootMessage) - 1U));
#endif
	s_xReadyEvents = xEventGroupCreateStatic(&s_xReadyEventStorage);
	if (s_xReadyEvents == NULL) {
#if (APP_COMM_LOG_EARLY_UART_ENABLE != 0U)
		(void)lAppCommLogPortEarlyWrite(aucEventFailMessage,
			(uint16_t)(sizeof(aucEventFailMessage) - 1U));
#endif
		return APP_TASK_MANAGER_RESULT_NO_RESOURCE;
	}

	vAppDebugInit();
	xClientResult = xAppModbusTcpClientInit();
	if ((xClientResult != APP_MODBUS_TCP_RESULT_OK) &&
		(xClientResult != APP_MODBUS_TCP_RESULT_ALREADY_INITIALIZED)) {
		s_xStatus.xStartResult = APP_TASK_MANAGER_RESULT_CLIENT_INIT;
		return APP_TASK_MANAGER_RESULT_CLIENT_INIT;
	}
	s_xStatus.ucClientReady = 1U;

	xResult = pdPASS;
#if (APP_COMM_LOG_ENABLE != 0U)
	xResult = xTaskCreate(vAppCommLogTask, "LOG", 256U, NULL,
		tskIDLE_PRIORITY + 3U, NULL);
#endif
	if (xResult == pdPASS) {
		xResult = xTaskCreate(vAppDebugTask, "Debug", 384U, NULL,
			tskIDLE_PRIORITY + 2U, NULL);
	}
	if (xResult == pdPASS) {
		xResult = xTaskCreate(vAppRobotTcpTask, "RobotTcp", 640U, NULL,
			tskIDLE_PRIORITY + 2U, NULL);
	}
	if (xResult == pdPASS) {
		xResult = xTaskCreate(vAppMilkTeaTcpTask, "MilkTeaTcp", 768U,
			NULL, tskIDLE_PRIORITY + 2U, NULL);
	}
	for (ucBusIndex = 0U;
		(ucBusIndex < APP_MODBUS_RTU_BUS_COUNT) &&
		(xResult == pdPASS);
		ucBusIndex++) {
		pxBusConfig = pxAppModbusRtuBusGetConfig(ucBusIndex);
		xResult = xTaskCreate(vAppModbusRtuBusTask,
			pxBusConfig->pcName, 384U, (void *)pxBusConfig,
			tskIDLE_PRIORITY + 2U, NULL);
	}
	if (xResult == pdPASS) {
		xResult = xTaskCreate(vAppWorkFlowTask, "WorkFlow", 512U, NULL,
			tskIDLE_PRIORITY + 2U, NULL);
	}
	if (xResult != pdPASS) {
#if (APP_COMM_LOG_EARLY_UART_ENABLE != 0U)
		(void)lAppCommLogPortEarlyWrite(aucTaskFailMessage,
			(uint16_t)(sizeof(aucTaskFailMessage) - 1U));
#endif
		s_xStatus.xStartResult = APP_TASK_MANAGER_RESULT_NO_RESOURCE;
		return APP_TASK_MANAGER_RESULT_NO_RESOURCE;
	}
#if (APP_COMM_LOG_EARLY_UART_ENABLE != 0U)
	(void)lAppCommLogPortEarlyWrite(aucTaskOkMessage,
		(uint16_t)(sizeof(aucTaskOkMessage) - 1U));
#endif
	s_xStatus.ucTasksCreated = 1U;
	s_xStatus.xStartResult = APP_TASK_MANAGER_RESULT_OK;
	return APP_TASK_MANAGER_RESULT_OK;
}

/*-----------------------------------------------------------*/
void vAppTaskManagerRunDefaultTask(void)
{
	taskENTER_CRITICAL();
	s_xStatus.ucNetworkStackReady = 1U;
	taskEXIT_CRITICAL();
	(void)xEventGroupSetBits(s_xReadyEvents,
		APP_TASK_EVENT_NETWORK_STACK_READY);

	/* Keep indicators off until their product roles are assigned. */
	HAL_GPIO_WritePin(PC13_LED_RUN_GPIO_Port, PC13_LED_RUN_Pin,
		GPIO_PIN_SET);
	HAL_GPIO_WritePin(PC0_LED_ALM_GPIO_Port, PC0_LED_ALM_Pin,
		GPIO_PIN_SET);
	HAL_GPIO_WritePin(PA4_LED_TF_GPIO_Port, PA4_LED_TF_Pin,
		GPIO_PIN_SET);
	for (;;) {
		AppNetMonitor_Process();
		prvPublishNetworkReady(ucAppNetMonitorIsReady());
		vTaskDelay(pdMS_TO_TICKS(100U));
	}
}

/*-----------------------------------------------------------*/
void vAppTaskManagerWaitNetworkStackReady(void)
{
	(void)xEventGroupWaitBits(s_xReadyEvents,
		APP_TASK_EVENT_NETWORK_STACK_READY,
		pdFALSE, pdTRUE, portMAX_DELAY);
}

/*-----------------------------------------------------------*/
uint8_t ucAppTaskManagerIsNetworkReady(void)
{
	EventBits_t xBits;

	if (s_xReadyEvents == NULL) {
		return 0U;
	}
	xBits = xEventGroupGetBits(s_xReadyEvents);
	return ((xBits & APP_TASK_EVENT_NETWORK_READY) != 0U) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
void vAppTaskManagerGetStatus(AppTaskManagerStatus_t *pxStatus)
{
	if (pxStatus == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	*pxStatus = s_xStatus;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
static void prvPublishNetworkReady(uint8_t ucReady)
{
	uint8_t ucChanged;

	ucReady = (ucReady != 0U) ? 1U : 0U;
	taskENTER_CRITICAL();
	ucChanged = (s_xStatus.ucNetworkReady != ucReady) ? 1U : 0U;
	s_xStatus.ucNetworkReady = ucReady;
	taskEXIT_CRITICAL();
	if (ucChanged == 0U) {
		return;
	}
	if (ucReady != 0U) {
		(void)xEventGroupSetBits(s_xReadyEvents,
			APP_TASK_EVENT_NETWORK_READY);
	} else {
		(void)xEventGroupClearBits(s_xReadyEvents,
			APP_TASK_EVENT_NETWORK_READY);
	}
}
