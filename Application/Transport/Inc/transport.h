/**
  * @file      transport.h
  * @brief     Define the backend-neutral byte transport abstraction.
  * @author    WHong
  * @date      2026-07-28
  *
  * @details   Provide registered channels, lifecycle operations, bounded IO,
  *            diagnostics, controls, and optional ISR event forwarding.
  */

#ifndef TRANSPORT_H
#define TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"

/** @brief Maximum number of channels stored in the Transport registry. */
#define TRANSPORT_MAX_CHANNELS        10U

/** @brief Define backend-neutral Transport operation results. */
typedef enum {
	TRANSPORT_RESULT_OK = 0, /*!< Complete operation succeeded. */
	TRANSPORT_RESULT_INVALID_ARG = -1, /*!< Invalid pointer or length. */
	TRANSPORT_RESULT_NOT_FOUND = -2, /*!< Named channel was not found. */
	TRANSPORT_RESULT_BUSY = -3, /*!< Resource is already in use. */
	TRANSPORT_RESULT_TIMEOUT = -4, /*!< Total operation deadline expired. */
	TRANSPORT_RESULT_IO_ERROR = -5, /*!< Backend IO failed. */
	TRANSPORT_RESULT_NO_RESOURCE = -6, /*!< RTOS or backend unavailable. */
	TRANSPORT_RESULT_NOT_OPEN = -7, /*!< Channel has no active endpoint. */
	TRANSPORT_RESULT_NOT_SUPPORTED = -8, /*!< Backend lacks the operation. */
	TRANSPORT_RESULT_NOT_READY = -9, /*!< Link or hardware is not ready. */
	TRANSPORT_RESULT_DISCONNECTED = -10 /*!< Peer closed or reset the link. */
} TransportResult_e;

/** @brief Define lifecycle states shared by every Transport backend. */
typedef enum {
	TRANSPORT_STATE_UNINITIALIZED = 0, /*!< Storage is not configured. */
	TRANSPORT_STATE_CLOSED = 1, /*!< Configured but not active. */
	TRANSPORT_STATE_OPEN = 2, /*!< Endpoint can perform IO. */
	TRANSPORT_STATE_BUSY = 3, /*!< A lifecycle or IO operation is active. */
	TRANSPORT_STATE_ERROR = 4 /*!< Last operation left an error state. */
} TransportState_e;

/** @brief Define optional asynchronous Transport notifications. */
typedef enum {
	TRANSPORT_EVENT_RX_DATA = 0, /*!< New receive bytes are available. */
	TRANSPORT_EVENT_TX_COMPLETE = 1, /*!< Asynchronous TX completed. */
	TRANSPORT_EVENT_ERROR = 2, /*!< Backend reported an IO error. */
	TRANSPORT_EVENT_RX_OVERFLOW = 3 /*!< Receive buffering lost data. */
} TransportEvent_e;

/** @brief Define operation stages stored with Transport faults. */
typedef enum {
	TRANSPORT_OPERATION_NONE = 0, /*!< No Transport operation recorded. */
	TRANSPORT_OPERATION_OPEN = 1, /*!< Connect, bind, listen, or enable. */
	TRANSPORT_OPERATION_CLOSE = 2, /*!< Close or disable an endpoint. */
	TRANSPORT_OPERATION_SEND = 3, /*!< Transfer bytes to a backend. */
	TRANSPORT_OPERATION_RECEIVE = 4, /*!< Obtain bytes from a backend. */
	TRANSPORT_OPERATION_CONTROL = 5 /*!< Execute a backend control command. */
} TransportOperation_e;

/** @brief Store normalized and backend-native details of the latest fault. */
typedef struct {
	TransportOperation_e xOperation; /*!< Operation stage that failed. */
	TransportResult_e xResult; /*!< Stable cross-backend error. */
	int32_t lNativeError; /*!< LwIP err_t, errno, or HAL status. */
	TickType_t xTimestamp; /*!< FreeRTOS Tick when failure was recorded. */
	uint16_t usRequestedLength; /*!< Requested byte length or capacity. */
	uint16_t usTransferredLength; /*!< Bytes completed before failure. */
} TransportFault_t;

/** @brief Store runtime counters and latest IO details for one channel. */
typedef struct {
	TransportState_e xState; /*!< Latest backend state. */
	TransportFault_t xLastFault; /*!< Latest non-OK operation. */
	TickType_t xLastOpenTick; /*!< Tick of latest successful open. */
	TickType_t xLastTxTick; /*!< Tick of latest successful send. */
	TickType_t xLastRxTick; /*!< Tick of latest successful receive. */
	uint32_t ulOpenCount; /*!< Number of successful opens. */
	uint32_t ulTxOperationCount; /*!< Number of complete sends. */
	uint32_t ulRxOperationCount; /*!< Number of receives with data. */
	uint32_t ulTxByteCount; /*!< Total successfully transmitted bytes. */
	uint32_t ulRxByteCount; /*!< Total received bytes. */
	uint32_t ulErrorCount; /*!< Number of non-timeout failures. */
	uint16_t usLastTxRequestedLength; /*!< Latest requested TX bytes. */
	uint16_t usLastTxTransferredLength; /*!< Latest completed TX bytes. */
	uint16_t usLastRxCapacity; /*!< Latest requested RX capacity. */
	uint16_t usLastRxTransferredLength; /*!< Latest received byte count. */
} TransportStatus_t;

