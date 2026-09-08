/**
  * @file      coffee2_log.c
  * @brief     Provide the Coffee2 log adapter over the common AppLog core.
  * @author    WHong
  * @date      2026-08-20
  *
  * @details   This adapter preserves the Coffee2 API and source labels and
  *            creates the USART1 Transport endpoint. Shared LwIP resource
  *            alerts are delegated to the common LwipAlert service.
  */

#include "coffee2_log.h"

#include <stdarg.h>
#include <stdio.h>

#include <string.h>

#include "LwipAlert/app_lwip_alert.h"
#include "coffee2_app_config.h"
#include "transport_uart.h"
#include "usart.h"

/** @brief Provide the unchanged Coffee2 source-to-prefix mapping. */
static const AppLogSourceDescriptor_t s_axCoffee2LogSources[
	COFFEE2_LOG_SOURCE_COUNT] = {
	{ "C2Main", "System" },
	{ "C2Server", "MBTcpServer" },
	{ "C2Workflow", "Workflow" },
	{ "C2Robot", "MBTcpClient" },
	{ "C2Bus2", "MBRtu" },
	{ "C2Bus3", "MBRtu" },
	{ "C2Bus4", "MBRtu" },
	{ "C2Bus5", "MBRtu" },
	{ "C2Workflow", "IO" },
	{ "C2Bus2", "Coffee" },
	{ "C2Bus3", "Cup" },
	{ "C2Bus3", "Syrup" },
	{ "C2Bus3", "Lid" },
	{ "C2Bus4", "Ice" },
	{ "C2Bus4", "Weigh" },
	{ "C2Bus3", "EnergyMeter" },
	{ "C2Bus5", "IoInput" },
	{ "C2Bus5", "IoOutput" }
};

/** @brief Generic channel bound to the Coffee2 USART1 backend. */
static TransportChannel_t s_xCoffee2LogChannel;
/** @brief USART1 backend context kept in normal SRAM for DMA ownership. */
static TransportUartContext_t s_xCoffee2LogTransport;

/*-----------------------------------------------------------*/
Coffee2LogResult_e xCoffee2LogInit(void)
{
	return xCoffee2LogInitWithTransport(1U);
}

/*-----------------------------------------------------------*/
Coffee2LogResult_e xCoffee2LogInitWithTransport(uint8_t ucEnableTransport)
{
	AppLogConfig_t xConfig;
	TransportUartConfig_t xUartConfig;
	AppLogResult_e xLogResult;
	TransportResult_e xTransportResult;

	if (g_xAppLogStatus.ucInitialized != 0U) {
		return COFFEE2_LOG_RESULT_ALREADY_INITIALIZED;
	}
	memset(&xConfig, 0, sizeof(xConfig));
	memset(&xUartConfig, 0, sizeof(xUartConfig));
	xTransportResult = TRANSPORT_RESULT_NOT_READY;
	xConfig.pxSourceTable = s_axCoffee2LogSources;
	xConfig.ucSourceCount = COFFEE2_LOG_SOURCE_COUNT;
	if (ucEnableTransport != 0U) {
		if (huart1.Instance == NULL) {
			xTransportResult = TRANSPORT_RESULT_NOT_OPEN;
		} else {
			xUartConfig.pxUart = &huart1;
			xUartConfig.ucReceiveEnabled = 0U;
			xTransportResult = xTransportUartCreate(
				&s_xCoffee2LogChannel, &s_xCoffee2LogTransport,
				"coffee2_log_uart", &xUartConfig);
			if (xTransportResult == TRANSPORT_RESULT_OK) {
				xConfig.pxTransportChannel = &s_xCoffee2LogChannel;
				xConfig.ucEnableTransport = 1U;
			}
		}
	}
	xLogResult = xAppLogInit(&xConfig);
	if ((xLogResult == APP_LOG_RESULT_TRANSPORT) &&
		(xConfig.pxTransportChannel == NULL)) {
		g_xAppLogStatus.lLastTransportError =
			(int32_t)xTransportResult;
	}
	return (Coffee2LogResult_e)xLogResult;
}

