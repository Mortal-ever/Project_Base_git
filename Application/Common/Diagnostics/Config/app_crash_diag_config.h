/**
  * @file      app_crash_diag_config.h
  * @brief     配置致命诊断的定长存储和输出时序。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef APP_CRASH_DIAG_CONFIG_H
#define APP_CRASH_DIAG_CONFIG_H

/** @brief 非零时启用致命故障采集和重复直接输出。 */
#define APP_CRASH_DIAG_ENABLE                    1U

/** @brief 产品已启动 IWDG 时，非零可在致命输出期间刷新看门狗。 */
#define APP_CRASH_IWDG_REFRESH_ENABLE            0U

/** @brief 静态任务登记表可保留的最大任务数。 */
#define APP_CRASH_MAX_TASKS                     16U
/** @brief 冻结崩溃文本缓冲容量，单位为字节。 */
#define APP_CRASH_TEXT_BUFFER_SIZE            4096U
/** @brief 两次重复崩溃报告之间的周期，单位为秒。 */
#define APP_CRASH_REPEAT_SECONDS                10U
/** @brief 致命输出等待期间的 IWDG 刷新频率，单位为赫兹。 */
#define APP_CRASH_WATCHDOG_REFRESH_HZ           10U
/** @brief 每个致命输出字节允许的 UART 轮询次数上限。 */
#define APP_CRASH_UART_SPIN_LIMIT          1000000UL
/** @brief 允许扫描的单任务栈范围上限，单位为字节。 */
#define APP_CRASH_MAX_STACK_BYTES            16384UL

/** @brief FreeRTOS 初始化未使用栈空间时填充的字节模式。 */
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
