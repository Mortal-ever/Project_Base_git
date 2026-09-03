/**
  * @file      coffee2_server.c
  * @brief     Implement the Coffee2 multi-slot Modbus TCP server and model.
  * @author    WHong
  * @date      2026-07-30
  */

#include "coffee2_server.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "coffee2_manager.h"
#include "coffee2_app_config.h"
#include "coffee2_device.h"
#include "coffee2_device_image.h"
#include "coffee2_io.h"
#include "coffee2_log.h"
#include "coffee2_ota.h"
#include "coffee2_robot_tcp.h"
#include "coffee2_rtu_bus.h"
#include "coffee2_workflow.h"
#include "lwip/errno.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "main.h"
#include "modbus_port.h"
#include "nanomodbus.h"
#include "task.h"
#include "transport_tcp.h"

/** @brief Monitoring block base address. */
#define COFFEE2_SERVER_DEBUG_BASE             0x1100U
/** @brief Upgrade command register base and count. */
#define COFFEE2_SERVER_UPGRADE_BASE           0x0200U
#define COFFEE2_SERVER_UPGRADE_COUNT          3U
/** @brief Delay a requested software reset until the Modbus reply can leave. */
#define COFFEE2_SERVER_OTA_RESET_DELAY_MS      50U
/** @brief Maximum connection event text kept on the Server task stack. */
#define COFFEE2_SERVER_CONNECTION_EVENT_LENGTH 64U
/** @brief Removed IO debug command hole; writes are rejected. */
#define COFFEE2_SERVER_REMOVED_IO_DEBUG_FIRST  0x0084U
#define COFFEE2_SERVER_REMOVED_IO_DEBUG_LAST   0x0086U

/** @brief Reusable resources for one accepted socket slot. */
typedef struct {
	TransportChannel_t xChannel;
	TransportTcpSocketContext_t xTransport;
	ModbusPort_t xPort;
	uint8_t ucCreated;
	uint8_t ucActive;
} Coffee2ServerSlot_t;

COFFEE2_CCM_DATA
Coffee2ServerStatus_t g_xCoffee2ServerStatus;

COFFEE2_CCM_DATA
static uint16_t s_ausCommandRegisters[COFFEE2_SERVER_COMMAND_COUNT];
COFFEE2_CCM_DATA
static uint16_t s_ausStatusRegisters[COFFEE2_SERVER_STATUS_COUNT];
COFFEE2_CCM_DATA
static uint16_t s_ausUpgradeRegisters[COFFEE2_SERVER_UPGRADE_COUNT];
COFFEE2_CCM_DATA
static nmbs_callbacks s_xCallbacks;
COFFEE2_CCM_DATA
static uint8_t s_ucOrderLatched;
/** @brief Coalesce repeated Modbus write rejections during one OTA session. */
static uint8_t s_ucOtaRejectLogged;
/** @brief Defer the legacy 0x0201 reset to the Server owner loop. */
static uint8_t s_ucOtaResetPending;
/** @brief Tick captured when the legacy reset command is accepted. */
static TickType_t s_xOtaResetRequestTick;
/** @brief Workflow-owned output state for the two Coffee2 delivery ports. */
static uint16_t s_ausOutputState[2];

/**
  * @brief  Log one accepted client with its actual peer IPv4 endpoint.
  * @param[in] ucSlot Accepted Server slot index.
  * @param[in] ulIpv4 Peer IPv4 address in host byte order.
  * @param[in] usPort Peer source port in host byte order.
  */
static void prvLogClientConnected(uint8_t ucSlot, uint32_t ulIpv4,
	uint16_t usPort);

/**
  * @brief  读取命令区、状态区、监控区或升级镜像寄存器。
  * @param[in]  usAddress 起始寄存器地址。
  * @param[in]  usQuantity 连续读取的寄存器数量。
  * @param[out] pusRegisters nanoMODBUS 提供的寄存器输出缓冲区。
  * @param[in]  ucUnitId 请求中的 Unit ID。
  * @param[in]  pvArgument Server 回调上下文，当前实现未使用。
  * @retval NMBS_ERROR_NONE 读取成功。
  * @retval 其他 nmbs_error 地址、数量或协议错误。
  */
static nmbs_error prvReadHolding(uint16_t usAddress,
	uint16_t usQuantity, uint16_t *pusRegisters, uint8_t ucUnitId,
	void *pvArgument);
/**
  * @brief  接受一次 FC06 单寄存器写入。
  * @param[in] usAddress 目标寄存器地址。
  * @param[in] usValue 要写入的寄存器值。
  * @param[in] ucUnitId 请求中的 Unit ID。
  * @param[in] pvArgument Server 回调上下文，当前实现未使用。
  * @retval NMBS_ERROR_NONE 写入并触发命令评估成功。
  * @retval 其他 nmbs_error 地址、值或状态错误。
  */
static nmbs_error prvWriteSingle(uint16_t usAddress, uint16_t usValue,
	uint8_t ucUnitId, void *pvArgument);
/**
  * @brief  接受一次 FC10 多寄存器写入。
  * @param[in] usAddress 连续写入的起始地址。
  * @param[in] usQuantity 写入寄存器数量。
  * @param[in] pusRegisters nanoMODBUS 提供的输入寄存器数组。
  * @param[in] ucUnitId 请求中的 Unit ID。
  * @param[in] pvArgument Server 回调上下文，当前实现未使用。
  * @retval NMBS_ERROR_NONE 写入并触发命令评估成功。
  * @retval 其他 nmbs_error 地址、数量或状态错误。
  */
static nmbs_error prvWriteMultiple(uint16_t usAddress,
	uint16_t usQuantity, const uint16_t *pusRegisters,
	uint8_t ucUnitId, void *pvArgument);
/**
  * @brief  校验并提交一次连续命令空间写入。
  * @param[in] usAddress 起始命令寄存器地址。
  * @param[in] usQuantity 连续写入的寄存器数量。
  * @param[in] pusRegisters 待校验和复制的寄存器数据。
  * @retval NMBS_ERROR_NONE 写入成功。
  * @retval 其他 nmbs_error 地址范围或参数错误。
  */
static nmbs_error prvCommitWrite(uint16_t usAddress,
	uint16_t usQuantity, const uint16_t *pusRegisters);
/**
  * @brief  Commit and execute the legacy 0x0200 through 0x0202 controls.
  * @param[in] usAddress First upgrade register address.
  * @param[in] usQuantity Number of consecutive registers.
  * @param[in] pusRegisters Values supplied by nanoMODBUS.
  * @retval NMBS_ERROR_NONE The controls were accepted.
  * @retval NMBS_EXCEPTION_SERVER_DEVICE_FAILURE HTTP startup was not queued.
  */
static nmbs_error prvCommitUpgradeWrite(uint16_t usAddress,
	uint16_t usQuantity, const uint16_t *pusRegisters);
/**
  * @brief  检测原子寄存器提交后是否产生一份已核对的新订单。
  * @note   只有订单存在且核对寄存器为 1 时才向工作流队列提交。
  */
static void prvEvaluateOrder(void);
/**
  * @brief  将维护寄存器内容转换为设备标准命令。
  * @param[in] usAddress 本次写入的起始寄存器地址。
  * @param[in] usQuantity 本次写入覆盖的寄存器数量。
  */
static void prvEvaluateManualCommands(uint16_t usAddress,
	uint16_t usQuantity);
static nmbs_error prvCommitIoDebugWrite(uint16_t usAddress,
	uint16_t usValue);
static nmbs_error prvCommitIoDebugWriteRange(uint16_t usAddress,
	uint16_t usQuantity, const uint16_t *pusRegisters);
static uint16_t prvReadIoDebugValue(uint16_t usAddress);
/**
  * @brief  提交一个非阻塞维护设备命令。
  * @param[in] xDeviceId 目标逻辑设备。
  * @param[in] xAction 目标设备动作。
  * @param[in] usParameter0 动作参数 0。
  * @param[in] usParameter1 动作参数 1。
  * @retval 1 命令已进入目标路由队列。
  * @retval 0 当前工作流忙或队列无法接收命令。
  */
static uint8_t prvSubmitManual(Coffee2DeviceId_e xDeviceId,
	Coffee2Action_e xAction, uint16_t usParameter0,
	uint16_t usParameter1);
/**
  * @brief  将协议 0x0031 的位置值映射为 Robot 标准动作。
  * @param[in] usPosition 上位机写入的位置枚举值。
  * @retval 1 位置值已转换并提交。
  * @retval 0 位置未定义、超范围或命令队列不可用。
  */
static uint8_t prvSubmitRobotPosition(uint16_t usPosition);
/**
  * @brief  按当前任务和设备状态构造 0x1100 监控区。
  * @param[out] pusDebug 监控寄存器输出数组，容量必须为定义的监控区长度。
  */
static void prvBuildDebugRegisters(uint16_t *pusDebug);
/**
  * @brief  将实时设备镜像投影到协议规定的 0x1000 状态区。
  * @note   函数只更新内存镜像，不执行设备通讯。
  */
static void prvRefreshStatusRegisters(void);
/**
  * @brief  为配置的 Server 端口创建非阻塞监听 socket。
  * @retval 非负监听 socket 描述符。
  * @retval -1 创建、绑定或监听失败。
  */
static int prvCreateListener(void);
/**
  * @brief  关闭并清理一个 Server 客户端槽位。
  * @param[in,out] pxSlot 槽位 Transport、Modbus 和 socket 资源。
  * @param[in,out] pxStatus 槽位公开统计状态。
  * @param[in]     ucSlotIndex 槽位索引，用于日志标识。
  * @param[in]     lReason 关闭原因或规范化错误码。
  */
static void prvCloseSlot(Coffee2ServerSlot_t *pxSlot,
	Coffee2ServerClientStatus_t *pxStatus, uint8_t ucSlotIndex,
	int32_t lReason);
/**
  * @brief  重新统计活动客户端数量并刷新 Server 在线状态。
  * @note   任意一个有效槽位连接时 Server 即视为在线。
  */
static void prvUpdateActiveClientCount(void);

/*-----------------------------------------------------------*/
BaseType_t xCoffee2ServerInitialize(void)
{
	memset(&g_xCoffee2ServerStatus, 0,
		sizeof(g_xCoffee2ServerStatus));
	memset(s_ausCommandRegisters, 0,
		sizeof(s_ausCommandRegisters));
	memset(s_ausStatusRegisters, 0,
		sizeof(s_ausStatusRegisters));
	memset(s_ausUpgradeRegisters, 0,
		sizeof(s_ausUpgradeRegisters));
	nmbs_callbacks_create(&s_xCallbacks);
	s_xCallbacks.read_holding_registers = prvReadHolding;
	s_xCallbacks.read_input_registers = prvReadHolding;
	s_xCallbacks.write_single_register = prvWriteSingle;
	s_xCallbacks.write_multiple_registers = prvWriteMultiple;
	s_xCallbacks.arg = NULL;
	s_ucOrderLatched = 0U;
	s_ucOtaRejectLogged = 0U;
	s_ucOtaResetPending = 0U;
	s_xOtaResetRequestTick = 0U;
	memset(s_ausOutputState, 0, sizeof(s_ausOutputState));
	g_xCoffee2ServerStatus.usListenPort = COFFEE2_SERVER_PORT;
	return pdPASS;
}

