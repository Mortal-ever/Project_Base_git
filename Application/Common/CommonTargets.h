/**
  * @file      CommonTargets.h
  * @brief     提供面向产品目标的公共服务统一包含入口。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   本头文件集中引入编译器兼容层、日志、网络告警、OTA 与 TCP
  *            会话服务，并按构建宏选择产品任务管理器；不创建运行对象。
  */

#ifndef COMMON_TARGETS_H
#define COMMON_TARGETS_H

#include "compiler_compat.h"
#include "Log/app_log.h"
#include "LwipAlert/app_lwip_alert.h"
#include "Ota/app_ota_flash.h"
#include "Ota/app_ota_http.h"
#include "TcpClientSession/tcp_client_session.h"

#ifndef USE_COFFEE2
/** @brief Coffee2Open 产品目标选择标志，非零时引入其任务管理器。 */
#define USE_COFFEE2 0
#endif
#ifndef USE_COFFEE3
/** @brief Coffee3Close 产品目标选择标志，非零时引入其任务管理器。 */
#define USE_COFFEE3 0
#endif

#if USE_COFFEE2
#include "coffee2_manager.h"
#endif
#if USE_COFFEE3
#include "coffee3_manager.h"
#endif

#endif /* COMMON_TARGETS_H */
