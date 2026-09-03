/**
  * @file      app_log.h
  * @brief     Define the product-neutral asynchronous application log core.
  * @author    WHong
  * @date      2026-08-20
  *
  * @details   Producers submit bounded structured records to one static
  *            overwrite-oldest ring. A caller-owned Transport channel is
  *            used by the single consumer task for output.
  *
  * @attention
  * - The write API is task-context only and never allocates memory.
  * - Product-specific source identifiers and Transport setup belong to the
  *   Target adapter, not to this core.
  */

#ifndef APP_LOG_H
#define APP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "transport.h"

/** @brief Define application log service results. */
typedef enum {
	APP_LOG_RESULT_OK = 0,
		/*!< The operation completed successfully. */
	APP_LOG_RESULT_ALREADY_INITIALIZED = 1,
		/*!< The log service was already initialized. */
	APP_LOG_RESULT_INVALID_ARG = -1,
		/*!< A pointer, level, or source was invalid. */
	APP_LOG_RESULT_NOT_READY = -2,
		/*!< The ring or signal is not available. */
	APP_LOG_RESULT_QUEUE_FULL = -3,
		/*!< Retained for adapters that expose the legacy result contract. */
	APP_LOG_RESULT_TRANSPORT = -4
		/*!< The Transport output is unavailable. */
} AppLogResult_e;

/** @brief Define log severity without a debug level. */
typedef enum {
	APP_LOG_LEVEL_INFO = 0,
	APP_LOG_LEVEL_WARNING = 1,
	APP_LOG_LEVEL_ERROR = 2,
	APP_LOG_LEVEL_COUNT = 3
} AppLogLevel_e;

/** @brief Identify one product-defined application log source. */
typedef uint8_t AppLogSourceId_t;

/** @brief Identify the Host order associated with one log record. */
typedef uint16_t AppLogOrderId_t;

/** @brief Describe the stable text labels for one log source. */
typedef struct {
	const char *pcTaskName; /*!< Task label written in the log prefix. */
	const char *pcModuleName; /*!< Module label written in the log prefix. */
} AppLogSourceDescriptor_t;

/** @brief Bind source labels and one optional output Transport channel. */
typedef struct {
	const AppLogSourceDescriptor_t *pxSourceTable;
		/*!< Product-owned source table stored in read-only memory. */
	uint8_t ucSourceCount;
		/*!< Number of valid entries in pxSourceTable. */
	TransportChannel_t *pxTransportChannel;
		/*!< Product-owned channel and context, or NULL for buffer-only mode. */
	uint8_t ucEnableTransport;
		/*!< Nonzero requests Transport open during initialization. */
} AppLogConfig_t;

/** @brief Store observable log counters for product monitoring. */
typedef struct {
	uint32_t ulQueuedCount; /*!< Records accepted by the ring. */
	uint32_t ulSentCount; /*!< Records transmitted successfully. */
	uint32_t ulDroppedCount; /*!< Oldest records overwritten by new writes. */
	uint32_t ulTransmitFailureCount; /*!< Transport transmission failures. */
	int32_t lLastTransportError; /*!< Latest normalized Transport result. */
	uint32_t ulRetryCount; /*!< Bounded output retries after send failures. */
	uint16_t usPendingCount; /*!< Records currently waiting in the ring. */
	uint16_t usQueueHighWatermark;
		/*!< Maximum observed pending record count. */
	uint8_t ucInitialized; /*!< Nonzero after the ring and signal are ready. */
	uint8_t ucBufferReady; /*!< Static ring storage is available. */
	uint8_t ucTransportReady; /*!< Transport can currently send. */
	uint8_t ucTaskReady; /*!< The product log task entered its loop. */
	uint8_t ucOutputPaused; /*!< Output is paused while Transport is retried. */
} AppLogStatus_t;

/** @brief Global public status stored by the one application log instance. */
extern AppLogStatus_t g_xAppLogStatus;

/**
  * @brief  Initialize the static ring, signal, and optional Transport.
  * @param[in]  pxConfig Product source table and Transport binding.
  * @retval APP_LOG_RESULT_OK The ring and requested Transport are ready.
  * @retval APP_LOG_RESULT_ALREADY_INITIALIZED Initialization ran before.
  * @retval APP_LOG_RESULT_INVALID_ARG A source table or count was invalid.
  * @retval APP_LOG_RESULT_NOT_READY Static ring or signal creation failed.
  * @retval APP_LOG_RESULT_TRANSPORT The ring is ready but Transport output
  *         is disabled or could not be opened.
  * @note   Call after the Transport manager has been initialized.
  */
AppLogResult_e xAppLogInit(const AppLogConfig_t *pxConfig);

