/**
  * @file      app_modbus_tcp_client.c
  * @brief     Run TCP device tasks with serialized access and reconnect.
  * @author    WHong
  * @date      2026-07-28
  */

#include "app_modbus_tcp_client.h"

#include <string.h>

#include "app_comm_log.h"
#include "app_debug.h"
#include "app_modbus_tcp_config.h"
#include "app_task_manager.h"
#include "robot_protocol.h"
#include "semphr.h"
#include "task.h"
#include "transport_tcp.h"

/** @brief Endpoint mutex acquisition timeout in milliseconds. */
#define APP_TCP_MUTEX_TIMEOUT_MS          1000U

/** @brief Store all resources owned by one product TCP endpoint. */
typedef struct {
	TransportChannel_t xChannel; /*!< Generic TCP channel. */
	TransportTcpContext_t xTransport; /*!< LwIP Netconn backend state. */
	ModbusPort_t xClient; /*!< Modbus TCP client instance. */
	ModbusPortTrace_t xTrace; /*!< Latest Modbus request and response trace. */
	StaticSemaphore_t xMutexStorage; /*!< Static endpoint mutex storage. */
	SemaphoreHandle_t xMutex; /*!< Serializes all endpoint operations. */
	AppModbusTcpStatus_t *pxStatus; /*!< Public endpoint status destination. */
	TickType_t xNextPollTick; /*!< Earliest Tick for the next health request. */
	volatile uint8_t ucReconnectRequested; /*!< Debug-requested reconnect flag. */
	uint8_t ucCreated; /*!< Nonzero after endpoint object creation. */
} AppTcpEndpoint_t;

/** @brief Public Robot TCP endpoint status. */
AppModbusTcpStatus_t g_xRobotTcpStatus;
/** @brief Public MilkTea TCP endpoint status. */
AppModbusTcpStatus_t g_xMilkTeaTcpStatus;

/** @brief Private Robot TCP endpoint resources. */
static AppTcpEndpoint_t s_xRobotEndpoint;
/** @brief Private MilkTea TCP endpoint resources. */
static AppTcpEndpoint_t s_xMilkTeaEndpoint;
/** @brief Robot device-protocol binding. */
static RobotProtocol_t s_xRobotProtocol;
/** @brief MilkTea device-protocol binding. */
static MilkTeaProtocol_t s_xMilkTeaProtocol;
/** @brief Nonzero after application initialization completes. */
static uint8_t s_ucInitialized;

/** @brief Test a FreeRTOS deadline using wrap-safe signed Tick arithmetic. */
static uint8_t prvTickReached(TickType_t xNow, TickType_t xDeadline);
/** @brief Select the bounded reconnect delay for a failure count. */
static uint32_t prvRetryDelayMs(uint32_t ulFailureCount);
/** @brief Create Transport, Modbus, trace, mutex, and status endpoint objects. */
static TransportResult_e prvCreateEndpoint(AppTcpEndpoint_t *pxEndpoint,
	AppModbusTcpStatus_t *pxStatus, const char *pcName,
	const uint8_t *pucIp, uint16_t usPort);
/** @brief Advance one endpoint's network, connect, and retry state machine. */
static void prvConnectionProcess(AppTcpEndpoint_t *pxEndpoint,
	AppCommSource_e xOpenSource, TickType_t xNow);
/** @brief Publish a new lifecycle state to one endpoint status object. */
static void prvSetState(AppTcpEndpoint_t *pxEndpoint,
	AppDeviceCommState_e xState);
/** @brief Close the endpoint and schedule its next reconnect attempt. */
static void prvScheduleRetry(AppTcpEndpoint_t *pxEndpoint,
	TickType_t xNow, uint8_t ucCountFailure);
/** @brief Close an active endpoint and update disconnect statistics. */
static void prvCloseEndpoint(AppTcpEndpoint_t *pxEndpoint);
/** @brief Update transaction counters and the latest result. */
static void prvRecordResult(AppModbusTcpStatus_t *pxStatus,
	ModbusPortResult_e xResult);
