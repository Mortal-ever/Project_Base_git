/**
 * @file    app_comm_log.h
 * @brief   日志子系统公共头文件（核心 API）
 *
 * 本文件定义了日志子系统的全部公共类型和 API，是业务代码使用日志系统
 * 的唯一入口。业务任务只需调用 vAppCommLogWrite() 等提交结构化事件，
 * 不直接操作 UART、网络或格式化逻辑。
 *
 * 核心设计：
 * - 生产者-消费者模型：业务任务将结构化事件写入固定 32 条环形队列
 * - 单一 LOG 任务从队列取出事件，格式化一次后通过 s_xBackend.pxWrite 发送
 * - 支持三类输出策略：即时事件、状态去重心跳、重复错误摘要
 * - 后端可动态替换（如 HTTP 替代 UART）
 * - 两个输出都关闭时，整个日志系统编译为零开销（LOG 任务不创建，API 为空宏）
 *
 * 调用链：
 *   业务任务 → vAppCommLogWrite() / ucAppCommLogWriteState() / vAppCommLogWriteRateLimited()
 *     → prvQueueEntry() 入队
 *       → xTaskNotifyGive() 唤醒 LOG 任务
 *         → LOG 任务: prvTakePendingEntry() 出队
 *           → prvFormatEntry() 格式化
 *             → s_xBackend.pxWrite() 后端写
 *               → prvPortWrite() 端口分发
 *                 → xTransportSend() (UART) / lAppCommLogNetworkWrite() (网络)
 */

#ifndef APP_COMM_LOG_H
#define APP_COMM_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "app_comm_log_config.h"

/**
 * @brief 日志事件来源枚举
 *
 * 每个枚举值对应一个具体的业务操作阶段，用于标识日志的 Task、Module 和 EVENT。
 * 业务代码调用 API 时传入此枚举，LOG 任务格式化时根据它查找对应的文本标签。
 *
 * 编号 0~20 也是状态槽和重复错误槽的数组索引，不可随意调整顺序。
 */
typedef enum {
	APP_COMM_SOURCE_BOOT = 0,             /**< 调度器启动前的引导探针 */
	APP_COMM_SOURCE_TASK = 1,             /**< FreeRTOS 任务创建/启动 */
	APP_COMM_SOURCE_NETWORK = 2,          /**< 网络接口/IPv4 就绪状态（lwIP netif） */
	APP_COMM_SOURCE_MODBUS_INIT = 3,      /**< Modbus 引擎共享初始化 */
	APP_COMM_SOURCE_ROBOT_OPEN = 4,       /**< Robot 机器人 TCP 连接发起 */
	APP_COMM_SOURCE_ROBOT_TX = 5,         /**< Robot Modbus 请求帧发送 */
	APP_COMM_SOURCE_ROBOT_RX = 6,         /**< Robot Modbus 响应帧接收等待 */
	APP_COMM_SOURCE_ROBOT_MODBUS = 7,     /**< Robot Modbus 事务协议结果 */
	APP_COMM_SOURCE_MILKTEA_OPEN = 8,     /**< 奶茶机 TCP 连接启动 */
	APP_COMM_SOURCE_MILKTEA_REQUEST = 9,  /**< 奶茶机 Modbus 请求结果 */
	APP_COMM_SOURCE_MILKTEA_LINK = 10,    /**< 奶茶机连接/断开 */
	APP_COMM_SOURCE_MILKTEA_HEALTH = 11,  /**< 奶茶机通信健康心跳 */
	APP_COMM_SOURCE_LOG = 12,             /**< 日志后端自身启动诊断 */
	APP_COMM_SOURCE_ROBOT_CONNECT = 13,   /**< Robot TCP 阻塞 Connect 边界 */
	APP_COMM_SOURCE_ROBOT_CLOSE = 14,     /**< Robot TCP 阻塞 Close 边界 */
	APP_COMM_SOURCE_ROBOT_HEALTH = 15,    /**< Robot 机器人连接健康心跳 */
	APP_COMM_SOURCE_MILKTEA_SESSION = 16, /**< 奶茶机连接会话变化 */
	APP_COMM_SOURCE_MILKTEA_GUARD = 17,   /**< 奶茶机连接保护动作 */
	APP_COMM_SOURCE_MILKTEA_FRAME = 18,   /**< 奶茶机 Modbus ADU 处理结果 */
	APP_COMM_SOURCE_MILKTEA_FLOW = 19,    /**< 奶茶制作工作流步骤 */
	APP_COMM_SOURCE_RUNNING_STATE = 20,   /**< 寄存器 50 的奶茶机运行状态监控 */
	APP_COMM_SOURCE_COUNT = 21            /**< 有效日志源总数（用于数组维度） */
} AppCommSource_e;

