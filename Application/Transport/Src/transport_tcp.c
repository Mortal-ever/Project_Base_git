/**
  * @file      transport_tcp.c
  * @brief     Implement LwIP Netconn and nonblocking Socket backends.
  * @author    WHong
  * @date      2026-07-28
  */

#include "transport_tcp.h"

#include <string.h>

#include "lwip/err.h"
#include "lwip/errno.h"
#include "lwip/netbuf.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "task.h"

/**
  * @brief  打开已配置的 Netconn 端点。
  * @param[in,out] pvContext TransportTcpContext_t 上下文。
  * @retval TransportResult_e 打开结果。
  */
static TransportResult_e prvOpen(void *pvContext);
/**
  * @brief  关闭上下文拥有的全部 Netconn 资源。
  * @param[in,out] pvContext TransportTcpContext_t 上下文。
  * @retval TransportResult_e 关闭结果。
  */
static TransportResult_e prvClose(void *pvContext);
/**
  * @brief  在一个总截止时间内发送完整 Netconn 字节序列。
  * @param[in,out] pvContext TransportTcpContext_t 上下文。
  * @param[in] pucData 待发送数据。
  * @param[in] usDataLen 待发送字节数。
  * @param[out] pusSentLen 实际发送字节数。
  * @param[in] ulTimeoutMs 总超时时间，单位为毫秒。
  * @retval TransportResult_e 发送结果。
  */
static TransportResult_e prvSend(void *pvContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs);
/**
  * @brief  接收指定上限的字节并保留 netbuf 未消费部分。
  * @param[in,out] pvContext TransportTcpContext_t 上下文。
  * @param[out] pucData 接收缓冲区。
  * @param[in] usMaxLen 接收缓冲区容量。
  * @param[out] pusReceivedLen 实际接收字节数。
  * @param[in] ulTimeoutMs 接收总超时时间，单位为毫秒。
  * @retval TransportResult_e 接收结果。
  */
static TransportResult_e prvReceive(void *pvContext,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs);
/**
  * @brief  执行一个 Netconn 专用控制请求。
  * @param[in,out] pvContext TransportTcpContext_t 上下文。
  * @param[in] xCommand 控制命令。
  * @param[in,out] pvArgument 控制参数，按命令类型解释，可以为 NULL。
  * @retval TransportResult_e 控制结果。
  */
static TransportResult_e prvControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument);
/**
  * @brief  读取 Netconn 后端生命周期状态。
  * @param[in] pvContext TransportTcpContext_t 上下文。
  * @retval 当前 TransportState_e 状态。
  */
static TransportState_e prvGetState(void *pvContext);
/**
  * @brief  读取最新的 LwIP 原生错误值。
  * @param[in] pvContext TransportTcpContext_t 上下文。
  * @retval LwIP 错误值；上下文无效时返回 0。
  */
static int32_t prvGetNativeError(void *pvContext);
/**
  * @brief  创建并连接 Netconn 客户端端点。
  * @param[in,out] pxContext TCP 客户端上下文。
  * @retval TransportResult_e 连接结果。
  */
static TransportResult_e prvOpenClient(TransportTcpContext_t *pxContext);
/**
  * @brief  Wait for a nonblocking Netconn client connection to finish.
  * @param[in,out] pxContext TCP client context with a pending connection.
  * @param[in] ulTimeoutMs Total connection deadline in milliseconds.
  * @retval ERR_OK when connected, ERR_TIMEOUT at the deadline, or LwIP error.
  */
static err_t prvWaitClientConnect(TransportTcpContext_t *pxContext,
	uint32_t ulTimeoutMs);
/**
  * @brief Verify one pending Netconn connection inside the TCP/IP core.
  * @param[in,out] pvContext TransportTcpContext_t being checked.
  * @note This callback is the sole reader of the raw TCP PCB state.
  */
static void prvCheckClientConnectInCore(void *pvContext);
/**
  * @brief  创建、绑定并监听 Netconn Server 端点。
  * @param[in,out] pxContext TCP Server 上下文。
  * @retval TransportResult_e 监听结果。
  */
static TransportResult_e prvOpenServer(TransportTcpContext_t *pxContext);
/**
  * @brief  为单会话 Netconn Server 接收一个客户端。
  * @param[in,out] pxContext TCP Server 上下文。
  * @param[in] ulTimeoutMs 接收连接的超时时间，单位为毫秒。
  * @retval TransportResult_e 接收结果。
  */
static TransportResult_e prvAcceptClient(TransportTcpContext_t *pxContext,
	uint32_t ulTimeoutMs);
/**
  * @brief  关闭活动 Netconn 连接并释放缓存接收数据。
  * @param[in,out] pxContext TCP 上下文。
  */
static void prvCloseConnection(TransportTcpContext_t *pxContext);
/**
  * @brief  删除保留的 netbuf 并复位读取游标。
  * @param[in,out] pxContext TCP 上下文。
  */
