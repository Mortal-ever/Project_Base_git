/**
  * @file      app_crash_diag.h
  * @brief     Define fatal exception and FreeRTOS crash diagnostics.
  * @author    WHong
  * @date      2026-07-28
  *
  * @details   Capture CPU, SCB, task, and stack information without relying
  *            on the scheduler after a fatal event.
  */

#ifndef APP_CRASH_DIAG_H
#define APP_CRASH_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief Write one frozen crash report from the fatal path.
  * @note The public diagnostic core provides a weak no-op default.  A target
  *       may provide a strong implementation without making the public core
  *       include a target-private log port header.
  */
int32_t lAppCrashDiagWrite(const uint8_t *pucData, uint16_t usLength);

/** @brief Classify fatal exception, RTOS, and assertion entry reasons. */
typedef enum {
	APP_CRASH_REASON_HARD_FAULT = 1, /*!< Cortex-M HardFault exception. */
	APP_CRASH_REASON_MEM_MANAGE = 2, /*!< Cortex-M MemManage exception. */
	APP_CRASH_REASON_BUS_FAULT = 3, /*!< Cortex-M BusFault exception. */
	APP_CRASH_REASON_USAGE_FAULT = 4, /*!< Cortex-M UsageFault exception. */
	APP_CRASH_REASON_STACK_OVERFLOW = 5, /*!< FreeRTOS stack overflow hook. */
	APP_CRASH_REASON_MALLOC_FAILED = 6, /*!< FreeRTOS allocation failure. */
	APP_CRASH_REASON_ASSERT = 7 /*!< configASSERT failure. */
} AppCrashReason_e;

/** @brief Task handle most recently selected by the scheduler trace hook. */
extern void * volatile g_pvAppCrashCurrentTask;

/** @brief R4-R11, MSP, and PSP captured by the ARMCC exception wrapper. */
extern volatile uint32_t g_aulAppCrashSavedRegisters[10];

/** @brief Initialize the cycle counter used by fatal diagnostic timing. */
void vAppCrashDiagInit(void);

/**
  * @brief Register one task for fatal stack and metadata diagnostics.
  * @param[in] pvTaskHandle FreeRTOS task handle.
  * @param[in] pcTaskName Persistent FreeRTOS task name.
  * @param[in] pvStackLow Inclusive low address of the task stack.
  * @param[in] pvStackHigh Exclusive high address of the task stack.
  * @param[in] ulPriority Initial task priority.
  * @note Called by a FreeRTOS trace hook; it performs no allocation.
  */
void vAppCrashDiagTraceTaskCreate(void *pvTaskHandle,
	const char *pcTaskName, void *pvStackLow, void *pvStackHigh,
	uint32_t ulPriority);

/**
  * @brief Remove one task from the fatal diagnostic registry.
  * @param[in] pvTaskHandle FreeRTOS task handle being deleted.
  */
void vAppCrashDiagTraceTaskDelete(void *pvTaskHandle);

/**
  * @brief Enter fatal diagnostics from a Cortex-M exception wrapper.
  * @param[in] pulFaultStack Selected MSP or PSP exception frame.
  * @param[in] ulExcReturn Exception return value captured in LR.
  * @param[in] ulReason AppCrashReason_e value supplied by the wrapper.
  * @warning This function does not return.
  */
void vAppCrashDiagFaultEntry(uint32_t *pulFaultStack,
	uint32_t ulExcReturn, uint32_t ulReason);

/**
  * @brief Enter fatal diagnostics from a FreeRTOS fatal hook.
  * @param[in] pvTaskHandle Affected task handle, or NULL when unavailable.
  * @param[in] pcTaskName Affected task name, or NULL when unavailable.
  * @param[in] ulReason AppCrashReason_e value.
  * @warning This function does not return.
  */
void vAppCrashDiagRtosEntry(void *pvTaskHandle,
	const char *pcTaskName, uint32_t ulReason);

/**
  * @brief Enter fatal diagnostics from the ARMCC assertion wrapper.
  * @param[in] pcFile Source file string supplied by configASSERT.
  * @param[in] ulLine One-based assertion source line.
  * @param[in] ulReason APP_CRASH_REASON_ASSERT.
  * @warning This function does not return.
  */
void vAppCrashDiagAssertCEntry(const char *pcFile,
	uint32_t ulLine, uint32_t ulReason);

/**
  * @brief Capture registers and enter assertion diagnostics in ARMCC assembly.
  * @param[in] pcFile Source file string supplied by configASSERT.
  * @param[in] ulLine One-based assertion source line.
  * @warning This function does not return.
  */
void vAppCrashDiagAssertEntry(const char *pcFile, uint32_t ulLine);

/**
  * @brief Reload the independent watchdog during fatal output.
  * @note The function is a no-op when watchdog refresh is disabled.
  */
void vAppCrashDiagWatchdogRefresh(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CRASH_DIAG_H */
