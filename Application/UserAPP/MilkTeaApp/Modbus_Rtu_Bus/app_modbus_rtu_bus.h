/**
  * @file      app_modbus_rtu_bus.h
  * @brief     Define product-owned Modbus RTU bus task instances.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef APP_MODBUS_RTU_BUS_H
#define APP_MODBUS_RTU_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "modbus_port.h"
#include "stm32f4xx_hal.h"

/** @brief Number of statically configured UART Modbus buses. */
#define APP_MODBUS_RTU_BUS_COUNT            4U

/** @brief Describe one product RTU bus binding. */
typedef struct {
	UART_HandleTypeDef *pxUart; /*!< CubeMX UART handle owned by the bus. */
	const char *pcName; /*!< Stable Transport registry name. */
	uint8_t ucBusId; /*!< One-based product bus identifier. */
	uint8_t ucEnabled; /*!< Nonzero when the bus task may open the UART. */
} AppModbusRtuBusConfig_t;

/** @brief Store the observable runtime state of one RTU bus task. */
typedef struct {
	ModbusPortResult_e xLastResult; /*!< Latest protocol poll result. */
	uint32_t ulPollCount; /*!< Number of completed poll-hook calls. */
	uint32_t ulErrorCount; /*!< Number of non-OK poll results. */
	uint8_t ucReady; /*!< Nonzero after Transport and Modbus initialization. */
	uint8_t ucEnabled; /*!< Copy of the product enable configuration. */
} AppModbusRtuBusStatus_t;

/** @brief Runtime status array indexed by zero-based bus index. */
extern AppModbusRtuBusStatus_t
	g_axModbusRtuBusStatus[APP_MODBUS_RTU_BUS_COUNT];

/**
  * @brief Get a statically configured RTU bus descriptor.
  * @param[in] ucBusIndex Zero-based index in the configured bus table.
  * @return Pointer to immutable bus configuration, or NULL when out of range.
  */
const AppModbusRtuBusConfig_t *pxAppModbusRtuBusGetConfig(
	uint8_t ucBusIndex);

/**
  * @brief Run one RTU bus owner task selected by its task argument.
  * @param[in] pvArgument Encoded zero-based bus index.
  * @note This FreeRTOS task entry never returns.
  */
void vAppModbusRtuBusTask(void *pvArgument);

/**
  * @brief Poll devices assigned to one RTU bus.
  * @param[in] ucBusId One-based product bus identifier.
  * @param[in,out] pxPort Initialized RTU Modbus client owned by the task.
  * @param[in,out] pxStatus Bus status updated by the implementation.
  * @note Override the weak implementation when device register maps are known.
  *       The hook runs only in the selected UART owner task.
  */
void vAppModbusRtuBusPoll(uint8_t ucBusId,
	ModbusPort_t *pxPort, AppModbusRtuBusStatus_t *pxStatus);

#ifdef __cplusplus
}
#endif

#endif /* APP_MODBUS_RTU_BUS_H */
