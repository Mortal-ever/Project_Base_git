/**
 * @file      transport_tcp.c
 * @brief     基于 LwIP Netconn 与非阻塞 Socket 的 TCP Transport 后端。
 * @author    WHong
 * @date      2026-07-28
 *
 * @details
 * 本文件实现两套 TCP 后端：
 * - s_xTcpOps：Netconn Client 与单活动会话 Server；
 * - s_xTcpSocketOps：由外部 Listener accept 后附着的固定 Socket 会话通道。
 *
 * 两种后端都通过 TransportOps_t 暴露统一 open/close/send/receive/control/state/error 接口，
 * 并把具体实例通过 pvContext 传回。Channel 与 Context 存储由调用者拥有；Netconn、netbuf
 * 或已附着 Socket 描述符则由对应 Context 在打开/关闭过程中管理。
 *
 * TCP 是字节流协议，单次发送/接收不保证对应一整帧，因此实现中显式处理短写、短读、
 * netbuf 剩余数据以及总超时预算，避免把一次底层调用误当成完整协议事务。
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

static TransportResult_e prvOpen(void *pvContext);

static TransportResult_e prvClose(void *pvContext);

static TransportResult_e prvSend(void *pvContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs);

static TransportResult_e prvReceive(void *pvContext,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs);

static TransportResult_e prvControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument);

static TransportState_e prvGetState(void *pvContext);

static int32_t prvGetNativeError(void *pvContext);

static TransportResult_e prvOpenClient(TransportTcpContext_t *pxContext);

static err_t prvWaitClientConnect(TransportTcpContext_t *pxContext,
	uint32_t ulTimeoutMs);

static void prvCheckClientConnectInCore(void *pvContext);

static TransportResult_e prvOpenServer(TransportTcpContext_t *pxContext);

static TransportResult_e prvAcceptClient(TransportTcpContext_t *pxContext,
	uint32_t ulTimeoutMs);

static void prvCloseConnection(TransportTcpContext_t *pxContext);

static void prvDeleteNetbuf(TransportTcpContext_t *pxContext);

static TransportResult_e prvMapLwipError(err_t xError);

static TransportResult_e prvSocketOpen(void *pvContext);

static TransportResult_e prvSocketClose(void *pvContext);

static TransportResult_e prvSocketSend(void *pvContext,
	const uint8_t *pucData, uint16_t usDataLen, uint16_t *pusSentLen,
	uint32_t ulTimeoutMs);

static TransportResult_e prvSocketReceive(void *pvContext,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs);

static TransportResult_e prvSocketControl(void *pvContext,
	TransportControl_e xCommand, void *pvArgument);

static TransportState_e prvSocketGetState(void *pvContext);

static int32_t prvSocketGetNativeError(void *pvContext);

static TransportResult_e prvMapSocketError(int lError);

static uint8_t prvAppendUnsignedDecimal(char *pcText, uint16_t usCapacity,
	uint16_t *pusLength, uint16_t usValue);

/** @brief Netconn Client / 单会话 Server 共用的后端操作表。 */
static const TransportOps_t s_xTcpOps = {
	prvOpen,
	prvClose,
	prvSend,
	prvReceive,
	prvControl,
	prvGetState,
	prvGetNativeError
};

/** @brief 已接受 Socket 会话使用的后端操作表。 */
static const TransportOps_t s_xTcpSocketOps = {
	prvSocketOpen,
	prvSocketClose,
	prvSocketSend,
	prvSocketReceive,
	prvSocketControl,
	prvSocketGetState,
	prvSocketGetNativeError
};

/*-----------------------------------------------------------*/

/**
 * @brief  将 IPv4 地址和端口格式化为受限长度的 "a.b.c.d:port" 文本。
 *
 * @details
 * 为避免在固件中额外引入 printf 格式化开销，本函数逐段追加十进制数字和分隔符。
 * 每次写入前都检查剩余容量，并始终为字符串结尾的 '\0' 预留空间；任一步失败都会把
 * 输出首字符清零，使调用者得到明确的空字符串。
 *
 * @param[in]  pucIpv4    四字节 IPv4 地址。
 * @param[in]  usPort     TCP 端口。
 * @param[out] pcText     输出字符缓冲区。
 * @param[in]  usCapacity 缓冲区总容量，包含结尾 '\0'。
 *
 * @retval 1 格式化成功。
 * @retval 0 参数无效或缓冲区容量不足。
 */

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

