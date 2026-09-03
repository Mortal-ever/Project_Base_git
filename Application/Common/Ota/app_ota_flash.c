/**
  * @file      app_ota_flash.c
  * @brief     Stage, verify, and commit a target firmware image.
  * @author    WHong
  * @date      2026-08-25
  *
  * @details   This module streams the incoming image to Flash and commits
  *            metadata only after vector and CRC verification. It has no
  *            product-specific symbols or dynamic storage.
  */

#include "Ota/app_ota_flash.h"

#include <stddef.h>
#include <string.h>

#include "main.h"
#include "task.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_flash_ex.h"

/** @brief Initial stack pointer must be word aligned. */
#define APP_OTA_STACK_ALIGNMENT_MASK       0x00000003UL

/** @brief Four-byte carry buffer for packets with arbitrary alignment. */
static uint8_t s_aucTail[4];
/** @brief Number of valid bytes currently held in s_aucTail. */
static uint8_t s_ucTailLength;
/** @brief Next staging address to program. */
static uint32_t s_ulFlashAddress;
/** @brief Exact unpadded image byte count. */
static uint32_t s_ulReceived;
/** @brief One active session flag shared with the target server. */
static volatile uint8_t s_ucActive;
/** @brief Immutable target binding retained for the service lifetime. */
static const AppOtaConfig_t *s_pxConfig;

static AppOtaResult_e prvProgramWord(uint32_t ulAddress, uint32_t ulWord);
static AppOtaResult_e prvEraseStaging(void);
static AppOtaResult_e prvFlushTail(void);
static uint8_t prvValidateVector(void);
static uint32_t prvCalculateCrc(uint32_t ulSize);
static void prvAbortWithLog(const char *pcText, int32_t lCode);
static void prvLockFlash(void);
static void prvWriteLog(AppLogLevel_e xLevel, const char *pcText,
	int32_t lCode);

/*-----------------------------------------------------------*/
static void prvWriteLog(AppLogLevel_e xLevel, const char *pcText,
	int32_t lCode)
{
	if (s_pxConfig != NULL) {
		(void)xAppLogWrite(xLevel, s_pxConfig->xLogSource, pcText, lCode);
	}
}

/*-----------------------------------------------------------*/
static AppOtaResult_e prvProgramWord(uint32_t ulAddress, uint32_t ulWord)
{
	HAL_StatusTypeDef xStatus;
	volatile uint32_t *pulFlash;
	uint8_t ucStagingAddress;
	uint8_t ucMetadataAddress;

	ucStagingAddress = (uint8_t)((ulAddress >= s_pxConfig->ulStagingAddress) &&
		(ulAddress <= (s_pxConfig->ulStagingEnd - 4U)));
	ucMetadataAddress = (uint8_t)((ulAddress >=
		s_pxConfig->ulMetadataAddress) &&
		(ulAddress <= (s_pxConfig->ulMetadataAddress + 12U)));
	if ((ucStagingAddress == 0U) && (ucMetadataAddress == 0U)) {
		return APP_OTA_RESULT_INVALID_ARG;
	}
	xStatus = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, ulAddress, ulWord);
	if (xStatus != HAL_OK) {
		return APP_OTA_RESULT_HAL_ERROR;
	}
	pulFlash = (volatile uint32_t *)ulAddress;
	if (*pulFlash != ulWord) {
		return APP_OTA_RESULT_HAL_ERROR;
	}
	return APP_OTA_RESULT_OK;
}

/*-----------------------------------------------------------*/
static AppOtaResult_e prvEraseStaging(void)
{
	FLASH_EraseInitTypeDef xErase;
	uint32_t ulSectorError;

	memset(&xErase, 0, sizeof(xErase));
	ulSectorError = 0U;
	xErase.TypeErase = FLASH_TYPEERASE_SECTORS;
	xErase.Sector = s_pxConfig->ulStagingFirstSector;
	xErase.NbSectors = s_pxConfig->ulStagingSectorCount;
	xErase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
	if (HAL_FLASHEx_Erase(&xErase, &ulSectorError) != HAL_OK) {
		return APP_OTA_RESULT_HAL_ERROR;
	}
	if (ulSectorError != 0xFFFFFFFFUL) {
		return APP_OTA_RESULT_HAL_ERROR;
	}
	return APP_OTA_RESULT_OK;
}

