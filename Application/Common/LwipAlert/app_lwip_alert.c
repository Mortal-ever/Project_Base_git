/**
  * @file      app_lwip_alert.c
  * @brief     Implement event-triggered LwIP resource alerts.
  * @author    WHong
  * @date      2026-08-27
  */

#include "LwipAlert/app_lwip_alert.h"

#include <string.h>

#include "FreeRTOS.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "task.h"

#if LWIP_STATS && MEMP_STATS

LWIP_MEMPOOL_PROTOTYPE(RX_POOL);

/** @brief Identify the LwIP resources inspected after an API failure. */
typedef enum {
	APP_LWIP_RESOURCE_MEM = 0,
	APP_LWIP_RESOURCE_TCP_PCB,
	APP_LWIP_RESOURCE_TCP_PCB_LISTEN,
	APP_LWIP_RESOURCE_TCP_SEG,
	APP_LWIP_RESOURCE_NETCONN,
	APP_LWIP_RESOURCE_PBUF,
	APP_LWIP_RESOURCE_PBUF_POOL,
	APP_LWIP_RESOURCE_RX_POOL,
	APP_LWIP_RESOURCE_SYS_TIMEOUT,
	APP_LWIP_RESOURCE_TCPIP_MSG_API,
	APP_LWIP_RESOURCE_TCPIP_MSG_INPKT,
	APP_LWIP_RESOURCE_COUNT
} AppLwipResource_e;

/** @brief Hold one atomic snapshot of an LwIP heap or memory pool. */
typedef struct {
	uint32_t ulError;
	uint32_t ulUsed;
	uint32_t ulMax;
	uint32_t ulAvail;
	uint8_t ucValid;
} AppLwipResourceStat_t;

/** @brief Stable allocation-failure event names shared by all targets. */
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

/** @brief Stable capacity event names shared by all targets. */
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

/** @brief Last observed cumulative allocation-error count per resource. */
static uint32_t s_aulLastErrors[APP_LWIP_RESOURCE_COUNT];

static uint8_t prvReadMempStatsLocked(memp_t xType,
	AppLwipResourceStat_t *pxStat);
static uint8_t prvReadResourceStatsLocked(uint8_t ucResource,
	AppLwipResourceStat_t *pxStat);

/*-----------------------------------------------------------*/
void vAppLwipAlertReportFailure(AppLogSourceId_t xSource,
	int32_t lNativeError)
{
	AppLwipResourceStat_t xStat;
	uint8_t ucResource;
	uint8_t ucErrorIncreased;
	uint32_t ulTcpPcbUsed;

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
		"LWIP_RESOURCE_ALERT", lNativeError, "tcp_pcb_used",
		(int32_t)ulTcpPcbUsed);

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
static uint8_t prvReadMempStatsLocked(memp_t xType,
	AppLwipResourceStat_t *pxStat)
{
	struct stats_mem *pxMempStats;

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
void vAppLwipAlertReportFailure(AppLogSourceId_t xSource,
	int32_t lNativeError)
{
	(void)xSource;
	(void)lNativeError;
}

#endif
