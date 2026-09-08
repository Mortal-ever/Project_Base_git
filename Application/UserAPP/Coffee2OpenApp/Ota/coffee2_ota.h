/**
  * @file      coffee2_ota.h
  * @brief     Define the Coffee2 OTA flash and HTTP owner interface.
  */

#ifndef COFFEE2_OTA_H
#define COFFEE2_OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "FreeRTOS.h"
#include "Ota/app_ota_flash.h"

#define COFFEE2_OTA_METADATA_ADDRESS       0x08004000UL
#define COFFEE2_OTA_STAGING_ADDRESS        0x08060000UL
#define COFFEE2_OTA_STAGING_END            0x080C0000UL
#define COFFEE2_OTA_APPLICATION_ADDRESS    0x0800C000UL
#define COFFEE2_OTA_APPLICATION_END        0x08060000UL
#define COFFEE2_OTA_MAX_IMAGE_SIZE \
	(COFFEE2_OTA_APPLICATION_END - COFFEE2_OTA_APPLICATION_ADDRESS)
#define COFFEE2_OTA_METADATA_MAGIC         0xDEADBEEFUL

typedef AppOtaResult_e Coffee2OtaResult_e;

Coffee2OtaResult_e xCoffee2OtaInitialize(void);
Coffee2OtaResult_e xCoffee2OtaBegin(void);
Coffee2OtaResult_e xCoffee2OtaWrite(const uint8_t *pucData, uint32_t ulLength);
Coffee2OtaResult_e xCoffee2OtaFinish(uint32_t *pulCrc32, uint32_t *pulSize);
void vCoffee2OtaAbort(void);
uint8_t ucCoffee2OtaIsActive(void);
BaseType_t xCoffee2OtaHttpInitialize(void);
uint8_t ucCoffee2OtaHttpIsActive(void);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE2_OTA_H */
