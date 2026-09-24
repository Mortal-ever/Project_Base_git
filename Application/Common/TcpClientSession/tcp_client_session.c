/**
  * @file      tcp_client_session.c
  * @brief     实现静态 TCP 客户端连接生命周期管理。
  * @author    WHong
  * @date      2026-09-24
  */

#include "tcp_client_session.h"

#include <string.h>

#include "task.h"

static void prvTransition(TcpClientSession_t *pxSession,
	TcpClientSessionState_e xNextState, int32_t lReason);
static void prvScheduleBackoff(TcpClientSession_t *pxSession,
	int32_t lReason);
static uint32_t prvGetRetryDelayMs(const TcpClientSession_t *pxSession);
static uint8_t prvNetworkReady(const TcpClientSession_t *pxSession);

/*-----------------------------------------------------------*/
/**
  * @brief  初始化由调用方持有的静态 TCP 客户端会话。
  * @param[out] pxSession 由单一所属任务持有的运行状态。
  * @param[in] pxConfig 生命周期回调与退避参数。
  * @param[in] pxChannel 已注册的 TCP Transport 客户端通道。
  * @param[in] pvOwnerContext 原样传给配置回调的上下文。
  * @retval pdPASS 初始化完成。
  * @retval pdFAIL 必需指针、回调或配置值不合法。
  */
BaseType_t xTcpClientSessionInit(TcpClientSession_t *pxSession,
	const TcpClientSessionConfig_t *pxConfig, TransportChannel_t *pxChannel,
	void *pvOwnerContext)
{
	if ((pxSession == NULL) || (pxConfig == NULL) ||
		(pxChannel == NULL) || (pxConfig->ucNetworkReady == NULL) ||
		(pxConfig->lProtocolProbe == NULL) ||
		(pxConfig->pulRetryDelayMs == NULL) ||
		(pxConfig->ucRetryDelayCount == 0U) ||
		(pxConfig->ulProbeTimeoutMs == 0U)) {
		return pdFAIL;
	}
	memset(pxSession, 0, sizeof(*pxSession));
	pxSession->pxConfig = pxConfig;
	pxSession->pxChannel = pxChannel;
	pxSession->pvOwnerContext = pvOwnerContext;
	pxSession->xState = TCP_CLIENT_SESSION_NETWORK_WAIT;
	pxSession->xLastTransportResult = TRANSPORT_RESULT_NOT_READY;
	pxSession->ucInitialized = 1U;
	return pdPASS;
}

/*-----------------------------------------------------------*/
/**
  * @brief  推进一次不含业务命令的 TCP 客户端生命周期处理。
  * @param[in,out] pxSession 已初始化且由当前任务串行持有的会话。
  * @note 调用方负责在相邻两次调用之间让出处理器。
  */
