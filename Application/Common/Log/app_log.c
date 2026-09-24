/**
  * @file      app_log.c
  * @brief     实现与产品无关的异步应用日志核心。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   本实现持有一个定长 CPU 环形缓冲和一个二值信号；产品代码
  *            持有输出 Transport 通道以及调用 vAppLogTask() 的任务。
  *
  * @attention
  * - 本模块不使用动态内存，也不包含产品私有头文件。
  * - 环形缓冲满时覆盖最旧记录，从而保留最新记录。
  */

#include "Log/app_log.h"

#include <string.h>

#include "compiler_compat.h"
#include "semphr.h"
#include "task.h"

/** @brief 覆盖最旧项环形缓冲可保留的记录数。 */
#define APP_LOG_RING_LENGTH             32U
/** @brief 事件文本最大容量，包含字符串结束符。 */
#define APP_LOG_TEXT_LENGTH             96U
/** @brief 命名字段最大容量，包含字符串结束符。 */
#define APP_LOG_FIELD_LENGTH            24U
/** @brief 格式化输出行最大容量，单位为字节。 */
#define APP_LOG_LINE_LENGTH             192U
/** @brief 单次 Transport 发送超时，单位为毫秒。 */
#define APP_LOG_SEND_TIMEOUT_MS         100U
/** @brief 发送失败后的初始重试间隔，单位为毫秒。 */
#define APP_LOG_RETRY_BASE_MS           100U
/** @brief 连续发送失败时的最大重试间隔，单位为毫秒。 */
#define APP_LOG_RETRY_MAX_MS            1000U

/** @brief 保存一条由环形缓冲持有的结构化日志记录。 */
typedef struct {
	uint32_t ulSequence; /*!< 用于窥视后条件提交的单调序号。 */
	AppLogLevel_e xLevel; /*!< 记录严重程度。 */
	AppLogSourceId_t xSource; /*!< 产品定义的记录来源。 */
	AppLogOrderId_t usOrderId; /*!< 关联订单编号，零表示无具体订单。 */
	int32_t lResult; /*!< 来源提供的操作结果或诊断码。 */
	int32_t lFieldValue; /*!< 可选命名诊断字段的数值。 */
	char acText[APP_LOG_TEXT_LENGTH]; /*!< 截断保存的事件文本。 */
	char acFieldName[APP_LOG_FIELD_LENGTH]; /*!< 截断保存的可选字段名。 */
	uint8_t ucHasResult; /*!< 非零表示格式化时输出结果字段。 */
	uint8_t ucHasField; /*!< 非零表示格式化时输出命名字段。 */
} AppLogEntry_t;

/** @brief 向产品适配层和调试器公开的唯一日志状态。 */
APP_CCM_DATA
AppLogStatus_t g_xAppLogStatus;

/** @brief 静态覆盖最旧项环形缓冲记录。 */
APP_CCM_DATA
static AppLogEntry_t s_axLogRing[APP_LOG_RING_LENGTH];
/** @brief 环形缓冲下一次写入索引。 */
APP_CCM_DATA
static uint16_t s_usLogHead;
/** @brief 环形缓冲当前最旧记录索引。 */
APP_CCM_DATA
static uint16_t s_usLogTail;
/** @brief 环形缓冲当前保留的有效记录数。 */
APP_CCM_DATA
static uint16_t s_usLogCount;
/** @brief 窥视和提交配对使用的单调记录序号。 */
APP_CCM_DATA
static uint32_t s_ulLogSequence;
/** @brief 合并多个生产者唤醒请求的静态二值信号句柄。 */
APP_CCM_DATA
static SemaphoreHandle_t s_xLogSignal;
/** @brief 二值信号使用的静态控制块存储。 */
APP_CCM_DATA
static StaticSemaphore_t s_xLogSignalStorage;
/** @brief 产品持有、日志消费者用于输出的 Transport 通道。 */
static TransportChannel_t *s_pxLogChannel;
/** @brief 产品持有且保存在只读存储器中的来源描述表。 */
static const AppLogSourceDescriptor_t *s_pxSourceTable;
/** @brief 产品来源描述表中的有效项数。 */
static uint8_t s_ucSourceCount;
/** @brief 非零表示已配置可供重试的 Transport 通道。 */
APP_CCM_DATA
static uint8_t s_ucTransportConfigured;
/** @brief 非零表示输出通道当前已经打开。 */
APP_CCM_DATA
static uint8_t s_ucTransportReady;
/** @brief 当前有上限的输出重试间隔，单位为毫秒。 */
APP_CCM_DATA
static uint32_t s_ulRetryDelayMs;

