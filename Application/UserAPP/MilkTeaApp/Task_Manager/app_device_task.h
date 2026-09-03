/**
  * @file      app_device_task.h
  * @brief     Define common device-task communication state.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_DEVICE_TASK_H
#define APP_DEVICE_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"

/** @brief Define the common communication lifecycle of a device task. */
typedef enum {
	APP_DEVICE_COMM_WAIT_NETWORK = 0, /*!< Wait for netif, link, and IPv4. */
	APP_DEVICE_COMM_WAIT_SERVICE = 1, /*!< Probe the connected peer service. */
	APP_DEVICE_COMM_CONNECTING = 2, /*!< Open the transport endpoint. */
	APP_DEVICE_COMM_ONLINE = 3, /*!< Process normal device transactions. */
	APP_DEVICE_COMM_RETRY_DELAY = 4 /*!< Wait for reconnect backoff. */
} AppDeviceCommState_e;

/** @brief Physical network became ready. */
#define APP_DEVICE_EVENT_NETWORK_UP        (1UL << 0)
/** @brief Physical network became unavailable. */
#define APP_DEVICE_EVENT_NETWORK_DOWN      (1UL << 1)
/** @brief Transport endpoint opened successfully. */
#define APP_DEVICE_EVENT_ENDPOINT_UP       (1UL << 2)
/** @brief Transport endpoint closed or failed. */
#define APP_DEVICE_EVENT_ENDPOINT_DOWN     (1UL << 3)
/** @brief Latest protocol request completed successfully. */
#define APP_DEVICE_EVENT_REQUEST_OK        (1UL << 4)
/** @brief Latest protocol request failed. */
#define APP_DEVICE_EVENT_REQUEST_FAILED    (1UL << 5)
/** @brief Peer service responded to its probe. */
#define APP_DEVICE_EVENT_PEER_UP           (1UL << 6)
/** @brief Peer service stopped responding. */
#define APP_DEVICE_EVENT_PEER_DOWN         (1UL << 7)

/**
  * @brief Store the task-local communication lifecycle snapshot.
  * @note The owning device task is the only writer.
  */
typedef struct {
	AppDeviceCommState_e xState; /*!< Current lifecycle state. */
	TickType_t xNextRetryTick; /*!< Earliest Tick for the next open attempt. */
	uint32_t ulEventFlags; /*!< Events raised during the current cycle. */
	uint32_t ulLastEventFlags; /*!< Events published by the previous cycle. */
	uint32_t ulStateSequence; /*!< Monotonic state-transition counter. */
	uint32_t ulConnectAttemptCount; /*!< Number of endpoint open attempts. */
	uint32_t ulConnectSuccessCount; /*!< Number of successful opens. */
	uint32_t ulDisconnectCount; /*!< Number of endpoint disconnects. */
	int32_t lLastNativeError; /*!< Latest backend-native error value. */
	uint8_t ucNetworkReady; /*!< Nonzero when network prerequisites exist. */
	uint8_t ucEndpointReady; /*!< Nonzero when the endpoint is open. */
	uint8_t ucTxAllowed; /*!< Nonzero when product traffic may be sent. */
} AppDeviceCommContext_t;

#ifdef __cplusplus
}
#endif

#endif /* APP_DEVICE_TASK_H */
