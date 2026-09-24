/**
  * @file      coffee3_log.c
  * @brief     基于公共 AppLog 内核实现 Coffee3 日志适配层。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   本适配层保留 Coffee3 接口与来源标签，创建 USART1 传输端点，
  *            并把 LwIP 资源故障交给公共 LwipAlert 服务处理。
  */

#include "coffee3_log.h"

#include <stdarg.h>
#include <stdio.h>

#include <string.h>

#include "LwipAlert/app_lwip_alert.h"
#include "coffee3_app_config.h"
#include "transport_uart.h"
#include "usart.h"

/** @brief 保存 Coffee3 日志来源与输出前缀的固定映射。 */
static const AppLogSourceDescriptor_t s_axCoffee3LogSources[
	COFFEE3_LOG_SOURCE_COUNT] = {
	{ "C3Main", "System" },
	{ "C3Server", "MBTcpServer" },
	{ "C3Workflow", "Workflow" },
	{ "C3Robot", "MBTcpClient" },
	{ "C3Bus2", "MBRtu" },
	{ "C3Bus3", "MBRtu" },
	{ "C3Bus4", "MBRtu" },
	{ "C3Bus5", "MBRtu" },
	{ "C3Workflow", "IO" },
	{ "C3Bus2", "Coffee" },
	{ "C3Bus3", "Cup" },
	{ "C3Bus3", "Syrup" },
	{ "C3Bus3", "Lid" },
	{ "C3Bus4", "Ice" },
	{ "C3Bus4", "Weigh" },
	{ "C3Bus3", "EnergyMeter" },
	{ "C3Bus5", "IoInput" },
	{ "C3Bus5", "IoOutput" }
};

/** @brief 绑定 Coffee3 USART1 后端的通用传输通道。 */
static TransportChannel_t s_xCoffee3LogChannel;
/** @brief 位于普通 SRAM 中、供 DMA 使用的 USART1 传输上下文。 */
static TransportUartContext_t s_xCoffee3LogTransport;

/*-----------------------------------------------------------*/
/**
  * @brief  初始化带 USART1 输出的 Coffee3 日志服务。
  * @retval COFFEE3_LOG_RESULT_OK 日志环形缓冲区与传输端点可用。
  * @retval COFFEE3_LOG_RESULT_ALREADY_INITIALIZED 日志服务已经初始化。
  * @retval 其他值 公共日志内核或 USART1 传输初始化失败。
  */
Coffee3LogResult_e xCoffee3LogInit(void)
{
	return xCoffee3LogInitWithTransport(1U);
}

/*-----------------------------------------------------------*/
/**
  * @brief  初始化日志环形缓冲区，并按需创建 USART1 传输端点。
  * @param[in] ucEnableTransport 非零时尝试创建 USART1 传输端点。
  * @retval COFFEE3_LOG_RESULT_OK 请求的日志能力初始化成功。
  * @retval COFFEE3_LOG_RESULT_ALREADY_INITIALIZED 日志服务已经初始化。
  * @retval 其他值 公共日志内核返回的初始化结果。
  * @note 串口创建失败时仍会初始化日志环形缓冲区，供启动诊断使用。
  */
Coffee3LogResult_e xCoffee3LogInitWithTransport(uint8_t ucEnableTransport)
{
	AppLogConfig_t xConfig; /*!< 传递给公共日志内核的初始化配置。 */
	TransportUartConfig_t xUartConfig; /*!< USART1 传输端点配置。 */
	AppLogResult_e xLogResult; /*!< 公共日志内核的初始化结果。 */
	TransportResult_e xTransportResult; /*!< USART1 传输端点的创建结果。 */

	/* 适配层保持幂等，避免重复创建静态资源。 */
	if (g_xAppLogStatus.ucInitialized != 0U) {
		return COFFEE3_LOG_RESULT_ALREADY_INITIALIZED;
	}
	/* 防御性清零，使未显式赋值的字段保持零值语义。 */
	memset(&xConfig, 0, sizeof(xConfig));			
	memset(&xUartConfig, 0, sizeof(xUartConfig));	
	xTransportResult = TRANSPORT_RESULT_NOT_READY;
	/* 绑定 Coffee3 固定日志来源表。 */
	xConfig.pxSourceTable = s_axCoffee3LogSources; 	
	xConfig.ucSourceCount = COFFEE3_LOG_SOURCE_COUNT; 
	/* 仅在调用方确认串口可用时创建发送传输端点。 */
	if (ucEnableTransport != 0U) {
		if (huart1.Instance == NULL) { /* 串口未初始化时记录端点未打开。 */
			xTransportResult = TRANSPORT_RESULT_NOT_OPEN;
		} else {
			xUartConfig.pxUart = &huart1;
			xUartConfig.ucReceiveEnabled = 0U; /* 日志端点只发送，不启用接收中断。 */
			xTransportResult = xTransportUartCreate(
				&s_xCoffee3LogChannel, &s_xCoffee3LogTransport,
				"coffee3_log_uart", &xUartConfig);
			if (xTransportResult == TRANSPORT_RESULT_OK) {
				xConfig.pxTransportChannel = &s_xCoffee3LogChannel;
				xConfig.ucEnableTransport = 1U;
			}
		}
	}
	/* 即使串口失败，环形缓冲区仍可能有效；保留底层传输错误供启动诊断。 */
	xLogResult = xAppLogInit(&xConfig);
	if ((xLogResult == APP_LOG_RESULT_TRANSPORT) &&
		(xConfig.pxTransportChannel == NULL)) {
		g_xAppLogStatus.lLastTransportError =
			(int32_t)xTransportResult;
	}
	return (Coffee3LogResult_e)xLogResult; /* 枚举值与公共日志结果保持一致。 */
}