/*-----------------------------------------------------------*/
static void prvLogClientConnected(uint8_t ucSlot, uint32_t ulIpv4,
	uint16_t usPort)
{
	static const char acPrefix[] = "SERVER_CLIENT_CONNECTED peer=";
	char acEvent[COFFEE2_SERVER_CONNECTION_EVENT_LENGTH];
	uint8_t aucIpv4[4];
	uint16_t usPrefixLength;
	const char *pcEvent;

	aucIpv4[0] = (uint8_t)(ulIpv4 >> 24);
	aucIpv4[1] = (uint8_t)(ulIpv4 >> 16);
	aucIpv4[2] = (uint8_t)(ulIpv4 >> 8);
	aucIpv4[3] = (uint8_t)ulIpv4;
	usPrefixLength = (uint16_t)(sizeof(acPrefix) - 1U);
	memcpy(acEvent, acPrefix, usPrefixLength);
	if (ucTransportTcpFormatIpv4Endpoint(aucIpv4, usPort,
		&acEvent[usPrefixLength],
		(uint16_t)(sizeof(acEvent) - usPrefixLength)) != 0U) {
		pcEvent = acEvent;
	} else {
		pcEvent = "SERVER_CLIENT_CONNECTED";
	}
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SERVER, pcEvent, 0, "slot",
		(int32_t)ucSlot);
}

/*-----------------------------------------------------------*/
void vCoffee2ServerTask(void *pvArgument)
{
	static const char * const
		apcSlotNames[COFFEE2_SERVER_MAX_CLIENTS] = {
		"coffee2_server_slot0", "coffee2_server_slot1",
		"coffee2_server_slot2", "coffee2_server_slot3"
	};
	Coffee2ServerSlot_t axSlots[COFFEE2_SERVER_MAX_CLIENTS];
	struct sockaddr_in xPeerAddress;
	socklen_t xPeerLength;
	fd_set xReadSet;
	struct timeval xTimeout;
	TransportResult_e xTransportResult;
	ModbusPortResult_e xResult;
	int lListener;
	int lAcceptedSocket;
	int lMaximumSocket;
	int lReady;
	int32_t lLastListenerError;
	uint8_t ucIndex;
	uint8_t ucFreeSlot;
	uint8_t ucListenerFailureLogged;
	uint8_t ucStackMarginLogged;

	(void)pvArgument;
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SERVER, "TASK_RUNNING:C2Server", 0);
	vAppTaskManagerWaitNetworkStackReady();
	memset(axSlots, 0, sizeof(axSlots));
	for (ucIndex = 0U; ucIndex < COFFEE2_SERVER_MAX_CLIENTS;
		ucIndex++) {
		axSlots[ucIndex].xTransport.lSocket = -1;
	}
	lListener = -1;
	lLastListenerError = 0;
	ucListenerFailureLogged = 0U;
	ucStackMarginLogged = 0U;
	(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
		COFFEE2_LOG_SOURCE_SERVER, "SERVER_OFFLINE", 0,
		"clients", 0);

	for (;;) {
		if ((s_ucOtaResetPending != 0U) &&
			((xTaskGetTickCount() - s_xOtaResetRequestTick) >=
			pdMS_TO_TICKS(COFFEE2_SERVER_OTA_RESET_DELAY_MS))) {
			(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_SERVER, "OTA_SOFT_RESET", 0);
			NVIC_SystemReset();
		}
		if (ucAppTaskManagerIsNetworkReady() == 0U) {
			if (lListener >= 0) {
				(void)lwip_close(lListener);
				lListener = -1;
				(void)xCoffee2LogWrite(
					COFFEE2_LOG_LEVEL_WARNING,
					COFFEE2_LOG_SOURCE_SERVER,
					"SERVER_LISTENER_CLOSED:NETWORK_DOWN", -1);
			}
			for (ucIndex = 0U; ucIndex < COFFEE2_SERVER_MAX_CLIENTS;
				ucIndex++) {
				prvCloseSlot(&axSlots[ucIndex],
					&g_xCoffee2ServerStatus.axClient[ucIndex],
					ucIndex, (int32_t)TRANSPORT_RESULT_DISCONNECTED);
			}
			g_xCoffee2ServerStatus.ucListening = 0U;
			ucListenerFailureLogged = 0U;
			prvUpdateActiveClientCount();
			vTaskDelay(pdMS_TO_TICKS(100U));
			continue;
		}
		if (lListener < 0) {
			lListener = prvCreateListener();
			if (lListener < 0) {
				g_xCoffee2ServerStatus.ulListenerErrorCount++;
				if ((ucListenerFailureLogged == 0U) ||
					(lLastListenerError != errno)) {
					lLastListenerError = errno;
					ucListenerFailureLogged = 1U;
						(void)xCoffee2LogWriteField(
							COFFEE2_LOG_LEVEL_ERROR,
							COFFEE2_LOG_SOURCE_SERVER,
							"SERVER_LISTEN_FAILED", -1,
							"native_error", errno);
						vCoffee2LogLwipResourceFailure(
							COFFEE2_LOG_SOURCE_SERVER, errno);
					}
				vTaskDelay(pdMS_TO_TICKS(1000U));
				continue;
			}
			ucListenerFailureLogged = 0U;
			g_xCoffee2ServerStatus.ucListening = 1U;
			(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_SERVER,
				"SERVER_LISTENING", 0, "port",
				(int32_t)COFFEE2_SERVER_PORT);
		}

		FD_ZERO(&xReadSet);
		FD_SET(lListener, &xReadSet);
		lMaximumSocket = lListener;
		for (ucIndex = 0U; ucIndex < COFFEE2_SERVER_MAX_CLIENTS;
			ucIndex++) {
			if (axSlots[ucIndex].ucActive != 0U) {
				FD_SET(axSlots[ucIndex].xTransport.lSocket,
					&xReadSet);
				if (axSlots[ucIndex].xTransport.lSocket >
					lMaximumSocket) {
					lMaximumSocket =
						axSlots[ucIndex].xTransport.lSocket;
				}
			}
		}
		xTimeout.tv_sec = 0;
		xTimeout.tv_usec = COFFEE2_SERVER_POLL_MS * 1000U;
		lReady = lwip_select(lMaximumSocket + 1, &xReadSet,
			NULL, NULL, &xTimeout);
			if (lReady < 0) {
				(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_ERROR,
					COFFEE2_LOG_SOURCE_SERVER, "SERVER_SELECT_FAILED",
					-1, "native_error", errno);
				vCoffee2LogLwipResourceFailure(
					COFFEE2_LOG_SOURCE_SERVER, errno);
				(void)lwip_close(lListener);
			lListener = -1;
			g_xCoffee2ServerStatus.ucListening = 0U;
			g_xCoffee2ServerStatus.ulListenerErrorCount++;
			continue;
		}

		if ((lReady > 0) && FD_ISSET(lListener, &xReadSet)) {
			xPeerLength = (socklen_t)sizeof(xPeerAddress);
			lAcceptedSocket = lwip_accept(lListener,
				(struct sockaddr *)&xPeerAddress, &xPeerLength);
				if (lAcceptedSocket >= 0) {
				(void)lwip_fcntl(lAcceptedSocket, F_SETFL,
					O_NONBLOCK);
				ucFreeSlot = COFFEE2_SERVER_MAX_CLIENTS;
				for (ucIndex = 0U;
					ucIndex < COFFEE2_SERVER_MAX_CLIENTS;
					ucIndex++) {
						if (axSlots[ucIndex].ucActive == 0U) {
							ucFreeSlot = ucIndex;
							break;
						}
					}
				if (ucFreeSlot >= COFFEE2_SERVER_MAX_CLIENTS) {
					(void)lwip_close(lAcceptedSocket);
					g_xCoffee2ServerStatus.ulRejectedCount++;
					(void)xCoffee2LogWriteField(
						COFFEE2_LOG_LEVEL_WARNING,
						COFFEE2_LOG_SOURCE_SERVER,
						"SERVER_CLIENT_REJECTED", -1,
						"remote_ipv4",
						(int32_t)ntohl(
							xPeerAddress.sin_addr.s_addr));
				} else {
					if (axSlots[ucFreeSlot].ucCreated == 0U) {
						xTransportResult =
							xTransportTcpSocketCreate(
								&axSlots[ucFreeSlot].xChannel,
								&axSlots[ucFreeSlot].xTransport,
								apcSlotNames[ucFreeSlot]);
						if (xTransportResult ==
							TRANSPORT_RESULT_OK) {
							axSlots[ucFreeSlot].ucCreated = 1U;
						}
					} else {
						xTransportResult = TRANSPORT_RESULT_OK;
					}
					if (xTransportResult == TRANSPORT_RESULT_OK) {
						xTransportResult =
							xTransportTcpSocketAttach(
								&axSlots[ucFreeSlot].xChannel,
								&axSlots[ucFreeSlot].xTransport,
								lAcceptedSocket);
					}
					if (xTransportResult == TRANSPORT_RESULT_OK) {
						xResult = xModbusPortServerInit(
							&axSlots[ucFreeSlot].xPort,
							&axSlots[ucFreeSlot].xChannel,
							MODBUS_PORT_TRANSPORT_TCP,
							COFFEE2_SERVER_UNIT_ID,
							&s_xCallbacks,
							COFFEE2_SERVER_BYTE_TIMEOUT_MS);
					} else {
						xResult = MODBUS_PORT_RESULT_TRANSPORT;
					}
					if (xResult == MODBUS_PORT_RESULT_OK) {
						axSlots[ucFreeSlot].ucActive = 1U;
						g_xCoffee2ServerStatus.axClient[
							ucFreeSlot].ucConnected = 1U;
						g_xCoffee2ServerStatus.axClient[
							ucFreeSlot].ulRemoteIpv4 =
							(uint32_t)ntohl(
								xPeerAddress.sin_addr.s_addr);
						g_xCoffee2ServerStatus.axClient[
							ucFreeSlot].usRemotePort =
							(uint16_t)ntohs(
								xPeerAddress.sin_port);
						g_xCoffee2ServerStatus.axClient[
							ucFreeSlot].ulLastActivityTick =
							(uint32_t)xTaskGetTickCount();
						g_xCoffee2ServerStatus.ulAcceptedCount++;
						prvLogClientConnected(ucFreeSlot,
							g_xCoffee2ServerStatus.axClient[
								ucFreeSlot].ulRemoteIpv4,
							g_xCoffee2ServerStatus.axClient[
								ucFreeSlot].usRemotePort);
					} else {
						if (axSlots[ucFreeSlot].xTransport.lSocket <
							0) {
							(void)lwip_close(lAcceptedSocket);
						}
						prvCloseSlot(&axSlots[ucFreeSlot],
							&g_xCoffee2ServerStatus.axClient[
								ucFreeSlot], ucFreeSlot,
							(int32_t)xResult);
						g_xCoffee2ServerStatus.ulRejectedCount++;
						(void)xCoffee2LogWriteField(
							COFFEE2_LOG_LEVEL_ERROR,
							COFFEE2_LOG_SOURCE_SERVER,
							"SERVER_SLOT_INIT_FAILED",
							(int32_t)xResult,
							"slot", (int32_t)ucFreeSlot);
					}
				}
				prvUpdateActiveClientCount();
			} else if ((errno != EWOULDBLOCK) && (errno != EAGAIN)) {
				(void)xCoffee2LogWriteField(
					COFFEE2_LOG_LEVEL_ERROR,
					COFFEE2_LOG_SOURCE_SERVER,
					"SERVER_ACCEPT_FAILED", -1,
					"native_error", errno);
				vCoffee2LogLwipResourceFailure(
					COFFEE2_LOG_SOURCE_SERVER, errno);
			}
		}

		for (ucIndex = 0U; ucIndex < COFFEE2_SERVER_MAX_CLIENTS;
			ucIndex++) {
			if ((axSlots[ucIndex].ucActive == 0U) ||
				(lReady <= 0) ||
				(!FD_ISSET(axSlots[ucIndex].xTransport.lSocket,
					&xReadSet))) {
				continue;
			}
			xResult = xModbusPortServerPoll(
				&axSlots[ucIndex].xPort,
				COFFEE2_SERVER_POLL_MS);
			g_xCoffee2ServerStatus.axClient[ucIndex].lLastResult =
				(int32_t)xResult;
			g_xCoffee2ServerStatus.axClient[ucIndex].
				ulRequestCount++;
			g_xCoffee2ServerStatus.axClient[ucIndex].
				ulLastActivityTick = (uint32_t)xTaskGetTickCount();
			if (xResult == MODBUS_PORT_RESULT_EXCEPTION) {
				(void)xCoffee2LogWriteField(
					COFFEE2_LOG_LEVEL_WARNING,
					COFFEE2_LOG_SOURCE_SERVER,
					"SERVER_REQUEST_EXCEPTION",
					(int32_t)xResult, "slot", (int32_t)ucIndex);
			}
			if ((xResult == MODBUS_PORT_RESULT_OK) &&
				(ucStackMarginLogged == 0U)) {
				ucStackMarginLogged = 1U;
				(void)xCoffee2LogWriteField(
					COFFEE2_LOG_LEVEL_INFO,
					COFFEE2_LOG_SOURCE_SERVER,
					"SERVER_STACK_MARGIN", 0, "hwm_words",
					(int32_t)uxTaskGetStackHighWaterMark(NULL));
			}
			if ((xResult != MODBUS_PORT_RESULT_OK) &&
				(xResult != MODBUS_PORT_RESULT_EXCEPTION) &&
				(xResult != MODBUS_PORT_RESULT_TIMEOUT)) {
				g_xCoffee2ServerStatus.axClient[ucIndex].
					ulErrorCount++;
				prvCloseSlot(&axSlots[ucIndex],
					&g_xCoffee2ServerStatus.axClient[ucIndex],
					ucIndex, (int32_t)xResult);
				prvUpdateActiveClientCount();
			}
		}
	}
}

