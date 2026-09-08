/**
  * @file      coffee2_log.h
  * @brief     Define the Coffee2 asynchronous USART1 log service.
  * @author    WHong
  * @date      2026-07-30
  *
  * @details   Producers submit bounded structured records to one static
  *            overwrite-oldest ring. The single log task owns USART1 output
  *            through Transport.
  *
  * @attention
  * - The write API is task-context only.
  * - This module does not provide a debug command or UART receive interface.
  */

#ifndef COFFEE2_LOG_H
#define COFFEE2_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "Log/app_log.h"

/** @brief Define Coffee2 log service results. */
typedef enum {
	COFFEE2_LOG_RESULT_OK = 0,
		/*!< The operation completed successfully. */
	COFFEE2_LOG_RESULT_ALREADY_INITIALIZED = 1,
		/*!< The log service was already initialized. */
	COFFEE2_LOG_RESULT_INVALID_ARG = -1,
		/*!< A pointer, level, or source was invalid. */
	COFFEE2_LOG_RESULT_NOT_READY = -2,
		/*!< The log queue or USART1 transport is unavailable. */
	COFFEE2_LOG_RESULT_QUEUE_FULL = -3,
		/*!< The bounded log queue could not accept the record. */
	COFFEE2_LOG_RESULT_TRANSPORT = -4
		/*!< USART1 Transport creation or opening failed. */
} Coffee2LogResult_e;

/** @brief Define log severity without a debug level. */
typedef enum {
	COFFEE2_LOG_LEVEL_INFO = 0,
	COFFEE2_LOG_LEVEL_WARNING = 1,
	COFFEE2_LOG_LEVEL_ERROR = 2,
	COFFEE2_LOG_LEVEL_COUNT = 3
} Coffee2LogLevel_e;

/** @brief Identify system and manual command log order namespaces. */
#define COFFEE2_LOG_ORDER_SYSTEM       0x0000U
#define COFFEE2_LOG_ORDER_DEBUG        0xF123U

/** @brief Identify the subsystem that submitted a Coffee2 log record. */
typedef enum {
	COFFEE2_LOG_SOURCE_SYSTEM = 0,
	COFFEE2_LOG_SOURCE_SERVER = 1,
	COFFEE2_LOG_SOURCE_WORKFLOW = 2,
	COFFEE2_LOG_SOURCE_ROBOT = 3,
	COFFEE2_LOG_SOURCE_BUS2 = 4,
	COFFEE2_LOG_SOURCE_BUS3 = 5,
	COFFEE2_LOG_SOURCE_BUS4 = 6,
	COFFEE2_LOG_SOURCE_BUS5 = 7,
	COFFEE2_LOG_SOURCE_IO = 8,
	COFFEE2_LOG_SOURCE_COFFEE = 9,
	COFFEE2_LOG_SOURCE_CUP = 10,
	COFFEE2_LOG_SOURCE_SYRUP = 11,
	COFFEE2_LOG_SOURCE_LID = 12,
	COFFEE2_LOG_SOURCE_ICE = 13,
	COFFEE2_LOG_SOURCE_WEIGH = 14,
	COFFEE2_LOG_SOURCE_ENERGY_METER = 15,
	COFFEE2_LOG_SOURCE_IO_INPUT = 16,
	COFFEE2_LOG_SOURCE_IO_OUTPUT = 17,
	COFFEE2_LOG_SOURCE_COUNT = 18
} Coffee2LogSource_e;

/** @brief Reuse the common status layout without a second status object. */
typedef AppLogStatus_t Coffee2LogStatus_t;

/** @brief Preserve the established Coffee2 status symbol at source level. */
#define g_xCoffee2LogStatus g_xAppLogStatus

/**
  * @brief  Initialize the static ring, signal, and USART1 Transport.
  * @retval COFFEE2_LOG_RESULT_OK Initialization completed.
  * @retval COFFEE2_LOG_RESULT_ALREADY_INITIALIZED Initialization ran before.
  * @retval COFFEE2_LOG_RESULT_NOT_READY Static ring or signal creation failed.
  * @retval COFFEE2_LOG_RESULT_TRANSPORT USART1 Transport setup failed while
  *         the ring remains available for Watch diagnostics.
  * @note   Call after vTransportManagerInit() and USART1 HAL initialization.
  */
Coffee2LogResult_e xCoffee2LogInit(void);

/**
  * @brief  Initialize the ring and optionally create the USART1 Transport.
  * @param[in] ucEnableTransport Nonzero only after log UART HAL setup passed.
  * @retval COFFEE2_LOG_RESULT_OK The ring and requested Transport are ready.
  * @retval COFFEE2_LOG_RESULT_TRANSPORT The ring is ready but output is
  *         deliberately disabled or Transport setup failed.
  * @retval COFFEE2_LOG_RESULT_NOT_READY Static ring or signal creation failed.
  */
Coffee2LogResult_e xCoffee2LogInitWithTransport(uint8_t ucEnableTransport);

