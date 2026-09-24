/**
  * @file      app_ota_http.h
  * @brief     定义与产品无关的 Raw LwIP HTTP OTA 服务接口。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef APP_OTA_HTTP_H
#define APP_OTA_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "FreeRTOS.h"

BaseType_t xAppOtaHttpInitialize(void);

uint8_t ucAppOtaHttpIsActive(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_OTA_HTTP_H */