/*-----------------------------------------------------------*/
void vCoffee2ServerPublishWorkflow(uint16_t usOrderId,
	uint16_t usProductionStatus, uint16_t usStep, int32_t lError)
{
	taskENTER_CRITICAL();
	s_ausStatusRegisters[COFFEE2_REG_ORDER_NUMBER -
		COFFEE2_REG_ORDER_NUMBER] = usOrderId;
	s_ausStatusRegisters[COFFEE2_REG_PRODUCTION_STATUS -
		COFFEE2_REG_STATUS_BASE] = usProductionStatus;
	s_ausStatusRegisters[COFFEE2_REG_WORKFLOW_STEP -
		COFFEE2_REG_STATUS_BASE] = usStep;
	s_ausStatusRegisters[COFFEE2_REG_WORKFLOW_ERROR -
		COFFEE2_REG_STATUS_BASE] = (uint16_t)lError;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
void vCoffee2ServerPublishOutput(uint16_t usOutput, uint16_t usState)
{
	if ((usOutput < 1U) || (usOutput > 2U)) {
		return;
	}
	taskENTER_CRITICAL();
	s_ausOutputState[usOutput - 1U] = usState;
	if ((usState == 2U) || (usState == 5U)) {
		s_ausStatusRegisters[0x0009U] = usOutput;
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
uint16_t usCoffee2ServerGetCommandRegister(uint16_t usAddress)
{
	uint16_t usValue;

	if (usAddress >= COFFEE2_SERVER_COMMAND_COUNT) {
		return 0U;
	}
	taskENTER_CRITICAL();
	usValue = s_ausCommandRegisters[usAddress];
	taskEXIT_CRITICAL();
	return usValue;
}

/*-----------------------------------------------------------*/
static nmbs_error prvReadHolding(uint16_t usAddress,
	uint16_t usQuantity, uint16_t *pusRegisters, uint8_t ucUnitId,
	void *pvArgument)
{
	uint16_t ausDebug[COFFEE2_SERVER_DEBUG_COUNT];
	uint16_t usIndex;

	(void)ucUnitId;
	(void)pvArgument;
	if ((pusRegisters == NULL) || (usQuantity == 0U)) {
		return NMBS_EXCEPTION_ILLEGAL_DATA_VALUE;
	}
	if (((uint32_t)usAddress + usQuantity) <=
		COFFEE2_SERVER_COMMAND_COUNT) {
		taskENTER_CRITICAL();
		memcpy(pusRegisters, &s_ausCommandRegisters[usAddress],
			(size_t)usQuantity * sizeof(uint16_t));
		taskEXIT_CRITICAL();
		return NMBS_ERROR_NONE;
	}
	if ((usAddress >= COFFEE2_REG_STATUS_BASE) &&
		((uint32_t)usAddress + usQuantity <=
			(uint32_t)COFFEE2_REG_STATUS_BASE +
			COFFEE2_SERVER_STATUS_COUNT)) {
		prvRefreshStatusRegisters();
		usIndex = (uint16_t)(usAddress -
			COFFEE2_REG_STATUS_BASE);
		taskENTER_CRITICAL();
		memcpy(pusRegisters, &s_ausStatusRegisters[usIndex],
			(size_t)usQuantity * sizeof(uint16_t));
		taskEXIT_CRITICAL();
		return NMBS_ERROR_NONE;
	}
	if ((usAddress >= COFFEE2_SERVER_DEBUG_BASE) &&
		((uint32_t)usAddress + usQuantity <=
			(uint32_t)COFFEE2_SERVER_DEBUG_BASE +
			COFFEE2_SERVER_DEBUG_COUNT)) {
		prvBuildDebugRegisters(ausDebug);
		memcpy(pusRegisters,
			&ausDebug[usAddress - COFFEE2_SERVER_DEBUG_BASE],
			(size_t)usQuantity * sizeof(uint16_t));
		return NMBS_ERROR_NONE;
	}
	if ((usAddress >= COFFEE2_REG_LOCAL_IO_DEBUG) &&
		((uint32_t)usAddress + usQuantity <=
			(uint32_t)COFFEE2_REG_EXTERNAL_IO_DEBUG + 1U)) {
		for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
			pusRegisters[usIndex] = prvReadIoDebugValue(
				(uint16_t)(usAddress + usIndex));
		}
		return NMBS_ERROR_NONE;
	}
	if ((usAddress >= COFFEE2_SERVER_UPGRADE_BASE) &&
		((uint32_t)usAddress + usQuantity <=
			(uint32_t)COFFEE2_SERVER_UPGRADE_BASE +
			COFFEE2_SERVER_UPGRADE_COUNT)) {
		memcpy(pusRegisters,
			&s_ausUpgradeRegisters[
				usAddress - COFFEE2_SERVER_UPGRADE_BASE],
			(size_t)usQuantity * sizeof(uint16_t));
		return NMBS_ERROR_NONE;
	}
	return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
}

/*-----------------------------------------------------------*/
static nmbs_error prvCommitIoDebugWrite(uint16_t usAddress,
	uint16_t usValue)
{
	Coffee2IoState_t xIoSnapshot;
	uint16_t usCurrent;
	uint16_t usChanged;
	uint8_t ucIndex;
	uint8_t ucValue;

	if (usAddress == COFFEE2_REG_LOCAL_IO_DEBUG) {
		if ((usValue & 0xFF00U) != 0U) {
			(void)xCoffee2LogPrintfOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
				"LOCAL_IO_DEBUG_INVALID_VALUE=0x%04X", usValue);
			return NMBS_EXCEPTION_ILLEGAL_DATA_VALUE;
		}
		for (ucIndex = 0U; ucIndex < COFFEE2_LOCAL_IO_COUNT;
			ucIndex++) {
			ucValue = ((usValue & (uint16_t)(1U << ucIndex)) != 0U) ?
				1U : 0U;
			if (ucCoffee2IoSetLocalOutput(ucIndex, ucValue) == 0U) {
				return NMBS_EXCEPTION_SERVER_DEVICE_FAILURE;
			}
		}
		(void)xCoffee2LogPrintfOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
			"LOCAL_IO_DEBUG_OUTPUT_SET=0x%04X", usValue);
		return NMBS_ERROR_NONE;
	}

	vCoffee2IoGetSnapshot(&xIoSnapshot);
	usCurrent = 0U;
	for (ucIndex = 0U; ucIndex < COFFEE2_MODBUS_IO_COUNT;
		ucIndex++) {
		if (xIoSnapshot.xOutput.aucMB2YPin[ucIndex] != 0U) {
			usCurrent |= (uint16_t)(1U << ucIndex);
		}
	}
	usChanged = (uint16_t)(usCurrent ^ usValue);
	(void)xCoffee2LogPrintfOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
		"REMOTE_IO_DEBUG_OUTPUT_REQUEST=0x%04X", usValue);
	for (ucIndex = 0U; ucIndex < COFFEE2_MODBUS_IO_COUNT;
		ucIndex++) {
		if ((usChanged & (uint16_t)(1U << ucIndex)) != 0U) {
			if (prvSubmitManual(COFFEE2_DEVICE_IO_OUTPUT,
				COFFEE2_ACTION_IO_WRITE, ucIndex,
				((usValue & (uint16_t)(1U << ucIndex)) != 0U) ? 1U : 0U) == 0U) {
				return NMBS_EXCEPTION_SERVER_DEVICE_FAILURE;
			}
		}
	}
	return NMBS_ERROR_NONE;
}

/*-----------------------------------------------------------*/
static uint16_t prvReadIoDebugValue(uint16_t usAddress)
{
	Coffee2IoState_t xIoSnapshot;
	uint16_t usValue;
	uint8_t ucIndex;

	if ((usAddress != COFFEE2_REG_LOCAL_IO_DEBUG) &&
		(usAddress != COFFEE2_REG_EXTERNAL_IO_DEBUG)) {
		return 0U;
	}
	if (usAddress == COFFEE2_REG_LOCAL_IO_DEBUG) {
		vCoffee2IoRefreshLocal();
	}
	vCoffee2IoGetSnapshot(&xIoSnapshot);
	usValue = 0U;
	if (usAddress == COFFEE2_REG_LOCAL_IO_DEBUG) {
		for (ucIndex = 0U; ucIndex < COFFEE2_LOCAL_IO_COUNT; ucIndex++) {
			if (xIoSnapshot.xOutput.aucYPin[ucIndex] != 0U) {
				usValue |= (uint16_t)(1U << ucIndex);
			}
		}
	} else {
		for (ucIndex = 0U; ucIndex < COFFEE2_MODBUS_IO_COUNT; ucIndex++) {
			if (xIoSnapshot.xOutput.aucMB2YPin[ucIndex] != 0U) {
				usValue |= (uint16_t)(1U << ucIndex);
			}
		}
	}
	return usValue;
}

