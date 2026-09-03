/**
  * @file      app_task_manager.c
  * @brief     Initialize and start the complete Coffee2 application target.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee2_manager.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "coffee2_app_config.h"
#include "coffee2_device.h"
#include "coffee2_io.h"
#include "coffee2_log.h"
#include "coffee2_robot_tcp.h"
#include "coffee2_rtu_bus.h"
#include "coffee2_server.h"
#include "coffee2_workflow.h"
#include "event_groups.h"
#include "gpio.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "task.h"
#include "transport.h"
#include "usart.h"

/** @brief FreeRTOS heap placed in CPU-only CCM by the linker section. */
#if (configAPPLICATION_ALLOCATED_HEAP == 1)
APP_CCM_HEAP
uint8_t ucHeap[configTOTAL_HEAP_SIZE];
#endif

/** @brief Event published after MX_LWIP_Init() returns. */
#define APP_TASK_EVENT_NETWORK_STACK_READY  (1UL << 0)
/** @brief Event mirroring physical, interface, and IPv4 readiness. */
#define APP_TASK_EVENT_NETWORK_READY        (1UL << 1)
/** @brief Reset-cause bits retained in AppTaskManagerStatus_t. */
#define APP_RESET_CAUSE_BOR                 (1UL << 0)
#define APP_RESET_CAUSE_PIN                 (1UL << 1)
#define APP_RESET_CAUSE_POR                 (1UL << 2)
#define APP_RESET_CAUSE_SOFTWARE            (1UL << 3)
#define APP_RESET_CAUSE_IWDG                (1UL << 4)
#define APP_RESET_CAUSE_WWDG                (1UL << 5)
#define APP_RESET_CAUSE_LOW_POWER           (1UL << 6)
/** @brief RUN LED heartbeat period in milliseconds. */
#define APP_RUN_LED_TOGGLE_MS                500U

static EventGroupHandle_t s_xReadyEvents;
static StaticEventGroup_t s_xReadyEventStorage;
static AppTaskManagerStatus_t s_xStatus;

/**
  * @brief  发布当前网络就绪状态。
  * @param[in] ucReady 非零表示 PHY、接口和 IPv4 状态均满足要求。
  */
static void prvPublishNetworkReady(uint8_t ucReady);
/**
  * @brief  检查默认 LwIP 接口的物理链路、接口和 IPv4 就绪状态。
  * @retval 1 网络已经可用于 Server/Robot 任务。
  * @retval 0 网络尚未满足工作条件。
  */
static uint8_t prvIsNetworkReady(void);
/**
  * @brief  在 CubeMX 生成的 LwIP 初始化后应用 Coffee2 的 IP 参数。
  * @note   仅修改应用拥有的默认网卡地址，不改变公用协议栈接口。
  */
static void prvApplyNetworkConfiguration(void);
/**
  * @brief  读取并清除 RCC 复位原因标志。
  * @retval 复位原因位掩码，供启动状态和崩溃分析使用。
  */
static uint32_t prvCaptureResetCause(void);
/**
  * @brief  创建一个 FreeRTOS 任务并记录任务创建结果。
  * @param[in]  pxTaskCode 任务入口函数；必须是有效的 FreeRTOS 任务函数。
  * @param[in]  pcTaskName 任务名称；用于 FreeRTOS 调试和日志识别。
  * @param[in]  usStackDepth 任务栈深度，单位为 StackType_t 个数。
  * @param[in]  pvArgument 传递给任务入口函数的参数指针，可以为 NULL。
  * @param[in]  uxPriority 任务优先级；必须符合当前系统的优先级范围。
  * @param[in]  xSource 任务所属的 Coffee2 日志来源，用于标记日志模块。
  * @param[in]  ulTaskMask 当前任务对应的创建状态位掩码。
  * @param[in]  pcLogText 任务创建结果的日志事件文本；必须保持有效。
  * @retval pdPASS 任务创建成功，并已置位创建掩码及输出成功日志。
  * @retval pdFAIL 任务资源不足导致创建失败，并已置位失败掩码及输出错误日志。
  * @note   任务由 FreeRTOS 动态创建；调用者负责保证入口函数和参数的生命周期。
  */
