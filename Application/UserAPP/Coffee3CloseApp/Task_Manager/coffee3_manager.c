/**
  * @file      coffee3_manager.c
  * @brief     初始化并启动完整的 Coffee3 应用目标。
  * @author    WHong
  * @date      2026-09-24
  */

#include "coffee3_manager.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"
#include "coffee3_config.h"
#include "coffee3_device.h"
#include "coffee3_io.h"
#include "coffee3_log.h"
#include "coffee3_robot_tcp.h"
#include "coffee3_rtu_bus.h"
#include "coffee3_server.h"
#include "coffee3_workflow.h"
#include "event_groups.h"
#include "gpio.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "task.h"
#include "transport.h"
#include "usart.h"

/** @brief 由链接段放入仅供 CPU 访问 CCM 的 FreeRTOS 堆。 */
#if (configAPPLICATION_ALLOCATED_HEAP == 1)
APP_CCM_HEAP
uint8_t ucHeap[configTOTAL_HEAP_SIZE];
#endif

/** @brief MX_LWIP_Init() 返回后发布的协议栈就绪事件。 */
#define APP_TASK_EVENT_NETWORK_STACK_READY  (1UL << 0)
/** @brief 同时反映物理链路、接口和 IPv4 状态的网络就绪事件。 */
#define APP_TASK_EVENT_NETWORK_READY        (1UL << 1)
/** @brief 保存在任务管理器状态中的复位原因位。 */
#define APP_RESET_CAUSE_BOR                 (1UL << 0)
#define APP_RESET_CAUSE_PIN                 (1UL << 1)
#define APP_RESET_CAUSE_POR                 (1UL << 2)
#define APP_RESET_CAUSE_SOFTWARE            (1UL << 3)
#define APP_RESET_CAUSE_IWDG                (1UL << 4)
#define APP_RESET_CAUSE_WWDG                (1UL << 5)
#define APP_RESET_CAUSE_LOW_POWER           (1UL << 6)
/** @brief 运行指示灯心跳周期，单位毫秒。 */
#define APP_RUN_LED_TOGGLE_MS                500U

static EventGroupHandle_t s_xReadyEvents; /*!< 启动与网络就绪事件组句柄。 */
static StaticEventGroup_t s_xReadyEventStorage; /*!< 就绪事件组静态存储区。 */
static AppTaskManagerStatus_t s_xStatus; /*!< 可对外查询的启动与网络状态。 */

static void prvPublishNetworkReady(uint8_t ucReady);
static uint8_t prvIsNetworkReady(void);
static void prvApplyNetworkConfiguration(void);
static uint32_t prvCaptureResetCause(void);
static BaseType_t prvCreateTaskLogged(TaskFunction_t pxTaskCode,
	const char *pcTaskName, uint16_t usStackDepth, void *pvArgument,
	UBaseType_t uxPriority, Coffee3LogSource_e xSource,
	uint32_t ulTaskMask, const char *pcLogText);
static void prvWriteRawStartupFailure(const uint8_t *pucData,
	uint16_t usLength);
static Coffee3LogSource_e prvGetBusLogSource(uint8_t ucBusIndex);
static void prvUpdateNetworkIndicators(uint8_t ucNetworkReady);

/*-----------------------------------------------------------*/
/**
  * @brief  创建 Coffee3 静态基础设施、产品模块与业务任务。
  * @retval APP_TASK_MANAGER_RESULT_OK 所有必需模块和业务任务创建成功。
  * @retval APP_TASK_MANAGER_RESULT_ALREADY_CREATED 此前已经完成创建。
  * @retval APP_TASK_MANAGER_RESULT_SERIAL_INIT 业务串口默认配置失败。
  * @retval APP_TASK_MANAGER_RESULT_MODULE_INIT 产品模块初始化失败。
  * @retval APP_TASK_MANAGER_RESULT_NO_RESOURCE 事件组或必需任务创建失败。
  * @note 日志初始化失败会降级继续，不返回 APP_TASK_MANAGER_RESULT_LOG_INIT。
  */