/** @brief 运行状态事件类：心跳（值未变） */
#define APP_COMM_RUNNING_STATE_EVENT_HEARTBEAT       0
/** @brief 运行状态事件类：状态已变更 */
#define APP_COMM_RUNNING_STATE_EVENT_CHANGED         1

/**
 * @brief 奶茶制作工作流步骤枚举
 *
 * 由 APP_COMM_SOURCE_MILKTEA_FLOW 使用，覆盖从就绪到制作成功/失败、
 * 再到暂停/中止/模拟的全流程。含 24 个步骤，其中 EXECUTOR_MISSING 和
 * PRODUCTION_TIMEOUT 为安全错误等级。
 */
typedef enum {
	APP_COMM_MILKTEA_READY = 0,              /**< 就绪，等待制作指令 */
	APP_COMM_MILKTEA_MAKE_ACCEPTED = 1,      /**< 制作指令已接受 */
	APP_COMM_MILKTEA_MAKE_REJECTED_EMPTY = 2,/**< 拒绝制作：原料不足 */
	APP_COMM_MILKTEA_MAKE_REJECTED_BUSY = 3, /**< 拒绝制作：正忙 */
	APP_COMM_MILKTEA_MAKE_RECEIVED = 4,      /**< 制作指令已收到 */
	APP_COMM_MILKTEA_IDENTIFIER_VALID = 5,   /**< 配方标识符合法 */
	APP_COMM_MILKTEA_IDENTIFIER_INVALID = 6, /**< 配方标识符非法 */
	APP_COMM_MILKTEA_EXECUTOR_START = 7,     /**< 执行器启动 */
	APP_COMM_MILKTEA_EXECUTOR_RUNNING = 8,   /**< 执行器运行中 */
	APP_COMM_MILKTEA_EXECUTOR_PROGRESS = 9,  /**< 执行器进度更新 */
	APP_COMM_MILKTEA_COMPLETE_SUCCESS = 10,  /**< 制作成功完成 */
	APP_COMM_MILKTEA_COMPLETE_FAILED = 11,   /**< 制作失败 */
	APP_COMM_MILKTEA_ABORT_ACCEPTED = 12,    /**< 中止请求已接受 */
	APP_COMM_MILKTEA_ABORT_IGNORED = 13,     /**< 中止请求被忽略 */
	APP_COMM_MILKTEA_ABORT_REQUEST = 14,     /**< 发起中止请求 */
	APP_COMM_MILKTEA_ABORT_WAIT = 15,        /**< 等待中止完成 */
	APP_COMM_MILKTEA_ABORT_COMPLETE = 16,    /**< 中止已完成 */
	APP_COMM_MILKTEA_ABORT_FAILED = 17,      /**< 中止失败 */
	APP_COMM_MILKTEA_EXECUTOR_MISSING = 18,  /**< 执行器缺失（安全错误） */
	APP_COMM_MILKTEA_PRODUCTION_TIMEOUT = 19,/**< 制作超时（安全错误） */
	APP_COMM_MILKTEA_SIMULATION_ENABLED = 20,/**< 模拟模式已启用 */
	APP_COMM_MILKTEA_SIMULATION_START = 21,  /**< 模拟制作开始 */
	APP_COMM_MILKTEA_SIMULATION_COMPLETE = 22,/**< 模拟制作完成 */
	APP_COMM_MILKTEA_SIMULATION_ABORT = 23   /**< 模拟制作中止 */
} AppCommMilkTeaStep_e;

/**
 * @brief 日志子系统操作结果码
 *
 * 正值表示成功（但含义不同），负值表示错误。
 */
