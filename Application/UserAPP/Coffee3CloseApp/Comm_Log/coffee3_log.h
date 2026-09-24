/**
  * @file      coffee3_log.h
  * @brief     声明 Coffee3 异步 USART1 日志服务。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   生产者把定长结构化记录写入静态覆盖式环形缓冲区，唯一日志任务
  *            通过 Transport 独占 USART1 输出。
  *
  * @attention
  * - 写入接口只能在任务上下文调用。
  * - 本模块不提供调试命令或串口接收接口。
  */

#ifndef COFFEE3_LOG_H
#define COFFEE3_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "Log/app_log.h"

/** @brief 定义 Coffee3 日志服务结果。 */
typedef enum {
	COFFEE3_LOG_RESULT_OK = 0, /*!< 操作完成。 */
	COFFEE3_LOG_RESULT_ALREADY_INITIALIZED = 1, /*!< 日志服务已经初始化。 */
	COFFEE3_LOG_RESULT_INVALID_ARG = -1, /*!< 指针、级别或来源无效。 */
	COFFEE3_LOG_RESULT_NOT_READY = -2, /*!< 环形缓冲区或传输端点未就绪。 */
	COFFEE3_LOG_RESULT_QUEUE_FULL = -3, /*!< 兼容保留的队列已满结果。 */
	COFFEE3_LOG_RESULT_TRANSPORT = -4 /*!< USART1 传输创建或打开失败。 */
} Coffee3LogResult_e;

/** @brief 定义不含调试级别的日志严重程度。 */
typedef enum {
	COFFEE3_LOG_LEVEL_INFO = 0, /*!< 普通运行信息。 */
	COFFEE3_LOG_LEVEL_WARNING = 1, /*!< 可恢复或需要关注的异常。 */
	COFFEE3_LOG_LEVEL_ERROR = 2, /*!< 已导致操作失败的错误。 */
	COFFEE3_LOG_LEVEL_COUNT = 3 /*!< 日志级别数量边界。 */
} Coffee3LogLevel_e;

/** @brief 标识系统日志与人工调试日志使用的订单号命名空间。 */
#define COFFEE3_LOG_ORDER_SYSTEM       0x0000U
#define COFFEE3_LOG_ORDER_DEBUG        0xF123U

/** @brief 标识提交 Coffee3 日志记录的子系统。 */
typedef enum {
	COFFEE3_LOG_SOURCE_SYSTEM = 0, /*!< 系统启动与全局状态。 */
	COFFEE3_LOG_SOURCE_SERVER = 1, /*!< 主机侧 Modbus TCP 服务。 */
	COFFEE3_LOG_SOURCE_WORKFLOW = 2, /*!< 订单与维护工作流。 */
	COFFEE3_LOG_SOURCE_ROBOT = 3, /*!< 机器人 TCP 客户端。 */
	COFFEE3_LOG_SOURCE_BUS2 = 4, /*!< 第二路串行总线。 */
	COFFEE3_LOG_SOURCE_BUS3 = 5, /*!< 第三路串行总线。 */
	COFFEE3_LOG_SOURCE_BUS4 = 6, /*!< 第四路串行总线。 */
	COFFEE3_LOG_SOURCE_BUS5 = 7, /*!< 第五路串行总线。 */
	COFFEE3_LOG_SOURCE_IO = 8, /*!< 产品 IO 状态层。 */
	COFFEE3_LOG_SOURCE_COFFEE = 9, /*!< 咖啡机设备。 */
	COFFEE3_LOG_SOURCE_CUP = 10, /*!< 落杯机设备。 */
	COFFEE3_LOG_SOURCE_SYRUP = 11, /*!< 糖浆机设备。 */
	COFFEE3_LOG_SOURCE_LID = 12, /*!< 落盖机设备。 */
	COFFEE3_LOG_SOURCE_ICE = 13, /*!< 制冰机设备。 */
	COFFEE3_LOG_SOURCE_WEIGH = 14, /*!< 称重设备。 */
	COFFEE3_LOG_SOURCE_ENERGY_METER = 15, /*!< 电能表设备。 */
	COFFEE3_LOG_SOURCE_IO_INPUT = 16, /*!< 外部输入模块。 */
	COFFEE3_LOG_SOURCE_IO_OUTPUT = 17, /*!< 外部输出模块。 */
	COFFEE3_LOG_SOURCE_COUNT = 18 /*!< 日志来源数量边界。 */
} Coffee3LogSource_e;

/** @brief 复用公共日志状态布局，不创建第二份状态对象。 */
typedef AppLogStatus_t Coffee3LogStatus_t;

/** @brief 把既有 Coffee3 状态符号映射到公共日志状态。 */
#define g_xCoffee3LogStatus g_xAppLogStatus

Coffee3LogResult_e xCoffee3LogInit(void);
Coffee3LogResult_e xCoffee3LogInitWithTransport(uint8_t ucEnableTransport);
Coffee3LogResult_e xCoffee3LogWrite(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, const char *pcText, int32_t lCode);
Coffee3LogResult_e xCoffee3LogWriteOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lCode);
Coffee3LogResult_e xCoffee3LogWriteField(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue);
Coffee3LogResult_e xCoffee3LogWriteFieldOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lResult, const char *pcFieldName, int32_t lFieldValue);
Coffee3LogResult_e xCoffee3LogWriteTextOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcText);
Coffee3LogResult_e xCoffee3LogPrintfOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcFormat, ...);
int32_t lCoffee3LogEarlyWrite(const uint8_t *pucData, uint16_t usLength);
void vCoffee3LogTask(void *pvArgument);
void vCoffee3LogSetTaskReady(uint8_t ucCreated);
void vCoffee3LogGetStatus(Coffee3LogStatus_t *pxStatus);
void vCoffee3LogLwipResourceFailure(Coffee3LogSource_e xSource,
	int32_t lNativeError);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_LOG_H */
