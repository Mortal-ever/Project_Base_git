/**
  * @file      app_lwip_alert.h
  * @brief     定义面向所有产品目标的 LwIP 资源故障告警接口。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   产品目标在网络接口失败后调用本服务。启用 LWIP_STATS
  *            和 MEMP_STATS 时截取内存池统计并记录诊断；
  *            关闭相关统计配置时对应采集路径不执行操作。
  */

#ifndef APP_LWIP_ALERT_H
#define APP_LWIP_ALERT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "Log/app_log.h"

void vAppLwipAlertReportFailure(AppLogSourceId_t xSource,
	int32_t lNativeError);

#ifdef __cplusplus
}
#endif

#endif /* APP_LWIP_ALERT_H */
