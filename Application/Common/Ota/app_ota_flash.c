/**
  * @file      app_ota_flash.c
  * @brief     暂存、校验并提交产品目标固件镜像。
  * @author    WHong
  * @date      2026-09-24
  *
  * @details   本模块将接收数据流写入 Flash，仅在向量表与 CRC 校验通过后
  *            提交引导元数据；不依赖产品私有符号，也不使用动态内存。
  */

#include "Ota/app_ota_flash.h"

#include <stddef.h>
#include <string.h>

#include "main.h"
#include "task.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_flash_ex.h"

/** @brief 初始栈指针必须满足的 4 字节对齐掩码。 */
#define APP_OTA_STACK_ALIGNMENT_MASK       0x00000003UL

/** @brief 暂存跨数据包且不足一个 Flash 字的尾部字节。 */
static uint8_t s_aucTail[4];
/** @brief 尾部有效字节数；缓存时为 0 至 3，补齐写入时可暂达 4。 */
static uint8_t s_ucTailLength;
/** @brief 下一个待编程 Flash 字的暂存区地址。 */
static uint32_t s_ulFlashAddress;
/** @brief 已接收镜像的精确字节数，不含尾部填充。 */
static uint32_t s_ulReceived;
/** @brief 非零表示当前上传会话独占 Flash 写入器。 */
static volatile uint8_t s_ucActive;
/** @brief 初始化后在整个服务生命周期内保留的产品配置。 */
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
/**
  * @brief  使用已绑定的日志来源写入一条 OTA 记录。
  * @param[in] xLevel 日志级别。
  * @param[in] pcText 稳定事件文本。
  * @param[in] lCode 与事件关联的结果或错误码。
  */
