/**
  * @file      tcp_client_session.h
  * @brief     Define a static TCP client connection lifecycle helper.
  * @author    WHong
  * @date      2026-08-27
  *
  * @details   The owner task calls this module periodically. It opens one
  *            Transport channel, verifies the application protocol once, and
  *            applies bounded reconnect backoff without creating a task,
  *            queue, or dynamic object.
  *
  * @attention
  * - One owner task must serialize Process and ForceReconnect calls.
  * - The probe callback must not issue a robot action or change business state.
  */

#ifndef TCP_CLIENT_SESSION_H
#define TCP_CLIENT_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "transport.h"

/** @brief Define the sole lifecycle state for one TCP client session. */
typedef enum {
	TCP_CLIENT_SESSION_NETWORK_WAIT = 0,
	TCP_CLIENT_SESSION_BACKOFF = 1,
	TCP_CLIENT_SESSION_CONNECTING = 2,
	TCP_CLIENT_SESSION_PROTOCOL_CHECK = 3,
	TCP_CLIENT_SESSION_ONLINE = 4
} TcpClientSessionState_e;

/** @brief Read whether the shared product network is ready for TCP use. */
typedef uint8_t (*TcpClientSessionNetworkReadyFn_t)(void *pvOwnerContext);

/** @brief Execute a side-effect-free protocol availability check. */
typedef int32_t (*TcpClientSessionProbeFn_t)(void *pvOwnerContext,
	uint32_t ulTimeoutMs);

/** @brief Report a lifecycle transition from the owning product module. */
typedef void (*TcpClientSessionEventFn_t)(void *pvOwnerContext,
	TcpClientSessionState_e xPreviousState,
	TcpClientSessionState_e xCurrentState, int32_t lReason,
	uint32_t ulAttempt, uint32_t ulRetryDelayMs);

/** @brief Define immutable behavior shared by static client instances. */
typedef struct {
	TcpClientSessionNetworkReadyFn_t ucNetworkReady;
	TcpClientSessionProbeFn_t lProtocolProbe;
	TcpClientSessionEventFn_t vEvent;
	const uint32_t *pulRetryDelayMs;
	uint8_t ucRetryDelayCount;
	uint32_t ulProbeTimeoutMs;
} TcpClientSessionConfig_t;

/** @brief Store caller-owned runtime state for one TCP client session. */
typedef struct {
	const TcpClientSessionConfig_t *pxConfig;
	TransportChannel_t *pxChannel;
	void *pvOwnerContext;
	TcpClientSessionState_e xState;
	TickType_t xNextActionTick;
	TransportResult_e xLastTransportResult;
	int32_t lLastProbeResult;
	uint32_t ulAttemptCount;
	uint32_t ulConsecutiveFailures;
	uint32_t ulNextRetryDelayMs;
	uint8_t ucInitialized;
} TcpClientSession_t;

/**
  * @brief  Initialize a caller-owned static TCP client session.
  * @param[out] pxSession Runtime storage owned by one owner task.
  * @param[in] pxConfig Immutable static configuration.
  * @param[in] pxChannel Registered TCP Transport client channel.
  * @param[in] pvOwnerContext Context passed to every configured callback.
  * @retval pdPASS Initialization completed.
  * @retval pdFAIL A required configuration field is invalid.
  */
BaseType_t xTcpClientSessionInit(TcpClientSession_t *pxSession,
	const TcpClientSessionConfig_t *pxConfig, TransportChannel_t *pxChannel,
	void *pvOwnerContext);

/**
  * @brief Advance one non-business TCP client lifecycle transition.
  * @param[in,out] pxSession Initialized session owned by the current task.
  * @note The caller remains responsible for yielding between Process calls.
  */
void vTcpClientSessionProcess(TcpClientSession_t *pxSession);

/**
  * @brief Close the active channel and schedule a capped reconnect attempt.
  * @param[in,out] pxSession Initialized session owned by the current task.
  * @param[in] lReason Product-level reason that invalidated the TCP session.
  * @note Do not call for a valid Modbus exception response.
  */
void vTcpClientSessionForceReconnect(TcpClientSession_t *pxSession,
	int32_t lReason);

/**
  * @brief Test whether the session passed the initial protocol availability check.
  * @param[in] pxSession Initialized session, or NULL.
  * @retval 1 The session is ONLINE.
  * @retval 0 The session is not available for business commands.
  */
uint8_t ucTcpClientSessionIsOnline(const TcpClientSession_t *pxSession);

#ifdef __cplusplus
}
#endif

#endif /* TCP_CLIENT_SESSION_H */
