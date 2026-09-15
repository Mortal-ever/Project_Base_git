/* Single-copy configuration storage. Never erase outside sector two. */
#include "config_store.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>

ConfigStoreResult_e xConfigStoreRead(uint32_t *pulWords, uint32_t ulCount)
{
	const volatile uint32_t *pulFlash;
	uint32_t ulIndex;

	if ((pulWords == NULL) || (ulCount == 0U) ||
		(ulCount > CONFIG_STORE_SIZE / 4U)) {
		return CONFIG_STORE_INVALID;
	}
	pulFlash = (const volatile uint32_t *)CONFIG_STORE_ADDRESS;
	for (ulIndex = 0U; ulIndex < ulCount; ulIndex++) {
		pulWords[ulIndex] = pulFlash[ulIndex];
	}
	return CONFIG_STORE_OK;
}

ConfigStoreResult_e xConfigStoreWrite(const uint32_t *pulWords,
	uint32_t ulCount)
{
	FLASH_EraseInitTypeDef xErase = {0};
	HAL_StatusTypeDef xStatus;
	uint32_t ulSectorError;
	uint32_t ulIndex;
	const volatile uint32_t *pulFlash;

	if ((pulWords == NULL) || (ulCount < 2U) ||
		(ulCount > CONFIG_STORE_SIZE / 4U) ||
		(pulWords[0] == 0xFFFFFFFFUL)) {
		return CONFIG_STORE_INVALID;
	}
	xStatus = HAL_FLASH_Unlock();
	if (xStatus != HAL_OK) {
		return CONFIG_STORE_WRITE_FAILED;
	}
	xErase.TypeErase = FLASH_TYPEERASE_SECTORS;
	xErase.Sector = FLASH_SECTOR_2;
	xErase.NbSectors = 1U;
	xErase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
	xStatus = HAL_FLASHEx_Erase(&xErase, &ulSectorError);
	pulFlash = (const volatile uint32_t *)CONFIG_STORE_ADDRESS;
	for (ulIndex = 1U; (ulIndex < ulCount) &&
		(xStatus == HAL_OK); ulIndex++) {
		xStatus = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
			CONFIG_STORE_ADDRESS + ulIndex * 4U, pulWords[ulIndex]);
		if ((xStatus == HAL_OK) && (pulFlash[ulIndex] != pulWords[ulIndex])) {
			xStatus = HAL_ERROR;
		}
	}
	/* Mark valid only after the payload has been programmed. */
	if (xStatus == HAL_OK) {
		xStatus = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
			CONFIG_STORE_ADDRESS, pulWords[0]);
	}
	if (HAL_FLASH_Lock() != HAL_OK) {
		xStatus = HAL_ERROR;
	}
	pulFlash = (const volatile uint32_t *)CONFIG_STORE_ADDRESS;
	for (ulIndex = 0U; (ulIndex < ulCount) &&
		(xStatus == HAL_OK); ulIndex++) {
		if (pulFlash[ulIndex] != pulWords[ulIndex]) {
			xStatus = HAL_ERROR;
		}
	}
	return (xStatus == HAL_OK) ? CONFIG_STORE_OK :
		CONFIG_STORE_WRITE_FAILED;
}
