/**
  * @file      coffee2_crash_log_port.c
  * @brief     Implement scheduler-independent Coffee2 crash output.
  * @author    WHong
  * @date      2026-07-30
  */

#include "app_comm_log_port.h"

#include <stddef.h>

#include "app_crash_diag.h"
#include "app_crash_diag_config.h"
#include "stm32f4xx_ll_usart.h"
#include "usart.h"

/*-----------------------------------------------------------*/
int32_t lAppCommLogPortCrashWrite(const uint8_t *pucData,
	uint16_t usLength)
{
	USART_TypeDef *pxInstance;
	uint32_t ulSpin;
	uint16_t usIndex;

	if ((pucData == NULL) || (usLength == 0U) ||
		(huart1.Instance == NULL)) {
		return -1;
	}
	pxInstance = huart1.Instance;
	if ((LL_USART_IsEnabled(pxInstance) == 0U) ||
		((pxInstance->CR1 & USART_CR1_TE) == 0U)) {
		return -1;
	}
	CLEAR_BIT(pxInstance->CR3, USART_CR3_DMAT);
	for (usIndex = 0U; usIndex < usLength; usIndex++) {
		vAppCrashDiagWatchdogRefresh();
		ulSpin = APP_CRASH_UART_SPIN_LIMIT;
		while (LL_USART_IsActiveFlag_TXE(pxInstance) == 0U) {
			ulSpin--;
			if (ulSpin == 0U) {
				return -2;
			}
			if ((ulSpin & 0x3FFFUL) == 0U) {
				vAppCrashDiagWatchdogRefresh();
			}
		}
		LL_USART_TransmitData8(pxInstance, pucData[usIndex]);
	}
	ulSpin = APP_CRASH_UART_SPIN_LIMIT;
	while (LL_USART_IsActiveFlag_TC(pxInstance) == 0U) {
		ulSpin--;
		if (ulSpin == 0U) {
			return -2;
		}
		if ((ulSpin & 0x3FFFUL) == 0U) {
			vAppCrashDiagWatchdogRefresh();
		}
	}
	return 0;
}

/* Strong Coffee2 binding for the public fatal diagnostic hook. */
int32_t lAppCrashDiagWrite(const uint8_t *pucData, uint16_t usLength)
{
	return lAppCommLogPortCrashWrite(pucData, usLength);
}