static uint16_t prvAppendText(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, const char *pcText);
static uint16_t prvAppendUnsigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint32_t ulValue);
static uint16_t prvAppendHex16(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint16_t usValue);
static uint16_t prvAppendSigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, int32_t lValue);
static uint16_t prvFormatEntry(const AppLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usCapacity);
static const char *prvGetLevelText(AppLogLevel_e xLevel);
static const char *prvGetTaskText(AppLogSourceId_t xSource);
static const char *prvGetModuleText(AppLogSourceId_t xSource);
static void prvCopyText(char *pcDestination, uint16_t usCapacity,
	const char *pcSource);
static void prvUpdateRingDepthLocked(void);
static BaseType_t prvPeekEntry(AppLogEntry_t *pxEntry, uint32_t *pulSequence);
static BaseType_t prvCommitEntry(uint32_t ulSequence);
static uint8_t prvIsTransportReady(void);
static uint8_t prvTryOpenTransport(void);
static void prvRecordTransportResult(TransportResult_e xResult,
	uint8_t ucCommitted);

/*-----------------------------------------------------------*/
/**
  * @brief  初始化静态环形缓冲、通知信号和可选 Transport 输出。
  * @param[in] pxConfig 产品来源表和输出通道绑定。
  * @retval APP_LOG_RESULT_OK 缓冲和请求的输出通道均已就绪。
  * @retval APP_LOG_RESULT_ALREADY_INITIALIZED 此前已经初始化。
  * @retval APP_LOG_RESULT_INVALID_ARG 来源表或来源数量不合法。
  * @retval APP_LOG_RESULT_NOT_READY 静态通知信号创建失败。
  * @retval APP_LOG_RESULT_TRANSPORT 仅缓冲可用，输出未配置或打开失败。
  */
AppLogResult_e xAppLogInit(const AppLogConfig_t *pxConfig)
{
	TransportResult_e xTransportResult; /*!< 初始化时打开输出通道的结果。 */

	if (pxConfig == NULL) {
		return APP_LOG_RESULT_INVALID_ARG;
	}
	if ((pxConfig->pxSourceTable == NULL) ||
		(pxConfig->ucSourceCount == 0U)) {
		return APP_LOG_RESULT_INVALID_ARG;
	}
	if (g_xAppLogStatus.ucInitialized != 0U) {
		return APP_LOG_RESULT_ALREADY_INITIALIZED;
	}

	memset(&g_xAppLogStatus, 0, sizeof(g_xAppLogStatus));
	memset(s_axLogRing, 0, sizeof(s_axLogRing));
	s_usLogHead = 0U;
	s_usLogTail = 0U;
	s_usLogCount = 0U;
	s_ulLogSequence = 0U;
	s_pxSourceTable = pxConfig->pxSourceTable;
	s_ucSourceCount = pxConfig->ucSourceCount;
	s_pxLogChannel = pxConfig->pxTransportChannel;
	s_ucTransportConfigured =
		((pxConfig->ucEnableTransport != 0U) &&
		 (s_pxLogChannel != NULL)) ? 1U : 0U;
	s_ucTransportReady = 0U;
	s_ulRetryDelayMs = APP_LOG_RETRY_BASE_MS;
	s_xLogSignal = xSemaphoreCreateBinaryStatic(&s_xLogSignalStorage);
	if (s_xLogSignal == NULL) {
		return APP_LOG_RESULT_NOT_READY;
	}
	g_xAppLogStatus.ucInitialized = 1U;
	g_xAppLogStatus.ucBufferReady = 1U;
	if (s_ucTransportConfigured == 0U) {
		g_xAppLogStatus.ucOutputPaused = 1U;
		g_xAppLogStatus.lLastTransportError =
			(int32_t)TRANSPORT_RESULT_NOT_READY;
		return APP_LOG_RESULT_TRANSPORT;
	}

	xTransportResult = xTransportOpen(s_pxLogChannel);
	if (xTransportResult != TRANSPORT_RESULT_OK) {
		g_xAppLogStatus.lLastTransportError = (int32_t)xTransportResult;
		g_xAppLogStatus.ucOutputPaused = 1U;
		return APP_LOG_RESULT_TRANSPORT;
	}

	s_ucTransportReady = 1U;
	g_xAppLogStatus.ucTransportReady = 1U;
	return APP_LOG_RESULT_OK;
}