/**
  * @brief  Submit one bounded record without blocking the caller.
  * @param[in]  xLevel Record severity.
  * @param[in]  xSource Product-defined source identifier.
  * @param[in]  pcText Null-terminated event text copied into the record.
  * @param[in]  lCode Source-specific result, error, or diagnostic code.
  * @retval APP_LOG_RESULT_OK The ring accepted the record, overwriting its
  *         oldest record when full.
  * @retval APP_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval APP_LOG_RESULT_NOT_READY Initialization is incomplete.
  * @warning Do not call this interface from an interrupt.
  */
AppLogResult_e xAppLogWrite(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, const char *pcText, int32_t lCode);

/**
  * @brief  Submit one bounded record with an explicit order identifier.
  * @param[in] xLevel Record severity.
  * @param[in] xSource Product-defined source identifier.
  * @param[in] usOrderId Host order, debug order, or system order identifier.
  * @param[in] pcText Null-terminated event text copied into the record.
  * @param[in] lCode Source-specific result, error, or diagnostic code.
  * @retval APP_LOG_RESULT_OK The ring accepted the record.
  * @retval APP_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval APP_LOG_RESULT_NOT_READY Initialization is incomplete.
  * @warning Do not call this interface from an interrupt.
  */
AppLogResult_e xAppLogWriteOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText, int32_t lCode);

/**
  * @brief  Submit one record with one named diagnostic field.
  * @param[in]  xLevel Record severity.
  * @param[in]  xSource Product-defined source identifier.
  * @param[in]  pcText Null-terminated event text copied into the record.
  * @param[in]  lResult Normalized operation or state result.
  * @param[in]  pcFieldName Optional field name, or NULL for no field.
  * @param[in]  lFieldValue Field value when pcFieldName is not NULL.
  * @retval APP_LOG_RESULT_OK The ring accepted the record, overwriting its
  *         oldest record when full.
  * @retval APP_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval APP_LOG_RESULT_NOT_READY Initialization is incomplete.
  * @warning Do not call this interface from an interrupt.
  */
AppLogResult_e xAppLogWriteField(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, const char *pcText, int32_t lResult,
	const char *pcFieldName, int32_t lFieldValue);

/**
  * @brief  Submit one named record with an explicit order identifier.
  * @param[in] xLevel Record severity.
  * @param[in] xSource Product-defined source identifier.
  * @param[in] usOrderId Host order, debug order, or system order identifier.
  * @param[in] pcText Null-terminated event text copied into the record.
  * @param[in] lResult Normalized operation or state result.
  * @param[in] pcFieldName Optional field name, or NULL for no field.
  * @param[in] lFieldValue Field value when pcFieldName is not NULL.
  * @retval APP_LOG_RESULT_OK The ring accepted the record.
  * @retval APP_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval APP_LOG_RESULT_NOT_READY Initialization is incomplete.
  * @warning Do not call this interface from an interrupt.
  */
AppLogResult_e xAppLogWriteFieldOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText, int32_t lResult, const char *pcFieldName,
	int32_t lFieldValue);

/**
  * @brief  Submit one human-readable record without a result suffix.
  * @param[in] xLevel Record severity.
  * @param[in] xSource Product-defined source identifier.
  * @param[in] usOrderId Host order, debug order, or system order.
  * @param[in] pcText Null-terminated text copied into the record.
  * @retval APP_LOG_RESULT_OK The ring accepted the record.
  * @retval APP_LOG_RESULT_INVALID_ARG A parameter was invalid.
  * @retval APP_LOG_RESULT_NOT_READY Initialization is incomplete.
  */
AppLogResult_e xAppLogWriteTextOrder(AppLogLevel_e xLevel,
	AppLogSourceId_t xSource, AppLogOrderId_t usOrderId,
	const char *pcText);

/**
  * @brief  Write one bounded startup probe through the configured channel.
  * @param[in]  pucData Bytes to transmit.
  * @param[in]  usLength Number of bytes to transmit.
  * @retval 0 Transmission completed.
  * @return Negative normalized Transport result on failure.
  * @note   Call only after xAppLogInit().
  */
int32_t lAppLogEarlyWrite(const uint8_t *pucData, uint16_t usLength);

/**
  * @brief  Run the sole application log output owner task.
  * @param[in]  pvArgument Unused task argument.
  * @note   The product adapter owns task creation and naming.
  */
void vAppLogTask(void *pvArgument);

/**
  * @brief  Publish the result of product log task creation.
  * @param[in]  ucCreated Nonzero when task creation returned pdPASS.
  */
void vAppLogSetTaskReady(uint8_t ucCreated);

/**
  * @brief  Copy a consistent application log status snapshot.
  * @param[out] pxStatus Caller-owned status destination; ignored when NULL.
  */
void vAppLogGetStatus(AppLogStatus_t *pxStatus);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H */
