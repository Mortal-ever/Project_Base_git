/**
  * @file      app_modbus_rtu_bus.c
  * @brief     Implement statically configured Modbus RTU bus tasks.
  * @author    WHong
  * @date      2026-07-28
  */

#include "app_modbus_rtu_bus.h"

#include <string.h>

#include "app_modbus_rtu_config.h"
#include "task.h"
#include "transport_uart.h"
#include "usart.h"

/** @brief Store private Transport and protocol objects for one RTU bus. */
typedef struct {
	TransportChannel_t xChannel; /*!< Generic UART channel. */
	TransportUartContext_t xTransport; /*!< HAL UART backend state. */
	ModbusPort_t xPort; /*!< RTU Modbus client instance. */
	uint8_t ucCreated; /*!< Nonzero after complete bus initialization. */
} AppModbusRtuBusContext_t;

/** @brief Runtime status of all statically configured RTU buses. */
AppModbusRtuBusStatus_t
	g_axModbusRtuBusStatus[APP_MODBUS_RTU_BUS_COUNT];

/** @brief Immutable UART, name, ID, and enable configuration table. */
static const AppModbusRtuBusConfig_t s_axBusConfig[
	APP_MODBUS_RTU_BUS_COUNT] = {
	{ &huart4, "rtu_bus1", 1U, APP_MODBUS_RTU_BUS1_ENABLE },
	{ &huart5, "rtu_bus2", 2U, APP_MODBUS_RTU_BUS2_ENABLE },
	{ &huart2, "rtu_bus3", 3U, APP_MODBUS_RTU_BUS3_ENABLE },
	{ &huart3, "rtu_bus4", 4U, APP_MODBUS_RTU_BUS4_ENABLE }
};

/** @brief Private runtime objects indexed by zero-based bus index. */
static AppModbusRtuBusContext_t
	s_axBusContext[APP_MODBUS_RTU_BUS_COUNT];

/*-----------------------------------------------------------*/
const AppModbusRtuBusConfig_t *pxAppModbusRtuBusGetConfig(
	uint8_t ucBusIndex)
{
	if (ucBusIndex >= APP_MODBUS_RTU_BUS_COUNT) {
		return NULL;
	}
	return &s_axBusConfig[ucBusIndex];
}

/*-----------------------------------------------------------*/
void vAppModbusRtuBusTask(void *pvArgument)
{
	const AppModbusRtuBusConfig_t *pxConfig;
	AppModbusRtuBusContext_t *pxContext;
	AppModbusRtuBusStatus_t *pxStatus;
	TransportUartConfig_t xUartConfig;
	ModbusPortResult_e xPortResult;
	TransportResult_e xTransportResult;
	uint8_t ucIndex;

	pxConfig = (const AppModbusRtuBusConfig_t *)pvArgument;
	if ((pxConfig == NULL) || (pxConfig->ucBusId == 0U) ||
		(pxConfig->ucBusId > APP_MODBUS_RTU_BUS_COUNT)) {
		vTaskDelete(NULL);
		return;
	}
	ucIndex = (uint8_t)(pxConfig->ucBusId - 1U);
	pxContext = &s_axBusContext[ucIndex];
	pxStatus = &g_axModbusRtuBusStatus[ucIndex];
	memset(pxContext, 0, sizeof(*pxContext));
	memset(pxStatus, 0, sizeof(*pxStatus));
	pxStatus->ucEnabled = pxConfig->ucEnabled;
	if (pxConfig->ucEnabled == 0U) {
		for (;;) {
			vTaskDelay(pdMS_TO_TICKS(APP_MODBUS_RTU_DISABLED_MS));
		}
	}

	memset(&xUartConfig, 0, sizeof(xUartConfig));
	xUartConfig.pxUart = pxConfig->pxUart;
	xUartConfig.xTxEnableLevel = GPIO_PIN_SET;
	xUartConfig.ucReceiveEnabled = 1U;
	xTransportResult = xTransportUartCreate(&pxContext->xChannel,
		&pxContext->xTransport, pxConfig->pcName, &xUartConfig);
	if (xTransportResult == TRANSPORT_RESULT_OK) {
		xTransportResult = xTransportOpen(&pxContext->xChannel);
	}
	if (xTransportResult == TRANSPORT_RESULT_OK) {
		xPortResult = xModbusPortClientInit(&pxContext->xPort,
			&pxContext->xChannel, MODBUS_PORT_TRANSPORT_RTU,
			APP_MODBUS_RTU_TIMEOUT_MS);
		if (xPortResult == MODBUS_PORT_RESULT_OK) {
			pxContext->ucCreated = 1U;
			pxStatus->ucReady = 1U;
		} else {
			pxStatus->xLastResult = xPortResult;
			pxStatus->ulErrorCount++;
		}
	} else {
		pxStatus->xLastResult = MODBUS_PORT_RESULT_TRANSPORT;
		pxStatus->ulErrorCount++;
	}

	for (;;) {
		if (pxContext->ucCreated != 0U) {
			vAppModbusRtuBusPoll(pxConfig->ucBusId,
				&pxContext->xPort, pxStatus);
			pxStatus->ulPollCount++;
		}
		vTaskDelay(pdMS_TO_TICKS(APP_MODBUS_RTU_LOOP_MS));
	}
}

/*-----------------------------------------------------------*/
__weak void vAppModbusRtuBusPoll(uint8_t ucBusId,
	ModbusPort_t *pxPort, AppModbusRtuBusStatus_t *pxStatus)
{
	(void)ucBusId;
	(void)pxPort;
	(void)pxStatus;
}