/*-----------------------------------------------------------*/
static AppOtaResult_e prvFlushTail(void)
{
	uint32_t ulWord;
	AppOtaResult_e xResult;

	if (s_ucTailLength == 0U) {
		return APP_OTA_RESULT_OK;
	}
	while (s_ucTailLength < 4U) {
		s_aucTail[s_ucTailLength] = 0xFFU;
		s_ucTailLength++;
	}
	memcpy(&ulWord, s_aucTail, sizeof(ulWord));
	xResult = prvProgramWord(s_ulFlashAddress, ulWord);
	if (xResult == APP_OTA_RESULT_OK) {
		s_ulFlashAddress += 4U;
	}
	s_ucTailLength = 0U;
	return xResult;
}

/*-----------------------------------------------------------*/
static uint8_t prvValidateVector(void)
{
	volatile const uint32_t *pulVector;
	uint32_t ulStack;
	uint32_t ulReset;
	uint32_t ulResetAddress;

	pulVector = (volatile const uint32_t *)s_pxConfig->ulStagingAddress;
	ulStack = pulVector[0];
	ulReset = pulVector[1];
	ulResetAddress = ulReset & ~1UL;
	if ((ulStack < s_pxConfig->ulSramStart) ||
		(ulStack >= s_pxConfig->ulSramEnd) ||
		((ulStack & 0x2FFE0000UL) != s_pxConfig->ulSramStart) ||
		((ulStack & APP_OTA_STACK_ALIGNMENT_MASK) != 0U)) {
		return 0U;
	}
	if ((ulReset & 1U) == 0U) {
		return 0U;
	}
	if ((ulResetAddress < (s_pxConfig->ulApplicationAddress + 4U)) ||
		(ulResetAddress >= s_pxConfig->ulApplicationEnd)) {
		return 0U;
	}
	return 1U;
}

/*-----------------------------------------------------------*/
static uint32_t prvCalculateCrc(uint32_t ulSize)
{
	uint32_t ulWordCount;

	ulWordCount = (ulSize + 3U) / 4U;
	return HAL_CRC_Calculate((CRC_HandleTypeDef *)s_pxConfig->pvCrc,
		(uint32_t *)s_pxConfig->ulStagingAddress, ulWordCount);
}

/*-----------------------------------------------------------*/
static void prvAbortWithLog(const char *pcText, int32_t lCode)
{
	prvLockFlash();
	s_ucTailLength = 0U;
	s_ulFlashAddress = s_pxConfig->ulStagingAddress;
	s_ulReceived = 0U;
	s_ucActive = 0U;
	prvWriteLog(APP_LOG_LEVEL_ERROR, pcText, lCode);
}

/*-----------------------------------------------------------*/
static void prvLockFlash(void)
{
	HAL_StatusTypeDef xStatus;

	xStatus = HAL_FLASH_Lock();
	if (xStatus != HAL_OK) {
		prvWriteLog(APP_LOG_LEVEL_ERROR, "OTA_HTTP_FLASH_FAILED",
			(int32_t)xStatus);
	}
}