typedef enum {
	APP_COMM_LOG_RESULT_OK = 0,           /**< 操作成功 */
	APP_COMM_LOG_RESULT_REPLACED = 1,     /**< 已有后端被替换 */
	APP_COMM_LOG_RESULT_INVALID_ARG = -1, /**< 参数无效（空指针等） */
	APP_COMM_LOG_RESULT_NOT_READY = -2,   /**< 后端未注册或系统未就绪 */
	APP_COMM_LOG_RESULT_BACKEND = -3      /**< 后端写入失败 */
} AppCommLogResult_e;

/**
 * @brief 日志输出触发原因
 *
 * 用于标记一条日志是因为什么策略触发的输出。
 * LOG 任务格式化时据此决定 Level 和 event= 字段。
 */
typedef enum {
	APP_COMM_LOG_EMISSION_EVENT = 0,          /**< 即时事件（每次都输出） */
	APP_COMM_LOG_EMISSION_STATE_CHANGED = 1,  /**< 状态首次出现或发生变化 */
	APP_COMM_LOG_EMISSION_STATE_HEARTBEAT = 2,/**< 状态未变化，周期心跳 */
	APP_COMM_LOG_EMISSION_ERROR_FIRST = 3,    /**< 重复错误首次出现 */
	APP_COMM_LOG_EMISSION_ERROR_SUMMARY = 4   /**< 重复错误周期摘要 */
} AppCommLogEmission_e;

/**
 * @brief 单条结构化日志事件
 *
 * 业务任务将事件字段填入此结构体，通过提交 API 写入队列。
 * LOG 任务从队列取出后再格式化为人类可读文本。
 * 这是一个暂态记录，不保存历史文本。
 */
typedef struct {
	TickType_t xTimestamp;               /**< 事件提交时的 FreeRTOS tick */
	AppCommSource_e xSource;             /**< 事件发生的具体操作阶段 */
	int32_t lResult;                     /**< 来源特定的归一化结果码 */
	int32_t lNativeError;                /**< 原生错误码（lwIP err_t / HAL status 等） */
	int32_t lAuxValue;                   /**< 辅助数据（如 Modbus FC 和异常码组合） */
	uint32_t ulRepeatCount;              /**< 重复错误摘要中代表的抑制次数 */
	AppCommLogEmission_e xEmission;      /**< 本次输出的触发原因 */
} AppCommLogEntry_t;

/**
 * @brief 后端写入函数指针类型
 *
 * 所有日志后端必须实现此签名。函数内必须同步消费或复制全部数据再返回。
 *
 * @param pvContext  后端上下文（由注册时提供）
 * @param pucData    格式化后的日志文本指针（可能位于 CCM，不能长期持有）
 * @param usLength   文本长度（字节）
 * @return 0 成功，非 0 失败
 */
typedef int32_t (*AppCommLogBackendWrite_t)(void *pvContext,
	const uint8_t *pucData, uint16_t usLength);

/**
 * @brief 日志后端描述符
 *
 * 包含写入函数指针和后端私有上下文。通过 xAppCommLogRegisterBackend()
 * 注册到日志核心后，LOG 任务通过此结构体调用底层输出。
 */
typedef struct {
	AppCommLogBackendWrite_t pxWrite; /**< 后端字节写入函数 */
	void *pvContext;                  /**< 后端私有上下文（如 UART 通道句柄） */
} AppCommLogBackend_t;

/**
 * @brief 日志子系统整体运行统计
 *
 * 仅包含计数器，不包含历史文本。Keil Watch 可直接监视 g_xAppCommLogStatus。
 * 业务代码通过 vAppCommLogGetStatus() 获取线程安全的快照。
 */
typedef struct {
	uint32_t ulSentCount;            /**< 至少一个输出成功的日志条数 */
	uint32_t ulDroppedCount;         /**< 队列满而丢弃的事件数 */
	uint32_t ulBackendFailureCount;  /**< 所有已启用输出均失败的次数 */
	uint32_t ulStateChangeCount;     /**< 状态首次/变更时立即输出的次数 */
	uint32_t ulHeartbeatCount;       /**< 状态未变更时周期心跳输出的次数 */
	uint32_t ulRateLimitedCount;     /**< 被合并到摘要中的重复错误次数 */
	uint32_t ulRepeatSummaryCount;   /**< 已输出的重复错误摘要条数 */
	int32_t lLastBackendError;       /**< 最近一次后端写入的总结果码 */
	uint16_t usPendingCount;         /**< 当前队列中等待发送的事件数 */
	uint16_t usQueueHighWatermark;   /**< 历史最高队列深度 */
	uint8_t ucBackendRegistered;     /**< 后端是否已注册（非零=已注册） */
} AppCommLogStatus_t;