/*-----------------------------------------------------------*/
/**
  * @brief  以系统订单号提交一条带结果码的日志。
  * @param[in] xLevel 日志级别。
  * @param[in] xSource 日志来源模块。
  * @param[in] pcText 复制到日志记录中的文本。
  * @param[in] lCode 来源模块定义的结果或诊断码。
  * @retval COFFEE3_LOG_RESULT_OK 日志记录已写入环形缓冲区。
  * @retval 其他值 参数无效或日志服务尚未就绪。
  */
Coffee3LogResult_e xCoffee3LogWrite(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, const char *pcText, int32_t lCode)
{
	return xCoffee3LogWriteOrder(xLevel, xSource,
		COFFEE3_LOG_ORDER_SYSTEM, pcText, lCode);
}

/*-----------------------------------------------------------*/
/**
  * @brief  以指定订单号提交一条带结果码的日志。
  * @param[in] xLevel 日志级别。
  * @param[in] xSource 日志来源模块。
  * @param[in] usOrderId 主机、调试或系统订单号。
  * @param[in] pcText 复制到日志记录中的文本。
  * @param[in] lCode 来源模块定义的结果或诊断码。
  * @retval COFFEE3_LOG_RESULT_OK 日志记录已写入环形缓冲区。
  * @retval 其他值 参数无效或日志服务尚未就绪。
  */
Coffee3LogResult_e xCoffee3LogWriteOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lCode)
{
	return (Coffee3LogResult_e)xAppLogWriteOrder(
		(AppLogLevel_e)xLevel, (AppLogSourceId_t)xSource,
		(AppLogOrderId_t)usOrderId, pcText, lCode);
}

/*-----------------------------------------------------------*/
/**
  * @brief  以系统订单号提交一条带命名字段的日志。
  * @param[in] xLevel 日志级别。
  * @param[in] xSource 日志来源模块。
  * @param[in] pcText 复制到日志记录中的文本。
  * @param[in] lResult 操作结果或状态结果。
  * @param[in] pcFieldName 字段名称；无需字段时可为空。
  * @param[in] lFieldValue 字段名称有效时对应的数值。
  * @retval COFFEE3_LOG_RESULT_OK 日志记录已写入环形缓冲区。
  * @retval 其他值 参数无效或日志服务尚未就绪。
  */
Coffee3LogResult_e xCoffee3LogWriteField(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue)
{
	return xCoffee3LogWriteFieldOrder(xLevel, xSource,
		COFFEE3_LOG_ORDER_SYSTEM, pcText, lResult, pcFieldName,
		lFieldValue);
}

/*-----------------------------------------------------------*/
/**
  * @brief  以指定订单号提交一条带命名字段的日志。
  * @param[in] xLevel 日志级别。
  * @param[in] xSource 日志来源模块。
  * @param[in] usOrderId 主机、调试或系统订单号。
  * @param[in] pcText 复制到日志记录中的文本。
  * @param[in] lResult 操作结果或状态结果。
  * @param[in] pcFieldName 字段名称；无需字段时可为空。
  * @param[in] lFieldValue 字段名称有效时对应的数值。
  * @retval COFFEE3_LOG_RESULT_OK 日志记录已写入环形缓冲区。
  * @retval 其他值 参数无效或日志服务尚未就绪。
  */
Coffee3LogResult_e xCoffee3LogWriteFieldOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lResult, const char *pcFieldName, int32_t lFieldValue)
{
	return (Coffee3LogResult_e)xAppLogWriteFieldOrder(
		(AppLogLevel_e)xLevel, (AppLogSourceId_t)xSource,
		(AppLogOrderId_t)usOrderId, pcText, lResult, pcFieldName,
		lFieldValue);
}

