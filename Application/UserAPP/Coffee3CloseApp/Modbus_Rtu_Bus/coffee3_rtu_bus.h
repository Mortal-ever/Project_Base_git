/**
  * @file      coffee3_rtu_bus.h
  * @brief     Define four Coffee3 UART bus owner tasks.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE3_RTU_BUS_H
#define COFFEE3_RTU_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee3_app_config.h"
#include "modbus_port.h"
#include "stm32f4xx_hal.h"

/** @brief Configure one physical Coffee3 UART bus owner. */
typedef struct {
    UART_HandleTypeDef *pxUart; /*!< Borrowed HAL UART handle, valid for the entire bus task lifetime. */
    const char *pcName; /*!< Borrowed static diagnostic name; storage outlives owner tasks. */
    uint32_t ulDefaultBaudRate; /*!< Product UART baud rate in bits per second. */
    uint8_t ucBusId; /*!< Physical route number, distinct from the zero-based bus array index. */
    uint8_t ucProtocolId; /*!< Wire protocol selected for the bound device or bus. */
} Coffee3RtuBusConfig_t;

/** @brief Store runtime counters for one RTU bus owner task. */
typedef struct {
	uint32_t ulCommandCount; /*!< Cumulative commands observed by this owner/status object. */
	uint32_t ulErrorCount; /*!< Cumulative failures; latest cause is retained separately. */
	uint32_t ulCurrentBaudRate; /*!< Baud rate recorded after owner transport initialization. */
	int32_t lLastResult; /*!< Latest native owner result; zero alone does not prove readiness. */
	uint8_t ucReady; /*!< Module-specific readiness; consult the producer before issuing work. */
	uint8_t ucActiveDevice; /*!< Logical device currently executing on this serial owner. */
} Coffee3RtuBusStatus_t;

extern Coffee3RtuBusStatus_t
	g_axCoffee3RtuBusStatus[COFFEE3_RTU_BUS_COUNT];

/**
  * @brief Reapply Coffee3-owned parameters to UART2-UART5.
  * @retval HAL_OK Every UART was initialized.
  * @return First HAL error otherwise.
  * @note Call before creating UART Transport channels.
  */
HAL_StatusTypeDef xCoffee3SerialApplyDefaults(void);

/**
  * @brief Reapply the Coffee3 log UART1 parameters.
  * @retval HAL_OK USART1 was initialized.
  * @return HAL error when the log UART could not be configured.
  * @note A failure degrades logging but does not block business UART startup.
  */
HAL_StatusTypeDef xCoffee3LogSerialApplyDefault(void);

/**
  * @brief Create four static command queues and register routes 2 through 5.
  * @retval pdPASS All queues were created.
  * @retval pdFAIL A queue resource was unavailable.
  */
BaseType_t xCoffee3RtuBusInitialize(void);

/**
  * @brief Return one zero-based UART bus task configuration.
  * @param[in] ucIndex Zero-based bus index.
  * @return Persistent configuration pointer, or NULL.
  */
const Coffee3RtuBusConfig_t *pxCoffee3RtuBusGetConfig(uint8_t ucIndex);

/**
  * @brief Own one UART, one selected protocol, and attached devices.
  * @param[in] pvArgument Persistent Coffee3RtuBusConfig_t pointer.
  */
void vCoffee3RtuBusTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_RTU_BUS_H */
