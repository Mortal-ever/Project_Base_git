/**
  * @file      coffee2_rtu_bus.h
  * @brief     Define four Coffee2 UART bus owner tasks.
  * @author    WHong
  * @date      2026-07-30
  */

#ifndef COFFEE2_RTU_BUS_H
#define COFFEE2_RTU_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "coffee2_app_config.h"
#include "modbus_port.h"
#include "stm32f4xx_hal.h"

/** @brief Configure one physical Coffee2 UART bus owner. */
typedef struct {
    UART_HandleTypeDef *pxUart;
    const char *pcName;
    uint32_t ulDefaultBaudRate;
    uint8_t ucBusId;
    uint8_t ucProtocolId;
} Coffee2RtuBusConfig_t;

/** @brief Store runtime counters for one RTU bus owner task. */
typedef struct {
	uint32_t ulCommandCount;
	uint32_t ulErrorCount;
	uint32_t ulCurrentBaudRate;
	int32_t lLastResult;
	uint8_t ucReady;
	uint8_t ucActiveDevice;
} Coffee2RtuBusStatus_t;

extern Coffee2RtuBusStatus_t
	g_axCoffee2RtuBusStatus[COFFEE2_RTU_BUS_COUNT];

/**
  * @brief Reapply Coffee2-owned parameters to UART2-UART5.
  * @retval HAL_OK Every UART was initialized.
  * @return First HAL error otherwise.
  * @note Call before creating UART Transport channels.
  */
HAL_StatusTypeDef xCoffee2SerialApplyDefaults(void);

/**
  * @brief Reapply the Coffee2 log UART1 parameters.
  * @retval HAL_OK USART1 was initialized.
  * @return HAL error when the log UART could not be configured.
  * @note A failure degrades logging but does not block business UART startup.
  */
HAL_StatusTypeDef xCoffee2LogSerialApplyDefault(void);

/**
  * @brief Create four static command queues and register routes 2 through 5.
  * @retval pdPASS All queues were created.
  * @retval pdFAIL A queue resource was unavailable.
  */
BaseType_t xCoffee2RtuBusInitialize(void);

/**
  * @brief Return one zero-based UART bus task configuration.
  * @param[in] ucIndex Zero-based bus index.
  * @return Persistent configuration pointer, or NULL.
  */
const Coffee2RtuBusConfig_t *pxCoffee2RtuBusGetConfig(uint8_t ucIndex);

/**
  * @brief Own one UART, one selected protocol, and attached devices.
  * @param[in] pvArgument Persistent Coffee2RtuBusConfig_t pointer.
  */
void vCoffee2RtuBusTask(void *pvArgument);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE2_RTU_BUS_H */