/**
 * @brief  向有界文本缓冲区追加一个 uint16_t 十进制数。
 *
 * @details
 * 先把低位到高位数字写入局部反向数组，再逆序复制到目标缓冲区。函数不负责追加 '\0'，
 * 但在容量判断中始终保留一个终止字符空间，由上层统一结束字符串。
 *
 * @param[in,out] pcText      输出缓冲区。
 * @param[in]     usCapacity  缓冲区容量。
 * @param[in,out] pusLength   当前长度，成功后更新为新长度。
 * @param[in]     usValue     待追加数值。
 *
 * @retval 1 追加成功。
 * @retval 0 参数无效或容量不足。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  创建并注册一个基于 LwIP Netconn 的 TCP Transport 通道。
 *
 * @details
 * 调用者提供 Channel、Context、名称和配置存储；本函数清零 Channel/Context，将配置按值
 * 复制到 Context，并绑定静态 s_xTcpOps 操作表和 pvContext。随后调用 xTransportRegister()
 * 把 Channel 指针加入公共注册表。Channel/Context 本体仍由调用者拥有，本函数不动态分配它们。
 *
 * @param[out] pxChannel  调用者提供的 TransportChannel_t。
 * @param[out] pxContext  调用者提供的 Netconn TCP Context。
 * @param[in]  pcName     通道名称；注册期间必须持续有效。
 * @param[in]  pxConfig   TCP 配置，函数内部按值复制。
 *
 * @retval TRANSPORT_RESULT_OK 创建并注册成功。
 * @retval TRANSPORT_RESULT_INVALID_ARG 参数、端口或模式无效。
 * @retval 其他 TransportResult_e 注册失败。
 */

