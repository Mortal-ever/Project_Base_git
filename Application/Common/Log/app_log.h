/**
  * @file      app_log.h
  * @brief     定义与产品无关的异步应用日志核心接口。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   生产者向静态覆盖最旧项环形缓冲提交定长结构化记录，唯一消费
  *            任务通过调用方持有的 Transport 通道输出。
  *
  * @attention
  * - 写入接口仅供任务上下文调用，且不分配动态内存。
  * - 产品日志来源编号和 Transport 配置由产品适配层负责。
  */

#ifndef APP_LOG_H
#define APP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "transport.h"

/** @brief 表示应用日志服务操作结果。 */
typedef enum {
	APP_LOG_RESULT_OK = 0,
		/*!< 操作成功完成。 */
	APP_LOG_RESULT_ALREADY_INITIALIZED = 1,
		/*!< 日志服务此前已经初始化。 */
	APP_LOG_RESULT_INVALID_ARG = -1,
		/*!< 指针、级别或来源编号不合法。 */
	APP_LOG_RESULT_NOT_READY = -2,
		/*!< 环形缓冲或通知信号尚未就绪。 */
	APP_LOG_RESULT_QUEUE_FULL = -3,
		/*!< 为仍公开旧结果约定的适配层保留。 */
	APP_LOG_RESULT_TRANSPORT = -4
		/*!< Transport 输出通道不可用。 */
} AppLogResult_e;

/** @brief 表示不包含调试级别的日志严重程度。 */
typedef enum {
	APP_LOG_LEVEL_INFO = 0, /*!< 正常运行信息。 */
	APP_LOG_LEVEL_WARNING = 1, /*!< 可继续运行但需要关注的异常。 */
	APP_LOG_LEVEL_ERROR = 2, /*!< 操作失败或状态错误。 */
	APP_LOG_LEVEL_COUNT = 3 /*!< 有效日志级别数量。 */
} AppLogLevel_e;

/** @brief 表示由产品定义的日志来源编号。 */
typedef uint8_t AppLogSourceId_t;

/** @brief 表示与日志记录关联的主机订单编号。 */
typedef uint16_t AppLogOrderId_t;

/** @brief 保存一个日志来源的稳定文本标签。 */
typedef struct {
	const char *pcTaskName; /*!< 写入日志前缀的任务标签。 */
	const char *pcModuleName; /*!< 写入日志前缀的模块标签。 */
} AppLogSourceDescriptor_t;

/** @brief 绑定产品来源标签和一个可选的输出 Transport 通道。 */
typedef struct {
	const AppLogSourceDescriptor_t *pxSourceTable;
		/*!< 产品持有且保存在只读存储器中的来源表。 */
	uint8_t ucSourceCount;
		/*!< 来源表中的有效项数。 */
	TransportChannel_t *pxTransportChannel;
		/*!< 产品持有的通道；仅缓冲模式时为空。 */
	uint8_t ucEnableTransport;
		/*!< 非零表示初始化时请求打开 Transport。 */
} AppLogConfig_t;

/** @brief 保存供产品监控的日志运行状态和累计计数。 */
typedef struct {
	uint32_t ulQueuedCount; /*!< 环形缓冲累计接收的记录数。 */
	uint32_t ulSentCount; /*!< 已成功发送并提交移除的记录数。 */
	uint32_t ulDroppedCount; /*!< 因缓冲已满而被覆盖的最旧记录数。 */
	uint32_t ulTransmitFailureCount; /*!< Transport 发送失败次数。 */
	int32_t lLastTransportError; /*!< 最近一次归一化 Transport 结果。 */
	uint32_t ulRetryCount; /*!< 发送失败后的累计重试次数。 */
	uint16_t usPendingCount; /*!< 当前等待输出的记录数。 */
	uint16_t usQueueHighWatermark;
		/*!< 已观察到的最大等待记录数。 */
	uint8_t ucInitialized; /*!< 非零表示环形缓冲和信号已经初始化。 */
	uint8_t ucBufferReady; /*!< 非零表示静态环形缓冲可用。 */
	uint8_t ucTransportReady; /*!< 非零表示 Transport 当前可发送。 */
	uint8_t ucTaskReady; /*!< 非零表示产品日志任务已经进入运行。 */
	uint8_t ucOutputPaused; /*!< 非零表示输出当前暂停或不可用。 */
} AppLogStatus_t;

/** @brief 唯一应用日志实例公开的运行状态。 */
extern AppLogStatus_t g_xAppLogStatus;

AppLogResult_e xAppLogInit(const AppLogConfig_t *pxConfig);

AppLogResult_e xAppLogWrite(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, const char *pcText, int32_t lCode);

AppLogResult_e xAppLogWriteOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText, int32_t lCode);

AppLogResult_e xAppLogWriteField(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue);

AppLogResult_e xAppLogWriteFieldOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText, int32_t lResult, const char *pcFieldName,
	int32_t lFieldValue);

AppLogResult_e xAppLogWriteTextOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText);

int32_t lAppLogEarlyWrite(const uint8_t *pucData, uint16_t usLength);

void vAppLogTask(void *pvArgument);

void vAppLogSetTaskReady(uint8_t ucCreated);

void vAppLogGetStatus(AppLogStatus_t *pxStatus);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H */