void vTcpClientSessionProcess(TcpClientSession_t *pxSession)
{
	TransportResult_e xTransportResult; /*!< 本次打开通道的结果。 */
	int32_t lProbeResult; /*!< 本次应用协议探测结果。 */

	if ((pxSession == NULL) || (pxSession->ucInitialized == 0U)) {
		return;
	}
	if (prvNetworkReady(pxSession) == 0U) {
		if (pxSession->xState != TCP_CLIENT_SESSION_NETWORK_WAIT) {
			(void)xTransportClose(pxSession->pxChannel);
			prvTransition(pxSession, TCP_CLIENT_SESSION_NETWORK_WAIT,
				(int32_t)TRANSPORT_RESULT_NOT_READY);
		}
		return;
	}
	/* 按“网络等待、退避、连接、协议检查、在线”顺序推进状态机。 */
	switch (pxSession->xState) {
	case TCP_CLIENT_SESSION_NETWORK_WAIT:
		pxSession->xNextActionTick = xTaskGetTickCount();
		prvTransition(pxSession, TCP_CLIENT_SESSION_BACKOFF, 0);
		break;

	case TCP_CLIENT_SESSION_BACKOFF:
		if ((int32_t)(xTaskGetTickCount() - pxSession->xNextActionTick) >=
			0) {
			pxSession->ulAttemptCount++;
			prvTransition(pxSession, TCP_CLIENT_SESSION_CONNECTING, 0);
		}
		break;

	case TCP_CLIENT_SESSION_CONNECTING:
		xTransportResult = xTransportOpen(pxSession->pxChannel);
		pxSession->xLastTransportResult = xTransportResult;
		if (xTransportResult == TRANSPORT_RESULT_OK) {
			prvTransition(pxSession,
				TCP_CLIENT_SESSION_PROTOCOL_CHECK, 0);
		} else {
			prvScheduleBackoff(pxSession, (int32_t)xTransportResult);
		}
		break;

	case TCP_CLIENT_SESSION_PROTOCOL_CHECK:
		lProbeResult = pxSession->pxConfig->lProtocolProbe(
			pxSession->pvOwnerContext,
			pxSession->pxConfig->ulProbeTimeoutMs);
		pxSession->lLastProbeResult = lProbeResult;
		if (lProbeResult == 0) {
			pxSession->ulConsecutiveFailures = 0U;
			pxSession->ulNextRetryDelayMs = 0U;
			prvTransition(pxSession, TCP_CLIENT_SESSION_ONLINE, 0);
		} else {
			(void)xTransportClose(pxSession->pxChannel);
			prvScheduleBackoff(pxSession, lProbeResult);
		}
		break;

	case TCP_CLIENT_SESSION_ONLINE:
	default:
		break;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  关闭当前通道并按连续失败次数安排下一次重连。
  * @param[in,out] pxSession 已初始化且由当前任务串行持有的会话。
  * @param[in] lReason 导致当前会话失效的产品级原因码。
  */
void vTcpClientSessionForceReconnect(TcpClientSession_t *pxSession,
	int32_t lReason)
{
	if ((pxSession == NULL) || (pxSession->ucInitialized == 0U)) {
		return;
	}
	(void)xTransportClose(pxSession->pxChannel);
	prvScheduleBackoff(pxSession, lReason);
}

/*-----------------------------------------------------------*/
/**
  * @brief  查询会话是否已通过应用协议检查。
  * @param[in] pxSession 待查询会话，允许为空。
  * @retval 1 会话处于在线状态，可执行业务命令。
  * @retval 0 会话为空、未初始化或尚未在线。
  */
uint8_t ucTcpClientSessionIsOnline(const TcpClientSession_t *pxSession)
{
	if ((pxSession == NULL) || (pxSession->ucInitialized == 0U)) {
		return 0U;
	}
	return (pxSession->xState == TCP_CLIENT_SESSION_ONLINE) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  切换会话状态并向产品模块报告真实发生的状态变化。
  * @param[in,out] pxSession 待更新的会话。
  * @param[in] xNextState 目标生命周期状态。
  * @param[in] lReason 触发切换的结果或原因码。
  */
static void prvTransition(TcpClientSession_t *pxSession,
	TcpClientSessionState_e xNextState, int32_t lReason)
{
	TcpClientSessionState_e xPreviousState; /*!< 切换前的生命周期状态。 */

	xPreviousState = pxSession->xState;
	if (xPreviousState == xNextState) {
		return;
	}
	pxSession->xState = xNextState;
	if (pxSession->pxConfig->vEvent != NULL) {
		pxSession->pxConfig->vEvent(pxSession->pvOwnerContext,
			xPreviousState, xNextState, lReason,
			pxSession->ulAttemptCount,
			pxSession->ulNextRetryDelayMs);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  根据连续失败次数计算退避并进入等待状态。
  * @param[in,out] pxSession 待安排重连的会话。
  * @param[in] lReason 本次失败原因码。
  */
static void prvScheduleBackoff(TcpClientSession_t *pxSession,
	int32_t lReason)
{
	pxSession->ulConsecutiveFailures++;
	pxSession->ulNextRetryDelayMs = prvGetRetryDelayMs(pxSession);
	pxSession->xNextActionTick = xTaskGetTickCount() + pdMS_TO_TICKS(
		pxSession->ulNextRetryDelayMs);
	prvTransition(pxSession, TCP_CLIENT_SESSION_BACKOFF, lReason);
}

/*-----------------------------------------------------------*/
/**
  * @brief  从配置表读取当前连续失败次数对应的退避时长。
  * @param[in] pxSession 含退避表和失败计数的会话。
  * @retval uint32_t 退避时长，单位为毫秒；超出表范围时使用末项。
  */
static uint32_t prvGetRetryDelayMs(const TcpClientSession_t *pxSession)
{
	uint32_t ulIndex; /*!< 退避时长表索引。 */

	ulIndex = (pxSession->ulConsecutiveFailures == 0U) ? 0U :
		pxSession->ulConsecutiveFailures - 1U;
	if (ulIndex >= pxSession->pxConfig->ucRetryDelayCount) {
		ulIndex = pxSession->pxConfig->ucRetryDelayCount - 1U;
	}
	return pxSession->pxConfig->pulRetryDelayMs[ulIndex];
}

/*-----------------------------------------------------------*/
/**
  * @brief  调用产品回调查询共享网络是否可用于 TCP。
  * @param[in] pxSession 含网络查询回调及上下文的会话。
  * @retval 0 网络未就绪。
  * @retval 非零 网络已就绪。
  */
static uint8_t prvNetworkReady(const TcpClientSession_t *pxSession)
{
	return pxSession->pxConfig->ucNetworkReady(
		pxSession->pvOwnerContext);
}