/*-----------------------------------------------------------*/
static nmbs_error prvCommitIoDebugWriteRange(uint16_t usAddress,
	uint16_t usQuantity, const uint16_t *pusRegisters)
{
	uint16_t usIndex;
	nmbs_error xResult;

	if ((pusRegisters == NULL) || (usQuantity == 0U) ||
		((uint32_t)usAddress + usQuantity >
			(uint32_t)COFFEE2_REG_EXTERNAL_IO_DEBUG + 1U)) {
		return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
	}
	for (usIndex = 0U; usIndex < usQuantity; usIndex++) {
		xResult = prvCommitIoDebugWrite(
			(uint16_t)(usAddress + usIndex), pusRegisters[usIndex]);
		if (xResult != NMBS_ERROR_NONE) {
			return xResult;
		}
	}
	return NMBS_ERROR_NONE;
}

/*-----------------------------------------------------------*/
static nmbs_error prvWriteSingle(uint16_t usAddress, uint16_t usValue,
	uint8_t ucUnitId, void *pvArgument)
{
	(void)ucUnitId;
	(void)pvArgument;
	return prvCommitWrite(usAddress, 1U, &usValue);
}

/*-----------------------------------------------------------*/
static nmbs_error prvWriteMultiple(uint16_t usAddress,
	uint16_t usQuantity, const uint16_t *pusRegisters,
	uint8_t ucUnitId, void *pvArgument)
{
	(void)ucUnitId;
	(void)pvArgument;
	return prvCommitWrite(usAddress, usQuantity, pusRegisters);
}

/*-----------------------------------------------------------*/
static nmbs_error prvCommitWrite(uint16_t usAddress,
	uint16_t usQuantity, const uint16_t *pusRegisters)
{
	uint32_t ulEndAddress;

	if ((pusRegisters == NULL) || (usQuantity == 0U)) {
		return NMBS_EXCEPTION_ILLEGAL_DATA_VALUE;
	}
	ulEndAddress = (uint32_t)usAddress + usQuantity;
	if (((uint32_t)usAddress <= COFFEE2_SERVER_REMOVED_IO_DEBUG_LAST) &&
		(ulEndAddress > COFFEE2_SERVER_REMOVED_IO_DEBUG_FIRST)) {
		return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
	}
	if ((usAddress >= COFFEE2_REG_LOCAL_IO_DEBUG) &&
		(ulEndAddress <=
			(uint32_t)COFFEE2_REG_EXTERNAL_IO_DEBUG + 1U)) {
		return prvCommitIoDebugWriteRange(usAddress, usQuantity,
			pusRegisters);
	}
	if ((usAddress >= COFFEE2_SERVER_UPGRADE_BASE) &&
		((uint32_t)usAddress + usQuantity <=
			(uint32_t)COFFEE2_SERVER_UPGRADE_BASE +
			COFFEE2_SERVER_UPGRADE_COUNT)) {
		return prvCommitUpgradeWrite(usAddress, usQuantity,
			pusRegisters);
	}
	if (ucCoffee2OtaHttpIsActive() != 0U) {
		if (s_ucOtaRejectLogged == 0U) {
			s_ucOtaRejectLogged = 1U;
			(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_SERVER, "OTA_HTTP_REJECTED", -8);
		}
		return NMBS_EXCEPTION_ILLEGAL_DATA_VALUE;
	}
	s_ucOtaRejectLogged = 0U;
	if (((uint32_t)usAddress + usQuantity) <=
		COFFEE2_SERVER_COMMAND_COUNT) {
		taskENTER_CRITICAL();
		memcpy(&s_ausCommandRegisters[usAddress], pusRegisters,
			(size_t)usQuantity * sizeof(uint16_t));
		taskEXIT_CRITICAL();
		prvEvaluateOrder();
		prvEvaluateManualCommands(usAddress, usQuantity);
		return NMBS_ERROR_NONE;
	}
	return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
}

/*-----------------------------------------------------------*/
static nmbs_error prvCommitUpgradeWrite(uint16_t usAddress,
	uint16_t usQuantity, const uint16_t *pusRegisters)
{
	uint32_t ulEndAddress;
	uint16_t usValue;
	BaseType_t xResult;

	memcpy(&s_ausUpgradeRegisters[
		usAddress - COFFEE2_SERVER_UPGRADE_BASE], pusRegisters,
		(size_t)usQuantity * sizeof(uint16_t));
	ulEndAddress = (uint32_t)usAddress + usQuantity;
	if ((usAddress <= COFFEE2_SERVER_UPGRADE_BASE) &&
		(ulEndAddress > COFFEE2_SERVER_UPGRADE_BASE)) {
		usValue = pusRegisters[COFFEE2_SERVER_UPGRADE_BASE - usAddress];
		if (usValue == 1U) {
			xResult = xCoffee2OtaHttpInitialize();
			(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_SERVER, "OTA_HTTP_TRIGGER",
				(int32_t)xResult);
			if (xResult != pdPASS) {
				return NMBS_EXCEPTION_SERVER_DEVICE_FAILURE;
			}
		}
	}
	if ((usAddress <= (COFFEE2_SERVER_UPGRADE_BASE + 1U)) &&
		(ulEndAddress > (COFFEE2_SERVER_UPGRADE_BASE + 1U))) {
		usValue = pusRegisters[(COFFEE2_SERVER_UPGRADE_BASE + 1U) -
			usAddress];
		if (usValue == 1U) {
			s_xOtaResetRequestTick = xTaskGetTickCount();
			s_ucOtaResetPending = 1U;
			(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_SERVER,
				"OTA_SOFT_RESET_SCHEDULED", 0);
		}
	}
	if ((usAddress <= (COFFEE2_SERVER_UPGRADE_BASE + 2U)) &&
		(ulEndAddress > (COFFEE2_SERVER_UPGRADE_BASE + 2U))) {
		usValue = pusRegisters[(COFFEE2_SERVER_UPGRADE_BASE + 2U) -
			usAddress];
		if (usValue == 1U) {
			(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_SERVER,
				COFFEE2_DEVICE_VERSION_EVENT, 0);
		}
	}
	return NMBS_ERROR_NONE;
}

/*-----------------------------------------------------------*/
static void prvEvaluateOrder(void)
{
	Coffee2Order_t xOrder;
	BaseType_t xResult;
	uint16_t usIndex;

	if (s_ausCommandRegisters[COFFEE2_REG_ORDER_PRESENT] == 0U) {
		s_ucOrderLatched = 0U;
		return;
	}
	if ((s_ucOrderLatched != 0U) ||
		(s_ausCommandRegisters[COFFEE2_REG_ORDER_VERIFIED] != 1U)) {
		return;
	}
	taskENTER_CRITICAL();
	for (usIndex = 0U; usIndex < COFFEE2_ORDER_REGISTER_COUNT;
		usIndex++) {
		xOrder.ausRegister[usIndex] =
			s_ausCommandRegisters[usIndex];
	}
	taskEXIT_CRITICAL();
	if ((xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER] ==
			COFFEE2_LOG_ORDER_SYSTEM) ||
		(xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER] ==
			COFFEE2_LOG_ORDER_DEBUG)) {
		s_ucOrderLatched = 1U;
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER,
			xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER],
			"ORDER_REJECTED_RESERVED_ID", -2, "order",
			(int32_t)xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER]);
		return;
	}
	xResult = xCoffee2WorkflowSubmitOrder(&xOrder);
	if (xResult == pdPASS) {
		s_ucOrderLatched = 1U;
		taskENTER_CRITICAL();
		memset(s_ausOutputState, 0, sizeof(s_ausOutputState));
		/* Only fields defined by the Coffee2 status protocol are copied. */
		s_ausStatusRegisters[0x0000U] =
			xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER];
		s_ausStatusRegisters[0x0001U] =
			xOrder.ausRegister[COFFEE2_REG_COFFEE_TYPE];
		s_ausStatusRegisters[0x0002U] =
			xOrder.ausRegister[COFFEE2_REG_LID_ENABLE];
		s_ausStatusRegisters[0x0003U] =
			xOrder.ausRegister[COFFEE2_REG_SYRUP_1];
		s_ausStatusRegisters[0x0004U] =
			xOrder.ausRegister[COFFEE2_REG_SYRUP_2];
		s_ausStatusRegisters[0x0005U] =
			xOrder.ausRegister[COFFEE2_REG_ICE_AMOUNT];
		s_ausStatusRegisters[0x0006U] = 0U;
		s_ausStatusRegisters[0x0007U] = 0U;
		s_ausStatusRegisters[0x0009U] = 0U;
		s_ausStatusRegisters[0x000AU] = 0U;
		s_ausStatusRegisters[0x000BU] = 0U;
		s_ausStatusRegisters[0x000CU] = 0U;
		s_ausStatusRegisters[0x000DU] =
			xOrder.ausRegister[COFFEE2_REG_ONLINE_OUTPUT];
		s_ausStatusRegisters[0x000EU] =
			xOrder.ausRegister[COFFEE2_REG_FRUIT_MILK_A];
		s_ausStatusRegisters[0x000FU] =
			xOrder.ausRegister[COFFEE2_REG_FRUIT_MILK_B];
		s_ausStatusRegisters[0x0014U] =
			xOrder.ausRegister[COFFEE2_REG_SYRUP_3];
		s_ausStatusRegisters[0x0015U] =
			xOrder.ausRegister[COFFEE2_REG_SYRUP_4];
		s_ausStatusRegisters[COFFEE2_REG_PRODUCTION_STATUS -
			COFFEE2_REG_STATUS_BASE] = COFFEE2_PRODUCTION_RUNNING;
		taskEXIT_CRITICAL();
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_SERVER,
			xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER],
			"ORDER_ACCEPTED", 0,
			"order", (int32_t)xOrder.ausRegister[
				COFFEE2_REG_ORDER_NUMBER]);
	} else {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER,
			xOrder.ausRegister[COFFEE2_REG_ORDER_NUMBER],
			"ORDER_REJECTED_BUSY", -1,
			"order", (int32_t)xOrder.ausRegister[
				COFFEE2_REG_ORDER_NUMBER]);
	}
}