static void prvDeleteNetbuf(TransportTcpContext_t *pxContext);
/**
  * @brief  将 LwIP err_t 映射为统一 TransportResult_e。
  * @param[in] xError LwIP 原生错误值。
  * @retval 规范化 TransportResult_e。
  */
static TransportResult_e prvMapLwipError(err_t xError);
/**
  * @brief  校验已接受的 Socket 会话是否已附着。
  * @param[in,out] pvContext Socket 会话上下文。
  * @retval TransportResult_e 校验结果。
  */
static TransportResult_e prvSocketOpen(void *pvContext);
/**
  * @brief  关闭并解除已接受的 Socket 描述符。
  * @param[in,out] pvContext Socket 会话上下文。
  * @retval TransportResult_e 关闭结果。
  */
static TransportResult_e prvSocketClose(void *pvContext);
/**
  * @brief  在非阻塞 Socket 上发送完整字节序列。
  * @param[in,out] pvContext Socket 会话上下文。
  * @param[in] pucData 待发送数据。
  * @param[in] usDataLen 待发送字节数。
  * @param[out] pusSentLen 实际发送字节数。
  * @param[in] ulTimeoutMs 总超时时间，单位为毫秒。
  * @retval TransportResult_e 发送结果。
  */
static TransportResult_e prvSocketSend(void *pvContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs);
/**
  * @brief  从非阻塞的已接受 Socket 接收字节。
  * @param[in,out] pvContext Socket 会话上下文。
  * @param[out] pucData 接收缓冲区。
  * @param[in] usMaxLen 接收缓冲区容量。
  * @param[out] pusReceivedLen 实际接收字节数。
  * @param[in] ulTimeoutMs 接收总超时时间，单位为毫秒。
  * @retval TransportResult_e 接收结果。
  */
static TransportResult_e prvSocketReceive(void *pvContext,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs);
/**
  * @brief  执行 Socket 会话控制请求。
  * @param[in,out] pvContext Socket 会话上下文。
  * @param[in] xCommand 控制命令。
  * @param[in,out] pvArgument 控制参数，按命令类型解释。
  * @retval TransportResult_e 控制结果。
  */
static TransportResult_e prvSocketControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument);
/**
  * @brief  读取已接受 Socket 会话的生命周期状态。
  * @param[in] pvContext Socket 会话上下文。
  * @retval 当前 TransportState_e 状态。
  */
static TransportState_e prvSocketGetState(void *pvContext);
/**
  * @brief  读取 Socket 最新 errno 值。
  * @param[in] pvContext Socket 会话上下文。
  * @retval Socket errno；上下文无效时返回 0。
  */
static int32_t prvSocketGetNativeError(void *pvContext);
/**
  * @brief  将 Socket errno 映射为统一 TransportResult_e。
  * @param[in] lError Socket 原生错误值。
  * @retval 规范化 TransportResult_e。
  */
static TransportResult_e prvMapSocketError(int lError);
/**
  * @brief  Append one unsigned decimal value to a bounded text buffer.
  * @param[in,out] pcText Output text buffer.
  * @param[in] usCapacity Buffer capacity including the terminator.
  * @param[in,out] pusLength Current and resulting text length.
  * @param[in] usValue Value to append.
  * @retval 1 Value was appended and space remains for the terminator.
  * @retval 0 Output capacity is insufficient or an argument is invalid.
  */
static uint8_t prvAppendUnsignedDecimal(char *pcText, uint16_t usCapacity,
	uint16_t *pusLength, uint16_t usValue);

/** @brief Netconn operation table for client and single-session server use. */
static const TransportOps_t s_xTcpOps = {
	prvOpen,
	prvClose,
	prvSend,
	prvReceive,
	prvControl,
	prvGetState,
	prvGetNativeError
};

/** @brief Socket operation table used by accepted session channels. */
static const TransportOps_t s_xTcpSocketOps = {
	prvSocketOpen,
	prvSocketClose,
	prvSocketSend,
	prvSocketReceive,
	prvSocketControl,
	prvSocketGetState,
	prvSocketGetNativeError
};