static BaseType_t prvCreateTaskLogged(TaskFunction_t pxTaskCode,
	const char *pcTaskName, uint16_t usStackDepth, void *pvArgument,
	UBaseType_t uxPriority, Coffee2LogSource_e xSource,
	uint32_t ulTaskMask, const char *pcLogText);
/**
  * @brief  通过已初始化的 USART1 输出受限长度的早期失败日志。
  * @param[in] pucData 待发送的字节数据，可以为 NULL 但此时不会发送。
  * @param[in] usLength 待发送字节数。
  */
static void prvWriteRawStartupFailure(const uint8_t *pucData,
	uint16_t usLength);
/**
  * @brief  将零基准 RTU 总线索引映射为日志来源。
  * @param[in] ucBusIndex 零基准总线索引，对应 Bus2 至 Bus5。
  * @retval 对应总线的 Coffee2 日志来源；越界时返回系统来源。
  */
static Coffee2LogSource_e prvGetBusLogSource(uint8_t ucBusIndex);
/**
  * @brief  更新运行指示灯、网络告警灯和上位机连接指示灯。
  * @param[in] ucNetworkReady 非零表示当前网络已经就绪。
  */
static void prvUpdateNetworkIndicators(uint8_t ucNetworkReady);

/*-----------------------------------------------------------*/
AppTaskManagerResult_e xAppTaskManagerCreateTasks(void)
{

	static const uint8_t aucBootMessage[] =
		"[0000INFO][BOOT:System] POWER_ON result=0\r\n";
	static const uint8_t aucVersionMessage[] =
		"[0000INFO][BOOT:System] " COFFEE2_DEVICE_VERSION_EVENT "\r\n";
	static const uint8_t aucSerialFailMessage[] =
		"[0000ERROR][BOOT:UART] SERIAL_REINIT result=-3\r\n";
	static const uint8_t aucLogFailMessage[] =
		"[0000ERROR][BOOT:Log] LOG_INIT result=-2\r\n";
	static const uint8_t aucReadyFailMessage[] =
		"[0000ERROR][BOOT:System] READY_EVENTS_INIT result=-1\r\n";
	static const uint8_t aucDeviceFailMessage[] =
		"[0000ERROR][BOOT:Device] DEVICE_INIT result=-4\r\n";
	static const uint8_t aucRtuFailMessage[] =
		"[0000ERROR][BOOT:ModbusRtu] RTU_INIT result=-4\r\n";
	static const uint8_t aucRobotFailMessage[] =
		"[0000ERROR][BOOT:Robot] ROBOT_INIT result=-4\r\n";
	static const uint8_t aucWorkflowFailMessage[] =
		"[0000ERROR][BOOT:Workflow] WORKFLOW_INIT result=-4\r\n";
	static const uint8_t aucServerFailMessage[] =
		"[0000ERROR][BOOT:Server] SERVER_INIT result=-4\r\n";
	static const uint8_t aucTaskFailMessage[] =
		"[0000ERROR][BOOT:FreeRTOS] TASK_CREATE result=-1\r\n";
	static const char * const apcBusTaskLog[COFFEE2_RTU_BUS_COUNT] = {
		"TASK_CREATE:C2Bus2", "TASK_CREATE:C2Bus3",
		"TASK_CREATE:C2Bus4", "TASK_CREATE:C2Bus5"
	};
	const Coffee2RtuBusConfig_t *pxBusConfig;
	Coffee2LogResult_e xLogResult;
	HAL_StatusTypeDef xLogSerialResult;
	BaseType_t xTaskResult;
	BaseType_t xLogTaskResult;
	uint8_t ucBusIndex;

	if (s_xStatus.ucInfrastructureCreated != 0U) {
		return APP_TASK_MANAGER_RESULT_ALREADY_CREATED;
	}
	memset(&s_xStatus, 0, sizeof(s_xStatus));
	s_xStatus.ulResetCause = prvCaptureResetCause();
	vTransportManagerInit();
	xLogSerialResult = xCoffee2LogSerialApplyDefault();
	xLogResult = xCoffee2LogInitWithTransport(
		(xLogSerialResult == HAL_OK) ? 1U : 0U);
	if ((xLogResult != COFFEE2_LOG_RESULT_OK) &&
		(xLogResult != COFFEE2_LOG_RESULT_ALREADY_INITIALIZED) &&
		(g_xCoffee2LogStatus.ucBufferReady == 0U)) {
		prvWriteRawStartupFailure(aucLogFailMessage,
			(uint16_t)(sizeof(aucLogFailMessage) - 1U));
	}
	s_xStatus.ucLogReady = 0U;
	if (xCoffee2SerialApplyDefaults() != HAL_OK) {
		prvWriteRawStartupFailure(aucSerialFailMessage,
			(uint16_t)(sizeof(aucSerialFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_SERIAL_INIT;
		return s_xStatus.xStartResult;
	}
	if (xLogSerialResult != HAL_OK) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SYSTEM, "LOG_UART_DEGRADED", 0,
			"hal", (int32_t)xLogSerialResult);
	}
	if ((xLogResult != COFFEE2_LOG_RESULT_OK) &&
		(xLogResult != COFFEE2_LOG_RESULT_ALREADY_INITIALIZED)) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SYSTEM, "LOG_TRANSPORT_DEGRADED",
			(int32_t)xLogResult, "buffer", 
			(int32_t)g_xCoffee2LogStatus.ucBufferReady);
	}
	(void)lCoffee2LogEarlyWrite(aucBootMessage,
		(uint16_t)(sizeof(aucBootMessage) - 1U));
	(void)lCoffee2LogEarlyWrite(aucVersionMessage,
		(uint16_t)(sizeof(aucVersionMessage) - 1U));
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "RESET_CAUSE", 0,
		"reset_mask", (int32_t)s_xStatus.ulResetCause);
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "SERIAL_REINIT", 0,
		"baud", (int32_t)COFFEE2_LOG_BAUD);
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "MODULE_INIT:Transport", 0);
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "MODULE_INIT:Log", 0);
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, COFFEE2_DEVICE_VERSION_EVENT, 0);
	s_xReadyEvents = xEventGroupCreateStatic(&s_xReadyEventStorage);
	if (s_xReadyEvents == NULL) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_SYSTEM, "MODULE_INIT:ReadyEvents",
			APP_TASK_MANAGER_RESULT_NO_RESOURCE);
		(void)lCoffee2LogEarlyWrite(aucReadyFailMessage,
			(uint16_t)(sizeof(aucReadyFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_NO_RESOURCE;
		return s_xStatus.xStartResult;
	}
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "MODULE_INIT:ReadyEvents", 0);
	if (xCoffee2DeviceInitialize() != pdPASS) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_SYSTEM, "MODULE_INIT:Devices",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee2LogEarlyWrite(aucDeviceFailMessage,
			(uint16_t)(sizeof(aucDeviceFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucDeviceReady = 1U;
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "MODULE_INIT:Devices", 0);
	vCoffee2IoInitialize();
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_IO, "MODULE_INIT:IoImage", 0);
	if (xCoffee2RtuBusInitialize() != pdPASS) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_SYSTEM, "MODULE_INIT:RtuBuses",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee2LogEarlyWrite(aucRtuFailMessage,
			(uint16_t)(sizeof(aucRtuFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucRtuReady = 1U;
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "MODULE_INIT:RtuBuses", 0);
	if (xCoffee2RobotTcpInitialize() != pdPASS) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_ROBOT, "MODULE_INIT:Robot",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee2LogEarlyWrite(aucRobotFailMessage,
			(uint16_t)(sizeof(aucRobotFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucRobotReady = 1U;
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_ROBOT, "MODULE_INIT:Robot", 0);
	if (xCoffee2WorkflowInitialize() != pdPASS) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_WORKFLOW, "MODULE_INIT:Workflow",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee2LogEarlyWrite(aucWorkflowFailMessage,
			(uint16_t)(sizeof(aucWorkflowFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucWorkflowReady = 1U;
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_WORKFLOW, "MODULE_INIT:Workflow", 0);
	if (xCoffee2ServerInitialize() != pdPASS) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_ERROR,
			COFFEE2_LOG_SOURCE_SERVER, "MODULE_INIT:Server",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee2LogEarlyWrite(aucServerFailMessage,
			(uint16_t)(sizeof(aucServerFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucServerReady = 1U;
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SERVER, "MODULE_INIT:Server", 0);

	s_xStatus.ulFreeHeapBeforeTasks =
		(uint32_t)xPortGetFreeHeapSize();
	xTaskResult = prvCreateTaskLogged(vCoffee2ServerTask, "C2Server",
		COFFEE2_SERVER_TASK_STACK, NULL,
		tskIDLE_PRIORITY + 3U, COFFEE2_LOG_SOURCE_SERVER,
		APP_TASK_MASK_SERVER, "TASK_CREATE:C2Server");
	if (xTaskResult == pdPASS) {
		xTaskResult = prvCreateTaskLogged(vCoffee2RobotTcpTask,
			"C2Robot",
			COFFEE2_ROBOT_TASK_STACK, NULL,
			tskIDLE_PRIORITY + 2U, COFFEE2_LOG_SOURCE_ROBOT,
			APP_TASK_MASK_ROBOT, "TASK_CREATE:C2Robot");
	}
	/* 为每一路已配置 RTU 总线创建一个任务，最后创建工作流任务。 */
	for (ucBusIndex = 0U;(ucBusIndex < COFFEE2_RTU_BUS_COUNT) &&(xTaskResult == pdPASS);ucBusIndex++) {
		pxBusConfig = pxCoffee2RtuBusGetConfig(ucBusIndex);  	/* 获取当前总线配置。 */
		xTaskResult = prvCreateTaskLogged(vCoffee2RtuBusTask, 	/* 创建 BUS 任务。 */
			pxBusConfig->pcName, COFFEE2_RTU_TASK_STACK,		
			(void *)pxBusConfig, tskIDLE_PRIORITY + 2U,			/* 传入配置指针作为任务参数。 */
			prvGetBusLogSource(ucBusIndex),
			(APP_TASK_MASK_BUS2 << ucBusIndex),
			apcBusTaskLog[ucBusIndex]);
	}
	if (xTaskResult == pdPASS) {
		xTaskResult = prvCreateTaskLogged(vCoffee2WorkflowTask,
			"C2Workflow", COFFEE2_WORKFLOW_TASK_STACK, NULL,
			tskIDLE_PRIORITY + 2U, COFFEE2_LOG_SOURCE_WORKFLOW,
			APP_TASK_MASK_WORKFLOW, "TASK_CREATE:C2Workflow");
	}
	if ((xTaskResult == pdPASS) &&
		(g_xCoffee2LogStatus.ucTransportReady != 0U)) {
		xLogTaskResult = prvCreateTaskLogged(vCoffee2LogTask, "C2Log",
			COFFEE2_LOG_TASK_STACK, NULL, tskIDLE_PRIORITY + 2U,
			COFFEE2_LOG_SOURCE_SYSTEM, APP_TASK_MASK_LOG,
			"TASK_CREATE:C2Log");
		vCoffee2LogSetTaskReady((xLogTaskResult == pdPASS) ? 1U : 0U);
		s_xStatus.ucLogReady =
			((xLogTaskResult == pdPASS) &&
			(g_xCoffee2LogStatus.ucTransportReady != 0U)) ? 1U : 0U;
	} else {
		vCoffee2LogSetTaskReady(0U);
		s_xStatus.ucLogReady = 0U;
	}
	s_xStatus.ulFreeHeapAfterTasks =
		(uint32_t)xPortGetFreeHeapSize();
	if (xTaskResult != pdPASS) {
		(void)lCoffee2LogEarlyWrite(aucTaskFailMessage,
			(uint16_t)(sizeof(aucTaskFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_NO_RESOURCE;
		return s_xStatus.xStartResult;
	}
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "HEAP_AFTER_TASK_CREATE", 0,
		"bytes", (int32_t)s_xStatus.ulFreeHeapAfterTasks);
	s_xStatus.ucInfrastructureCreated = 1U;
	s_xStatus.ucTasksCreated = 1U;
	s_xStatus.xStartResult = APP_TASK_MANAGER_RESULT_OK;
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "STARTUP_COMPLETE", 0,
		"task_mask", (int32_t)s_xStatus.ulTaskCreatedMask);
	return APP_TASK_MANAGER_RESULT_OK;
}

/*-----------------------------------------------------------*/
void vAppTaskManagerRunDefaultTask(void)
{
	uint8_t ucInitialNetworkReady;

	prvApplyNetworkConfiguration();
	taskENTER_CRITICAL();
	s_xStatus.ucNetworkStackReady = 1U;
	taskEXIT_CRITICAL();
	(void)xEventGroupSetBits(s_xReadyEvents,
		APP_TASK_EVENT_NETWORK_STACK_READY);
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "LWIP_STACK_READY", 0,
		"ip_last_octet", (int32_t)COFFEE2_IP_ADDRESS_3);
	ucInitialNetworkReady = prvIsNetworkReady();
	if (ucInitialNetworkReady == 0U) {
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SYSTEM, "NETWORK_WAIT", -1);
	}
	prvPublishNetworkReady(ucInitialNetworkReady);
	prvUpdateNetworkIndicators(ucInitialNetworkReady);
	for (;;) {
		ucInitialNetworkReady = prvIsNetworkReady();
		prvPublishNetworkReady(ucInitialNetworkReady);
		prvUpdateNetworkIndicators(ucInitialNetworkReady);
		vTaskDelay(pdMS_TO_TICKS(300U));
	}
}

/*-----------------------------------------------------------*/
void vAppTaskManagerWaitNetworkStackReady(void)
{
	if (s_xReadyEvents == NULL) {
		return;
	}
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
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_SYSTEM, "NETWORK_READY", 0);
	} else {
		(void)xEventGroupClearBits(s_xReadyEvents,
			APP_TASK_EVENT_NETWORK_READY);
		(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SYSTEM, "NETWORK_DOWN", -1);
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvIsNetworkReady(void)
{
	if ((netif_default == NULL) ||
		(netif_is_up(netif_default) == 0) ||
		(netif_is_link_up(netif_default) == 0) ||
		ip_addr_isany(netif_ip_addr4(netif_default))) {
		return 0U;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
static void prvApplyNetworkConfiguration(void)
{
	ip4_addr_t xAddress;
	ip4_addr_t xNetmask;
	ip4_addr_t xGateway;

	if (netif_default == NULL) {
		return;
	}
	IP4_ADDR(&xAddress, COFFEE2_IP_ADDRESS_0,
		COFFEE2_IP_ADDRESS_1, COFFEE2_IP_ADDRESS_2,
		COFFEE2_IP_ADDRESS_3);
	IP4_ADDR(&xNetmask, COFFEE2_NETMASK_0,
		COFFEE2_NETMASK_1, COFFEE2_NETMASK_2,
		COFFEE2_NETMASK_3);
	IP4_ADDR(&xGateway, COFFEE2_GATEWAY_0,
		COFFEE2_GATEWAY_1, COFFEE2_GATEWAY_2,
		COFFEE2_GATEWAY_3);
	netif_set_addr(netif_default, &xAddress, &xNetmask, &xGateway);
}

/*-----------------------------------------------------------*/
static uint32_t prvCaptureResetCause(void)
{
	uint32_t ulCause;

	ulCause = 0U;
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST) != RESET) {
		ulCause |= APP_RESET_CAUSE_BOR;
	}
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST) != RESET) {
		ulCause |= APP_RESET_CAUSE_PIN;
	}
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST) != RESET) {
		ulCause |= APP_RESET_CAUSE_POR;
	}
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST) != RESET) {
		ulCause |= APP_RESET_CAUSE_SOFTWARE;
	}
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) {
		ulCause |= APP_RESET_CAUSE_IWDG;
	}
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST) != RESET) {
		ulCause |= APP_RESET_CAUSE_WWDG;
	}
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST) != RESET) {
		ulCause |= APP_RESET_CAUSE_LOW_POWER;
	}
	__HAL_RCC_CLEAR_RESET_FLAGS();
	return ulCause;
}