/*-----------------------------------------------------------*/
/**
  * @brief  提交一条无订单编号的结构化日志记录。
  * @param[in] xLevel 记录严重程度。
  * @param[in] xSource 产品定义的来源编号。
  * @param[in] pcText 复制到记录中的事件文本。
  * @param[in] lCode 来源定义的结果或诊断码。
  * @retval AppLogResult_e 写入环形缓冲的结果。
  */
AppLogResult_e xAppLogWrite(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, const char *pcText, int32_t lCode)
{
	return xAppLogWriteOrder(xLevel, xSource, 0U, pcText, lCode);
}

/*-----------------------------------------------------------*/
/**
  * @brief  提交一条带订单编号但不带命名字段的结构化日志记录。
  * @param[in] xLevel 记录严重程度。
  * @param[in] xSource 产品定义的来源编号。
  * @param[in] usOrderId 关联订单编号，零可表示系统事件。
  * @param[in] pcText 复制到记录中的事件文本。
  * @param[in] lCode 来源定义的结果或诊断码。
  * @retval AppLogResult_e 写入环形缓冲的结果。
  */
AppLogResult_e xAppLogWriteOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText, int32_t lCode)
{
	return xAppLogWriteFieldOrder(xLevel, xSource, usOrderId, pcText,
		lCode, NULL, 0);
}

/*-----------------------------------------------------------*/
/**
  * @brief  提交一条无订单编号但带命名诊断字段的日志记录。
  * @param[in] xLevel 记录严重程度。
  * @param[in] xSource 产品定义的来源编号。
  * @param[in] pcText 复制到记录中的事件文本。
  * @param[in] lResult 归一化操作结果。
  * @param[in] pcFieldName 可选字段名，为空时不输出字段。
  * @param[in] lFieldValue 命名字段的数值。
  * @retval AppLogResult_e 写入环形缓冲的结果。
  */
AppLogResult_e xAppLogWriteField(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue)
{
	return xAppLogWriteFieldOrder(xLevel, xSource, 0U, pcText, lResult,
		pcFieldName, lFieldValue);
}

/*-----------------------------------------------------------*/
/**
  * @brief  校验并将一条完整结构化记录写入覆盖最旧项环形缓冲。
  * @param[in] xLevel 记录严重程度。
  * @param[in] xSource 产品定义的来源编号。
  * @param[in] usOrderId 关联订单编号。
  * @param[in] pcText 复制并按容量截断的事件文本。
  * @param[in] lResult 归一化操作结果。
  * @param[in] pcFieldName 可选字段名。
  * @param[in] lFieldValue 命名字段数值。
  * @retval APP_LOG_RESULT_OK 记录已入队，必要时覆盖最旧记录。
  * @retval APP_LOG_RESULT_INVALID_ARG 文本、级别或来源不合法。
  * @retval APP_LOG_RESULT_NOT_READY 日志缓冲尚未初始化。
  */
AppLogResult_e xAppLogWriteFieldOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText, int32_t lResult, const char *pcFieldName,
	int32_t lFieldValue)
{
	AppLogEntry_t xEntry; /*!< 在进入临界区前构造的完整日志记录。 */

	if ((pcText == NULL) ||
		((uint32_t)xLevel >= (uint32_t)APP_LOG_LEVEL_COUNT) ||
		((uint32_t)xSource >= (uint32_t)s_ucSourceCount)) {
		return APP_LOG_RESULT_INVALID_ARG;
	}
	if ((g_xAppLogStatus.ucInitialized == 0U) ||
		(g_xAppLogStatus.ucBufferReady == 0U)) {
		return APP_LOG_RESULT_NOT_READY;
	}

	memset(&xEntry, 0, sizeof(xEntry));
	xEntry.xLevel = xLevel;
	xEntry.xSource = xSource;
	xEntry.usOrderId = usOrderId;
	xEntry.lResult = lResult;
	xEntry.lFieldValue = lFieldValue;
	xEntry.ucHasResult = 1U;
	prvCopyText(xEntry.acText, APP_LOG_TEXT_LENGTH, pcText);
	if ((pcFieldName != NULL) && (pcFieldName[0] != '\0')) {
		prvCopyText(xEntry.acFieldName, APP_LOG_FIELD_LENGTH, pcFieldName);
		xEntry.ucHasField = 1U;
	}

	/* 临界区内分配序号、写入记录，并维护覆盖最旧项的环形索引。 */
	taskENTER_CRITICAL();
	s_ulLogSequence++;
	if (s_ulLogSequence == 0U) {
		s_ulLogSequence = 1U;
	}
	xEntry.ulSequence = s_ulLogSequence;
	s_axLogRing[s_usLogHead] = xEntry;
	s_usLogHead++;
	if (s_usLogHead >= APP_LOG_RING_LENGTH) {
		s_usLogHead = 0U;
	}
	if (s_usLogCount < APP_LOG_RING_LENGTH) {
		s_usLogCount++;
	} else {
		s_usLogTail++;
		if (s_usLogTail >= APP_LOG_RING_LENGTH) {
			s_usLogTail = 0U;
		}
		g_xAppLogStatus.ulDroppedCount++;
	}
	g_xAppLogStatus.ulQueuedCount++;
	prvUpdateRingDepthLocked();
	taskEXIT_CRITICAL();
	if (s_xLogSignal != NULL) {
		(void)xSemaphoreGive(s_xLogSignal);
	}
	return APP_LOG_RESULT_OK;
}