/*-----------------------------------------------------------*/
/**
  * @brief  以指定订单号提交一条纯文本日志。
  * @param[in] xLevel 日志级别。
  * @param[in] xSource 日志来源模块。
  * @param[in] usOrderId 主机、调试或系统订单号。
  * @param[in] pcText 复制到日志记录中的文本。
  * @retval COFFEE3_LOG_RESULT_OK 日志记录已写入环形缓冲区。
  * @retval 其他值 参数无效或日志服务尚未就绪。
  */
Coffee3LogResult_e xCoffee3LogWriteTextOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcText)
{
	return (Coffee3LogResult_e)xAppLogWriteTextOrder(
		(AppLogLevel_e)xLevel, (AppLogSourceId_t)xSource,
		(AppLogOrderId_t)usOrderId, pcText);
}

/*-----------------------------------------------------------*/
/**
  * @brief  格式化并提交一条长度受限的纯文本日志。
  * @param[in] xLevel 日志级别。
  * @param[in] xSource 日志来源模块。
  * @param[in] usOrderId 主机、调试或系统订单号。
  * @param[in] pcFormat 格式字符串；不可为空。
  * @retval COFFEE3_LOG_RESULT_OK 日志记录已写入环形缓冲区。
  * @retval COFFEE3_LOG_RESULT_INVALID_ARG 格式字符串无效或格式化失败。
  * @retval 其他值 公共日志服务未就绪。
  * @note 格式化结果超过 95 字节时会截断并补充字符串结束符。
  */
Coffee3LogResult_e xCoffee3LogPrintfOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcFormat, ...)
{
	char acText[96];       /*!< 格式化日志使用的有界栈缓冲区。 */
	va_list xArguments;    /*!< 可变参数遍历状态。 */
	int lLength;           /*!< 格式化结果长度，负数表示失败。 */

	if (pcFormat == NULL) {
		return COFFEE3_LOG_RESULT_INVALID_ARG;
	}
	/* 从格式字符串后的第一个可变参数开始读取。 */
	va_start(xArguments, pcFormat);
	lLength = vsnprintf(acText, sizeof(acText), pcFormat, xArguments);
	va_end(xArguments);
	if (lLength < 0) {
		return COFFEE3_LOG_RESULT_INVALID_ARG;
	}
	acText[sizeof(acText) - 1U] = '\0';
	return xCoffee3LogWriteTextOrder(xLevel, xSource, usOrderId, acText);
}

/*-----------------------------------------------------------*/
/**
  * @brief  在调度器启动前通过已初始化的日志通道写入探测数据。
  * @param[in] pucData 待发送字节；不可为空。
  * @param[in] usLength 待发送长度，单位为字节。
  * @retval 0 发送完成。
  * @retval 负数 底层传输尚未就绪或发送失败。
  */
int32_t lCoffee3LogEarlyWrite(const uint8_t *pucData, uint16_t usLength)
{
	return lAppLogEarlyWrite(pucData, usLength);
}

/*-----------------------------------------------------------*/
/**
  * @brief  运行唯一拥有 USART1 输出权的日志任务。
  * @param[in] pvArgument 透传给公共日志任务的参数。
  * @note 本函数进入公共日志任务后不会返回。
  */
void vCoffee3LogTask(void *pvArgument)
{
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "TASK_RUNNING:C3Log", 0);
	vAppLogTask(pvArgument);
}

/*-----------------------------------------------------------*/
/**
  * @brief  发布 Coffee3 日志任务的创建结果。
  * @param[in] ucCreated 非零表示任务创建成功。
  */
void vCoffee3LogSetTaskReady(uint8_t ucCreated)
{
	vAppLogSetTaskReady(ucCreated);
}

/*-----------------------------------------------------------*/
/**
  * @brief  复制当前日志服务状态。
  * @param[out] pxStatus 调用方提供的状态缓冲区；为空时忽略。
  */
void vCoffee3LogGetStatus(Coffee3LogStatus_t *pxStatus)
{
	vAppLogGetStatus(pxStatus);
}

/*-----------------------------------------------------------*/
/**
  * @brief  在网络接口失败后记录 LwIP 资源状态。
  * @param[in] xSource 观察到失败的 Coffee3 模块。
  * @param[in] lNativeError 套接字或 LwIP 原生错误码。
  * @note 本函数只报告故障，不改变调用方原有的失败结果。
  */
void vCoffee3LogLwipResourceFailure(Coffee3LogSource_e xSource,
	int32_t lNativeError)
{
	vAppLwipAlertReportFailure((AppLogSourceId_t)xSource, lNativeError);
}