/** @brief Define generic controls dispatched to Transport backends. */
typedef enum {
	TRANSPORT_CTRL_RX_PAUSE = 0, /*!< Stop accepting receive data. */
	TRANSPORT_CTRL_RX_RESUME = 1, /*!< Resume receive operation. */
	TRANSPORT_CTRL_RX_FLUSH = 2, /*!< Discard buffered receive data. */
	TRANSPORT_CTRL_GET_BAUD_RATE = 3, /*!< Query UART bits per second. */
	TRANSPORT_CTRL_CONNECTION_RESET = 4 /*!< Reset the active connection. */
} TransportControl_e;

struct TransportChannel;

/**
  * @brief Receive an optional asynchronous event from a Transport backend.
  * @param[in] pxChannel Channel that produced the event.
  * @param[in] xEvent Event classification.
  * @param[in] pucData Optional event data owned by the backend.
  * @param[in] usDataLen Number of valid event-data bytes.
  * @param[in,out] pxHigherPriorityTaskWoken ISR wake flag, when applicable.
  * @param[in] pvCallbackContext Caller-owned callback context.
  * @warning ISR-originated callbacks must not block.
  */
typedef void (*TransportEventCallback_t)(
	struct TransportChannel *pxChannel,
	TransportEvent_e xEvent,
	const uint8_t *pucData,
	uint16_t usDataLen,
	BaseType_t *pxHigherPriorityTaskWoken,
	void *pvCallbackContext);

/**
  * @brief Define the operation table implemented by each Transport backend.
  * @note xSend may return OK only after transferring the complete request.
  */
typedef struct {
	TransportResult_e (*xOpen)(void *pvContext); /*!< Open backend endpoint. */
	TransportResult_e (*xClose)(void *pvContext); /*!< Close endpoint. */
	TransportResult_e (*xSend)(void *pvContext,
							 const uint8_t *pucData,
							 uint16_t usDataLen,
							 uint16_t *pusSentLen,
							 uint32_t ulTimeoutMs); /*!< Send all bytes. */
	TransportResult_e (*xReceive)(void *pvContext,
								 uint8_t *pucData,
								 uint16_t usMaxLen,
								 uint16_t *pusReceivedLen,
								 uint32_t ulTimeoutMs); /*!< Receive up to capacity. */
	TransportResult_e (*xControl)(void *pvContext,
								 TransportControl_e xCommand,
								 void *pvArgument); /*!< Execute a control. */
	TransportState_e (*xGetState)(void *pvContext); /*!< Read state. */
	int32_t (*lGetNativeError)(void *pvContext); /*!< Read native error. */
} TransportOps_t;

/** @brief Bind one named protocol-facing channel to a backend context. */
typedef struct TransportChannel {
	const char *pcName; /*!< Persistent unique registry name. */
	const TransportOps_t *pxOps; /*!< Backend operation table. */
	void *pvContext; /*!< Caller-owned TCP or UART backend context. */
	volatile TransportState_e xState; /*!< Fast current-state view. */
	TransportStatus_t xStatus; /*!< Runtime diagnostic snapshot. */
	TransportEventCallback_t pxEventCallback; /*!< Optional event sink. */
	void *pvEventContext; /*!< Context passed to the event callback. */
} TransportChannel_t;

/**
  * @brief Reset the Transport channel registry.
  * @note Call once before product channels are registered.
  */
void vTransportManagerInit(void);

/**
  * @brief Register one fully configured Transport channel.
  * @param[in,out] pxChannel Persistent caller-owned channel object.
  * @retval TRANSPORT_RESULT_OK The channel was registered.
  * @retval TRANSPORT_RESULT_INVALID_ARG Required fields are missing.
  * @retval TRANSPORT_RESULT_BUSY The name is already registered.
  * @retval TRANSPORT_RESULT_NO_RESOURCE The registry is full.
  */
TransportResult_e xTransportRegister(TransportChannel_t *pxChannel);

/**
  * @brief Find a registered channel by its stable name.
  * @param[in] pcName Null-terminated channel name.
  * @return Registered channel pointer, or NULL when not found.
  */
TransportChannel_t *pxTransportFind(const char *pcName);

/**
  * @brief Open a channel's concrete backend endpoint.
  * @param[in,out] pxChannel Registered channel.
  * @return Normalized backend result.
  */
TransportResult_e xTransportOpen(TransportChannel_t *pxChannel);

/**
  * @brief Close a channel's concrete backend endpoint.
  * @param[in,out] pxChannel Registered channel.
  * @return Normalized backend result.
  */
TransportResult_e xTransportClose(TransportChannel_t *pxChannel);

