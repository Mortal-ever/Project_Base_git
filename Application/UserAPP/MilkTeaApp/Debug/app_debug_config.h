/**
  * @file      app_debug_config.h
  * @brief     Configure the UART device-protocol Debug service.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_DEBUG_CONFIG_H
#define APP_DEBUG_CONFIG_H

/** @brief Enable the Debug command task when set to one. */
#define APP_DEBUG_ENABLE                    1U
/** @brief Maximum number of registered Debug devices. */
#define APP_DEBUG_DEVICE_COUNT              4U
/** @brief Maximum input command length including terminator storage. */
#define APP_DEBUG_LINE_LENGTH             192U
/** @brief Maximum formatted Debug output length in bytes. */
#define APP_DEBUG_OUTPUT_LENGTH           600U
/** @brief Number of UART input bytes requested per receive call. */
#define APP_DEBUG_RX_CHUNK_LENGTH          32U
/** @brief UART receive wait per Debug task iteration in milliseconds. */
#define APP_DEBUG_UART_WAIT_MS             20U
/** @brief Endpoint mutex acquisition timeout in milliseconds. */
#define APP_DEBUG_MUTEX_TIMEOUT_MS       1000U
/** @brief Total Modbus transaction timeout in milliseconds. */
#define APP_DEBUG_TRANSACTION_TIMEOUT_MS 1000U
/** @brief Bounded Debug output timeout in milliseconds. */
#define APP_DEBUG_OUTPUT_TIMEOUT_MS       2000U

#endif /* APP_DEBUG_CONFIG_H */