/*-----------------------------------------------------------*/
static void prvEvaluateManualCommands(uint16_t usAddress,
	uint16_t usQuantity)
{
	uint32_t ulEndAddress;
	uint16_t usValue;
	uint8_t ucAccepted;

	ulEndAddress = (uint32_t)usAddress + usQuantity;
	if ((usAddress <= COFFEE2_REG_PICKUP_CONFIRM) &&
		(ulEndAddress > COFFEE2_REG_PICKUP_CONFIRM)) {
		usValue = s_ausCommandRegisters[COFFEE2_REG_PICKUP_CONFIRM];
		if (usValue == 0x0001U) {
			vCoffee2ServerPublishOutput(1U, 0x0010U);
		} else if (usValue == 0x0010U) {
			vCoffee2ServerPublishOutput(2U, 0x0010U);
		}
		s_ausCommandRegisters[COFFEE2_REG_PICKUP_CONFIRM] = 0U;
	}
	if ((usAddress <= COFFEE2_REG_CANCEL_ORDER) &&
		(ulEndAddress > COFFEE2_REG_CANCEL_ORDER) &&
		(s_ausCommandRegisters[COFFEE2_REG_CANCEL_ORDER] != 0U)) {
		vCoffee2WorkflowRequestCancel();
		s_ausCommandRegisters[COFFEE2_REG_CANCEL_ORDER] = 0U;
	}
	if ((usAddress <= COFFEE2_REG_CLEAR_ALARM) &&
		(ulEndAddress > COFFEE2_REG_CLEAR_ALARM) &&
		(s_ausCommandRegisters[COFFEE2_REG_CLEAR_ALARM] != 0U)) {
		vCoffee2WorkflowAcknowledgeAlarm();
		(void)prvSubmitManual(COFFEE2_DEVICE_ROBOT,
			COFFEE2_ACTION_ROBOT_CLEAR_ALARM, 0U, 0U);
		(void)prvSubmitManual(COFFEE2_DEVICE_COFFEE_MACHINE,
			COFFEE2_ACTION_RESET, 0U, 0U);
		s_ausCommandRegisters[COFFEE2_REG_CLEAR_ALARM] = 0U;
	}
	if ((usAddress <= 0x0030U) && (ulEndAddress > 0x0030U)) {
		usValue = s_ausCommandRegisters[0x0030U];
		ucAccepted = 0U;
		switch (usValue) {
		case 0U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_START, 0U, 0U);
			break;
		case 1U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_STOP, 0U, 0U);
			break;
		case 2U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_PAUSE, 0U, 0U);
			break;
		case 3U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_ENABLE, 0U, 0U);
			break;
		case 4U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_DISABLE, 0U, 0U);
			break;
		case 5U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_CLEAR_ALARM, 0U, 0U);
			break;
		case 6U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_ENTER_DRAG, 0U, 0U);
			break;
		case 7U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_EXIT_DRAG, 0U, 0U);
			break;
		case 8U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_AUTO_MODE, 0U, 0U);
			break;
		case 9U:
			ucAccepted = prvSubmitManual(COFFEE2_DEVICE_ROBOT,
				COFFEE2_ACTION_ROBOT_MANUAL_MODE, 0U, 0U);
			break;
		default:
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
				"MANUAL_ROBOT_CONTROL_UNSUPPORTED", -1,
				"value", (int32_t)usValue);
			break;
		}
		if (ucAccepted != 0U) {
			s_ausCommandRegisters[0x0030U] = 0U;
		}
	}
	if ((usAddress <= 0x0031U) && (ulEndAddress > 0x0031U)) {
		usValue = s_ausCommandRegisters[0x0031U];
		if (prvSubmitRobotPosition(usValue) != 0U) {
			s_ausCommandRegisters[0x0031U] = 0U;
		}
	}
	if ((usAddress <= 0x0040U) && (ulEndAddress > 0x0040U) &&
		(s_ausCommandRegisters[0x0040U] <= COFFEE2_COFFEE_RECIPE_MAX) &&
		(prvSubmitManual(COFFEE2_DEVICE_COFFEE_MACHINE,
			COFFEE2_ACTION_COFFEE_MAKE,
			s_ausCommandRegisters[0x0040U], 0U) != 0U)) {
		s_ausCommandRegisters[0x0040U] = 0U;
	}
	if ((usAddress <= 0x0042U) && (ulEndAddress > 0x0042U) &&
		(s_ausCommandRegisters[0x0042U] >= 1U) &&
		(s_ausCommandRegisters[0x0042U] <= 6U) &&
		(xCoffee2WorkflowSubmitMaintenance(
			COFFEE2_MAINTENANCE_COFFEE_CLEAN,
			s_ausCommandRegisters[0x0042U], 0U) == pdPASS)) {
		s_ausCommandRegisters[0x0042U] = 0U;
	}
	if ((usAddress <= 0x0042U) && (ulEndAddress > 0x0042U) &&
		(s_ausCommandRegisters[0x0042U] > 6U)) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
			"COFFEE_CLEAN_UNSUPPORTED",
			-1, "value", (int32_t)s_ausCommandRegisters[0x0042U]);
	}
	if ((usAddress <= 0x0047U) && (ulEndAddress > 0x0047U) &&
		(s_ausCommandRegisters[0x0047U] != 0U) &&
		(prvSubmitManual(COFFEE2_DEVICE_COFFEE_MACHINE,
			COFFEE2_ACTION_CANCEL, 0U, 0U) != 0U)) {
		s_ausCommandRegisters[0x0047U] = 0U;
	}
	if ((usAddress <= 0x0050U) && (ulEndAddress > 0x0050U)) {
		usValue = s_ausCommandRegisters[0x0050U];
		if ((usValue == 0U) &&
			(prvSubmitManual(COFFEE2_DEVICE_CUP_MACHINE,
				COFFEE2_ACTION_CUP_DROP_2, 0U, 0U) != 0U)) {
			s_ausCommandRegisters[0x0050U] = 0U;
		} else if ((usValue == 1U) &&
			(prvSubmitManual(COFFEE2_DEVICE_CUP_MACHINE,
				COFFEE2_ACTION_CUP_DROP_1, 0U, 0U) != 0U)) {
			s_ausCommandRegisters[0x0050U] = 0U;
		} else if ((usValue == 2U) &&
			(prvSubmitManual(COFFEE2_DEVICE_LID_MACHINE,
				COFFEE2_ACTION_LID_DROP_2, 0U, 0U) != 0U)) {
			s_ausCommandRegisters[0x0050U] = 0U;
		} else if ((usValue == 3U) &&
			(prvSubmitManual(COFFEE2_DEVICE_LID_MACHINE,
				COFFEE2_ACTION_LID_DROP_1, 0U, 0U) != 0U)) {
			s_ausCommandRegisters[0x0050U] = 0U;
		} else if (usValue > 3U) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
				"CUP_LID_COMMAND_UNSUPPORTED", -1,
				"value", (int32_t)usValue);
		}
	}
	if ((usAddress <= 0x0060U) && (ulEndAddress > 0x0060U) &&
		(s_ausCommandRegisters[0x0060U] <= 3U) &&
		(prvSubmitManual(COFFEE2_DEVICE_SYRUP_MACHINE,
			COFFEE2_ACTION_SYRUP_DISPENSE,
			(uint16_t)(s_ausCommandRegisters[0x0060U] + 1U),
			s_ausCommandRegisters[0x0061U]) != 0U)) {
		s_ausCommandRegisters[0x0060U] = 0U;
	}
	if ((usAddress <= 0x0060U) && (ulEndAddress > 0x0060U) &&
		(s_ausCommandRegisters[0x0060U] > 3U)) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
			"SYRUP_CHANNEL_UNAVAILABLE",
			-1, "channel", (int32_t)s_ausCommandRegisters[0x0060U]);
	}
	if ((usAddress <= 0x0062U) && (ulEndAddress > 0x0062U) &&
		(prvSubmitManual(COFFEE2_DEVICE_SYRUP_MACHINE,
			COFFEE2_ACTION_SYRUP_SET_REMAINING,
			s_ausCommandRegisters[0x0062U], 0U) != 0U)) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
			"SYRUP_REMAINING_SET", 0,
			"time_ds", (int32_t)s_ausCommandRegisters[0x0062U]);
	}
	if ((usAddress <= 0x0063U) && (ulEndAddress > 0x0063U) &&
		(s_ausCommandRegisters[0x0063U] != 0U) &&
		(xCoffee2WorkflowSubmitMaintenance(
			COFFEE2_MAINTENANCE_SYRUP_CLEAN, 0U, 0U) == pdPASS)) {
		s_ausCommandRegisters[0x0063U] = 0U;
	}
	if ((usAddress <= 0x0070U) && (ulEndAddress > 0x0070U) &&
		(s_ausCommandRegisters[0x0070U] != 0U)) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
			"MANUAL_CUP_ASSUMED", 0,
			"target_dg",
			(int32_t)s_ausCommandRegisters[0x0071U]);
		if (xCoffee2WorkflowSubmitManualIce(
			s_ausCommandRegisters[0x0071U]) == pdPASS) {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
				COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
				"MANUAL_ICE_ACCEPTED", 0,
				"target_dg",
				(int32_t)s_ausCommandRegisters[0x0071U]);
			s_ausCommandRegisters[0x0070U] = 0U;
		} else {
			(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
				COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
				"MANUAL_ICE_REJECTED", -1,
				"target_dg",
				(int32_t)s_ausCommandRegisters[0x0071U]);
		}
	}
	if ((usAddress <= COFFEE2_REG_HOT_WATER_START) &&
		(ulEndAddress > COFFEE2_REG_HOT_WATER_START)) {
		usValue = s_ausCommandRegisters[COFFEE2_REG_HOT_WATER_START];
		if (xCoffee2WorkflowSetHotWater((usValue != 0U) ? 1U : 0U,
			s_ausCommandRegisters[COFFEE2_REG_HOT_WATER_MINUTES]) ==
			pdPASS) {
			s_ausCommandRegisters[COFFEE2_REG_HOT_WATER_START] = 0U;
		}
	}
	if ((usAddress <= COFFEE2_REG_SYRUP_CLEAN) &&
		(ulEndAddress > COFFEE2_REG_SYRUP_CLEAN) &&
		(s_ausCommandRegisters[COFFEE2_REG_SYRUP_CLEAN] != 0U) &&
		(xCoffee2WorkflowSubmitMaintenance(
			COFFEE2_MAINTENANCE_SYRUP_CLEAN, 0U, 0U) == pdPASS)) {
		s_ausCommandRegisters[COFFEE2_REG_SYRUP_CLEAN] = 0U;
	}
	if ((usAddress <= COFFEE2_REG_COFFEE_PIPE_CLEAN) &&
		(ulEndAddress > COFFEE2_REG_COFFEE_PIPE_CLEAN) &&
		(s_ausCommandRegisters[COFFEE2_REG_COFFEE_PIPE_CLEAN] != 0U) &&
		(xCoffee2WorkflowSubmitMaintenance(
			COFFEE2_MAINTENANCE_COFFEE_CLEAN,
			6U,
			0U) == pdPASS)) {
		s_ausCommandRegisters[COFFEE2_REG_COFFEE_PIPE_CLEAN] = 0U;
	}
	if (((usAddress <= COFFEE2_REG_MANUAL_FRUIT_TYPE) &&
		(ulEndAddress > COFFEE2_REG_MANUAL_FRUIT_TYPE)) ||
		((usAddress <= COFFEE2_REG_MANUAL_FRUIT_AMOUNT) &&
		(ulEndAddress > COFFEE2_REG_MANUAL_FRUIT_AMOUNT))) {
		usValue = s_ausCommandRegisters[COFFEE2_REG_MANUAL_FRUIT_TYPE];
		if (((usValue == 0x0001U) || (usValue == 0x0010U)) &&
			(s_ausCommandRegisters[COFFEE2_REG_MANUAL_FRUIT_AMOUNT] != 0U) &&
			(xCoffee2WorkflowSubmitMaintenance(
				COFFEE2_MAINTENANCE_FRUIT_DISPENSE,
				(usValue == 0x0001U) ? 1U : 2U,
				s_ausCommandRegisters[
					COFFEE2_REG_MANUAL_FRUIT_AMOUNT]) == pdPASS)) {
			s_ausCommandRegisters[COFFEE2_REG_MANUAL_FRUIT_TYPE] = 0U;
			s_ausCommandRegisters[COFFEE2_REG_MANUAL_FRUIT_AMOUNT] = 0U;
		}
	}
	if ((usAddress <= COFFEE2_REG_FRUIT_A_CLEAN) &&
		(ulEndAddress > COFFEE2_REG_FRUIT_A_CLEAN) &&
		(s_ausCommandRegisters[COFFEE2_REG_FRUIT_A_CLEAN] != 0U) &&
		(xCoffee2WorkflowSubmitMaintenance(
			COFFEE2_MAINTENANCE_FRUIT_CLEAN, 1U, 0U) == pdPASS)) {
		s_ausCommandRegisters[COFFEE2_REG_FRUIT_A_CLEAN] = 0U;
	}
	if ((usAddress <= COFFEE2_REG_FRUIT_B_CLEAN) &&
		(ulEndAddress > COFFEE2_REG_FRUIT_B_CLEAN) &&
		(s_ausCommandRegisters[COFFEE2_REG_FRUIT_B_CLEAN] != 0U) &&
		(xCoffee2WorkflowSubmitMaintenance(
			COFFEE2_MAINTENANCE_FRUIT_CLEAN, 2U, 0U) == pdPASS)) {
		s_ausCommandRegisters[COFFEE2_REG_FRUIT_B_CLEAN] = 0U;
	}
}