/*-----------------------------------------------------------*/
/**
  * @brief  提交一条不带结果后缀的可读文本记录。
  * @param[in] xLevel 记录严重程度。
  * @param[in] xSource 产品定义的来源编号。
  * @param[in] usOrderId 关联订单编号。
  * @param[in] pcText 复制并按容量截断的文本。
  * @retval APP_LOG_RESULT_OK 记录已入队。
  * @retval APP_LOG_RESULT_INVALID_ARG 参数不合法。
  * @retval APP_LOG_RESULT_NOT_READY 日志缓冲尚未初始化。
  */
AppLogResult_e xAppLogWriteTextOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText)
{
	AppLogEntry_t xEntry; /*!< 不含结果字段的待入队日志记录。 */

	if ((pcText == NULL) ||
		((uint32_t)xLevel >= (uint32_t)APP_LOG_LEVEL_COUNT) ||
		((uint32_t)xSource >= (uint32_t)s_ucSourceCount)) {
		return APP_LOG_RESULT_INVALID_ARG;
	}
	if ((g_xAppLogStatus.ucInitialized == 0U) ||
		(g_xAppLogStatus.ucBufferReady == 0U)) {
		return APP_LOG_RESULT_NOT_READY;
	}
	memset(&xEntry, 0, sizeof(xEntry));
	xEntry.xLevel = xLevel;
	xEntry.xSource = xSource;
	xEntry.usOrderId = usOrderId;
	prvCopyText(xEntry.acText, APP_LOG_TEXT_LENGTH, pcText);
	taskENTER_CRITICAL();
	s_ulLogSequence++;
	if (s_ulLogSequence == 0U) {
		s_ulLogSequence = 1U;
	}
	xEntry.ulSequence = s_ulLogSequence;
	s_axLogRing[s_usLogHead] = xEntry;
	s_usLogHead++;
	if (s_usLogHead >= APP_LOG_RING_LENGTH) {
		s_usLogHead = 0U;
	}
	if (s_usLogCount < APP_LOG_RING_LENGTH) {
		s_usLogCount++;
	} else {
		s_usLogTail++;
		if (s_usLogTail >= APP_LOG_RING_LENGTH) {
			s_usLogTail = 0U;
		}
		g_xAppLogStatus.ulDroppedCount++;
	}
	g_xAppLogStatus.ulQueuedCount++;
	prvUpdateRingDepthLocked();
	taskEXIT_CRITICAL();
	if (s_xLogSignal != NULL) {
		(void)xSemaphoreGive(s_xLogSignal);
	}
	return APP_LOG_RESULT_OK;
}

/*-----------------------------------------------------------*/
/**
  * @brief  初始化后直接通过当前输出通道发送一段启动诊断数据。
  * @param[in] pucData 待发送字节。
  * @param[in] usLength 待发送字节数。
  * @retval 0 发送成功。
  * @retval 负数 参数、输出状态或 Transport 操作失败。
  */
