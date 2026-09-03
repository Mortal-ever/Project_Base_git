/**
  * @file      app_lwip_alert.h
  * @brief     Define event-triggered LwIP resource alerts for all targets.
  * @author    WHong
  * @date      2026-08-27
  *
  * @details   A target calls this service only after a network API failure.
  *            The service snapshots LwIP allocation statistics and writes
  *            diagnostic records through the common AppLog interface.
  */

#ifndef APP_LWIP_ALERT_H
#define APP_LWIP_ALERT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "Log/app_log.h"

/**
  * @brief  Report one network API failure with an LwIP resource snapshot.
  * @param[in] xSource Target-defined log source associated with the failure.
  * @param[in] lNativeError Native LwIP or socket error value.
  * @note   Task-context only. The function performs no polling, allocation,
  *         retry, or control-flow change. When LwIP statistics are disabled,
  *         it compiles to a no-operation implementation.
  */
void vAppLwipAlertReportFailure(AppLogSourceId_t xSource,
	int32_t lNativeError);

#ifdef __cplusplus
}
#endif

#endif /* APP_LWIP_ALERT_H */