/* Formats an endpoint without pulling a stdio formatter into the firmware. */
/*-----------------------------------------------------------*/
uint8_t ucTransportTcpFormatIpv4Endpoint(const uint8_t pucIpv4[4],
	uint16_t usPort, char *pcText, uint16_t usCapacity)
{
	uint16_t usLength;
	uint8_t ucIndex;

	if ((pucIpv4 == NULL) || (pcText == NULL) || (usCapacity == 0U)) {
		return 0U;
	}
	pcText[0] = '\0';
	usLength = 0U;
	for (ucIndex = 0U; ucIndex < 4U; ucIndex++) {
		if (prvAppendUnsignedDecimal(pcText, usCapacity, &usLength,
			(uint16_t)pucIpv4[ucIndex]) == 0U) {
			pcText[0] = '\0';
			return 0U;
		}
		if (ucIndex < 3U) {
			if ((uint32_t)usLength + 1U >= (uint32_t)usCapacity) {
				pcText[0] = '\0';
				return 0U;
			}
			pcText[usLength++] = '.';
		}
	}
	if ((uint32_t)usLength + 1U >= (uint32_t)usCapacity) {
		pcText[0] = '\0';
		return 0U;
	}
	pcText[usLength++] = ':';
	if (prvAppendUnsignedDecimal(pcText, usCapacity, &usLength,
		usPort) == 0U) {
		pcText[0] = '\0';
		return 0U;
	}
	pcText[usLength] = '\0';
	return 1U;
}

/*-----------------------------------------------------------*/
static uint8_t prvAppendUnsignedDecimal(char *pcText, uint16_t usCapacity,
	uint16_t *pusLength, uint16_t usValue)
{
	char acReverseDigits[5];
	uint16_t usLength;
	uint8_t ucDigitCount;

	if ((pcText == NULL) || (pusLength == NULL) || (usCapacity == 0U)) {
		return 0U;
	}
	usLength = *pusLength;
	ucDigitCount = 0U;
	do {
		acReverseDigits[ucDigitCount++] =
			(char)('0' + (char)(usValue % 10U));
		usValue = (uint16_t)(usValue / 10U);
	} while ((usValue != 0U) &&
		(ucDigitCount < (uint8_t)sizeof(acReverseDigits)));
	if ((uint32_t)usLength + (uint32_t)ucDigitCount >=
		(uint32_t)usCapacity) {
		return 0U;
	}
	while (ucDigitCount > 0U) {
		ucDigitCount--;
		pcText[usLength++] = acReverseDigits[ucDigitCount];
	}
	*pusLength = usLength;
	return 1U;
}

