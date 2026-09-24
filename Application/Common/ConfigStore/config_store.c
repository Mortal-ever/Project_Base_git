/**
  * @file      config_store.c
  * @brief     实现内部 Flash 单副本配置区的读写与回读校验。
  * @author    WHong
  * @date      2026-09-24
  *
  * @attention
  * - 调用方负责串行化访问，写操作仅擦除扇区 2。
  * - 配置字 0 是最后写入的格式有效标记。
  */
#include "config_store.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>

/**
  * @brief  从固定配置区读取指定数量的 32 位配置字。
  * @param[out] pulWords 调用方提供的接收数组，不可为空。
  * @param[in] ulCount 读取的配置字数量，范围为 1 至配置区容量。
  * @retval CONFIG_STORE_OK 读取完成。
  * @retval CONFIG_STORE_INVALID 参数或数量超出配置区范围。
  */
ConfigStoreResult_e xConfigStoreRead(uint32_t *pulWords, uint32_t ulCount)
{
	const volatile uint32_t *pulFlash; /*!< 配置区的只读映射地址。 */
	uint32_t ulIndex; /*!< 当前复制的配置字索引。 */

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

/**
  * @brief  擦除配置扇区并按“载荷优先、有效标记最后”写入配置。
  * @param[in] pulWords 待写入的配置字数组，字 0 为格式有效标记。
  * @param[in] ulCount 配置字数量，至少为 2 且不超过配置区容量。
  * @retval CONFIG_STORE_OK 写入、加锁和全量回读校验均成功。
  * @retval CONFIG_STORE_INVALID 参数、数量或有效标记不合法。
  * @retval CONFIG_STORE_WRITE_FAILED Flash 操作或回读校验失败。
  */
ConfigStoreResult_e xConfigStoreWrite(const uint32_t *pulWords,
	uint32_t ulCount)
{
	FLASH_EraseInitTypeDef xErase = {0}; /*!< 扇区擦除参数。 */
	HAL_StatusTypeDef xStatus; /*!< 当前 Flash 操作的 HAL 状态。 */
	uint32_t ulSectorError; /*!< HAL 返回的失败扇区编号。 */
	uint32_t ulIndex; /*!< 当前写入或校验的配置字索引。 */
	const volatile uint32_t *pulFlash; /*!< 配置区的回读映射地址。 */

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
	/* 载荷全部写入成功后再提交有效标记，避免掉电留下伪有效配置。 */
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
