/**
  * @file      coffee2_ota.c
  * @brief     Bind Coffee2 OTA partition constants to the common services.
  */

#include "coffee2_ota.h"

#include "coffee2_log.h"
#include "Ota/app_ota_http.h"
#include "crc.h"

#define COFFEE2_OTA_SRAM_START 0x20000000UL
#define COFFEE2_OTA_SRAM_END   0x20020000UL

static void prvReportLwipFailure(AppLogSourceId_t xSource, int32_t lNativeError)
{
	vCoffee2LogLwipResourceFailure((Coffee2LogSource_e)xSource, lNativeError);
}

static const AppOtaConfig_t s_xCoffee2OtaConfig = {
	COFFEE2_OTA_METADATA_ADDRESS, COFFEE2_OTA_STAGING_ADDRESS,
	COFFEE2_OTA_STAGING_END, COFFEE2_OTA_APPLICATION_ADDRESS,
	COFFEE2_OTA_APPLICATION_END, COFFEE2_OTA_SRAM_START, COFFEE2_OTA_SRAM_END,
	COFFEE2_OTA_METADATA_MAGIC, FLASH_SECTOR_1, FLASH_SECTOR_7, 3U, 80U, 500U,
	(AppLogSourceId_t)COFFEE2_LOG_SOURCE_SYSTEM, &hcrc, prvReportLwipFailure
};

Coffee2OtaResult_e xCoffee2OtaInitialize(void)
{
	return xAppOtaInitialize(&s_xCoffee2OtaConfig);
}

Coffee2OtaResult_e xCoffee2OtaBegin(void)
{
	if (xCoffee2OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashBegin();
}

Coffee2OtaResult_e xCoffee2OtaWrite(const uint8_t *pucData, uint32_t ulLength)
{
	if (xCoffee2OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashWrite(pucData, ulLength);
}

Coffee2OtaResult_e xCoffee2OtaFinish(uint32_t *pulCrc32, uint32_t *pulSize)
{
	if (xCoffee2OtaInitialize() != APP_OTA_RESULT_OK) return APP_OTA_RESULT_BUSY;
	return xAppOtaFlashFinish(pulCrc32, pulSize);
}

void vCoffee2OtaAbort(void)
{
	(void)xCoffee2OtaInitialize();
	vAppOtaFlashAbort();
}

uint8_t ucCoffee2OtaIsActive(void)
{
	return ucAppOtaFlashIsActive();
}

BaseType_t xCoffee2OtaHttpInitialize(void)
{
	if (xCoffee2OtaInitialize() != APP_OTA_RESULT_OK) return pdFAIL;
	return xAppOtaHttpInitialize();
}

uint8_t ucCoffee2OtaHttpIsActive(void)
{
	return ucAppOtaHttpIsActive();
}
