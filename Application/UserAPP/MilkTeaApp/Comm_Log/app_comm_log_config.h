/**
 * @file    app_comm_log_config.h
 * @brief   日志子系统全局配置
 *
 * 本文件集中管理日志子系统的所有编译期开关和参数。
 * 所有日志模块（核心、端口层、传输层）都依赖此文件的宏定义。
 *
 * 配置原则：
 * - 只修改两个输出开关即可控制 UART/网络输出的启停
 * - APP_COMM_LOG_ENABLE 由开关自动派生，不要手动修改
 * - 心跳、摘要周期和逐帧开关可根据调试需要临时调整
 * - 修改任何宏后必须编译验证且为 0 Error 0 Warning
 *
 * 当前默认输出：USART1（通过传输层统一发送）
 */

#ifndef APP_COMM_LOG_CONFIG_H
#define APP_COMM_LOG_CONFIG_H

/**
 * @name 输出开关
 *
 * 通过这两个开关选择日志输出目标，四个组合各有不同的行为：
 *   UART=1 NETWORK=0 → 默认模式，仅 UART 输出
 *   UART=0 NETWORK=1 → 仅网络输出（当前弱实现，返回 NOT_READY）
 *   UART=1 NETWORK=1 → 双路输出，同一份数据先 UART 再网络
 *   UART=0 NETWORK=0 → 日志完全关闭，LOG 任务不创建，API 为空实现
 *
 * @{
 */
#define APP_COMM_LOG_UART_OUTPUT_ENABLE          1U  /**< UART 日志输出开关（1=开启 0=关闭） */
#define APP_COMM_LOG_NETWORK_OUTPUT_ENABLE       0U  /**< 网络日志输出开关（1=开启 0=关闭） */
/** @} */

/**
 * @brief 启动前早期日志开关
 *
 * 调度器启动前的启动探针（如 "BOOT USART1 READY"）只能通过 UART 轮询输出。
 * lwIP 和信号量此时尚未初始化，网络和 DMA 模式不可用。
 * 此宏与 UART 输出开关绑定，UART 关闭时早期日志也自动关闭。
 */
#define APP_COMM_LOG_EARLY_UART_ENABLE \
	APP_COMM_LOG_UART_OUTPUT_ENABLE

/**
 * @name 格式化与队列参数
 *
 * LOG 任务单次格式化一条日志行，最大 LINE_LENGTH 字节（含 CRLF）。
 * 结构化事件队列固定 QUEUE_LENGTH 条，无动态分配。
 * UART_TIMEOUT_MS 是单次 xTransportSend 的超时预算。
 *
 * @{
 */
#define APP_COMM_LOG_LINE_LENGTH                160U  /**< 单条日志行最大字节数（含 CRLF） */
#define APP_COMM_LOG_QUEUE_LENGTH                32U  /**< 结构化事件队列深度 */
#define APP_COMM_LOG_UART_TIMEOUT_MS            100U  /**< UART 发送超时（毫秒） */
/** @} */

/**
 * @name 输出频率策略参数
 *
 * HEARTBEAT_PERIOD_MS：状态未变化时每 5 秒输出一次心跳
 * REPEAT_SUMMARY_PERIOD_MS：相同错误每 5 秒输出一次累计摘要
 * MILKTEA_FRAME_TRACE_ENABLE: MilkTea Modbus frame log switch.
 *
 * @{
 */
#define APP_COMM_LOG_HEARTBEAT_PERIOD_MS        5000U  /**< 状态心跳输出间隔（毫秒） */
#define APP_COMM_LOG_REPEAT_SUMMARY_PERIOD_MS   5000U  /**< 重复错误摘要输出间隔（毫秒） */
#define APP_COMM_LOG_MILKTEA_FRAME_TRACE_ENABLE    0U  /**< MilkTea frame log switch. */
/** @} */

/*
 * TEMPORARY CCM FAULT INJECTION TEST.
 *
 * When enabled, the LOG task writes to the first address beyond the STM32F407
 * 64 KB CCM region after attempting its startup log. This intentionally
 * causes a BusFault/HardFault so the fatal diagnostic UART path can be tested.
 *
 * Set this macro to zero or remove this complete test block after validation.
 */
#define APP_COMM_LOG_CCM_FAULT_INJECTION_ENABLE    0U

/**
 * @brief 日志总开关（由两个输出开关自动派生，请勿手动修改）
 *
 * 只要 UART 或网络任一输出开启，日志即启用。
 * 两个都关闭时，LOG 任务不创建，所有日志 API 编译为空实现（零开销）。
 */
#define APP_COMM_LOG_ENABLE \
	((APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U) || \
	 (APP_COMM_LOG_NETWORK_OUTPUT_ENABLE != 0U))

/* ==================== 编译期约束检查 ==================== */

#if ((APP_COMM_LOG_UART_OUTPUT_ENABLE != 0U) && \
	(APP_COMM_LOG_UART_OUTPUT_ENABLE != 1U))
#error "APP_COMM_LOG_UART_OUTPUT_ENABLE must be zero or one"
#endif

#if ((APP_COMM_LOG_NETWORK_OUTPUT_ENABLE != 0U) && \
	(APP_COMM_LOG_NETWORK_OUTPUT_ENABLE != 1U))
#error "APP_COMM_LOG_NETWORK_OUTPUT_ENABLE must be zero or one"
#endif

#if ((APP_COMM_LOG_EARLY_UART_ENABLE != 0U) && \
	(APP_COMM_LOG_EARLY_UART_ENABLE != 1U))
#error "APP_COMM_LOG_EARLY_UART_ENABLE must be zero or one"
#endif

#if (APP_COMM_LOG_LINE_LENGTH < 128U)
#error "APP_COMM_LOG_LINE_LENGTH is too small for structured log lines"
#endif

#if (APP_COMM_LOG_HEARTBEAT_PERIOD_MS == 0U)
#error "APP_COMM_LOG_HEARTBEAT_PERIOD_MS must be greater than zero"
#endif

#if (APP_COMM_LOG_REPEAT_SUMMARY_PERIOD_MS == 0U)
#error "APP_COMM_LOG_REPEAT_SUMMARY_PERIOD_MS must be greater than zero"
#endif

#if ((APP_COMM_LOG_MILKTEA_FRAME_TRACE_ENABLE != 0U) && \
	(APP_COMM_LOG_MILKTEA_FRAME_TRACE_ENABLE != 1U))
#error "APP_COMM_LOG_MILKTEA_FRAME_TRACE_ENABLE must be zero or one"
#endif

#if ((APP_COMM_LOG_CCM_FAULT_INJECTION_ENABLE != 0U) && \
	(APP_COMM_LOG_CCM_FAULT_INJECTION_ENABLE != 1U))
#error "APP_COMM_LOG_CCM_FAULT_INJECTION_ENABLE must be zero or one"
#endif

#endif /* APP_COMM_LOG_CONFIG_H */