/*-----------------------------------------------------------*/
static BaseType_t prvCreateTaskLogged(TaskFunction_t pxTaskCode,
	const char *pcTaskName, uint16_t usStackDepth, void *pvArgument,
	UBaseType_t uxPriority, Coffee2LogSource_e xSource,
	uint32_t ulTaskMask, const char *pcLogText)
{
	BaseType_t xResult;

	xResult = xTaskCreate(pxTaskCode, pcTaskName, usStackDepth,
		pvArgument, uxPriority, NULL);
	if (xResult == pdPASS) {
		s_xStatus.ulTaskCreatedMask |= ulTaskMask;
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			xSource, pcLogText, 0, "task_mask", (int32_t)ulTaskMask);
	} else {
		s_xStatus.ulTaskFailedMask |= ulTaskMask;
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_ERROR,
			xSource, pcLogText,
			APP_TASK_MANAGER_RESULT_NO_RESOURCE,
			"task_mask", (int32_t)ulTaskMask);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
static void prvWriteRawStartupFailure(const uint8_t *pucData,
	uint16_t usLength)
{
	if ((pucData == NULL) || (usLength == 0U) ||
		(huart1.Instance == NULL)) {
		return;
	}
	(void)HAL_UART_Transmit(&huart1, (uint8_t *)pucData,
		usLength, 100U);
}

