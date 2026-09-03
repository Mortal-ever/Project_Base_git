/**
  * @file      app_ota_flash.h
  * @brief     Define the product-neutral staged firmware flash writer.
  * @author    WHong
  * @date      2026-08-25
  *
  * @details   The caller supplies fixed target partition and diagnostic
  *            bindings. This module owns no RTOS object and stores only one
  *            streaming word tail for the active upload.
  */

#ifndef APP_OTA_FLASH_H
#define APP_OTA_FLASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "FreeRTOS.h"
#include "Log/app_log.h"

/** @brief Report staged firmware writer outcomes. */
typedef enum {
	APP_OTA_RESULT_OK = 0,
	APP_OTA_RESULT_INVALID_ARG = 1,
	APP_OTA_RESULT_BUSY = 2,
	APP_OTA_RESULT_REJECTED = 3,
	APP_OTA_RESULT_HAL_ERROR = 4,
	APP_OTA_RESULT_INVALID_IMAGE = 5,
	APP_OTA_RESULT_CRC_ERROR = 6
} AppOtaResult_e;

/** @brief Report an lwIP resource failure through a target diagnostic port. */
typedef void (*AppOtaLwipFailureHook_t)(AppLogSourceId_t xSource,
	int32_t lNativeError);

/**
  * @brief  Describe one target's immutable OTA partition and service binding.
  * @note   All addresses ending in End are exclusive limits.
  */
typedef struct {
	uint32_t ulMetadataAddress; /*!< Boot metadata record base address. */
	uint32_t ulStagingAddress; /*!< First byte of the incoming image slot. */
	uint32_t ulStagingEnd; /*!< Exclusive end of the incoming image slot. */
	uint32_t ulApplicationAddress; /*!< Bootloader application copy base. */
	uint32_t ulApplicationEnd; /*!< Exclusive application copy limit. */
	uint32_t ulSramStart; /*!< Lowest valid main stack pointer address. */
	uint32_t ulSramEnd; /*!< Exclusive valid main stack pointer limit. */
	uint32_t ulMetadataMagic; /*!< First metadata word consumed by bootloader. */
	uint32_t ulMetadataSector; /*!< HAL sector number containing metadata. */
	uint32_t ulStagingFirstSector; /*!< HAL first erase sector of staging. */
	uint32_t ulStagingSectorCount; /*!< Number of staging sectors to erase. */
	uint16_t usHttpPort; /*!< Raw-lwIP HTTP listener port. */
	uint16_t usResetDelayMs; /*!< Reset delay after success response begins. */
	AppLogSourceId_t xLogSource; /*!< Target log source for OTA records. */
	void *pvCrc; /*!< Initialized target HAL CRC handle. */
	AppOtaLwipFailureHook_t pxLwipFailureHook;
		/*!< Optional target-specific lwIP resource diagnostic hook. */
} AppOtaConfig_t;

/**
  * @brief  Bind the immutable OTA configuration used by the common service.
  * @param[in] pxConfig Target-owned static configuration.
  * @retval APP_OTA_RESULT_OK Configuration is available.
  * @retval APP_OTA_RESULT_INVALID_ARG Configuration is incomplete.
  * @warning Call only before the first upload and from task context.
  */
AppOtaResult_e xAppOtaInitialize(const AppOtaConfig_t *pxConfig);

/**
  * @brief  Begin one staged image and erase the configured staging sectors.
  * @retval APP_OTA_RESULT_OK Erase completed and the session is active.
  * @retval APP_OTA_RESULT_BUSY Another upload owns the flash writer.
  * @retval APP_OTA_RESULT_HAL_ERROR Flash unlock or erase failed.
  */
AppOtaResult_e xAppOtaFlashBegin(void);

/**
  * @brief  Append image bytes using aligned word programming and readback.
  * @param[in] pucData Source bytes owned by the caller for this call only.
  * @param[in] ulLength Number of source bytes.
  * @retval APP_OTA_RESULT_OK Bytes were accepted.
  * @retval APP_OTA_RESULT_INVALID_ARG Session, pointer, or length was invalid.
  * @retval APP_OTA_RESULT_HAL_ERROR A write or readback failed.
  */
AppOtaResult_e xAppOtaFlashWrite(const uint8_t *pucData,
	uint32_t ulLength);

/**
  * @brief  Validate the image and commit bootloader metadata.
  * @param[out] pulCrc32 Optional destination for the calculated CRC.
  * @param[out] pulSize Optional destination for exact image bytes.
  * @retval APP_OTA_RESULT_OK Image and metadata were committed.
  * @retval APP_OTA_RESULT_INVALID_IMAGE Vector data was invalid.
  * @retval APP_OTA_RESULT_CRC_ERROR Verification changed unexpectedly.
  * @retval APP_OTA_RESULT_HAL_ERROR Metadata write failed.
  */
AppOtaResult_e xAppOtaFlashFinish(uint32_t *pulCrc32, uint32_t *pulSize);

/** @brief Abort the active session, lock Flash, and leave metadata untouched. */
void vAppOtaFlashAbort(void);

/** @brief Return nonzero while a staged image owns the flash writer. */
uint8_t ucAppOtaFlashIsActive(void);

/** @brief Return the immutable configuration after successful initialization. */
const AppOtaConfig_t *pxAppOtaGetConfig(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_OTA_FLASH_H */
