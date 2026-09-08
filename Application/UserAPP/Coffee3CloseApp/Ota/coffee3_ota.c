/**
  * @file      coffee3_ota.c
  * @brief     Bind Coffee3 OTA partition constants to the common services.
  */

#include "coffee3_ota.h"

#include "coffee3_log.h"
#include "Ota/app_ota_http.h"
#include "crc.h"

#define COFFEE3_OTA_SRAM_START 0x20000000UL
#define COFFEE3_OTA_SRAM_END   0x20020000UL

static void prvReportLwipFailure(AppLogSourceId_t xSource, int32_t lNativeError)
{
	vCoffee3LogLwipResourceFailure((Coffee3LogSource_e)xSource, lNativeError);
}

static const AppOtaConfig_t s_xCoffee3OtaConfig = {
	COFFEE3_OTA_METADATA_ADDRESS, COFFEE3_OTA_STAGING_ADDRESS,
	COFFEE3_OTA_STAGING_END, COFFEE3_OTA_APPLICATION_ADDRESS,
	COFFEE3_OTA_APPLICATION_END, COFFEE3_OTA_SRAM_START, COFFEE3_OTA_SRAM_END,
	COFFEE3_OTA_METADATA_MAGIC, FLASH_SECTOR_1, FLASH_SECTOR_7, 3U, 80U, 500U,
	(AppLogSourceId_t)COFFEE3_LOG_SOURCE_SYSTEM, &hcrc, prvReportLwipFailure
};

Coffee3OtaResult_e xCoffee3OtaInitialize(void)
{
	return xAppOtaInitialize(&s_xCoffee3OtaConfig);
}

Coffee3OtaResult_e xCoffee3OtaBegin(void)
{
	if (xCoffee3OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashBegin();
}

Coffee3OtaResult_e xCoffee3OtaWrite(const uint8_t *pucData, uint32_t ulLength)
{
	if (xCoffee3OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashWrite(pucData, ulLength);
}

Coffee3OtaResult_e xCoffee3OtaFinish(uint32_t *pulCrc32, uint32_t *pulSize)
{
	if (xCoffee3OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashFinish(pulCrc32, pulSize);
}

void vCoffee3OtaAbort(void)
{
	(void)xCoffee3OtaInitialize();
	vAppOtaFlashAbort();
}

uint8_t ucCoffee3OtaIsActive(void)
{
	return ucAppOtaFlashIsActive();
}

BaseType_t xCoffee3OtaHttpInitialize(void)
{
	if (xCoffee3OtaInitialize() != APP_OTA_RESULT_OK) return pdFAIL;
	return xAppOtaHttpInitialize();
}

uint8_t ucCoffee3OtaHttpIsActive(void)
{
	return ucAppOtaHttpIsActive();
}
