/**
  * @file      coffee3_crash_log_port.c
  * @brief     实现不依赖调度器的 Coffee3 崩溃信息输出。
  * @author    WHong
  * @date      2026-09-24
  */

#include "app_comm_log_port.h"

#include <stddef.h>

#include "app_crash_diag.h"
#include "app_crash_diag_config.h"
#include "stm32f4xx_ll_usart.h"
#include "usart.h"

/*-----------------------------------------------------------*/
/**
  * @brief  以轮询方式把冻结的崩溃报告写入 USART1。
  * @param[in] pucData 待发送的报告字节；不可为空。
  * @param[in] usLength 报告长度，单位为字节；必须大于零。
  * @retval 0 全部字节发送完毕且发送完成标志已置位。
  * @retval -1 参数无效，或 USART1 尚未启用发送功能。
  * @retval -2 等待发送寄存器或发送完成标志时超过自旋上限。
  * @note 本函数关闭 USART1 的 DMA 发送位且不会恢复，用于致命故障路径。
  */
int32_t lAppCommLogPortCrashWrite(const uint8_t *pucData,
	uint16_t usLength)
{
	USART_TypeDef *pxInstance; /*!< USART1 寄存器实例。 */
	uint32_t ulSpin;           /*!< 单次标志等待的剩余自旋次数。 */
	uint16_t usIndex;          /*!< 当前发送字节在报告中的索引。 */

	if ((pucData == NULL) || (usLength == 0U) ||
		(huart1.Instance == NULL)) {
		return -1;
	}
	pxInstance = huart1.Instance;
	if ((LL_USART_IsEnabled(pxInstance) == 0U) ||
		((pxInstance->CR1 & USART_CR1_TE) == 0U)) {
		return -1;
	}
	/* 崩溃路径改用直接寄存器轮询，先停止可能仍挂起的 DMA 发送。 */
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

/**
  * @brief  把公共致命诊断写入钩子绑定到 Coffee3 崩溃输出端口。
  * @param[in] pucData 待发送的报告字节。
  * @param[in] usLength 报告长度，单位为字节。
  * @retval 0 报告发送完毕。
  * @retval 负数 参数、串口状态或轮询等待失败。
  * @note 致命故障路径会关闭 USART1 的 DMA 发送位，返回前不恢复。
  */
int32_t lAppCrashDiagWrite(const uint8_t *pucData, uint16_t usLength)
{
	return lAppCommLogPortCrashWrite(pucData, usLength);
}
