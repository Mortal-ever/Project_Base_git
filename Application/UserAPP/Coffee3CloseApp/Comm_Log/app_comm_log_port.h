/**
  * @file      app_comm_log_port.h
  * @brief     声明 Coffee3 致命故障路径使用的 USART1 兼容端口。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef APP_COMM_LOG_PORT_H
#define APP_COMM_LOG_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

int32_t lAppCommLogPortCrashWrite(const uint8_t *pucData,
	uint16_t usLength);

#ifdef __cplusplus
}
#endif

#endif /* APP_COMM_LOG_PORT_H */