/**
  * @brief  Submit one bounded record without blocking the caller.
  * @param[in] xLevel Record severity.
  * @param[in] xSource Subsystem that generated the record.
  * @param[in] pcText Null-terminated text copied into the queue record.
  * @param[in] lCode Source-specific result, error, or diagnostic code.
  * @retval COFFEE2_LOG_RESULT_OK The ring accepted the record, overwriting
  *         its oldest record when full.
  * @retval COFFEE2_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval COFFEE2_LOG_RESULT_NOT_READY Initialization is incomplete.
  * @retval COFFEE2_LOG_RESULT_QUEUE_FULL Retained for API compatibility; the
  *         overwrite ring does not return this result for a valid write.
  * @warning Do not call this interface from an interrupt.
  */
Coffee2LogResult_e xCoffee2LogWrite(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, const char *pcText, int32_t lCode);

/**
  * @brief  Submit one Coffee2 log record with an explicit order identifier.
  * @param[in] xLevel Record severity.
  * @param[in] xSource Subsystem that generated the record.
  * @param[in] usOrderId Host, debug, or system order identifier.
  * @param[in] pcText Null-terminated event text.
  * @param[in] lCode Source-specific result or diagnostic code.
  * @retval COFFEE2_LOG_RESULT_OK The ring accepted the record.
  * @retval COFFEE2_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval COFFEE2_LOG_RESULT_NOT_READY Initialization is incomplete.
  */
Coffee2LogResult_e xCoffee2LogWriteOrder(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lCode);

/**
  * @brief  Submit one record with one named diagnostic field.
  * @param[in] xLevel Record severity.
  * @param[in] xSource Subsystem that generated the record.
  * @param[in] pcText Null-terminated event text copied into the record.
  * @param[in] lResult Normalized operation or state result.
  * @param[in] pcFieldName Optional field name, or NULL for no field.
  * @param[in] lFieldValue Field value when pcFieldName is not NULL.
  * @retval COFFEE2_LOG_RESULT_OK The ring accepted the record, overwriting
  *         its oldest record when full.
  * @retval COFFEE2_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval COFFEE2_LOG_RESULT_NOT_READY Initialization is incomplete.
  * @retval COFFEE2_LOG_RESULT_QUEUE_FULL Retained for API compatibility; the
  *         overwrite ring does not return this result for a valid write.
  * @warning Do not call this interface from an interrupt.
  */
Coffee2LogResult_e xCoffee2LogWriteField(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue);

/**
  * @brief  Submit one named Coffee2 record with an order identifier.
  * @param[in] xLevel Record severity.
  * @param[in] xSource Subsystem that generated the record.
  * @param[in] usOrderId Host, debug, or system order identifier.
  * @param[in] pcText Null-terminated event text.
  * @param[in] lResult Normalized operation or state result.
  * @param[in] pcFieldName Optional field name, or NULL for no field.
  * @param[in] lFieldValue Field value when pcFieldName is not NULL.
  * @retval COFFEE2_LOG_RESULT_OK The ring accepted the record.
  * @retval COFFEE2_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval COFFEE2_LOG_RESULT_NOT_READY Initialization is incomplete.
  */
Coffee2LogResult_e xCoffee2LogWriteFieldOrder(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, uint16_t usOrderId, const char *pcText,
	int32_t lResult, const char *pcFieldName, int32_t lFieldValue);

/** @brief Submit a human-readable Coffee2 log without result fields. */
Coffee2LogResult_e xCoffee2LogWriteTextOrder(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, uint16_t usOrderId, const char *pcText);

/** @brief Submit a bounded formatted human-readable Coffee2 log. */
Coffee2LogResult_e xCoffee2LogPrintfOrder(Coffee2LogLevel_e xLevel,
	Coffee2LogSource_e xSource, uint16_t usOrderId, const char *pcFormat, ...);

/**
  * @brief  Write one startup probe before the scheduler begins.
  * @param[in] pucData Bytes to transmit through the initialized log channel.
  * @param[in] usLength Number of bytes to transmit.
  * @retval 0 Transmission completed.
  * @return Negative normalized Transport result on failure.
  * @note   Call only after xCoffee2LogInit().
  */
int32_t lCoffee2LogEarlyWrite(const uint8_t *pucData, uint16_t usLength);

/**
  * @brief  Run the sole USART1 log output owner task.
  * @param[in] pvArgument Unused task argument.
  * @note   Create only after xCoffee2LogInit(); a transport failure does not
  *         invalidate the ring.
  */
void vCoffee2LogTask(void *pvArgument);

/**
  * @brief  Publish the result of C2Log task creation.
  * @param[in] ucCreated Nonzero when xTaskCreate returned pdPASS.
  * @note   A failed logger task does not stop business task creation.
  */
void vCoffee2LogSetTaskReady(uint8_t ucCreated);

/**
  * @brief  Copy a consistent log status snapshot.
  * @param[out] pxStatus Caller-owned status destination; ignored when NULL.
  */
void vCoffee2LogGetStatus(Coffee2LogStatus_t *pxStatus);

/**
  * @brief  Log LwIP resource statistics after a network API failure.
  * @param[in] xSource Coffee2 subsystem that observed the failure.
  * @param[in] lNativeError Native socket or LwIP error value.
  * @note   This interface never changes the caller's failure result and does
  *         not make logging a prerequisite for normal operation.
  */
void vCoffee2LogLwipResourceFailure(Coffee2LogSource_e xSource,
	int32_t lNativeError);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE2_LOG_H */
