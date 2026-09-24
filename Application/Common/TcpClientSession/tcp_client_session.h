/**
  * @file      tcp_client_session.h
  * @brief     定义静态 TCP 客户端连接生命周期管理接口。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   所属任务周期调用处理接口；模块负责打开一个 Transport
  *            通道、验证一次应用协议，并按配置执行有上限的重连退避。
  *
  * @attention
  * - 同一所属任务必须串行调用处理和强制重连接口。
  * - 协议探测可以重置会话握手，但不得触发物理动作。
  */

#ifndef TCP_CLIENT_SESSION_H
#define TCP_CLIENT_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "transport.h"

/** @brief 表示一个 TCP 客户端会话的生命周期状态。 */
typedef enum {
	TCP_CLIENT_SESSION_NETWORK_WAIT = 0, /*!< 等待产品网络就绪。 */
	TCP_CLIENT_SESSION_BACKOFF = 1, /*!< 等待本次重连退避截止。 */
	TCP_CLIENT_SESSION_CONNECTING = 2, /*!< 正在打开 Transport 通道。 */
	TCP_CLIENT_SESSION_PROTOCOL_CHECK = 3, /*!< 正在验证应用层协议。 */
	TCP_CLIENT_SESSION_ONLINE = 4 /*!< 协议探测通过，可执行业务命令。 */
} TcpClientSessionState_e;

/** @brief 查询产品网络是否可供 TCP 客户端使用。 */
typedef uint8_t (*TcpClientSessionNetworkReadyFn_t)(void *pvOwnerContext);

/** @brief 检查应用协议是否可用，所属模块可在回调中初始化握手。 */
typedef int32_t (*TcpClientSessionProbeFn_t)(void *pvOwnerContext,
	uint32_t ulTimeoutMs);

/** @brief 向所属产品模块报告一次生命周期状态切换。 */
typedef void (*TcpClientSessionEventFn_t)(void *pvOwnerContext,
	TcpClientSessionState_e xPreviousState,
	TcpClientSessionState_e xCurrentState, int32_t lReason,
	uint32_t ulAttempt, uint32_t ulRetryDelayMs);

/** @brief 保存静态客户端实例使用的不可变行为配置。 */
typedef struct {
	TcpClientSessionNetworkReadyFn_t ucNetworkReady; /*!< 网络就绪查询回调。 */
	TcpClientSessionProbeFn_t lProtocolProbe; /*!< 应用协议探测回调。 */
	TcpClientSessionEventFn_t vEvent; /*!< 可选的状态切换通知回调。 */
	const uint32_t *pulRetryDelayMs; /*!< 重连退避时长表，单位为毫秒。 */
	uint8_t ucRetryDelayCount; /*!< 退避时长表的有效项数。 */
	uint32_t ulProbeTimeoutMs; /*!< 单次协议探测超时，单位为毫秒。 */
} TcpClientSessionConfig_t;

/** @brief 保存由调用方持有的单个 TCP 客户端会话运行状态。 */
typedef struct {
	const TcpClientSessionConfig_t *pxConfig; /*!< 不可变的会话配置。 */
	TransportChannel_t *pxChannel; /*!< 会话独占使用的 Transport 通道。 */
	void *pvOwnerContext; /*!< 原样传给产品回调的上下文。 */
	TcpClientSessionState_e xState; /*!< 当前生命周期状态。 */
	TickType_t xNextActionTick; /*!< 允许下次连接尝试的系统节拍。 */
	TransportResult_e xLastTransportResult; /*!< 最近一次通道操作结果。 */
	int32_t lLastProbeResult; /*!< 最近一次协议探测结果。 */
	uint32_t ulAttemptCount; /*!< 自初始化以来发起的连接尝试次数。 */
	uint32_t ulConsecutiveFailures; /*!< 当前连续连接或探测失败次数。 */
	uint32_t ulNextRetryDelayMs; /*!< 当前采用的退避时长，单位为毫秒。 */
	uint8_t ucInitialized; /*!< 非零表示实例已经完成初始化。 */
} TcpClientSession_t;

BaseType_t xTcpClientSessionInit(TcpClientSession_t *pxSession,
	const TcpClientSessionConfig_t *pxConfig, TransportChannel_t *pxChannel,
	void *pvOwnerContext);

void vTcpClientSessionProcess(TcpClientSession_t *pxSession);

void vTcpClientSessionForceReconnect(TcpClientSession_t *pxSession,
	int32_t lReason);

uint8_t ucTcpClientSessionIsOnline(const TcpClientSession_t *pxSession);

#ifdef __cplusplus
}
#endif

#endif /* TCP_CLIENT_SESSION_H */