static void prvWriteLog(AppLogLevel_e xLevel, const char *pcText,
	int32_t lCode)
{
	if (s_pxConfig != NULL) {
		(void)xAppLogWrite(xLevel, s_pxConfig->xLogSource, pcText, lCode);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  在允许的暂存区或元数据区编程并回读一个 Flash 字。
  * @param[in] ulAddress 4 字节编程目标地址。
  * @param[in] ulWord 待写入的 32 位数据。
  * @retval APP_OTA_RESULT_OK 写入和回读一致。
  * @retval APP_OTA_RESULT_INVALID_ARG 地址不属于允许的写入范围。
  * @retval APP_OTA_RESULT_HAL_ERROR HAL 写入或回读校验失败。
  */
static AppOtaResult_e prvProgramWord(uint32_t ulAddress, uint32_t ulWord)
{
	HAL_StatusTypeDef xStatus; /*!< HAL Flash 编程结果。 */
	volatile uint32_t *pulFlash; /*!< 用于编程后立即回读的映射地址。 */
	uint8_t ucStagingAddress; /*!< 地址是否落在完整暂存字范围内。 */
	uint8_t ucMetadataAddress; /*!< 地址是否落在四字元数据记录内。 */

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
/**
  * @brief  擦除配置指定的全部固件暂存扇区。
  * @retval APP_OTA_RESULT_OK 所有暂存扇区擦除成功。
  * @retval APP_OTA_RESULT_HAL_ERROR HAL 擦除失败或报告失败扇区。
  */
static AppOtaResult_e prvEraseStaging(void)
{
	FLASH_EraseInitTypeDef xErase; /*!< 暂存区连续扇区擦除参数。 */
	uint32_t ulSectorError; /*!< HAL 返回的失败扇区号。 */

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
/**
  * @brief  用 0xFF 补齐并写入当前不足四字节的镜像尾部。
  * @retval APP_OTA_RESULT_OK 没有尾部或尾部写入成功。
  * @retval APP_OTA_RESULT_INVALID_ARG 暂存区地址不在允许的写入范围。
  * @retval APP_OTA_RESULT_HAL_ERROR 尾部编程或回读失败。
  */
static AppOtaResult_e prvFlushTail(void)
{
	uint32_t ulWord; /*!< 由尾部缓存拼成的 Flash 编程字。 */
	AppOtaResult_e xResult; /*!< 尾部编程和回读结果。 */

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
/**
  * @brief  校验暂存镜像的初始栈指针与复位向量。
  * @retval 1 栈指针范围、对齐和复位入口均符合目标配置。
  * @retval 0 向量表任一约束不满足。
  */
static uint8_t prvValidateVector(void)
{
	volatile const uint32_t *pulVector; /*!< 暂存镜像向量表映射地址。 */
	uint32_t ulStack; /*!< 镜像声明的初始主栈指针。 */
	uint32_t ulReset; /*!< 带 Thumb 状态位的复位向量。 */
	uint32_t ulResetAddress; /*!< 去除 Thumb 状态位后的复位入口地址。 */

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
/**
  * @brief  按四字节向上取整计算暂存镜像的硬件 CRC。
  * @param[in] ulSize 镜像精确字节数。
  * @retval uint32_t HAL CRC 外设计算结果。
  */
static uint32_t prvCalculateCrc(uint32_t ulSize)
{
	uint32_t ulWordCount; /*!< 覆盖镜像及尾部填充的 32 位字数。 */

	ulWordCount = (ulSize + 3U) / 4U;
	return HAL_CRC_Calculate((CRC_HandleTypeDef *)s_pxConfig->pvCrc,
		(uint32_t *)s_pxConfig->ulStagingAddress, ulWordCount);
}

/*-----------------------------------------------------------*/
/**
  * @brief  结束失败会话、复位流式状态并记录错误。
  * @param[in] pcText 失败事件文本。
  * @param[in] lCode 失败结果或诊断码。
  */
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
/** @brief 锁定 Flash；失败时通过已绑定日志来源记录 HAL 状态。 */
static void prvLockFlash(void)
{
	HAL_StatusTypeDef xStatus; /*!< Flash 加锁操作的 HAL 状态。 */

	xStatus = HAL_FLASH_Lock();
	if (xStatus != HAL_OK) {
		prvWriteLog(APP_LOG_LEVEL_ERROR, "OTA_HTTP_FLASH_FAILED",
			(int32_t)xStatus);
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  绑定公共 OTA 服务使用的不可变产品配置。
  * @param[in] pxConfig 产品持有且生命周期覆盖服务运行期的静态配置。
  * @retval APP_OTA_RESULT_OK 配置通过校验并已绑定。
  * @retval APP_OTA_RESULT_INVALID_ARG 必需地址、句柄或参数不合法。
  * @retval APP_OTA_RESULT_BUSY 已绑定另一个配置实例。
  */
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
/**
  * @brief  独占写入器、解锁 Flash 并擦除固件暂存区。
  * @retval APP_OTA_RESULT_OK 暂存区擦除完成且会话已激活。
  * @retval APP_OTA_RESULT_INVALID_ARG 服务尚未绑定配置。
  * @retval APP_OTA_RESULT_BUSY 另一个上传会话正在占用写入器。
  * @retval APP_OTA_RESULT_HAL_ERROR Flash 解锁或擦除失败。
  */
AppOtaResult_e xAppOtaFlashBegin(void)
{
	AppOtaResult_e xResult; /*!< 暂存区擦除结果。 */

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
/**
  * @brief  将任意分包的镜像字节按 Flash 字对齐编程并回读。
  * @param[in] pucData 本次调用期间有效的镜像数据，可在长度为零时为空。
  * @param[in] ulLength 本次追加的镜像字节数。
  * @retval APP_OTA_RESULT_OK 数据已写入或保存在尾部缓存。
  * @retval APP_OTA_RESULT_INVALID_ARG 会话、指针或累计长度不合法。
  * @retval APP_OTA_RESULT_HAL_ERROR Flash 编程或回读失败且会话已终止。
  */
AppOtaResult_e xAppOtaFlashWrite(const uint8_t *pucData, uint32_t ulLength)
{
	uint32_t ulIndex; /*!< 本数据块内当前处理的字节索引。 */
	uint32_t ulWord; /*!< 尾部缓存凑满后形成的编程字。 */
	AppOtaResult_e xResult; /*!< 当前 Flash 字的编程和回读结果。 */
	uint32_t ulMaximum; /*!< 目标应用分区允许的最大镜像字节数。 */

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
	/* 逐字节聚合成四字节编程单元，跨调用保留不足一字的尾部。 */
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
/**
  * @brief  补齐尾部、验证镜像并提交供引导程序使用的元数据。
  * @param[out] pulCrc32 可选的最终 CRC 接收位置。
  * @param[out] pulSize 可选的镜像精确字节数接收位置。
  * @retval APP_OTA_RESULT_OK 镜像校验和元数据提交完成。
  * @retval APP_OTA_RESULT_INVALID_ARG 会话状态或镜像长度不合法。
  * @retval APP_OTA_RESULT_INVALID_IMAGE 向量表校验失败。
  * @retval APP_OTA_RESULT_CRC_ERROR 两次 CRC 计算结果不一致。
  * @retval APP_OTA_RESULT_HAL_ERROR 尾部或元数据 Flash 操作失败。
  */
AppOtaResult_e xAppOtaFlashFinish(uint32_t *pulCrc32, uint32_t *pulSize)
{
	AppOtaResult_e xResult; /*!< 当前尾部或元数据字的操作结果。 */
	uint32_t ulCrc; /*!< 对完整暂存镜像计算的 CRC。 */
	uint32_t ulSectorError; /*!< 元数据擦除失败时的 HAL 扇区号。 */
	uint32_t aulMetadata[4]; /*!< 魔数、版本、镜像长度和 CRC 元数据。 */
	FLASH_EraseInitTypeDef xErase; /*!< 单个元数据扇区的擦除参数。 */
	uint8_t ucIndex; /*!< 四字元数据的编程索引。 */
	uint32_t ulMaximum; /*!< 目标应用分区允许的最大镜像字节数。 */

	if (s_pxConfig == NULL) {
		return APP_OTA_RESULT_INVALID_ARG;
	}
	ulMaximum = s_pxConfig->ulApplicationEnd -
		s_pxConfig->ulApplicationAddress;
	if ((s_ucActive == 0U) || (s_ulReceived == 0U) ||
		(s_ulReceived > ulMaximum)) {
		return APP_OTA_RESULT_INVALID_ARG;
	}
	/* 步骤 1：补齐并写入最后一个不完整的 Flash 字。 */
	xResult = prvFlushTail();
	if (xResult != APP_OTA_RESULT_OK) {
		prvAbortWithLog("OTA_HTTP_FLASH_FAILED", (int32_t)xResult);
		return xResult;
	}
	/* 步骤 2：验证向量表，并用两次 CRC 计算排除瞬时读取差异。 */
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

	/* 步骤 3：擦除旧元数据，再逐字提交新镜像描述。 */
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
	/* 步骤 4：锁定 Flash、返回结果并释放上传会话。 */
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
/** @brief 终止活动上传、锁定 Flash 并保留现有引导元数据。 */
void vAppOtaFlashAbort(void)
{
	if ((s_pxConfig != NULL) && (s_ucActive != 0U)) {
		prvAbortWithLog("OTA_HTTP_ABORT", 0);
	} else {
		prvLockFlash();
	}
}

/*-----------------------------------------------------------*/
/**
  * @brief  查询是否有上传会话占用 Flash 写入器。
  * @retval 0 当前没有活动上传。
  * @retval 非零 当前上传会话仍处于活动状态。
  */
uint8_t ucAppOtaFlashIsActive(void)
{
	return s_ucActive;
}

/*-----------------------------------------------------------*/
/**
  * @brief  获取成功初始化后绑定的不可变产品配置。
  * @retval NULL 服务尚未初始化。
  * @retval 非空 产品持有的 AppOtaConfig_t 配置地址。
  */
const AppOtaConfig_t *pxAppOtaGetConfig(void)
{
	return s_pxConfig;
}