/* Initializes caller-owned storage and registers the TCP operation table. */
/*-----------------------------------------------------------*/
TransportResult_e xTransportTcpCreate(TransportChannel_t *pxChannel,
	TransportTcpContext_t *pxContext, const char *pcName,
	const TransportTcpConfig_t *pxConfig)
{
	/* ① 参数校验。 */
	if ((pxChannel == NULL) || (pxContext == NULL) ||
		(pcName == NULL) || (pxConfig == NULL) ||
		(pxConfig->usPort == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if ((pxConfig->xMode != TRANSPORT_TCP_MODE_CLIENT) &&
		(pxConfig->xMode != TRANSPORT_TCP_MODE_SERVER)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	/* ② 清零上下文和通道。 */
	memset(pxContext, 0, sizeof(*pxContext));
	memset(pxChannel, 0, sizeof(*pxChannel));

	/* ③ 保存配置到上下文。 */
	pxContext->pxChannel = pxChannel;
	pxContext->xConfig = *pxConfig;
	pxContext->xState = TRANSPORT_STATE_CLOSED;
	IP_ADDR4(&pxContext->xRemoteAddress,
		pxConfig->aucRemoteIp[0], pxConfig->aucRemoteIp[1],
		pxConfig->aucRemoteIp[2], pxConfig->aucRemoteIp[3]); /* 设置远程 IP。 */

	/* ④ 绑定函数指针表。 */
	pxChannel->pcName = pcName;  		/* 通道名称，例如 RobotTcp。 */
	pxChannel->pxOps = &s_xTcpOps;  	/* TCP 操作表。 */
	pxChannel->pvContext = pxContext; 	/* 后端上下文。 */
	pxChannel->xState = TRANSPORT_STATE_CLOSED;

	/* ⑤ 调用 xTransportRegister 注册到全局表。 */
	return xTransportRegister(pxChannel);
}

/* Creates a reusable protocol channel before any client is accepted. */
/*-----------------------------------------------------------*/
TransportResult_e xTransportTcpSocketCreate(TransportChannel_t *pxChannel,
	TransportTcpSocketContext_t *pxContext, const char *pcName)
{
	if ((pxChannel == NULL) || (pxContext == NULL) || (pcName == NULL)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	memset(pxContext, 0, sizeof(*pxContext));
	memset(pxChannel, 0, sizeof(*pxChannel));
	pxContext->pxChannel = pxChannel;
	pxContext->lSocket = -1;
	pxContext->xState = TRANSPORT_STATE_CLOSED;
	pxChannel->pcName = pcName;
	pxChannel->pxOps = &s_xTcpSocketOps;
	pxChannel->pvContext = pxContext;
	pxChannel->xState = TRANSPORT_STATE_CLOSED;
	return xTransportRegister(pxChannel);
}

/* Attaches one accepted socket without allocating a protocol object. */
/*-----------------------------------------------------------*/
TransportResult_e xTransportTcpSocketAttach(TransportChannel_t *pxChannel,
	TransportTcpSocketContext_t *pxContext, int lSocket)
{
	if ((pxChannel == NULL) || (pxContext == NULL) || (lSocket < 0) ||
		(pxContext->pxChannel != pxChannel)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if (pxContext->lSocket >= 0) {
		return TRANSPORT_RESULT_BUSY;
	}
	pxContext->lSocket = lSocket;
	pxContext->lLastNativeError = 0;
	pxContext->xState = TRANSPORT_STATE_CLOSED;
	return xTransportOpen(pxChannel);
}

/* Selects client connect or server listen according to immutable config. */
/*-----------------------------------------------------------*/
static TransportResult_e prvOpen(void *pvContext)
{
	TransportTcpContext_t *pxContext;

	pxContext = (TransportTcpContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if (pxContext->xState == TRANSPORT_STATE_OPEN) {
		return TRANSPORT_RESULT_OK;
	}

	pxContext->xState = TRANSPORT_STATE_BUSY;
	if (pxContext->xConfig.xMode == TRANSPORT_TCP_MODE_CLIENT) {
		return prvOpenClient(pxContext);
	}
	return prvOpenServer(pxContext);
}

/* Creates one netconn and performs the bounded robot TCP connection. */
/*-----------------------------------------------------------*/
static TransportResult_e prvOpenClient(TransportTcpContext_t *pxContext)
{
	err_t xError;

	if ((netif_default == NULL) || (netif_is_up(netif_default) == 0) ||
		(netif_is_link_up(netif_default) == 0) ||
		ip_addr_isany(netif_ip_addr4(netif_default))) {
		pxContext->lLastNativeError = (int32_t)ERR_IF;
		pxContext->xState = TRANSPORT_STATE_ERROR;
		return TRANSPORT_RESULT_NOT_READY;
	}

	pxContext->pxConnection = netconn_new(NETCONN_TCP);
	if (pxContext->pxConnection == NULL) {
		pxContext->lLastNativeError = (int32_t)ERR_MEM;
		pxContext->xState = TRANSPORT_STATE_ERROR;
		return TRANSPORT_RESULT_NO_RESOURCE;
	}

	netconn_set_recvtimeout(pxContext->pxConnection,
		pxContext->xConfig.ulIoTimeoutMs);
	netconn_set_sendtimeout(pxContext->pxConnection,
		pxContext->xConfig.ulIoTimeoutMs);
	if (pxContext->xConfig.ulConnectTimeoutMs != 0U) {
		netconn_set_nonblocking(pxContext->pxConnection, 1);
	}
	xError = netconn_connect(pxContext->pxConnection,
		&pxContext->xRemoteAddress, pxContext->xConfig.usPort);
	if ((xError == ERR_INPROGRESS) &&
		(pxContext->xConfig.ulConnectTimeoutMs != 0U)) {
		xError = prvWaitClientConnect(pxContext,
			pxContext->xConfig.ulConnectTimeoutMs);
	}
	if ((xError == ERR_OK) && (pxContext->pxConnection != NULL)) {
		netconn_set_nonblocking(pxContext->pxConnection, 0);
	}
	pxContext->lLastNativeError = (int32_t)xError;
	if (xError != ERR_OK) {
		prvCloseConnection(pxContext);
		pxContext->xState =
			((pxContext->xConfig.xMode == TRANSPORT_TCP_MODE_SERVER) &&
			 (pxContext->pxListener != NULL)) ? TRANSPORT_STATE_OPEN :
			 TRANSPORT_STATE_ERROR;
		return prvMapLwipError(xError);
	}

	pxContext->xState = TRANSPORT_STATE_OPEN;
	return TRANSPORT_RESULT_OK;
}

/* Waits for LwIP's nonblocking connect callback or the local deadline. */
/*-----------------------------------------------------------*/
static err_t prvWaitClientConnect(TransportTcpContext_t *pxContext,
	uint32_t ulTimeoutMs)
{
	uint32_t ulStartMs;
	err_t xError;

	ulStartMs = sys_now();
	for (;;) {
		pxContext->ucConnectCheckComplete = 0U;
		xError = tcpip_callback_with_block(prvCheckClientConnectInCore,
			pxContext, 1);
		if (xError != ERR_OK) {
			return xError;
		}
		while (pxContext->ucConnectCheckComplete == 0U) {
			vTaskDelay(pdMS_TO_TICKS(1U));
		}
		xError = pxContext->xConnectCheckResult;
		if (xError != ERR_INPROGRESS) {
			return xError;
		}
		if ((sys_now() - ulStartMs) >= ulTimeoutMs) {
			return ERR_TIMEOUT;
		}
		vTaskDelay(pdMS_TO_TICKS(10U));
	}
}

/* Verify a completed nonblocking connect in the TCP/IP core only. */
/*-----------------------------------------------------------*/
static void prvCheckClientConnectInCore(void *pvContext)
{
	TransportTcpContext_t *pxContext;
	err_t xError;

	pxContext = (TransportTcpContext_t *)pvContext;
	xError = ERR_ARG;
	if ((pxContext != NULL) && (pxContext->pxConnection != NULL)) {
		if (netconn_is_flag_set(pxContext->pxConnection,
			NETCONN_FLAG_IN_NONBLOCKING_CONNECT)) {
			xError = ERR_INPROGRESS;
		} else if (pxContext->pxConnection->pcb.tcp == NULL) {
			xError = pxContext->pxConnection->pending_err;
			if (xError == ERR_OK) {
				xError = ERR_CLSD;
			}
		} else if (pxContext->pxConnection->pcb.tcp->state ==
			ESTABLISHED) {
			xError = ERR_OK;
		} else {
			xError = ERR_CONN;
		}
	}
	pxContext->xConnectCheckResult = xError;
	pxContext->ucConnectCheckComplete = 1U;
}

/* Creates and binds a generic server listener; accept occurs on receive. */
/*-----------------------------------------------------------*/
static TransportResult_e prvOpenServer(TransportTcpContext_t *pxContext)
{
	err_t xError;

	pxContext->pxListener = netconn_new(NETCONN_TCP);
	if (pxContext->pxListener == NULL) {
		pxContext->lLastNativeError = (int32_t)ERR_MEM;
		pxContext->xState = TRANSPORT_STATE_ERROR;
		return TRANSPORT_RESULT_NO_RESOURCE;
	}

	xError = netconn_bind(pxContext->pxListener, IP_ADDR_ANY,
		pxContext->xConfig.usPort);
	if (xError == ERR_OK) {
		xError = netconn_listen(pxContext->pxListener);
	}
	pxContext->lLastNativeError = (int32_t)xError;
	if (xError != ERR_OK) {
		(void)netconn_delete(pxContext->pxListener);
		pxContext->pxListener = NULL;
		pxContext->xState = TRANSPORT_STATE_ERROR;
		return prvMapLwipError(xError);
	}

	pxContext->xState = TRANSPORT_STATE_OPEN;
	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static TransportResult_e prvClose(void *pvContext)
{
	TransportTcpContext_t *pxContext;

	pxContext = (TransportTcpContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}

	prvCloseConnection(pxContext);
	if (pxContext->pxListener != NULL) {
		(void)netconn_close(pxContext->pxListener);
		(void)netconn_delete(pxContext->pxListener);
		pxContext->pxListener = NULL;
	}
	pxContext->xState = TRANSPORT_STATE_CLOSED;
	return TRANSPORT_RESULT_OK;
}

/* Sends all requested bytes or returns a mapped LwIP error. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSend(void *pvContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs)
{
	TransportTcpContext_t *pxContext;
	err_t xError;
	size_t uxBytesWritten;
	size_t uxTotalWritten;
	uint32_t ulStartMs;
	uint32_t ulElapsedMs;
	uint32_t ulRemainingMs;

	pxContext = (TransportTcpContext_t *)pvContext;
	if ((pxContext == NULL) || (pucData == NULL) ||
		(pusSentLen == NULL) || (usDataLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	*pusSentLen = 0U;
	if (pxContext->pxConnection == NULL) {
		return TRANSPORT_RESULT_NOT_OPEN;
	}

	uxTotalWritten = 0U;
	ulStartMs = sys_now();
	xError = ERR_OK;
	while (uxTotalWritten < (size_t)usDataLen) {
		ulRemainingMs = 0U;
		if (ulTimeoutMs != 0U) {
			ulElapsedMs = sys_now() - ulStartMs;
			if (ulElapsedMs >= ulTimeoutMs) {
				xError = ERR_TIMEOUT;
				break;
			}
			ulRemainingMs = ulTimeoutMs - ulElapsedMs;
			if (ulRemainingMs == 0U) {
				ulRemainingMs = 1U;
			}
		}
		netconn_set_sendtimeout(pxContext->pxConnection, ulRemainingMs);
		uxBytesWritten = 0U;
		xError = netconn_write_partly(pxContext->pxConnection,
			&pucData[uxTotalWritten],
			(size_t)usDataLen - uxTotalWritten, NETCONN_COPY,
			&uxBytesWritten);
		if (uxBytesWritten > ((size_t)usDataLen - uxTotalWritten)) {
			xError = ERR_VAL;
			break;
		}
		uxTotalWritten += uxBytesWritten;
		*pusSentLen = (uint16_t)uxTotalWritten;
		if (xError != ERR_OK) {
			break;
		}
		if (uxBytesWritten == 0U) {
			xError = ERR_TIMEOUT;
			break;
		}
	}
	pxContext->lLastNativeError = (int32_t)xError;
	if ((xError != ERR_OK) || (uxTotalWritten != (size_t)usDataLen)) {
		prvCloseConnection(pxContext);
		pxContext->xState =
			((pxContext->xConfig.xMode == TRANSPORT_TCP_MODE_SERVER) &&
			 (pxContext->pxListener != NULL)) ? TRANSPORT_STATE_OPEN :
			 TRANSPORT_STATE_ERROR;
		return prvMapLwipError(xError);
	}
	return TRANSPORT_RESULT_OK;
}

/*
 * Receives from the active connection and preserves unused netbuf data.
 * Server mode accepts a client here before waiting for its first request.
 */
/*-----------------------------------------------------------*/
static TransportResult_e prvReceive(void *pvContext,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs)
{
	TransportTcpContext_t *pxContext;
	TransportResult_e xResult;
	uint16_t usAvailable;
	uint16_t usCopyLen;
	u16_t usCopied;
	err_t xError;

	pxContext = (TransportTcpContext_t *)pvContext;
	if ((pxContext == NULL) || (pucData == NULL) ||
		(pusReceivedLen == NULL) || (usMaxLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	*pusReceivedLen = 0U;

	if ((pxContext->xConfig.xMode == TRANSPORT_TCP_MODE_SERVER) &&
		(pxContext->pxConnection == NULL)) {
		xResult = prvAcceptClient(pxContext, ulTimeoutMs);
		if (xResult != TRANSPORT_RESULT_OK) {
			return xResult;
		}
	}
	if (pxContext->pxConnection == NULL) {
		return TRANSPORT_RESULT_NOT_OPEN;
	}

	if (pxContext->pxRxBuffer == NULL) {
		netconn_set_recvtimeout(pxContext->pxConnection, ulTimeoutMs);
		xError = netconn_recv(pxContext->pxConnection,
			&pxContext->pxRxBuffer);
		pxContext->lLastNativeError = (int32_t)xError;
		if ((xError != ERR_OK) || (pxContext->pxRxBuffer == NULL)) {
			if (xError != ERR_TIMEOUT) {
				prvCloseConnection(pxContext);
				pxContext->xState =
					((pxContext->xConfig.xMode ==
					  TRANSPORT_TCP_MODE_SERVER) &&
					 (pxContext->pxListener != NULL)) ?
					 TRANSPORT_STATE_OPEN : TRANSPORT_STATE_ERROR;
			}
			return prvMapLwipError(xError);
		}
		pxContext->usRxOffset = 0U;
	}

	usAvailable = (uint16_t)(netbuf_len(pxContext->pxRxBuffer) -
		pxContext->usRxOffset);
	usCopyLen = (usAvailable < usMaxLen) ? usAvailable : usMaxLen;
	usCopied = netbuf_copy_partial(pxContext->pxRxBuffer, pucData,
		usCopyLen, pxContext->usRxOffset);
	if (usCopied != usCopyLen) {
		prvCloseConnection(pxContext);
		return TRANSPORT_RESULT_IO_ERROR;
	}

	pxContext->usRxOffset = (uint16_t)(pxContext->usRxOffset + usCopyLen);
	*pusReceivedLen = usCopyLen;
	if (pxContext->usRxOffset >= netbuf_len(pxContext->pxRxBuffer)) {
		prvDeleteNetbuf(pxContext);
	}
	return TRANSPORT_RESULT_OK;
}

/* Accepts one server client with the configured receive timeout. */
/*-----------------------------------------------------------*/
static TransportResult_e prvAcceptClient(TransportTcpContext_t *pxContext,
	uint32_t ulTimeoutMs)
{
	err_t xError;

	if (pxContext->pxListener == NULL) {
		return TRANSPORT_RESULT_NOT_OPEN;
	}
	netconn_set_recvtimeout(pxContext->pxListener, ulTimeoutMs);
	xError = netconn_accept(pxContext->pxListener,
		&pxContext->pxConnection);
	pxContext->lLastNativeError = (int32_t)xError;
	if ((xError != ERR_OK) || (pxContext->pxConnection == NULL)) {
		pxContext->pxConnection = NULL;
		return prvMapLwipError(xError);
	}

	netconn_set_recvtimeout(pxContext->pxConnection,
		pxContext->xConfig.ulIoTimeoutMs);
	netconn_set_sendtimeout(pxContext->pxConnection,
		pxContext->xConfig.ulIoTimeoutMs);
	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static TransportResult_e prvControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument)
{
	TransportTcpContext_t *pxContext;

	(void)pvArgument;
	pxContext = (TransportTcpContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if (xCommand != TRANSPORT_CTRL_CONNECTION_RESET) {
		return TRANSPORT_RESULT_NOT_SUPPORTED;
	}

	prvCloseConnection(pxContext);
	if (pxContext->xConfig.xMode == TRANSPORT_TCP_MODE_CLIENT) {
		pxContext->xState = TRANSPORT_STATE_CLOSED;
	} else if (pxContext->pxListener != NULL) {
		pxContext->xState = TRANSPORT_STATE_OPEN;
	}
	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/
static TransportState_e prvGetState(void *pvContext)
{
	TransportTcpContext_t *pxContext;

	pxContext = (TransportTcpContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_STATE_UNINITIALIZED;
	}
	return pxContext->xState;
}

/*-----------------------------------------------------------*/
static int32_t prvGetNativeError(void *pvContext)
{
	TransportTcpContext_t *pxContext;

	pxContext = (TransportTcpContext_t *)pvContext;
	if (pxContext == NULL) {
		return (int32_t)ERR_ARG;
	}
	return pxContext->lLastNativeError;
}

/*-----------------------------------------------------------*/
static void prvCloseConnection(TransportTcpContext_t *pxContext)
{
	prvDeleteNetbuf(pxContext);
	if (pxContext->pxConnection != NULL) {
		(void)netconn_close(pxContext->pxConnection);
		(void)netconn_delete(pxContext->pxConnection);
		pxContext->pxConnection = NULL;
	}
}

/*-----------------------------------------------------------*/
static void prvDeleteNetbuf(TransportTcpContext_t *pxContext)
{
	if (pxContext->pxRxBuffer != NULL) {
		netbuf_delete(pxContext->pxRxBuffer);
		pxContext->pxRxBuffer = NULL;
	}
	pxContext->usRxOffset = 0U;
}

/* Opens a socket channel only after the listener has attached a descriptor. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSocketOpen(void *pvContext)
{
	TransportTcpSocketContext_t *pxContext;

	pxContext = (TransportTcpSocketContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if (pxContext->lSocket < 0) {
		pxContext->xState = TRANSPORT_STATE_CLOSED;
		return TRANSPORT_RESULT_NOT_OPEN;
	}
	pxContext->xState = TRANSPORT_STATE_OPEN;
	return TRANSPORT_RESULT_OK;
}

/* Closes one accepted socket and returns its fixed slot to the listener. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSocketClose(void *pvContext)
{
	TransportTcpSocketContext_t *pxContext;
	int lSocket;
	int lResult;

	pxContext = (TransportTcpSocketContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	lSocket = pxContext->lSocket;
	pxContext->lSocket = -1;
	pxContext->xState = TRANSPORT_STATE_CLOSED;
	if (lSocket < 0) {
		return TRANSPORT_RESULT_OK;
	}
	(void)lwip_shutdown(lSocket, SHUT_RDWR);
	lResult = lwip_close(lSocket);
	if (lResult == 0) {
		pxContext->lLastNativeError = 0;
		return TRANSPORT_RESULT_OK;
	}
	pxContext->lLastNativeError = errno;
	return prvMapSocketError(errno);
}

/* Sends a complete Modbus response through one nonblocking client socket. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSocketSend(void *pvContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs)
{
	TransportTcpSocketContext_t *pxContext;
	uint32_t ulStartMs;
	uint32_t ulElapsedMs;
	uint16_t usOffset;
	int lSent;

	pxContext = (TransportTcpSocketContext_t *)pvContext;
	if ((pxContext == NULL) || (pucData == NULL) ||
		(pusSentLen == NULL) || (usDataLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	*pusSentLen = 0U;
	if ((pxContext->lSocket < 0) ||
		(pxContext->xState != TRANSPORT_STATE_OPEN)) {
		return TRANSPORT_RESULT_NOT_OPEN;
	}
	ulStartMs = sys_now();
	usOffset = 0U;
	while (usOffset < usDataLen) {
		lSent = lwip_send(pxContext->lSocket, &pucData[usOffset],
			(size_t)(usDataLen - usOffset), MSG_DONTWAIT);
		if (lSent > 0) {
			usOffset = (uint16_t)(usOffset + (uint16_t)lSent);
			*pusSentLen = usOffset;
			continue;
		}
		if (lSent == 0) {
			pxContext->lLastNativeError = ECONNRESET;
			pxContext->xState = TRANSPORT_STATE_ERROR;
			return TRANSPORT_RESULT_DISCONNECTED;
		}
		pxContext->lLastNativeError = errno;
		if ((errno != EWOULDBLOCK) && (errno != EAGAIN)) {
			pxContext->xState = TRANSPORT_STATE_ERROR;
			return prvMapSocketError(errno);
		}
		ulElapsedMs = sys_now() - ulStartMs;
		if ((ulTimeoutMs == 0U) || (ulElapsedMs >= ulTimeoutMs)) {
			return TRANSPORT_RESULT_TIMEOUT;
		}
		vTaskDelay(pdMS_TO_TICKS(1U));
	}
	pxContext->lLastNativeError = 0;
	return TRANSPORT_RESULT_OK;
}

/* Receives currently available bytes without blocking another client slot. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSocketReceive(void *pvContext,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs)
{
	TransportTcpSocketContext_t *pxContext;
	int lReceived;

	(void)ulTimeoutMs;
	pxContext = (TransportTcpSocketContext_t *)pvContext;
	if ((pxContext == NULL) || (pucData == NULL) ||
		(pusReceivedLen == NULL) || (usMaxLen == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	*pusReceivedLen = 0U;
	if ((pxContext->lSocket < 0) ||
		(pxContext->xState != TRANSPORT_STATE_OPEN)) {
		return TRANSPORT_RESULT_NOT_OPEN;
	}
	lReceived = lwip_recv(pxContext->lSocket, pucData, usMaxLen,
		MSG_DONTWAIT);
	if (lReceived > 0) {
		*pusReceivedLen = (uint16_t)lReceived;
		pxContext->lLastNativeError = 0;
		return TRANSPORT_RESULT_OK;
	}
	if (lReceived == 0) {
		pxContext->lLastNativeError = ECONNRESET;
		pxContext->xState = TRANSPORT_STATE_ERROR;
		return TRANSPORT_RESULT_DISCONNECTED;
	}
	pxContext->lLastNativeError = errno;
	if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
		return TRANSPORT_RESULT_TIMEOUT;
	}
	pxContext->xState = TRANSPORT_STATE_ERROR;
	return prvMapSocketError(errno);
}

/* Resets an accepted socket without changing the fixed channel allocation. */
/*-----------------------------------------------------------*/
static TransportResult_e prvSocketControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument)
{
	TransportTcpSocketContext_t *pxContext;
	struct linger xLinger;

	(void)pvArgument;
	if (xCommand != TRANSPORT_CTRL_CONNECTION_RESET) {
		return TRANSPORT_RESULT_NOT_SUPPORTED;
	}
	pxContext = (TransportTcpSocketContext_t *)pvContext;
	if (pxContext == NULL) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if (pxContext->lSocket >= 0) {
		xLinger.l_onoff = 1;
		xLinger.l_linger = 0;
		(void)lwip_setsockopt(pxContext->lSocket, SOL_SOCKET, SO_LINGER,
			&xLinger, sizeof(xLinger));
	}
	return prvSocketClose(pvContext);
}

/*-----------------------------------------------------------*/
static TransportState_e prvSocketGetState(void *pvContext)
{
	TransportTcpSocketContext_t *pxContext;

	pxContext = (TransportTcpSocketContext_t *)pvContext;
	return (pxContext != NULL) ? pxContext->xState :
		TRANSPORT_STATE_UNINITIALIZED;
}

/*-----------------------------------------------------------*/
static int32_t prvSocketGetNativeError(void *pvContext)
{
	TransportTcpSocketContext_t *pxContext;

	pxContext = (TransportTcpSocketContext_t *)pvContext;
	return (pxContext != NULL) ? pxContext->lLastNativeError : EINVAL;
}

/* Maps socket errno values into the same backend-neutral Transport results. */
/*-----------------------------------------------------------*/
static TransportResult_e prvMapSocketError(int lError)
{
	switch (lError) {
	case 0:
		return TRANSPORT_RESULT_OK;
	case EWOULDBLOCK:
#if (EAGAIN != EWOULDBLOCK)
	case EAGAIN:
#endif
	case ETIMEDOUT:
		return TRANSPORT_RESULT_TIMEOUT;
	case ECONNRESET:
	case ECONNABORTED:
	case ENOTCONN:
	case EPIPE:
		return TRANSPORT_RESULT_DISCONNECTED;
	case ENOMEM:
	case ENOBUFS:
		return TRANSPORT_RESULT_NO_RESOURCE;
	case ENETDOWN:
	case ENETUNREACH:
	case EHOSTUNREACH:
		return TRANSPORT_RESULT_NOT_READY;
	default:
		return TRANSPORT_RESULT_IO_ERROR;
	}
}

/* Converts LwIP err_t values into stable errors used by protocol code. */
/*-----------------------------------------------------------*/
static TransportResult_e prvMapLwipError(err_t xError)
{
	switch (xError) {
	case ERR_OK:
		return TRANSPORT_RESULT_OK;
	case ERR_TIMEOUT:
	case ERR_WOULDBLOCK:
		return TRANSPORT_RESULT_TIMEOUT;
	case ERR_MEM:
	case ERR_BUF:
		return TRANSPORT_RESULT_NO_RESOURCE;
	case ERR_INPROGRESS:
	case ERR_ALREADY:
		return TRANSPORT_RESULT_BUSY;
	case ERR_RST:
	case ERR_ABRT:
	case ERR_CLSD:
	case ERR_CONN:
		return TRANSPORT_RESULT_DISCONNECTED;
	case ERR_RTE:
	case ERR_IF:
		return TRANSPORT_RESULT_NOT_READY;
	default:
		return TRANSPORT_RESULT_IO_ERROR;
	}
}
