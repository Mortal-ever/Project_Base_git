/**
  * @file      app_crash_diag_config.h
  * @brief     Configure bounded fatal diagnostic storage and timing.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_CRASH_DIAG_CONFIG_H
#define APP_CRASH_DIAG_CONFIG_H

/** @brief Enable fatal-fault capture and repeated direct output. */
#define APP_CRASH_DIAG_ENABLE                    1U

/** @brief Refresh IWDG during fatal output when the product starts it. */
#define APP_CRASH_IWDG_REFRESH_ENABLE            0U

/** @brief Maximum number of tasks retained in the static registry. */
#define APP_CRASH_MAX_TASKS                     16U
/** @brief Fatal text-buffer capacity in bytes. */
#define APP_CRASH_TEXT_BUFFER_SIZE            4096U
/** @brief Period between repeated crash reports in seconds. */
#define APP_CRASH_REPEAT_SECONDS                10U
/** @brief IWDG refresh frequency during fatal output in hertz. */
#define APP_CRASH_WATCHDOG_REFRESH_HZ           10U
/** @brief Maximum UART polling iterations per fatal-output byte. */
#define APP_CRASH_UART_SPIN_LIMIT          1000000UL
/** @brief Maximum validated task-stack range in bytes. */
#define APP_CRASH_MAX_STACK_BYTES            16384UL

/** @brief Byte pattern used by FreeRTOS to initialize unused stack space. */
#define APP_CRASH_STACK_FILL_BYTE              0xA5U

#if ((APP_CRASH_DIAG_ENABLE != 0U) && \
	(APP_CRASH_DIAG_ENABLE != 1U))
#error "APP_CRASH_DIAG_ENABLE must be zero or one"
#endif

#if ((APP_CRASH_IWDG_REFRESH_ENABLE != 0U) && \
	(APP_CRASH_IWDG_REFRESH_ENABLE != 1U))
#error "APP_CRASH_IWDG_REFRESH_ENABLE must be zero or one"
#endif

#if (APP_CRASH_MAX_TASKS == 0U)
#error "APP_CRASH_MAX_TASKS must be greater than zero"
#endif

#if (APP_CRASH_TEXT_BUFFER_SIZE < 1024U)
#error "APP_CRASH_TEXT_BUFFER_SIZE is too small"
#endif

#if ((APP_CRASH_REPEAT_SECONDS == 0U) || \
	(APP_CRASH_WATCHDOG_REFRESH_HZ == 0U))
#error "Crash repeat timing must be greater than zero"
#endif

#endif /* APP_CRASH_DIAG_CONFIG_H */