TransportResult_e xTransportTcpCreate(TransportChannel_t *pxChannel,
	TransportTcpContext_t *pxContext, const char *pcName,
	const TransportTcpConfig_t *pxConfig)
{
	
	if ((pxChannel == NULL) || (pxContext == NULL) ||
		(pcName == NULL) || (pxConfig == NULL) ||
		(pxConfig->usPort == 0U)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	if ((pxConfig->xMode != TRANSPORT_TCP_MODE_CLIENT) &&
		(pxConfig->xMode != TRANSPORT_TCP_MODE_SERVER)) {
		return TRANSPORT_RESULT_INVALID_ARG;
	}
	
	memset(pxContext, 0, sizeof(*pxContext));
	memset(pxChannel, 0, sizeof(*pxChannel));

	pxContext->pxChannel = pxChannel;
	/* 配置结构按值复制；其中若含指针成员，指针目标仍由外部保证生命周期。 */
	pxContext->xConfig = *pxConfig;
	pxContext->xState = TRANSPORT_STATE_CLOSED;
	IP_ADDR4(&pxContext->xRemoteAddress,
		pxConfig->aucRemoteIp[0], pxConfig->aucRemoteIp[1],
		pxConfig->aucRemoteIp[2], pxConfig->aucRemoteIp[3]); 

	pxChannel->pcName = pcName;  		
	/* 绑定共享 Netconn 操作表；pvContext 再把调用恢复到当前 TCP 实例。 */
	pxChannel->pxOps = &s_xTcpOps;  	
	pxChannel->pvContext = pxContext; 	
	pxChannel->xState = TRANSPORT_STATE_CLOSED;

	return xTransportRegister(pxChannel);
}

/*-----------------------------------------------------------*/

/**
 * @brief  创建并注册一个可复用的“已接受 Socket 会话”Transport 通道。
 *
 * @details
 * 该接口只建立固定 Channel/Context 和 Socket 操作表，不创建或 accept 套接字。
 * Context 初始 lSocket 为 -1，后续由 xTransportTcpSocketAttach() 把外部已接受的描述符
 * 绑定进来。这样协议对象可长期存在，而每次客户端连接只替换底层 socket 描述符。
 *
 * @param[out] pxChannel 调用者提供的 Channel。
 * @param[out] pxContext 调用者提供的 Socket Context。
 * @param[in]  pcName    通道名称。
 *
 * @retval TransportResult_e 创建或注册结果。
 */

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

/*-----------------------------------------------------------*/

/**
 * @brief  把一个已经 accept 的 Socket 描述符附着到固定 Transport 通道。
 *
 * @details
 * 本函数不创建协议对象，只把 lSocket 写入与 pxChannel 匹配的 Context，然后通过
 * xTransportOpen() 验证并把通道置为 OPEN。若 Context 已经持有描述符则返回 BUSY，
 * 防止覆盖仍在使用的会话。
 *
 * @param[in,out] pxChannel  目标固定通道。
 * @param[in,out] pxContext  与该通道绑定的 Socket Context。
 * @param[in]     lSocket    已接受的有效 Socket 描述符。
 *
 * @retval TransportResult_e 附着并打开结果。
 */

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

/*-----------------------------------------------------------*/

/**
 * @brief  根据 TCP Context 配置选择 Netconn Client 或 Server 打开路径。
 *
 * @details
 * s_xTcpOps 的 xOpen 统一指向本函数。它首先把 Context 转回 TransportTcpContext_t，
 * 若已 OPEN 则幂等返回；否则进入 BUSY，并依据 xConfig.xMode 分派到 prvOpenClient()
 * 或 prvOpenServer()。
 *
 * @param[in,out] pvContext TransportTcpContext_t 上下文。
 *
 * @retval TransportResult_e Client 连接或 Server 监听结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  创建 Netconn TCP Client，并在配置的截止时间内完成连接。
 *
 * @details
 * 连接前先检查默认网络接口是否存在、UP、Link UP 且拥有有效 IPv4。随后创建 netconn，
 * 可选通过 usReserveLocalPort() 取得本地端口并 bind，再设置收发超时。配置了连接超时时，
 * 使用 nonblocking connect + prvWaitClientConnect() 实现有界连接；成功后恢复 blocking 模式。
 * 任一步失败都会记录 LwIP 原生错误并关闭已创建连接资源。
 *
 * @param[in,out] pxContext TCP Client Context。
 *
 * @retval TRANSPORT_RESULT_OK 已连接。
 * @retval 其他 TransportResult_e 网络未就绪、资源不足、超时或连接错误。
 */
static TransportResult_e prvOpenClient(TransportTcpContext_t *pxContext)
{
	err_t xError;
	uint16_t usLocalPort;

	if ((netif_default == NULL) || (netif_is_up(netif_default) == 0) ||
		(netif_is_link_up(netif_default) == 0) ||
		ip_addr_isany(netif_ip_addr4(netif_default))) {
		pxContext->lLastNativeError = (int32_t)ERR_IF;
		pxContext->xState = TRANSPORT_STATE_ERROR;
		return TRANSPORT_RESULT_NOT_READY;
	}

	/* Netconn 对象属于当前 Context 的运行期资源，失败/关闭路径负责释放。 */
	pxContext->pxConnection = netconn_new(NETCONN_TCP);
	if (pxContext->pxConnection == NULL) {
		pxContext->lLastNativeError = (int32_t)ERR_MEM;
		pxContext->xState = TRANSPORT_STATE_ERROR;
		return TRANSPORT_RESULT_NO_RESOURCE;
	}

	if (pxContext->usReserveLocalPort != NULL) {
		usLocalPort = pxContext->usReserveLocalPort();
		xError = (usLocalPort == 0U) ? ERR_VAL :
			netconn_bind(pxContext->pxConnection, IP_ADDR_ANY, usLocalPort);
		if (xError != ERR_OK) {
			pxContext->lLastNativeError = (int32_t)xError;
			prvCloseConnection(pxContext);
			return prvMapLwipError(xError);
		}
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

/*-----------------------------------------------------------*/

/**
 * @brief  轮询等待非阻塞 Netconn Client 连接完成。
 *
 * @details
 * 使用 sys_now() 建立总连接截止时间，并通过 tcpip_callback_with_block() 把 PCB 状态检查
 * 放到 LwIP TCP/IP Core 执行。只要状态仍为 ERR_INPROGRESS 就周期性重试，超过总预算返回
 * ERR_TIMEOUT；该循环不会在每次检查时重置连接总超时。
 *
 * @param[in,out] pxContext   正在连接的 TCP Context。
 * @param[in]     ulTimeoutMs 总连接超时时间。
 *
 * @retval ERR_OK 连接建立。
 * @retval ERR_TIMEOUT 总预算耗尽。
 * @retval 其他 err_t LwIP 检查或连接错误。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  在 LwIP TCP/IP Core 上下文中检查一次非阻塞连接状态。
 *
 * @details
 * 该函数直接查看 netconn 的 nonblocking-connect 标志、pending_err 与底层 TCP PCB 状态，
 * 并把检查结果写回 Context，最后置 ucConnectCheckComplete 通知等待任务。把 PCB 访问限定
 * 在 TCP/IP Core 中，避免普通任务直接跨线程读取 raw PCB 状态。
 *
 * @param[in,out] pvContext TransportTcpContext_t 上下文。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  创建、绑定并监听 Netconn TCP Server。
 *
 * @details
 * Server 打开阶段只创建 listener、bind 到配置端口并 listen，不在这里阻塞等待客户端。
 * 具体 accept 延迟到 prvReceive() 首次需要数据且当前没有活动连接时执行，因此一个 OPEN
 * Server 可以在没有客户端时保持监听状态。
 *
 * @param[in,out] pxContext TCP Server Context。
 *
 * @retval TRANSPORT_RESULT_OK Listener 已建立。
 * @retval 其他 TransportResult_e 资源、绑定或监听失败。
 */
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

/**
 * @brief  关闭 Netconn TCP Context 的活动连接和 Listener。
 *
 * @details
 * 先通过 prvCloseConnection() 删除残留 netbuf 并关闭活动会话，再关闭/删除 Listener，
 * 最后把 Context 状态置为 CLOSED。Context 和 Channel 存储仍由调用者拥有，不在此释放。
 *
 * @param[in,out] pvContext TransportTcpContext_t 上下文。
 *
 * @retval TRANSPORT_RESULT_OK 关闭完成。
 * @retval TRANSPORT_RESULT_INVALID_ARG Context 无效。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  在一个总发送预算内通过 Netconn 写完全部请求字节。
 *
 * @details
 * netconn_write_partly() 可能一次只写入部分数据，因此通过 uxTotalWritten 持续推进偏移。
 * 每次调用前都根据 sys_now() 重新计算“剩余总预算”，并把剩余值写给 Netconn send timeout，
 * 从而保证分段发送不会反复获得完整 timeout。短写、错误或超时会关闭当前活动连接。
 *
 * @param[in,out] pvContext    TransportTcpContext_t 上下文。
 * @param[in]     pucData      待发送数据。
 * @param[in]     usDataLen    总字节数。
 * @param[out]    pusSentLen   实际累计发送字节数。
 * @param[in]     ulTimeoutMs  整体发送超时时间。
 *
 * @retval TRANSPORT_RESULT_OK 完整发送成功。
 * @retval 其他 TransportResult_e 超时、断开或 LwIP 错误。
 */
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
	/* 一次 write 可能短写，因此循环累计，同时始终受同一个总截止时间约束。 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  从 Netconn 活动连接读取数据，并保留未消费的 netbuf 字节。
 *
 * @details
 * Server 模式在没有活动连接时先调用 prvAcceptClient()。只有当前没有缓存 netbuf 时才执行
 * netconn_recv()；收到 netbuf 后，按 usRxOffset 从中复制最多 usMaxLen 字节给调用者。
 * 若一次没有消费完整 netbuf，则剩余数据和偏移保留到下一次 receive，避免丢失 TCP 流中
 * 同一 netbuf 内尚未读取的字节。
 *
 * @param[in,out] pvContext       TransportTcpContext_t 上下文。
 * @param[out]    pucData         接收缓冲区。
 * @param[in]     usMaxLen        本次最多复制字节数。
 * @param[out]    pusReceivedLen  实际复制字节数。
 * @param[in]     ulTimeoutMs     本次 Netconn 等待超时。
 *
 * @retval TransportResult_e 接收结果。
 */
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

	/* 只有缓存 netbuf 已消费完才向 LwIP 请求新的数据块。 */
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

	/* 未消费完的 netbuf 保留在 Context 中，下一次 receive 从 usRxOffset 继续。 */
	pxContext->usRxOffset = (uint16_t)(pxContext->usRxOffset + usCopyLen);
	*pusReceivedLen = usCopyLen;
	if (pxContext->usRxOffset >= netbuf_len(pxContext->pxRxBuffer)) {
		prvDeleteNetbuf(pxContext);
	}
	return TRANSPORT_RESULT_OK;
}

/*-----------------------------------------------------------*/

/**
 * @brief  为单会话 Netconn Server 接受一个客户端连接。
 *
 * @details
 * 使用本次调用提供的 ulTimeoutMs 设置 Listener 接收超时并执行 netconn_accept()。
 * accept 成功后再为活动连接恢复配置中的普通 IO 收发超时。当前 Context 只保存一个
 * pxConnection，因此该路径表达的是单活动会话模型。
 *
 * @param[in,out] pxContext   TCP Server Context。
 * @param[in]     ulTimeoutMs 本次 accept 超时。
 *
 * @retval TransportResult_e accept 结果。
 */
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

/**
 * @brief  执行 Netconn TCP Context 控制命令。
 *
 * @details
 * 当前只支持 TRANSPORT_CTRL_CONNECTION_RESET：关闭当前活动连接并释放缓存数据。
 * Client 重置后回到 CLOSED；Server 若 Listener 仍存在则保持 OPEN，等待下一次 receive
 * 再 accept 新客户端。
 *
 * @param[in,out] pvContext  TransportTcpContext_t 上下文。
 * @param[in]     xCommand   控制命令。
 * @param[in,out] pvArgument 当前实现未使用。
 *
 * @retval TRANSPORT_RESULT_OK 重置成功。
 * @retval TRANSPORT_RESULT_NOT_SUPPORTED 命令不支持。
 */
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

/**
 * @brief  返回 Netconn TCP Context 当前生命周期状态。
 *
 * @param[in] pvContext TransportTcpContext_t 上下文。
 *
 * @retval TransportState_e 当前状态；Context 无效时返回 UNINITIALIZED。
 */
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

/**
 * @brief  返回 Netconn TCP Context 最近一次 LwIP 原生错误。
 *
 * @param[in] pvContext TransportTcpContext_t 上下文。
 *
 * @retval 最近 err_t 的整数表示；Context 无效时返回 ERR_ARG。
 */
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

/**
 * @brief  关闭并删除当前活动 Netconn 会话。
 *
 * @details
 * 在释放连接前先删除 pxRxBuffer 并复位接收偏移，确保缓存数据不会跨连接残留。
 * Listener 不在这里关闭，因此 Server 可以结束一个客户端会话后继续监听。
 *
 * @param[in,out] pxContext TCP Context。
 */
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

/**
 * @brief  删除当前缓存 netbuf 并复位流内读取偏移。
 *
 * @param[in,out] pxContext TCP Context。
 */
static void prvDeleteNetbuf(TransportTcpContext_t *pxContext)
{
	if (pxContext->pxRxBuffer != NULL) {
		netbuf_delete(pxContext->pxRxBuffer);
		pxContext->pxRxBuffer = NULL;
	}
	pxContext->usRxOffset = 0U;
}

/*-----------------------------------------------------------*/

/**
 * @brief  校验已附着 Socket Context 并把会话标记为 OPEN。
 *
 * @details
 * 该后端的 socket 描述符由外部 Listener/accept 流程提供；xOpen() 本身不创建 socket。
 * lSocket < 0 时保持 CLOSED 并返回 NOT_OPEN。
 *
 * @param[in,out] pvContext TransportTcpSocketContext_t 上下文。
 *
 * @retval TransportResult_e 打开校验结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  关闭并解除当前已接受的 Socket 描述符。
 *
 * @details
 * 先把 Context 中 lSocket 置为 -1、状态置 CLOSED，再对旧描述符执行 shutdown/close。
 * 因而固定 Channel/Context 可以继续保留，之后重新附着新的客户端 socket。
 *
 * @param[in,out] pvContext TransportTcpSocketContext_t 上下文。
 *
 * @retval TransportResult_e 关闭结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  在总截止时间内通过非阻塞 Socket 完整发送一段数据。
 *
 * @details
 * lwip_send(MSG_DONTWAIT) 可能短写或返回 EWOULDBLOCK/EAGAIN，因此使用 usOffset 累计进度。
 * 暂时不可写时只等待 1 Tick 并检查自函数入口起的总时间；连接被关闭或发生其它 errno 时
 * 立即映射为 TransportResult_e。成功返回前保证 pusSentLen == usDataLen。
 *
 * @param[in,out] pvContext    Socket Context。
 * @param[in]     pucData      待发送数据。
 * @param[in]     usDataLen    总字节数。
 * @param[out]    pusSentLen   实际累计发送字节数。
 * @param[in]     ulTimeoutMs  总发送超时。
 *
 * @retval TransportResult_e 发送结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  从已接受的非阻塞 Socket 读取当前可用字节。
 *
 * @details
 * 当前实现固定使用 MSG_DONTWAIT，并不在本函数内部消费 ulTimeoutMs。没有数据且 errno 为
 * EWOULDBLOCK/EAGAIN 时返回 TIMEOUT，由上层 ReceiveExact 的总截止时间循环负责重试。
 * recv 返回 0 视为对端断开，并把 Context 状态置为 ERROR。
 *
 * @param[in,out] pvContext       Socket Context。
 * @param[out]    pucData         接收缓冲区。
 * @param[in]     usMaxLen        最大读取长度。
 * @param[out]    pusReceivedLen  实际读取长度。
 * @param[in]     ulTimeoutMs     当前实现不直接使用，由上层总预算负责约束。
 *
 * @retval TransportResult_e 接收结果。
 */
static TransportResult_e prvSocketReceive(void *pvContext,
	uint8_t *pucData, uint16_t usMaxLen, uint16_t *pusReceivedLen,
	uint32_t ulTimeoutMs)
{
	TransportTcpSocketContext_t *pxContext;
	int lReceived;

	/* Socket 后端自身固定非阻塞；整体 timeout 由上层 ReceiveExact 循环控制。 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  对固定 Socket 会话执行连接重置。
 *
 * @details
 * 当前仅支持 CONNECTION_RESET。若描述符有效，先设置 SO_LINGER={1,0} 请求立即复位连接，
 * 随后复用 prvSocketClose() 完成关闭并清除 Context 中的描述符。
 *
 * @param[in,out] pvContext  Socket Context。
 * @param[in]     xCommand   控制命令。
 * @param[in,out] pvArgument 当前实现未使用。
 *
 * @retval TransportResult_e 控制结果。
 */
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

/**
 * @brief  返回固定 Socket 会话当前状态。
 *
 * @param[in] pvContext Socket Context。
 *
 * @retval TransportState_e 当前状态；Context 无效时返回 UNINITIALIZED。
 */
static TransportState_e prvSocketGetState(void *pvContext)
{
	TransportTcpSocketContext_t *pxContext;

	pxContext = (TransportTcpSocketContext_t *)pvContext;
	return (pxContext != NULL) ? pxContext->xState :
		TRANSPORT_STATE_UNINITIALIZED;
}

/*-----------------------------------------------------------*/

/**
 * @brief  返回固定 Socket 会话最近一次 errno。
 *
 * @param[in] pvContext Socket Context。
 *
 * @retval 最近 errno；Context 无效时返回 EINVAL。
 */
static int32_t prvSocketGetNativeError(void *pvContext)
{
	TransportTcpSocketContext_t *pxContext;

	pxContext = (TransportTcpSocketContext_t *)pvContext;
	return (pxContext != NULL) ? pxContext->lLastNativeError : EINVAL;
}

/*-----------------------------------------------------------*/

/**
 * @brief  将 Socket errno 映射到稳定的 TransportResult_e 错误域。
 *
 * @details
 * 把“暂时不可读写/超时”“连接断开”“资源不足”“网络未就绪”等平台错误归并成
 * 上层可稳定处理的 Transport 结果，使协议层不需要依赖具体 errno 数值。
 *
 * @param[in] lError Socket errno。
 *
 * @retval TransportResult_e 规范化结果。
 */
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

/*-----------------------------------------------------------*/

/**
 * @brief  将 LwIP err_t 映射到稳定的 TransportResult_e 错误域。
 *
 * @details
 * 该映射把 Netconn 后端的超时、资源、忙、断开和网络未就绪语义统一到公共 Transport
 * 枚举，与 Socket 后端对上层暴露同一错误模型。
 *
 * @param[in] xError LwIP err_t。
 *
 * @retval TransportResult_e 规范化结果。
 */
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