/*-----------------------------------------------------------*/
AppOtaResult_e xAppOtaInitialize(const AppOtaConfig_t *pxConfig)
{
	if ((pxConfig == NULL) || (pxConfig->pvCrc == NULL) ||
		(pxConfig->ulMetadataAddress == 0U) ||
		(pxConfig->ulStagingAddress >= pxConfig->ulStagingEnd) ||
		(pxConfig->ulApplicationAddress >= pxConfig->ulApplicationEnd) ||
		(pxConfig->ulSramStart >= pxConfig->ulSramEnd) ||
		(pxConfig->ulStagingSectorCount == 0U) ||
		(pxConfig->usHttpPort == 0U)) {
		return APP_OTA_RESULT_INVALID_ARG;
	}
	if ((s_pxConfig != NULL) && (s_pxConfig != pxConfig)) {
		return APP_OTA_RESULT_BUSY;
	}
	s_pxConfig = pxConfig;
	return APP_OTA_RESULT_OK;
}

/*-----------------------------------------------------------*/
AppOtaResult_e xAppOtaFlashBegin(void)
{
	AppOtaResult_e xResult;

	if (s_pxConfig == NULL) {
		return APP_OTA_RESULT_INVALID_ARG;
	}
	taskENTER_CRITICAL();
	if (s_ucActive != 0U) {
		taskEXIT_CRITICAL();
		return APP_OTA_RESULT_BUSY;
	}
	s_ucActive = 1U;
	taskEXIT_CRITICAL();

	if (HAL_FLASH_Unlock() != HAL_OK) {
		prvAbortWithLog("OTA_HTTP_FLASH_FAILED", -8);
		return APP_OTA_RESULT_HAL_ERROR;
	}
	xResult = prvEraseStaging();
	if (xResult != APP_OTA_RESULT_OK) {
		prvAbortWithLog("OTA_HTTP_FLASH_FAILED", (int32_t)xResult);
		return xResult;
	}
	s_ulFlashAddress = s_pxConfig->ulStagingAddress;
	s_ulReceived = 0U;
	s_ucTailLength = 0U;
	s_ucActive = 1U;
	prvWriteLog(APP_LOG_LEVEL_INFO, "OTA_HTTP_BEGIN", 0);
	return APP_OTA_RESULT_OK;
}

/*-----------------------------------------------------------*/
AppOtaResult_e xAppOtaFlashWrite(const uint8_t *pucData, uint32_t ulLength)
{
	uint32_t ulIndex;
	uint32_t ulWord;
	AppOtaResult_e xResult;
	uint32_t ulMaximum;

	if ((s_pxConfig == NULL) || (s_ucActive == 0U) ||
		((pucData == NULL) && (ulLength != 0U))) {
		return APP_OTA_RESULT_INVALID_ARG;
	}
	ulMaximum = s_pxConfig->ulApplicationEnd -
		s_pxConfig->ulApplicationAddress;
	if (ulLength > (ulMaximum - s_ulReceived)) {
		prvAbortWithLog("OTA_HTTP_FLASH_FAILED", -4);
		return APP_OTA_RESULT_INVALID_ARG;
	}
	for (ulIndex = 0U; ulIndex < ulLength; ulIndex++) {
		s_aucTail[s_ucTailLength] = pucData[ulIndex];
		s_ucTailLength++;
		if (s_ucTailLength == 4U) {
			memcpy(&ulWord, s_aucTail, sizeof(ulWord));
			xResult = prvProgramWord(s_ulFlashAddress, ulWord);
			if (xResult != APP_OTA_RESULT_OK) {
				prvAbortWithLog("OTA_HTTP_FLASH_FAILED", (int32_t)xResult);
				return xResult;
			}
			s_ulFlashAddress += 4U;
			s_ucTailLength = 0U;
		}
	}
	s_ulReceived += ulLength;
	return APP_OTA_RESULT_OK;
}

