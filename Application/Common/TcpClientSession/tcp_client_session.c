/**
  * @file      tcp_client_session.c
  * @brief     Implement a static TCP client connection lifecycle helper.
  * @author    WHong
  * @date      2026-08-27
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
void vTcpClientSessionProcess(TcpClientSession_t *pxSession)
{
	TransportResult_e xTransportResult;
	int32_t lProbeResult;

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
uint8_t ucTcpClientSessionIsOnline(const TcpClientSession_t *pxSession)
{
	if ((pxSession == NULL) || (pxSession->ucInitialized == 0U)) {
		return 0U;
	}
	return (pxSession->xState == TCP_CLIENT_SESSION_ONLINE) ? 1U : 0U;
}

/*-----------------------------------------------------------*/
static void prvTransition(TcpClientSession_t *pxSession,
	TcpClientSessionState_e xNextState, int32_t lReason)
{
	TcpClientSessionState_e xPreviousState;

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
static uint32_t prvGetRetryDelayMs(const TcpClientSession_t *pxSession)
{
	uint32_t ulIndex;

	ulIndex = (pxSession->ulConsecutiveFailures == 0U) ? 0U :
		pxSession->ulConsecutiveFailures - 1U;
	if (ulIndex >= pxSession->pxConfig->ucRetryDelayCount) {
		ulIndex = pxSession->pxConfig->ucRetryDelayCount - 1U;
	}
	return pxSession->pxConfig->pulRetryDelayMs[ulIndex];
}

/*-----------------------------------------------------------*/
static uint8_t prvNetworkReady(const TcpClientSession_t *pxSession)
{
	return pxSession->pxConfig->ucNetworkReady(
		pxSession->pvOwnerContext);
}
