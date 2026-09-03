/**
  * @file      app_comm_log_port.h
  * @brief     Provide the Coffee2 fatal-path USART1 compatibility port.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef APP_COMM_LOG_PORT_H
#define APP_COMM_LOG_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief Write a frozen crash report without RTOS, DMA, or interrupts.
  * @param[in] pucData Report bytes.
  * @param[in] usLength Number of report bytes.
  * @retval 0 Transmission completed.
  * @return Negative value on invalid state or bounded spin timeout.
  */
int32_t lAppCommLogPortCrashWrite(const uint8_t *pucData,
	uint16_t usLength);

#ifdef __cplusplus
}
#endif

#endif /* APP_COMM_LOG_PORT_H */