/*-----------------------------------------------------------*/
static uint8_t prvSubmitRobotPosition(uint16_t usPosition)
{
	Coffee2Action_e xAction;
	uint16_t usParameter;

	usParameter = 0U;
	switch (usPosition) {
	case 0x0000U:
		xAction = COFFEE2_ACTION_ROBOT_HOME;
		break;
	case 0x0001U:
		xAction = COFFEE2_ACTION_ROBOT_TAKE_HOT_CUP;
		break;
	case 0x0002U:
		xAction = COFFEE2_ACTION_ROBOT_TAKE_COLD_CUP;
		break;
	case 0x0003U:
	case 0x0004U:
		xAction = COFFEE2_ACTION_ROBOT_TO_LID;
		usParameter = (uint16_t)(usPosition - 2U);
		break;
	case 0x0005U:
	case 0x0006U:
		xAction = COFFEE2_ACTION_ROBOT_TO_COFFEE;
		usParameter = (uint16_t)(usPosition - 5U);
		break;
	case 0x0007U:
		xAction = COFFEE2_ACTION_ROBOT_TO_ICE;
		break;
	case 0x0008U:
		xAction = COFFEE2_ACTION_ROBOT_TO_FRUIT_SYRUP;
		break;
	case 0x0009U:
		xAction = COFFEE2_ACTION_ROBOT_TAKE_OUTPUT_1;
		break;
	case 0x000AU:
		xAction = COFFEE2_ACTION_ROBOT_TAKE_OUTPUT_2;
		break;
	case 0x000BU:
		xAction = COFFEE2_ACTION_ROBOT_PUT_OUTPUT;
		usParameter = 1U;
		break;
	case 0x000CU:
		xAction = COFFEE2_ACTION_ROBOT_TAKE_LID;
		break;
	case 0x000DU:
		xAction = COFFEE2_ACTION_ROBOT_COVER_LID;
		break;
	case 0x000EU:
		xAction = COFFEE2_ACTION_ROBOT_PUT_OUTPUT;
		usParameter = 2U;
		break;
	case 0x000FU:
		xAction = COFFEE2_ACTION_ROBOT_TAKE_COFFEE;
		break;
	default:
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
			"MANUAL_ROBOT_POSITION_UNSUPPORTED", -1,
			"position", (int32_t)usPosition);
		return 0U;
	}
	return prvSubmitManual(COFFEE2_DEVICE_ROBOT, xAction,
		usParameter, 0U);
}

/*-----------------------------------------------------------*/
static uint8_t prvSubmitManual(Coffee2DeviceId_e xDeviceId,
	Coffee2Action_e xAction, uint16_t usParameter0,
	uint16_t usParameter1)
{
	Coffee2Command_t xCommand;

	memset(&xCommand, 0, sizeof(xCommand));
	xCommand.ucDeviceId = (uint8_t)xDeviceId;
	xCommand.ucSource = (uint8_t)COFFEE2_COMMAND_SOURCE_SERVER;
	xCommand.ulOrderId = COFFEE2_LOG_ORDER_DEBUG;
	xCommand.usAction = (uint16_t)xAction;
	xCommand.ausParameter[0] = usParameter0;
	xCommand.ausParameter[1] = usParameter1;
	xCommand.ulTimeoutMs = COFFEE2_WORKFLOW_DEFAULT_TIMEOUT_MS;
	xCommand.ucRetryLimit = 1U;
	if (((xDeviceId == COFFEE2_DEVICE_ROBOT) ?
		xCoffee2CommandSubmitUrgent(&xCommand, 0U) :
		xCoffee2CommandSubmit(&xCommand, 0U)) != pdPASS) {
		(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
			"MANUAL_COMMAND_QUEUE_FULL",
			-1, "action", (int32_t)xAction);
		return 0U;
	}
	(void)xCoffee2LogWriteFieldOrder(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SERVER, COFFEE2_LOG_ORDER_DEBUG,
		"MANUAL_COMMAND_ACCEPTED",
		0, "action", (int32_t)xAction);
	return 1U;
}

/*-----------------------------------------------------------*/
static void prvBuildDebugRegisters(uint16_t *pusDebug)
{
	AppTaskManagerStatus_t xTaskStatus;
	Coffee2LogStatus_t xLogStatus;
	EventBits_t xEvents;
	uint32_t ulHeapBytes;
	uint32_t ulMinimumHeapBytes;
	uint16_t usBase;
	uint16_t usReadyMask;
	uint8_t ucBusIndex;
	uint8_t ucClientIndex;
	uint8_t ucDeviceId;

	memset(pusDebug, 0,
		COFFEE2_SERVER_DEBUG_COUNT * sizeof(uint16_t));
	vCoffee2LogGetStatus(&xLogStatus);
	vAppTaskManagerGetStatus(&xTaskStatus);
	pusDebug[0] = 0x4332U;
	pusDebug[1] = 0x0101U;
	pusDebug[2] = g_xCoffee2ServerStatus.ucActiveClients;
	pusDebug[3] = (uint16_t)g_xCoffee2ServerStatus.ulAcceptedCount;
	pusDebug[4] = (uint16_t)g_xCoffee2ServerStatus.ulRejectedCount;
	pusDebug[5] = xLogStatus.usPendingCount;
	pusDebug[6] = (uint16_t)xLogStatus.ulDroppedCount;
	pusDebug[7] = (uint16_t)g_xCoffee2WorkflowStatus.xState;
	pusDebug[8] = g_xCoffee2WorkflowStatus.usCurrentStep;
	pusDebug[9] = g_xCoffee2WorkflowStatus.usCurrentOrderId;
	ulHeapBytes = (uint32_t)xPortGetFreeHeapSize();
	ulMinimumHeapBytes =
		(uint32_t)xPortGetMinimumEverFreeHeapSize();
	pusDebug[10] = (uint16_t)ulHeapBytes;
	pusDebug[11] = (uint16_t)(ulHeapBytes >> 16);
	pusDebug[12] = (uint16_t)ulMinimumHeapBytes;
	pusDebug[13] = (uint16_t)(ulMinimumHeapBytes >> 16);
	pusDebug[14] = g_xCoffee2ServerStatus.ucListening;
	pusDebug[15] = g_xCoffee2ServerStatus.usListenPort;
	for (ucDeviceId = 1U; ucDeviceId < COFFEE2_DEVICE_COUNT;
		ucDeviceId++) {
		usBase = (uint16_t)(0x10U +
			((uint16_t)(ucDeviceId - 1U) * 4U));
		xEvents = xCoffee2DeviceGetEvents(
			(Coffee2DeviceId_e)ucDeviceId);
		pusDebug[usBase] = (uint16_t)xEvents;
		pusDebug[usBase + 1U] = (uint16_t)
			g_axCoffee2DeviceStatus[ucDeviceId].lLastResult;
		pusDebug[usBase + 2U] = (uint16_t)
			g_axCoffee2DeviceStatus[ucDeviceId].ulLastCommandId;
		pusDebug[usBase + 3U] = (uint16_t)
			(g_axCoffee2DeviceStatus[ucDeviceId].ulLastCommandId >>
				16);
	}
	usReadyMask = 0U;
	usReadyMask |= (xTaskStatus.ucLogReady != 0U) ? 0x0001U : 0U;
	usReadyMask |= (xTaskStatus.ucDeviceReady != 0U) ? 0x0002U : 0U;
	usReadyMask |= (xTaskStatus.ucServerReady != 0U) ? 0x0004U : 0U;
	usReadyMask |= (xTaskStatus.ucRobotReady != 0U) ? 0x0008U : 0U;
	usReadyMask |= (xTaskStatus.ucRtuReady != 0U) ? 0x0010U : 0U;
	usReadyMask |= (xTaskStatus.ucWorkflowReady != 0U) ? 0x0020U : 0U;
	usReadyMask |= (xTaskStatus.ucNetworkStackReady != 0U) ?
		0x0040U : 0U;
	usReadyMask |= (xTaskStatus.ucNetworkReady != 0U) ? 0x0080U : 0U;
	pusDebug[0x38U] = (uint16_t)xTaskStatus.ulResetCause;
	pusDebug[0x39U] = (uint16_t)(xTaskStatus.ulResetCause >> 16);
	pusDebug[0x3AU] = (uint16_t)xTaskStatus.ulTaskCreatedMask;
	pusDebug[0x3BU] =
		(uint16_t)(xTaskStatus.ulTaskCreatedMask >> 16);
	pusDebug[0x3CU] = (uint16_t)xTaskStatus.ulTaskFailedMask;
	pusDebug[0x3DU] =
		(uint16_t)(xTaskStatus.ulTaskFailedMask >> 16);
	pusDebug[0x3EU] = (uint16_t)xTaskStatus.xStartResult;
	pusDebug[0x3FU] = usReadyMask;
	for (ucClientIndex = 0U;
		ucClientIndex < COFFEE2_SERVER_MAX_CLIENTS;
		ucClientIndex++) {
		Coffee2ServerClientStatus_t *pxClient;

		pxClient =
			&g_xCoffee2ServerStatus.axClient[ucClientIndex];
		usBase = (uint16_t)(0x40U +
			((uint16_t)ucClientIndex * 12U));
		pusDebug[usBase] = pxClient->ucConnected;
		pusDebug[usBase + 1U] =
			(uint16_t)(pxClient->ulRemoteIpv4 >> 16);
		pusDebug[usBase + 2U] =
			(uint16_t)pxClient->ulRemoteIpv4;
		pusDebug[usBase + 3U] = pxClient->usRemotePort;
		pusDebug[usBase + 4U] =
			(uint16_t)pxClient->ulRequestCount;
		pusDebug[usBase + 5U] =
			(uint16_t)(pxClient->ulRequestCount >> 16);
		pusDebug[usBase + 6U] =
			(uint16_t)pxClient->ulErrorCount;
		pusDebug[usBase + 7U] =
			(uint16_t)(pxClient->ulErrorCount >> 16);
		pusDebug[usBase + 8U] =
			(uint16_t)pxClient->ulDisconnectCount;
		pusDebug[usBase + 9U] =
			(uint16_t)pxClient->lLastResult;
		pusDebug[usBase + 10U] =
			(uint16_t)pxClient->ulLastActivityTick;
		pusDebug[usBase + 11U] =
			(uint16_t)(pxClient->ulLastActivityTick >> 16);
	}
	pusDebug[0x58U] = g_xCoffee2RobotTcpStatus.ucConnected;
	pusDebug[0x59U] =
		(uint16_t)g_xCoffee2RobotTcpStatus.ulConnectAttemptCount;
	pusDebug[0x5AU] =
		(uint16_t)g_xCoffee2RobotTcpStatus.ulConnectSuccessCount;
	pusDebug[0x5BU] =
		(uint16_t)g_xCoffee2RobotTcpStatus.ulDisconnectCount;
	pusDebug[0x5CU] =
		(uint16_t)g_xCoffee2RobotTcpStatus.ulConsecutiveFailures;
	pusDebug[0x5DU] =
		(uint16_t)g_xCoffee2RobotTcpStatus.ulNextRetryDelayMs;
	pusDebug[0x5EU] = (uint16_t)
		(g_xCoffee2RobotTcpStatus.ulNextRetryDelayMs >> 16);
	pusDebug[0x5FU] =
		(uint16_t)g_xCoffee2RobotTcpStatus.lLastResult;
	for (ucBusIndex = 0U; ucBusIndex < COFFEE2_RTU_BUS_COUNT;
		ucBusIndex++) {
		Coffee2RtuBusStatus_t *pxBus;

		pxBus = &g_axCoffee2RtuBusStatus[ucBusIndex];
		usBase = (uint16_t)(0x60U +
			((uint16_t)ucBusIndex * 6U));
		pusDebug[usBase] = pxBus->ucReady;
		pusDebug[usBase + 1U] = pxBus->ucActiveDevice;
		pusDebug[usBase + 2U] =
			(uint16_t)pxBus->ulCurrentBaudRate;
		pusDebug[usBase + 3U] =
			(uint16_t)(pxBus->ulCurrentBaudRate >> 16);
		pusDebug[usBase + 4U] =
			(uint16_t)pxBus->lLastResult;
		pusDebug[usBase + 5U] =
			(uint16_t)pxBus->ulErrorCount;
	}
}