/** @brief Keil Watch 可直接监视的全局日志状态变量 */
extern AppCommLogStatus_t g_xAppCommLogStatus;

/**
 * @brief LOG 任务入口函数
 *
 * 日志子系统的唯一后端 I/O 所有者。启动时注册输出路由，
 * 之后循环从队列取出事件、格式化并发送到后端。
 * 无待处理事件时通过 ulTaskNotifyTake 挂起等待。
 *
 * @param pvArgument FreeRTOS 任务参数（未使用）
 */
void vAppCommLogTask(void *pvArgument);

/**
 * @brief 注册/替换日志输出后端
 *
 * 日志系统启动时由端口层调用一次。也可在运行时调用以动态替换后端
 * （如从 UART 切换到网络）。同一时间只有一个激活的后端。
 *
 * @param pxBackend 后端描述符（包含写函数和上下文）
 * @return APP_COMM_LOG_RESULT_OK 首次注册成功
 *         APP_COMM_LOG_RESULT_REPLACED 替换了已存在的后端
 *         APP_COMM_LOG_RESULT_INVALID_ARG 后端或写函数为空
 */
AppCommLogResult_e xAppCommLogRegisterBackend(
	const AppCommLogBackend_t *pxBackend);

/**
 * @brief 提交即时事件到日志队列（简化版）
 *
 * 业务任务调用此函数提交一条即时事件。队列满时丢弃并累加 ulDroppedCount。
 * 内部调用 vAppCommLogWriteExtended()，AuxValue 固定为 0。
 *
 * @param xSource       事件来源（如 APP_COMM_SOURCE_MILKTEA_FLOW）
 * @param lResult       来源特定的归一化结果码
 * @param lNativeError  原生错误码（lwIP err_t / HAL status 等）
 */
void vAppCommLogWrite(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError);

/**
 * @brief 提交即时事件到日志队列（扩展版）
 *
 * 与 vAppCommLogWrite() 相比多了 lAuxValue 参数，
 * 用于携带订单号、Modbus FC/异常等辅助诊断信息。
 *
 * @param xSource       事件来源
 * @param lResult       来源特定的归一化结果码
 * @param lNativeError  原生错误码
 * @param lAuxValue     辅助诊断值
 */
void vAppCommLogWriteExtended(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError, int32_t lAuxValue);

/**
 * @brief 提交状态快照（带去重心跳）
 *
 * 首次出现或状态变化时立即输出（event=CHANGED）；
 * 状态未变化时每 5 秒输出一次心跳（event=HEARTBEAT）。
 * 每个日志源独立维护状态槽，互不干扰。
 *
 * 典型用法：Robot 健康状态、MilkTea 连接状态、运行状态。
 *
 * @param xSource       状态来源
 * @param lResult       当前状态值
 * @param lNativeError  当前原生错误码
 * @return 1 本次实际输出了日志，0 未输出（抑制）
 */
uint8_t ucAppCommLogWriteState(AppCommSource_e xSource, int32_t lResult,
	int32_t lNativeError);

/**
 * @brief 提交错误日志（带重复抑制）
 *
 * 同源同错误的首次出现立即输出（event=FIRST）；
 * 后续相同错误只累加计数，每 5 秒输出一次摘要（event=REPEAT_SUMMARY）。
 * 错误类型变化时先输出之前错误的摘要再重置。
 *
 * 典型用法：Robot Modbus 超时、MilkTea 帧超时、连接保护动作。
 *
 * @param xSource       错误来源
 * @param lResult       错误结果码
 * @param lNativeError  原生错误码
 */
void vAppCommLogWriteRateLimited(AppCommSource_e xSource,
	int32_t lResult, int32_t lNativeError);

/**
 * @brief 获取日志子系统整体状态快照（线程安全）
 *
 * 在临界区内完整复制 g_xAppCommLogStatus 到调用者提供的缓冲区。
 *
 * @param pxStatus [出] 接收状态快照的缓冲区，为 NULL 时静默返回
 */
void vAppCommLogGetStatus(AppCommLogStatus_t *pxStatus);

#ifdef __cplusplus
}
#endif

#endif /* APP_COMM_LOG_H */