/**
  * @brief Send an entire byte sequence within a bounded timeout.
  * @param[in,out] pxChannel Open registered channel.
  * @param[in] pucData Source bytes valid for the duration of the call.
  * @param[in] usDataLen Number of bytes to send; must be greater than zero.
  * @param[in] ulTimeoutMs Total send timeout in milliseconds.
  * @retval TRANSPORT_RESULT_OK Every byte was sent.
  * @retval TRANSPORT_RESULT_TIMEOUT The deadline expired.
  * @retval TRANSPORT_RESULT_IO_ERROR The backend sent a partial sequence.
  */
TransportResult_e xTransportSend(TransportChannel_t *pxChannel,
								 const uint8_t *pucData,
								 uint16_t usDataLen,
								 uint32_t ulTimeoutMs);
/**
  * @brief Receive up to the supplied buffer capacity.
  * @param[in,out] pxChannel Open registered channel.
  * @param[out] pucData Caller-owned receive buffer.
  * @param[in] usMaxLen Buffer capacity in bytes.
  * @param[out] pusReceivedLen Number of bytes received.
  * @param[in] ulTimeoutMs Receive timeout in milliseconds.
  * @retval TRANSPORT_RESULT_OK Bytes were received or the peer returned a
  *         valid zero-length result.
  * @retval TRANSPORT_RESULT_INVALID_ARG A pointer or capacity was invalid.
  * @retval TRANSPORT_RESULT_TIMEOUT The bounded receive deadline expired.
  * @retval TRANSPORT_RESULT_DISCONNECTED The peer closed the channel.
  * @retval Other TransportResult_e Backend receive failed.
  * @note A partial receive may return TRANSPORT_RESULT_OK.
  */
TransportResult_e xTransportReceive(TransportChannel_t *pxChannel,
									uint8_t *pucData,
									uint16_t usMaxLen,
									uint16_t *pusReceivedLen,
									uint32_t ulTimeoutMs);

/**
  * @brief Receive an exact byte count within one total deadline.
  * @param[in,out] pxChannel Open registered channel.
  * @param[out] pucData Caller-owned destination buffer.
  * @param[in] usExpectedLen Exact required byte count.
  * @param[out] pusReceivedLen Bytes accumulated before return.
  * @param[in] ulTimeoutMs Total deadline in milliseconds.
  * @retval TRANSPORT_RESULT_OK The exact byte count was received.
  * @retval TRANSPORT_RESULT_TIMEOUT The deadline expired; partial data remains.
  * @note Intermediate receives reuse the original absolute deadline.
  */
TransportResult_e xTransportReceiveExact(TransportChannel_t *pxChannel,
										 uint8_t *pucData,
										 uint16_t usExpectedLen,
										 uint16_t *pusReceivedLen,
										 uint32_t ulTimeoutMs);

/**
  * @brief Dispatch a generic control request to a channel backend.
  * @param[in,out] pxChannel Registered channel.
  * @param[in] xCommand Control operation to execute.
  * @param[in,out] pvArgument Command-specific argument or NULL.
  * @return Normalized backend result.
  */
TransportResult_e xTransportControl(TransportChannel_t *pxChannel,
									TransportControl_e xCommand,
									void *pvArgument);

/**
  * @brief Read a channel's current backend lifecycle state.
  * @param[in] pxChannel Registered channel.
  * @return Current state, or TRANSPORT_STATE_UNINITIALIZED when invalid.
  */
TransportState_e xTransportGetState(TransportChannel_t *pxChannel);

/**
  * @brief Copy a channel's runtime counters and latest fault.
  * @param[in] pxChannel Registered channel.
  * @param[out] pxStatus Caller-owned status destination.
  * @retval TRANSPORT_RESULT_OK Status was copied.
  * @retval TRANSPORT_RESULT_INVALID_ARG A pointer is NULL.
  */
TransportResult_e xTransportGetStatus(TransportChannel_t *pxChannel,
	TransportStatus_t *pxStatus);

/**
  * @brief Set or clear a channel's asynchronous event callback.
  * @param[in,out] pxChannel Registered channel.
  * @param[in] pxCallback Callback, or NULL to disable notifications.
  * @param[in] pvCallbackContext Context passed to the callback.
  */
void vTransportSetEventCallback(TransportChannel_t *pxChannel,
								TransportEventCallback_t pxCallback,
								void *pvCallbackContext);

/**
  * @brief Forward one backend event from an interrupt context.
  * @param[in] pxChannel Channel that produced the event.
  * @param[in] xEvent Event classification.
  * @param[in] pucData Optional event data.
  * @param[in] usDataLen Number of valid event-data bytes.
  * @param[in,out] pxHigherPriorityTaskWoken FreeRTOS ISR wake flag.
  * @warning The registered callback must be ISR-safe and nonblocking.
  */
void vTransportNotifyEventFromISR(TransportChannel_t *pxChannel,
								  TransportEvent_e xEvent,
								  const uint8_t *pucData,
								  uint16_t usDataLen,
								  BaseType_t *pxHigherPriorityTaskWoken);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_H */