/*-----------------------------------------------------------*/
static void prvRefreshStatusRegisters(void)
{
	Coffee2IoState_t xIoSnapshot;
	EventBits_t xRobotEvents;
	EventBits_t xCoffeeEvents;
	EventBits_t xCupEvents;
	EventBits_t xLidEvents;
	EventBits_t xSyrupEvents;
	EventBits_t xIceEvents;
	EventBits_t xScaleEvents;
	uint16_t usIoStatus;
	uint16_t usMachineStatus;
	uint16_t usFault;
	uint16_t usCupState;
	uint16_t usLidState;
	uint16_t usCupFault;
	uint16_t usLidFault;
	uint16_t usCupFault2;
	uint16_t usLidFault2;
	uint16_t usSyrupState;
	uint16_t usFruitState;
	uint16_t usLocalIoBitmap;
	uint16_t usInputIoBitmap;
	uint16_t usOutputIoBitmap;
	uint16_t usOutputCupMask;
	uint16_t usEnergyInteger;
	uint16_t usEnergyFraction;
	uint32_t ulEnergyCentikWh;
	uint8_t ucIndex;

	usMachineStatus =
		(uint16_t)g_xCoffee2WorkflowStatus.xMachineState;
	xRobotEvents = xCoffee2DeviceGetEvents(COFFEE2_DEVICE_ROBOT);
	xCoffeeEvents = xCoffee2DeviceGetEvents(
		COFFEE2_DEVICE_COFFEE_MACHINE);
	xCupEvents = xCoffee2DeviceGetEvents(COFFEE2_DEVICE_CUP_MACHINE);
	xLidEvents = xCoffee2DeviceGetEvents(COFFEE2_DEVICE_LID_MACHINE);
	xSyrupEvents = xCoffee2DeviceGetEvents(
		COFFEE2_DEVICE_SYRUP_MACHINE);
	xIceEvents = xCoffee2DeviceGetEvents(COFFEE2_DEVICE_ICE_MACHINE);
	xScaleEvents = xCoffee2DeviceGetEvents(COFFEE2_DEVICE_SCALE);
	usCupFault =
		(g_xCoffee2CupLidImage.aucCupCoils[0] == 0U ? 0x0001U : 0U) |
		(g_xCoffee2CupLidImage.aucCupCoils[1] != 0U ? 0x0010U : 0U) |
		(g_xCoffee2CupLidImage.aucCupCoils[2] != 0U ? 0x0100U : 0U);
	usLidFault =
		(g_xCoffee2CupLidImage.aucLidCoils[0] == 0U ? 0x0001U : 0U) |
		(g_xCoffee2CupLidImage.aucLidCoils[1] != 0U ? 0x0010U : 0U) |
		(g_xCoffee2CupLidImage.aucLidCoils[2] != 0U ? 0x0100U : 0U);
	usCupFault2 =
		(g_xCoffee2CupLidImage.aucCupCoils[5] == 0U ? 0x0001U : 0U) |
		(g_xCoffee2CupLidImage.aucCupCoils[6] != 0U ? 0x0010U : 0U) |
		(g_xCoffee2CupLidImage.aucCupCoils[7] != 0U ? 0x0100U : 0U);
	usLidFault2 =
		(g_xCoffee2CupLidImage.aucLidCoils[5] == 0U ? 0x0001U : 0U) |
		(g_xCoffee2CupLidImage.aucLidCoils[6] != 0U ? 0x0010U : 0U) |
		(g_xCoffee2CupLidImage.aucLidCoils[7] != 0U ? 0x0100U : 0U);
	if ((xCupEvents & COFFEE2_DEVICE_EVENT_ONLINE) == 0U) {
		usCupFault = 0U;
		usCupFault2 = 0U;
	}
	if ((xLidEvents & COFFEE2_DEVICE_EVENT_ONLINE) == 0U) {
		usLidFault = 0U;
		usLidFault2 = 0U;
	}
	if ((xCupEvents & COFFEE2_DEVICE_EVENT_ONLINE) == 0U) {
		usCupState = 4U;
	} else if ((usCupFault != 0U) || (usCupFault2 != 0U) ||
		((xCupEvents & (COFFEE2_DEVICE_EVENT_DEVICE_FAULT |
		COFFEE2_DEVICE_EVENT_COMMAND_FAILED)) != 0U)) {
		usCupState = 3U;
	} else if ((xCupEvents & COFFEE2_DEVICE_EVENT_BUSY) != 0U) {
		usCupState = 2U;
	} else {
		usCupState = 1U;
	}
	if ((xLidEvents & COFFEE2_DEVICE_EVENT_ONLINE) == 0U) {
		usLidState = 4U;
	} else if ((usLidFault != 0U) || (usLidFault2 != 0U) ||
		((xLidEvents & (COFFEE2_DEVICE_EVENT_DEVICE_FAULT |
		COFFEE2_DEVICE_EVENT_COMMAND_FAILED)) != 0U)) {
		usLidState = 3U;
	} else if ((xLidEvents & COFFEE2_DEVICE_EVENT_BUSY) != 0U) {
		usLidState = 2U;
	} else {
		usLidState = 1U;
	}
	usSyrupState =
		((xSyrupEvents & COFFEE2_DEVICE_EVENT_ONLINE) != 0U) ? 1U : 0U;
	for (ucIndex = 1U;
		(ucIndex <= 5U) && (usSyrupState != 0U); ucIndex++) {
		if (g_xCoffee2SyrupImage.ausRegisters[ucIndex] == 4U) {
			usSyrupState = 3U;
			break;
		}
		if (g_xCoffee2SyrupImage.ausRegisters[ucIndex] == 2U) {
			usSyrupState = 2U;
		}
	}
	usIoStatus = 0x2000U;
	if (g_axCoffee2DeviceStatus[COFFEE2_DEVICE_IO_INPUT].ucOnline !=
		0U) {
		usIoStatus |= 0x0001U;
	}
	if (g_axCoffee2DeviceStatus[COFFEE2_DEVICE_IO_OUTPUT].ucOnline !=
		0U) {
		usIoStatus |= 0x0002U;
	}
	vCoffee2IoRefreshLocal();
	vCoffee2IoGetSnapshot(&xIoSnapshot);
	usLocalIoBitmap = 0U;
	usInputIoBitmap = 0U;
	usOutputIoBitmap = 0U;
	for (ucIndex = 0U; ucIndex < COFFEE2_LOCAL_IO_COUNT;
		ucIndex++) {
		if (xIoSnapshot.xInput.aucXPin[ucIndex] != 0U) {
			usLocalIoBitmap |= (uint16_t)(1U << ucIndex);
		}
		if (xIoSnapshot.xOutput.aucYPin[ucIndex] != 0U) {
			usLocalIoBitmap |= (uint16_t)(1U << (ucIndex + 8U));
		}
	}
	for (ucIndex = 0U; ucIndex < COFFEE2_MODBUS_IO_COUNT;
		ucIndex++) {
		if (xIoSnapshot.xInput.aucMB1XPin[ucIndex] != 0U) {
			usInputIoBitmap |= (uint16_t)(1U << ucIndex);
		}
		if (xIoSnapshot.xOutput.aucMB2YPin[ucIndex] != 0U) {
			usOutputIoBitmap |= (uint16_t)(1U << ucIndex);
		}
	}
	usFruitState = g_xCoffee2WorkflowStatus.aucFruitState[0];
	if (g_xCoffee2WorkflowStatus.aucFruitState[1] > usFruitState) {
		usFruitState = g_xCoffee2WorkflowStatus.aucFruitState[1];
	}
	usOutputCupMask = 0U;
	if (xIoSnapshot.xInput.aucMB1XPin[
		COFFEE2_EXTERNAL_DI_OUTPUT_FRONT_CUP] != 0U) {
		usOutputCupMask |= 0x0001U;
	}
	if (xIoSnapshot.xInput.aucMB1XPin[
		COFFEE2_EXTERNAL_DI_OUTPUT_REAR_CUP] != 0U) {
		usOutputCupMask |= 0x0002U;
	}
	usEnergyInteger = 0U;
	usEnergyFraction = 0U;
	ulEnergyCentikWh = 0U;
	if (g_xCoffee2PowerMeterImage.fEnergy > 0.0f) {
		ulEnergyCentikWh = (uint32_t)(
			(g_xCoffee2PowerMeterImage.fEnergy * 100.0f) + 0.5f);
		usEnergyInteger = (uint16_t)(ulEnergyCentikWh / 100U);
		usEnergyFraction = (uint16_t)(ulEnergyCentikWh % 100U);
	}
	taskENTER_CRITICAL();
	s_ausStatusRegisters[COFFEE2_REG_MACHINE_STATUS -
		COFFEE2_REG_STATUS_BASE] = usMachineStatus;
	s_ausStatusRegisters[0x0025U] = usIoStatus;
	s_ausStatusRegisters[0x0029U] =
		(uint16_t)g_xCoffee2WorkflowStatus.lLastError;
	/* The protocol reserves a 32-channel page for each IO category. */
	s_ausStatusRegisters[COFFEE2_REG_LOCAL_INPUT_LOW -
		COFFEE2_REG_STATUS_BASE] = (uint16_t)(usLocalIoBitmap & 0x00FFU);
	s_ausStatusRegisters[COFFEE2_REG_LOCAL_INPUT_HIGH -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_INPUT_1_LOW -
		COFFEE2_REG_STATUS_BASE] = usInputIoBitmap;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_INPUT_1_HIGH -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_INPUT_2_LOW -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_INPUT_2_HIGH -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_INPUT_3_LOW -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_INPUT_3_HIGH -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_LOCAL_OUTPUT_LOW -
		COFFEE2_REG_STATUS_BASE] =
		(uint16_t)((usLocalIoBitmap >> 8U) & 0x00FFU);
	s_ausStatusRegisters[COFFEE2_REG_LOCAL_OUTPUT_HIGH -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_OUTPUT_1_LOW -
		COFFEE2_REG_STATUS_BASE] = usOutputIoBitmap;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_OUTPUT_1_HIGH -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_OUTPUT_2_LOW -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_OUTPUT_2_HIGH -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_OUTPUT_3_LOW -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_EXTERNAL_OUTPUT_3_HIGH -
		COFFEE2_REG_STATUS_BASE] = 0U;
	s_ausStatusRegisters[COFFEE2_REG_HOT_WATER_STATUS -
		COFFEE2_REG_STATUS_BASE] =
		g_xCoffee2WorkflowStatus.ucHotWaterState;
	s_ausStatusRegisters[COFFEE2_REG_COFFEE_PIPE_STATUS -
		COFFEE2_REG_STATUS_BASE] =
		g_xCoffee2WorkflowStatus.ucCoffeeCleanState;
	s_ausStatusRegisters[COFFEE2_REG_FRUIT_STATUS -
		COFFEE2_REG_STATUS_BASE] = usFruitState;
	s_ausStatusRegisters[COFFEE2_REG_FRUIT_A_LOW -
		COFFEE2_REG_STATUS_BASE] = xIoSnapshot.xInput.aucMB1XPin[
		COFFEE2_EXTERNAL_DI_FRUIT_MILK_A_LOW];
	s_ausStatusRegisters[COFFEE2_REG_FRUIT_B_LOW -
		COFFEE2_REG_STATUS_BASE] = xIoSnapshot.xInput.aucMB1XPin[
		COFFEE2_EXTERNAL_DI_FRUIT_MILK_B_LOW];
	s_ausStatusRegisters[COFFEE2_REG_FRUIT_A_STATUS -
		COFFEE2_REG_STATUS_BASE] =
		g_xCoffee2WorkflowStatus.aucFruitState[0];
	s_ausStatusRegisters[COFFEE2_REG_FRUIT_B_STATUS -
		COFFEE2_REG_STATUS_BASE] =
		g_xCoffee2WorkflowStatus.aucFruitState[1];
	s_ausStatusRegisters[0x000BU] = s_ausOutputState[0];
	s_ausStatusRegisters[0x000CU] = s_ausOutputState[1];
	s_ausStatusRegisters[0x001AU] = usEnergyInteger;
	s_ausStatusRegisters[0x001BU] = usEnergyFraction;
	s_ausStatusRegisters[0x0023U] = usOutputCupMask;
	for (ucIndex = 0U; ucIndex < 12U; ucIndex++) {
		s_ausStatusRegisters[0x0030U + ucIndex] =
			g_xCoffee2RobotData.aucBaseInputs[ucIndex];
	}
	s_ausStatusRegisters[0x003CU] =
		((xRobotEvents & COFFEE2_DEVICE_EVENT_DEVICE_FAULT) != 0U) ?
		3U : (((xRobotEvents & COFFEE2_DEVICE_EVENT_BUSY) != 0U) ?
		2U : (((xRobotEvents & COFFEE2_DEVICE_EVENT_ONLINE) != 0U) ?
		1U : 0U));

	s_ausStatusRegisters[0x0040U] =
		((xCoffeeEvents & COFFEE2_DEVICE_EVENT_DEVICE_FAULT) != 0U) ?
		3U : (((xCoffeeEvents & COFFEE2_DEVICE_EVENT_BUSY) != 0U) ?
		2U : (((xCoffeeEvents & COFFEE2_DEVICE_EVENT_ONLINE) != 0U) ?
		1U : 0U));
	s_ausStatusRegisters[0x0042U] =
		g_xCoffee2CoffeeMachineImage.ausStatus[0];
	for (ucIndex = 0U; ucIndex < 7U; ucIndex++) {
		s_ausStatusRegisters[0x0043U + ucIndex] =
			g_xCoffee2CoffeeMachineImage.ausStatus[1U + ucIndex];
	}
	for (ucIndex = 0U; ucIndex < 6U; ucIndex++) {
		s_ausStatusRegisters[0x004AU + ucIndex] =
			g_xCoffee2CoffeeMachineImage.ausStatus[9U + ucIndex];
	}

	s_ausStatusRegisters[0x0050U] = usCupState |
		(uint16_t)(usLidState << 4U);
	s_ausStatusRegisters[0x0051U] =
		g_xCoffee2CupLidImage.aucCupCoils[0];
	s_ausStatusRegisters[0x0052U] =
		g_xCoffee2CupLidImage.aucCupCoils[5];
	s_ausStatusRegisters[0x0053U] =
		g_xCoffee2CupLidImage.aucLidCoils[0];
	s_ausStatusRegisters[0x0054U] =
		g_xCoffee2CupLidImage.aucLidCoils[5];
	s_ausStatusRegisters[0x0055U] =
		usCupFault;
	s_ausStatusRegisters[0x0056U] =
		usCupFault2;
	s_ausStatusRegisters[0x0057U] =
		usLidFault;
	s_ausStatusRegisters[0x0058U] =
		usLidFault2;

	s_ausStatusRegisters[0x0060U] = usSyrupState;
	for (ucIndex = 0U; ucIndex < 4U; ucIndex++) {
		s_ausStatusRegisters[0x0061U + ucIndex] =
			g_xCoffee2SyrupImage.ausRegisters[11U + ucIndex];
	}

	s_ausStatusRegisters[0x0070U] =
		((xIceEvents & COFFEE2_DEVICE_EVENT_COMMAND_FAILED) != 0U) ?
		3U : (((xIceEvents & COFFEE2_DEVICE_EVENT_BUSY) != 0U) ?
		2U : (((xIceEvents & COFFEE2_DEVICE_EVENT_ONLINE) != 0U) ?
		1U : 0U));
	s_ausStatusRegisters[0x0071U] =
		g_xCoffee2IceImage.ausRegisters[2];
	s_ausStatusRegisters[0x0072U] =
		g_xCoffee2IceImage.ausRegisters[0];
	s_ausStatusRegisters[0x0073U] =
		((xIceEvents & COFFEE2_DEVICE_EVENT_ONLINE) != 0U) &&
		(g_xCoffee2IceImage.ausRegisters[1] == 0U) ? 1U : 0U;
	usFault = g_xCoffee2IceImage.ausRegisters[3] |
		g_xCoffee2IceImage.ausRegisters[4] |
		g_xCoffee2IceImage.ausRegisters[5];
	s_ausStatusRegisters[0x0074U] = usFault;
	s_ausStatusRegisters[0x0078U] =
		g_xCoffee2IceImage.ausRegisters[3];
	s_ausStatusRegisters[0x007AU] =
		g_xCoffee2IceImage.ausRegisters[5];
	s_ausStatusRegisters[0x007CU] =
		g_xCoffee2IceImage.ausRegisters[10];
	s_ausStatusRegisters[0x007DU] =
		((xScaleEvents & (COFFEE2_DEVICE_EVENT_COMM_FAULT |
		COFFEE2_DEVICE_EVENT_DEVICE_FAULT |
		COFFEE2_DEVICE_EVENT_COMMAND_FAILED)) != 0U) ? 1U : 0U;
	s_ausStatusRegisters[0x0080U] =
		(xIoSnapshot.xInput.aucXPin[COFFEE2_LOCAL_DI_HOT_WATER_LOW] ==
		0U) ? 1U : 0U;
	s_ausStatusRegisters[0x0081U] =
		(xIoSnapshot.xInput.aucXPin[COFFEE2_LOCAL_DI_HOT_WATER_HIGH] ==
		0U) ? 1U : 0U;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
