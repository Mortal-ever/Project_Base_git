/**
  * @file      app_crash_diag.h
  * @brief     定义致命异常和 FreeRTOS 崩溃诊断接口。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   致命事件发生后，在不依赖调度器的条件下采集 CPU、SCB、任务
  *            和栈信息。
  */

#ifndef APP_CRASH_DIAG_H
#define APP_CRASH_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

int32_t lAppCrashDiagWrite(const uint8_t *pucData, uint16_t usLength);

/** @brief 标识进入致命异常、RTOS Hook 或断言诊断的原因。 */
typedef enum {
	APP_CRASH_REASON_HARD_FAULT = 1, /*!< Cortex-M HardFault 异常。 */
	APP_CRASH_REASON_MEM_MANAGE = 2, /*!< Cortex-M MemManage 异常。 */
	APP_CRASH_REASON_BUS_FAULT = 3, /*!< Cortex-M BusFault 异常。 */
	APP_CRASH_REASON_USAGE_FAULT = 4, /*!< Cortex-M UsageFault 异常。 */
	APP_CRASH_REASON_STACK_OVERFLOW = 5, /*!< FreeRTOS 栈溢出 Hook。 */
	APP_CRASH_REASON_MALLOC_FAILED = 6, /*!< FreeRTOS 内存分配失败 Hook。 */
	APP_CRASH_REASON_ASSERT = 7 /*!< configASSERT 失败。 */
} AppCrashReason_e;

/** @brief 调度器跟踪 Hook 最近选择运行的任务句柄。 */
extern void * volatile g_pvAppCrashCurrentTask;

/** @brief ARMCC 异常包装器保存的 R4 至 R11、MSP 和 PSP。 */
extern volatile uint32_t g_aulAppCrashSavedRegisters[10];

void vAppCrashDiagInit(void);

void vAppCrashDiagTraceTaskCreate(void *pvTaskHandle,
	const char *pcTaskName, void *pvStackLow, void *pvStackHigh,
	uint32_t ulPriority);

void vAppCrashDiagTraceTaskDelete(void *pvTaskHandle);

void vAppCrashDiagFaultEntry(uint32_t *pulFaultStack,
	uint32_t ulExcReturn, uint32_t ulReason);

void vAppCrashDiagRtosEntry(void *pvTaskHandle,
	const char *pcTaskName, uint32_t ulReason);

void vAppCrashDiagAssertCEntry(const char *pcFile,
	uint32_t ulLine, uint32_t ulReason);

void vAppCrashDiagAssertEntry(const char *pcFile, uint32_t ulLine);

void vAppCrashDiagWatchdogRefresh(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CRASH_DIAG_H */