/*-----------------------------------------------------------*/
AppOtaResult_e xAppOtaFlashFinish(uint32_t *pulCrc32, uint32_t *pulSize)
{
	AppOtaResult_e xResult;
	uint32_t ulCrc;
	uint32_t ulSectorError;
	uint32_t aulMetadata[4];
	FLASH_EraseInitTypeDef xErase;
	uint8_t ucIndex;
	uint32_t ulMaximum;

	if (s_pxConfig == NULL) {
		return APP_OTA_RESULT_INVALID_ARG;
	}
	ulMaximum = s_pxConfig->ulApplicationEnd -
		s_pxConfig->ulApplicationAddress;
	if ((s_ucActive == 0U) || (s_ulReceived == 0U) ||
		(s_ulReceived > ulMaximum)) {
		return APP_OTA_RESULT_INVALID_ARG;
	}
	xResult = prvFlushTail();
	if (xResult != APP_OTA_RESULT_OK) {
		prvAbortWithLog("OTA_HTTP_FLASH_FAILED", (int32_t)xResult);
		return xResult;
	}
	if (prvValidateVector() == 0U) {
		prvAbortWithLog("OTA_HTTP_CRC_FAILED", -5);
		return APP_OTA_RESULT_INVALID_IMAGE;
	}
	ulCrc = prvCalculateCrc(s_ulReceived);
	if (prvCalculateCrc(s_ulReceived) != ulCrc) {
		prvAbortWithLog("OTA_HTTP_CRC_FAILED", -6);
		return APP_OTA_RESULT_CRC_ERROR;
	}
	(void)xAppLogWriteField(APP_LOG_LEVEL_INFO, s_pxConfig->xLogSource,
		"OTA_HTTP_CRC", 0, "crc32", (int32_t)ulCrc);

	aulMetadata[0] = s_pxConfig->ulMetadataMagic;
	aulMetadata[1] = 1U;
	aulMetadata[2] = s_ulReceived;
	aulMetadata[3] = ulCrc;
	memset(&xErase, 0, sizeof(xErase));
	xErase.TypeErase = FLASH_TYPEERASE_SECTORS;
	xErase.Sector = s_pxConfig->ulMetadataSector;
	xErase.NbSectors = 1U;
	xErase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
	ulSectorError = 0U;
	if (HAL_FLASHEx_Erase(&xErase, &ulSectorError) != HAL_OK) {
		prvAbortWithLog("OTA_HTTP_FLASH_FAILED", -7);
		return APP_OTA_RESULT_HAL_ERROR;
	}
	if (ulSectorError != 0xFFFFFFFFUL) {
		prvAbortWithLog("OTA_HTTP_FLASH_FAILED", -7);
		return APP_OTA_RESULT_HAL_ERROR;
	}
	for (ucIndex = 0U; ucIndex < 4U; ucIndex++) {
		xResult = prvProgramWord(s_pxConfig->ulMetadataAddress +
			((uint32_t)ucIndex * 4U), aulMetadata[ucIndex]);
		if (xResult != APP_OTA_RESULT_OK) {
			prvAbortWithLog("OTA_HTTP_FLASH_FAILED", (int32_t)xResult);
			return xResult;
		}
	}
	prvLockFlash();
	if (pulCrc32 != NULL) {
		*pulCrc32 = ulCrc;
	}
	if (pulSize != NULL) {
		*pulSize = s_ulReceived;
	}
	s_ucTailLength = 0U;
	s_ulFlashAddress = s_pxConfig->ulStagingAddress;
	s_ulReceived = 0U;
	s_ucActive = 0U;
	prvWriteLog(APP_LOG_LEVEL_INFO, "OTA_HTTP_COMMIT", 0);
	return APP_OTA_RESULT_OK;
}

/*-----------------------------------------------------------*/
void vAppOtaFlashAbort(void)
{
	if ((s_pxConfig != NULL) && (s_ucActive != 0U)) {
		prvAbortWithLog("OTA_HTTP_ABORT", 0);
	} else {
		prvLockFlash();
	}
}

/*-----------------------------------------------------------*/
uint8_t ucAppOtaFlashIsActive(void)
{
	return s_ucActive;
}

/*-----------------------------------------------------------*/
const AppOtaConfig_t *pxAppOtaGetConfig(void)
{
	return s_pxConfig;
}