static int prvCreateListener(void)
{
	struct sockaddr_in xAddress;
	int lListener;
	int lReuse;

	lListener = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lListener < 0) {
		return -1;
	}
	lReuse = 1;
	(void)lwip_setsockopt(lListener, SOL_SOCKET, SO_REUSEADDR,
		&lReuse, (socklen_t)sizeof(lReuse));
	memset(&xAddress, 0, sizeof(xAddress));
	xAddress.sin_family = AF_INET;
	xAddress.sin_port = htons(COFFEE2_SERVER_PORT);
	xAddress.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
	if (lwip_bind(lListener, (struct sockaddr *)&xAddress,
		(socklen_t)sizeof(xAddress)) < 0) {
		(void)lwip_close(lListener);
		return -1;
	}
	if (lwip_listen(lListener, COFFEE2_SERVER_MAX_CLIENTS) < 0) {
		(void)lwip_close(lListener);
		return -1;
	}
	(void)lwip_fcntl(lListener, F_SETFL, O_NONBLOCK);
	return lListener;
}

/*-----------------------------------------------------------*/
static void prvCloseSlot(Coffee2ServerSlot_t *pxSlot,
	Coffee2ServerClientStatus_t *pxStatus, uint8_t ucSlotIndex,
	int32_t lReason)
{
	uint8_t ucWasConnected;

	if ((pxSlot == NULL) || (pxStatus == NULL)) {
		return;
	}
	ucWasConnected = pxStatus->ucConnected;
	if ((pxSlot->ucCreated != 0U) &&
		(pxSlot->xTransport.lSocket >= 0)) {
		(void)xTransportClose(&pxSlot->xChannel);
	}
	pxSlot->ucActive = 0U;
	pxStatus->ucConnected = 0U;
	pxStatus->lLastResult = lReason;
	if (ucWasConnected != 0U) {
		pxStatus->ulDisconnectCount++;
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER, "SERVER_CLIENT_DISCONNECTED",
			lReason, "slot", (int32_t)ucSlotIndex);
	}
}

/*-----------------------------------------------------------*/
static void prvUpdateActiveClientCount(void)
{
	uint8_t ucCount;
	uint8_t ucIndex;
	uint8_t ucWasOnline;

	ucWasOnline = g_xCoffee2ServerStatus.ucOnline;
	ucCount = 0U;
	for (ucIndex = 0U; ucIndex < COFFEE2_SERVER_MAX_CLIENTS;
		ucIndex++) {
		if (g_xCoffee2ServerStatus.axClient[ucIndex].ucConnected !=
			0U) {
			ucCount++;
		}
	}
	g_xCoffee2ServerStatus.ucActiveClients = ucCount;
	g_xCoffee2ServerStatus.ucOnline = (ucCount != 0U) ? 1U : 0U;
	if (g_xCoffee2ServerStatus.ucOnline == ucWasOnline) {
		return;
	}
	g_xCoffee2ServerStatus.ulOnlineTransitionCount++;
	if (g_xCoffee2ServerStatus.ucOnline != 0U) {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_INFO,
			COFFEE2_LOG_SOURCE_SERVER, "SERVER_ONLINE", 0,
			"clients", (int32_t)ucCount);
	} else {
		(void)xCoffee2LogWriteField(COFFEE2_LOG_LEVEL_WARNING,
			COFFEE2_LOG_SOURCE_SERVER, "SERVER_OFFLINE", -1,
			"clients", 0);
	}
}