int32_t lAppLogEarlyWrite(const uint8_t *pucData, uint16_t usLength)
{
	TransportResult_e xResult; /*!< 启动诊断数据的 Transport 发送结果。 */

	if ((pucData == NULL) || (usLength == 0U) ||
		(g_xAppLogStatus.ucInitialized == 0U)) {
		return (int32_t)TRANSPORT_RESULT_INVALID_ARG;
	}
	if (prvIsTransportReady() == 0U) {
		return g_xAppLogStatus.lLastTransportError;
	}
	xResult = xTransportSend(s_pxLogChannel, pucData, usLength,
		APP_LOG_SEND_TIMEOUT_MS);
	return (int32_t)xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  运行唯一日志消费任务，持续发送最旧记录并处理通道重试。
  * @param[in] pvArgument 任务参数，本实现不使用。
  * @note 产品适配层负责创建并命名本任务；函数正常情况下不返回。
  */
void vAppLogTask(void *pvArgument)
{
	AppLogEntry_t xEntry; /*!< 当前窥视但尚未提交移除的最旧记录。 */
	TransportResult_e xTransportResult; /*!< 当前格式化行的发送结果。 */
	uint8_t aucLine[APP_LOG_LINE_LENGTH]; /*!< 单条格式化输出行缓存。 */
	uint16_t usLength; /*!< 格式化输出行的有效字节数。 */
	uint32_t ulSequence; /*!< 窥视记录的条件提交序号。 */
	uint8_t ucTransportReady; /*!< 本轮发送前的通道就绪快照。 */
	BaseType_t xCommitted; /*!< 发送后是否成功移除同一条记录。 */

	(void)pvArgument;
	taskENTER_CRITICAL();
	g_xAppLogStatus.ucTaskReady = 1U;
	taskEXIT_CRITICAL();
	for (;;) {
		/* 步骤 1：窥视最旧记录；为空时阻塞等待生产者信号。 */
		if (prvPeekEntry(&xEntry, &ulSequence) != pdPASS) {
			if (s_xLogSignal != NULL) {
				(void)xSemaphoreTake(s_xLogSignal, portMAX_DELAY);
			}
			continue;
		}
		/* 步骤 2：确保输出通道已打开，否则按当前退避时长等待。 */
		ucTransportReady = prvIsTransportReady();
		if (ucTransportReady == 0U) {
			(void)prvTryOpenTransport();
			ucTransportReady = prvIsTransportReady();
		}
		if (ucTransportReady == 0U) {
			taskENTER_CRITICAL();
			g_xAppLogStatus.ucOutputPaused = 1U;
			taskEXIT_CRITICAL();
			vTaskDelay(pdMS_TO_TICKS(s_ulRetryDelayMs));
			continue;
		}
		/* 步骤 3：格式化并发送，只有发送成功才条件移除原记录。 */
		usLength = prvFormatEntry(&xEntry, aucLine, APP_LOG_LINE_LENGTH);
		xTransportResult = xTransportSend(s_pxLogChannel, aucLine,
			usLength, APP_LOG_SEND_TIMEOUT_MS);
		if (xTransportResult == TRANSPORT_RESULT_OK) {
			xCommitted = prvCommitEntry(ulSequence);
			prvRecordTransportResult(xTransportResult,
				(xCommitted == pdPASS) ? 1U : 0U);
		} else {
			prvRecordTransportResult(xTransportResult, 0U);
			vTaskDelay(pdMS_TO_TICKS(s_ulRetryDelayMs));
		}
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  发布产品日志任务的创建结果。
  * @param[in] ucCreated 非零表示任务创建成功。
  */
void vAppLogSetTaskReady(uint8_t ucCreated)
{
	taskENTER_CRITICAL();
	g_xAppLogStatus.ucTaskReady = (ucCreated != 0U) ? 1U : 0U;
	if (ucCreated == 0U) {
		g_xAppLogStatus.ucOutputPaused = 1U;
	}
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
/**
  * @brief  在临界区内复制一致的应用日志状态快照。
  * @param[out] pxStatus 调用方状态接收位置；为空时忽略请求。
  */
void vAppLogGetStatus(AppLogStatus_t *pxStatus)
{
	if (pxStatus == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	*pxStatus = g_xAppLogStatus;
	taskEXIT_CRITICAL();
}

/*-----------------------------------------------------------*/
/**
  * @brief  将一条结构化记录格式化为稳定的日志文本行。
  * @param[in] pxEntry 待格式化记录。
  * @param[out] pucLine 输出行缓存。
  * @param[in] usCapacity 输出缓存容量。
  * @retval uint16_t 实际格式化长度，包含末尾回车换行。
  */
static uint16_t prvFormatEntry(const AppLogEntry_t *pxEntry,
	uint8_t *pucLine, uint16_t usCapacity)
{
	uint16_t usLength; /*!< 当前输出行已写入字节数。 */

	if ((pxEntry == NULL) || (pucLine == NULL) || (usCapacity == 0U)) {
		return 0U;
	}
	usLength = 0U;
	usLength = prvAppendText(pucLine, usLength, usCapacity, "[");
	usLength = prvAppendHex16(pucLine, usLength, usCapacity,
		pxEntry->usOrderId);
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetLevelText(pxEntry->xLevel));
	usLength = prvAppendText(pucLine, usLength, usCapacity, "][");
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetTaskText(pxEntry->xSource));
	usLength = prvAppendText(pucLine, usLength, usCapacity, ":");
	usLength = prvAppendText(pucLine, usLength, usCapacity,
		prvGetModuleText(pxEntry->xSource));
	usLength = prvAppendText(pucLine, usLength, usCapacity, "] ");
	usLength = prvAppendText(pucLine, usLength, usCapacity, pxEntry->acText);
	if (pxEntry->ucHasResult != 0U) {
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			" result=");
		usLength = prvAppendSigned(pucLine, usLength, usCapacity,
			pxEntry->lResult);
	}
	if ((pxEntry->ucHasResult != 0U) &&
		(pxEntry->ucHasField != 0U)) {
		usLength = prvAppendText(pucLine, usLength, usCapacity, " ");
		usLength = prvAppendText(pucLine, usLength, usCapacity,
			pxEntry->acFieldName);
		usLength = prvAppendText(pucLine, usLength, usCapacity, "=");
		usLength = prvAppendSigned(pucLine, usLength, usCapacity,
			pxEntry->lFieldValue);
	}
	usLength = prvAppendText(pucLine, usLength, usCapacity, "\r\n");
	return usLength;
}

/*-----------------------------------------------------------*/
/**
  * @brief  在容量范围内向输出行追加字符串。
  * @param[in,out] pucLine 输出行缓存。
  * @param[in] usLength 当前长度。
  * @param[in] usCapacity 总容量。
  * @param[in] pcText 待追加字符串，允许为空。
  * @retval uint16_t 追加后的长度，不超过总容量。
  */
static uint16_t prvAppendText(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, const char *pcText)
{
	if (pcText == NULL) {
		return usLength;
	}
	while ((*pcText != '\0') && (usLength < usCapacity)) {
		pucLine[usLength] = (uint8_t)*pcText;
		usLength++;
		pcText++;
	}
	return usLength;
}

/*-----------------------------------------------------------*/
/**
  * @brief  向输出行追加无符号十进制数。
  * @param[in,out] pucLine 输出行缓存。
  * @param[in] usLength 当前长度。
  * @param[in] usCapacity 总容量。
  * @param[in] ulValue 待格式化数值。
  * @retval uint16_t 追加后的长度。
  */
static uint16_t prvAppendUnsigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint32_t ulValue)
{
	char acDigits[10]; /*!< 按低位到高位暂存的十进制数字。 */
	uint8_t ucCount; /*!< 已暂存且尚未反向输出的数字个数。 */

	ucCount = 0U;
	do {
		acDigits[ucCount] = (char)('0' + (ulValue % 10U));
		ucCount++;
		ulValue /= 10U;
	} while ((ulValue != 0U) && (ucCount < (uint8_t)sizeof(acDigits)));

	while ((ucCount > 0U) && (usLength < usCapacity)) {
		ucCount--;
		pucLine[usLength] = (uint8_t)acDigits[ucCount];
		usLength++;
	}
	return usLength;
}

/*-----------------------------------------------------------*/
/**
  * @brief  向输出行追加四位大写十六进制数。
  * @param[in,out] pucLine 输出行缓存。
  * @param[in] usLength 当前长度。
  * @param[in] usCapacity 总容量。
  * @param[in] usValue 待格式化的 16 位数值。
  * @retval uint16_t 追加后的长度。
  */
static uint16_t prvAppendHex16(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, uint16_t usValue)
{
	static const char acHex[] = "0123456789ABCDEF"; /*!< 十六进制字符表。 */
	uint8_t ucIndex; /*!< 四个半字节的输出索引。 */

	for (ucIndex = 0U; ucIndex < 4U; ucIndex++) {
		if (usLength >= usCapacity) {
			break;
		}
		pucLine[usLength] = (uint8_t)acHex[
			(usValue >> (uint8_t)(12U - (ucIndex * 4U))) & 0x0FU];
		usLength++;
	}
	return usLength;
}

/*-----------------------------------------------------------*/
/**
  * @brief  向输出行追加有符号十进制数。
  * @param[in,out] pucLine 输出行缓存。
  * @param[in] usLength 当前长度。
  * @param[in] usCapacity 总容量。
  * @param[in] lValue 待格式化数值。
  * @retval uint16_t 追加后的长度。
  */
static uint16_t prvAppendSigned(uint8_t *pucLine, uint16_t usLength,
	uint16_t usCapacity, int32_t lValue)
{
	uint32_t ulMagnitude; /*!< 规避最小负数溢出的无符号绝对值。 */

	if (lValue < 0) {
		usLength = prvAppendText(pucLine, usLength, usCapacity, "-");
		ulMagnitude = (uint32_t)(-(lValue + 1));
		ulMagnitude++;
	} else {
		ulMagnitude = (uint32_t)lValue;
	}
	return prvAppendUnsigned(pucLine, usLength, usCapacity, ulMagnitude);
}

/*-----------------------------------------------------------*/
/**
  * @brief  获取日志级别对应的稳定文本。
  * @param[in] xLevel 日志级别。
  * @retval const char* 静态级别文本；无效值返回 INVALID。
  */
static const char *prvGetLevelText(AppLogLevel_e xLevel)
{
	static const char * const apcLevels[APP_LOG_LEVEL_COUNT] = {
		"INFO", "WARN", "ERROR"
	}; /*!< 与 AppLogLevel_e 数值顺序一致的文本表。 */

	if ((uint32_t)xLevel >= (uint32_t)APP_LOG_LEVEL_COUNT) {
		return "INVALID";
	}
	return apcLevels[xLevel];
}

/*-----------------------------------------------------------*/
/**
  * @brief  获取产品日志来源对应的任务标签。
  * @param[in] xSource 产品来源编号。
  * @retval const char* 静态任务标签；无效来源返回 INVALID。
  */
static const char *prvGetTaskText(AppLogSourceId_t xSource)
{
	if ((s_pxSourceTable == NULL) ||
		((uint32_t)xSource >= (uint32_t)s_ucSourceCount) ||
		(s_pxSourceTable[xSource].pcTaskName == NULL)) {
		return "INVALID";
	}
	return s_pxSourceTable[xSource].pcTaskName;
}

/*-----------------------------------------------------------*/
/**
  * @brief  获取产品日志来源对应的模块标签。
  * @param[in] xSource 产品来源编号。
  * @retval const char* 静态模块标签；无效来源返回 INVALID。
  */
static const char *prvGetModuleText(AppLogSourceId_t xSource)
{
	if ((s_pxSourceTable == NULL) ||
		((uint32_t)xSource >= (uint32_t)s_ucSourceCount) ||
		(s_pxSourceTable[xSource].pcModuleName == NULL)) {
		return "INVALID";
	}
	return s_pxSourceTable[xSource].pcModuleName;
}

/*-----------------------------------------------------------*/
/**
  * @brief  将字符串安全复制到定长记录字段并保证结束符。
  * @param[out] pcDestination 目标字段。
  * @param[in] usCapacity 包含结束符的目标容量。
  * @param[in] pcSource 来源字符串；为空时写入空字符串。
  */
static void prvCopyText(char *pcDestination, uint16_t usCapacity,
	const char *pcSource)
{
	uint16_t usIndex; /*!< 当前复制的字符索引。 */

	if ((pcDestination == NULL) || (usCapacity == 0U)) {
		return;
	}
	if (pcSource == NULL) {
		pcDestination[0] = '\0';
		return;
	}
	usIndex = 0U;
	while ((pcSource[usIndex] != '\0') &&
		(usIndex < (uint16_t)(usCapacity - 1U))) {
		pcDestination[usIndex] = pcSource[usIndex];
		usIndex++;
	}
	pcDestination[usIndex] = '\0';
}

/*-----------------------------------------------------------*/
/** @brief 在临界区内同步等待数并更新环形缓冲历史高水位。 */
static void prvUpdateRingDepthLocked(void)
{
	g_xAppLogStatus.usPendingCount = s_usLogCount;
	if (s_usLogCount > g_xAppLogStatus.usQueueHighWatermark) {
		g_xAppLogStatus.usQueueHighWatermark = s_usLogCount;
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  在短临界区内复制最旧记录及其条件提交序号。
  * @param[out] pxEntry 最旧记录接收位置。
  * @param[out] pulSequence 对应记录序号接收位置。
  * @retval pdPASS 成功窥视一条记录。
  * @retval pdFAIL 参数无效或环形缓冲为空。
  */
static BaseType_t prvPeekEntry(AppLogEntry_t *pxEntry,
	uint32_t *pulSequence)
{
	BaseType_t xResult; /*!< 本次窥视结果。 */

	if ((pxEntry == NULL) || (pulSequence == NULL)) {
		return pdFAIL;
	}
	taskENTER_CRITICAL();
	if (s_usLogCount == 0U) {
		xResult = pdFAIL;
	} else {
		*pxEntry = s_axLogRing[s_usLogTail];
		*pulSequence = pxEntry->ulSequence;
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  仅在最旧记录序号仍匹配时提交移除该记录。
  * @param[in] ulSequence 此前窥视得到的记录序号。
  * @retval pdPASS 已移除同一条最旧记录。
  * @retval pdFAIL 缓冲为空或最旧记录已被覆盖替换。
  */
static BaseType_t prvCommitEntry(uint32_t ulSequence)
{
	BaseType_t xResult; /*!< 条件提交结果。 */

	taskENTER_CRITICAL();
	if ((s_usLogCount == 0U) ||
		(s_axLogRing[s_usLogTail].ulSequence != ulSequence)) {
		xResult = pdFAIL;
	} else {
		s_usLogTail++;
		if (s_usLogTail >= APP_LOG_RING_LENGTH) {
			s_usLogTail = 0U;
		}
		s_usLogCount--;
		prvUpdateRingDepthLocked();
		xResult = pdPASS;
	}
	taskEXIT_CRITICAL();
	return xResult;
}

/*-----------------------------------------------------------*/
/**
  * @brief  原子读取当前 Transport 输出就绪标志。
  * @retval 0 输出通道当前不可用。
  * @retval 非零 输出通道当前可发送。
  */
static uint8_t prvIsTransportReady(void)
{
	uint8_t ucReady; /*!< 临界区内获取的通道就绪快照。 */

	taskENTER_CRITICAL();
	ucReady = s_ucTransportReady;
	taskEXIT_CRITICAL();
	return ucReady;
}

/*-----------------------------------------------------------*/
/**
  * @brief  尝试重新打开已配置的 Transport 输出通道。
  * @retval 1 通道已成功打开并恢复输出。
  * @retval 0 未配置通道或打开失败。
  */
static uint8_t prvTryOpenTransport(void)
{
	TransportResult_e xResult; /*!< 本次打开输出通道的结果。 */

	if ((s_ucTransportConfigured == 0U) ||
		(s_pxLogChannel == NULL)) {
		return 0U;
	}
	xResult = xTransportOpen(s_pxLogChannel);
	if (xResult == TRANSPORT_RESULT_OK) {
		taskENTER_CRITICAL();
		s_ucTransportReady = 1U;
		g_xAppLogStatus.ucTransportReady = 1U;
		g_xAppLogStatus.ucOutputPaused = 0U;
		g_xAppLogStatus.lLastTransportError =
			(int32_t)TRANSPORT_RESULT_OK;
		taskEXIT_CRITICAL();
		return 1U;
	}
	taskENTER_CRITICAL();
	g_xAppLogStatus.lLastTransportError = (int32_t)xResult;
	g_xAppLogStatus.ucOutputPaused = 1U;
	taskEXIT_CRITICAL();
	return 0U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  根据一次发送和提交结果更新计数、就绪状态与重试退避。
  * @param[in] xResult 本次 Transport 发送结果。
  * @param[in] ucCommitted 非零表示发送记录已成功从环形缓冲移除。
  */
static void prvRecordTransportResult(TransportResult_e xResult,
	uint8_t ucCommitted)
{
	taskENTER_CRITICAL();
	g_xAppLogStatus.lLastTransportError = (int32_t)xResult;
	if (xResult == TRANSPORT_RESULT_OK) {
		if (ucCommitted != 0U) {
			g_xAppLogStatus.ulSentCount++;
		}
		s_ulRetryDelayMs = APP_LOG_RETRY_BASE_MS;
		g_xAppLogStatus.ucOutputPaused = 0U;
		g_xAppLogStatus.ucTransportReady = 1U;
		s_ucTransportReady = 1U;
	} else {
		g_xAppLogStatus.ulTransmitFailureCount++;
		g_xAppLogStatus.ulRetryCount++;
		g_xAppLogStatus.ucOutputPaused = 1U;
		if ((xResult == TRANSPORT_RESULT_NOT_OPEN) ||
			(xResult == TRANSPORT_RESULT_NOT_READY)) {
			s_ucTransportReady = 0U;
			g_xAppLogStatus.ucTransportReady = 0U;
		}
		if (s_ulRetryDelayMs < APP_LOG_RETRY_MAX_MS) {
			s_ulRetryDelayMs *= 2U;
			if (s_ulRetryDelayMs > APP_LOG_RETRY_MAX_MS) {
				s_ulRetryDelayMs = APP_LOG_RETRY_MAX_MS;
			}
		}
	}
	taskEXIT_CRITICAL();
}
