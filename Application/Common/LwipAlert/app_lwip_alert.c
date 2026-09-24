/**
  * @file      app_lwip_alert.c
  * @brief     实现由网络接口失败事件触发的 LwIP 资源告警。
  * @author    WHong
  * @date      2026-09-24
  */

#include "LwipAlert/app_lwip_alert.h"

#include <string.h>

#include "FreeRTOS.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "task.h"

#if LWIP_STATS && MEMP_STATS

LWIP_MEMPOOL_PROTOTYPE(RX_POOL);

/** @brief 标识网络接口失败后需要检查的 LwIP 内存资源。 */
typedef enum {
	APP_LWIP_RESOURCE_MEM = 0, /*!< LwIP 通用堆。 */
	APP_LWIP_RESOURCE_TCP_PCB, /*!< 活动 TCP 控制块池。 */
	APP_LWIP_RESOURCE_TCP_PCB_LISTEN, /*!< TCP 监听控制块池。 */
	APP_LWIP_RESOURCE_TCP_SEG, /*!< TCP 报文段描述池。 */
	APP_LWIP_RESOURCE_NETCONN, /*!< 顺序接口连接对象池。 */
	APP_LWIP_RESOURCE_PBUF, /*!< PBUF 描述对象池。 */
	APP_LWIP_RESOURCE_PBUF_POOL, /*!< PBUF 数据缓冲池。 */
	APP_LWIP_RESOURCE_RX_POOL, /*!< 以太网接收缓冲池。 */
	APP_LWIP_RESOURCE_SYS_TIMEOUT, /*!< 软件定时器节点池。 */
	APP_LWIP_RESOURCE_TCPIP_MSG_API, /*!< TCPIP 接口消息池。 */
	APP_LWIP_RESOURCE_TCPIP_MSG_INPKT, /*!< TCPIP 收包消息池。 */
	APP_LWIP_RESOURCE_COUNT /*!< 资源项总数，不代表实际资源。 */
} AppLwipResource_e;

/** @brief 保存一次临界区内读取的 LwIP 堆或内存池快照。 */
typedef struct {
	uint32_t ulError; /*!< 累计分配失败次数。 */
	uint32_t ulUsed; /*!< 当前已用资源项数量。 */
	uint32_t ulMax; /*!< 历史同时占用峰值。 */
	uint32_t ulAvail; /*!< 配置的可用资源项总数。 */
	uint8_t ucValid; /*!< 非零表示本快照字段有效。 */
} AppLwipResourceStat_t;

/** @brief 各资源累计分配失败数增长时使用的稳定事件名。 */
static const char * const s_apcAllocEvents[APP_LWIP_RESOURCE_COUNT] = {
	"LWIP_MEM_ALLOC_FAILED",
	"LWIP_TCP_PCB_ALLOC_FAILED",
	"LWIP_TCP_PCB_LISTEN_ALLOC_FAILED",
	"LWIP_TCP_SEG_ALLOC_FAILED",
	"LWIP_NETCONN_ALLOC_FAILED",
	"LWIP_PBUF_ALLOC_FAILED",
	"LWIP_PBUF_POOL_ALLOC_FAILED",
	"LWIP_RX_POOL_ALLOC_FAILED",
	"LWIP_SYS_TIMEOUT_ALLOC_FAILED",
	"LWIP_TCPIP_MSG_API_ALLOC_FAILED",
	"LWIP_TCPIP_MSG_INPKT_ALLOC_FAILED"
};

/** @brief 与分配失败事件配套输出容量快照的稳定事件名。 */
static const char * const s_apcCapacityEvents[APP_LWIP_RESOURCE_COUNT] = {
	"LWIP_MEM_CAPACITY",
	"LWIP_TCP_PCB_CAPACITY",
	"LWIP_TCP_PCB_LISTEN_CAPACITY",
	"LWIP_TCP_SEG_CAPACITY",
	"LWIP_NETCONN_CAPACITY",
	"LWIP_PBUF_CAPACITY",
	"LWIP_PBUF_POOL_CAPACITY",
	"LWIP_RX_POOL_CAPACITY",
	"LWIP_SYS_TIMEOUT_CAPACITY",
	"LWIP_TCPIP_MSG_API_CAPACITY",
	"LWIP_TCPIP_MSG_INPKT_CAPACITY"
};

