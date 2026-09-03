/**
  * @file      app_ota_http.h
  * @brief     Define the product-neutral raw-lwIP HTTP OTA service.
  * @author    WHong
  * @date      2026-08-25
  */

#ifndef APP_OTA_HTTP_H
#define APP_OTA_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "FreeRTOS.h"

/**
  * @brief  Queue raw-lwIP HTTP listener initialization on the tcpip thread.
  * @retval pdPASS The listener is ready or initialization was queued.
  * @retval pdFAIL The OTA configuration is unavailable or queueing failed.
  * @note   Call after MX_LWIP_Init() has started the tcpip thread.
  */
BaseType_t xAppOtaHttpInitialize(void);

/** @brief Return nonzero while an HTTP upload owns the OTA session. */
uint8_t ucAppOtaHttpIsActive(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_OTA_HTTP_H */