AppTaskManagerResult_e xAppTaskManagerCreateTasks(void)
{
	/* 日志服务不可用时直接通过 USART1 输出的早期启动消息。 */
	static const uint8_t aucBootMessage[] =
		"[0000INFO][BOOT:System] POWER_ON result=0\r\n"; /*!< 上电事件。 */
	static const uint8_t aucVersionMessage[] =
		"[0000INFO][BOOT:System] " COFFEE3_DEVICE_VERSION_EVENT "\r\n"; /*!< 版本事件。 */
	static const uint8_t aucSerialFailMessage[] =
		"[0000ERROR][BOOT:UART] SERIAL_REINIT result=-3\r\n"; /*!< 串口失败消息。 */
	static const uint8_t aucLogFailMessage[] =
		"[0000ERROR][BOOT:Log] LOG_INIT result=-2\r\n"; /*!< 日志失败消息。 */
	static const uint8_t aucReadyFailMessage[] =
		"[0000ERROR][BOOT:System] READY_EVENTS_INIT result=-1\r\n"; /*!< 事件组失败消息。 */
	static const uint8_t aucDeviceFailMessage[] =
		"[0000ERROR][BOOT:Device] DEVICE_INIT result=-4\r\n"; /*!< 设备层失败消息。 */
	static const uint8_t aucRtuFailMessage[] =
		"[0000ERROR][BOOT:ModbusRtu] RTU_INIT result=-4\r\n"; /*!< RTU 失败消息。 */
	static const uint8_t aucRobotFailMessage[] =
		"[0000ERROR][BOOT:Robot] ROBOT_INIT result=-4\r\n"; /*!< 机器人失败消息。 */
	static const uint8_t aucWorkflowFailMessage[] =
		"[0000ERROR][BOOT:Workflow] WORKFLOW_INIT result=-4\r\n"; /*!< 工作流失败消息。 */
	static const uint8_t aucServerFailMessage[] =
		"[0000ERROR][BOOT:Server] SERVER_INIT result=-4\r\n"; /*!< 服务端失败消息。 */
	static const uint8_t aucTaskFailMessage[] =
		"[0000ERROR][BOOT:FreeRTOS] TASK_CREATE result=-1\r\n"; /*!< 任务失败消息。 */
	static const char * const apcBusTaskLog[COFFEE3_RTU_BUS_COUNT] = {
		"TASK_CREATE:C3Bus2", "TASK_CREATE:C3Bus3",
		"TASK_CREATE:C3Bus4", "TASK_CREATE:C3Bus5"
	}; /*!< 四个 RTU 任务对应的创建日志事件。 */
	const Coffee3RtuBusConfig_t *pxBusConfig; /*!< 当前 RTU 总线固定配置。 */
	Coffee3LogResult_e xLogResult; /*!< 日志缓冲区与传输初始化结果。 */
	HAL_StatusTypeDef xLogSerialResult; /*!< USART1 默认配置结果。 */
	BaseType_t xTaskResult; /*!< 必需业务任务的累计创建结果。 */
	BaseType_t xLogTaskResult; /*!< 可降级日志任务的创建结果。 */
	uint8_t ucBusIndex; /*!< 当前创建任务的 RTU 总线索引。 */

	/* 步骤 1：拒绝重复创建并建立本次启动状态基线。 */
	if (s_xStatus.ucInfrastructureCreated != 0U) {
		return APP_TASK_MANAGER_RESULT_ALREADY_CREATED;
	}
	
	memset(&s_xStatus, 0, sizeof(s_xStatus));
	s_xStatus.ulResetCause = prvCaptureResetCause();
	/* 步骤 2：初始化传输与日志；日志失败只记录降级状态。 */
	vTransportManagerInit();
	xLogSerialResult = xCoffee3LogSerialApplyDefault(); /* 配置 USART1 日志口。 */
	xLogResult = xCoffee3LogInitWithTransport((xLogSerialResult == HAL_OK) ? 1U : 0U);
	if ((xLogResult != COFFEE3_LOG_RESULT_OK) &&
		(xLogResult != COFFEE3_LOG_RESULT_ALREADY_INITIALIZED) &&
		(g_xCoffee3LogStatus.ucBufferReady == 0U)) {
		prvWriteRawStartupFailure(aucLogFailMessage,
			(uint16_t)(sizeof(aucLogFailMessage) - 1U));
	}
	s_xStatus.ucLogReady = 0U;
	if (xCoffee3SerialApplyDefaults() != HAL_OK) {
		prvWriteRawStartupFailure(aucSerialFailMessage,
			(uint16_t)(sizeof(aucSerialFailMessage) - 1U));
		s_xStatus.xStartResult = APP_TASK_MANAGER_RESULT_SERIAL_INIT;
		return s_xStatus.xStartResult;
	}
	if (xLogSerialResult != HAL_OK) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_SYSTEM, "LOG_UART_DEGRADED", 0,
			"hal", (int32_t)xLogSerialResult);
	}
	if ((xLogResult != COFFEE3_LOG_RESULT_OK) &&
		(xLogResult != COFFEE3_LOG_RESULT_ALREADY_INITIALIZED)) {
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_SYSTEM, "LOG_TRANSPORT_DEGRADED",
			(int32_t)xLogResult, "buffer", 
			(int32_t)g_xCoffee3LogStatus.ucBufferReady);
	}
	(void)lCoffee3LogEarlyWrite(aucBootMessage,
		(uint16_t)(sizeof(aucBootMessage) - 1U));
	(void)lCoffee3LogEarlyWrite(aucVersionMessage,
		(uint16_t)(sizeof(aucVersionMessage) - 1U));
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "RESET_CAUSE", 0,
		"reset_mask", (int32_t)s_xStatus.ulResetCause);
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "SERIAL_REINIT", 0,
		"baud", (int32_t)COFFEE3_LOG_BAUD);
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "MODULE_INIT:Transport", 0);
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "MODULE_INIT:Log", 0);
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, COFFEE3_DEVICE_VERSION_EVENT, 0);
	/* 步骤 3：创建网络就绪事件并依次初始化所有产品模块。 */
	s_xReadyEvents = xEventGroupCreateStatic(&s_xReadyEventStorage);
	if (s_xReadyEvents == NULL) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_SYSTEM, "MODULE_INIT:ReadyEvents",
			APP_TASK_MANAGER_RESULT_NO_RESOURCE);
		(void)lCoffee3LogEarlyWrite(aucReadyFailMessage,
			(uint16_t)(sizeof(aucReadyFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_NO_RESOURCE;
		return s_xStatus.xStartResult;
	}
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "MODULE_INIT:ReadyEvents", 0);
	if (xCoffee3ConfigInitialize() != CONFIG_STORE_OK) {
		static const uint8_t aucConfigFail[] = "Coffee3 configuration initialization failed; startup blocked\r\n";
			/*!< 配置读取或修复失败时的早期消息。 */
		(void)lCoffee3LogEarlyWrite(aucConfigFail,
			(uint16_t)(sizeof(aucConfigFail) - 1U));
		s_xStatus.xStartResult = APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	if (xCoffee3DeviceInitialize() != pdPASS) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_SYSTEM, "MODULE_INIT:Devices",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee3LogEarlyWrite(aucDeviceFailMessage,
			(uint16_t)(sizeof(aucDeviceFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucDeviceReady = 1U;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "MODULE_INIT:Devices", 0);
	vCoffee3IoInitialize();
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_IO, "MODULE_INIT:IoImage", 0);
	if (xCoffee3RtuBusInitialize() != pdPASS) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_SYSTEM, "MODULE_INIT:RtuBuses",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee3LogEarlyWrite(aucRtuFailMessage,
			(uint16_t)(sizeof(aucRtuFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucRtuReady = 1U;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "MODULE_INIT:RtuBuses", 0);
	if (xCoffee3RobotTcpInitialize() != pdPASS) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_ROBOT, "MODULE_INIT:Robot",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee3LogEarlyWrite(aucRobotFailMessage,
			(uint16_t)(sizeof(aucRobotFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucRobotReady = 1U;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_ROBOT, "MODULE_INIT:Robot", 0);
	if (xCoffee3WorkflowInitialize() != pdPASS) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_WORKFLOW, "MODULE_INIT:Workflow",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee3LogEarlyWrite(aucWorkflowFailMessage,
			(uint16_t)(sizeof(aucWorkflowFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucWorkflowReady = 1U;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_WORKFLOW, "MODULE_INIT:Workflow", 0);
	if (xCoffee3ServerInitialize() != pdPASS) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_ERROR,
			COFFEE3_LOG_SOURCE_SERVER, "MODULE_INIT:Server",
			APP_TASK_MANAGER_RESULT_MODULE_INIT);
		(void)lCoffee3LogEarlyWrite(aucServerFailMessage,
			(uint16_t)(sizeof(aucServerFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_MODULE_INIT;
		return s_xStatus.xStartResult;
	}
	s_xStatus.ucServerReady = 1U;
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SERVER, "MODULE_INIT:Server", 0);

	s_xStatus.ulFreeHeapBeforeTasks =
		(uint32_t)xPortGetFreeHeapSize();
	/* 步骤 4：创建服务端、机器人、四路 RTU 与工作流任务。 */
	xTaskResult = prvCreateTaskLogged(vCoffee3ServerTask, "C3Server",
		COFFEE3_SERVER_TASK_STACK, NULL,
		tskIDLE_PRIORITY + 3U, COFFEE3_LOG_SOURCE_SERVER,
		APP_TASK_MASK_SERVER, "TASK_CREATE:C3Server");
	if (xTaskResult == pdPASS) {
		xTaskResult = prvCreateTaskLogged(vCoffee3RobotTcpTask,
			"C3Robot",
			COFFEE3_ROBOT_TASK_STACK, NULL,
			tskIDLE_PRIORITY + 2U, COFFEE3_LOG_SOURCE_ROBOT,
			APP_TASK_MASK_ROBOT, "TASK_CREATE:C3Robot");
	}
	/* 为每一路已配置 RTU 总线创建一个任务。 */
	for (ucBusIndex = 0U;(ucBusIndex < COFFEE3_RTU_BUS_COUNT) &&(xTaskResult == pdPASS);ucBusIndex++) {
		pxBusConfig = pxCoffee3RtuBusGetConfig(ucBusIndex);  	/* 获取当前总线配置。 */
		xTaskResult = prvCreateTaskLogged(vCoffee3RtuBusTask, 	/* 创建 BUS 任务。 */
			pxBusConfig->pcName, COFFEE3_RTU_TASK_STACK,		/* 传入总线名称和栈深度。 */
			(void *)pxBusConfig, tskIDLE_PRIORITY + 2U,			/* 传入配置指针作为任务参数。 */
			prvGetBusLogSource(ucBusIndex),						/* 设置日志来源。 */
			(APP_TASK_MASK_BUS2 << ucBusIndex),					/* 设置任务掩码。 */
			apcBusTaskLog[ucBusIndex]);							/* 设置日志文本。 */
	}
	if (xTaskResult == pdPASS) {
		xTaskResult = prvCreateTaskLogged(vCoffee3WorkflowTask,
			"C3Workflow", COFFEE3_WORKFLOW_TASK_STACK, NULL,
			tskIDLE_PRIORITY + 2U, COFFEE3_LOG_SOURCE_WORKFLOW,
			APP_TASK_MASK_WORKFLOW, "TASK_CREATE:C3Workflow");
	}
	if ((xTaskResult == pdPASS) &&
		(g_xCoffee3LogStatus.ucTransportReady != 0U)) {
		/* 步骤 5：仅在传输就绪时创建可降级的日志输出任务。 */
		xLogTaskResult = prvCreateTaskLogged(vCoffee3LogTask, "C3Log",
			COFFEE3_LOG_TASK_STACK, NULL, tskIDLE_PRIORITY + 2U,
			COFFEE3_LOG_SOURCE_SYSTEM, APP_TASK_MASK_LOG,
			"TASK_CREATE:C3Log");
		vCoffee3LogSetTaskReady((xLogTaskResult == pdPASS) ? 1U : 0U);
		s_xStatus.ucLogReady =
			((xLogTaskResult == pdPASS) &&
			(g_xCoffee3LogStatus.ucTransportReady != 0U)) ? 1U : 0U;
	} else {
		vCoffee3LogSetTaskReady(0U);
		s_xStatus.ucLogReady = 0U;
	}
	s_xStatus.ulFreeHeapAfterTasks =
		(uint32_t)xPortGetFreeHeapSize();
	/* 步骤 6：必需任务失败时终止启动，否则发布完整启动状态。 */
	if (xTaskResult != pdPASS) {
		(void)lCoffee3LogEarlyWrite(aucTaskFailMessage,
			(uint16_t)(sizeof(aucTaskFailMessage) - 1U));
		s_xStatus.xStartResult =
			APP_TASK_MANAGER_RESULT_NO_RESOURCE;
		return s_xStatus.xStartResult;
	}
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "HEAP_AFTER_TASK_CREATE", 0,
		"bytes", (int32_t)s_xStatus.ulFreeHeapAfterTasks);
	s_xStatus.ucInfrastructureCreated = 1U;
	s_xStatus.ucTasksCreated = 1U;
	s_xStatus.xStartResult = APP_TASK_MANAGER_RESULT_OK;
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "STARTUP_COMPLETE", 0,
		"task_mask", (int32_t)s_xStatus.ulTaskCreatedMask);
	return APP_TASK_MANAGER_RESULT_OK;
}

/*-----------------------------------------------------------*/
/**
  * @brief  在 LwIP 初始化后应用静态地址并持续维护网络状态与指示灯。
  * @note 本函数作为默认任务主体运行，不会返回。
  */
void vAppTaskManagerRunDefaultTask(void)
{
	uint8_t ucInitialNetworkReady; /*!< 当前链路、接口与 IPv4 综合就绪状态。 */

	/* 步骤 1：应用产品静态地址并发布协议栈初始化完成。 */
	prvApplyNetworkConfiguration();
	taskENTER_CRITICAL();
	s_xStatus.ucNetworkStackReady = 1U;
	taskEXIT_CRITICAL();
	(void)xEventGroupSetBits(s_xReadyEvents,
		APP_TASK_EVENT_NETWORK_STACK_READY);
	(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "LWIP_STACK_READY", 0,
		"ip_last_octet", (int32_t)COFFEE3_IP_ADDRESS_3);
	ucInitialNetworkReady = prvIsNetworkReady();
	if (ucInitialNetworkReady == 0U) {
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_SYSTEM, "NETWORK_WAIT", -1);
	}
	prvPublishNetworkReady(ucInitialNetworkReady);
	prvUpdateNetworkIndicators(ucInitialNetworkReady);
	/* 步骤 2：周期检测网络条件并同步事件、状态和指示灯。 */
	for (;;) {
		ucInitialNetworkReady = prvIsNetworkReady();
		prvPublishNetworkReady(ucInitialNetworkReady);
		prvUpdateNetworkIndicators(ucInitialNetworkReady);
		vTaskDelay(pdMS_TO_TICKS(300U));
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  阻塞等待默认任务发布 LwIP 协议栈初始化完成。
  * @note 就绪事件组尚未创建时直接返回。
  */
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
/**
  * @brief  读取当前综合网络就绪事件。
  * @retval 1 物理链路、接口与 IPv4 地址均已就绪。
  * @retval 0 事件组未创建或至少一个网络条件不满足。
  */
uint8_t ucAppTaskManagerIsNetworkReady(void)
{
	EventBits_t xBits; /*!< 就绪事件组当前位快照。 */

	if (s_xReadyEvents == NULL) {
		return 0U;
	}
	xBits = xEventGroupGetBits(s_xReadyEvents);
	return ((xBits & APP_TASK_EVENT_NETWORK_READY) != 0U) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  在临界区内复制任务管理器状态快照。
  * @param[out] pxStatus 调用方提供的状态缓冲区；为空时忽略。
  */
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
/**
  * @brief  发布综合网络就绪状态，并只在状态变化时记录日志。
  * @param[in] ucReady 非零表示物理链路、接口和 IPv4 均满足要求。
  */
static void prvPublishNetworkReady(uint8_t ucReady)
{
	uint8_t ucChanged; /*!< 非零表示综合网络状态发生变化。 */

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
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
			COFFEE3_LOG_SOURCE_SYSTEM, "NETWORK_READY", 0);
	} else {
		(void)xEventGroupClearBits(s_xReadyEvents,
			APP_TASK_EVENT_NETWORK_READY);
		(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_WARNING,
			COFFEE3_LOG_SOURCE_SYSTEM, "NETWORK_DOWN", -1);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  检查默认 LwIP 接口的物理链路、接口与 IPv4 地址。
  * @retval 1 网络可供服务端与机器人任务使用。
  * @retval 0 至少一个网络条件尚未满足。
  */
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
/**
  * @brief  把 Coffee3 静态 IPv4 地址、掩码和网关应用到默认网卡。
  * @note 默认网卡为空时直接返回。
  */
static void prvApplyNetworkConfiguration(void)
{
	ip4_addr_t xAddress; /*!< 产品静态 IPv4 地址。 */
	ip4_addr_t xNetmask; /*!< 产品 IPv4 子网掩码。 */
	ip4_addr_t xGateway; /*!< 产品 IPv4 默认网关。 */

	if (netif_default == NULL) {
		return;
	}
	IP4_ADDR(&xAddress, COFFEE3_IP_ADDRESS_0,
		COFFEE3_IP_ADDRESS_1, COFFEE3_IP_ADDRESS_2,
		COFFEE3_IP_ADDRESS_3);
	IP4_ADDR(&xNetmask, COFFEE3_NETMASK_0,
		COFFEE3_NETMASK_1, COFFEE3_NETMASK_2,
		COFFEE3_NETMASK_3);
	IP4_ADDR(&xGateway, COFFEE3_GATEWAY_0,
		COFFEE3_GATEWAY_1, COFFEE3_GATEWAY_2,
		COFFEE3_GATEWAY_3);
	netif_set_addr(netif_default, &xAddress, &xNetmask, &xGateway);
}

/*-----------------------------------------------------------*/
/**
  * @brief  读取 RCC 复位标志并转换为产品状态位后清除硬件标志。
  * @retval uint32_t 供启动诊断使用的复位原因位掩码。
  */
static uint32_t prvCaptureResetCause(void)
{
	uint32_t ulCause; /*!< 累积转换后的产品复位原因位。 */

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
/**
  * @brief  创建一个 FreeRTOS 任务并记录创建结果。
  * @param[in] pxTaskCode 任务入口函数。
  * @param[in] pcTaskName FreeRTOS 调试任务名称。
  * @param[in] usStackDepth 任务栈深度，单位为 StackType_t。
  * @param[in] pvArgument 传递给任务入口的参数，可为空。
  * @param[in] uxPriority 任务优先级。
  * @param[in] xSource 创建结果的日志来源。
  * @param[in] ulTaskMask 该任务在启动状态中的位掩码。
  * @param[in] pcLogText 创建结果使用的日志事件文本。
  * @retval pdPASS 任务创建成功且创建掩码已置位。
  * @retval pdFAIL 资源不足且失败掩码已置位。
  */
static BaseType_t prvCreateTaskLogged(TaskFunction_t pxTaskCode,
	const char *pcTaskName, uint16_t usStackDepth, void *pvArgument,
	UBaseType_t uxPriority, Coffee3LogSource_e xSource,
	uint32_t ulTaskMask, const char *pcLogText)
{
	BaseType_t xResult; /*!< FreeRTOS 任务创建结果。 */

	xResult = xTaskCreate(pxTaskCode, pcTaskName, usStackDepth,
		pvArgument, uxPriority, NULL);
	if (xResult == pdPASS) {
		s_xStatus.ulTaskCreatedMask |= ulTaskMask;
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_INFO,
			xSource, pcLogText, 0, "task_mask", (int32_t)ulTaskMask);
	} else {
		s_xStatus.ulTaskFailedMask |= ulTaskMask;
		(void)xCoffee3LogWriteField(COFFEE3_LOG_LEVEL_ERROR,
			xSource, pcLogText,
			APP_TASK_MANAGER_RESULT_NO_RESOURCE,
			"task_mask", (int32_t)ulTaskMask);
	}
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  通过已经初始化的 USART1 输出受限长度的早期失败消息。
  * @param[in] pucData 待发送字节；为空时忽略。
  * @param[in] usLength 待发送长度，单位字节；零值时忽略。
  */
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
/**
  * @brief  把零起始 RTU 总线索引映射为 Coffee3 日志来源。
  * @param[in] ucBusIndex 总线索引，零至三对应第二至第五路总线。
  * @retval Coffee3LogSource_e 对应总线来源；越界时返回系统来源。
  */
static Coffee3LogSource_e prvGetBusLogSource(uint8_t ucBusIndex)
{
	switch (ucBusIndex) {
	case 0U:
		return COFFEE3_LOG_SOURCE_BUS2;
	case 1U:
		return COFFEE3_LOG_SOURCE_BUS3;
	case 2U:
		return COFFEE3_LOG_SOURCE_BUS4;
	case 3U:
		return COFFEE3_LOG_SOURCE_BUS5;
	default:  
		return COFFEE3_LOG_SOURCE_SYSTEM;  /* 非法索引回落到系统来源 */
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  更新运行心跳、网络告警与上位机连接指示灯。
  * @param[in] ucNetworkReady 非零表示当前网络综合状态就绪。
  */
static void prvUpdateNetworkIndicators(uint8_t ucNetworkReady)
{
	static TickType_t s_xLastRunToggleTick; /*!< 上次切换运行灯的 RTOS 节拍。 */
	static uint8_t s_ucRunOn; /*!< 当前运行灯逻辑亮灭状态。 */
	TickType_t xNow; /*!< 本轮更新读取的 RTOS 节拍。 */

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
		(g_xCoffee3ServerStatus.ucOnline != 0U)) ?
		GPIO_PIN_RESET : GPIO_PIN_SET);
}