/*-----------------------------------------------------------*/
static Coffee2LogSource_e prvGetBusLogSource(uint8_t ucBusIndex)
{
	switch (ucBusIndex) {
	case 0U:
		return COFFEE2_LOG_SOURCE_BUS2;
	case 1U:
		return COFFEE2_LOG_SOURCE_BUS3;
	case 2U:
		return COFFEE2_LOG_SOURCE_BUS4;
	default:
		return COFFEE2_LOG_SOURCE_BUS5;
	}
}

/*-----------------------------------------------------------*/
static void prvUpdateNetworkIndicators(uint8_t ucNetworkReady)
{
	static TickType_t s_xLastRunToggleTick;
	static uint8_t s_ucRunOn;
	TickType_t xNow;

	xNow = xTaskGetTickCount();
	if ((xNow - s_xLastRunToggleTick) >=
		pdMS_TO_TICKS(APP_RUN_LED_TOGGLE_MS)) {
		s_xLastRunToggleTick = xNow;
		s_ucRunOn = (s_ucRunOn == 0U) ? 1U : 0U;
	}
	HAL_GPIO_WritePin(PC13_LED_RUN_GPIO_Port, PC13_LED_RUN_Pin,
		(s_ucRunOn != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
	HAL_GPIO_WritePin(PC0_LED_ALM_GPIO_Port, PC0_LED_ALM_Pin,
		(ucNetworkReady == 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
	HAL_GPIO_WritePin(PA4_LED_TF_GPIO_Port, PA4_LED_TF_Pin,
		((ucNetworkReady != 0U) &&
		(g_xCoffee2ServerStatus.ucOnline != 0U)) ?
		GPIO_PIN_RESET : GPIO_PIN_SET);
}
