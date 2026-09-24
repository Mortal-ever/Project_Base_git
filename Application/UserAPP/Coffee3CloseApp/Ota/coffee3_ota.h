/**
  * @file      coffee3_ota.h
  * @brief     定义 Coffee3 OTA Flash 与 HTTP 服务接口。
  * @author    WHong
  * @date      2026-09-24
  */

#ifndef COFFEE3_OTA_H
#define COFFEE3_OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "FreeRTOS.h"
#include "Ota/app_ota_flash.h"

/** @brief Coffee3 OTA 元数据、暂存区与应用区 Flash 边界。 */
#define COFFEE3_OTA_METADATA_ADDRESS       0x08004000UL
#define COFFEE3_OTA_STAGING_ADDRESS        0x08060000UL
#define COFFEE3_OTA_STAGING_END            0x080C0000UL
#define COFFEE3_OTA_APPLICATION_ADDRESS    0x0800C000UL
#define COFFEE3_OTA_APPLICATION_END        0x08060000UL
#define COFFEE3_OTA_MAX_IMAGE_SIZE \
	(COFFEE3_OTA_APPLICATION_END - COFFEE3_OTA_APPLICATION_ADDRESS)
#define COFFEE3_OTA_METADATA_MAGIC         0xDEADBEEFUL

/** @brief 复用公共 OTA 服务的结果枚举。 */
typedef AppOtaResult_e Coffee3OtaResult_e;

Coffee3OtaResult_e xCoffee3OtaInitialize(void);
Coffee3OtaResult_e xCoffee3OtaBegin(void);
Coffee3OtaResult_e xCoffee3OtaWrite(const uint8_t *pucData, uint32_t ulLength);
Coffee3OtaResult_e xCoffee3OtaFinish(uint32_t *pulCrc32, uint32_t *pulSize);
void vCoffee3OtaAbort(void);
uint8_t ucCoffee3OtaIsActive(void);
BaseType_t xCoffee3OtaHttpInitialize(void);
uint8_t ucCoffee3OtaHttpIsActive(void);

#ifdef __cplusplus
}
#endif

#endif /* COFFEE3_OTA_H */