/** @brief Classify Modbus results that require TCP reconstruction. */
static uint8_t prvResultNeedsReconnect(ModbusPortResult_e xResult);
/** @brief Report endpoint readiness to the Debug service. */
static uint8_t prvDebugReady(void *pvContext);
/** @brief Record a Debug transaction and request reconnect when required. */
static void prvDebugResult(ModbusPortResult_e xResult, void *pvContext);
/** @brief Register the MilkTea endpoint with the UART Debug service. */
static void prvRegisterMilkTeaDebug(void);

/*-----------------------------------------------------------*/
AppModbusTcpResult_e xAppModbusTcpClientInit(void)
{
	if (s_ucInitialized != 0U) {
		return APP_MODBUS_TCP_RESULT_ALREADY_INITIALIZED;
	}
	memset(&g_xRobotTcpStatus, 0, sizeof(g_xRobotTcpStatus));
	memset(&g_xMilkTeaTcpStatus, 0, sizeof(g_xMilkTeaTcpStatus));
	g_xRobotTcpStatus.ucEnabled = APP_ROBOT_TCP_ENABLE;
	g_xMilkTeaTcpStatus.ucEnabled = APP_MILKTEA_TCP_ENABLE;
	g_xRobotTcpStatus.xState = APP_DEVICE_COMM_WAIT_NETWORK;
	g_xMilkTeaTcpStatus.xState = APP_DEVICE_COMM_WAIT_NETWORK;
	s_ucInitialized = 1U;
	return APP_MODBUS_TCP_RESULT_OK;
}

/*-----------------------------------------------------------*/
void vAppRobotTcpTask(void *pvArgument)
{
	static const uint8_t aucIp[4] = {
		APP_ROBOT_TCP_IP_0, APP_ROBOT_TCP_IP_1,
		APP_ROBOT_TCP_IP_2, APP_ROBOT_TCP_IP_3
	};
	TickType_t xNow;
	ModbusPortResult_e xResult;

	(void)pvArgument;
	vAppTaskManagerWaitNetworkStackReady();
	if (g_xRobotTcpStatus.ucEnabled != 0U) {
		if (prvCreateEndpoint(&s_xRobotEndpoint, &g_xRobotTcpStatus,
			"robot_tcp", aucIp, APP_ROBOT_TCP_PORT) ==
			TRANSPORT_RESULT_OK) {
			vRobotProtocolInit(&s_xRobotProtocol,
				&s_xRobotEndpoint.xClient, APP_ROBOT_TCP_UNIT_ID);
		}
	}
	for (;;) {
		xNow = xTaskGetTickCount();
		if ((g_xRobotTcpStatus.ucEnabled != 0U) &&
			(s_xRobotEndpoint.ucCreated != 0U)) {
			prvConnectionProcess(&s_xRobotEndpoint,
				APP_COMM_SOURCE_ROBOT_OPEN, xNow);
			if (g_xRobotTcpStatus.xState ==
				APP_DEVICE_COMM_WAIT_SERVICE) {
				if (xSemaphoreTake(s_xRobotEndpoint.xMutex,
					pdMS_TO_TICKS(APP_TCP_MUTEX_TIMEOUT_MS)) ==
					pdTRUE) {
					xResult = xRobotProtocolReadHolding(
						&s_xRobotProtocol, 0U, 1U,
						APP_MODBUS_TCP_IO_TIMEOUT_MS);
					(void)xSemaphoreGive(
						s_xRobotEndpoint.xMutex);
					prvRecordResult(&g_xRobotTcpStatus, xResult);
					if ((xResult == MODBUS_PORT_RESULT_OK) ||
						(xResult == MODBUS_PORT_RESULT_EXCEPTION)) {
						g_xRobotTcpStatus.ulConsecutiveFailures = 0U;
						prvSetState(&s_xRobotEndpoint,
							APP_DEVICE_COMM_ONLINE);
						s_xRobotEndpoint.xNextPollTick = xNow +
							pdMS_TO_TICKS(APP_ROBOT_TEST_MS);
					} else {
						prvScheduleRetry(&s_xRobotEndpoint,
							xNow, 1U);
					}
				}
			} else if ((g_xRobotTcpStatus.xState ==
				APP_DEVICE_COMM_ONLINE) &&
				(APP_ROBOT_TEST_ENABLE != 0U) &&
				(prvTickReached(xNow,
				s_xRobotEndpoint.xNextPollTick) != 0U)) {
				if (xSemaphoreTake(s_xRobotEndpoint.xMutex, 0U) ==
					pdTRUE) {
					xResult = xRobotProtocolReadHolding(
						&s_xRobotProtocol, 0U, 1U,
						APP_MODBUS_TCP_IO_TIMEOUT_MS);
					(void)xSemaphoreGive(
						s_xRobotEndpoint.xMutex);
					prvRecordResult(&g_xRobotTcpStatus, xResult);
					vAppCommLogWrite(APP_COMM_SOURCE_ROBOT_MODBUS,
						(int32_t)xResult,
						(xResult == MODBUS_PORT_RESULT_OK) ?
						(int32_t)g_xRobotData.holdingRegisters[0] :
						s_xRobotEndpoint.xTransport.lLastNativeError);
					s_xRobotEndpoint.xNextPollTick = xNow +
						pdMS_TO_TICKS(APP_ROBOT_TEST_MS);
					if (prvResultNeedsReconnect(xResult) != 0U) {
						prvScheduleRetry(&s_xRobotEndpoint,
							xNow, 1U);
					}
				}
			}
		}
		vTaskDelay(pdMS_TO_TICKS(APP_MODBUS_TCP_LOOP_MS));
	}
}

