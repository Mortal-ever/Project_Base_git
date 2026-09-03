/**
  * @file      CommonTargets.h
  * @brief     Provide one target-facing entry point for common services.
  * @author    WHong
  * @date      2026-08-25
  *
  * @details   This header exposes compiler compatibility, common logging, and
  *            OTA services. It also selects the active target task manager.
  *            It creates no runtime object.
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
#define USE_COFFEE2 0
#endif

#if USE_COFFEE2
#include "coffee2_manager.h"
#endif

#endif /* COMMON_TARGETS_H */