/** @brief 每类资源上次报告时观察到的累计分配失败数。 */
static uint32_t s_aulLastErrors[APP_LWIP_RESOURCE_COUNT];

static uint8_t prvReadMempStatsLocked(memp_t xType,
	AppLwipResourceStat_t *pxStat);
static uint8_t prvReadResourceStatsLocked(uint8_t ucResource,
	AppLwipResourceStat_t *pxStat);

/*-----------------------------------------------------------*/
/**
  * @brief  记录一次网络接口错误并报告发生增长的 LwIP 资源故障。
  * @param[in] xSource 与本次失败关联的产品日志来源。
  * @param[in] lNativeError LwIP 或套接字返回的原始错误码。
  * @note 仅供任务上下文调用；本函数不分配资源、不重试也不改变网络流程。
  */
void vAppLwipAlertReportFailure(AppLogSourceId_t xSource,
	int32_t lNativeError)
{
	AppLwipResourceStat_t xStat; /*!< 当前正在读取的资源统计快照。 */
	uint8_t ucResource; /*!< 遍历的资源类型索引。 */
	uint8_t ucErrorIncreased; /*!< 本资源累计错误数是否较上次增长。 */
	uint32_t ulTcpPcbUsed; /*!< 记录错误发生时活动 TCP 控制块数量。 */

	memset(&xStat, 0, sizeof(xStat));
	taskENTER_CRITICAL();
	if (prvReadResourceStatsLocked(
		(uint8_t)APP_LWIP_RESOURCE_TCP_PCB, &xStat) != 0U) {
		ulTcpPcbUsed = xStat.ulUsed;
	} else {
		ulTcpPcbUsed = 0U;
	}
	taskEXIT_CRITICAL();
	(void)xAppLogWriteField(APP_LOG_LEVEL_WARNING, xSource,
		"LWIP_RESOURCE_REPORT", lNativeError, "tcp_pcb_used",
		(int32_t)ulTcpPcbUsed);

	/* 逐项比较累计错误数，仅对本次新增的分配失败输出详细容量。 */
	for (ucResource = 0U;
		ucResource < (uint8_t)APP_LWIP_RESOURCE_COUNT;
		ucResource++) {
		memset(&xStat, 0, sizeof(xStat));
		taskENTER_CRITICAL();
		if (prvReadResourceStatsLocked(ucResource, &xStat) != 0U) {
			ucErrorIncreased = (xStat.ulError >
				s_aulLastErrors[ucResource]) ? 1U : 0U;
			s_aulLastErrors[ucResource] = xStat.ulError;
		} else {
			ucErrorIncreased = 0U;
		}
		taskEXIT_CRITICAL();
		if (ucErrorIncreased != 0U) {
			(void)xAppLogWriteField(APP_LOG_LEVEL_ERROR, xSource,
				s_apcAllocEvents[ucResource], (int32_t)xStat.ulError,
				"used", (int32_t)xStat.ulUsed);
			(void)xAppLogWriteField(APP_LOG_LEVEL_WARNING, xSource,
				s_apcCapacityEvents[ucResource], (int32_t)xStat.ulMax,
				"avail", (int32_t)xStat.ulAvail);
		}
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  在调用方已进入临界区时读取指定 LwIP 内存池统计。
  * @param[in] xType LwIP 内存池类型。
  * @param[out] pxStat 统计快照接收位置。
  * @retval 1 类型有效且对应统计对象存在。
  * @retval 0 参数、类型或统计对象无效。
  */
static uint8_t prvReadMempStatsLocked(memp_t xType,
	AppLwipResourceStat_t *pxStat)
{
	struct stats_mem *pxMempStats; /*!< 指向 LwIP 内存池的实时统计对象。 */

	if ((pxStat == NULL) ||
		((uint32_t)xType >= (uint32_t)MEMP_MAX)) {
		return 0U;
	}
	pxMempStats = lwip_stats.memp[xType];
	if (pxMempStats == NULL) {
		return 0U;
	}
	pxStat->ulError = (uint32_t)pxMempStats->err;
	pxStat->ulUsed = (uint32_t)pxMempStats->used;
	pxStat->ulMax = (uint32_t)pxMempStats->max;
	pxStat->ulAvail = (uint32_t)pxMempStats->avail;
	pxStat->ucValid = 1U;
	return 1U;
}

/*-----------------------------------------------------------*/
/**
  * @brief  在调用方已进入临界区时读取统一资源编号对应的统计。
  * @param[in] ucResource AppLwipResource_e 资源编号。
  * @param[out] pxStat 初始化并写入的资源统计快照。
  * @retval 1 当前编译配置提供该资源的有效统计。
  * @retval 0 参数无效、资源未知或相关统计未启用。
  */
static uint8_t prvReadResourceStatsLocked(uint8_t ucResource,
	AppLwipResourceStat_t *pxStat)
{
	if (pxStat == NULL) {
		return 0U;
	}
	pxStat->ulError = 0U;
	pxStat->ulUsed = 0U;
	pxStat->ulMax = 0U;
	pxStat->ulAvail = 0U;
	pxStat->ucValid = 0U;
	switch ((AppLwipResource_e)ucResource) {
	case APP_LWIP_RESOURCE_MEM:
#if MEM_STATS
		pxStat->ulError = (uint32_t)lwip_stats.mem.err;
		pxStat->ulUsed = (uint32_t)lwip_stats.mem.used;
		pxStat->ulMax = (uint32_t)lwip_stats.mem.max;
		pxStat->ulAvail = (uint32_t)lwip_stats.mem.avail;
		pxStat->ucValid = 1U;
#endif
		break;
	case APP_LWIP_RESOURCE_TCP_PCB:
#if LWIP_TCP
		(void)prvReadMempStatsLocked(MEMP_TCP_PCB, pxStat);
#endif
		break;
	case APP_LWIP_RESOURCE_TCP_PCB_LISTEN:
#if LWIP_TCP
		(void)prvReadMempStatsLocked(MEMP_TCP_PCB_LISTEN, pxStat);
#endif
		break;
	case APP_LWIP_RESOURCE_TCP_SEG:
#if LWIP_TCP
		(void)prvReadMempStatsLocked(MEMP_TCP_SEG, pxStat);
#endif
		break;
	case APP_LWIP_RESOURCE_NETCONN:
#if LWIP_NETCONN || LWIP_SOCKET
		(void)prvReadMempStatsLocked(MEMP_NETCONN, pxStat);
#endif
		break;
	case APP_LWIP_RESOURCE_PBUF:
		(void)prvReadMempStatsLocked(MEMP_PBUF, pxStat);
		break;
	case APP_LWIP_RESOURCE_PBUF_POOL:
		(void)prvReadMempStatsLocked(MEMP_PBUF_POOL, pxStat);
		break;
	case APP_LWIP_RESOURCE_RX_POOL:
		if (memp_RX_POOL.stats != NULL) {
			pxStat->ulError = (uint32_t)memp_RX_POOL.stats->err;
			pxStat->ulUsed = (uint32_t)memp_RX_POOL.stats->used;
			pxStat->ulMax = (uint32_t)memp_RX_POOL.stats->max;
			pxStat->ulAvail = (uint32_t)memp_RX_POOL.stats->avail;
			pxStat->ucValid = 1U;
		}
		break;
	case APP_LWIP_RESOURCE_SYS_TIMEOUT:
#if LWIP_TIMERS && !LWIP_TIMERS_CUSTOM
		(void)prvReadMempStatsLocked(MEMP_SYS_TIMEOUT, pxStat);
#endif
		break;
	case APP_LWIP_RESOURCE_TCPIP_MSG_API:
#if NO_SYS == 0
		(void)prvReadMempStatsLocked(MEMP_TCPIP_MSG_API, pxStat);
#endif
		break;
	case APP_LWIP_RESOURCE_TCPIP_MSG_INPKT:
#if (NO_SYS == 0) && !LWIP_TCPIP_CORE_LOCKING_INPUT
		(void)prvReadMempStatsLocked(MEMP_TCPIP_MSG_INPKT, pxStat);
#endif
		break;
	default:
		break;
	}
	return pxStat->ucValid;
}

#else

/*-----------------------------------------------------------*/
/**
  * @brief  在 LwIP 统计关闭时提供无操作的故障报告实现。
  * @param[in] xSource 未使用的产品日志来源。
  * @param[in] lNativeError 未使用的原始网络错误码。
  */
void vAppLwipAlertReportFailure(AppLogSourceId_t xSource,
	int32_t lNativeError)
{
	(void)xSource;
	(void)lNativeError;
}

#endif

