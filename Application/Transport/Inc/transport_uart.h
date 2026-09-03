/**
  * @file      transport_uart.h
  * @brief     Define the HAL UART and RS485 Transport backend.
  * @author    WHong
  * @date      2026-07-28
  */

#ifndef TRANSPORT_UART_H
#define TRANSPORT_UART_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32f4xx_hal.h"
#include "stream_buffer.h"
#include "transport.h"

/** @brief Maximum number of UART contexts routed by HAL callbacks. */
#define TRANSPORT_UART_MAX_CHANNELS       6U
/** @brief Static UART receive stream capacity in bytes. */
#define TRANSPORT_UART_RX_BUFFER_SIZE     256U
/** @brief DMA-accessible UART transmit staging capacity in bytes. */
#define TRANSPORT_UART_TX_BUFFER_SIZE     256U

/** @brief Configure one UART or RS485 Transport endpoint. */
typedef struct {
	UART_HandleTypeDef *pxUart; /*!< CubeMX HAL UART handle. */
	GPIO_TypeDef *pxDirectionPort; /*!< RS485 DE port, or NULL. */
	uint16_t usDirectionPin; /*!< RS485 DE GPIO pin mask. */
	GPIO_PinState xTxEnableLevel; /*!< GPIO level that enables TX. */
	uint8_t ucHalfDuplex; /*!< Nonzero enables half-duplex switching. */
	uint8_t ucReceiveEnabled; /*!< Nonzero enables managed interrupt RX. */
} TransportUartConfig_t;

/** @brief Store static RTOS objects and ISR state for one UART channel. */
typedef struct {
	TransportChannel_t *pxChannel; /*!< Generic channel owning this context. */
	TransportUartConfig_t xConfig; /*!< Copied UART/RS485 configuration. */
	SemaphoreHandle_t xTxMutex; /*!< Serializes task transmitters. */
	SemaphoreHandle_t xTxDone; /*!< HAL TX-complete signal. */
	StreamBufferHandle_t xRxStream; /*!< ISR-to-task receive stream. */
	StaticSemaphore_t xTxMutexStorage; /*!< Static TX mutex storage. */
	StaticSemaphore_t xTxDoneStorage; /*!< Static TX signal storage. */
	StaticStreamBuffer_t xRxStreamStorage; /*!< Static stream metadata. */
	uint8_t aucTxStorage[TRANSPORT_UART_TX_BUFFER_SIZE];
		/*!< DMA-accessible TX staging storage. */
	uint8_t aucRxStorage[TRANSPORT_UART_RX_BUFFER_SIZE];
		/*!< Static StreamBuffer data storage. */
	uint8_t ucRxByte; /*!< One-byte HAL interrupt receive target. */
	volatile uint8_t ucIsOpen; /*!< Nonzero while the channel is active. */
	volatile uint8_t ucRxPaused; /*!< Nonzero while RX rearming is paused. */
	volatile uint8_t ucTxActive; /*!< Nonzero while DMA TX is active. */
	volatile uint8_t ucTxError; /*!< Nonzero after active TX callback error. */
	volatile uint32_t ulRxDropCount; /*!< ISR bytes lost on full stream. */
	volatile uint32_t ulErrorCount; /*!< HAL UART callback error count. */
	volatile int32_t lLastNativeError; /*!< Latest HAL status or error code. */
} TransportUartContext_t;

/**
  * @brief Create and register one HAL UART Transport channel.
  * @param[out] pxChannel Caller-owned generic channel object.
  * @param[out] pxContext Persistent caller-owned UART context.
  * @param[in] pcName Persistent unique registry name.
  * @param[in] pxConfig UART configuration copied into the context.
  * @return Normalized creation, HAL, or registration result.
  * @note All RTOS objects are created from storage inside pxContext.
  */
TransportResult_e xTransportUartCreate(TransportChannel_t *pxChannel,
									   TransportUartContext_t *pxContext,
									   const char *pcName,
									   const TransportUartConfig_t *pxConfig);

/**
  * @brief Handle an unclaimed HAL UART transmit-complete callback.
  * @param[in] pxUart HAL UART handle not owned by this module.
  * @note Override the weak implementation for another UART owner.
  */
void vTransportUartUnclaimedTxCallback(UART_HandleTypeDef *pxUart);

/**
  * @brief Handle an unclaimed HAL UART receive-complete callback.
  * @param[in] pxUart HAL UART handle not owned by this module.
  * @note Override the weak implementation for another UART owner.
  */
void vTransportUartUnclaimedRxCallback(UART_HandleTypeDef *pxUart);

/**
  * @brief Handle an unclaimed HAL UART error callback.
  * @param[in] pxUart HAL UART handle not owned by this module.
  * @note Override the weak implementation for another UART owner.
  */
void vTransportUartUnclaimedErrorCallback(UART_HandleTypeDef *pxUart);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_UART_H */
