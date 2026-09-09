/**
  * @file      coffee3_log.c
  * @brief     Provide the Coffee3 log adapter over the common AppLog core.
  * @author    WHong
  * @date      2026-08-20
  *
  * @details   This adapter preserves the Coffee3 API and source labels and
  *            creates the USART1 Transport endpoint. Shared LwIP resource
  *            alerts are delegated to the common LwipAlert service.
  */

#include "coffee3_log.h"

#include <stdarg.h>
#include <stdio.h>

#include <string.h>

#include "LwipAlert/app_lwip_alert.h"
#include "coffee3_app_config.h"
#include "transport_uart.h"
#include "usart.h"

/** @brief Provide the unchanged Coffee3 source-to-prefix mapping. */
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

/** @brief Generic channel bound to the Coffee3 USART1 backend. */
static TransportChannel_t s_xCoffee3LogChannel;
/** @brief USART1 backend context kept in normal SRAM for DMA ownership. */
static TransportUartContext_t s_xCoffee3LogTransport;

/*-----------------------------------------------------------*/
Coffee3LogResult_e xCoffee3LogInit(void)
{
	return xCoffee3LogInitWithTransport(1U);
}

/*-----------------------------------------------------------*/
Coffee3LogResult_e xCoffee3LogInitWithTransport(uint8_t ucEnableTransport)
{
	/* Build the source table first, then optionally bind USART1. The common
	 * log core remains usable for early diagnostics when transport is absent. */
	AppLogConfig_t xConfig;
	TransportUartConfig_t xUartConfig;
	AppLogResult_e xLogResult;
	TransportResult_e xTransportResult;

	/* Initialization is intentionally idempotent at the adapter boundary. */
	if (g_xAppLogStatus.ucInitialized != 0U) {
		return COFFEE3_LOG_RESULT_ALREADY_INITIALIZED;
	}
	memset(&xConfig, 0, sizeof(xConfig));
	memset(&xUartConfig, 0, sizeof(xUartConfig));
	xTransportResult = TRANSPORT_RESULT_NOT_READY;
	xConfig.pxSourceTable = s_axCoffee3LogSources;
	xConfig.ucSourceCount = COFFEE3_LOG_SOURCE_COUNT;
	if (ucEnableTransport != 0U) {
		if (huart1.Instance == NULL) {
			xTransportResult = TRANSPORT_RESULT_NOT_OPEN;
		} else {
			xUartConfig.pxUart = &huart1;
			xUartConfig.ucReceiveEnabled = 0U;
			xTransportResult = xTransportUartCreate(
				&s_xCoffee3LogChannel, &s_xCoffee3LogTransport,
				"coffee3_log_uart", &xUartConfig);
			if (xTransportResult == TRANSPORT_RESULT_OK) {
				xConfig.pxTransportChannel = &s_xCoffee3LogChannel;
				xConfig.ucEnableTransport = 1U;
			}
		}
	}
	/* The ring can be valid even when UART setup failed; preserve that status
	 * so startup can emit a raw failure and continue module diagnosis. */
	xLogResult = xAppLogInit(&xConfig);
	if ((xLogResult == APP_LOG_RESULT_TRANSPORT) &&
		(xConfig.pxTransportChannel == NULL)) {
		g_xAppLogStatus.lLastTransportError =
			(int32_t)xTransportResult;
	}
	return (Coffee3LogResult_e)xLogResult;
}

/*-----------------------------------------------------------*/
Coffee3LogResult_e xCoffee3LogWrite(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, const char *pcText, int32_t lCode)
{
	return xCoffee3LogWriteOrder(xLevel, xSource,
		COFFEE3_LOG_ORDER_SYSTEM, pcText, lCode);
}

/*-----------------------------------------------------------*/
Coffee3LogResult_e xCoffee3LogWriteOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lCode)
{
	return (Coffee3LogResult_e)xAppLogWriteOrder(
		(AppLogLevel_e)xLevel, (AppLogSourceId_t)xSource,
		(AppLogOrderId_t)usOrderId, pcText, lCode);
}

/*-----------------------------------------------------------*/
Coffee3LogResult_e xCoffee3LogWriteField(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue)
{
	return xCoffee3LogWriteFieldOrder(xLevel, xSource,
		COFFEE3_LOG_ORDER_SYSTEM, pcText, lResult, pcFieldName,
		lFieldValue);
}

/*-----------------------------------------------------------*/
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
Coffee3LogResult_e xCoffee3LogWriteTextOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcText)
{
	return (Coffee3LogResult_e)xAppLogWriteTextOrder(
		(AppLogLevel_e)xLevel, (AppLogSourceId_t)xSource,
		(AppLogOrderId_t)usOrderId, pcText);
}

/*-----------------------------------------------------------*/
Coffee3LogResult_e xCoffee3LogPrintfOrder(Coffee3LogLevel_e xLevel,
	Coffee3LogSource_e xSource, uint16_t usOrderId, const char *pcFormat, ...)
{
	/* Format into a bounded stack buffer before copying text into the common
	 * record. Truncation is preferred to unbounded allocation in a task. */
	char acText[72];
	va_list xArguments;
	int lLength;

	if (pcFormat == NULL) {
		return COFFEE3_LOG_RESULT_INVALID_ARG;
	}
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
int32_t lCoffee3LogEarlyWrite(const uint8_t *pucData, uint16_t usLength)
{
	return lAppLogEarlyWrite(pucData, usLength);
}

/*-----------------------------------------------------------*/
void vCoffee3LogTask(void *pvArgument)
{
	(void)xCoffee3LogWrite(COFFEE3_LOG_LEVEL_INFO,
		COFFEE3_LOG_SOURCE_SYSTEM, "TASK_RUNNING:C3Log", 0);
	vAppLogTask(pvArgument);
}

/*-----------------------------------------------------------*/
void vCoffee3LogSetTaskReady(uint8_t ucCreated)
{
	vAppLogSetTaskReady(ucCreated);
}

/*-----------------------------------------------------------*/
void vCoffee3LogGetStatus(Coffee3LogStatus_t *pxStatus)
{
	vAppLogGetStatus(pxStatus);
}

/*-----------------------------------------------------------*/
void vCoffee3LogLwipResourceFailure(Coffee3LogSource_e xSource,
	int32_t lNativeError)
{
	vAppLwipAlertReportFailure((AppLogSourceId_t)xSource, lNativeError);
}
