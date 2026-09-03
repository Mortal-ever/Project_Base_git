/**
  * @file      transport_tcp.h
  * @brief     Define LwIP Netconn and Socket Transport backends.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef TRANSPORT_TCP_H
#define TRANSPORT_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "transport.h"

/** @brief Maximum IPv4 endpoint text including the terminator. */
#define TRANSPORT_TCP_ENDPOINT_TEXT_LENGTH    22U

/** @brief Select TCP client-connect or server-listen behavior. */
typedef enum {
	TRANSPORT_TCP_MODE_CLIENT = 0, /*!< Connect to the configured peer. */
	TRANSPORT_TCP_MODE_SERVER = 1 /*!< Listen on the configured local port. */
} TransportTcpMode_e;

/** @brief Configure one Netconn TCP Transport endpoint. */
typedef struct {
	TransportTcpMode_e xMode; /*!< Client or server behavior. */
	uint8_t aucRemoteIp[4]; /*!< Client destination; ignored by server. */
	uint16_t usPort; /*!< Remote or local TCP port in host byte order. */
	uint32_t ulConnectTimeoutMs; /*!< Zero keeps the legacy blocking connect. */
	uint32_t ulIoTimeoutMs; /*!< Default IO timeout in milliseconds. */
} TransportTcpConfig_t;

/** @brief Store one Netconn TCP endpoint's private runtime state. */
typedef struct {
	TransportChannel_t *pxChannel; /*!< Generic channel owning this context. */
	TransportTcpConfig_t xConfig; /*!< Copied product configuration. */
	ip_addr_t xRemoteAddress; /*!< LwIP client destination representation. */
	struct netconn *pxListener; /*!< Server listener, or NULL for clients. */
	struct netconn *pxConnection; /*!< Connected or accepted Netconn. */
	struct netbuf *pxRxBuffer; /*!< Partly consumed receive buffer. */
	uint16_t usRxOffset; /*!< Read cursor inside pxRxBuffer. */
	volatile TransportState_e xState; /*!< Backend lifecycle state. */
	volatile int32_t lLastNativeError; /*!< Latest LwIP err_t value. */
	volatile err_t xConnectCheckResult;
	volatile uint8_t ucConnectCheckComplete;
} TransportTcpContext_t;

/** @brief Store one accepted nonblocking Socket session. */
typedef struct {
	TransportChannel_t *pxChannel; /*!< Generic channel owning this context. */
	volatile TransportState_e xState; /*!< Attached socket lifecycle. */
	volatile int32_t lLastNativeError; /*!< Latest socket errno value. */
	int lSocket; /*!< Accepted socket descriptor, or -1 when detached. */
} TransportTcpSocketContext_t;

/**
  * @brief Create and register one LwIP Netconn Transport channel.
  * @param[out] pxChannel Caller-owned generic channel object.
  * @param[out] pxContext Caller-owned backend context.
  * @param[in] pcName Persistent unique registry name.
  * @param[in] pxConfig TCP configuration copied into the context.
  * @return Normalized creation or registration result.
  * @note Call after LwIP initialization and before opening the endpoint.
  */
TransportResult_e xTransportTcpCreate(TransportChannel_t *pxChannel,
	TransportTcpContext_t *pxContext, const char *pcName,
	const TransportTcpConfig_t *pxConfig);

/**
  * @brief Create a reusable Transport channel for accepted TCP sockets.
  * @param[out] pxChannel Caller-owned generic channel object.
  * @param[out] pxContext Caller-owned Socket backend context.
  * @param[in] pcName Persistent unique registry name.
  * @return Normalized creation or registration result.
  */
TransportResult_e xTransportTcpSocketCreate(TransportChannel_t *pxChannel,
	TransportTcpSocketContext_t *pxContext, const char *pcName);

/**
  * @brief Attach an accepted nonblocking socket to a reusable channel.
  * @param[in,out] pxChannel Socket-backed registered channel.
  * @param[in,out] pxContext Context that owns the descriptor.
  * @param[in] lSocket Valid accepted socket descriptor.
  * @return Normalized attach result.
  * @warning The caller transfers descriptor ownership to pxContext on success.
  */
TransportResult_e xTransportTcpSocketAttach(TransportChannel_t *pxChannel,
	TransportTcpSocketContext_t *pxContext, int lSocket);

/**
  * @brief  Format one host-order IPv4 endpoint as dotted IP and decimal port.
  * @param[in] pucIpv4 Four IPv4 octets in display order.
  * @param[in] usPort TCP port in host byte order.
  * @param[out] pcText Caller-owned output buffer.
  * @param[in] usCapacity Output capacity including the terminator.
  * @retval 1 Endpoint text was written successfully.
  * @retval 0 An argument or output capacity was invalid.
  * @note   This function uses no dynamic memory, static buffer, or stdio.
  */
uint8_t ucTransportTcpFormatIpv4Endpoint(const uint8_t pucIpv4[4],
	uint16_t usPort, char *pcText, uint16_t usCapacity);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_TCP_H */