/*-----------------------------------------------------------*/
Coffee2LogResult_e xCoffee2LogWrite(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, const char *pcText, int32_t lCode)
{
	return xCoffee2LogWriteOrder(xLevel, xSource,
		COFFEE2_LOG_ORDER_SYSTEM, pcText, lCode);
}

/*-----------------------------------------------------------*/
Coffee2LogResult_e xCoffee2LogWriteOrder(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lCode)
{
	return (Coffee2LogResult_e)xAppLogWriteOrder(
		(AppLogLevel_e)xLevel, (AppLogSourceId_t)xSource,
		(AppLogOrderId_t)usOrderId, pcText, lCode);
}

/*-----------------------------------------------------------*/
Coffee2LogResult_e xCoffee2LogWriteField(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue)
{
	return xCoffee2LogWriteFieldOrder(xLevel, xSource,
		COFFEE2_LOG_ORDER_SYSTEM, pcText, lResult, pcFieldName,
		lFieldValue);
}

/*-----------------------------------------------------------*/
Coffee2LogResult_e xCoffee2LogWriteFieldOrder(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lResult, const char *pcFieldName, int32_t lFieldValue)
{
	return (Coffee2LogResult_e)xAppLogWriteFieldOrder(
		(AppLogLevel_e)xLevel, (AppLogSourceId_t)xSource,
		(AppLogOrderId_t)usOrderId, pcText, lResult, pcFieldName,
		lFieldValue);
}

/*-----------------------------------------------------------*/
Coffee2LogResult_e xCoffee2LogWriteTextOrder(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, uint16_t usOrderId, const char *pcText)
{
	return (Coffee2LogResult_e)xAppLogWriteTextOrder(
		(AppLogLevel_e)xLevel, (AppLogSourceId_t)xSource,
		(AppLogOrderId_t)usOrderId, pcText);
}

/*-----------------------------------------------------------*/
Coffee2LogResult_e xCoffee2LogPrintfOrder(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, uint16_t usOrderId, const char *pcFormat, ...)
{
	char acText[72];
	va_list xArguments;
	int lLength;

	if (pcFormat == NULL) {
		return COFFEE2_LOG_RESULT_INVALID_ARG;
	}
	va_start(xArguments, pcFormat);
	lLength = vsnprintf(acText, sizeof(acText), pcFormat, xArguments);
	va_end(xArguments);
	if (lLength < 0) {
		return COFFEE2_LOG_RESULT_INVALID_ARG;
	}
	acText[sizeof(acText) - 1U] = '\0';
	return xCoffee2LogWriteTextOrder(xLevel, xSource, usOrderId, acText);
}

/*-----------------------------------------------------------*/
int32_t lCoffee2LogEarlyWrite(const uint8_t *pucData, uint16_t usLength)
{
	return lAppLogEarlyWrite(pucData, usLength);
}

/*-----------------------------------------------------------*/
void vCoffee2LogTask(void *pvArgument)
{
	(void)xCoffee2LogWrite(COFFEE2_LOG_LEVEL_INFO,
		COFFEE2_LOG_SOURCE_SYSTEM, "TASK_RUNNING:C2Log", 0);
	vAppLogTask(pvArgument);
}

/*-----------------------------------------------------------*/
void vCoffee2LogSetTaskReady(uint8_t ucCreated)
{
	vAppLogSetTaskReady(ucCreated);
}

/*-----------------------------------------------------------*/
void vCoffee2LogGetStatus(Coffee2LogStatus_t *pxStatus)
{
	vAppLogGetStatus(pxStatus);
}

/*-----------------------------------------------------------*/
void vCoffee2LogLwipResourceFailure(Coffee2LogSource_e xSource,
	int32_t lNativeError)
{
	vAppLwipAlertReportFailure((AppLogSourceId_t)xSource, lNativeError);
}