/*-----------------------------------------------------------*/
void vAppMilkTeaTcpTask(void *pvArgument)
{
	static const uint8_t aucIp[4] = {
		APP_MILKTEA_TCP_IP_0, APP_MILKTEA_TCP_IP_1,
		APP_MILKTEA_TCP_IP_2, APP_MILKTEA_TCP_IP_3
	};
	TickType_t xNow;
	ModbusPortResult_e xResult;

	(void)pvArgument;
	vAppTaskManagerWaitNetworkStackReady();
	if (g_xMilkTeaTcpStatus.ucEnabled != 0U) {
		if (prvCreateEndpoint(&s_xMilkTeaEndpoint,
			&g_xMilkTeaTcpStatus, "milktea_tcp", aucIp,
			APP_MILKTEA_TCP_PORT) == TRANSPORT_RESULT_OK) {
			vMilkTeaProtocolInit(&s_xMilkTeaProtocol,
				&s_xMilkTeaEndpoint.xClient,
				APP_MILKTEA_TCP_UNIT_ID);
			prvRegisterMilkTeaDebug();
		}
	}
	for (;;) {
		xNow = xTaskGetTickCount();
		if ((g_xMilkTeaTcpStatus.ucEnabled != 0U) &&
			(s_xMilkTeaEndpoint.ucCreated != 0U)) {
			prvConnectionProcess(&s_xMilkTeaEndpoint,
				APP_COMM_SOURCE_MILKTEA_OPEN, xNow);
			if (g_xMilkTeaTcpStatus.xState ==
				APP_DEVICE_COMM_WAIT_SERVICE) {
				if (xSemaphoreTake(s_xMilkTeaEndpoint.xMutex,
					pdMS_TO_TICKS(APP_TCP_MUTEX_TIMEOUT_MS)) ==
					pdTRUE) {
					xResult = xMilkTeaProtocolReadStatus(
						&s_xMilkTeaProtocol,
						APP_MODBUS_TCP_IO_TIMEOUT_MS);
					(void)xSemaphoreGive(
						s_xMilkTeaEndpoint.xMutex);
					prvRecordResult(&g_xMilkTeaTcpStatus, xResult);
					if ((xResult == MODBUS_PORT_RESULT_OK) ||
						(xResult == MODBUS_PORT_RESULT_EXCEPTION)) {
						g_xMilkTeaTcpStatus.ulConsecutiveFailures =
							0U;
						prvSetState(&s_xMilkTeaEndpoint,
							APP_DEVICE_COMM_ONLINE);
						s_xMilkTeaEndpoint.xNextPollTick = xNow +
							pdMS_TO_TICKS(APP_MILKTEA_HEALTH_MS);
					} else {
						prvScheduleRetry(&s_xMilkTeaEndpoint,
							xNow, 1U);
					}
				}
			} else if ((g_xMilkTeaTcpStatus.xState ==
				APP_DEVICE_COMM_ONLINE) &&
				(prvTickReached(xNow,
				s_xMilkTeaEndpoint.xNextPollTick) != 0U)) {
				if (xSemaphoreTake(s_xMilkTeaEndpoint.xMutex, 0U) ==
					pdTRUE) {
					xResult = xMilkTeaProtocolReadStatus(
						&s_xMilkTeaProtocol,
						APP_MODBUS_TCP_IO_TIMEOUT_MS);
					(void)xSemaphoreGive(
						s_xMilkTeaEndpoint.xMutex);
					prvRecordResult(&g_xMilkTeaTcpStatus, xResult);
					vAppCommLogWrite(APP_COMM_SOURCE_MILKTEA_HEALTH,
						(int32_t)xResult,
						(xResult == MODBUS_PORT_RESULT_OK) ?
						(int32_t)g_xMilkTeaData.holdingRegisters[0] :
						s_xMilkTeaEndpoint.xTransport.lLastNativeError);
					s_xMilkTeaEndpoint.xNextPollTick = xNow +
						pdMS_TO_TICKS(APP_MILKTEA_HEALTH_MS);
					if (prvResultNeedsReconnect(xResult) != 0U) {
						prvScheduleRetry(&s_xMilkTeaEndpoint,
							xNow, 1U);
					}
				}
			}
		}
		vTaskDelay(pdMS_TO_TICKS(APP_MODBUS_TCP_LOOP_MS));
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvTickReached(TickType_t xNow, TickType_t xDeadline)
{
	return (((int32_t)(xNow - xDeadline)) >= 0) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static uint32_t prvRetryDelayMs(uint32_t ulFailureCount)
{
	static const uint32_t aulDelayMs[] = {
		1000U, 2000U, 5000U, 10000U, 30000U
	};
	uint32_t ulIndex;

	ulIndex = (ulFailureCount == 0U) ? 0U : ulFailureCount - 1U;
	if (ulIndex >= (sizeof(aulDelayMs) / sizeof(aulDelayMs[0]))) {
		ulIndex = (sizeof(aulDelayMs) / sizeof(aulDelayMs[0])) - 1U;
	}
	return aulDelayMs[ulIndex];
}

/*-----------------------------------------------------------*/
static TransportResult_e prvCreateEndpoint(AppTcpEndpoint_t *pxEndpoint,
	AppModbusTcpStatus_t *pxStatus, const char *pcName,
	const uint8_t *pucIp, uint16_t usPort)
{
	TransportTcpConfig_t xConfig;
	ModbusPortResult_e xPortResult;
	TransportResult_e xResult;

	memset(pxEndpoint, 0, sizeof(*pxEndpoint));
	memset(&xConfig, 0, sizeof(xConfig));
	// ==== 步骤A: 创建 Mutex（第295-299行） ====
	pxEndpoint->xMutex =
		xSemaphoreCreateMutexStatic(&pxEndpoint->xMutexStorage);
	if (pxEndpoint->xMutex == NULL) {
		return TRANSPORT_RESULT_NO_RESOURCE;
	}
	// ==== 步骤B: 配置 TCP 参数（第300-303行） ====
	xConfig.xMode = TRANSPORT_TCP_MODE_CLIENT;  		// 客户端模式
	memcpy(xConfig.aucRemoteIp, pucIp, sizeof(xConfig.aucRemoteIp));  // 服务器 IP
	xConfig.usPort = usPort; 									// 端口 502/1502
	xConfig.ulIoTimeoutMs = APP_MODBUS_TCP_IO_TIMEOUT_MS; 		// 1000ms
	 // ==== 步骤C: 创建 Transport TCP 通道（第304-305行） ====
	xResult = xTransportTcpCreate(&pxEndpoint->xChannel,
		&pxEndpoint->xTransport, pcName, &xConfig);
	// ==== 步骤D: 创建 Modbus 客户端（第307-309行） ====
	if (xResult == TRANSPORT_RESULT_OK) {
		xPortResult = xModbusPortClientInit(&pxEndpoint->xClient,
			&pxEndpoint->xChannel, MODBUS_PORT_TRANSPORT_TCP,
			APP_MODBUS_TCP_IO_TIMEOUT_MS);
		// ==== 步骤E: 绑定追踪缓冲区（第311-312行） ====
		if (xPortResult == MODBUS_PORT_RESULT_OK) {
			vModbusPortSetTrace(&pxEndpoint->xClient,
				&pxEndpoint->xTrace);
			pxEndpoint->pxStatus = pxStatus;
			pxEndpoint->ucCreated = 1U;  // 标记创建完成
			pxStatus->xState = APP_DEVICE_COMM_WAIT_NETWORK;
			pxStatus->xNextRetryTick = xTaskGetTickCount();
		} else {
			xResult = TRANSPORT_RESULT_IO_ERROR;
		}
	}
	return xResult;
}

/*-----------------------------------------------------------*/
static void prvConnectionProcess(AppTcpEndpoint_t *pxEndpoint,
	AppCommSource_e xOpenSource, TickType_t xNow)
{
	AppModbusTcpStatus_t *pxStatus;
	TransportResult_e xResult;

	pxStatus = pxEndpoint->pxStatus;
	if (ucAppTaskManagerIsNetworkReady() == 0U) {
		if ((pxStatus->ucConnected != 0U) ||
			(pxStatus->xState != APP_DEVICE_COMM_WAIT_NETWORK)) {
			prvCloseEndpoint(pxEndpoint);
			pxStatus->ulDisconnectCount++;
		}
		pxEndpoint->ucReconnectRequested = 0U;
		pxStatus->xNextRetryTick = xNow;
		prvSetState(pxEndpoint, APP_DEVICE_COMM_WAIT_NETWORK);
		return;
	}
	if ((pxEndpoint->ucReconnectRequested != 0U) ||
		((pxStatus->xState == APP_DEVICE_COMM_ONLINE) &&
		(xTransportGetState(&pxEndpoint->xChannel) !=
			TRANSPORT_STATE_OPEN))) {
		prvScheduleRetry(pxEndpoint, xNow, 1U);
		return;
	}
	if (pxStatus->xState == APP_DEVICE_COMM_WAIT_NETWORK) {
		prvSetState(pxEndpoint, APP_DEVICE_COMM_CONNECTING);
	}
	if (pxStatus->xState == APP_DEVICE_COMM_RETRY_DELAY) {
		if (prvTickReached(xNow, pxStatus->xNextRetryTick) == 0U) {
			return;
		}
		prvSetState(pxEndpoint, APP_DEVICE_COMM_CONNECTING);
	}
	if (pxStatus->xState != APP_DEVICE_COMM_CONNECTING) {
		return;
	}
	if (xSemaphoreTake(pxEndpoint->xMutex,
		pdMS_TO_TICKS(APP_TCP_MUTEX_TIMEOUT_MS)) != pdTRUE) {
		return;
	}
	if (xTransportGetState(&pxEndpoint->xChannel) !=
		TRANSPORT_STATE_CLOSED) {
		(void)xTransportClose(&pxEndpoint->xChannel);
	}
	pxStatus->ulConnectAttemptCount++;
	xResult = xTransportOpen(&pxEndpoint->xChannel);
	(void)xSemaphoreGive(pxEndpoint->xMutex);
	vAppCommLogWrite(xOpenSource, (int32_t)xResult,
		pxEndpoint->xTransport.lLastNativeError);
	if (xResult == TRANSPORT_RESULT_OK) {
		pxStatus->ucConnected = 1U;
		pxStatus->ulConnectSuccessCount++;
		pxEndpoint->ucReconnectRequested = 0U;
		prvSetState(pxEndpoint, APP_DEVICE_COMM_WAIT_SERVICE);
	} else {
		pxStatus->ulErrorCount++;
		prvScheduleRetry(pxEndpoint, xNow, 1U);
	}
}

/*-----------------------------------------------------------*/
static void prvSetState(AppTcpEndpoint_t *pxEndpoint,
	AppDeviceCommState_e xState)
{
	pxEndpoint->pxStatus->xState = xState;
}

/*-----------------------------------------------------------*/
static void prvScheduleRetry(AppTcpEndpoint_t *pxEndpoint,
	TickType_t xNow, uint8_t ucCountFailure)
{
	AppModbusTcpStatus_t *pxStatus;

	pxStatus = pxEndpoint->pxStatus;
	prvCloseEndpoint(pxEndpoint);
	if (ucCountFailure != 0U) {
		pxStatus->ulConsecutiveFailures++;
	}
	pxStatus->ulDisconnectCount++;
	pxStatus->xNextRetryTick = xNow + pdMS_TO_TICKS(
		prvRetryDelayMs(pxStatus->ulConsecutiveFailures));
	pxEndpoint->ucReconnectRequested = 0U;
	prvSetState(pxEndpoint, APP_DEVICE_COMM_RETRY_DELAY);
}

/*-----------------------------------------------------------*/
static void prvCloseEndpoint(AppTcpEndpoint_t *pxEndpoint)
{
	if (xSemaphoreTake(pxEndpoint->xMutex,
		pdMS_TO_TICKS(APP_TCP_MUTEX_TIMEOUT_MS)) == pdTRUE) {
		(void)xTransportClose(&pxEndpoint->xChannel);
		(void)xSemaphoreGive(pxEndpoint->xMutex);
	}
	pxEndpoint->pxStatus->ucConnected = 0U;
}

/*-----------------------------------------------------------*/
static void prvRecordResult(AppModbusTcpStatus_t *pxStatus,
	ModbusPortResult_e xResult)
{
	pxStatus->xLastResult = xResult;
	pxStatus->ulRequestCount++;
	if (xResult == MODBUS_PORT_RESULT_OK) {
		pxStatus->xLastSuccessTick = xTaskGetTickCount();
	} else {
		pxStatus->ulErrorCount++;
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvResultNeedsReconnect(ModbusPortResult_e xResult)
{
	return ucModbusPortResultIsLinkFailure(xResult);
}

/*-----------------------------------------------------------*/
static uint8_t prvDebugReady(void *pvContext)
{
	AppTcpEndpoint_t *pxEndpoint;

	pxEndpoint = (AppTcpEndpoint_t *)pvContext;
	if ((pxEndpoint == NULL) || (pxEndpoint->pxStatus == NULL)) {
		return 0U;
	}
	return ((pxEndpoint->pxStatus->xState == APP_DEVICE_COMM_ONLINE) &&
		(pxEndpoint->pxStatus->ucConnected != 0U) &&
		(pxEndpoint->ucReconnectRequested == 0U) &&
		(xTransportGetState(&pxEndpoint->xChannel) ==
		TRANSPORT_STATE_OPEN)) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static void prvDebugResult(ModbusPortResult_e xResult, void *pvContext)
{
	AppTcpEndpoint_t *pxEndpoint;

	pxEndpoint = (AppTcpEndpoint_t *)pvContext;
	if ((pxEndpoint == NULL) || (pxEndpoint->pxStatus == NULL)) {
		return;
	}
	prvRecordResult(pxEndpoint->pxStatus, xResult);
	if (prvResultNeedsReconnect(xResult) != 0U) {
		pxEndpoint->ucReconnectRequested = 1U;
	}
}

/*-----------------------------------------------------------*/
static void prvRegisterMilkTeaDebug(void)
{
	AppDebugTcpDevice_t xDevice;

	memset(&xDevice, 0, sizeof(xDevice));
	xDevice.pcName = "milktea";
	xDevice.pxClient = &s_xMilkTeaEndpoint.xClient;
	xDevice.pxTrace = &s_xMilkTeaEndpoint.xTrace;
	xDevice.xMutex = s_xMilkTeaEndpoint.xMutex;
	xDevice.pxIsReady = prvDebugReady;
	xDevice.pxResult = prvDebugResult;
	xDevice.pvContext = &s_xMilkTeaEndpoint;
	xDevice.ulFunctionMask = APP_DEBUG_FC01 | APP_DEBUG_FC03 |
		APP_DEBUG_FC06 | APP_DEBUG_FC16;
	xDevice.usCoilCount = MILKTEA_COIL_COUNT;
	xDevice.usHoldingCount = MILKTEA_HOLDING_COUNT;
	xDevice.usMaxWriteRegisters = MILKTEA_PRODUCT_LENGTH;
	(void)xAppDebugRegisterTcp(&xDevice);
}
